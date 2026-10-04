/*
 * PRTERM - CB & Amateur Radio Terminal
 * probe.c - TNC-Erkennung und Boot-Abfang.
 *
 * Regeln, die aus dem Betrieb mit MAX25-Stack uebernommen sind und die
 * Grundlage dieses Moduls bilden:
 *
 *   1. DEN PORT NICHT SCHLIESSEN. Ein fallendes DTR versetzt einen TNC2C
 *      in einen Echo-only-Zustand, aus dem er nicht mehr antwortet.
 *      Ein frueherer Entwurf hat nach jedem Profil geschlossen und damit
 *      genau dieses Problem erzeugt.
 *
 *   2. DTR/RTS waehrend des Einschaltens HOCH HALTEN. Wer das Geraet neu
 *      startet, muss den Port bereits offen haben.
 *
 *   3. Der Probe heisst ESC V (1B 56 0D), nicht INFO oder HELP.
 *
 *   4. Nur LESENDE Kommandos in der Erkennung. "KISS" oder "MYCALL"
 *      schalten das Geraet um.
 *
 *   5. Echo erkennen und entfernen. Ein TNC im Command-Mode spiegelt die
 *      Eingabe; das ist keine Antwort.
 *
 * SPDX-License-Identifier: MIT
 */
#include "prterm_compat.h"

#include "probe.h"
#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* ======================================================================= */
/* Profile                                                                 */
/* ======================================================================= */

typedef struct pr_profile {
    long baud;
    int  databits;
    int  parity;
    int  stopbits;
    const char *name;
} pr_profile;

/*
 * Landolt TNC2C  : 19200, Handbuch 7E1 - im Feld bestaetigt
 * PK-TNC2        : 9600 8N1
 * TNC2-Klone     : 2400 7E1
 * T-Modem        : 115200 8N1
 */
static const pr_profile profiles[] = {
    { 19200,  7, PR_PAR_EVEN, 1, "TNC2C (Handbuch)" },
    { 19200,  8, PR_PAR_NONE, 1, "TNC2C 8N1"        },
    {  9600,  8, PR_PAR_NONE, 1, "PK-TNC2"          },
    {  9600,  7, PR_PAR_EVEN, 1, "9600 7E1"         },
    {  4800,  7, PR_PAR_EVEN, 1, "4800 7E1"         },
    {  2400,  7, PR_PAR_EVEN, 1, "2400 7E1"         },
    {  2400,  8, PR_PAR_NONE, 1, "2400 8N1"         },
    {  1200,  7, PR_PAR_EVEN, 1, "1200 7E1"         },
    { 115200, 8, PR_PAR_NONE, 1, "T-Modem"          },
};

#define PROFILE_COUNT (sizeof profiles / sizeof profiles[0])

/*
 * Sondierungen - ausschliesslich LESEND.
 *
 * ESC V (1B 56 0D) ist der nativer TheFirmware-Probe. Ein leeres CR holt
 * den Prompt. Alles andere wuerde das Geraet veraendern.
 */
static const char *const probes[] = {
    "\r",
    "\x1b" "V\r",
};
#define PROBE_COUNT (sizeof probes / sizeof probes[0])

/* Banner-Marker aus der Firmware-Erkennung */
static const char *const banner_markers[] = {
    "TheFirmware", "NORD", "Version 2.7", "Checksum", "Copyright",
    "DAMA", "SMACK", "cmd:", "CMD:", "TNC", "WA8DED",
};

static void fmt_line(int databits, int parity, int stopbits,
                     char *dst, size_t dstlen)
{
    snprintf(dst, dstlen, "%d%c%d", databits,
             parity == PR_PAR_EVEN ? 'E' : parity == PR_PAR_ODD ? 'O' : 'N',
             stopbits);
}

void pr_probe_format(const pr_probe_result *r, char *dst, size_t dstlen)
{
    snprintf(dst, dstlen, "%ld %d%c%d", r->baud, r->databits,
             r->parity == PR_PAR_EVEN ? 'E' :
             r->parity == PR_PAR_ODD  ? 'O' : 'N',
             r->stopbits);
}

