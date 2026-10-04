/*
 * PRTERM - CB & Amateur Radio Terminal
 * probe.c - TNC-Erkennung ueber Profil-Sweep.
 *
 * Wichtige Erfahrungen aus dem Betrieb:
 *
 *   - Ein TNC im Command-Mode SPIEGELT die Eingabe, solange das Echo nicht
 *     ausgeschaltet ist. Ein Echo ist keine Antwort - es beweist aber, dass
 *     Port und Baudrate stimmen.
 *
 *   - Die Rohantwort enthaelt NUL-Bytes. Alles was mit strstr/strlen
 *     arbeitet, haelt dort an. Deshalb wird hier byte-orientiert gearbeitet.
 *
 *   - Bei falscher Baudrate kommt Muell. Muell darf nicht punkten - sonst
 *     gewinnt ein zufaelliges Profil gegen ein sauberes Echo.
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
 * Reihenfolge bewusst: die in der Praxis bewaehrten Profile zuerst.
 *
 * Landolt TNC2C    - Handbuch sagt 7E1, in der Praxis laeuft er bei 19200 8N1
 * PK-TNC2          - 9600 8N1
 * TNC2-Klone allg. - 2400 7E1
 * T-Modem          - 115200 8N1
 */
static const pr_profile profiles[] = {
    { 19200,  8, PR_PAR_NONE, 1, "TNC2C Praxis"    },
    { 19200,  7, PR_PAR_EVEN, 1, "TNC2C Handbuch"  },
    {  9600,  8, PR_PAR_NONE, 1, "PK-TNC2"         },
    {  9600,  7, PR_PAR_EVEN, 1, "9600 7E1"        },
    {  4800,  7, PR_PAR_EVEN, 1, "4800 7E1"        },
    {  2400,  8, PR_PAR_NONE, 1, "2400 8N1"        },
    {  2400,  7, PR_PAR_EVEN, 1, "2400 7E1"        },
    {  1200,  7, PR_PAR_EVEN, 1, "1200 7E1"        },
    { 115200, 8, PR_PAR_NONE, 1, "T-Modem"         },
};

#define PROFILE_COUNT (sizeof profiles / sizeof profiles[0])

/*
 * Die gesendeten Sondierungen - fuer Echo-Erkennung und -Entfernung.
 *
 * WICHTIG: nur LESENDE Kommandos. Zustandsaendernde wie "KISS" oder
 * "MYCALL <x>" duerfen hier nicht stehen - eine Erkennung darf das Geraet
 * nicht umschalten. Ein frueherer Entwurf hat "KISS\r" mitgesendet und das
 * TNC damit in den KISS-Modus versetzt; danach antwortete nichts mehr.
 */
static const char *const probes[] = {
    "\r", "\r\r", "INFO\r", "HELP\r", "?\r"
};
#define PROBE_COUNT (sizeof probes / sizeof probes[0])

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

/* Entfernt alle Vorkommen von needle. Liefert die neue Laenge. */
size_t pr_probe_remove_bytes(unsigned char *buf, size_t len,
                           const unsigned char *needle, size_t nlen)
{
    if (nlen == 0 || len < nlen)
        return len;

    size_t w = 0;
    size_t i = 0;
    while (i < len) {
        if (i + nlen <= len && memcmp(buf + i, needle, nlen) == 0) {
            i += nlen;                  /* Muster ueberspringen */
        } else {
            buf[w++] = buf[i++];
        }
    }
    return w;
}

/*
 * Ein Muster das ausschliesslich aus Zeilenenden besteht, taugt nicht zur
 * Echo-Erkennung: ein einzelnes "\r" ist von der Zeilenstruktur einer
 * echten Antwort nicht zu unterscheiden. Solche Muster werden beim
 * Entfernen uebersprungen - sonst geht Inhalt verloren.
 */
static bool pattern_is_degenerate(const char *pat)
{
    for (const char *p = pat; *p != '\0'; p++) {
        if (*p != '\r' && *p != '\n' && *p != ' ' && *p != '\t')
            return false;               /* hat echten Inhalt */
    }
    return true;
}

/*
 * Entfernt das Echo. Laengere Muster zuerst - sonst zerstoert das kurze
 * Muster "\r" die Markierung in "INFO\r".
 */
