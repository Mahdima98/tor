/* Copyright (c) 2001, Matej Pfajfar.
 * Copyright (c) 2001-2004, Roger Dingledine.
 * Copyright (c) 2004-2006, Roger Dingledine, Nick Mathewson.
 * Copyright (c) 2007-2021, The Tor Project, Inc. */
/* See LICENSE for licensing information */

/**
 * \file crypto_openssl_mgt.c
 *
 * \brief Block of functions related to operations from OpenSSL.
 **/

#include "lib/crypt_ops/compat_openssl.h"
#include "lib/crypt_ops/crypto_openssl_mgt.h"
#include "lib/crypt_ops/crypto_rand.h"
#include "lib/crypt_ops/aes.h"
#include "lib/string/util_string.h"
#include "lib/lock/compat_mutex.h"
#include "lib/log/log.h"
#include "lib/log/util_bug.h"
#include "lib/testsupport/testsupport.h"
#include "lib/thread/threads.h"

DISABLE_GCC_WARNING("-Wredundant-decls")

#include <openssl/err.h>
#include <openssl/rsa.h>
#include <openssl/pem.h>
#include <openssl/evp.h>
#include <openssl/engine.h>
#include <openssl/rand.h>
#include <openssl/bn.h>
#include <openssl/dh.h>
#include <openssl/conf.h>
#include <openssl/hmac.h>
#include <openssl/crypto.h>
#include <openssl/ssl.h>

ENABLE_GCC_WARNING("-Wredundant-decls")

#include <string.h>

#ifdef OPENSSL_NO_ENGINE
/* Android's OpenSSL seems to have removed all of its Engine support. */
#define DISABLE_ENGINES
#endif

/** Declare STATIC functions */
STATIC char * parse_openssl_version_str(const char *raw_version);

/** Log all pending crypto errors at level <b>severity</b>.  Use
 * <b>doing</b> to describe our current activities.
 */
void
crypto_openssl_log_errors(int severity, const char *doing)
{
  unsigned long err;
  const char *msg, *lib, *func;
  while ((err = ERR_get_error()) != 0) {
    msg = (const char*)ERR_reason_error_string(err);
    lib = (const char*)ERR_lib_error_string(err);
    func = (const char*)ERR_func_error_string(err);
    if (!msg) msg = "(null)";
    if (!lib) lib = "(null)";
    if (!func) func = "(null)";
    if (BUG(!doing)) doing = "(null)";
    tor_log(severity, LD_CRYPTO, "crypto error while %s: %s (in %s:%s)",
              doing, msg, lib, func);
  }
}

/* Returns a trimmed and human-readable version of an openssl version string
* <b>raw_version</b>. They are usually in the form of 'OpenSSL 1.0.0b 10
* May 2012' and this will parse them into a form similar to '1.0.0b' */
STATIC char *
parse_openssl_version_str(const char *raw_version)
{
  const char *end_of_version = NULL;
  /* The output should be something like "OpenSSL 1.0.0b 10 May 2012. Let's
     trim that down. */
  if (!strcmpstart(raw_version, "OpenSSL ")) {
    raw_version += strlen("OpenSSL ");
    end_of_version = strchr(raw_version, ' ');
  }

  if (end_of_version)
    return tor_strndup(raw_version,
                      end_of_version-raw_version);
  else
    return tor_strdup(raw_version);
}

static char *crypto_openssl_version_str = NULL;
/* Return a human-readable version of the run-time openssl version number. */
const char *
crypto_openssl_get_version_str(void)
{
  const int query = OPENSSL_VERSION;

  if (crypto_openssl_version_str == NULL) {
    const char *raw_version = OpenSSL_version(query);
    crypto_openssl_version_str = parse_openssl_version_str(raw_version);
  }
  return crypto_openssl_version_str;
}

static char *crypto_openssl_header_version_str = NULL;
/* Return a human-readable version of the compile-time openssl version
* number. */
const char *
crypto_openssl_get_header_version_str(void)
{
  if (crypto_openssl_header_version_str == NULL) {
    crypto_openssl_header_version_str =
                        parse_openssl_version_str(OPENSSL_VERSION_TEXT);
  }
  return crypto_openssl_header_version_str;
}

#ifndef COCCI
#ifndef OPENSSL_THREADS
#error "OpenSSL has been built without thread support. Tor requires an \
 OpenSSL library with thread support enabled."