/* ======================================================================= */
/* Byte-orientierte Echo-Behandlung                                        */
/* ======================================================================= */

size_t pr_probe_remove_bytes(unsigned char *buf, size_t len,
                             const unsigned char *needle, size_t nlen)
{
    if (nlen == 0 || len < nlen)
        return len;

    size_t w = 0, i = 0;
    while (i < len) {
        if (i + nlen <= len && memcmp(buf + i, needle, nlen) == 0)
            i += nlen;
        else
            buf[w++] = buf[i++];
    }
    return w;
}

/*
 * Ein Muster ohne inhaltliche Zeichen taugt nicht zur Echo-Erkennung:
 * ein einzelnes "\r" ist von der Zeilenstruktur einer echten Antwort
 * nicht zu unterscheiden. Solche Muster werden nicht entfernt.
 */
static bool pattern_is_degenerate(const char *pat)
{
    for (const char *p = pat; *p != '\0'; p++) {
        if (*p != '\r' && *p != '\n' && *p != ' ' && *p != '\t')
            return false;
    }
    return true;
}

size_t pr_probe_strip_echo(unsigned char *buf, size_t len)
{
    size_t order[PROBE_COUNT];
    for (size_t i = 0; i < PROBE_COUNT; i++)
        order[i] = i;

    /* laengere Muster zuerst */
    for (size_t i = 0; i + 1 < PROBE_COUNT; i++) {
        for (size_t j = i + 1; j < PROBE_COUNT; j++) {
            if (strlen(probes[order[j]]) > strlen(probes[order[i]])) {
                size_t t = order[i];
                order[i] = order[j];
                order[j] = t;
            }
        }
    }

    for (size_t i = 0; i < PROBE_COUNT; i++) {
        const char *pat = probes[order[i]];
        if (pattern_is_degenerate(pat))
            continue;
        len = pr_probe_remove_bytes(buf, len,
                                    (const unsigned char *)pat, strlen(pat));
    }
    return len;
}

double pr_probe_printable_ratio(const unsigned char *buf, size_t len)
{
    if (len == 0)
        return 0.0;
    size_t ok = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = buf[i];
        if ((c >= 0x20 && c < 0x7f) || c == '\r' || c == '\n' || c == '\t')
            ok++;
    }
    return (double)ok / (double)len;
}

bool pr_probe_has_content(const unsigned char *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        unsigned char c = buf[i];
        if (c != '\r' && c != '\n' && c != '\t' && c != ' ' && c != 0)
            return true;
    }
    return false;
}

/* ======================================================================= */
/* Byte-orientierte Suche                                                  */
/* ======================================================================= */

/*
 * Suche ueber rohe Bytes, die NIEMALS an NUL-Bytes haengenbleibt.
 *
 * Das ist der dritte Fehler derselben Sortie: Antworten enthalten NULs
 * (die Ruecksetzfolge hinterlaesst sie), und strstr/strlen halten dort an.
 * Alles was in einer Antwort sucht, muss ueber memcmp laufen.
 */
