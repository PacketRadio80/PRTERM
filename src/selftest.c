/*
 * PRTERM - CB & Amateur Radio Terminal
 * selftest.c - Zustandspruefung der TNCs und Notfall-Ruecksetzung.
 *
 * SPDX-License-Identifier: MIT
 */
#include "prterm_compat.h"

#include "selftest.h"
#include "probe.h"
#include "serial.h"
#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

const char *pr_test_status_name(int status)
{
    switch (status) {
    case PR_TEST_PASS: return "ok";
    case PR_TEST_WARN: return "Hinweis";
    case PR_TEST_FAIL: return "FEHLER";
    case PR_TEST_SKIP: return "uebersprungen";
    default:           return "?";
    }
}

/*
 * Den lesbaren Teil eines Puffers anzeigen.
 *
 * Die Ruecksetzfolge hinterlaesst NULs und BEL vor dem eigentlichen
 * Banner. Diese zu zeigen macht die Ausgabe irrefuehrend - es sah aus
 * wie Muell, obwohl die Firmware sauber erkannt wurde.
 */
static void show_readable(char *dst, size_t dstlen,
                          const unsigned char *src, size_t len)
{
    size_t start = 0;
    while (start < len) {
        unsigned char c = src[start];
        if ((c >= 0x20 && c < 0x7f) || c == '\r' || c == '\n')
            break;
        start++;
    }

    size_t w = 0;
    for (size_t i = start; i < len && w + 1 < dstlen; i++) {
        unsigned char c = src[i];
        if (c == '\r' || c == '\n') {
            dst[w++] = ' ';
        } else if (c >= 0x20 && c < 0x7f) {
            dst[w++] = (char)c;
        } else if (c == 0) {
            size_t k = i;
            while (k < len && src[k] == 0) k++;
            if (k == len) break;
        }
    }
    while (w > 0 && dst[w - 1] == ' ') w--;
    dst[w] = '\0';
}

static void add(pr_selftest *st, const char *name, int status, const char *fmt, ...)
{
    if (st->n >= PR_SELFTEST_MAX)
        return;
    pr_test_result *r = &st->items[st->n++];
    pr_strlcpy(r->name, name, sizeof r->name);
    r->status = status;

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->detail, sizeof r->detail, fmt, ap);
    va_end(ap);

    if (status == PR_TEST_FAIL)
        st->overall_ok = false;
}

/* Zeilenformat aus der Konfiguration lesen */
static bool parse_line_cfg(const pr_config *cfg, int *db, int *par, int *sb)
{
    return pr_serial_parse_line(cfg->serial_line, db, par, sb);
}

/*
 * Kernprüfung: antwortet das Geraet, und welche Firmware steckt darin.
 * Liefert die Firmware-Kennung zurueck, damit Aufrufer sie anzeigen koennen.
 */
