/*
 * PRTERM - Test: state and log persistence
 *
 * Living in its own runtime directory so the tests touch nothing
 * that belongs to operation.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "prterm_compat.h"

#include "config.h"
#include "state.h"
#include "testutil.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TESTDIR "test-runtime"

static void wipe(void)
{
    char cmd[128];
    snprintf(cmd, sizeof cmd, "rm -rf %s", TESTDIR);
    if (system(cmd) != 0)
        fprintf(stderr, "Warning: %s could not be removed\n", cmd);
}

static pr_msg mk(char kind, const char *from, const char *text, int db)
{
    pr_msg m;
    memset(&m, 0, sizeof m);
    m.kind = kind;
    snprintf(m.from, sizeof m.from, "%s", from);
    snprintf(m.text, sizeof m.text, "%s", text);
    m.db = db;
    m.ts = 1700000000;
    return m;
}

int main(void)
{
    char err[256];
    wipe();

    pr_config cfg;
    pr_config_defaults(&cfg);
    pr_strlcpy(cfg.runtime_dir, TESTDIR, sizeof cfg.runtime_dir);
    cfg.max_log = 50;

    printf("== Laufzeitverzeichnis ==\n");
    CHECK_INT(pr_runtime_init(&cfg, err, sizeof err), 0);
    CHECK_INT(access(TESTDIR, F_OK), 0);

    printf("\n== Rig-Zustand ==\n");
    {
        pr_rig_state st;
        CHECK_INT(pr_state_load(&cfg, &st, err, sizeof err), 0);
        CHECK_INT(st.freq_hz, cfg.freq_hz);   /* Default from the config */

        st.freq_hz = 27405000L;
        st.mode = PR_BAND_FM;
        st.duplex = PR_DUPLEX_HALF;
        st.ptt = true;
        st.rx_count = 42;
        st.tx_count = 7;
        snprintf(st.device, sizeof st.device, "sim");
        snprintf(st.status, sizeof st.status, "running");
        st.link_ok = true;

        CHECK_INT(pr_state_save(&cfg, &st, err, sizeof err), 0);

        pr_rig_state back;
        CHECK_INT(pr_state_load(&cfg, &back, err, sizeof err), 0);
        CHECK_INT(back.freq_hz, 27405000L);
        CHECK_INT(back.mode, PR_BAND_FM);
        CHECK(back.duplex == PR_DUPLEX_HALF);
        CHECK(back.ptt);
        CHECK_INT(back.rx_count, 42);
        CHECK_INT(back.tx_count, 7);
        CHECK_STR(back.device, "sim");
        CHECK_STR(back.status, "running");
        CHECK(back.link_ok);
    }

    printf("\n== Log schreiben und lesen ==\n");
    {
        pr_msg m1 = mk(PR_MSG_RX, "DL1ABC", "Guten Morgen", -42);
        pr_msg m2 = mk(PR_MSG_TX, "PRTERM-1", "Hallo zurueck", -6);
        pr_msg m3 = mk(PR_MSG_SYS, "SYS", "gestartet", 0);

        CHECK_INT(pr_log_append(&cfg, &m1, err, sizeof err), 0);
        CHECK_INT(pr_log_append(&cfg, &m2, err, sizeof err), 0);
        CHECK_INT(pr_log_append(&cfg, &m3, err, sizeof err), 0);
        CHECK_INT(pr_log_count(&cfg), 3);

        pr_msg out[8];
        size_t n = 0;
        CHECK_INT(pr_log_tail(&cfg, out, 8, &n, err, sizeof err), 0);
        CHECK_INT(n, 3);

        CHECK_INT(out[0].kind, PR_MSG_RX);
        CHECK_STR(out[0].from, "DL1ABC");
        CHECK_STR(out[0].text, "Guten Morgen");
        CHECK_INT(out[0].db, -42);

        CHECK_INT(out[1].kind, PR_MSG_TX);
        CHECK_STR(out[1].from, "PRTERM-1");

        CHECK_INT(out[2].kind, PR_MSG_SYS);
        CHECK_STR(out[2].text, "gestartet");
    }

    printf("\n== Log-Begrenzung ==\n");
    {
        for (int k = 0; k < 200; k++) {
            pr_msg m = mk(PR_MSG_RX, "DL1ABC", "test", -50);
            CHECK_INT(pr_log_append(&cfg, &m, err, sizeof err), 0);
        }
        long c = pr_log_count(&cfg);
        CHECK(c > 0);
        CHECK_INT(c, 50);            /* exactly max_log, no tolerance */
    }

    printf("\n== Nur die letzten N ==\n");
    {
        pr_msg out[4];
        size_t n = 0;
        CHECK_INT(pr_log_tail(&cfg, out, 4, &n, err, sizeof err), 0);
        CHECK_INT(n, 4);
    }

    printf("\n== Sonderzeichen im Text ==\n");
    {
        /* Tabs and line breaks must not break the tabular form             */
        pr_msg m = mk(PR_MSG_RX, "DL1ABC", "mit\ttab\nund\nbruch", -30);
        CHECK_INT(pr_log_append(&cfg, &m, err, sizeof err), 0);

        pr_msg out[4];
        size_t n = 0;
        CHECK_INT(pr_log_tail(&cfg, out, 4, &n, err, sizeof err), 0);
        CHECK(n >= 1);
        if (n >= 1) {
            const pr_msg *last = &out[n - 1];
            CHECK_INT(last->kind, PR_MSG_RX);
            CHECK(strchr(last->text, '\t') == NULL);
            CHECK(strchr(last->text, '\n') == NULL);
        }
    }

    printf("\n== Sperre ==\n");
    {
        int fd = pr_state_lock(&cfg, err, sizeof err);
        CHECK(fd >= 0);
        pr_state_unlock(fd);
    }

    wipe();
    TEST_SUMMARY("state");
}
