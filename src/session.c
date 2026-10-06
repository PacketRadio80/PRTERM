/*
 * PRTERM - CB & Amateur Radio Terminal
 * session.c - Admin login, sessions and CSRF.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "prterm_compat.h"

#include "session.h"
#include "ini.h"
#include "sha256.h"
#include "state.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PR_HASH_ITER 10000
#define PR_COOKIE_NAME "PRTERM_SID"

/* ======================================================================= */
/* Password                                                                */
/* ======================================================================= */

/* sha256 over (salt + password), stretched PR_HASH_ITER times. */
static void stretch(const char *salt, const char *pass,
                    unsigned char out[PR_SHA256_DIGEST_LEN])
{
    pr_sha256_ctx ctx;
    pr_sha256_init(&ctx);
    pr_sha256_update(&ctx, salt, strlen(salt));
    pr_sha256_update(&ctx, pass, strlen(pass));
    pr_sha256_final(&ctx, out);

    for (int i = 1; i < PR_HASH_ITER; i++) {
        pr_sha256_init(&ctx);
        pr_sha256_update(&ctx, out, PR_SHA256_DIGEST_LEN);
        pr_sha256_update(&ctx, salt, strlen(salt));
        pr_sha256_final(&ctx, out);
    }
}

int pr_hash_password(const char *pass, char *out, size_t outlen)
{
    if (pass == NULL || out == NULL || outlen < 128)
        return -1;

    char salt[33];
    pr_random_hex(salt, sizeof salt);

    unsigned char digest[PR_SHA256_DIGEST_LEN];
    stretch(salt, pass, digest);

    char hex[PR_SHA256_HEX_LEN + 1];
    pr_sha256_to_hex(digest, hex);

    snprintf(out, outlen, "sha256$%s$%s", salt, hex);
    return 0;
}

/*
 * Constant-time comparison of a C string against a reference. Length
 * differences are folded in so the loop cannot be used to fish out the
 * length either. Used for passwords that are compared in the clear.
 */
static bool const_time_eq(const char *given, const char *want)
{
    if (want == NULL)
        return false;
    size_t n = given != NULL ? strlen(given) : 0;
    size_t m = strlen(want);
    unsigned char diff = (unsigned char)(n ^ m);
    for (size_t i = 0; i < m; i++)
        diff |= (unsigned char)(want[i] ^ (given != NULL && i < n ? given[i] : 0));
    return diff == 0;
}

bool pr_verify_password(const char *pass, const char *stored)
{
    if (pass == NULL || stored == NULL || stored[0] == '\0')
        return false;

    char scheme[32], salt[64], want[PR_SHA256_HEX_LEN + 8];
    if (!pr_split3(stored, '$', scheme, sizeof scheme,
                   salt, sizeof salt, want, sizeof want))
        return false;
    if (!pr_str_eq_ci(scheme, "sha256"))
        return false;

    unsigned char digest[PR_SHA256_DIGEST_LEN];
    stretch(salt, pass, digest);

    char got[PR_SHA256_HEX_LEN + 1];
    pr_sha256_to_hex(digest, got);

    /* Constant-time comparison   */
    size_t n = strlen(got), m = strlen(want);
    unsigned char diff = (unsigned char)(n ^ m);
    for (size_t i = 0; i < n; i++)
        diff |= (unsigned char)(got[i] ^ want[i < m ? i : 0]);
    return diff == 0;
}

/* ======================================================================= */
/* Session files                                                           */
/* ======================================================================= */

static void session_path(const pr_config *cfg, const char *sid,
                         char *dst, size_t dstlen)
{
    char dir[1024];
    pr_session_dir(cfg, dir, sizeof dir);
    snprintf(dst, dstlen, "%.480s/%.64s", dir, sid);
}