#endif
#endif /* !defined(COCCI) */

/** Helper: Construct mutexes, and set callbacks to help OpenSSL handle being
 * multithreaded. Returns 0. */
static int
setup_openssl_threading(void)
{
  return 0;
}

/** free OpenSSL variables */
static void
crypto_openssl_free_all(void)
{
  tor_free(crypto_openssl_version_str);
  tor_free(crypto_openssl_header_version_str);
}

/** Perform early (pre-configuration) initialization tasks for OpenSSL. */
void
crypto_openssl_early_init(void)
{
    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS |
                     OPENSSL_INIT_LOAD_CRYPTO_STRINGS |
                     OPENSSL_INIT_ADD_ALL_CIPHERS |
                     OPENSSL_INIT_ADD_ALL_DIGESTS, NULL);

    setup_openssl_threading();

    unsigned long version_num = tor_OpenSSL_version_num();
    const char *version_str = crypto_openssl_get_version_str();
    if (version_num == OPENSSL_VERSION_NUMBER &&
        !strcmp(version_str, OPENSSL_VERSION_TEXT)) {
      log_info(LD_CRYPTO, "OpenSSL version matches version from headers "
                 "(%lx: %s).", version_num, version_str);
    } else if ((version_num & 0xffff0000) ==
               (OPENSSL_VERSION_NUMBER & 0xffff0000)) {
      log_notice(LD_CRYPTO,
               "We compiled with OpenSSL %lx: %s and we "
               "are running with OpenSSL %lx: %s. "
               "These two versions should be binary compatible.",
               (unsigned long)OPENSSL_VERSION_NUMBER, OPENSSL_VERSION_TEXT,
               version_num, version_str);
    } else {
      log_warn(LD_CRYPTO, "OpenSSL version from headers does not match the "
               "version we're running with. If you get weird crashes, that "
               "might be why. (Compiled with %lx: %s; running with %lx: %s).",
               (unsigned long)OPENSSL_VERSION_NUMBER, OPENSSL_VERSION_TEXT,
               version_num, version_str);
    }

    crypto_force_rand_ssleay();
}

#ifndef DISABLE_ENGINES
/** Try to load an engine in a shared library via fully qualified path.
 */
static ENGINE *
try_load_engine(const char *path, const char *engine)
{
  ENGINE *e = ENGINE_by_id("dynamic");
  if (e) {
    if (!ENGINE_ctrl_cmd_string(e, "ID", engine, 0) ||
        !ENGINE_ctrl_cmd_string(e, "DIR_LOAD", "2", 0) ||
        !ENGINE_ctrl_cmd_string(e, "DIR_ADD", path, 0) ||
        !ENGINE_ctrl_cmd_string(e, "LOAD", NULL, 0)) {
      ENGINE_free(e);
      e = NULL;
    }
  }
  return e;
}
#endif /* !defined(DISABLE_ENGINES) */

#ifndef DISABLE_ENGINES
/** Log any OpenSSL engines we're using at NOTICE. */
static void
log_engine(const char *fn, ENGINE *e)
{
  if (e) {
    const char *name, *id;
    name = ENGINE_get_name(e);
    id = ENGINE_get_id(e);
    log_notice(LD_CRYPTO, "Default OpenSSL engine for %s is %s [%s]",
               fn, name?name:"?", id?id:"?");
  } else {
    log_info(LD_CRYPTO, "Using default implementation for %s", fn);
  }
}
#endif /* !defined(DISABLE_ENGINES) */

/** Initialize engines for openssl (if enabled).  Load all the built-in
 * engines, along with the one called <b>accelName</b> (which may be NULL).
 * If <b>accelName</b> is prefixed with "!", then it is required: return -1
 * if it can't be loaded.  Otherwise return 0.
 *
 * If <b>accelDir</b> is not NULL, it is the path from which the engine should
 * be loaded. */
