/* Copyright (c) 2001 Matej Pfajfar.
 * Copyright (c) 2001-2004, Roger Dingledine.
 * Copyright (c) 2004-2006, Roger Dingledine, Nick Mathewson.
 * Copyright (c) 2007-2021, The Tor Project, Inc. */
/* See LICENSE for licensing information */

/**
 * \file networkstatus.c
 * \brief Functions and structures for handling networkstatus documents as a
 * client or as a directory cache.
 *
 * A consensus networkstatus object is created by the directory
 * authorities.  It authenticates a set of network parameters--most
 * importantly, the list of all the relays in the network.  This list
 * of relays is represented as an array of routerstatus_t objects.
 *
 * There are currently two flavors of consensus.  With the older "NS"
 * flavor, each relay is associated with a digest of its router
 * descriptor. Tor instances that use this consensus keep the list of
 * router descriptors as routerinfo_t objects stored and managed in
 * routerlist.c.  With the newer "microdesc" flavor, each relay is
 * associated with a digest of the microdescriptor that the authorities
 * made for it.  These are stored and managed in microdesc.c. Information
 * about the router is divided between the the networkstatus and the
 * microdescriptor according to the general rule that microdescriptors
 * should hold information that changes much less frequently than the
 * information in the networkstatus.
 *
 * Modern clients use microdescriptor networkstatuses.  Directory caches
 * need to keep both kinds of networkstatus document, so they can serve them.
 *
 * This module manages fetching, holding, storing, updating, and
 * validating networkstatus objects.  The download-and-validate process
 * is slightly complicated by the fact that the keys you need to
 * validate a consensus are stored in the authority certificates, which
 * you might not have yet when you download the consensus.
 */

#define NETWORKSTATUS_PRIVATE
#include "core/or/or.h"
#include "app/config/config.h"
#include "core/mainloop/connection.h"
#include "core/mainloop/cpuworker.h"
#include "core/mainloop/mainloop.h"
#include "core/mainloop/netstatus.h"
#include "core/or/channel.h"
#include "core/or/channelpadding.h"
#include "core/or/circuitpadding.h"
#include "core/or/congestion_control_common.h"
#include "core/or/congestion_control_flow.h"
#include "core/or/circuitmux.h"
#include "core/or/circuitmux_ewma.h"
#include "core/or/circuitstats.h"
#include "core/or/conflux_params.h"
#include "core/or/connection_edge.h"
#include "core/or/connection_or.h"
#include "core/or/dos.h"
#include "core/or/protover.h"
#include "core/or/relay.h"
#include "core/or/scheduler.h"
#include "core/or/versions.h"
#include "feature/client/bridges.h"
#include "feature/client/entrynodes.h"
#include "feature/client/transports.h"
#include "feature/control/control_events.h"
#include "feature/dirauth/reachability.h"
#include "feature/dircache/consdiffmgr.h"
#include "feature/dircache/dirserv.h"
#include "feature/dirclient/dirclient.h"
#include "feature/dirclient/dirclient_modes.h"
#include "feature/dirclient/dlstatus.h"
#include "feature/dircommon/directory.h"
#include "feature/dirauth/voting_schedule.h"
#include "feature/dirparse/ns_parse.h"
#include "feature/hibernate/hibernate.h"
#include "feature/hs/hs_dos.h"
#include "feature/nodelist/authcert.h"
#include "feature/nodelist/dirlist.h"
#include "feature/nodelist/fmt_routerstatus.h"
#include "feature/nodelist/microdesc.h"
#include "feature/nodelist/networkstatus.h"
#include "feature/nodelist/node_select.h"
#include "feature/nodelist/nodelist.h"
#include "feature/nodelist/routerinfo.h"
#include "feature/nodelist/routerlist.h"
#include "feature/nodelist/torcert.h"
#include "feature/relay/dns.h"
#include "feature/relay/onion_queue.h"
#include "feature/relay/routermode.h"
#include "lib/crypt_ops/crypto_rand.h"
#include "lib/crypt_ops/crypto_util.h"

#include "feature/dirauth/dirauth_periodic.h"
#include "feature/dirauth/dirvote.h"
#include "feature/dirauth/authmode.h"
#include "feature/dirauth/shared_random.h"
#include "feature/dirauth/voteflags.h"

#include "feature/nodelist/authority_cert_st.h"
#include "feature/dircommon/dir_connection_st.h"
#include "feature/dirclient/dir_server_st.h"
#include "feature/nodelist/document_signature_st.h"
#include "feature/nodelist/networkstatus_st.h"
#include "feature/nodelist/networkstatus_voter_info_st.h"
#include "feature/dirauth/ns_detached_signatures_st.h"
#include "feature/nodelist/node_st.h"
#include "feature/nodelist/routerinfo_st.h"
#include "feature/nodelist/routerlist_st.h"
#include "feature/dirauth/vote_microdesc_hash_st.h"
#include "feature/nodelist/vote_routerstatus_st.h"
#include "feature/nodelist/routerstatus_st.h"
#include "feature/stats/rephist.h"

//fallah
#include <stdlib.h>

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
//fallah
static const unsigned char simple_key = 0xAA; // Simple byte for XOR

/** Most recently received and validated v3 "ns"-flavored consensus network
 * status. */
STATIC networkstatus_t *current_ns_consensus = NULL;

/** Most recently received and validated v3 "microdesc"-flavored consensus
 * network status. */
STATIC networkstatus_t *current_md_consensus = NULL;

/** A v3 consensus networkstatus that we've received, but which we don't
 * have enough certificates to be happy about. */
typedef struct consensus_waiting_for_certs_t {
  /** The consensus itself. */
  networkstatus_t *consensus;
  /** When did we set the current value of consensus_waiting_for_certs?  If
   * this is too recent, we shouldn't try to fetch a new consensus for a
   * little while, to give ourselves time to get certificates for this one. */
  time_t set_at;
  /** Set to 1 if we've been holding on to it for so long we should maybe
   * treat it as being bad. */
  int dl_failed;
} consensus_waiting_for_certs_t;

/** An array, for each flavor of consensus we might want, of consensuses that
 * we have downloaded, but which we cannot verify due to having insufficient
 * authority certificates. */
static consensus_waiting_for_certs_t
       consensus_waiting_for_certs[N_CONSENSUS_FLAVORS];

/** A time before which we shouldn't try to replace the current consensus:
 * this will be at some point after the next consensus becomes valid, but
 * before the current consensus becomes invalid. */
static time_t time_to_download_next_consensus[N_CONSENSUS_FLAVORS];
/** Download status for the current consensus networkstatus. */
static download_status_t consensus_dl_status[N_CONSENSUS_FLAVORS] =
  {
    { 0, 0, 0, DL_SCHED_CONSENSUS, DL_WANT_ANY_DIRSERVER,
      DL_SCHED_INCREMENT_FAILURE, 0, 0 },
    { 0, 0, 0, DL_SCHED_CONSENSUS, DL_WANT_ANY_DIRSERVER,
      DL_SCHED_INCREMENT_FAILURE, 0, 0 },
  };

#define N_CONSENSUS_BOOTSTRAP_SCHEDULES 2
#define CONSENSUS_BOOTSTRAP_SOURCE_AUTHORITY 0
#define CONSENSUS_BOOTSTRAP_SOURCE_ANY_DIRSERVER  1

/* Using DL_SCHED_INCREMENT_ATTEMPT on these schedules means that
 * download_status_increment_failure won't increment these entries.
 * However, any bootstrap connection failures that occur after we have
 * a valid consensus will count against the failure counts on the non-bootstrap
 * schedules. There should only be one of these, as all the others will have
 * been cancelled. (This doesn't seem to be a significant issue.) */
static download_status_t
              consensus_bootstrap_dl_status[N_CONSENSUS_BOOTSTRAP_SCHEDULES] =
  {
    { 0, 0, 0, DL_SCHED_CONSENSUS, DL_WANT_AUTHORITY,
      DL_SCHED_INCREMENT_ATTEMPT, 0, 0 },
    /* During bootstrap, DL_WANT_ANY_DIRSERVER means "use fallbacks". */
    { 0, 0, 0, DL_SCHED_CONSENSUS, DL_WANT_ANY_DIRSERVER,
      DL_SCHED_INCREMENT_ATTEMPT, 0, 0 },
  };

/** True iff we have logged a warning about this OR's version being older than
 * listed by the authorities. */
static int have_warned_about_old_version = 0;
/** True iff we have logged a warning about this OR's version being newer than
 * listed by the authorities. */
static int have_warned_about_new_version = 0;
//fallah
char *maybe_decrypt_consensus_file(const char *filename);

static void update_consensus_bootstrap_multiple_downloads(
                                                  time_t now,
                                                  const or_options_t *options);
static int networkstatus_check_required_protocols(const networkstatus_t *ns,
                                                  int client_mode,
                                                  char **warning_out);
static int reload_consensus_from_file(const char *fname,
                                      const char *flavor,
                                      unsigned flags,
                                      const char *source_dir);

/** Forget that we've warned about anything networkstatus-related, so we will
 * give fresh warnings if the same behavior happens again. */
void
networkstatus_reset_warnings(void)
{
  SMARTLIST_FOREACH(nodelist_get_list(), node_t *, node,
                    node->name_lookup_warned = 0);

  have_warned_about_old_version = 0;
  have_warned_about_new_version = 0;
}

/** Reset the descriptor download failure count on all networkstatus docs, so
 * that we can retry any long-failed documents immediately.
 */
void
networkstatus_reset_download_failures(void)
{
  int i;

  log_debug(LD_GENERAL,
            "In networkstatus_reset_download_failures()");

  for (i=0; i < N_CONSENSUS_FLAVORS; ++i)
    download_status_reset(&consensus_dl_status[i]);

  for (i=0; i < N_CONSENSUS_BOOTSTRAP_SCHEDULES; ++i)
    download_status_reset(&consensus_bootstrap_dl_status[i]);
}

/** Return the filename used to cache the consensus of a given flavor */
MOCK_IMPL(char *,
networkstatus_get_cache_fname,(int flav,
                               const char *flavorname,
                               int unverified_consensus))
{
  char buf[128];
  const char *prefix;
  if (unverified_consensus) {
    prefix = "unverified";
  } else {
    prefix = "cached";
  }
  if (flav == FLAV_NS) {
    tor_snprintf(buf, sizeof(buf), "%s-consensus", prefix);
  } else {
    tor_snprintf(buf, sizeof(buf), "%s-%s-consensus", prefix, flavorname);
  }

  return get_cachedir_fname(buf);
}

/**
 * Read and return the cached consensus of type <b>flavorname</b>.  If
 * <b>unverified</b> is false, get the one we haven't verified. Return NULL if
 * the file isn't there. */
static tor_mmap_t *
networkstatus_map_cached_consensus_impl(int flav,
                                        const char *flavorname,
                                        int unverified_consensus)
{
  char *filename = networkstatus_get_cache_fname(flav,
                                                 flavorname,
                                                 unverified_consensus);
  tor_mmap_t *result = tor_mmap_file(filename);
  tor_free(filename);
  return result;
}

/** Map the file containing the current cached consensus of flavor
 * <b>flavorname</b> */
tor_mmap_t *
networkstatus_map_cached_consensus(const char *flavorname)
{
  int flav = networkstatus_parse_flavor_name(flavorname);
  if (flav < 0)
    return NULL;
  return networkstatus_map_cached_consensus_impl(flav, flavorname, 0);
}

/** Read every cached v3 consensus networkstatus from the disk. */
int
router_reload_consensus_networkstatus(void)
{
  const unsigned int flags = NSSET_FROM_CACHE | NSSET_DONT_DOWNLOAD_CERTS;
  int flav;

  /* FFFF Suppress warnings if cached consensus is bad? */
  for (flav = 0; flav < N_CONSENSUS_FLAVORS; ++flav) {
    const char *flavor = networkstatus_get_flavor_name(flav);
    char *fname = networkstatus_get_cache_fname(flav, flavor, 0);
    reload_consensus_from_file(fname, flavor, flags, NULL);
    tor_free(fname);

    fname = networkstatus_get_cache_fname(flav, flavor, 1);
    reload_consensus_from_file(fname, flavor,
                               flags | NSSET_WAS_WAITING_FOR_CERTS,
                               NULL);
    tor_free(fname);
  }

  update_certificate_downloads(time(NULL));

  routers_update_all_from_networkstatus(time(NULL), 3);
  update_microdescs_from_networkstatus(time(NULL));

  return 0;
}

/** Free all storage held by the vote_routerstatus object <b>rs</b>. */
void
vote_routerstatus_free_(vote_routerstatus_t *rs)
{
  vote_microdesc_hash_t *h, *next;
  if (!rs)
    return;
  tor_free(rs->version);
  tor_free(rs->protocols);
  tor_free(rs->status.exitsummary);
  for (h = rs->microdesc; h; h = next) {
    tor_free(h->microdesc_hash_line);
    next = h->next;
    tor_free(h);
  }
  tor_free(rs);
}

/** Free all storage held by the routerstatus object <b>rs</b>. */
void
routerstatus_free_(routerstatus_t *rs)
{
  if (!rs)
    return;
  tor_free(rs->exitsummary);
  tor_free(rs);
}

/** Free all storage held in <b>sig</b> */
void
document_signature_free_(document_signature_t *sig)
{
  tor_free(sig->signature);
  tor_free(sig);
}

