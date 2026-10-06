/*
 * PRTERM - Test: TNC detection, echo handling and scoring
 *
 * These functions proved wrong in operation several times:
 *   1. short patterns destroyed the marker in longer ones
 *   2. NUL bytes aborted the string-based removal
 *   3. bare line ends ate content of the real response
 *   4. garbage beat a clean echo
 * That is why there is a test here instead of just a manual run.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "prterm_compat.h"

#include "probe.h"
#include "testutil.h"
#include "util.h"

#include <string.h>

/*
 * The same probes as in the production code - exclusively reading.
 * ESC V is the native TheFirmware probe.
 */
static const char *const probes[] = { "\r", "\x1b" "V\r" };
#define NPROBES (sizeof probes / sizeof probes[0])

int main(void)
{
    printf("== detecting garbage ==\n");
    {
        /* At the wrong baud rate garbage arrives - it must not score. */
        unsigned char garbage[] = {
            0xff, 0xfe, 0xfd, 0xfc, 0xfb, 0xfa, 0xf9, 0xf8,
            0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87
        };
        CHECK(pr_probe_printable_ratio(garbage, sizeof garbage) < 0.70);
        CHECK_INT(pr_probe_score(garbage, sizeof garbage), 0);

        const unsigned char *good =
            (const unsigned char *)"cmd: MYCALL TNC2C Version 2.7";
        CHECK(pr_probe_printable_ratio(good, strlen((const char *)good)) > 0.95);
        CHECK(pr_probe_score(good, strlen((const char *)good)) > 100);
    }

    printf("\n== random hits in garbage ==\n");
    {
        /* A word like KISS must not count in garbage.               */
        unsigned char garbage[64];
        for (size_t i = 0; i < sizeof garbage; i++)
            garbage[i] = (unsigned char)(0x80 + (i % 32));
        garbage[0] = 'K'; garbage[1] = 'I'; garbage[2] = 'S'; garbage[3] = 'S';

        CHECK(pr_probe_printable_ratio(garbage, sizeof garbage) < 0.70);
        CHECK_INT(pr_probe_score(garbage, sizeof garbage), 0);
    }

    printf("\n== stripping echo ==\n");
    {
        /* Pure echo of the probes      */
        unsigned char echo[64];
        size_t len = 0;
        for (size_t i = 0; i < NPROBES; i++) {
            size_t n = strlen(probes[i]);
            memcpy(echo + len, probes[i], n);
            len += n;
        }
        CHECK_INT(len, 4);

        size_t after = pr_probe_strip_echo(echo, len);
        /*
         * Line ends may remain - patterns without meaningful
         * characters are not removed. Crucial: NO content remains.
         */
        CHECK(!pr_probe_has_content(echo, after));
    }

    printf("\n== NUL bytes do not stop the removal ==\n");
    {
        /*
         * That was the bug: strstr/strlen stop at NUL. The removal
         * must work across the NUL.
         */
        unsigned char buf[64];
        size_t len = 0;
        buf[len++] = 0x00;                       /* leading ^@    */
        memcpy(buf + len, "\x1b" "V\r", 3); len += 3;
        buf[len++] = 0x00;
        memcpy(buf + len, "\x1b" "V\r", 3); len += 3;

        size_t after = pr_probe_strip_echo(buf, len);
        /* The two NULs remain, the probes disappear              */
        CHECK_INT(after, 2);
        CHECK(!pr_probe_has_content(buf, after));
    }

    printf("\n== short patterns do not destroy longer ones ==\n");
    {
        /*
         * The bare "\r" is not removed (it cannot be told apart from
         * real line structure) and therefore must not break up the
         * longer probe either.
         */
        unsigned char b1[64];
        memcpy(b1, "\x1b" "V\r\x1b" "V\r", 6);
        CHECK_INT(pr_probe_strip_echo(b1, 6), 0);

        /* General: a short pattern must not dissolve a longer one */
        unsigned char b2[] = "XXYZ";
        CHECK_INT(pr_probe_remove_bytes(b2, 4, (const unsigned char *)"YZ", 2), 2);
    }

    printf("\n== echo plus a real reply ==\n");
    {
        unsigned char buf[256];
        size_t len = 0;
        const char *echo_part = "\r\x1b" "V\r\r";
        memcpy(buf + len, echo_part, strlen(echo_part));
        len += strlen(echo_part);
        const char *real = "TheFirmware V2.7\r\ncmd: \r";
        memcpy(buf + len, real, strlen(real));
        len += strlen(real);

        size_t after = pr_probe_strip_echo(buf, len);
        buf[after] = '\0';

        /* The response must survive completely, even with line ends      */
        CHECK(strstr((const char *)buf, "TheFirmware V2.7\r\n") != NULL);
        CHECK(strstr((const char *)buf, "cmd: ") != NULL);
        CHECK(pr_probe_has_content(buf, after));
        CHECK(pr_probe_score(buf, after) > 100);
    }

    printf("\n== banner detection ==\n");
    {
        /* The real criterion for "a TNC speaks here"            */
        const unsigned char *b1 = (const unsigned char *)"TheFirmware NORD V2.7";
        CHECK(pr_probe_has_banner(b1, strlen((const char *)b1)));
        CHECK(pr_probe_score(b1, strlen((const char *)b1)) > 50);

        const unsigned char *b2 = (const unsigned char *)"cmd: ";
        CHECK(pr_probe_has_banner(b2, strlen((const char *)b2)));
        CHECK(pr_probe_score(b2, strlen((const char *)b2)) >= 160);

        /* Garbage is no banner  */
        const unsigned char *g = (const unsigned char *)"\xff\xfe\xfd";
        CHECK(!pr_probe_has_banner(g, 3));

        /* Simple prompt without firmware marker */
        const unsigned char *p = (const unsigned char *)"hello world";
        CHECK(!pr_probe_has_banner(p, strlen((const char *)p)));
    }

    printf("\n== banner behind NUL bytes ==\n");
    {
        /*
         * That was the third bug of the same kind: the reset sequence
         * leaves NULs in front of the banner, and strstr gets stuck on
         * them. Detection and scoring must search via bytes.
         */
        unsigned char buf[128];
        size_t len = 0;
        memset(buf, 0, sizeof buf);
        for (int i = 0; i < 24; i++) buf[len++] = 0x00;   /* Reset residue */
        const char *banner = "* TF2.7b/TNC2 07Jun95 Copyright (C) by NORD *";
        memcpy(buf + len, banner, strlen(banner));
        len += strlen(banner);

        CHECK(pr_probe_has_banner(buf, len));
        CHECK(pr_probe_score(buf, len) > 200);
    }

    printf("\n== short markers in garbage ==\n");
    {
        /*
         * The marker "TNC" is only three chars long. In random
         * garbage it could match and would then count as a banner -
         * including lifting of the printability filter. It may only
         * hit in a printable context.
         */
        unsigned char noise[256];
        for (size_t i = 0; i < sizeof noise; i++)
            noise[i] = (unsigned char)(0x80 + (i % 32));

        /* Three bytes of garbage that happen to form "TNC" */
        noise[100] = 'T'; noise[101] = 'N'; noise[102] = 'C';

        CHECK(!pr_probe_has_banner(noise, sizeof noise));
        CHECK_INT(pr_probe_score(noise, sizeof noise), 0);

        /* The same in a printable context is a real hit              */
        unsigned char real[64];
        memcpy(real, "xx TNC yy", 9);
        CHECK(pr_probe_has_banner(real, 9));
    }

    printf("\n== pattern removal in general ==\n");
    {
        unsigned char b1[] = "abcXXXdefXXX";
        CHECK_INT(pr_probe_remove_bytes(b1, 12, (const unsigned char *)"XXX", 3), 6);

        unsigned char b2[] = "abcdef";
        CHECK_INT(pr_probe_remove_bytes(b2, 6, (const unsigned char *)"X", 1), 6);

        unsigned char b3[] = "aaaa";
        CHECK_INT(pr_probe_remove_bytes(b3, 4, (const unsigned char *)"aa", 2), 0);

        unsigned char b4[] = "abc";
        CHECK_INT(pr_probe_remove_bytes(b4, 3, (const unsigned char *)"", 0), 3);
    }

    printf("\n== empty and whitespace ==\n");
    {
        unsigned char ws[] = { '\r', '\n', ' ', '\t', 0, 0, '\r' };
        CHECK(!pr_probe_has_content(ws, sizeof ws));

        unsigned char one[] = { '\r', 'A', '\n' };
        CHECK(pr_probe_has_content(one, sizeof one));

        CHECK(!pr_probe_has_content(NULL, 0));
    }

    TEST_SUMMARY("probe");
}