/* SIDs must contain only hex - important against path attacks. */
static bool sid_ok(const char *sid)
{
    if (sid == NULL)
        return false;
    size_t n = strlen(sid);
    if (n != PR_SID_LEN)
        return false;
    for (size_t i = 0; i < n; i++) {
        char c = sid[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    }
    return true;
}

bool pr_session_login(const pr_config *cfg, const char *user, const char *pass,
                      pr_session *out, char *err, size_t errlen)
{
    memset(out, 0, sizeof *out);

    if (!cfg->admin_enabled) {
        snprintf(err, errlen, "the admin area is locked");
        return false;
    }

    /* Same message for user and password - does not reveal what was wrong.       */
    if (user == NULL || !pr_str_eq_ci(user, cfg->admin_user) ||
        !pr_auth_check_password(cfg, pass)) {
        snprintf(err, errlen, "user or password is wrong");
        return false;
    }

    return pr_session_create(cfg, user, out, err, errlen) == 0;
}

bool pr_auth_check_password(const pr_config *cfg, const char *pass)
{
    if (cfg == NULL)
        return false;
    if (cfg->admin_pass_hash[0] == '\0')
        return const_time_eq(pass, PR_DEFAULT_ADMIN_PASS);
    return pr_verify_password(pass, cfg->admin_pass_hash);
}

bool pr_auth_is_default(const pr_config *cfg)
{
    return cfg != NULL && cfg->admin_pass_hash[0] == '\0';
}

int pr_session_create(const pr_config *cfg, const char *user,
                      pr_session *out, char *err, size_t errlen)
{
    memset(out, 0, sizeof *out);

    char sid[PR_SID_LEN + 1];
    pr_random_hex(sid, sizeof sid);
    pr_strlcpy(out->sid, sid, sizeof out->sid);

    char csrf[PR_CSRF_LEN + 1];
    pr_random_hex(csrf, sizeof csrf);
    pr_strlcpy(out->csrf, csrf, sizeof out->csrf);

    pr_strlcpy(out->user, user != NULL ? user : "", sizeof out->user);
    out->created = pr_now_s();
    out->expires = out->created + (long long)cfg->session_ttl_min * 60;
    out->valid = true;

    char path[1024];
    session_path(cfg, out->sid, path, sizeof path);

    ini *i = ini_new();
    if (i == NULL) {
        snprintf(err, errlen, "out of memory");
        return -1;
    }
    ini_set(i, "session", "user", out->user);
    ini_set(i, "session", "csrf", out->csrf);
    ini_set_int(i, "session", "created", out->created);
    ini_set_int(i, "session", "expires", out->expires);

    int rc = ini_save(i, path, err, errlen);
    ini_free(i);
    return rc;
}

bool pr_session_lookup(const pr_config *cfg, const char *sid, pr_session *out)
{
    memset(out, 0, sizeof *out);
    if (!sid_ok(sid))
        return false;

    char path[1024];
    session_path(cfg, sid, path, sizeof path);
    if (!pr_file_exists(path))
        return false;

    char err[128];
    ini *i = ini_load(path, err, sizeof err);
    if (i == NULL)
        return false;

    long long now = pr_now_s();
    long long expires = ini_get_int(i, "session", "expires", 0);

    if (expires < now) {
        ini_free(i);
        pr_session_destroy(cfg, sid);
        return false;
    }

    pr_strlcpy(out->sid, sid, sizeof out->sid);
    pr_strlcpy(out->user, ini_get(i, "session", "user", ""), sizeof out->user);
    pr_strlcpy(out->csrf, ini_get(i, "session", "csrf", ""), sizeof out->csrf);
    out->created = ini_get_int(i, "session", "created", 0);
    out->expires = expires;
    out->valid = true;

    ini_free(i);
    return true;
}

int pr_session_destroy(const pr_config *cfg, const char *sid)
{
    if (!sid_ok(sid))
        return -1;
    char path[1024];
    session_path(cfg, sid, path, sizeof path);
    return unlink(path) == 0 ? 0 : -1;
}

void pr_session_prune(const pr_config *cfg)
{
    /* Sessions are small single files; expired ones are removed on
     * access. A full directory scan is not worth it here. */
    (void)cfg;
}

bool pr_session_from_request(const pr_config *cfg, const pr_request *req,
                             pr_session *out)
{
    memset(out, 0, sizeof *out);

    char sid[PR_SID_LEN + 8];
    if (!pr_req_cookie(req, PR_COOKIE_NAME, sid, sizeof sid))
        return false;
    return pr_session_lookup(cfg, sid, out);
}

bool pr_session_check_csrf(const pr_session *s, const char *token)
{
    if (s == NULL || !s->valid || token == NULL)
        return false;

    const char *a = s->csrf;
    const char *b = token;
    unsigned char diff = 0;
    size_t na = strlen(a), nb = strlen(b);
    diff = (unsigned char)(na ^ nb);
    size_t n = na < nb ? na : nb;
    for (size_t i = 0; i < n; i++)
        diff |= (unsigned char)(a[i] ^ b[i]);
    return diff == 0;
}

/* ======================================================================= */
/* Login lockout after failed attempts                                         */
/* ======================================================================= */

#define PR_LOGIN_MAX_FAIL 5
#define PR_LOGIN_BAN_SEC  600

static void throttle_path(const pr_config *cfg, const char *ip,
                          char *dst, size_t dstlen)
{
    char safe[64];
    size_t w = 0;
    for (const char *p = ip; *p != '\0' && w + 1 < sizeof safe; p++) {
        char c = *p;
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') || c == '.' || c == ':' || c == '-')
            safe[w++] = c;
    }
    safe[w] = '\0';

    char dir[1024];
    pr_session_dir(cfg, dir, sizeof dir);
    snprintf(dst, dstlen, "%.480s/fail-%.64s", dir, safe);
}

static long read_counter(const char *path)
{
    char err[64];
    char *text = NULL;
    size_t len = 0;
    if (pr_read_file(path, &text, &len, err, sizeof err) != 0)
        return -1;
    long v = atol(text);
    free(text);
    return v;
}

bool pr_login_throttle(const pr_config *cfg, const char *ip)
{
    char path[1024];
    throttle_path(cfg, ip, path, sizeof path);

    long v = read_counter(path);
    if (v < 0)
        return true;                    /* no history          */
    return v < PR_LOGIN_MAX_FAIL;
}

void pr_login_fail(const pr_config *cfg, const char *ip)
{
    char path[1024];
    throttle_path(cfg, ip, path, sizeof path);

    long v = read_counter(path);
    if (v < 0)
        v = 0;
    v++;

    char buf[32];
    snprintf(buf, sizeof buf, "%ld", v);
    char err[64];
    (void)pr_write_file_atomic(path, buf, strlen(buf), err, sizeof err);
}

void pr_login_ok(const pr_config *cfg, const char *ip)
{
    char path[1024];
    throttle_path(cfg, ip, path, sizeof path);
    unlink(path);
}