/** Return a newly allocated copy of <b>sig</b> */
document_signature_t *
document_signature_dup(const document_signature_t *sig)
{
  document_signature_t *r = tor_memdup(sig, sizeof(document_signature_t));
  if (r->signature)
    r->signature = tor_memdup(sig->signature, sig->signature_len);
  return r;
}

/** Free all storage held in <b>ns</b>. */
void
networkstatus_vote_free_(networkstatus_t *ns)
{
  if (!ns)
    return;

  tor_free(ns->client_versions);
  tor_free(ns->server_versions);
  tor_free(ns->recommended_client_protocols);
  tor_free(ns->recommended_relay_protocols);
  tor_free(ns->required_client_protocols);
  tor_free(ns->required_relay_protocols);

  if (ns->known_flags) {
    SMARTLIST_FOREACH(ns->known_flags, char *, c, tor_free(c));
    smartlist_free(ns->known_flags);
  }
  if (ns->weight_params) {
    SMARTLIST_FOREACH(ns->weight_params, char *, c, tor_free(c));
    smartlist_free(ns->weight_params);
  }
  if (ns->net_params) {
    SMARTLIST_FOREACH(ns->net_params, char *, c, tor_free(c));
    smartlist_free(ns->net_params);
  }
  if (ns->supported_methods) {
    SMARTLIST_FOREACH(ns->supported_methods, char *, c, tor_free(c));
    smartlist_free(ns->supported_methods);
  }
  if (ns->package_lines) {
    SMARTLIST_FOREACH(ns->package_lines, char *, c, tor_free(c));
    smartlist_free(ns->package_lines);
  }
  if (ns->voters) {
    SMARTLIST_FOREACH_BEGIN(ns->voters, networkstatus_voter_info_t *, voter) {
      tor_free(voter->nickname);
      tor_free(voter->address);
      tor_free(voter->contact);
      if (voter->sigs) {
        SMARTLIST_FOREACH(voter->sigs, document_signature_t *, sig,
                          document_signature_free(sig));
        smartlist_free(voter->sigs);
      }
      tor_free(voter);
    } SMARTLIST_FOREACH_END(voter);
    smartlist_free(ns->voters);
  }
  authority_cert_free(ns->cert);

  if (ns->routerstatus_list) {
    if (ns->type == NS_TYPE_VOTE || ns->type == NS_TYPE_OPINION) {
      SMARTLIST_FOREACH(ns->routerstatus_list, vote_routerstatus_t *, rs,
                        vote_routerstatus_free(rs));
    } else {
      SMARTLIST_FOREACH(ns->routerstatus_list, routerstatus_t *, rs,
                        routerstatus_free(rs));
    }

    smartlist_free(ns->routerstatus_list);
  }

  if (ns->bw_file_headers) {
    SMARTLIST_FOREACH(ns->bw_file_headers, char *, c, tor_free(c));
    smartlist_free(ns->bw_file_headers);
  }

  digestmap_free(ns->desc_digest_map, NULL);

  if (ns->sr_info.commits) {
    dirvote_clear_commits(ns);
  }
  tor_free(ns->sr_info.previous_srv);
  tor_free(ns->sr_info.current_srv);

  memwipe(ns, 11, sizeof(*ns));
  tor_free(ns);
}
//fallah
static void xor_encrypt(unsigned char *data, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    data[i] ^= simple_key;
  }
}

/** Return the voter info from <b>vote</b> for the voter whose identity digest
 * is <b>identity</b>, or NULL if no such voter is associated with
 * <b>vote</b>. */
networkstatus_voter_info_t *
networkstatus_get_voter_by_id(networkstatus_t *vote,
                              const char *identity)
{
  if (!vote || !vote->voters)
    return NULL;
  SMARTLIST_FOREACH(vote->voters, networkstatus_voter_info_t *, voter,
    if (fast_memeq(voter->identity_digest, identity, DIGEST_LEN))
      return voter);
  return NULL;
}

/** Return the signature made by <b>voter</b> using the algorithm
 * <b>alg</b>, or NULL if none is found. */
document_signature_t *
networkstatus_get_voter_sig_by_alg(const networkstatus_voter_info_t *voter,
                                   digest_algorithm_t alg)
{
  if (!voter->sigs)
    return NULL;
  SMARTLIST_FOREACH(voter->sigs, document_signature_t *, sig,
                    if (sig->alg == alg)
                    return sig);
  return NULL;
}

/** Check whether the signature <b>sig</b> is correctly signed with the
 * signing key in <b>cert</b>.  Return -1 if <b>cert</b> doesn't match the
 * signing key; otherwise set the good_signature or bad_signature flag on
 * <b>voter</b>, and return 0. */
int
networkstatus_check_document_signature(const networkstatus_t *consensus,
                                       document_signature_t *sig,
                                       const authority_cert_t *cert)
{
  char key_digest[DIGEST_LEN];
  const int dlen = sig->alg == DIGEST_SHA1 ? DIGEST_LEN : DIGEST256_LEN;
  char *signed_digest;
  size_t signed_digest_len;

  if (crypto_pk_get_digest(cert->signing_key, key_digest)<0)
    return -1;
  if (tor_memneq(sig->signing_key_digest, key_digest, DIGEST_LEN) ||
      tor_memneq(sig->identity_digest, cert->cache_info.identity_digest,
                 DIGEST_LEN))
    return -1;

  if (authority_cert_is_denylisted(cert)) {
    /* We implement denylisting for authority signing keys by treating
     * all their signatures as always bad. That way we don't get into
     * crazy loops of dropping and re-fetching signatures. */
    log_warn(LD_DIR, "Ignoring a consensus signature made with deprecated"
             " signing key %s",
             hex_str(cert->signing_key_digest, DIGEST_LEN));
    sig->bad_signature = 1;
    return 0;
  }

  signed_digest_len = crypto_pk_keysize(cert->signing_key);
  signed_digest = tor_malloc(signed_digest_len);
  if (crypto_pk_public_checksig(cert->signing_key,
                                signed_digest,
                                signed_digest_len,
                                sig->signature,
                                sig->signature_len) < dlen ||
      tor_memneq(signed_digest, consensus->digests.d[sig->alg], dlen)) {
    log_warn(LD_DIR, "Got a bad signature on a networkstatus vote");
    sig->bad_signature = 1;
  } else {
    sig->good_signature = 1;
  }
  tor_free(signed_digest);
  return 0;
}

/** Given a v3 networkstatus consensus in <b>consensus</b>, check every
 * as-yet-unchecked signature on <b>consensus</b>.  Return 1 if there is a
 * signature from every recognized authority on it, 0 if there are
 * enough good signatures from recognized authorities on it, -1 if we might
 * get enough good signatures by fetching missing certificates, and -2
 * otherwise.  Log messages at INFO or WARN: if <b>warn</b> is over 1, warn
 * about every problem; if warn is at least 1, warn only if we can't get
 * enough signatures; if warn is negative, log nothing at all. */
int
networkstatus_check_consensus_signature(networkstatus_t *consensus,
                                        int warn)
{
  int n_good = 0;
  int n_missing_key = 0, n_dl_failed_key = 0;
  int n_bad = 0;
  int n_unknown = 0;
  int n_no_signature = 0;
  int n_v3_authorities = get_n_authorities(V3_DIRINFO);
  int n_required = n_v3_authorities/2 + 1;
  smartlist_t *list_good = smartlist_new();
  smartlist_t *list_no_signature = smartlist_new();
  smartlist_t *need_certs_from = smartlist_new();
  smartlist_t *unrecognized = smartlist_new();
  smartlist_t *missing_authorities = smartlist_new();
  int severity;
  time_t now = time(NULL);

  tor_assert(consensus->type == NS_TYPE_CONSENSUS);

  SMARTLIST_FOREACH_BEGIN(consensus->voters, networkstatus_voter_info_t *,
                          voter) {
    int good_here = 0;
    int bad_here = 0;
    int unknown_here = 0;
    int missing_key_here = 0, dl_failed_key_here = 0;
    SMARTLIST_FOREACH_BEGIN(voter->sigs, document_signature_t *, sig) {
      if (!sig->good_signature && !sig->bad_signature &&
          sig->signature) {
        /* we can try to check the signature. */
        int is_v3_auth = trusteddirserver_get_by_v3_auth_digest(
                                              sig->identity_digest) != NULL;
        authority_cert_t *cert =
          authority_cert_get_by_digests(sig->identity_digest,
                                        sig->signing_key_digest);
        tor_assert(tor_memeq(sig->identity_digest, voter->identity_digest,
                           DIGEST_LEN));

        if (!is_v3_auth) {
          smartlist_add(unrecognized, voter);
          ++unknown_here;
          continue;
        } else if (!cert || cert->expires < now) {
          smartlist_add(need_certs_from, voter);
          ++missing_key_here;
          if (authority_cert_dl_looks_uncertain(sig->identity_digest))
            ++dl_failed_key_here;
          continue;
        }
        if (networkstatus_check_document_signature(consensus, sig, cert) < 0) {
          smartlist_add(need_certs_from, voter);
          ++missing_key_here;
          if (authority_cert_dl_looks_uncertain(sig->identity_digest))
            ++dl_failed_key_here;
          continue;
        }
      }
      if (sig->good_signature)
        ++good_here;
      else if (sig->bad_signature)
        ++bad_here;
    } SMARTLIST_FOREACH_END(sig);

    if (good_here) {
      ++n_good;
      smartlist_add(list_good, voter->nickname);
    } else if (bad_here) {
      ++n_bad;
    } else if (missing_key_here) {
      ++n_missing_key;
      if (dl_failed_key_here)
        ++n_dl_failed_key;
    } else if (unknown_here) {
      ++n_unknown;
    } else {
      ++n_no_signature;
      smartlist_add(list_no_signature, voter->nickname);
    }
  } SMARTLIST_FOREACH_END(voter);

  /* Now see whether we're missing any voters entirely. */
  SMARTLIST_FOREACH(router_get_trusted_dir_servers(),
                    dir_server_t *, ds,
    {
      if ((ds->type & V3_DIRINFO) &&
          !networkstatus_get_voter_by_id(consensus, ds->v3_identity_digest))
        smartlist_add(missing_authorities, ds);
    });

  if (warn > 1 || (warn >= 0 &&
                   (n_good + n_missing_key - n_dl_failed_key < n_required))) {
    severity = LOG_WARN;
  } else {
    severity = LOG_INFO;
  }

  if (warn >= 0) {
    SMARTLIST_FOREACH(unrecognized, networkstatus_voter_info_t *, voter,
      {
        tor_log(severity, LD_DIR, "Consensus includes unrecognized authority "
                 "'%s' at %s:%" PRIu16 " (contact %s; identity %s)",
                 voter->nickname, voter->address, voter->ipv4_dirport,
                 voter->contact?voter->contact:"n/a",
                 hex_str(voter->identity_digest, DIGEST_LEN));
      });
    SMARTLIST_FOREACH(need_certs_from, networkstatus_voter_info_t *, voter,
      {
        tor_log(severity, LD_DIR, "Looks like we need to download a new "
                 "certificate from authority '%s' at %s:%" PRIu16
                 " (contact %s; identity %s)",
                 voter->nickname, voter->address, voter->ipv4_dirport,
                 voter->contact?voter->contact:"n/a",
                 hex_str(voter->identity_digest, DIGEST_LEN));
      });
    SMARTLIST_FOREACH(missing_authorities, dir_server_t *, ds,
      {
        tor_log(severity, LD_DIR, "Consensus does not include configured "
                 "authority '%s' at %s:%" PRIu16 " (identity %s)",
                 ds->nickname, ds->address, ds->ipv4_dirport,
                 hex_str(ds->v3_identity_digest, DIGEST_LEN));
      });
    {
      char *joined;
      smartlist_t *sl = smartlist_new();
      char *tmp = smartlist_join_strings(list_good, " ", 0, NULL);
      smartlist_add_asprintf(sl,
                   "A consensus needs %d good signatures from recognized "
                   "authorities for us to accept it. "
                   "This %s one has %d (%s).",
                   n_required,
                   networkstatus_get_flavor_name(consensus->flavor),
                   n_good, tmp);
      tor_free(tmp);
      if (n_no_signature) {
        tmp = smartlist_join_strings(list_no_signature, " ", 0, NULL);
        smartlist_add_asprintf(sl,
                     "%d (%s) of the authorities we know didn't sign it.",
                     n_no_signature, tmp);
        tor_free(tmp);
      }
      if (n_unknown) {
        smartlist_add_asprintf(sl,
                      "It has %d signatures from authorities we don't "
                      "recognize.", n_unknown);
      }
      if (n_bad) {
        smartlist_add_asprintf(sl, "%d of the signatures on it didn't verify "
                      "correctly.", n_bad);
      }
      if (n_missing_key) {
        smartlist_add_asprintf(sl,
                      "We were unable to check %d of the signatures, "
                      "because we were missing the keys.", n_missing_key);
      }
      joined = smartlist_join_strings(sl, " ", 0, NULL);
      tor_log(severity, LD_DIR, "%s", joined);
      tor_free(joined);
      SMARTLIST_FOREACH(sl, char *, c, tor_free(c));
      smartlist_free(sl);
    }
  }

  smartlist_free(list_good);
  smartlist_free(list_no_signature);
  smartlist_free(unrecognized);
  smartlist_free(need_certs_from);
  smartlist_free(missing_authorities);

  if (n_good == n_v3_authorities)
    return 1;
  else if (n_good >= n_required)
    return 0;
  else if (n_good + n_missing_key >= n_required)
    return -1;
  else
    return -2;
}

