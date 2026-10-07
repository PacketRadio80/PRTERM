/*
 * PRTERM - Test: admin login, sessions, CSRF and brute-force throttle
 *
 * Locks the security-relevant behaviour:
 *
 *   - the password hash (sha256$<salt>$<hash>) and its verification
 *   - the BUILT-IN password of a fresh installation - and that it is
 *     OFF the moment an operator hash is set
 *   - the session lifecycle, including expiry
 *   - the CSRF comparison (constant time, length folded in)
 *   - the login throttle after failed attempts
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "prterm_compat.h"

#include "config.h"
#include "session.h"
#include "state.h"
#include "testutil.h"
#include "util.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define TEST_DIR "test-session.runtime"
#define TEST_INI TEST_DIR "/prterm.ini"

static bool write_ini(void)
{
    FILE *f = fopen(TEST_INI, "w");
    if (f == NULL)
        return false;

    fprintf(f,
        "[site]\n"
        "name = Test\n\n"
        "[admin]\n"
        "enabled = true\n"
        "user = admin\n"
        "pass_hash =\n"
        "session_ttl_min = 60\n\n"
        "[paths]\n"
        "runtime_dir = %s\n",
        TEST_DIR);

    fclose(f);
    return true;
}

int main(void)
{
    char err[256];

    printf("== Password hash ==\n");
    {
        char hash[200];
        CHECK(pr_hash_password("geheim-1", hash, sizeof hash) == 0);
        CHECK(strncmp(hash, "sha256$", 7) == 0);
        CHECK(pr_verify_password("geheim-1", hash));
        CHECK(!pr_verify_password("geheim-2", hash));
        CHECK(!pr_verify_password("", hash));
        CHECK(!pr_verify_password("geheim-1", ""));
        CHECK(!pr_verify_password("geheim-1", "sha256$nur-zwei-teile"));
        CHECK(!pr_verify_password("geheim-1", "bcrypt$abc$def"));

        /* a fresh hash every time - the salt is random            */
        char hash2[200];
        CHECK(pr_hash_password("geheim-1", hash2, sizeof hash2) == 0);
        CHECK(strcmp(hash, hash2) != 0);
        CHECK(pr_verify_password("geheim-1", hash2));

        CHECK(pr_hash_password("geheim-1", NULL, 0) == -1);
    }

    (void)mkdir(TEST_DIR, 0777);
    CHECK(write_ini());

    pr_config cfg;
    CHECK(pr_config_load(&cfg, TEST_INI, err, sizeof err) == 0);
    CHECK(pr_runtime_init(&cfg, err, sizeof err) == 0);

    printf("\n== Built-in password of a fresh installation ==\n");
    {
        /* the INI has no hash: the built-in password is active     */
        CHECK(cfg.admin_pass_hash[0] == '\0');
        CHECK(pr_auth_is_default(&cfg));
        CHECK(pr_auth_check_password(&cfg, PR_DEFAULT_ADMIN_PASS));
        CHECK(!pr_auth_check_password(&cfg, "prtterm"));
        CHECK(!pr_auth_check_password(&cfg, ""));
        CHECK(!pr_auth_check_password(&cfg, "PRTerm "));

        /* the moment an operator hash is set, it is OFF            */
        char hash[200];
        CHECK(pr_hash_password("geheim-1", hash, sizeof hash) == 0);
        pr_strlcpy(cfg.admin_pass_hash, hash, sizeof cfg.admin_pass_hash);

        CHECK(!pr_auth_is_default(&cfg));
        CHECK(pr_auth_check_password(&cfg, "geheim-1"));
        CHECK(!pr_auth_check_password(&cfg, PR_DEFAULT_ADMIN_PASS));
    }

    printf("\n== Sessions ==\n");
    {
        pr_session s;
        CHECK(pr_session_create(&cfg, "admin", &s, err, sizeof err) == 0);
        CHECK(s.valid);
        CHECK_INT(strlen(s.sid), PR_SID_LEN);
        CHECK_INT(strlen(s.csrf), PR_CSRF_LEN);
        CHECK_STR(s.user, "admin");
        CHECK(s.expires > s.created);

        pr_session back;
        CHECK(pr_session_lookup(&cfg, s.sid, &back));
        CHECK(back.valid);
        CHECK_STR(back.user, "admin");
        CHECK_STR(back.csrf, s.csrf);

        /* no session for a name that is not a session id          */
        CHECK(!pr_session_lookup(&cfg, "nichts", &back));
        CHECK(!pr_session_lookup(&cfg, "", &back));
        CHECK(!pr_session_lookup(&cfg, NULL, &back));
        {
            /* right length, wrong characters                     */
            char bad[PR_SID_LEN + 1];
            memset(bad, 'z', sizeof bad);
            bad[PR_SID_LEN] = '\0';
            CHECK(!pr_session_lookup(&cfg, bad, &back));
        }

        /* CSRF - constant time, but the same verdict             */
        CHECK(pr_session_check_csrf(&s, s.csrf));
        CHECK(!pr_session_check_csrf(&s, "etwas anderes"));
        CHECK(!pr_session_check_csrf(&s, ""));
        CHECK(!pr_session_check_csrf(&s, NULL));
        CHECK(!pr_session_check_csrf(NULL, s.csrf));
        {
            /* a prefix of the token is not the token             */
            char part[PR_CSRF_LEN + 1];
            pr_strlcpy(part, s.csrf, sizeof part);
            part[PR_CSRF_LEN - 1] = '\0';
            CHECK(!pr_session_check_csrf(&s, part));
        }

        CHECK(pr_session_destroy(&cfg, s.sid) == 0);
        CHECK(!pr_session_lookup(&cfg, s.sid, &back));
        CHECK(pr_session_destroy(&cfg, s.sid) != 0);   /* gone      */
    }

    printf("\n== Expiry ==\n");
    {
        long long saved = cfg.session_ttl_min;
        cfg.session_ttl_min = -1;                  /* expired at once */

        pr_session old;
        CHECK(pr_session_create(&cfg, "admin", &old, err, sizeof err) == 0);
        pr_session back;
        CHECK(!pr_session_lookup(&cfg, old.sid, &back));
        CHECK(!back.valid);

        cfg.session_ttl_min = saved;
        pr_session_prune(&cfg);                    /* must not crash */
        CHECK(pr_session_create(&cfg, "admin", &old, err, sizeof err) == 0);
        CHECK(pr_session_lookup(&cfg, old.sid, &back));
    }

    printf("\n== Login ==\n");
    {
        pr_session s;

        /* the same message for a wrong user and a wrong password  */
        char err_user[256], err_pass[256];
        CHECK(!pr_session_login(&cfg, "fremd", "geheim-1", &s, err_user, sizeof err_user));
        CHECK(!pr_session_login(&cfg, "admin", "falsch", &s, err_pass, sizeof err_pass));
        CHECK_STR(err_user, err_pass);

        CHECK(pr_session_login(&cfg, "admin", "geheim-1", &s, err, sizeof err));
        CHECK(s.valid);

        /* the cookie of the request identifies the session        */
        pr_request req;
        memset(&req, 0, sizeof req);
        snprintf(req.cookie, sizeof req.cookie, "PRTERM_SID=%s", s.sid);
        pr_session from;
        CHECK(pr_session_from_request(&cfg, &req, &from));
        CHECK(from.valid);
        CHECK_STR(from.user, "admin");
        CHECK_STR(from.csrf, s.csrf);

        /* and a wrong cookie does not                             */
        memset(&req, 0, sizeof req);
        pr_strlcpy(req.cookie,
                   "PRTERM_SID=0000000000000000000000000000000000000000",
                   sizeof req.cookie);
        CHECK(!pr_session_from_request(&cfg, &req, &from));
        memset(&req, 0, sizeof req);
        CHECK(!pr_session_from_request(&cfg, &req, &from));

        /* the admin area can be switched off entirely             */
        bool saved = cfg.admin_enabled;
        cfg.admin_enabled = false;
        CHECK(!pr_session_login(&cfg, "admin", "geheim-1", &s, err, sizeof err));
        cfg.admin_enabled = saved;
    }

    printf("\n== Brute force ==\n");
    {
        /* the counter lives in the runtime directory and survives a
         * second run - start clean so the test is reproducible */
        (void)unlink(TEST_DIR "/sessions/fail-192.0.2.1");

        CHECK(pr_login_throttle(&cfg, "192.0.2.1"));
        for (int i = 0; i < 5; i++)
            pr_login_fail(&cfg, "192.0.2.1");
        CHECK(!pr_login_throttle(&cfg, "192.0.2.1"));   /* locked   */
        CHECK(pr_login_throttle(&cfg, "192.0.2.2"));    /* others   */
    }

    TEST_SUMMARY("session");
}