static const unsigned char *memfind_ci(const unsigned char *hay, size_t hlen,
                                       const char *needle, size_t nlen)
{
    if (nlen == 0 || nlen > hlen)
        return NULL;

    for (size_t i = 0; i + nlen <= hlen; i++) {
        size_t k = 0;
        for (; k < nlen; k++) {
            unsigned char a = hay[i + k];
            unsigned char b = (unsigned char)needle[k];
            if (a >= 'A' && a <= 'Z') a = (unsigned char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (unsigned char)(b - 'A' + 'a');
            if (a != b)
                break;
        }
        if (k == nlen)
            return hay + i;
    }
    return NULL;
}

/* ======================================================================= */
/* Bewertung                                                               */
/* ======================================================================= */

int pr_probe_score(const unsigned char *buf, size_t len)
{
    if (len == 0)
        return 0;

    /*
     * Ein erkannter Firmware-Banner ist nie Muell, auch wenn die
     * Ruecksetzfolge NUL-Reste davor hinterlaesst. Der Druckbarkeits-
     * Filter gilt nur fuer Antworten OHNE Banner.
     */
    bool banner = pr_probe_has_banner(buf, len);
    if (!banner && pr_probe_printable_ratio(buf, len) < 0.70)
        return 0;

    /* Marker suchen - ueber Bytes, nicht ueber strstr */
    int score = banner ? 200 : 0;
    for (size_t i = 0; i < sizeof banner_markers / sizeof banner_markers[0]; i++) {
        const char *m = banner_markers[i];
        if (memfind_ci(buf, len, m, strlen(m)) != NULL)
            score += 40;
    }
    if (memfind_ci(buf, len, "cmd:", 4)   != NULL) score += 160;
    if (memfind_ci(buf, len, "mycall", 6) != NULL) score += 120;
    if (memfind_ci(buf, len, "txdelay", 7)!= NULL) score +=  80;
    if (memfind_ci(buf, len, "kiss", 4)   != NULL) score +=  40;

    return score;
}

/* Banner erkennen - auch wenn die Bewertung noch niedrig ist */
bool pr_probe_has_banner(const unsigned char *buf, size_t len)
{
    if (buf == NULL || len == 0)
        return false;

    for (size_t i = 0; i < sizeof banner_markers / sizeof banner_markers[0]; i++) {
        const char *m = banner_markers[i];
        if (memfind_ci(buf, len, m, strlen(m)) != NULL)
            return true;
    }
    return false;
}

static void say(pr_probe_cb cb, void *ud, const char *fmt, ...)
{
    if (cb == NULL)
        return;
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    (void)cb(ud, msg);
}

/* ======================================================================= */
/* Ruecksetzfolge - Port bleibt dabei offen                                */
/* ======================================================================= */

/*
 * Reihenfolge aus tnc_serial_recovery.py:
 *
 *   11 18                       Puffer leeren (^Q^X)
 *   300 x 00 + JHOST 0          WA8DED-Hostmode verlassen
 *   C0 FF C0                    KISS verlassen / Firmware-Ruecksetz
 *   1B 56 0D                    ESC V - Probe
 *
 * WICHTIG: waehrend ESC QRES (1B 51 52 45 53 0D) muss DTR hoch bleiben.
 * Deshalb wird der Port hier NIEMALS geschlossen.
 */
size_t pr_probe_reset(pr_serial *s, unsigned char *out, size_t outcap)
{
    char err[128];
    size_t total = 0;

    static const unsigned char buf_flush[] = { 0x11, 0x18 };
    (void)pr_serial_write(s, buf_flush, sizeof buf_flush, err, sizeof err);
    usleep(200000);

    unsigned char nuls[300];
    memset(nuls, 0, sizeof nuls);
    (void)pr_serial_write(s, nuls, sizeof nuls, err, sizeof err);
    usleep(150000);

    static const unsigned char jhost[] = {
        0x00, 0x01, 0x06, 'J', 'H', 'O', 'S', 'T', ' ', '0', '\r'
    };
    (void)pr_serial_write(s, jhost, sizeof jhost, err, sizeof err);
    usleep(800000);

    /* C0 FF C0 setzt die Firmware zurueck und laesst den Banner erscheinen */
    static const unsigned char kiss_return[] = { 0xC0, 0xFF, 0xC0 };
    (void)pr_serial_write(s, kiss_return, sizeof kiss_return, err, sizeof err);

    /*
     * Ausreichend lange warten und alles mitnehmen. Der Reset braucht
     * laut Herleitung rund 2,5 s bis der Banner steht.
     */
    unsigned char tmp[512];
    long n = pr_serial_read_quiet(s, tmp, sizeof tmp, 2500, 600, err, sizeof err);
    if (n > 0 && out != NULL && outcap > 0) {
        size_t k = (size_t)n < outcap ? (size_t)n : outcap;
        memcpy(out, tmp, k);
        total = k;
    }
    return total;
}

/* ======================================================================= */
/* Sondieren                                                               */
/* ======================================================================= */

static size_t run_probes(pr_serial *s, unsigned char *answer, size_t cap)
{
    size_t alen = 0;

    for (size_t k = 0; k < PROBE_COUNT; k++) {
        char err[128];
        if (pr_serial_write(s, probes[k], strlen(probes[k]), err, sizeof err) != 0)
            break;

        usleep(400000);                 /* der Firmware Zeit zum Antworten */

        unsigned char tmp[512];
        long n = pr_serial_read_quiet(s, tmp, sizeof tmp, 1500, 300,
                                      err, sizeof err);
        if (n > 0 && alen + (size_t)n < cap) {
            memcpy(answer + alen, tmp, (size_t)n);
            alen += (size_t)n;
        }
    }
    return alen;
}

/* ======================================================================= */
/* Sweep - Port wird EINMAL geoeffnet und offen gehalten                   */
/* ======================================================================= */

int pr_probe_device(const char *dev, pr_probe_result *best,
                    pr_probe_cb progress, void *ud,
                    char *err, size_t errlen)
{
    memset(best, 0, sizeof *best);
    pr_strlcpy(best->dev, dev, sizeof best->dev);

    /*
     * EINMAL oeffnen. Alle Profile werden danach nur noch umkonfiguriert.
     * Ein Schliessen waehrend der Suche wuerde DTR fallen lassen und das
     * Geraet in den Echo-only-Zustand treiben.
     */
    pr_serial s;
    if (pr_serial_open(&s, dev, 19200, 7, PR_PAR_EVEN, 1, true,
                       err, errlen) != 0)
        return -1;

    say(progress, ud, "Schnittstelle %s geoeffnet - Port bleibt offen", dev);

    int best_score = -1;

    for (size_t i = 0; i < PROFILE_COUNT; i++) {
        const pr_profile *p = &profiles[i];

        char pf[32], line[8];
        fmt_line(p->databits, p->parity, p->stopbits, line, sizeof line);
        snprintf(pf, sizeof pf, "%ld %s", p->baud, line);
        say(progress, ud, "  Profil %-14s (%s)", pf, p->name);

        if (pr_serial_reconfigure(&s, p->baud, p->databits, p->parity,
                                  p->stopbits, err, errlen) != 0) {
            say(progress, ud, "    nicht setzbar: %s", err);
            continue;
        }
        (void)pr_serial_hold_dtr(&s, err, errlen);
        pr_serial_flush(&s, true, true);

        unsigned char answer[1024];
        size_t alen = 0;

        /*
         * Die Ruecksetzfolge loest bei TheFirmware den Boot-Banner aus.
         * Diese Antwort ist die wertvollste Information ueberhaupt - sie
         * wird mitgenommen statt verworfen.
         */
        alen += pr_probe_reset(&s, answer + alen, sizeof answer - alen);
        alen += run_probes(&s, answer + alen, sizeof answer - alen);
        size_t rawlen = alen;

        /* Echo erkennen und entfernen */
        unsigned char probe_copy[1024];
        memcpy(probe_copy, answer, alen);
        size_t stripped = pr_probe_strip_echo(probe_copy, alen);
        bool echo = (rawlen > 0) && !pr_probe_has_content(probe_copy, stripped);

        alen = pr_probe_strip_echo(answer, alen);

        bool banner = pr_probe_has_banner(answer, alen);
        int sc = pr_probe_score(answer, alen);
        double pr = pr_probe_printable_ratio(answer, alen);
        bool clean = (rawlen > 0) && (banner || echo || pr >= 0.70);

        say(progress, ud, "    %u Bytes roh, %u nach Echo-Entfernung, "
                          "Bewertung %d%s%s",
            (unsigned)rawlen, (unsigned)alen, sc,
            echo ? " (Echo)" : "", banner ? " [Banner]" : "");

        if (pr_probe_has_content(answer, alen)) {
            char show[161];
            size_t n = alen < sizeof show - 1 ? alen : sizeof show - 1;
            memcpy(show, answer, n);
            show[n] = '\0';
            say(progress, ud, "    >> %s", show);
        }

        /*
         * Rangfolge: Banner > echte Antwort > sauberes Echo > Muell.
         * Ein sauberes Echo beweist die richtige Baudrate - Muell bei
         * falscher Rate darf das nicht ueberstimmen.
         */
        int rank = banner ? 5 : (sc > 0) ? 4 : echo ? 3 : clean ? 2 : 0;
        int key = rank * 1000 + sc;

        if (key > best_score) {
            best_score = key;
            best->baud       = p->baud;
            best->databits   = p->databits;
            best->parity     = p->parity;
            best->stopbits   = p->stopbits;
            best->score      = sc;
            best->echo_only  = echo;
            best->banner     = banner;
            best->clean_link = clean;
            best->responds   = rawlen > 0;
            pr_strlcpy(best->answer, (const char *)answer, sizeof best->answer);
        }
    }

    /* Port erst am Ende schliessen */
    pr_serial_close(&s);

    if (best_score < 0) {
        snprintf(err, errlen, "%s konnte nicht geoeffnet werden", dev);
        return -1;
    }
    if (!best->responds) {
        snprintf(err, errlen,
                 "%s: keine Antwort in irgendeinem Profil.\n"
                 "  Wichtig: den Port NICHT schliessen, wenn das Geraet neu\n"
                 "  gestartet wird - fallendes DTR versetzt es in einen\n"
                 "  Zustand ohne Antwort. Mit --bootwait waehrend des\n"
                 "  Einschaltens auf den Banner hoeren.", dev);
        return -1;
    }
    return 0;
}

/* ======================================================================= */
/* Boot-Abfang                                                             */
/* ======================================================================= */

int pr_probe_bootwait(const char *dev, long baud, int databits, int parity,
                      int stopbits, int seconds,
                      pr_probe_result *best, pr_probe_cb progress, void *ud,
                      char *err, size_t errlen)
{
    memset(best, 0, sizeof *best);
    pr_strlcpy(best->dev, dev, sizeof best->dev);
    best->baud = baud;

    pr_serial s;
    if (pr_serial_open(&s, dev, baud, databits, parity, stopbits, true,
                       err, errlen) != 0)
        return -1;

    say(progress, ud, "Port %s offen, DTR/RTS liegen an.", dev);
    say(progress, ud, "==> Geraet JETZT einschalten - ich hoere %d s auf den Banner.",
        seconds);

    unsigned char answer[2048];
    size_t alen = 0;

    /* waehrend des Bootens nur LESEN - nichts senden */
    for (int i = 0; i < seconds * 5; i++) {
        unsigned char tmp[256];
        char e[64];
        long n = pr_serial_read(&s, tmp, sizeof tmp, 200, e, sizeof e);
        if (n > 0 && alen + (size_t)n < sizeof answer) {
            memcpy(answer + alen, tmp, (size_t)n);
            alen += (size_t)n;
        }
    }

    if (alen > 0) {
        say(progress, ud, "  %u Bytes beim Boot empfangen", (unsigned)alen);
        char show[161];
        size_t n = alen < sizeof show - 1 ? alen : sizeof show - 1;
        memcpy(show, answer, n);
        show[n] = '\0';
        say(progress, ud, "  >> %s", show);
    } else {
        say(progress, ud, "  kein Banner - versuche ESC V");
    }

    /* Immer auch ESC V probieren, auch wenn kein Banner kam */
    unsigned char more[1024];
    size_t mlen = run_probes(&s, more, sizeof more);
    if (mlen > 0 && alen + mlen < sizeof answer) {
        memcpy(answer + alen, more, mlen);
        alen += mlen;
    }

    unsigned char stripped[2048];
    memcpy(stripped, answer, alen);
    alen = pr_probe_strip_echo(stripped, alen);
    memcpy(answer, stripped, alen);

    best->score      = pr_probe_score(answer, alen);
    best->banner     = pr_probe_has_banner(answer, alen);
    best->responds   = alen > 0;
    best->clean_link = alen > 0;
    best->echo_only  = !pr_probe_has_content(answer, alen);
    pr_strlcpy(best->answer, (const char *)answer, sizeof best->answer);

    /*
     * Der Port bleibt offen, damit DTR nicht faellt. Der Aufrufer
     * entscheidet, wann geschlossen wird.
     */
    best->fd = s.fd;
    return best->responds ? 0 : -1;
}