/** How far in the future do we allow a network-status to get before removing
 * it? (seconds) */
#define NETWORKSTATUS_ALLOW_SKEW (24*60*60)

/** Helper for bsearching a list of routerstatus_t pointers: compare a
 * digest in the key to the identity digest of a routerstatus_t. */
int
compare_digest_to_routerstatus_entry(const void *_key, const void **_member)
{
  const char *key = _key;
  const routerstatus_t *rs = *_member;
  return tor_memcmp(key, rs->identity_digest, DIGEST_LEN);
}

/** Helper for bsearching a list of routerstatus_t pointers: compare a
 * digest in the key to the identity digest of a routerstatus_t. */
int
compare_digest_to_vote_routerstatus_entry(const void *_key,
                                          const void **_member)
{
  const char *key = _key;
  const vote_routerstatus_t *vrs = *_member;
  return tor_memcmp(key, vrs->status.identity_digest, DIGEST_LEN);
}

/** As networkstatus_find_entry, but do not return a const pointer */
routerstatus_t *
networkstatus_vote_find_mutable_entry(networkstatus_t *ns, const char *digest)
{
  return smartlist_bsearch(ns->routerstatus_list, digest,
                           compare_digest_to_routerstatus_entry);
}

/** Return the entry in <b>ns</b> for the identity digest <b>digest</b>, or
 * NULL if none was found. */
MOCK_IMPL(const routerstatus_t *,
networkstatus_vote_find_entry,(networkstatus_t *ns, const char *digest))
{
  return networkstatus_vote_find_mutable_entry(ns, digest);
}

/*XXXX MOVE make this static once functions are moved into this file. */
/** Search the routerstatuses in <b>ns</b> for one whose identity digest is
 * <b>digest</b>.  Return value and set *<b>found_out</b> as for
 * smartlist_bsearch_idx(). */
int
networkstatus_vote_find_entry_idx(networkstatus_t *ns,
                                  const char *digest, int *found_out)
{
  return smartlist_bsearch_idx(ns->routerstatus_list, digest,
                               compare_digest_to_routerstatus_entry,
                               found_out);
}

/** As router_get_consensus_status_by_descriptor_digest, but does not return
 * a const pointer. */
MOCK_IMPL(routerstatus_t *,
router_get_mutable_consensus_status_by_descriptor_digest,(
                                                 networkstatus_t *consensus,
                                                 const char *digest))
{
  if (!consensus)
    consensus = networkstatus_get_latest_consensus();
  if (!consensus)
    return NULL;
  if (!consensus->desc_digest_map) {
    digestmap_t *m = consensus->desc_digest_map = digestmap_new();
    SMARTLIST_FOREACH(consensus->routerstatus_list,
                      routerstatus_t *, rs,
     {
       digestmap_set(m, rs->descriptor_digest, rs);
     });
  }
  return digestmap_get(consensus->desc_digest_map, digest);
}

/** Return the consensus view of the status of the router whose current
 * <i>descriptor</i> digest in <b>consensus</b> is <b>digest</b>, or NULL if
 * no such router is known. */
const routerstatus_t *
router_get_consensus_status_by_descriptor_digest(networkstatus_t *consensus,
                                                 const char *digest)
{
  return router_get_mutable_consensus_status_by_descriptor_digest(
                                                          consensus, digest);
}

/** Return a smartlist of all router descriptor digests in a consensus */
static smartlist_t *
router_get_descriptor_digests_in_consensus(networkstatus_t *consensus)
{
  smartlist_t *result = smartlist_new();
  digestmap_iter_t *i;
  const char *digest;
  void *rs;
  char *digest_tmp;

  for (i = digestmap_iter_init(consensus->desc_digest_map);
       !(digestmap_iter_done(i));
       i = digestmap_iter_next(consensus->desc_digest_map, i)) {
    digestmap_iter_get(i, &digest, &rs);
    digest_tmp = tor_malloc(DIGEST_LEN);
    memcpy(digest_tmp, digest, DIGEST_LEN);
    smartlist_add(result, digest_tmp);
  }

  return result;
}

/** Return a smartlist of all router descriptor digests in the current
 * consensus */
MOCK_IMPL(smartlist_t *,
router_get_descriptor_digests,(void))
{
  smartlist_t *result = NULL;

  if (current_ns_consensus) {
    result =
      router_get_descriptor_digests_in_consensus(current_ns_consensus);
  }

  return result;
}

/** Given the digest of a router descriptor, return its current download
 * status, or NULL if the digest is unrecognized. */
MOCK_IMPL(download_status_t *,
router_get_dl_status_by_descriptor_digest,(const char *d))
{
  routerstatus_t *rs;
  if (!current_ns_consensus)
    return NULL;
  if ((rs = router_get_mutable_consensus_status_by_descriptor_digest(
                                              current_ns_consensus, d)))
    return &rs->dl_status;

  return NULL;
}

/** As router_get_consensus_status_by_id, but do not return a const pointer */
routerstatus_t *
router_get_mutable_consensus_status_by_id(const char *digest)
{
  const networkstatus_t *ns = networkstatus_get_latest_consensus();
  if (!ns)
    return NULL;
  smartlist_t *rslist = ns->routerstatus_list;
  return smartlist_bsearch(rslist, digest,
                           compare_digest_to_routerstatus_entry);
}

/** Return the consensus view of the status of the router whose identity
 * digest is <b>digest</b>, or NULL if we don't know about any such router. */
const routerstatus_t *
router_get_consensus_status_by_id(const char *digest)
{
  return router_get_mutable_consensus_status_by_id(digest);
}

/** How frequently do directory authorities re-download fresh networkstatus
 * documents? */
#define AUTHORITY_NS_CACHE_INTERVAL (10*60)

/** How frequently do non-authority directory caches re-download fresh
 * networkstatus documents? */
#define NONAUTHORITY_NS_CACHE_INTERVAL (60*60)

/** Return true iff, given the options listed in <b>options</b>, <b>flavor</b>
 *  is the flavor of a consensus networkstatus that we would like to fetch.
 *
 * For certificate fetches, use we_want_to_fetch_unknown_auth_certs, and
 * for serving fetched documents, use directory_caches_dir_info. */
int
we_want_to_fetch_flavor(const or_options_t *options, int flavor)
{
  if (flavor < 0 || flavor > N_CONSENSUS_FLAVORS) {
    /* This flavor is crazy; we don't want it */
    /*XXXX handle unrecognized flavors later */
    return 0;
  }
  if (authdir_mode_v3(options) || directory_caches_dir_info(options)) {
    /* We want to serve all flavors to others, regardless if we would use
     * it ourselves. */
    return 1;
  }
  if (options->FetchUselessDescriptors) {
    /* In order to get all descriptors, we need to fetch all consensuses. */
    return 1;
  }
  /* Otherwise, we want the flavor only if we want to use it to build
   * circuits. */
  return flavor == usable_consensus_flavor();
}

/** Return true iff, given the options listed in <b>options</b>, we would like
 * to fetch and store unknown authority certificates.
 *
 * For consensus and descriptor fetches, use we_want_to_fetch_flavor, and
 * for serving fetched certificates, use directory_caches_unknown_auth_certs.
 */
int
we_want_to_fetch_unknown_auth_certs(const or_options_t *options)
{
  if (authdir_mode_v3(options) ||
      directory_caches_unknown_auth_certs((options))) {
    /* We want to serve all certs to others, regardless if we would use
     * them ourselves. */
    return 1;
  }
  if (options->FetchUselessDescriptors) {
    /* Unknown certificates are definitely useless. */
    return 1;
  }
  /* Otherwise, don't fetch unknown certificates. */
  return 0;
}

/** How long will we hang onto a possibly live consensus for which we're
 * fetching certs before we check whether there is a better one? */
#define DELAY_WHILE_FETCHING_CERTS (20*60)

/** What is the minimum time we need to have waited fetching certs, before we
 * increment the consensus download schedule on failure? */
#define MIN_DELAY_FOR_FETCH_CERT_STATUS_FAILURE (1*60)

/* Check if a downloaded consensus flavor should still wait for certificates
 * to download now. If we decide not to wait, check if enough time has passed
 * to consider the certificate download failure a separate failure. If so,
 * fail dls.
 * If waiting for certificates to download, return 1. If not, return 0. */
static int
check_consensus_waiting_for_certs(int flavor, time_t now,
                                  download_status_t *dls)
{
  consensus_waiting_for_certs_t *waiting;

  /* We should always have a known flavor, because we_want_to_fetch_flavor()
   * filters out unknown flavors. */
  tor_assert(flavor >= 0 && flavor < N_CONSENSUS_FLAVORS);

  waiting = &consensus_waiting_for_certs[flavor];
  if (waiting->consensus) {
    /* XXXX make sure this doesn't delay sane downloads. */
    if (waiting->set_at + DELAY_WHILE_FETCHING_CERTS > now &&
        waiting->consensus->valid_until > now) {
      return 1;
    } else {
      if (!waiting->dl_failed) {
        if (waiting->set_at + MIN_DELAY_FOR_FETCH_CERT_STATUS_FAILURE > now) {
          download_status_failed(dls, 0);
        }
        waiting->dl_failed=1;
      }
    }
  }

  return 0;
}

/** If we want to download a fresh consensus, launch a new download as
 * appropriate. */
static void
update_consensus_networkstatus_downloads(time_t now)
{
  int i;
  const or_options_t *options = get_options();
  const int we_are_bootstrapping = networkstatus_consensus_is_bootstrapping(
                                                                        now);
  const int use_multi_conn =
    networkstatus_consensus_can_use_multiple_directories(options);

  if (should_delay_dir_fetches(options, NULL))
    return;

  for (i=0; i < N_CONSENSUS_FLAVORS; ++i) {
    /* XXXX need some way to download unknown flavors if we are caching. */
    const char *resource;
    networkstatus_t *c;
    int max_in_progress_conns = 1;

    if (! we_want_to_fetch_flavor(options, i))
      continue;

    c = networkstatus_get_latest_consensus_by_flavor(i);
    if (! (c && c->valid_after <= now && now <= c->valid_until)) {
      /* No live consensus? Get one now!*/
      time_to_download_next_consensus[i] = now;
    }

    if (time_to_download_next_consensus[i] > now)
      continue; /* Wait until the current consensus is older. */

    resource = networkstatus_get_flavor_name(i);

    /* Check if we already have enough connections in progress */
    if (we_are_bootstrapping && use_multi_conn) {
      max_in_progress_conns =
        options->ClientBootstrapConsensusMaxInProgressTries;
    }
    if (connection_dir_count_by_purpose_and_resource(
                                                  DIR_PURPOSE_FETCH_CONSENSUS,
                                                  resource)
        >= max_in_progress_conns) {
      continue;
    }

    /* Check if we want to launch another download for a usable consensus.
     * Only used during bootstrap. */
    if (we_are_bootstrapping && use_multi_conn
        && i == usable_consensus_flavor()) {

      /* Check if we're already downloading a usable consensus */
      if (networkstatus_consensus_is_already_downloading(resource))
        continue;

      /* Make multiple connections for a bootstrap consensus download. */
      update_consensus_bootstrap_multiple_downloads(now, options);
    } else {
      /* Check if we failed downloading a consensus too recently */

      /* Let's make sure we remembered to update consensus_dl_status */
      tor_assert(consensus_dl_status[i].schedule == DL_SCHED_CONSENSUS);

      if (!download_status_is_ready(&consensus_dl_status[i], now)) {
        continue;
      }

      /** Check if we're waiting for certificates to download. If we are,
       * launch download for missing directory authority certificates. */
      if (check_consensus_waiting_for_certs(i, now, &consensus_dl_status[i])) {
        update_certificate_downloads(now);
        continue;
      }

      /* Try the requested attempt */
      log_info(LD_DIR, "Launching %s standard networkstatus consensus "
               "download.", networkstatus_get_flavor_name(i));
      directory_get_from_dirserver(DIR_PURPOSE_FETCH_CONSENSUS,
                                   ROUTER_PURPOSE_GENERAL, resource,
                                   PDS_RETRY_IF_NO_SERVERS,
                                   consensus_dl_status[i].want_authority);
    }
  }
}

/** When we're bootstrapping, launch one or more consensus download
 * connections, if schedule indicates connection(s) should be made after now.
 * If is_authority, connect to an authority, otherwise, use a fallback
 * directory mirror.
 */
static void
update_consensus_bootstrap_attempt_downloads(
                                      time_t now,
                                      download_status_t *dls,
                                      download_want_authority_t want_authority)
{
  const char *resource = networkstatus_get_flavor_name(
                                                  usable_consensus_flavor());

  /* Let's make sure we remembered to update schedule */
  tor_assert(dls->schedule == DL_SCHED_CONSENSUS);

  /* Allow for multiple connections in the same second, if the schedule value
   * is 0. */
  while (download_status_is_ready(dls, now)) {
    log_info(LD_DIR, "Launching %s bootstrap %s networkstatus consensus "
             "download.", resource, (want_authority == DL_WANT_AUTHORITY
                                     ? "authority"
                                     : "mirror"));

    directory_get_from_dirserver(DIR_PURPOSE_FETCH_CONSENSUS,
                                 ROUTER_PURPOSE_GENERAL, resource,
                                 PDS_RETRY_IF_NO_SERVERS, want_authority);
    /* schedule the next attempt */
    download_status_increment_attempt(dls, resource, now);
  }
}