static int talk_to_device(const pr_config *cfg, pr_selftest *st, bool *responds)
{
    *responds = false;

    if (!pr_file_exists(cfg->port)) {
        add(st, "Schnittstelle", PR_TEST_FAIL,
            "%s existiert nicht - Kabel, Konverter oder Geraet fehlt?",
            cfg->port);
        return -1;
    }
    add(st, "Schnittstelle", PR_TEST_PASS, "%s", cfg->port);

    int db = 8, par = PR_PAR_NONE, sb = 1;
    if (!parse_line_cfg(cfg, &db, &par, &sb)) {
        add(st, "Zeilenformat", PR_TEST_FAIL,
            "\"%s\" ist nicht lesbar (erwartet 8n1, 7e1, 8e1, 8o1)",
            cfg->serial_line);
        return -1;
    }

    char err[256];
    pr_serial s;
    if (pr_serial_open(&s, cfg->port, cfg->baud, db, par, sb, true,
                       err, sizeof err) != 0) {
        add(st, "Port oeffnen", PR_TEST_FAIL, "%s", err);
        return -1;
    }
    add(st, "Port oeffnen", PR_TEST_PASS, "%ld %s", cfg->baud, cfg->serial_line);

    /* DTR/RTS muessen anliegen - ohne sie antwortet ein TNC2C nicht */
    if (pr_serial_hold_dtr(&s, err, sizeof err) != 0) {
        add(st, "Modemleitungen", PR_TEST_WARN,
            "DTR/RTS nicht setzbar: %s", err);
    } else {
        add(st, "Modemleitungen", PR_TEST_PASS, "DTR und RTS liegen an");
    }

    /*
     * Antwort abfragen. ESC V (1B 56 0D) ist der Probe; die Ruecksetzfolge
     * laesst bei TheFirmware zusaetzlich den Banner erscheinen.
     */
    unsigned char answer[1024];
    /* Ohne Ruecksetzfolge - ein Gesundheits-Check veraendert nichts. */
    size_t alen = 0;

    /*
     * Bewusst NUR ESC V. Ein vorgeschicktes "\r" hat bei TheFirmware
     * dazu gefuehrt, dass die Antwort nicht mehr ankam - der direkte
     * Aufruf mit ESC V allein funktionierte dagegen zuverlaessig.
     */
    static const char *const probes[] = { "\x1b" "V\r" };
    for (size_t i = 0; i < sizeof probes / sizeof probes[0]; i++) {
        (void)pr_serial_write(&s, probes[i], strlen(probes[i]), err, sizeof err);
        usleep(400000);
        unsigned char tmp[512];
        long n = pr_serial_read_quiet(&s, tmp, sizeof tmp, 1500, 300,
                                      err, sizeof err);
        if (n > 0 && alen + (size_t)n < sizeof answer) {
            memcpy(answer + alen, tmp, (size_t)n);
            alen += (size_t)n;
        }
    }

    /* Echo entfernen, dann auswerten */
    alen = pr_probe_strip_echo(answer, alen);

    if (alen == 0) {
        add(st, "Gerät antwortet", PR_TEST_FAIL,
            "keine Antwort auf ESC V - siehe Hinweis zum Port");
        pr_serial_close(&s);
        return -1;
    }
    *responds = true;
    add(st, "Gerät antwortet", PR_TEST_PASS, "%u Byte(s) Antwort", (unsigned)alen);

    if (pr_probe_has_banner(answer, alen)) {
        char show[128];
        show_readable(show, sizeof show, answer, alen);
        pr_strlcpy(st->firmware, show, sizeof st->firmware);
        add(st, "Firmware", PR_TEST_PASS, "%.110s", show);
    } else {
        add(st, "Firmware", PR_TEST_WARN,
            "kein Banner erkannt - Geraet spricht, ist aber unbekannt");
    }

    pr_serial_close(&s);
    return 0;
}

int pr_selftest_run(const pr_config *cfg, pr_selftest *out)
{
    memset(out, 0, sizeof *out);
    out->overall_ok = true;

    add(out, "Treiber", PR_TEST_PASS, "%s", cfg->rig_driver);
    add(out, "CALLERID", callerid_valid(cfg->callerid, &cfg->callsign)
                             ? PR_TEST_PASS : PR_TEST_FAIL,
        "%s", cfg->callerid);

    /* Frequenz und Betriebsart gegen die Zuteilung pruefen */
    {
        char err[256];
        const pr_channel *ch = cfg->bandplan
            ? pr_bandplan_at_freq(cfg->bandplan, cfg->freq_hz) : NULL;
        if (ch == NULL) {
            add(out, "Kanal", PR_TEST_FAIL,
                "%.3f MHz liegt auf keinem zugeteilten Kanal",
                cfg->freq_hz / 1000000.0);
        } else {
            int st2 = pr_bandplan_tx_freq_ok(cfg->bandplan, cfg->freq_hz,
                                             cfg->mode, err, sizeof err)
                        ? PR_TEST_PASS : PR_TEST_FAIL;
            add(out, "Kanal", st2, "Kanal %d auf %.3f MHz",
                ch->num, ch->freq_hz / 1000000.0);
        }
    }

    /* Simulation braucht keine serielle Verbindung */
    if (pr_str_eq_ci(cfg->rig_driver, "sim")) {
        add(out, "Schnittstelle", PR_TEST_SKIP, "Simulation - kein Geraet noetig");
        return out->overall_ok ? 0 : 1;
    }

    add(out, "Funk-Baudrate", PR_TEST_PASS,
        "%ld Baud auf dem Kanal (Modem: %s)",
        cfg->radio_baud, cfg->modem[0] != '\0' ? cfg->modem : "unbekannt");

    bool responds = false;
    (void)talk_to_device(cfg, out, &responds);

    if (responds) {
        add(out, "Gesamt", PR_TEST_PASS, "Geraet ist betriebsbereit");
    } else {
        add(out, "Gesamt", PR_TEST_FAIL,
            "Geraet antwortet nicht - mit --reset-tnc versuchen");
    }

    /* Fehler zaehlen */
    int fails = 0;
    for (size_t i = 0; i < out->n; i++)
        if (out->items[i].status == PR_TEST_FAIL)
            fails++;
    return fails;
}