size_t pr_probe_strip_echo(unsigned char *buf, size_t len)
{
    size_t order[PROBE_COUNT];
    for (size_t i = 0; i < PROBE_COUNT; i++)
        order[i] = i;

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

/* Anteil druckbarer Zeichen. Muell hat einen sehr niedrigen Wert. */
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

/* Bleibt nach dem Entfernen von CR/LF etwas uebrig? */
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
/* Bewertung                                                               */
/* ======================================================================= */

/*
 * Needles aus den Referenzwerkzeugen: ein TNC antwortet auf einen leeren
 * Prompt mit "cmd:" oder "CMD:" und nennt MYCALL und TXDELAY in der INFO.
 */
int pr_probe_score(const unsigned char *buf, size_t len)
{
    if (len == 0)
        return 0;

    /* Muell darf nicht punkten - sonst gewinnt ein zufaelliges Profil */
    if (pr_probe_printable_ratio(buf, len) < 0.70)
        return 0;

    char txt[1024];
    size_t n = len < sizeof txt - 1 ? len : sizeof txt - 1;
    memcpy(txt, buf, n);
    txt[n] = '\0';
    pr_lower(txt);

    int score = 0;
    if (strstr(txt, "cmd:")     != NULL) score += 200;
    if (strstr(txt, "mycall")   != NULL) score += 120;
    if (strstr(txt, "txdelay")  != NULL) score +=  80;
    if (strstr(txt, "kiss")     != NULL) score +=  40;
    if (strstr(txt, "tnc")      != NULL) score +=  30;
    if (strstr(txt, "help")     != NULL) score +=  20;
    if (strstr(txt, "monitor")  != NULL) score +=  20;
    if (strstr(txt, "connected")!= NULL) score +=  20;
    if (strstr(txt, "version")  != NULL) score +=  15;
    if (strstr(txt, "nord")     != NULL) score +=  25;
    if (strstr(txt, "firmware") != NULL) score +=  25;
    if (strstr(txt, "nocal")    != NULL) score +=  40;

    return score;
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
/* Sweep                                                                   */
/* ======================================================================= */

/*
 * Fuehrt das Geraet in einen bekannten Zustand zurueck.
 *
 * Voraussetzung fuer eine zuverlaessige Erkennung: das TNC darf in einem
 * Zustand stecken, in dem es nicht antwortet - etwa im KISS-Modus oder im
 * Hostmode. Die Reihenfolge folgt der bekannten Ruecksetzleiter:
 *
 *   C0 FF C0          KISS verlassen / Firmware-Ruecksetz
 *   11 18             Puffer leeren (^Q ^X)
 *   300 x 00 + JHOST  Hostmode (WA8DED) verlassen
 *   CR CR             Prompt holen
 */
void pr_probe_reset(pr_serial *s)
{
    char err[128];

    static const unsigned char kiss_return[] = { 0xC0, 0xFF, 0xC0 };
    (void)pr_serial_write(s, kiss_return, sizeof kiss_return, err, sizeof err);
    usleep(400000);

    static const unsigned char buf_flush[] = { 0x11, 0x18 };
    (void)pr_serial_write(s, buf_flush, sizeof buf_flush, err, sizeof err);
    usleep(100000);

    unsigned char nuls[300];
    memset(nuls, 0, sizeof nuls);
    (void)pr_serial_write(s, nuls, sizeof nuls, err, sizeof err);

    static const unsigned char jhost[] = {
        0x00, 0x01, 0x06, 'J', 'H', 'O', 'S', 'T', ' ', '0', '\r'
    };
    (void)pr_serial_write(s, jhost, sizeof jhost, err, sizeof err);
    usleep(300000);

    (void)pr_serial_write(s, "\r\r", 2, err, sizeof err);
    usleep(200000);

    /* Was dabei ankam, ist uninteressant - weg damit */
    pr_serial_flush(s, true, true);
}

static void run_probes(pr_serial *s, unsigned char *answer, size_t *alen)
{
    for (size_t k = 0; k < PROBE_COUNT; k++) {
        char err[128];
        if (pr_serial_write(s, probes[k], strlen(probes[k]), err, sizeof err) != 0)
            break;

        usleep(150000);                 /* dem TNC Zeit zum Reagieren */

        unsigned char tmp[256];
        long n = pr_serial_read_quiet(s, tmp, sizeof tmp, 600, 150,
                                      err, sizeof err);
        if (n > 0 && *alen + (size_t)n < 900) {
            memcpy(answer + *alen, tmp, (size_t)n);
            *alen += (size_t)n;
        }
    }
}

int pr_probe_device(const char *dev, pr_probe_result *best,
                    pr_probe_cb progress, void *ud,
                    char *err, size_t errlen)
{
    memset(best, 0, sizeof *best);
    pr_strlcpy(best->dev, dev, sizeof best->dev);

    int best_score = -1;
    int best_clean = -1;

    say(progress, ud, "Schnittstelle %s wird geprueft", dev);

    for (size_t i = 0; i < PROFILE_COUNT; i++) {
        const pr_profile *p = &profiles[i];

        char pf[32], line[8];
        fmt_line(p->databits, p->parity, p->stopbits, line, sizeof line);
        snprintf(pf, sizeof pf, "%ld %s", p->baud, line);
        say(progress, ud, "  Profil %-14s (%s)", pf, p->name);

        pr_serial s;
        if (pr_serial_open(&s, dev, p->baud, p->databits, p->parity,
                           p->stopbits, true, err, errlen) != 0) {
            say(progress, ud, "    nicht oeffnbar: %s", err);
            continue;
        }

        pr_serial_flush(&s, true, true);

        /* In einen bekannten Zustand - sonst antwortet ein TNC im KISS-
         * oder Hostmode schlicht nicht und wird als "tot" gemeldet. */
        pr_probe_reset(&s);

        unsigned char answer[1024];
        size_t alen = 0;
        run_probes(&s, answer, &alen);
        size_t rawlen = alen;

        /*
         * Echo erkennen. Das ist der normale Zustand im Command-Mode und
         * wird mit ESC E0 (1B 45 30 0D) ausgeschaltet.
         */
        unsigned char probe_copy[1024];
        memcpy(probe_copy, answer, alen);
        size_t stripped = pr_probe_strip_echo(probe_copy, alen);
        bool echo = (rawlen > 0) && !pr_probe_has_content(probe_copy, stripped);

        if (echo) {
            say(progress, ud, "    nur Echo - Echo wird mit ESC E0 ausgeschaltet");

            static const unsigned char esc_e0[] = { 0x1b, 'E', '0', '\r' };
            char e[128];
            if (pr_serial_write(&s, esc_e0, sizeof esc_e0, e, sizeof e) == 0) {
                usleep(300000);
                unsigned char drop[256];
                (void)pr_serial_read_quiet(&s, drop, sizeof drop, 300, 150,
                                           e, sizeof e);
            }
            run_probes(&s, answer, &alen);
            rawlen = alen;
        }

        /* Echo entfernen; was bleibt, ist die echte Antwort */
        alen = pr_probe_strip_echo(answer, alen);

        int sc = pr_probe_score(answer, alen);
        double pr = pr_probe_printable_ratio(answer, alen);
        bool clean = (rawlen > 0) && (echo || pr >= 0.70);

        say(progress, ud, "    %d Bytes roh, %d nach Echo-Entfernung, "
                          "Bewertung %d%s",
            (int)rawlen, (int)alen, sc,
            echo ? " (Echo erkannt)" : "");

        if (pr_probe_has_content(answer, alen)) {
            char show[161];
            size_t n = alen < sizeof show - 1 ? alen : sizeof show - 1;
            memcpy(show, answer, n);
            show[n] = '\0';
            say(progress, ud, "    >> %s", show);
        }

        pr_serial_close(&s);

        /*
         * Rangfolge: echte Antwort > sauberes Echo > Muell > Stille.
         * Ein sauberes Echo bei der richtigen Baudrate ist ein klarer
         * Hinweis - Muell bei falscher Rate darf das nicht ueberstimmen.
         */
        int rank = (sc > 0) ? 3 : (clean && echo) ? 2 : (clean) ? 1 : 0;
        int key = rank * 1000 + (sc > 0 ? sc : 0);

        if (key > best_score) {
            best_score = key;
            best_clean = rank;
            best->baud      = p->baud;
            best->databits  = p->databits;
            best->parity    = p->parity;
            best->stopbits  = p->stopbits;
            best->score     = sc;
            best->echo_only = echo;
            best->clean_link = clean;
            best->responds  = rawlen > 0;
            pr_strlcpy(best->banner, (const char *)answer, sizeof best->banner);
        }
    }

    (void)best_clean;

    if (best_score < 0) {
        snprintf(err, errlen, "%s konnte nicht geoeffnet werden", dev);
        return -1;
    }
    if (!best->responds) {
        snprintf(err, errlen,
                 "%s: keine Antwort in irgendeinem Profil - "
                 "ist das TNC eingeschaltet und korrekt verkabelt?", dev);
        return -1;
    }
    return 0;
}