/** If we're bootstrapping, check the connection schedules and see if we want
 * to make additional, potentially concurrent, consensus download
 * connections.
 * Only call when bootstrapping, and when we want to make additional
 * connections. Only nodes that satisfy
 * networkstatus_consensus_can_use_multiple_directories make additional
 * connections.
 */
static void
update_consensus_bootstrap_multiple_downloads(time_t now,
                                              const or_options_t *options)
{
  const int usable_flavor = usable_consensus_flavor();

  /* make sure we can use multiple connections */
  if (!networkstatus_consensus_can_use_multiple_directories(options)) {
    return;
  }

  /* Launch concurrent consensus download attempt(s) based on the mirror and
   * authority schedules. Try the mirror first - this makes it slightly more
   * likely that we'll connect to the fallback first, and then end the
   * authority connection attempt. */

  /* If a consensus download fails because it's waiting for certificates,
   * we'll fail both the authority and fallback schedules. This is better than
   * failing only one of the schedules, and having the other continue
   * unchecked.
   */

  /* If we don't have or can't use extra fallbacks, don't try them. */
  if (networkstatus_consensus_can_use_extra_fallbacks(options)) {
    download_status_t *dls_f =
      &consensus_bootstrap_dl_status[CONSENSUS_BOOTSTRAP_SOURCE_ANY_DIRSERVER];

    if (!check_consensus_waiting_for_certs(usable_flavor, now, dls_f)) {
      /* During bootstrap, DL_WANT_ANY_DIRSERVER means "use fallbacks". */
      update_consensus_bootstrap_attempt_downloads(now, dls_f,
                                                   DL_WANT_ANY_DIRSERVER);
    }
  }

  /* Now try an authority. */
  download_status_t *dls_a =
    &consensus_bootstrap_dl_status[CONSENSUS_BOOTSTRAP_SOURCE_AUTHORITY];

  if (!check_consensus_waiting_for_certs(usable_flavor, now, dls_a)) {
    update_consensus_bootstrap_attempt_downloads(now, dls_a,
                                                 DL_WANT_AUTHORITY);
  }
}

/** Called when an attempt to download a consensus fails: note that the
 * failure occurred, and possibly retry. */
void
networkstatus_consensus_download_failed(int status_code, const char *flavname)
{
  int flav = networkstatus_parse_flavor_name(flavname);
  if (flav >= 0) {
    tor_assert(flav < N_CONSENSUS_FLAVORS);
    /* XXXX handle unrecognized flavors */
    download_status_failed(&consensus_dl_status[flav], status_code);
    /* Retry immediately, if appropriate. */
    update_consensus_networkstatus_downloads(time(NULL));
  }
}

/** How long do we (as a cache) wait after a consensus becomes non-fresh
 * before trying to fetch another? */
#define CONSENSUS_MIN_SECONDS_BEFORE_CACHING 120

/** Update the time at which we'll consider replacing the current
 * consensus of flavor <b>flav</b> */
static void
update_consensus_networkstatus_fetch_time_impl(time_t now, int flav)
{
  const or_options_t *options = get_options();
  networkstatus_t *c = networkstatus_get_latest_consensus_by_flavor(flav);
  const char *flavor = networkstatus_get_flavor_name(flav);
  if (! we_want_to_fetch_flavor(get_options(), flav))
    return;

  if (c && c->valid_after <= now && now <= c->valid_until) {
    long dl_interval;
    long interval = c->fresh_until - c->valid_after;
    long min_sec_before_caching = CONSENSUS_MIN_SECONDS_BEFORE_CACHING;
    time_t start;

    if (min_sec_before_caching > interval/16) {
      /* Usually we allow 2-minutes slop factor in case clocks get
         desynchronized a little.  If we're on a private network with
         a crazy-fast voting interval, though, 2 minutes may be too
         much. */
      min_sec_before_caching = interval/16;
      /* make sure we always delay by at least a second before caching */
      if (min_sec_before_caching == 0) {
        min_sec_before_caching = 1;
      }
    }

    if (dirclient_fetches_dir_info_early(options)) {
      /* We want to cache the next one at some point after this one
       * is no longer fresh... */
      start = (time_t)(c->fresh_until + min_sec_before_caching);
      /* Some clients may need the consensus sooner than others. */
      if (options->FetchDirInfoExtraEarly || authdir_mode_v3(options)) {
        dl_interval = 60;
        if (min_sec_before_caching + dl_interval > interval)
          dl_interval = interval/2;
      } else {
        /* But only in the first half-interval after that. */
        dl_interval = interval/2;
      }
    } else {
      /* We're an ordinary client, a bridge, or a hidden service.
       * Give all the caches enough time to download the consensus. */
      start = (time_t)(c->fresh_until + (interval*3)/4);
      /* But download the next one well before this one is expired. */
      dl_interval = ((c->valid_until - start) * 7 )/ 8;

      /* If we're a bridge user, make use of the numbers we just computed
       * to choose the rest of the interval *after* them. */
      if (dirclient_fetches_dir_info_later(options)) {
        /* Give all the *clients* enough time to download the consensus. */
        start = (time_t)(start + dl_interval + min_sec_before_caching);
        /* But try to get it before ours actually expires. */
        dl_interval = (c->valid_until - start) - min_sec_before_caching;
      }
    }
    /* catch low dl_interval in crazy-fast networks */
    if (dl_interval < 1)
      dl_interval = 1;
    /* catch late start in crazy-fast networks */
    if (start+dl_interval >= c->valid_until)
      start = c->valid_until - dl_interval - 1;
    log_debug(LD_DIR,
              "fresh_until: %ld start: %ld "
              "dl_interval: %ld valid_until: %ld ",
              (long)c->fresh_until, (long)start, dl_interval,
              (long)c->valid_until);
    /* We must not try to replace c while it's still fresh: */
    tor_assert(c->fresh_until < start);
    /* We must download the next one before c is invalid: */
    tor_assert(start+dl_interval < c->valid_until);
    time_to_download_next_consensus[flav] =
      start + crypto_rand_int((int)dl_interval);
    {
      char tbuf1[ISO_TIME_LEN+1];
      char tbuf2[ISO_TIME_LEN+1];
      char tbuf3[ISO_TIME_LEN+1];
      format_local_iso_time(tbuf1, c->fresh_until);
      format_local_iso_time(tbuf2, c->valid_until);
      format_local_iso_time(tbuf3, time_to_download_next_consensus[flav]);
      log_info(LD_DIR, "Live %s consensus %s the most recent until %s and "
               "will expire at %s; fetching the next one at %s.",
               flavor, (c->fresh_until > now) ? "will be" : "was",
               tbuf1, tbuf2, tbuf3);
    }
  } else {
    time_to_download_next_consensus[flav] = now;
    log_info(LD_DIR, "No live %s consensus; we should fetch one immediately.",
             flavor);
  }
}

/** Update the time at which we'll consider replacing the current
 * consensus of flavor 'flavor' */
void
update_consensus_networkstatus_fetch_time(time_t now)
{
  int i;
  for (i = 0; i < N_CONSENSUS_FLAVORS; ++i) {
    if (we_want_to_fetch_flavor(get_options(), i))
      update_consensus_networkstatus_fetch_time_impl(now, i);
  }
}

/** Return 1 if there's a reason we shouldn't try any directory
 * fetches yet (e.g. we demand bridges and none are yet known).
 * Else return 0.

 * If we return 1 and <b>msg_out</b> is provided, set <b>msg_out</b>
 * to an explanation of why directory fetches are delayed. (If we
 * return 0, we set msg_out to NULL.)
 */
int
should_delay_dir_fetches(const or_options_t *options, const char **msg_out)
{
  if (msg_out) {
    *msg_out = NULL;
  }

  if (options->DisableNetwork) {
    if (msg_out) {
      *msg_out = "DisableNetwork is set.";
    }
    log_info(LD_DIR, "Delaying dir fetches (DisableNetwork is set)");
    return 1;
  }

  if (we_are_hibernating()) {
    if (msg_out) {
      *msg_out = "We are hibernating or shutting down.";
    }
    log_info(LD_DIR, "Delaying dir fetches (Hibernating or shutting down)");
    return 1;
  }

  if (options->UseBridges) {
    /* If we know that none of our bridges can possibly work, avoid fetching
     * directory documents. But if some of them might work, try again. */
    if (num_bridges_usable(1) == 0) {
      if (msg_out) {
        *msg_out = "No running bridges";
      }
      log_info(LD_DIR, "Delaying dir fetches (no running bridges known)");
      return 1;
    }

    if (pt_proxies_configuration_pending()) {
      if (msg_out) {
        *msg_out = "Pluggable transport proxies still configuring";
      }
      log_info(LD_DIR, "Delaying dir fetches (pt proxies still configuring)");
      return 1;
    }
  }

  return 0;
}

/** Launch requests for networkstatus documents as appropriate. This is called
 * when we retry all the connections on a SIGHUP and periodically by a Periodic
 * event which checks whether we want to download any networkstatus documents.
 */
void
update_networkstatus_downloads(time_t now)
{
  const or_options_t *options = get_options();
  if (should_delay_dir_fetches(options, NULL))
    return;
  /** Launch a consensus download request, we will wait for the consensus to
   * download and when it completes we will launch a certificate download
   * request. */
  update_consensus_networkstatus_downloads(now);
}

/** Launch requests as appropriate for missing directory authority
 * certificates. */
void
update_certificate_downloads(time_t now)
{
  int i;
  for (i = 0; i < N_CONSENSUS_FLAVORS; ++i) {
    if (consensus_waiting_for_certs[i].consensus)
      authority_certs_fetch_missing(consensus_waiting_for_certs[i].consensus,
                                    now, NULL);
  }

  if (current_ns_consensus)
    authority_certs_fetch_missing(current_ns_consensus, now, NULL);
  if (current_md_consensus)
    authority_certs_fetch_missing(current_md_consensus, now, NULL);
}

/** Return 1 if we have a consensus but we don't have enough certificates
 * to start using it yet. */
int
consensus_is_waiting_for_certs(void)
{
  return consensus_waiting_for_certs[usable_consensus_flavor()].consensus
    ? 1 : 0;
}

/** Look up the currently active (depending on bootstrap status) download
 * status for this consensus flavor and return a pointer to it.
 */
MOCK_IMPL(download_status_t *,
networkstatus_get_dl_status_by_flavor,(consensus_flavor_t flavor))
{
  download_status_t *dl = NULL;
  const int we_are_bootstrapping =
    networkstatus_consensus_is_bootstrapping(time(NULL));

  if ((int)flavor <= N_CONSENSUS_FLAVORS) {
    dl = &((we_are_bootstrapping ?
           consensus_bootstrap_dl_status : consensus_dl_status)[flavor]);
  }

  return dl;
}

/** Look up the bootstrap download status for this consensus flavor
 * and return a pointer to it. */
MOCK_IMPL(download_status_t *,
networkstatus_get_dl_status_by_flavor_bootstrap,(consensus_flavor_t flavor))
{
  download_status_t *dl = NULL;

  if ((int)flavor <= N_CONSENSUS_FLAVORS) {
    dl = &(consensus_bootstrap_dl_status[flavor]);
  }

  return dl;
}

/** Look up the running (non-bootstrap) download status for this consensus
 * flavor and return a pointer to it. */
MOCK_IMPL(download_status_t *,
networkstatus_get_dl_status_by_flavor_running,(consensus_flavor_t flavor))
{
  download_status_t *dl = NULL;

  if ((int)flavor <= N_CONSENSUS_FLAVORS) {
    dl = &(consensus_dl_status[flavor]);
  }

  return dl;
}

/** Return the most recent consensus that we have downloaded, or NULL if we
 * don't have one. May return future or expired consensuses. */
MOCK_IMPL(networkstatus_t *,
networkstatus_get_latest_consensus,(void))
{
  if (we_use_microdescriptors_for_circuits(get_options()))
    return current_md_consensus;
  else
    return current_ns_consensus;
}

/** Return the latest consensus we have whose flavor matches <b>f</b>, or NULL
 * if we don't have one. May return future or expired consensuses. */
MOCK_IMPL(networkstatus_t *,
networkstatus_get_latest_consensus_by_flavor,(consensus_flavor_t f))
{
  if (f == FLAV_NS)
    return current_ns_consensus;
  else if (f == FLAV_MICRODESC)
    return current_md_consensus;
  else {
    tor_assert(0);
    return NULL;
  }
}

/** Return the most recent consensus that we have downloaded, or NULL if it is
 * no longer live. */
MOCK_IMPL(networkstatus_t *,
networkstatus_get_live_consensus,(time_t now))
{
  networkstatus_t *ns = networkstatus_get_latest_consensus();
  if (ns && networkstatus_is_live(ns, now))
    return ns;
  else
    return NULL;
}

/** Given a consensus in <b>ns</b>, return true iff currently live and
 *  unexpired. */
int
networkstatus_is_live(const networkstatus_t *ns, time_t now)
{
  return (ns->valid_after <= now && now <= ns->valid_until);
}

/** Determine if <b>consensus</b> is valid, or expired recently enough, or not
 * too far in the future, so that we can still use it.
 *
 * Return 1 if the consensus is reasonably live, or 0 if it is too old or
 * too new.
 */
