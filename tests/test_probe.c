/*
 * PRTERM - Test: TNC-Erkennung, Echo-Behandlung und Bewertung
 *
 * Diese Funktionen haben sich im Betrieb mehrfach als falsch erwiesen:
 *   1. kurze Muster haben die Markierung in laengeren zerstoert
 *   2. NUL-Bytes haben das String-basierten Entfernen abgebrochen
 *   3. nackte Zeilenenden haben Inhalt der echten Antwort gefressen
 *   4. Muell hat ein sauberes Echo geschlagen
 * Deshalb steht hier ein Test statt nur einem manuellen Durchlauf.
 *
 * SPDX-License-Identifier: MIT
 */
#include "prterm_compat.h"

#include "probe.h"
#include "testutil.h"
#include "util.h"

#include <string.h>

/*
 * Dieselben Sondierungen wie im Produktivcode - ausschliesslich lesend.
 * ESC V ist der nativer TheFirmware-Probe.
 */
static const char *const probes[] = { "\r", "\x1b" "V\r" };
#define NPROBES (sizeof probes / sizeof probes[0])

int main(void)
{
    printf("== Muell erkennen ==\n");
    {
        /* Bei falscher Baudrate kommt Muell - der darf nicht punkten. */
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

    printf("\n== Zufallstreffer in Muell ==\n");
    {
        /* Ein Wort wie KISS darf im Muell nicht ins Gewicht fallen. */
        unsigned char garbage[64];
        for (size_t i = 0; i < sizeof garbage; i++)
            garbage[i] = (unsigned char)(0x80 + (i % 32));
        garbage[0] = 'K'; garbage[1] = 'I'; garbage[2] = 'S'; garbage[3] = 'S';

        CHECK(pr_probe_printable_ratio(garbage, sizeof garbage) < 0.70);
        CHECK_INT(pr_probe_score(garbage, sizeof garbage), 0);
    }

    printf("\n== Echo entfernen ==\n");
    {
        /* Reines Echo der Sondierungen */
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
         * Ubrig bleiben duerfen Zeilenenden - Muster ohne inhaltliche
         * Zeichen werden nicht entfernt. Entscheidend: KEIN Inhalt bleibt.
         */
        CHECK(!pr_probe_has_content(echo, after));
    }

    printf("\n== NUL-Bytes brechen das Entfernen nicht ab ==\n");
    {
        /*
         * Das war der Fehler: strstr/strlen halten bei NUL an. Die
         * Entfernung muss ueber das NUL hinweg arbeiten.
         */
        unsigned char buf[64];
        size_t len = 0;
        buf[len++] = 0x00;                       /* fuehrendes ^@ */
        memcpy(buf + len, "\x1b" "V\r", 3); len += 3;
        buf[len++] = 0x00;
        memcpy(buf + len, "\x1b" "V\r", 3); len += 3;

        size_t after = pr_probe_strip_echo(buf, len);
        /* Die beiden NULs bleiben, die Sondierungen verschwinden */
        CHECK_INT(after, 2);
        CHECK(!pr_probe_has_content(buf, after));
    }

    printf("\n== Kurze Muster zerstoeren keine laengeren ==\n");
    {
        /*
         * Das nackte "\r" wird nicht entfernt (es ist von echter
         * Zeilenstruktur nicht zu unterscheiden) und darf deshalb auch
         * nichts an der laengeren Sondierung zerlegen.
         */
        unsigned char b1[64];
        memcpy(b1, "\x1b" "V\r\x1b" "V\r", 6);
        CHECK_INT(pr_probe_strip_echo(b1, 6), 0);

        /* Allgemein: kurzes Muster darf laengeres nicht aufloesen */
        unsigned char b2[] = "XXYZ";
        CHECK_INT(pr_probe_remove_bytes(b2, 4, (const unsigned char *)"YZ", 2), 2);
    }

    printf("\n== Echo + echte Antwort ==\n");
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

        /* Die Antwort muss vollstaendig ueberleben, auch mit Zeilenenden */
        CHECK(strstr((const char *)buf, "TheFirmware V2.7\r\n") != NULL);
        CHECK(strstr((const char *)buf, "cmd: ") != NULL);
        CHECK(pr_probe_has_content(buf, after));
        CHECK(pr_probe_score(buf, after) > 100);
    }

    printf("\n== Banner-Erkennung ==\n");
    {
        /* Das eigentliche Kriterium fuer "hier spricht ein TNC" */
        const unsigned char *b1 = (const unsigned char *)"TheFirmware NORD V2.7";
        CHECK(pr_probe_has_banner(b1, strlen((const char *)b1)));
        CHECK(pr_probe_score(b1, strlen((const char *)b1)) > 50);

        const unsigned char *b2 = (const unsigned char *)"cmd: ";
        CHECK(pr_probe_has_banner(b2, strlen((const char *)b2)));
        CHECK(pr_probe_score(b2, strlen((const char *)b2)) >= 160);

        /* Muell ist kein Banner */
        const unsigned char *g = (const unsigned char *)"\xff\xfe\xfd";
        CHECK(!pr_probe_has_banner(g, 3));

        /* Einfacher Prompt ohne Firmware-Marker */
        const unsigned char *p = (const unsigned char *)"hello world";
        CHECK(!pr_probe_has_banner(p, strlen((const char *)p)));
    }

    printf("\n== Banner hinter NUL-Bytes ==\n");
    {
        /*
         * Das war der dritte Fehler derselben Sortie: die Ruecksetzfolge
         * hinterlaesst NULs vor dem Banner, und strstr haengt daran fest.
         * Erkennung und Bewertung muessen ueber Bytes suchen.
         */
        unsigned char buf[128];
        size_t len = 0;
        memset(buf, 0, sizeof buf);
        for (int i = 0; i < 24; i++) buf[len++] = 0x00;   /* Reset-Reste */
        const char *banner = "* TF2.7b/TNC2 07Jun95 Copyright (C) by NORD *";
        memcpy(buf + len, banner, strlen(banner));
        len += strlen(banner);

        CHECK(pr_probe_has_banner(buf, len));
        CHECK(pr_probe_score(buf, len) > 200);
    }

    printf("\n== Muster-Entfernung allgemein ==\n");
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

    printf("\n== Leer- und Weissschraum ==\n");
    {
        unsigned char ws[] = { '\r', '\n', ' ', '\t', 0, 0, '\r' };
        CHECK(!pr_probe_has_content(ws, sizeof ws));

        unsigned char one[] = { '\r', 'A', '\n' };
        CHECK(pr_probe_has_content(one, sizeof one));

        CHECK(!pr_probe_has_content(NULL, 0));
    }

    TEST_SUMMARY("probe");
}