static int
crypto_openssl_init_engines(const char *accelName,
                            const char *accelDir)
{
#ifdef DISABLE_ENGINES
  (void)accelName;
  (void)accelDir;
  log_warn(LD_CRYPTO, "No OpenSSL hardware acceleration support enabled.");
  if (accelName && accelName[0] == '!') {
    log_warn(LD_CRYPTO, "Unable to load required dynamic OpenSSL engine "
             "\"%s\".", accelName+1);
    return -1;
  }
  return 0;
#else /* !defined(DISABLE_ENGINES) */
  ENGINE *e = NULL;

  log_info(LD_CRYPTO, "Initializing OpenSSL engine support.");
  ENGINE_load_builtin_engines();
  ENGINE_register_all_complete();

  if (accelName) {
    const bool required = accelName[0] == '!';
    if (required)
      ++accelName;
    if (accelDir) {
      log_info(LD_CRYPTO, "Trying to load dynamic OpenSSL engine \"%s\""
               " via path \"%s\".", accelName, accelDir);
      e = try_load_engine(accelName, accelDir);
    } else {
      log_info(LD_CRYPTO, "Initializing dynamic OpenSSL engine \"%s\""
               " acceleration support.", accelName);
      e = ENGINE_by_id(accelName);
    }
    if (!e) {
      log_warn(LD_CRYPTO, "Unable to load %sdynamic OpenSSL engine \"%s\".",
               required?"required ":"",
               accelName);
      if (required)
        return -1;
    } else {
      log_info(LD_CRYPTO, "Loaded dynamic OpenSSL engine \"%s\".",
               accelName);
    }
  }
  if (e) {
    log_info(LD_CRYPTO, "Loaded OpenSSL hardware acceleration engine,"
             " setting default ciphers.");
    ENGINE_set_default(e, ENGINE_METHOD_ALL);
  }
  /* Log, if available, the intersection of the set of algorithms
     used by Tor and the set of algorithms available in the engine */
  log_engine("RSA", ENGINE_get_default_RSA());
  log_engine("DH", ENGINE_get_default_DH());
  log_engine("EC", ENGINE_get_default_EC());
  log_engine("RAND", ENGINE_get_default_RAND());
  log_engine("RAND (which we will not use)", ENGINE_get_default_RAND());
  log_engine("SHA1", ENGINE_get_digest_engine(NID_sha1));
  log_engine("3DES-CBC", ENGINE_get_cipher_engine(NID_des_ede3_cbc));
  log_engine("AES-128-ECB", ENGINE_get_cipher_engine(NID_aes_128_ecb));
  log_engine("AES-128-CBC", ENGINE_get_cipher_engine(NID_aes_128_cbc));
#ifdef NID_aes_128_ctr
  log_engine("AES-128-CTR", ENGINE_get_cipher_engine(NID_aes_128_ctr));
#endif
#ifdef NID_aes_128_gcm
  log_engine("AES-128-GCM", ENGINE_get_cipher_engine(NID_aes_128_gcm));
#endif
  log_engine("AES-256-CBC", ENGINE_get_cipher_engine(NID_aes_256_cbc));
#ifdef NID_aes_256_gcm
  log_engine("AES-256-GCM", ENGINE_get_cipher_engine(NID_aes_256_gcm));
#endif
  return 0;

#endif /* defined(DISABLE_ENGINES) */
}

/** Perform late (post-init) initialization tasks for OpenSSL */
int
crypto_openssl_late_init(int useAccel, const char *accelName,
                         const char *accelDir)
{
  if (tor_OpenSSL_version_num() < OPENSSL_V_SERIES(3,0,0)) {
    log_warn(LD_CRYPTO, "Running with OpenSSL version \"%s\", "
             "which is no longer maintained by the OpenSSL project. "
             "We recommend that you upgrade to OpenSSL 3.0 or later. "
             "OpenSSL >=3.5 would be ideal.",
             OPENSSL_VERSION_TEXT);
  }

  if (useAccel > 0) {
    if (crypto_openssl_init_engines(accelName, accelDir) < 0)
      return -1;
  } else {
    log_info(LD_CRYPTO, "NOT using OpenSSL engine support.");
  }

  if (crypto_force_rand_ssleay()) {
    if (crypto_seed_rng() < 0)
      return -1;
  }

  evaluate_evp_for_aes(-1);
  evaluate_ctr_for_aes();

  return 0;
}

/** Free crypto resources held by this thread. */
void
crypto_openssl_thread_cleanup(void)
{
}

/** Clean up global resources held by openssl. */
void
crypto_openssl_global_cleanup(void)
{
  CONF_modules_unload(1);

  crypto_openssl_free_all();
}