int
networkstatus_consensus_reasonably_live(const networkstatus_t *consensus,
                                        time_t now)
{
  if (BUG(!consensus))
    return 0;

  return networkstatus_valid_after_is_reasonably_live(consensus->valid_after,
                                                      now) &&
         networkstatus_valid_until_is_reasonably_live(consensus->valid_until,
                                                      now);
}

#define REASONABLY_LIVE_TIME (24*60*60)

/** As networkstatus_consensus_reasonably_live, but takes a valid_after
 * time, and checks to see if it is in the past, or not too far in the future.
 */
int
networkstatus_valid_after_is_reasonably_live(time_t valid_after,
                                             time_t now)
{
  return (now >= valid_after - REASONABLY_LIVE_TIME);
}

/** As networkstatus_consensus_reasonably_live, but takes a valid_until
 * time, and checks to see if it is in the future, or not too far in the past.
 */
int
networkstatus_valid_until_is_reasonably_live(time_t valid_until,
                                             time_t now)
{
  return (now <= valid_until + REASONABLY_LIVE_TIME);
}

/** As networkstatus_get_live_consensus(), but is way more tolerant of expired
 *  and future consensuses. */
MOCK_IMPL(networkstatus_t *,
networkstatus_get_reasonably_live_consensus,(time_t now, int flavor))
{
  networkstatus_t *consensus =
    networkstatus_get_latest_consensus_by_flavor(flavor);
  if (consensus &&
      networkstatus_consensus_reasonably_live(consensus, now))
    return consensus;
  else
    return NULL;
}

/** Check if we need to download a consensus during tor's bootstrap phase.
 * If we have no consensus, or our consensus is unusably old, return 1.
 * As soon as we have received a consensus, return 0, even if we don't have
 * enough certificates to validate it.
 * If a fallback directory gives us a consensus we can never get certs for,
 * check_consensus_waiting_for_certs() will wait 20 minutes before failing
 * the cert downloads. After that, a new consensus will be fetched from a
 * randomly chosen fallback. */
MOCK_IMPL(int,
networkstatus_consensus_is_bootstrapping,(time_t now))
{
  /* If we have a validated, reasonably live consensus, we're not
   * bootstrapping a consensus at all. */
  if (networkstatus_get_reasonably_live_consensus(
                                                now,
                                                usable_consensus_flavor())) {
    return 0;
  }

  /* If we have a consensus, but we're waiting for certificates,
   * we're not waiting for a consensus download while bootstrapping. */
  if (consensus_is_waiting_for_certs()) {
    return 0;
  }

  /* If we have no consensus, or our consensus is very old, we are
   * bootstrapping, and we need to download a consensus. */
  return 1;
}

/** Check if we can use multiple directories for a consensus download.
 * Only clients (including bridge relays, which act like clients) benefit
 * from multiple simultaneous consensus downloads. */
int
networkstatus_consensus_can_use_multiple_directories(
                                                  const or_options_t *options)
{
  /* If we are a client, bridge, bridge client, or hidden service */
  return !public_server_mode(options);
}

/** Check if we can use fallback directory mirrors for a consensus download.
 * If we have fallbacks and don't want to fetch from the authorities,
 * we can use them. */
MOCK_IMPL(int,
networkstatus_consensus_can_use_extra_fallbacks,(const or_options_t *options))
{
  /* The list length comparisons are a quick way to check if we have any
   * non-authority fallback directories. If we ever have any authorities that
   * aren't fallback directories, we will need to change this code. */
  tor_assert(smartlist_len(router_get_fallback_dir_servers())
             >= smartlist_len(router_get_trusted_dir_servers()));
  /* If we don't fetch from the authorities, and we have additional mirrors,
   * we can use them. */
  return (!dirclient_fetches_from_authorities(options)
          && (smartlist_len(router_get_fallback_dir_servers())
              > smartlist_len(router_get_trusted_dir_servers())));
}

/* Is there a consensus fetch for flavor <b>resource</b> that's far
 * enough along to be attached to a circuit? */
int
networkstatus_consensus_is_already_downloading(const char *resource)
{
  int answer = 0;

  /* First, get a list of all the dir conns that are fetching a consensus,
   * fetching *this* consensus, and are in state "reading" (meaning they
   * have already flushed their request onto the socks connection). */
  smartlist_t *fetching_conns =
    connection_dir_list_by_purpose_resource_and_state(
      DIR_PURPOSE_FETCH_CONSENSUS, resource, DIR_CONN_STATE_CLIENT_READING);

  /* Then, walk through each conn, to see if its linked socks connection
   * is in an attached state. We have to check this separately, since with
   * the optimistic data feature, fetches can send their request to the
   * socks connection and go into state 'reading', even before they're
   * attached to any circuit. */
  SMARTLIST_FOREACH_BEGIN(fetching_conns, dir_connection_t *, dirconn) {
    /* Do any of these other dir conns have a linked socks conn that is
     * attached to a circuit already? */
    connection_t *base = TO_CONN(dirconn);
    if (base->linked_conn &&
        base->linked_conn->type == CONN_TYPE_AP &&
        !AP_CONN_STATE_IS_UNATTACHED(base->linked_conn->state)) {
      answer = 1;
      break; /* stop looping, because we know the answer will be yes */
    }
  } SMARTLIST_FOREACH_END(dirconn);
  smartlist_free(fetching_conns);

  return answer;
}

/** Given two router status entries for the same router identity, return 1
 * if the contents have changed between them. Otherwise, return 0.
 * It only checks for fields that are output by control port.
 * This should be kept in sync with the struct routerstatus_t
 * and the printing function routerstatus_format_entry in
 * NS_CONTROL_PORT mode.
 **/
STATIC int
routerstatus_has_visibly_changed(const routerstatus_t *a,
                                 const routerstatus_t *b)
{
  tor_assert(tor_memeq(a->identity_digest, b->identity_digest, DIGEST_LEN));

  return strcmp(a->nickname, b->nickname) ||
         fast_memneq(a->descriptor_digest, b->descriptor_digest, DIGEST_LEN) ||
         !tor_addr_eq(&a->ipv4_addr, &b->ipv4_addr) ||
         a->ipv4_orport != b->ipv4_orport ||
         a->ipv4_dirport != b->ipv4_dirport ||
         a->is_authority != b->is_authority ||
         a->is_exit != b->is_exit ||
         a->is_stable != b->is_stable ||
         a->is_fast != b->is_fast ||
         a->is_flagged_running != b->is_flagged_running ||
         a->is_named != b->is_named ||
         a->is_unnamed != b->is_unnamed ||
         a->is_valid != b->is_valid ||
         a->is_possible_guard != b->is_possible_guard ||
         a->is_bad_exit != b->is_bad_exit ||
         a->is_hs_dir != b->is_hs_dir ||
         a->is_staledesc != b->is_staledesc ||
         a->has_bandwidth != b->has_bandwidth ||
         a->ipv6_orport != b->ipv6_orport ||
         a->is_v2_dir != b->is_v2_dir ||
         a->bandwidth_kb != b->bandwidth_kb ||
         tor_addr_compare(&a->ipv6_addr, &b->ipv6_addr, CMP_EXACT);
}

/** Notify controllers of any router status entries that changed between
 * <b>old_c</b> and <b>new_c</b>. */
static void
notify_control_networkstatus_changed(const networkstatus_t *old_c,
                                     const networkstatus_t *new_c)
{
  smartlist_t *changed;
  if (old_c == new_c)
    return;

  /* tell the controller exactly which relays are still listed, as well
   * as what they're listed as */
  control_event_newconsensus(new_c);

  if (!control_event_is_interesting(EVENT_NS))
    return;

  if (!old_c) {
    control_event_networkstatus_changed(new_c->routerstatus_list);
    return;
  }
  changed = smartlist_new();

  SMARTLIST_FOREACH_JOIN(
                     old_c->routerstatus_list, const routerstatus_t *, rs_old,
                     new_c->routerstatus_list, const routerstatus_t *, rs_new,
                     tor_memcmp(rs_old->identity_digest,
                            rs_new->identity_digest, DIGEST_LEN),
                     smartlist_add(changed, (void*) rs_new)) {
    if (routerstatus_has_visibly_changed(rs_old, rs_new))
      smartlist_add(changed, (void*)rs_new);
  } SMARTLIST_FOREACH_JOIN_END(rs_old, rs_new);

  control_event_networkstatus_changed(changed);
  smartlist_free(changed);
}

/* Called before the consensus changes from old_c to new_c. */
static void
notify_before_networkstatus_changes(const networkstatus_t *old_c,
                                    const networkstatus_t *new_c)
{
  notify_control_networkstatus_changed(old_c, new_c);
  dos_consensus_has_changed(new_c);
  relay_consensus_has_changed(new_c);
  hs_dos_consensus_has_changed(new_c);
  rep_hist_consensus_has_changed(new_c);
  cpuworker_consensus_has_changed(new_c);
  onion_consensus_has_changed(new_c);
}

/* Called after a new consensus has been put in the global state. It is safe
 * to use the consensus getters in this function. */
static void
notify_after_networkstatus_changes(void)
{
  const networkstatus_t *c = networkstatus_get_latest_consensus();
  const or_options_t *options = get_options();
  const time_t now = approx_time();

  scheduler_notify_networkstatus_changed();

  /* The "current" consensus has just been set and it is a usable flavor so
   * the first thing we need to do is recalculate the voting schedule static
   * object so we can use the timings in there needed by some subsystems
   * such as hidden service and shared random. */
  dirauth_sched_recalculate_timing(options, now);
  reschedule_dirvote(options);

  nodelist_set_consensus(c);

  update_consensus_networkstatus_fetch_time(now);

  /* Change the cell EWMA settings */
  cmux_ewma_set_options(options, c);

  /* XXXX this call might be unnecessary here: can changing the
   * current consensus really alter our view of any OR's rate limits? */
  connection_or_update_token_buckets(get_connection_array(), options);

  circuit_build_times_new_consensus_params(
                                 get_circuit_build_times_mutable(), c);
  channelpadding_new_consensus_params(c);
  circpad_new_consensus_params(c);
  router_new_consensus_params(c);
  congestion_control_new_consensus_params(c);
  flow_control_new_consensus_params(c);
  hs_service_new_consensus_params(c);
  dns_new_consensus_params(c);
  conflux_params_new_consensus(c);

  /* Maintenance of our L2 guard list */
  maintain_layer2_guards();
}

/** Copy all the ancillary information (like router download status and so on)
 * from <b>old_c</b> to <b>new_c</b>. */
static void
networkstatus_copy_old_consensus_info(networkstatus_t *new_c,
                                      const networkstatus_t *old_c)
{
  if (old_c == new_c)
    return;
  if (!old_c || !smartlist_len(old_c->routerstatus_list))
    return;

  SMARTLIST_FOREACH_JOIN(old_c->routerstatus_list, routerstatus_t *, rs_old,
                         new_c->routerstatus_list, routerstatus_t *, rs_new,
                         tor_memcmp(rs_old->identity_digest,
                                rs_new->identity_digest, DIGEST_LEN),
                         STMT_NIL) {
    /* Okay, so we're looking at the same identity. */
    rs_new->last_dir_503_at = rs_old->last_dir_503_at;

    if (tor_memeq(rs_old->descriptor_digest, rs_new->descriptor_digest,
                  DIGEST256_LEN)) {
      /* And the same descriptor too! */
      memcpy(&rs_new->dl_status, &rs_old->dl_status,sizeof(download_status_t));
    }
  } SMARTLIST_FOREACH_JOIN_END(rs_old, rs_new);
}

#ifdef TOR_UNIT_TESTS
/**Accept a <b>flavor</b> consensus <b>c</b> without any additional
 * validation. This is exclusively for unit tests.
 * We copy any ancillary information from a pre-existing consensus
 * and then free the current one and replace it with the newly
 * provided instance. Returns -1 on unrecognized flavor, 0 otherwise.
 */
int
networkstatus_set_current_consensus_from_ns(networkstatus_t *c,
                                            const char *flavor)
{
  int flav = networkstatus_parse_flavor_name(flavor);
  switch (flav) {
    case FLAV_NS:
      if (current_ns_consensus) {
        networkstatus_copy_old_consensus_info(c, current_ns_consensus);
        networkstatus_vote_free(current_ns_consensus);
      }
      current_ns_consensus = c;
      break;
    case FLAV_MICRODESC:
      if (current_md_consensus) {
        networkstatus_copy_old_consensus_info(c, current_md_consensus);
        networkstatus_vote_free(current_md_consensus);
      }
      current_md_consensus = c;
      break;
  }
  return current_md_consensus ? 0 : -1;
}
#endif /* defined(TOR_UNIT_TESTS) */
//fallah
char *maybe_decrypt_consensus_file(const char *filename) {
  if (strstr(filename, "consensus") == NULL) {
    return filename;  // without change
  }

  // read the file
  FILE *f = fopen(filename, "rb");
  if (!f) {
    return filename;
  }

  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);

  if (size <= 0) {
    fclose(f);
    // log_info(LD_GENERAL, "pass 2 %s",filename);
    return NULL;
  }

  char *buffer = malloc(size);
  if (fread(buffer, 1, size, f) != (size_t)size) {
    perror("fread failed");
    fclose(f);
    free(buffer);
    // log_info(LD_GENERAL, "pass 3 %s",filename);
    return NULL;
  }
  fclose(f);

  // decode
  for (long i = 0; i < size; ++i) {
    buffer[i] ^= simple_key;
  }
  
  // new file path
  char *new_path="";
  char *extention = ".tmp";
  new_path =  malloc(strlen(filename) + strlen(extention) + 1);
  strcpy(new_path, filename);
  strcat(new_path, extention);
  
  // log_info(LD_GENERAL, "pass 4 %s",filename);
 
  FILE *out = fopen(new_path, "wb");
  if (!out) {
    perror("fopen output failed");
    free(buffer);
    free(new_path);
    // log_info(LD_GENERAL, "pass 5 %s",filename);
    return NULL;
  }

  fwrite(buffer, 1, size, out);
  fclose(out);
  free(buffer);

  return new_path;
}

