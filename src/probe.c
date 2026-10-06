/*
 * PRTERM - CB & Amateur Radio Terminal
 * probe.c - TNC detection and boot interception.
 *
 * Rules learned the hard way that form the basis of this module:
 *
 *   1. DO NOT CLOSE THE PORT. A falling DTR puts a TNC2C into an
 *      echo-only state where it no longer responds. An earlier draft
 *      closed after every profile and thus produced exactly this
 *      problem.
 *
 *   2. HOLD DTR/RTS HIGH while powering up. Whoever restarts the
 *      device must already have the port open.
 *
 *   3. The probe is called ESC V (1B 56 0D), not INFO or HELP.
 *
 *   4. Only READ commands during detection. "KISS" or "MYCALL"
 *      switch the device over.
 *
 *   5. Detect and remove echo. A TNC in command mode mirrors the
 *      input; that is not a response.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
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
 * Landolt TNC2C  : 19200, manual 7E1 - confirmed in the field
 * PK-TNC2        : 9600 8N1
 * TNC2 clones    : 2400 7E1
 * T-Modem        : 115200 8N1
 */
static const pr_profile profiles[] = {
    { 19200,  7, PR_PAR_EVEN, 1, "TNC2C (manual)"    },
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
 * Probing - exclusively READING.
 *
 * ESC V (1B 56 0D) is the native TheFirmware probe. An empty CR gets
 * the prompt. Anything else would change the device.
 */
static const char *const probes[] = {
    "\r",
    "\x1b" "V\r",
};
#define PROBE_COUNT (sizeof probes / sizeof probes[0])

/* Banner markers from firmware detection   */
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
/* Byte-oriented echo handling                                             */
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
 * A pattern without meaningful characters is useless for echo
 * detection: a single "\r" cannot be told apart from the line
 * structure of a real response. Such patterns are not removed.
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

    /* longer patterns first  */
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
/* Byte-oriented search                                                    */
/* ======================================================================= */

/*
 * Search over raw bytes that NEVER gets stuck on NUL bytes.
 *
 * This is the third bug of the same kind: responses contain NULs (the
 * reset sequence leaves them behind), and strstr/strlen stop there.
 * Anything that searches a response must go through memcmp.
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
/* Scoring                                                                 */
/* ======================================================================= */

int pr_probe_score(const unsigned char *buf, size_t len)
{
    if (len == 0)
        return 0;

    /*
     * A recognized firmware banner is never garbage, even if the reset
     * sequence leaves NUL residue in front of it. The printability
     * filter only applies to responses WITHOUT a banner.
     */
    bool banner = pr_probe_has_banner(buf, len);
    if (!banner && pr_probe_printable_ratio(buf, len) < 0.70)
        return 0;

    /* Search markers - via bytes, not via strstr      */
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

/* Detect banner - even if the score is still low             */
bool pr_probe_has_banner(const unsigned char *buf, size_t len)
{
    if (buf == NULL || len == 0)
        return false;

    /*
     * Search only in PRINTABLE sections.
     *
     * A short marker like "TNC" would match garbage by chance, and
     * exactly that hit would then be counted as a banner - including
     * lifting of the printability filter. So the response is first
     * split into printable pieces and searched only within them.
     */
    size_t i = 0;
    while (i < len) {
        /* Find piece start    */
        while (i < len) {
            unsigned char c = buf[i];
            if ((c >= 0x20 && c < 0x7f) || c == '\r' || c == '\n' || c == '\t')
                break;
            i++;
        }
        size_t start = i;

        /* Find piece end    */
        while (i < len) {
            unsigned char c = buf[i];
            if (!((c >= 0x20 && c < 0x7f) || c == '\r' || c == '\n' || c == '\t'))
                break;
            i++;
        }
        size_t runlen = i - start;
        if (runlen < 3)
            continue;

        for (size_t k = 0; k < sizeof banner_markers / sizeof banner_markers[0]; k++) {
            const char *m = banner_markers[k];
            size_t mlen = strlen(m);

            /*
             * The hit needs CONTEXT and substance.
             *
             *   runlen >= 5          the text piece must be more than
             *                        a few random printable bytes
             *   runlen >  mlen       the marker must not fill the whole
             *                        piece - then it comes from garbage,
             *                        not from a text
             *
             * This way "TNC" in three random printable bytes fails,
             * while a prompt like "cmd: " is still recognized.
             */
            if (runlen < 5 || runlen <= mlen)
                continue;

            if (memfind_ci(buf + start, runlen, m, mlen) != NULL)
                return true;
        }
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
/* Reset sequence - the port stays open                                    */
/* ======================================================================= */

/*
 * Sequence from tnc_serial_recovery.py:
 *
 *   11 18                       flush buffer (^Q^X)
 *   300 x 00 + JHOST 0          leave WA8DED host mode
 *   C0 FF C0                    leave KISS / firmware reset
 *   1B 56 0D                    ESC V - probe
 *
 * IMPORTANT: during ESC QRES (1B 51 52 45 53 0D) DTR must stay high.
 * That is why the port is NEVER closed here.
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

    /* C0 FF C0 resets the firmware and makes the banner appear             */
    static const unsigned char kiss_return[] = { 0xC0, 0xFF, 0xC0 };
    (void)pr_serial_write(s, kiss_return, sizeof kiss_return, err, sizeof err);

    /*
     * Wait long enough and take everything along. According to the
     * derivation the reset takes about 2.5 s until the banner shows.
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
/* Probing                                                                 */
/* ======================================================================= */

static size_t run_probes(pr_serial *s, unsigned char *answer, size_t cap)
{
    size_t alen = 0;

    for (size_t k = 0; k < PROBE_COUNT; k++) {
        char err[128];
        if (pr_serial_write(s, probes[k], strlen(probes[k]), err, sizeof err) != 0)
            break;

        usleep(400000);                 /* give the firmware time to respond */

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
/* Sweep - the port is opened ONCE and kept open                           */
/* ======================================================================= */

int pr_probe_device(const char *dev, pr_probe_result *best,
                    pr_probe_cb progress, void *ud,
                    char *err, size_t errlen)
{
    memset(best, 0, sizeof *best);
    pr_strlcpy(best->dev, dev, sizeof best->dev);

    /*
     * Open ONCE. All profiles are only reconfigured afterwards.
     * Closing during the sweep would drop DTR and drive the device
     * into the echo-only state.
     */
    pr_serial s;
    if (pr_serial_open(&s, dev, 19200, 7, PR_PAR_EVEN, 1, true,
                       err, errlen) != 0)
        return -1;

    say(progress, ud, "Interface %s opened - port stays open", dev);

    int best_score = -1;

    for (size_t i = 0; i < PROFILE_COUNT; i++) {
        const pr_profile *p = &profiles[i];

        char pf[32], line[8];
        fmt_line(p->databits, p->parity, p->stopbits, line, sizeof line);
        snprintf(pf, sizeof pf, "%ld %s", p->baud, line);
        say(progress, ud, "  Profile %-14s (%s)", pf, p->name);

        if (pr_serial_reconfigure(&s, p->baud, p->databits, p->parity,
                                  p->stopbits, err, errlen) != 0) {
            say(progress, ud, "    not settable: %s", err);
            continue;
        }
        (void)pr_serial_hold_dtr(&s, err, errlen);
        pr_serial_flush(&s, true, true);

        unsigned char answer[1024];
        size_t alen = 0;

        /*
         * The reset sequence triggers the boot banner on TheFirmware.
         * This response is the most valuable information of all - it
         * is taken along instead of discarded.
         */
        alen += pr_probe_reset(&s, answer + alen, sizeof answer - alen);
        alen += run_probes(&s, answer + alen, sizeof answer - alen);
        size_t rawlen = alen;

        /* Detect and remove echo      */
        unsigned char probe_copy[1024];
        memcpy(probe_copy, answer, alen);
        size_t stripped = pr_probe_strip_echo(probe_copy, alen);
        bool echo = (rawlen > 0) && !pr_probe_has_content(probe_copy, stripped);

        alen = pr_probe_strip_echo(answer, alen);

        bool banner = pr_probe_has_banner(answer, alen);
        int sc = pr_probe_score(answer, alen);
        double pr = pr_probe_printable_ratio(answer, alen);
        bool clean = (rawlen > 0) && (banner || echo || pr >= 0.70);

        say(progress, ud, "    %u bytes raw, %u after echo removal, "
                          "score %d%s%s",
            (unsigned)rawlen, (unsigned)alen, sc,
            echo ? " (echo)" : "", banner ? " [banner]" : "");

        if (pr_probe_has_content(answer, alen)) {
            char show[161];
            size_t n = alen < sizeof show - 1 ? alen : sizeof show - 1;
            memcpy(show, answer, n);
            show[n] = '\0';
            say(progress, ud, "    >> %s", show);
        }

        /*
         * Ranking: banner > real response > clean echo > garbage.
         * A clean echo proves the right baud rate - garbage at the
         * wrong rate must not outvote that.
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

    /* Close the port only at the end */
    pr_serial_close(&s);

    if (best_score < 0) {
        snprintf(err, errlen, "%s could not be opened", dev);
        return -1;
    }
    if (!best->responds) {
        snprintf(err, errlen,
                 "%s: no reply in any profile.\n"
                 "  Important: do NOT close the port while the device is\n"
                 "  restarted - falling DTR puts it into a state without\n"
                 "  reply. Use --bootwait to listen for the banner while\n"
                 "  switching on.", dev);
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

    say(progress, ud, "Port %s open, DTR/RTS asserted.", dev);
    say(progress, ud, "==> Switch the device on NOW - listening %d s for the banner.",
        seconds);

    unsigned char answer[2048];
    size_t alen = 0;

    /* only READ during boot - send nothing           */
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
        say(progress, ud, "  %u bytes received at boot", (unsigned)alen);
        char show[161];
        size_t n = alen < sizeof show - 1 ? alen : sizeof show - 1;
        memcpy(show, answer, n);
        show[n] = '\0';
        say(progress, ud, "  >> %s", show);
    } else {
        say(progress, ud, "  no banner - trying ESC V");
    }

    /* Always try ESC V too, even if no banner came          */
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
     * The port stays open so DTR does not fall. The caller decides
     * when to close.
     */
    best->fd = s.fd;
    return best->responds ? 0 : -1;
}