int pr_selftest_reset(const pr_config *cfg, pr_selftest *out)
{
    memset(out, 0, sizeof *out);
    out->overall_ok = true;

    if (!pr_file_exists(cfg->port)) {
        add(out, "Schnittstelle", PR_TEST_FAIL, "%s existiert nicht", cfg->port);
        return 1;
    }

    int db = 8, par = PR_PAR_NONE, sb = 1;
    (void)parse_line_cfg(cfg, &db, &par, &sb);

    char err[256];
    pr_serial s;
    if (pr_serial_open(&s, cfg->port, cfg->baud, db, par, sb, true,
                       err, sizeof err) != 0) {
        add(out, "Port oeffnen", PR_TEST_FAIL, "%s", err);
        return 1;
    }

    add(out, "Ruecksetzfolge", PR_TEST_PASS,
        "Puffer leeren, Hostmode verlassen, Firmware-Ruecksetz");
    unsigned char answer[1024];
    size_t alen = pr_probe_reset(&s, answer, sizeof answer);

    /* Nach dem Reset muss das Geraet wieder ansprechbar sein */
    static const char *const probes[] = { "\r", "\x1b" "V\r" };
    for (size_t i = 0; i < sizeof probes / sizeof probes[0]; i++) {
        (void)pr_serial_write(&s, probes[i], strlen(probes[i]), err, sizeof err);
        usleep(400000);
        unsigned char tmp[512];
        long n = pr_serial_read_quiet(&s, tmp, sizeof tmp, 1500, 300,
                                      err, sizeof err);
        if (n > 0 && alen + (size_t)n < sizeof answer) {
            memcpy(answer + alen, tmp, (size_t)n);
            alen += (size_t)n;
        }
    }
    alen = pr_probe_strip_echo(answer, alen);

    if (alen == 0) {
        add(out, "Nach dem Reset", PR_TEST_FAIL,
            "immer noch keine Antwort - Netzteil trennen und neu einschalten");
    } else if (pr_probe_has_banner(answer, alen)) {
        char show[128];
        show_readable(show, sizeof show, answer, alen);
        pr_strlcpy(out->firmware, show, sizeof out->firmware);
        add(out, "Nach dem Reset", PR_TEST_PASS, "%.110s", show);
    } else {
        add(out, "Nach dem Reset", PR_TEST_WARN,
            "Geraet spricht, aber ohne Banner");
    }

    pr_serial_close(&s);

    int fails = 0;
    for (size_t i = 0; i < out->n; i++)
        if (out->items[i].status == PR_TEST_FAIL)
            fails++;
    out->overall_ok = (fails == 0);
    return fails;
}

void pr_selftest_print(const pr_selftest *st, FILE *f)
{
    for (size_t i = 0; i < st->n; i++) {
        const pr_test_result *r = &st->items[i];
        const char *mark = (r->status == PR_TEST_PASS) ? " ok  " :
                           (r->status == PR_TEST_WARN) ? " !   " :
                           (r->status == PR_TEST_SKIP) ? " -   " : " FEHL";
        fprintf(f, "  [%s] %-22s %s\n", mark, r->name, r->detail);
    }
    fprintf(f, "\n  %s\n", st->overall_ok ? "Alles in Ordnung."
                                          : "Es gibt Befunde - siehe oben.");
}