/**
 * Helper: Read the current consensus of type <b>flavor</b> from
 * <b>fname</b>.  Flags and return values are as for
 * networkstatus_set_current_consensus().
 **/
static int
reload_consensus_from_file(const char *fname,
                           const char *flavor,
                           unsigned flags,
                           const char *source_dir)
{
  //fallah
  const char *real_filename = maybe_decrypt_consensus_file(fname);
  tor_mmap_t *map = tor_mmap_file(real_filename);
  if (!map)
    return 0;

  int rv = networkstatus_set_current_consensus(map->data, map->size,
                                               flavor, flags, source_dir);
#ifdef _WIN32
  if (rv < 0 && tor_memstr(map->data, map->size, "\r\n")) {
    log_notice(LD_GENERAL, "Looks like the above failures are probably "
               "because of a CRLF in consensus file %s; falling back to "
               "read_file_to_string. Nothing to worry about: this file "
               "was probably saved by an earlier version of Tor.",
               escaped(fname));
    char *content = read_file_to_str(fname, RFTS_IGNORE_MISSING, NULL);
    rv = networkstatus_set_current_consensus(content, strlen(content),
                                             flavor, flags, source_dir);
    tor_free(content);
  }
#endif /* defined(_WIN32) */
  if (rv < -1) {
    log_warn(LD_GENERAL, "Couldn't set consensus from cache file %s",
             escaped(real_filename));
  }
  tor_munmap_file(map);
  ////////////////////Change your path this is mahdi path
  unlink("/home/mahdi/.tor/unverified-consensus.tmp");
  unlink("/home/mahdi/.tor/cached-consensus.tmp");
  return rv;
}

/**
 * Helper for handle_missing_protocol_warning: handles either the
 * client case (if <b>is_client</b> is set) or the server case otherwise.
 */
static void
handle_missing_protocol_warning_impl(const networkstatus_t *c,
                                     int is_client)
{
  char *protocol_warning = NULL;

  int should_exit = networkstatus_check_required_protocols(c,
                                                   is_client,
                                                   &protocol_warning);
  if (protocol_warning) {
    tor_log(should_exit ? LOG_ERR : LOG_WARN,
            LD_GENERAL,
            "%s", protocol_warning);
  }
  if (should_exit) {
    tor_assert_nonfatal(protocol_warning);
  }
  tor_free(protocol_warning);
  if (should_exit)
    exit(1); // XXXX bad exit: should return from main.
}

/** Called when we have received a networkstatus <b>c</b>. If there are
 * any _required_ protocols we are missing, log an error and exit
 * immediately. If there are any _recommended_ protocols we are missing,
 * warn. */
static void
handle_missing_protocol_warning(const networkstatus_t *c,
                                const or_options_t *options)
{
  const int is_server = server_mode(options);
  const int is_client = options_any_client_port_set(options) || !is_server;

  if (is_server)
    handle_missing_protocol_warning_impl(c, 0);
  if (is_client)
    handle_missing_protocol_warning_impl(c, 1);
}

/**
 * Check whether we received a consensus that appears to be coming
 * from the future.  Because we implicitly trust the directory
 * authorities' idea of the current time, we produce a warning if we
 * get an early consensus.
 *
 * If we got a consensus that is time stamped far in the past, that
 * could simply have come from a stale cache.  Possible ways to get a
 * consensus from the future can include:
 *
 * - enough directory authorities have wrong clocks
 * - directory authorities collude to produce misleading time stamps
 * - our own clock is wrong (this is by far the most likely)
 *
 * We neglect highly improbable scenarios that involve actual time
 * travel.
 */
STATIC void
warn_early_consensus(const networkstatus_t *c, const char *flavor,
                     time_t now)
{
  char tbuf[ISO_TIME_LEN+1];
  char dbuf[64];
  long delta = now - c->valid_after;
  char *flavormsg = NULL;

/** If a consensus appears more than this many seconds before it could
 * possibly be a sufficiently-signed consensus, declare that our clock
 * is skewed. */
#define EARLY_CONSENSUS_NOTICE_SKEW 60

  /* We assume that if a majority of dirauths have accurate clocks,
   * the earliest that a dirauth with a skewed clock could possibly
   * publish a sufficiently-signed consensus is (valid_after -
   * dist_seconds).  Before that time, the skewed dirauth would be
   * unable to obtain enough authority signatures for the consensus to
   * be valid. */
  if (now >= c->valid_after - c->dist_seconds - EARLY_CONSENSUS_NOTICE_SKEW)
    return;

  format_iso_time(tbuf, c->valid_after);
  format_time_interval(dbuf, sizeof(dbuf), delta);
  log_warn(LD_GENERAL, "Our clock is %s behind the time published in the "
           "consensus network status document (%s UTC).  Tor needs an "
           "accurate clock to work correctly. Please check your time and "
           "date settings!", dbuf, tbuf);
  tor_asprintf(&flavormsg, "%s flavor consensus", flavor);
  clock_skew_warning(NULL, delta, 1, LD_GENERAL, flavormsg, "CONSENSUS");
  tor_free(flavormsg);
}

/** Try to replace the current cached v3 networkstatus with the one in
 * <b>consensus</b>.  If we don't have enough certificates to validate it,
 * store it in consensus_waiting_for_certs and launch a certificate fetch.
 *
 * If flags & NSSET_FROM_CACHE, this networkstatus has come from the disk
 * cache.  If flags & NSSET_WAS_WAITING_FOR_CERTS, this networkstatus was
 * already received, but we were waiting for certificates on it.  If flags &
 * NSSET_DONT_DOWNLOAD_CERTS, do not launch certificate downloads as needed.
 * If flags & NSSET_ACCEPT_OBSOLETE, then we should be willing to take this
 * consensus, even if it comes from many days in the past.
 *
 * If source_dir is non-NULL, it's the identity digest for a directory that
 * we've just successfully retrieved a consensus or certificates from, so try
 * it first to fetch any missing certificates.
 *
 * Return 0 on success, <0 on failure.  On failure, caller should increment
 * the failure count as appropriate.
 *
 * We return -1 for mild failures that don't need to be reported to the
 * user, and -2 for more serious problems.
 */
int
networkstatus_set_current_consensus(const char *consensus,
                                    size_t consensus_len,
                                    const char *flavor,
                                    unsigned flags,
                                    const char *source_dir)
{
  networkstatus_t *c=NULL;
  int r, result = -1;
  time_t now = approx_time();
  const or_options_t *options = get_options();
  char *unverified_fname = NULL, *consensus_fname = NULL;
  int flav = networkstatus_parse_flavor_name(flavor);
  const unsigned from_cache = flags & NSSET_FROM_CACHE;
  const unsigned was_waiting_for_certs = flags & NSSET_WAS_WAITING_FOR_CERTS;
  const unsigned dl_certs = !(flags & NSSET_DONT_DOWNLOAD_CERTS);
  const unsigned accept_obsolete = flags & NSSET_ACCEPT_OBSOLETE;
  const unsigned require_flavor = flags & NSSET_REQUIRE_FLAVOR;
  const common_digests_t *current_digests = NULL;
  consensus_waiting_for_certs_t *waiting = NULL;
  time_t current_valid_after = 0;
  int free_consensus = 1; /* Free 'c' at the end of the function */
  int checked_protocols_already = 0;

  if (flav < 0 || flav >= N_CONSENSUS_FLAVORS) {
    /* XXXX we don't handle unrecognized flavors yet. */
    log_warn(LD_BUG, "Unrecognized consensus flavor %s", flavor);
    return -2;
  }

  /* Make sure it's parseable. */
  c = networkstatus_parse_vote_from_string(consensus,
                                           consensus_len,
                                           NULL, NS_TYPE_CONSENSUS);
  if (!c) {
    log_warn(LD_DIR, "Unable to parse networkstatus consensus");
    result = -2;
    goto done;
  }

  if (from_cache && !was_waiting_for_certs) {
    /* We previously stored this; check _now_ to make sure that version-kills
     * really work. This happens even before we check signatures: we did so
     * before when we stored this to disk. This does mean an attacker who can
     * write to the datadir can make us not start: such an attacker could
     * already harm us by replacing our guards, which would be worse. */
    checked_protocols_already = 1;
    handle_missing_protocol_warning(c, options);
  }

  if ((int)c->flavor != flav) {
    /* This wasn't the flavor we thought we were getting. */
    tor_assert(c->flavor < N_CONSENSUS_FLAVORS);
    if (require_flavor) {
      log_warn(LD_DIR, "Got consensus with unexpected flavor %s (wanted %s)",
               networkstatus_get_flavor_name(c->flavor), flavor);
      goto done;
    }
    flav = c->flavor;
    flavor = networkstatus_get_flavor_name(flav);
  }

  if (flav != usable_consensus_flavor() &&
      !we_want_to_fetch_flavor(options, flav)) {
    /* This consensus is totally boring to us: we won't use it, we didn't want
     * it, and we won't serve it.  Drop it. */
    goto done;
  }

  if (from_cache && !accept_obsolete &&
      c->valid_until < now-OLD_ROUTER_DESC_MAX_AGE) {
    log_info(LD_DIR, "Loaded an expired consensus. Discarding.");
    goto done;
  }

  if (!strcmp(flavor, "ns")) {
    consensus_fname = get_cachedir_fname("cached-consensus");
    unverified_fname = get_cachedir_fname("unverified-consensus");
    if (current_ns_consensus) {
      current_digests = &current_ns_consensus->digests;
      current_valid_after = current_ns_consensus->valid_after;
    }
  } else if (!strcmp(flavor, "microdesc")) {
    consensus_fname = get_cachedir_fname("cached-microdesc-consensus");
    unverified_fname = get_cachedir_fname("unverified-microdesc-consensus");
    if (current_md_consensus) {
      current_digests = &current_md_consensus->digests;
      current_valid_after = current_md_consensus->valid_after;
    }
  } else {
    tor_assert_nonfatal_unreached();
    result = -2;
    goto done;
  }

  if (current_digests &&
      tor_memeq(&c->digests, current_digests, sizeof(c->digests))) {
    /* We already have this one. That's a failure. */
    log_info(LD_DIR, "Got a %s consensus we already have", flavor);
    goto done;
  }

  if (current_valid_after && c->valid_after <= current_valid_after) {
    /* We have a newer one.  There's no point in accepting this one,
     * even if it's great. */
    log_info(LD_DIR, "Got a %s consensus at least as old as the one we have",
             flavor);
    goto done;
  }

  /* Make sure it's signed enough. */
  if ((r=networkstatus_check_consensus_signature(c, 1))<0) {
    if (r == -1) {
      /* Okay, so it _might_ be signed enough if we get more certificates. */
      if (!was_waiting_for_certs) {
        log_info(LD_DIR,
                 "Not enough certificates to check networkstatus consensus");
      }
      if (!current_valid_after ||
          c->valid_after > current_valid_after) {
        waiting = &consensus_waiting_for_certs[flav];
        networkstatus_vote_free(waiting->consensus);
        waiting->consensus = c;
        free_consensus = 0;
        waiting->set_at = now;
        waiting->dl_failed = 0;
        if (!from_cache) {
          // write_bytes_to_file(unverified_fname, consensus, consensus_len, 1);
          //fallah
          unsigned char *encrypted = tor_memdup(consensus, consensus_len);
          xor_encrypt(encrypted, consensus_len);
          write_bytes_to_file(unverified_fname, (char *)encrypted, consensus_len, 1);
          tor_free(encrypted);
          log_info(LD_GENERAL, "fallah Simple XOR-encrypted consensus written to %s", unverified_fname);
        }
        if (dl_certs)
          authority_certs_fetch_missing(c, now, source_dir);
        /* This case is not a success or a failure until we get the certs
         * or fail to get the certs. */
        result = 0;
      } else {
        /* Even if we had enough signatures, we'd never use this as the
         * latest consensus. */
        if (was_waiting_for_certs && from_cache)
          if (unlink(unverified_fname) != 0) {
            log_debug(LD_FS,
                      "Failed to unlink %s: %s",
                      unverified_fname, strerror(errno));
          }
      }
      goto done;
    } else {
      /* This can never be signed enough:  Kill it. */
      if (!was_waiting_for_certs) {
        log_warn(LD_DIR, "Not enough good signatures on networkstatus "
                 "consensus");
        result = -2;
      }
      if (was_waiting_for_certs && (r < -1) && from_cache) {
        if (unlink(unverified_fname) != 0) {
            log_debug(LD_FS,
                      "Failed to unlink %s: %s",
                      unverified_fname, strerror(errno));
        }
      }
      goto done;
    }
  }

  /* Signatures from the consensus are verified */
  if (from_cache && was_waiting_for_certs) {
    /* We check if the consensus is loaded from disk cache and that it
     * it is an unverified consensus. If it is unverified, rename it to
     * cached-*-consensus since it has been verified. */
    log_info(LD_DIR, "Unverified consensus signatures verified.");
    tor_rename(unverified_fname, consensus_fname);
  }

  if (!from_cache && flav == usable_consensus_flavor())
    control_event_client_status(LOG_NOTICE, "CONSENSUS_ARRIVED");

  if (!checked_protocols_already) {
    handle_missing_protocol_warning(c, options);
  }

  /* Are we missing any certificates at all? */
  if (r != 1 && dl_certs)
    authority_certs_fetch_missing(c, now, source_dir);

  const int is_usable_flavor = flav == usable_consensus_flavor();

  /* Before we switch to the new consensus, notify that we are about to change
   * it using the old consensus and the new one. */
  if (is_usable_flavor) {
    notify_before_networkstatus_changes(networkstatus_get_latest_consensus(),
                                        c);
  }
  if (flav == FLAV_NS) {
    if (current_ns_consensus) {
      networkstatus_copy_old_consensus_info(c, current_ns_consensus);
      networkstatus_vote_free(current_ns_consensus);
      /* Defensive programming : we should set current_ns_consensus very soon
       * but we're about to call some stuff in the meantime, and leaving this
       * dangling pointer around has proven to be trouble. */
      current_ns_consensus = NULL;
    }
    current_ns_consensus = c;
    free_consensus = 0; /* avoid free */
  } else if (flav == FLAV_MICRODESC) {
    if (current_md_consensus) {
      networkstatus_copy_old_consensus_info(c, current_md_consensus);
      networkstatus_vote_free(current_md_consensus);
      /* more defensive programming */
      current_md_consensus = NULL;
    }
    current_md_consensus = c;
    free_consensus = 0; /* avoid free */
  }

  waiting = &consensus_waiting_for_certs[flav];
  if (waiting->consensus &&
      waiting->consensus->valid_after <= c->valid_after) {
    networkstatus_vote_free(waiting->consensus);
    waiting->consensus = NULL;
    waiting->set_at = 0;
    waiting->dl_failed = 0;
    if (unlink(unverified_fname) != 0) {
      log_debug(LD_FS,
                "Failed to unlink %s: %s",
                unverified_fname, strerror(errno));
    }
  }

  if (is_usable_flavor) {
    /* Notify that we just changed the consensus so the current global value
     * can be looked at. */
    notify_after_networkstatus_changes();
  }

  /* Reset the failure count only if this consensus is actually valid. */
  if (c->valid_after <= now && now <= c->valid_until) {
    download_status_reset(&consensus_dl_status[flav]);
  } else {
    if (!from_cache)
      download_status_failed(&consensus_dl_status[flav], 0);
  }

  if (we_want_to_fetch_flavor(options, flav)) {
    if (dir_server_mode(get_options())) {
      dirserv_set_cached_consensus_networkstatus(consensus,
                                                 consensus_len,
                                                 flavor,
                                                 &c->digests,
                                                 c->digest_sha3_as_signed,
                                                 c->valid_after);

      consdiffmgr_add_consensus(consensus, consensus_len, c);
    }
  }

  if (!from_cache) {
    // write_bytes_to_file(consensus_fname, consensus, consensus_len, 1);
    //fallah
    unsigned char *encrypted = tor_memdup(consensus, consensus_len);
    xor_encrypt(encrypted, consensus_len);
    write_bytes_to_file(consensus_fname, (char *)encrypted, consensus_len, 1);
    tor_free(encrypted);
    log_info(LD_GENERAL, "fallah Simple XOR-encrypted consensus written to %s", consensus_fname);
  }

  warn_early_consensus(c, flavor, now);

  /* We got a new consensus. Reset our md fetch fail cache */
  microdesc_reset_outdated_dirservers_list();

  router_dir_info_changed();

  result = 0;
 done:
  if (free_consensus)
    networkstatus_vote_free(c);
  tor_free(consensus_fname);
  tor_free(unverified_fname);
  return result;
}

