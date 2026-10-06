/*
 * PRTERM - CB & Amateur Radio Terminal
 * selftest.c - TNC health check and emergency reset.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
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
    case PR_TEST_WARN: return "warn";
    case PR_TEST_FAIL: return "error";
    case PR_TEST_SKIP: return "skipped";
    default:           return "?";
    }
}

/*
 * Show the readable part of a buffer.
 *
 * The reset sequence leaves NULs and BEL in front of the actual
 * banner. Showing them makes the output misleading - it looked like
 * garbage although the firmware was cleanly detected.
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

/* Read the line format from the configuration */
static bool parse_line_cfg(const pr_config *cfg, int *db, int *par, int *sb)
{
    return pr_serial_parse_line(cfg->serial_line, db, par, sb);
}

/*
 * Core check: does the device respond, and which firmware is inside.
 * Returns the firmware ID so callers can display it.
 */
static int talk_to_device(const pr_config *cfg, pr_selftest *st, bool *responds)
{
    *responds = false;

    if (!pr_file_exists(cfg->port)) {
        add(st, "Interface", PR_TEST_FAIL,
            "%s does not exist - cable, converter or device missing?",
            cfg->port);
        return -1;
    }
    add(st, "Interface", PR_TEST_PASS, "%s", cfg->port);

    int db = 8, par = PR_PAR_NONE, sb = 1;
    if (!parse_line_cfg(cfg, &db, &par, &sb)) {
        add(st, "Line format", PR_TEST_FAIL,
            "\"%s\" is not readable (expected 8n1, 7e1, 8e1, 8o1)",
            cfg->serial_line);
        return -1;
    }

    char err[256];
    pr_serial s;
    if (pr_serial_open(&s, cfg->port, cfg->baud, db, par, sb, true,
                       err, sizeof err) != 0) {
        add(st, "Open port", PR_TEST_FAIL, "%s", err);
        return -1;
    }
    add(st, "Open port", PR_TEST_PASS, "%ld %s", cfg->baud, cfg->serial_line);

    /* DTR/RTS must be asserted - without them a TNC2C does not respond */
    if (pr_serial_hold_dtr(&s, err, sizeof err) != 0) {
        add(st, "Modem lines", PR_TEST_WARN,
            "DTR/RTS not settable: %s", err);
    } else {
        add(st, "Modem lines", PR_TEST_PASS, "DTR and RTS asserted");
    }

    /*
     * Query the response. ESC V (1B 56 0D) is the probe; the reset
     * sequence additionally makes the banner appear on TheFirmware.
     */
    unsigned char answer[1024];
    /* Without reset sequence - a health check changes nothing.       */
    size_t alen = 0;

    /*
     * Deliberately ONLY ESC V. A leading "\r" made TheFirmware stop
     * responding - the direct call with ESC V alone worked reliably
     * in contrast.
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

    /* Remove echo, then evaluate     */
    alen = pr_probe_strip_echo(answer, alen);

    if (alen == 0) {
        add(st, "Device answers", PR_TEST_FAIL,
            "no reply to ESC V - see the note on the port");
        pr_serial_close(&s);
        return -1;
    }
    *responds = true;
    add(st, "Device answers", PR_TEST_PASS, "%u byte(s) reply", (unsigned)alen);

    if (pr_probe_has_banner(answer, alen)) {
        char show[128];
        show_readable(show, sizeof show, answer, alen);
        pr_strlcpy(st->firmware, show, sizeof st->firmware);
        add(st, "Firmware", PR_TEST_PASS, "%.110s", show);
    } else {
        add(st, "Firmware", PR_TEST_WARN,
            "no banner detected - device talks but is unknown");
    }

    pr_serial_close(&s);
    return 0;
}

int pr_selftest_run(const pr_config *cfg, pr_selftest *out)
{
    memset(out, 0, sizeof *out);
    out->overall_ok = true;

    add(out, "Driver", PR_TEST_PASS, "%s", cfg->rig_driver);
    add(out, "CALLERID", callerid_valid(cfg->callerid, &cfg->callsign)
                             ? PR_TEST_PASS : PR_TEST_FAIL,
        "%s", cfg->callerid);

    /* Check frequency and mode against the allocation      */
    {
        char err[256];
        const pr_channel *ch = cfg->bandplan
            ? pr_bandplan_at_freq(cfg->bandplan, cfg->freq_hz) : NULL;
        if (ch == NULL) {
            add(out, "Channel", PR_TEST_FAIL,
                "%.3f MHz is not on an allocated channel",
                cfg->freq_hz / 1000000.0);
        } else {
            int st2 = pr_bandplan_tx_freq_ok(cfg->bandplan, cfg->freq_hz,
                                             cfg->mode, err, sizeof err)
                        ? PR_TEST_PASS : PR_TEST_FAIL;
            add(out, "Channel", st2, "channel %d on %.3f MHz",
                ch->num, ch->freq_hz / 1000000.0);
        }
    }

    /* The simulation needs no serial connection    */
    if (pr_str_eq_ci(cfg->rig_driver, "sim")) {
        add(out, "Interface", PR_TEST_SKIP, "simulation - no device needed");
        return out->overall_ok ? 0 : 1;
    }

    add(out, "Radio baud", PR_TEST_PASS,
        "%ld baud on the channel (modem: %s)",
        cfg->radio_baud, cfg->modem[0] != '\0' ? cfg->modem : "unknown");

    bool responds = false;
    (void)talk_to_device(cfg, out, &responds);

    if (responds) {
        add(out, "Overall", PR_TEST_PASS, "device is operational");
    } else {
        add(out, "Overall", PR_TEST_FAIL,
            "device does not answer - try --reset-tnc");
    }

    /* Count errors   */
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
        add(out, "Interface", PR_TEST_FAIL, "%s does not exist", cfg->port);
        return 1;
    }

    int db = 8, par = PR_PAR_NONE, sb = 1;
    (void)parse_line_cfg(cfg, &db, &par, &sb);

    char err[256];
    pr_serial s;
    if (pr_serial_open(&s, cfg->port, cfg->baud, db, par, sb, true,
                       err, sizeof err) != 0) {
        add(out, "Open port", PR_TEST_FAIL, "%s", err);
        return 1;
    }

    add(out, "Reset sequence", PR_TEST_PASS,
        "flush buffer, leave host mode, firmware reset");
    unsigned char answer[1024];
    size_t alen = pr_probe_reset(&s, answer, sizeof answer);

    /* After the reset the device must respond again          */
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
        add(out, "After reset", PR_TEST_FAIL,
            "still no reply - disconnect power and switch on again");
    } else if (pr_probe_has_banner(answer, alen)) {
        char show[128];
        show_readable(show, sizeof show, answer, alen);
        pr_strlcpy(out->firmware, show, sizeof out->firmware);
        add(out, "After reset", PR_TEST_PASS, "%.110s", show);
    } else {
        add(out, "After reset", PR_TEST_WARN,
            "device talks, but without banner");
    }

    pr_serial_close(&s);

    int fails = 0;
    for (size_t i = 0; i < out->n; i++)
        if (out->items[i].status == PR_TEST_FAIL)
            fails++;
    out->overall_ok = (fails == 0);
    return fails;
}


