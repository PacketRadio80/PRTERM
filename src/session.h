/*
 * PRTERM - CB & Amateur Radio Terminal
 * session.h - Admin-Anmeldung, Sessions und CSRF.
 *
 * Der Passwort-Hash hat die Form   sha256$<salt>$<hash>
 * und wird erzeugt mit   ./prterm.cgi --hash-password "passwort"
 * Ein leerer pass_hash-Eintrag in der INI heisst: KEIN Login moeglich.
 *
 * SPDX-License-Identifier: MIT
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

/* ---- Passwort --------------------------------------------------------- */
int  pr_hash_password(const char *pass, char *out, size_t outlen);
bool pr_verify_password(const char *pass, const char *stored);

/* ---- Session ---------------------------------------------------------- */
bool pr_session_login(const pr_config *cfg, const char *user, const char *pass,
                      pr_session *out, char *err, size_t errlen);
int  pr_session_create(const pr_config *cfg, const char *user,
                       pr_session *out, char *err, size_t errlen);
bool pr_session_lookup(const pr_config *cfg, const char *sid, pr_session *out);
int  pr_session_destroy(const pr_config *cfg, const char *sid);
void pr_session_prune(const pr_config *cfg);

/* Session aus dem Request ermitteln (Cookie). valid = eingeloggt. */
bool pr_session_from_request(const pr_config *cfg, const pr_request *req,
                             pr_session *out);

/* CSRF-Token vergleichen (konstantzeitig). */
bool pr_session_check_csrf(const pr_session *s, const char *token);

/* Einfacher Schutz gegen Brute-Force: Fehlversuche pro IP. */
bool pr_login_throttle(const pr_config *cfg, const char *ip);
void pr_login_fail(const pr_config *cfg, const char *ip);
void pr_login_ok(const pr_config *cfg, const char *ip);

#endif /* PRTERM_SESSION_H */
