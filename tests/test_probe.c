/*
 * PRTERM - Test: TNC-Erkennung, Echo-Behandlung und Bewertung
 *
 * Diese Funktionen haben sich im Betrieb zweimal als falsch erwiesen:
 *   1. kurze Muster haben die Markierung in laengeren zerstoert
 *   2. NUL-Bytes haben das String-basierte Entfernen abgebrochen
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
 * dieselben Sondierungen wie im Produktivcode - ausschliesslich lesende
 * Kommandos, damit die Erkennung das Geraet nicht umschaltet.
 */
static const char *const probes[] = {
    "\r", "\r\r", "INFO\r", "HELP\r", "?\r"
};
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

        /* Klartext muss punkten */
        const unsigned char *good =
            (const unsigned char *)"cmd: MYCALL TNC2C Version 2.7";
        CHECK(pr_probe_printable_ratio(good, strlen((const char *)good)) > 0.95);
        CHECK(pr_probe_score(good, strlen((const char *)good)) > 100);
    }

    printf("\n== Echo entfernen ==\n");
    {
        /* Reines Echo der Sondierungen - exakt das, was der TNC2C liefert */
        unsigned char echo[256];
        size_t len = 0;
        for (size_t i = 0; i < NPROBES; i++) {
            size_t n = strlen(probes[i]);
            memcpy(echo + len, probes[i], n);
            len += n;
        }
        CHECK_INT(len, 15);

        size_t after = pr_probe_strip_echo(echo, len);
        /*
         * Es bleiben nur Zeilenenden uebrig - die Muster ohne Inhalt
         * werden nicht entfernt, weil sie von echter Zeilenstruktur nicht
         * zu unterscheiden sind. Entscheidend ist, dass KEIN Inhalt bleibt.
         */
        CHECK(!pr_probe_has_content(echo, after));
    }

    printf("\n== NUL-Bytes brechen das Entfernen nicht ab ==\n");
    {
        /* Das war der Fehler: strstr/strlen halten bei NUL an. */
        unsigned char buf[256];
        size_t len = 0;
        buf[len++] = 0x00;                     /* fuehrendes ^@ */
        memcpy(buf + len, "INFO\r", 5);  len += 5;
        buf[len++] = 0x00;
        memcpy(buf + len, "HELP\r", 5);  len += 5;

        size_t after = pr_probe_strip_echo(buf, len);
        /* Die beiden NULs bleiben, die Muster verschwinden auch hinter NUL */
        CHECK_INT(after, 2);
        CHECK(!pr_probe_has_content(buf, after));
    }

    printf("\n== Kurze Muster zerstoeren keine laengeren ==\n");
    {
        /* "INFO\\r" muss komplett verschwinden, nicht nur das "\\r". */
        unsigned char buf[64];
        memcpy(buf, "INFO\rHELP\r", 10);
        size_t after = pr_probe_strip_echo(buf, 10);
        CHECK_INT(after, 0);
    }

    printf("\n== Echo + echte Antwort ==\n");
    {
        /* Der TNC spiegelt UND antwortet - die Antwort muss erhalten bleiben. */
        unsigned char buf[256];
        size_t len = 0;
        const char *echo_part = "\r\rINFO\rHELP\r";
        memcpy(buf + len, echo_part, strlen(echo_part));
        len += strlen(echo_part);
        const char *real = "cmd: TNC2C bereit\r";
        memcpy(buf + len, real, strlen(real));
        len += strlen(real);

        size_t after = pr_probe_strip_echo(buf, len);
        buf[after] = '\0';

        /*
         * Die echte Antwort muss VOLLSTAENDIG ueberleben - auch mit ihrem
         * eigenen Zeilenende. Ubrig bleiben koennen Zeilenenden der
         * reinen Zeilen-Muster, die bewusst nicht entfernt werden.
         */
        CHECK(strstr((const char *)buf, "cmd: TNC2C bereit\r") != NULL);
        CHECK(strstr((const char *)buf, "INFO") == NULL);
        CHECK(strstr((const char *)buf, "HELP") == NULL);
        CHECK(pr_probe_has_content(buf, after));
        CHECK(pr_probe_score(buf, after) >= 200);
    }

    printf("\n== Zufaelliger Muell enthaelt Zufallstreffer ==\n");
    {
        /*
         * Wichtig: ein Wort wie "KISS" darf im Muell nicht genug Punkte
         * bringen, um ein sauberes Echo zu schlagen. Der Vergleich im
         * Sweep zieht die Druckbarkeit VOR den Needles heran.
         */
        unsigned char garbage[64];
        for (size_t i = 0; i < sizeof garbage; i++)
            garbage[i] = (unsigned char)(0x80 + (i % 32));
        garbage[0] = 'K'; garbage[1] = 'I'; garbage[2] = 'S'; garbage[3] = 'S';

        CHECK(pr_probe_printable_ratio(garbage, sizeof garbage) < 0.70);
        CHECK_INT(pr_probe_score(garbage, sizeof garbage), 0);
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
