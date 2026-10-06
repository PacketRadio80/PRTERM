/*
 * PRTERM - Test: INI parser
 * Most important property: comments and order survive saving - the
 * prterm.ini is documented and maintained by hand.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "prterm_compat.h"

#include "ini.h"
#include "testutil.h"
#include "util.h"

#include <stdlib.h>

static const char *SRC =
    "; Kopfkommentar\n"
    "# zweite Kommentarform\n"
    "\n"
    "[site]\n"
    "name = PRTERM      ; Inline-Kommentar\n"
    "language = de\n"
    "\n"
    "[radio]\n"
    "duplex = full\n"
    "freq_hz = 27125000\n"
    "\n"
    "[ban]\n"
    "DL9* = Spam\n"
    "\n";

int main(void)
{
    char err[256];

    printf("== INI lesen ==\n");
    ini *i = ini_parse(SRC, err, sizeof err);
    CHECK(i != NULL);
    if (i == NULL) TEST_SUMMARY("ini");

    CHECK_STR(ini_get(i, "site", "name", "?"), "PRTERM");
    CHECK_STR(ini_get(i, "radio", "duplex", "?"), "full");
    CHECK_INT(ini_get_int(i, "radio", "freq_hz", 0), 27125000L);
    CHECK_STR(ini_get(i, "site", "fehlt", "DEFAULT"), "DEFAULT");
    CHECK_INT(ini_section_count(i, "ban"), 1);

    /* Keys and sections are case-insensitive        */
    CHECK_STR(ini_get(i, "SITE", "NAME", "?"), "PRTERM");

    printf("\n== INI schreiben ==\n");
    ini_set(i, "radio", "duplex", "half");     /* change existing value    */
    ini_set_int(i, "radio", "rx_poll_ms", 250); /* new key                 */
    ini_set(i, "admin", "user", "admin");       /* new section             */
    ini_del(i, "site", "language");             /* delete                  */

    char *out = ini_dump(i);
    CHECK(out != NULL);
    if (out == NULL) {
        ini_free(i);
        TEST_SUMMARY("ini");
    }

    printf("\n--- saved ---\n%s-------------------\n", out);

    CHECK(strstr(out, "; Kopfkommentar") != NULL);
    CHECK(strstr(out, "# zweite Kommentarform") != NULL);
    CHECK(strstr(out, "name = PRTERM      ; Inline-Kommentar") != NULL);
    CHECK(strstr(out, "duplex = half") != NULL);
    CHECK(strstr(out, "rx_poll_ms = 250") != NULL);
    CHECK(strstr(out, "[admin]") != NULL);
    CHECK(strstr(out, "language") == NULL);
    CHECK(strstr(out, "freq_hz = 27125000") != NULL);

    /* No duplicates    */
    {
        const char *p = strstr(out, "rx_poll_ms");
        CHECK(p != NULL);
        if (p != NULL)
            CHECK(strstr(p + 1, "rx_poll_ms") == NULL);
    }

    printf("\n== Roundtrip ==\n");
    ini *j = ini_parse(out, err, sizeof err);
    CHECK(j != NULL);
    if (j != NULL) {
        CHECK_STR(ini_get(j, "radio", "duplex", "?"), "half");
        CHECK_INT(ini_get_int(j, "radio", "rx_poll_ms", 0), 250);
        CHECK_STR(ini_get(j, "admin", "user", "?"), "admin");
        CHECK_INT(ini_get_int(j, "radio", "freq_hz", 0), 27125000L);
        ini_free(j);
    }

    printf("\n== Schlechtes Eingabematerial ==\n");
    {
        ini *k = ini_parse("nur text ohne format\n[tiefe\nkey ohne wert\n",
                           err, sizeof err);
        CHECK(k != NULL);          /* must be tolerated     */
        ini_free(k);
    }
    {
        ini *k = ini_parse("[s]\nleer =\nquoted = \"mit ; semikolon\"\n",
                           err, sizeof err);
        CHECK(k != NULL);
        if (k != NULL) {
            CHECK_STR(ini_get(k, "s", "quoted", "?"), "mit ; semikolon");
            ini_free(k);
        }
    }

    free(out);
    ini_free(i);
    TEST_SUMMARY("ini");
}