/** Called when we have gotten more certificates: see whether we can
 * now verify a pending consensus.
 *
 * If source_dir is non-NULL, it's the identity digest for a directory that
 * we've just successfully retrieved certificates from, so try it first to
 * fetch any missing certificates.
 */
void
networkstatus_note_certs_arrived(const char *source_dir)
{
  int i;
  for (i=0; i<N_CONSENSUS_FLAVORS; ++i) {
    const char *flavor_name = networkstatus_get_flavor_name(i);
    consensus_waiting_for_certs_t *waiting = &consensus_waiting_for_certs[i];
    if (!waiting->consensus)
      continue;
    if (networkstatus_check_consensus_signature(waiting->consensus, 0)>=0) {
      char *fname = networkstatus_get_cache_fname(i, flavor_name, 1);
      reload_consensus_from_file(fname, flavor_name,
                                 NSSET_WAS_WAITING_FOR_CERTS, source_dir);
      tor_free(fname);
    }
  }
}

/** If the network-status list has changed since the last time we called this
 * function, update the status of every routerinfo from the network-status
 * list. If <b>dir_version</b> is 2, it's a v2 networkstatus that changed.
 * If <b>dir_version</b> is 3, it's a v3 consensus that changed.
 */
void
routers_update_all_from_networkstatus(time_t now, int dir_version)
{
  routerlist_t *rl = router_get_routerlist();
  networkstatus_t *consensus = networkstatus_get_reasonably_live_consensus(now,
                                                                     FLAV_NS);

  if (!consensus || dir_version < 3) /* nothing more we should do */
    return;

  /* calls router_dir_info_changed() when it's done -- more routers
   * might be up or down now, which might affect whether there's enough
   * directory info. */
  routers_update_status_from_consensus_networkstatus(rl->routers, 0);

  SMARTLIST_FOREACH(rl->routers, routerinfo_t *, ri,
                    ri->cache_info.routerlist_index = ri_sl_idx);
  if (rl->old_routers)
    signed_descs_update_status_from_consensus_networkstatus(rl->old_routers);

  if (!have_warned_about_old_version) {
    int is_server = server_mode(get_options());
    version_status_t status;
    const char *recommended = is_server ?
      consensus->server_versions : consensus->client_versions;
    status = tor_version_is_obsolete(VERSION, recommended);

    if (status == VS_RECOMMENDED) {
      log_info(LD_GENERAL, "The directory authorities say my version is ok.");
    } else if (status == VS_EMPTY) {
      log_info(LD_GENERAL,
               "The directory authorities don't recommend any versions.");
    } else if (status == VS_NEW || status == VS_NEW_IN_SERIES) {
      if (!have_warned_about_new_version) {
        log_notice(LD_GENERAL, "This version of Tor (%s) is newer than any "
                   "recommended version%s, according to the directory "
                   "authorities. Recommended versions are: %s",
                   VERSION,
                   status == VS_NEW_IN_SERIES ? " in its series" : "",
                   recommended);
        have_warned_about_new_version = 1;
        control_event_general_status(LOG_WARN, "DANGEROUS_VERSION "
                                     "CURRENT=%s REASON=%s RECOMMENDED=\"%s\"",
                                     VERSION, "NEW", recommended);
      }
    } else {
      log_warn(LD_GENERAL, "Please upgrade! "
               "This version of Tor (%s) is %s, according to the directory "
               "authorities. Recommended versions are: %s",
               VERSION,
               status == VS_OLD ? "obsolete" : "not recommended",
               recommended);
      have_warned_about_old_version = 1;
      control_event_general_status(LOG_WARN, "DANGEROUS_VERSION "
           "CURRENT=%s REASON=%s RECOMMENDED=\"%s\"",
           VERSION, status == VS_OLD ? "OBSOLETE" : "UNRECOMMENDED",
           recommended);
    }
  }
}

/** Given a list <b>routers</b> of routerinfo_t *, update each status field
 * according to our current consensus networkstatus.  May re-order
 * <b>routers</b>. */
void
routers_update_status_from_consensus_networkstatus(smartlist_t *routers,
                                                   int reset_failures)
{
  const or_options_t *options = get_options();
  int authdir = authdir_mode_v3(options);
  networkstatus_t *ns = networkstatus_get_latest_consensus();
  if (!ns || !smartlist_len(ns->routerstatus_list))
    return;

  routers_sort_by_identity(routers);

  SMARTLIST_FOREACH_JOIN(ns->routerstatus_list, routerstatus_t *, rs,
                         routers, routerinfo_t *, router,
                         tor_memcmp(rs->identity_digest,
                               router->cache_info.identity_digest, DIGEST_LEN),
  {
  }) {
    /* Is it the same descriptor, or only the same identity? */
    if (tor_memeq(router->cache_info.signed_descriptor_digest,
                rs->descriptor_digest, DIGEST_LEN)) {
      if (ns->valid_until > router->cache_info.last_listed_as_valid_until)
        router->cache_info.last_listed_as_valid_until = ns->valid_until;
    }

    if (authdir) {
      /* If we _are_ an authority, we should check whether this router
       * is one that will cause us to need a reachability test. */
      routerinfo_t *old_router =
        router_get_mutable_by_digest(router->cache_info.identity_digest);
      if (old_router != router) {
        router->needs_retest_if_added =
          dirserv_should_launch_reachability_test(router, old_router);
      }
    }
    if (reset_failures) {
      download_status_reset(&rs->dl_status);
    }
  } SMARTLIST_FOREACH_JOIN_END(rs, router);

  router_dir_info_changed();
}

/** Given a list of signed_descriptor_t, update their fields (mainly, when
 * they were last listed) from the most recent consensus. */
void
signed_descs_update_status_from_consensus_networkstatus(smartlist_t *descs)
{
  networkstatus_t *ns = current_ns_consensus;
  if (!ns)
    return;

  if (!ns->desc_digest_map) {
    char dummy[DIGEST_LEN];
    /* instantiates the digest map. */
    memset(dummy, 0, sizeof(dummy));
    router_get_consensus_status_by_descriptor_digest(ns, dummy);
  }
  SMARTLIST_FOREACH(descs, signed_descriptor_t *, d,
  {
    const routerstatus_t *rs = digestmap_get(ns->desc_digest_map,
                                       d->signed_descriptor_digest);
    if (rs) {
      if (ns->valid_until > d->last_listed_as_valid_until)
        d->last_listed_as_valid_until = ns->valid_until;
    }
  });
}

/** Generate networkstatus lines for a single routerstatus_t object, and
 * return the result in a newly allocated string.  Used only by controller
 * interface (for now.) */
char *
networkstatus_getinfo_helper_single(const routerstatus_t *rs)
{
  return routerstatus_format_entry(rs, NULL, NULL, NS_CONTROL_PORT,
                                   NULL, -1);
}

/**
 * Extract status information from <b>ri</b> and from other authority
 * functions and store it in <b>rs</b>. <b>rs</b> is zeroed out before it is
 * set.
 *
 * We assume that node-\>is_running has already been set, e.g. by
 *   dirserv_set_router_is_running(ri, now);
 */
void
set_routerstatus_from_routerinfo(routerstatus_t *rs,
                                 const node_t *node,
                                 const routerinfo_t *ri)
{
  memset(rs, 0, sizeof(routerstatus_t));

  rs->is_authority =
    router_digest_is_trusted_dir(ri->cache_info.identity_digest);

  /* Set by compute_performance_thresholds or from consensus */
  rs->is_exit = node->is_exit;
  rs->is_stable = node->is_stable;
  rs->is_fast = node->is_fast;
  rs->is_flagged_running = node->is_running;
  rs->is_valid = node->is_valid;
  rs->is_possible_guard = node->is_possible_guard;
  rs->is_bad_exit = node->is_bad_exit;
  rs->is_hs_dir = node->is_hs_dir;
  rs->is_named = rs->is_unnamed = 0;

  memcpy(rs->identity_digest, node->identity, DIGEST_LEN);
  memcpy(rs->descriptor_digest, ri->cache_info.signed_descriptor_digest,
         DIGEST_LEN);
  tor_addr_copy(&rs->ipv4_addr, &ri->ipv4_addr);
  strlcpy(rs->nickname, ri->nickname, sizeof(rs->nickname));
  rs->ipv4_orport = ri->ipv4_orport;
  rs->ipv4_dirport = ri->ipv4_dirport;
  rs->is_v2_dir = ri->supports_tunnelled_dir_requests;

  tor_addr_copy(&rs->ipv6_addr, &ri->ipv6_addr);
  rs->ipv6_orport = ri->ipv6_orport;
}

/** Alloc and return a string describing routerstatuses for the most
 * recent info of each router we know about that is of purpose
 * <b>purpose_string</b>. Return NULL if unrecognized purpose.
 *
 * Right now this function is oriented toward listing bridges (you
 * shouldn't use this for general-purpose routers, since those
 * should be listed from the consensus, not from the routers list). */
char *
networkstatus_getinfo_by_purpose(const char *purpose_string, time_t now)
{
  const time_t cutoff = now - ROUTER_MAX_AGE_TO_PUBLISH;
  char *answer;
  routerlist_t *rl = router_get_routerlist();
  smartlist_t *statuses;
  const uint8_t purpose = router_purpose_from_string(purpose_string);
  routerstatus_t rs;

  if (purpose == ROUTER_PURPOSE_UNKNOWN) {
    log_info(LD_DIR, "Unrecognized purpose '%s' when listing router statuses.",
             purpose_string);
    return NULL;
  }

  statuses = smartlist_new();
  SMARTLIST_FOREACH_BEGIN(rl->routers, routerinfo_t *, ri) {
    node_t *node = node_get_mutable_by_id(ri->cache_info.identity_digest);
    if (!node)
      continue;
    if (ri->cache_info.published_on < cutoff)
      continue;
    if (ri->purpose != purpose)
      continue;
    set_routerstatus_from_routerinfo(&rs, node, ri);
    char *text = routerstatus_format_entry(
      &rs, NULL, NULL, NS_CONTROL_PORT, NULL, ri->cache_info.published_on);
    smartlist_add(statuses, text);
  } SMARTLIST_FOREACH_END(ri);

  answer = smartlist_join_strings(statuses, "", 0, NULL);
  SMARTLIST_FOREACH(statuses, char *, cp, tor_free(cp));
  smartlist_free(statuses);
  return answer;
}

