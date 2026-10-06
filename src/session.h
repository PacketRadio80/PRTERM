/*
 * PRTERM - CB & Amateur Radio Terminal
 * session.h - Admin login, sessions and CSRF.
 *
 * The password hash has the form   sha256$<salt>$<hash>
 * and is created with   ./prterm.cgi --hash-password "passwort"
 * An empty pass_hash entry in the INI means: NO login possible.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PRTERM_SESSION_H
#define PRTERM_SESSION_H

#include "cgi.h"
#include "config.h"

#include <stdbool.h>
#include <stddef.h>

#define PR_SID_LEN  40
#define PR_CSRF_LEN 32

typedef struct pr_session {
    char      sid[PR_SID_LEN + 1];
    char      user[64];
    char      csrf[PR_CSRF_LEN + 1];
    long long created;
    long long expires;
    bool      valid;
} pr_session;

/* ---- Password --------------------------------------------------------- */
int  pr_hash_password(const char *pass, char *out, size_t outlen);
bool pr_verify_password(const char *pass, const char *stored);

/*
 * Built-in password for the admin area.
 *
 * It applies as long as no hash is set under [admin] pass_hash - the area has
 * to be reachable on a fresh install, otherwise nobody can ever configure
 * anything. Change it as soon as the installation is yours; while it is still
 * in place the whole admin area is open to anyone who knows this string.
 */
#define PR_DEFAULT_ADMIN_PASS "PRTerm"

/** True while the built-in password is still the effective one. */
bool pr_auth_is_default(const pr_config *cfg);

/**
 * Check a password against whatever is currently effective: the operator
 * hash when one is set, the built-in default otherwise. Login and password
 * change must both go through here so they can never disagree.
 */
bool pr_auth_check_password(const pr_config *cfg, const char *pass);


/* ---- Session ---------------------------------------------------------- */
bool pr_session_login(const pr_config *cfg, const char *user, const char *pass,
                      pr_session *out, char *err, size_t errlen);
int  pr_session_create(const pr_config *cfg, const char *user,
                       pr_session *out, char *err, size_t errlen);
bool pr_session_lookup(const pr_config *cfg, const char *sid, pr_session *out);
int  pr_session_destroy(const pr_config *cfg, const char *sid);
void pr_session_prune(const pr_config *cfg);

/* Determine session from the request (cookie). valid = logged in. */
bool pr_session_from_request(const pr_config *cfg, const pr_request *req,
                             pr_session *out);

/* Compare CSRF token (constant time).      */
bool pr_session_check_csrf(const pr_session *s, const char *token);

/* Simple protection against brute force: failed attempts per IP. */
bool pr_login_throttle(const pr_config *cfg, const char *ip);
void pr_login_fail(const pr_config *cfg, const char *ip);
void pr_login_ok(const pr_config *cfg, const char *ip);

#endif /* PRTERM_SESSION_H */
