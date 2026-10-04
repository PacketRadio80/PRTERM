/*
 * PRTERM - Test: Konfigurationsmodell
 *
 * SPDX-License-Identifier: MIT
 */
#include "prterm_compat.h"

#include "config.h"
#include "testutil.h"

#include <stdlib.h>

static const char *INI =
    "[site]\n"
    "name = Mein Terminal\n"
    "subtitle = Test\n"
    "language = de\n"
    "\n"
    "[station]\n"
    "callerid = DL1ABC-1\n"
    "qth = JN48\n"
    "\n"
    "[radio]\n"
    "driver = sim\n"
    "duplex = full\n"
    "freq_hz = 27125000\n"
    "mode = am\n"
    "tx_power_mw = 4000\n"
    "baud = 19200\n"
    "\n"
    "[callsign]\n"
    "callid_max_len = 6\n"
    "callerid_base_len = 6\n"
    "callerid_max_total = 8\n"
    "callerid_allow_ssid = true\n"
    "callerid_ssid_digits = 1\n"
    "\n"
    "[ui]\n"
    "font_file = ./fonts/test.ttf\n"
    "font_size = 16\n"
    "line_height = 1.3\n"
    "density = compact\n"
    "theme = silver\n"
    "\n"
    "[ban]\n"
    "DL9* = Spam\n"
    "KB1ABC-3 = Stoerer\n";

int main(void)
{
    char err[256];

    printf("== Defaults ==\n");
    pr_config d;
    pr_config_defaults(&d);
    CHECK_STR(d.rig_driver, "sim");
    CHECK_STR(d.ui_theme, "silver");
    CHECK_STR(d.callerid, "PRTERM-1");
    CHECK_INT(d.tx_power_mw, 4000L);
    CHECK(d.duplex == PR_DUPLEX_FULL);
    pr_config_free(&d);

    printf("\n== Uebernehmen ==\n");
    ini *i = ini_parse(INI, err, sizeof err);
    CHECK(i != NULL);
    if (i == NULL) TEST_SUMMARY("config");

    pr_config c;
    CHECK_INT(pr_config_apply(&c, i, err, sizeof err), 0);

    CHECK_STR(c.site_name, "Mein Terminal");
    CHECK_STR(c.callerid, "DL1ABC-1");
    CHECK_STR(c.rig_driver, "sim");
    CHECK_INT(c.freq_hz, 27125000L);
    CHECK_INT(c.baud, 19200L);
    CHECK_INT(c.tx_power_mw, 4000L);
    CHECK(c.duplex == PR_DUPLEX_FULL);
    CHECK_INT(c.mode, PR_BAND_AM);

    /* Schrift: Groesse + Zeilenabstand in Prozent */
    CHECK_STR(c.font_file, "./fonts/test.ttf");
    CHECK_INT(c.font_size, 16);
    CHECK_INT(c.line_height_pct, 130);

    printf("\n== Kanal zu Frequenz ==\n");
    CHECK_INT(pr_config_channel(&c), 14);      /* 27.125 MHz = Kanal 14 */
    CHECK(pr_config_set_channel(&c, 40));
    CHECK_INT(c.freq_hz, 27405000L);
    CHECK(pr_config_set_channel(&c, 41));
    CHECK_INT(c.freq_hz, 26565000L);
    /* Kanal 41 erlaubt nur FM - Betriebsart muss mitgezogen werden */
    CHECK_INT(c.mode, PR_BAND_FM);
    CHECK(!pr_config_set_channel(&c, 0));
    CHECK(!pr_config_set_channel(&c, 99));

    printf("\n== Ban-Liste ==\n");
    CHECK_INT(c.nbans, 2);
    CHECK(pr_config_is_banned(&c, "DL9ABC"));
    CHECK(pr_config_is_banned(&c, "DL9"));
    CHECK(!pr_config_is_banned(&c, "DL8ABC"));
    CHECK(pr_config_is_banned(&c, "KB1ABC-3"));
    CHECK(!pr_config_is_banned(&c, "KB1ABC"));

    CHECK(pr_config_add_ban(&c, "OK1*", "Test") == 0);
    CHECK_INT(c.nbans, 3);
    CHECK(pr_config_is_banned(&c, "OK1KQ"));

    CHECK(pr_config_del_ban(&c, "OK1*"));
    CHECK_INT(c.nbans, 2);
    CHECK(!pr_config_is_banned(&c, "OK1KQ"));
    CHECK(!pr_config_del_ban(&c, "OK1*"));     /* nochmal: nicht vorhanden */

    printf("\n== Ablehnung schlechter Werte ==\n");
    {
        ini *bad = ini_parse("[station]\ncallerid = DL1ABCD-1\n", err, sizeof err);
        pr_config t;
        CHECK(pr_config_apply(&t, bad, err, sizeof err) != 0);
        pr_config_free(&t);
        ini_free(bad);
    }
    {
        ini *bad = ini_parse("[radio]\nmode = xyz\n", err, sizeof err);
        pr_config t;
        CHECK(pr_config_apply(&t, bad, err, sizeof err) != 0);
        pr_config_free(&t);
        ini_free(bad);
    }

    printf("\n== Rueckschreiben ==\n");
    {
        ini *out = ini_new();
        pr_config_write(&c, out);
        CHECK_STR(ini_get(out, "station", "callerid", "?"), "DL1ABC-1");
        CHECK_STR(ini_get(out, "radio", "mode", "?"), "fm");
        CHECK_INT(ini_get_int(out, "radio", "tx_power_mw", 0), 4000L);
        CHECK_INT(ini_get_int(out, "ui", "font_size", 0), 16);
        ini_free(out);
    }

    pr_config_free(&c);
    ini_free(i);
    TEST_SUMMARY("config");
}
