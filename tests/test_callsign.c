/*
 * PRTERM - Test: CALLID / CALLERID
 *
 * The spec is deliberately stricter than AX.25:
 *   CALLID   max. 6 chars, no suffix
 *   CALLERID max. 6 chars + "-<digit>" = 8 chars total
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "prterm_compat.h"

#include "callsign.h"
#include "testutil.h"

#include <string.h>

int main(void)
{
    pr_call_rules r;
    pr_call_rules_default(&r);

    printf("== Regeln ==\n");
    CHECK_INT(r.callid_max_len, 6);
    CHECK_INT(r.callerid_base_len, 6);
    CHECK_INT(r.callerid_max_total, 8);
    CHECK(r.allow_ssid);
    CHECK_INT(r.ssid_digits, 1);

    printf("\n== CALLID ==\n");
    CHECK(callid_valid("DL1ABC", &r));
    CHECK(callid_valid("CQ", &r));
    CHECK(callid_valid("A1", &r));
    CHECK(callid_valid("PRTERM", &r));
    CHECK(!callid_valid("", &r));
    CHECK(!callid_valid("DL1ABCD", &r));      /* 7 chars   */
    CHECK(!callid_valid("DL1AB-1", &r));      /* no suffix   */
    CHECK(!callid_valid("DL 1AB", &r));       /* blanks      */
    CHECK(!callid_valid("DL1A-", &r));

    printf("\n== CALLERID: 6 + 2 ==\n");
    CHECK(callerid_valid("DL1ABC", &r));      /* without SSID */
    CHECK(callerid_valid("DL1ABC-1", &r));    /* 6 + 2 = 8 */
    CHECK(callerid_valid("PRTERM-7", &r));
    CHECK(callerid_valid("A-0", &r));
    CHECK(!callerid_valid("DL1ABC-12", &r));  /* 9 chars: too long  */
    CHECK(!callerid_valid("DL1ABCD-1", &r));  /* base too long */
    CHECK(!callerid_valid("DL1ABC-", &r));    /* SSID without digit */
    CHECK(!callerid_valid("DL1ABC-A", &r));   /* SSID not a digit  */
    CHECK(!callerid_valid("", &r));

    printf("\n== SSID-Begrenzung konfigurierbar ==\n");
    {
        pr_call_rules ax = r;
        ax.ssid_digits = 2;
        ax.callerid_max_total = 9;
        CHECK(callerid_valid("DL1ABC-12", &ax));   /* AX.25: -0..-15 */
        CHECK(!callerid_valid("DL1ABC-123", &ax));
    }
    {
        pr_call_rules nos = r;
        nos.allow_ssid = false;
        CHECK(!callerid_valid("DL1ABC-1", &nos));
        CHECK(callerid_valid("DL1ABC", &nos));
    }

    printf("\n== Normalisierung ==\n");
    {
        char buf[16];
        CHECK(callerid_normalize(buf, sizeof buf, "  dl1abc-1  ", &r));
        CHECK_STR(buf, "DL1ABC-1");

        CHECK(callid_normalize(buf, sizeof buf, "dl1abc", &r));
        CHECK_STR(buf, "DL1ABC");

        CHECK(!callerid_normalize(buf, sizeof buf, "vielzulang-9", &r));
    }

    printf("\n== Zerlegen ==\n");
    {
        char base[16];
        int ssid = -2;
        CHECK(callerid_split("DL1ABC-1", base, sizeof base, &ssid));
        CHECK_STR(base, "DL1ABC");
        CHECK_INT(ssid, 1);

        CHECK(callerid_split("DL1ABC", base, sizeof base, &ssid));
        CHECK_STR(base, "DL1ABC");
        CHECK_INT(ssid, -1);

        CHECK(!callerid_split("DL1ABC-", base, sizeof base, &ssid));
    }

    printf("\n== Muster ==\n");
    CHECK(call_pattern_match("DL9*", "DL9ABC"));
    CHECK(call_pattern_match("DL9*", "DL9"));
    CHECK(!call_pattern_match("DL9*", "DL8ABC"));
    CHECK(call_pattern_match("KB1ABC-3", "KB1ABC-3"));
    CHECK(call_pattern_match("KB1ABC-?", "KB1ABC-3"));
    CHECK(!call_pattern_match("KB1ABC-?", "KB1ABC-33"));
    CHECK(call_pattern_match("*SPAM*", "XSPAMX"));
    CHECK(call_pattern_match("*", ""));
    CHECK(call_pattern_match("dl9*", "DL9ABC"));   /* case-insensitive */
    CHECK(!call_pattern_match("", "X"));

    {
        const char *const pats[] = { "DL9*", "KB1ABC-3" };
        CHECK(call_pattern_list_match(pats, 2, "DL9XYZ") != NULL);
        CHECK(call_pattern_list_match(pats, 2, "KB1ABC-3") != NULL);
        CHECK(call_pattern_list_match(pats, 2, "OK1KQ") == NULL);
    }

    printf("\n== AX.25-Wire-Encoding ==\n");
    {
        unsigned char a[7], b[7];
        CHECK(call_to_ax25("DL1ABC", a));
        CHECK(call_to_ax25("DL1ABC-1", b));

        /* base must be identical (space padded, shifted)      */
        CHECK(memcmp(a, b, 6) == 0);
        CHECK(a[0] == (unsigned char)('D' << 1));

        char back[16];
        CHECK(call_from_ax25(a, back, sizeof back));
        CHECK_STR(back, "DL1ABC");
        CHECK(call_from_ax25(b, back, sizeof back));
        CHECK_STR(back, "DL1ABC-1");
    }

    TEST_SUMMARY("callsign");
}