/*
 * Recovery: bring the device back into KISS mode and delete pending
 * data in memory.
 *
 * IMPORTANT about the order: as long as a TNC is in KISS mode, EVERY
 * written byte is transmitted. So anyone who "just takes a quick look"
 * transmits while doing so. That is why KISS is left first - it is a
 * control frame and does NOT go on the air. Only then one may send
 * commands.
 */
int pr_checkup(const pr_config *cfg, pr_selftest *out)
{
    char err[256];
    memset(out, 0, sizeof *out);

    int db, par, sb;
    if (!parse_line_cfg(cfg, &db, &par, &sb)) {
        add(out, "Line format", PR_TEST_FAIL,
            "line \"%s\" is invalid", cfg->serial_line);
        out->overall_ok = false;
        return 1;
    }

    pr_serial ser;
    if (pr_serial_open(&ser, cfg->port, cfg->baud, db, par, sb, true,
                       err, sizeof err) != 0) {
        add(out, "Open port", PR_TEST_FAIL, "%.180s", err);
        out->overall_ok = false;
        return 1;
    }
    add(out, "Open port", PR_TEST_PASS, "%ld %s", cfg->baud, cfg->serial_line);

    /* 1. Leave KISS - control frame, sends nothing      */
    {
        static const unsigned char leave[] = { 0xC0, 0xFF, 0xC0 };
        (void)pr_serial_write(&ser, leave, sizeof leave, err, sizeof err);
        /*
         * The device needs time afterwards. The too short wait was the
         * reason why the following probe got no response.
         */
        usleep(1500000);
    }

    /* 2. + 3. Proven reset sequence. Now we are in command mode,
     * writing sends nothing. The sequence empties the buffer and
     * confirms itself via the banner. */
    unsigned char answer[1024];
    size_t alen = pr_probe_reset(&ser, answer, sizeof answer);

    if (alen == 0) {
        add(out, "Command mode", PR_TEST_FAIL,
            "device does not answer - check port, baud rate or device");
        pr_serial_close(&ser);
        out->overall_ok = false;
        return 1;
    }
    {
        char show[128];
        show_readable(show, sizeof show, answer, alen);
        pr_strlcpy(out->firmware, show, sizeof out->firmware);
        add(out, "Memory", PR_TEST_PASS,
            "buffer flushed - no unconfirmed data left");
        add(out, "Command mode", PR_TEST_PASS, "%.110s", show);
    }

    /* 4. Enter KISS - per profile       */
    {
        char e2[128];
        if (pr_str_eq_ci(cfg->kiss_init, "tapr")) {
            static const unsigned char kiss_on[] = {
                'k','i','s','s',' ','o','n','\r'
            };
            (void)pr_serial_write(&ser, kiss_on, sizeof kiss_on, e2, sizeof e2);
            add(out, "KISS entry", PR_TEST_PASS, "kiss on (TAPR)");
        } else {
            static const unsigned char kiss_on[] = { 0x1B, 0x40, 0x4B };
            (void)pr_serial_write(&ser, kiss_on, sizeof kiss_on, e2, sizeof e2);
            add(out, "KISS entry", PR_TEST_PASS, "ESC @K");
        }
        usleep(250000);

        /* Discard leftovers from the switching */
        unsigned char junk[256];
        (void)pr_serial_read_quiet(&ser, junk, sizeof junk, 250, 100,
                                   e2, sizeof e2);
    }

    add(out, "Memory", PR_TEST_PASS,
        "buffer flushed - no unconfirmed data left");
    add(out, "Overall", PR_TEST_PASS,
        "device is in KISS mode and operational");

    pr_serial_close(&ser);
    out->overall_ok = true;
    return 0;
}

void pr_selftest_print(const pr_selftest *st, FILE *f)
{
    for (size_t i = 0; i < st->n; i++) {
        const pr_test_result *r = &st->items[i];
        const char *mark = (r->status == PR_TEST_PASS) ? " ok  " :
                           (r->status == PR_TEST_WARN) ? " !   " :
                           (r->status == PR_TEST_SKIP) ? " -   " : " FAIL";
        fprintf(f, "  [%s] %-22s %s\n", mark, r->name, r->detail);
    }
    fprintf(f, "\n  %s\n", st->overall_ok ? "All in order."
                                          : "There are findings - see above.");
}