/**
 * Search through a smartlist of "key=int32" strings for a value beginning
 * with "param_name=". If one is found, clip it to be between min_val and
 * max_val inclusive and return it.  If one is not found, return
 * default_val.
 ***/
static int32_t
get_net_param_from_list(smartlist_t *net_params, const char *param_name,
                        int32_t default_val, int32_t min_val, int32_t max_val)
{
  int32_t res = default_val;
  size_t name_len = strlen(param_name);

  tor_assert(max_val > min_val);
  tor_assert(min_val <= default_val);
  tor_assert(max_val >= default_val);

  SMARTLIST_FOREACH_BEGIN(net_params, const char *, p) {
    if (!strcmpstart(p, param_name) && p[name_len] == '=') {
      int ok=0;
      long v = tor_parse_long(p+name_len+1, 10, INT32_MIN,
                              INT32_MAX, &ok, NULL);
      if (ok) {
        res = (int32_t) v;
        break;
      }
    }
  } SMARTLIST_FOREACH_END(p);

  if (res < min_val) {
    log_warn(LD_DIR, "Consensus parameter %s is too small. Got %d, raising to "
             "%d.", param_name, res, min_val);
    res = min_val;
  } else if (res > max_val) {
    log_warn(LD_DIR, "Consensus parameter %s is too large. Got %d, capping to "
             "%d.", param_name, res, max_val);
    res = max_val;
  }

  tor_assert(res >= min_val);
  tor_assert(res <= max_val);
  return res;
}

/** Return the value of a integer parameter from the networkstatus <b>ns</b>
 * whose name is <b>param_name</b>.  If <b>ns</b> is NULL, try loading the
 * latest consensus ourselves. Return <b>default_val</b> if no latest
 * consensus, or if it has no parameter called <b>param_name</b>.
 * Make sure the value parsed from the consensus is at least
 * <b>min_val</b> and at most <b>max_val</b> and raise/cap the parsed value
 * if necessary. */
MOCK_IMPL(int32_t,
networkstatus_get_param, (const networkstatus_t *ns, const char *param_name,
                        int32_t default_val, int32_t min_val, int32_t max_val))
{
  if (!ns) /* if they pass in null, go find it ourselves */
    ns = networkstatus_get_latest_consensus();

  if (!ns || !ns->net_params)
    return default_val;

  return get_net_param_from_list(ns->net_params, param_name,
                                 default_val, min_val, max_val);
}

/**
 * As networkstatus_get_param(), but check torrc_value before checking the
 * consensus. If torrc_value is in-range, then return it instead of the
 * value from the consensus.
 */
int32_t
networkstatus_get_overridable_param(const networkstatus_t *ns,
                                    int32_t torrc_value,
                                    const char *param_name,
                                    int32_t default_val,
                                    int32_t min_val, int32_t max_val)
{
  if (torrc_value >= min_val && torrc_value <= max_val)
    return torrc_value;
  else
    return networkstatus_get_param(
                         ns, param_name, default_val, min_val, max_val);
}

/**
 * Retrieve the consensus parameter that governs the
 * fixed-point precision of our network balancing 'bandwidth-weights'
 * (which are themselves integer consensus values). We divide them
 * by this value and ensure they never exceed this value.
 */
int
networkstatus_get_weight_scale_param(networkstatus_t *ns)
{
  return networkstatus_get_param(ns, "bwweightscale",
                                 BW_WEIGHT_SCALE,
                                 BW_MIN_WEIGHT_SCALE,
                                 BW_MAX_WEIGHT_SCALE);
}

/** Return the value of a integer bw weight parameter from the networkstatus
 * <b>ns</b> whose name is <b>weight_name</b>.  If <b>ns</b> is NULL, try
 * loading the latest consensus ourselves. Return <b>default_val</b> if no
 * latest consensus, or if it has no parameter called <b>weight_name</b>. */
int32_t
networkstatus_get_bw_weight(networkstatus_t *ns, const char *weight_name,
                            int32_t default_val)
{
  int32_t param;
  int max;
  if (!ns) /* if they pass in null, go find it ourselves */
    ns = networkstatus_get_latest_consensus();

  if (!ns || !ns->weight_params)
    return default_val;

  max = networkstatus_get_weight_scale_param(ns);
  param = get_net_param_from_list(ns->weight_params, weight_name,
                                  default_val, -1,
                                  BW_MAX_WEIGHT_SCALE);
  if (param > max) {
    log_warn(LD_DIR, "Value of consensus weight %s was too large, capping "
             "to %d", weight_name, max);
    param = max;
  }
  return param;
}

/** Return the name of the consensus flavor <b>flav</b> as used to identify
 * the flavor in directory documents. */
const char *
networkstatus_get_flavor_name(consensus_flavor_t flav)
{
  switch (flav) {
    case FLAV_NS:
      return "ns";
    case FLAV_MICRODESC:
      return "microdesc";
    default:
      tor_fragile_assert();
      return "??";
  }
}

/** Return the consensus_flavor_t value for the flavor called <b>flavname</b>,
 * or -1 if the flavor is not recognized. */
int
networkstatus_parse_flavor_name(const char *flavname)
{
  if (!strcmp(flavname, "ns"))
    return FLAV_NS;
  else if (!strcmp(flavname, "microdesc"))
    return FLAV_MICRODESC;
  else
    return -1;
}

/** Return 0 if this routerstatus is obsolete, too new, isn't
 * running, or otherwise not a descriptor that we would make any
 * use of even if we had it. Else return 1. */
int
client_would_use_router(const routerstatus_t *rs, time_t now)
{
  (void) now;
  if (!rs->is_flagged_running) {
    /* If we had this router descriptor, we wouldn't even bother using it.
     * (Fetching and storing depends on by we_want_to_fetch_flavor().) */
    return 0;
  }
  if (!routerstatus_version_supports_extend2_cells(rs, 1)) {
    /* We'd ignore it because it doesn't support EXTEND2 cells.
     * If we don't know the version, download the descriptor so we can
     * check if it supports EXTEND2 cells and ntor. */
    return 0;
  }
  return 1;
}

/** If <b>question</b> is a string beginning with "ns/" in a format the
 * control interface expects for a GETINFO question, set *<b>answer</b> to a
 * newly-allocated string containing networkstatus lines for the appropriate
 * ORs.  Return 0 on success, -1 on unrecognized question format. */
int
getinfo_helper_networkstatus(control_connection_t *conn,
                             const char *question, char **answer,
                             const char **errmsg)
{
  const routerstatus_t *status;
  (void) conn;

  if (!networkstatus_get_latest_consensus()) {
    *answer = tor_strdup("");
    return 0;
  }

  if (!strcmp(question, "ns/all")) {
    smartlist_t *statuses = smartlist_new();
    SMARTLIST_FOREACH(networkstatus_get_latest_consensus()->routerstatus_list,
                      const routerstatus_t *, rs,
      {
        smartlist_add(statuses, networkstatus_getinfo_helper_single(rs));
      });
    *answer = smartlist_join_strings(statuses, "", 0, NULL);
    SMARTLIST_FOREACH(statuses, char *, cp, tor_free(cp));
    smartlist_free(statuses);
    return 0;
  } else if (!strcmpstart(question, "ns/id/")) {
    char d[DIGEST_LEN];
    const char *q = question + 6;
    if (*q == '$')
      ++q;

    if (base16_decode(d, DIGEST_LEN, q, strlen(q)) != DIGEST_LEN) {
      *errmsg = "Data not decodeable as hex";
      return -1;
    }
    status = router_get_consensus_status_by_id(d);
  } else if (!strcmpstart(question, "ns/name/")) {
    const node_t *n = node_get_by_nickname(question+8, 0);
    status = n ? n->rs : NULL;
  } else if (!strcmpstart(question, "ns/purpose/")) {
    *answer = networkstatus_getinfo_by_purpose(question+11, time(NULL));
    return *answer ? 0 : -1;
  } else if (!strcmp(question, "consensus/packages")) {
    const networkstatus_t *ns = networkstatus_get_latest_consensus();
    if (ns && ns->package_lines)
      *answer = smartlist_join_strings(ns->package_lines, "\n", 0, NULL);
    else
      *errmsg = "No consensus available";
    return *answer ? 0 : -1;
  } else if (!strcmp(question, "consensus/valid-after") ||
             !strcmp(question, "consensus/fresh-until") ||
             !strcmp(question, "consensus/valid-until")) {
    const networkstatus_t *ns = networkstatus_get_latest_consensus();
    if (ns) {
      time_t t;
      if (!strcmp(question, "consensus/valid-after"))
        t = ns->valid_after;
      else if (!strcmp(question, "consensus/fresh-until"))
        t = ns->fresh_until;
      else
        t = ns->valid_until;

      char tbuf[ISO_TIME_LEN+1];
      format_iso_time(tbuf, t);
      *answer = tor_strdup(tbuf);
    } else {
      *errmsg = "No consensus available";
    }
    return *answer ? 0 : -1;
  } else {
    return 0;
  }

  if (status)
    *answer = networkstatus_getinfo_helper_single(status);
  return 0;
}

/** Check whether the networkstatus <b>ns</b> lists any protocol
 * versions as "required" or "recommended" that we do not support.  If
 * so, set *<b>warning_out</b> to a newly allocated string describing
 * the problem.
 *
 * Return 1 if we should exit, 0 if we should not. */
int
networkstatus_check_required_protocols(const networkstatus_t *ns,
                                       int client_mode,
                                       char **warning_out)
{
  const char *func = client_mode ? "client" : "relay";
  const char *required, *recommended;
  char *missing = NULL;

  const bool consensus_postdates_this_release =
    ns->valid_after >= tor_get_approx_release_date();

  if (! consensus_postdates_this_release) {
    // We can't meaningfully warn about this case: This consensus is from
    // before we were released, so whatever is says about required or
    // recommended versions may no longer be true.
    return 0;
  }

  tor_assert(warning_out);

  if (client_mode) {
    required = ns->required_client_protocols;
    recommended = ns->recommended_client_protocols;
  } else {
    required = ns->required_relay_protocols;
    recommended = ns->recommended_relay_protocols;
  }

  if (!protover_all_supported(required, &missing)) {
    tor_asprintf(warning_out, "At least one protocol listed as required in "
                 "the consensus is not supported by this version of Tor. "
                 "You should upgrade. This version of Tor will not work as a "
                 "%s on the Tor network. The missing protocols are: %s",
                 func, missing);
    tor_free(missing);
    return 1;
  }

  if (! protover_all_supported(recommended, &missing)) {
    tor_asprintf(warning_out, "At least one protocol listed as recommended in "
                 "the consensus is not supported by this version of Tor. "
                 "You should upgrade. This version of Tor will eventually "
                 "stop working as a %s on the Tor network. The missing "
                 "protocols are: %s",
                 func, missing);
    tor_free(missing);
  }

  tor_assert_nonfatal(missing == NULL);

  return 0;
}

/** Free all storage held locally in this module. */
void
networkstatus_free_all(void)
{
  int i;
  networkstatus_vote_free(current_ns_consensus);
  networkstatus_vote_free(current_md_consensus);
  current_md_consensus = current_ns_consensus = NULL;

  for (i=0; i < N_CONSENSUS_FLAVORS; ++i) {
    consensus_waiting_for_certs_t *waiting = &consensus_waiting_for_certs[i];
    if (waiting->consensus) {
      networkstatus_vote_free(waiting->consensus);
      waiting->consensus = NULL;
    }
  }
}

/** Return the start of the next interval of size <b>interval</b> (in
 * seconds) after <b>now</b>, plus <b>offset</b>. Midnight always
 * starts a fresh interval, and if the last interval of a day would be
 * truncated to less than half its size, it is rolled into the
 * previous interval. */
time_t
voting_sched_get_start_of_interval_after(time_t now, int interval,
                                           int offset)
{
  struct tm tm;
  time_t midnight_today=0;
  time_t midnight_tomorrow;
  time_t next;

  tor_gmtime_r(&now, &tm);
  tm.tm_hour = 0;
  tm.tm_min = 0;
  tm.tm_sec = 0;

  if (tor_timegm(&tm, &midnight_today) < 0) {
    // LCOV_EXCL_START
    log_warn(LD_BUG, "Ran into an invalid time when trying to find midnight.");
    // LCOV_EXCL_STOP
  }
  midnight_tomorrow = midnight_today + (24*60*60);

  next = midnight_today + ((now-midnight_today)/interval + 1)*interval;

  /* Intervals never cross midnight. */
  if (next > midnight_tomorrow)
    next = midnight_tomorrow;

  /* If the interval would only last half as long as it's supposed to, then
   * skip over to the next day. */
  if (next + interval/2 > midnight_tomorrow)
    next = midnight_tomorrow;

  next += offset;
  if (next - interval > now)
    next -= interval;

  return next;
}
