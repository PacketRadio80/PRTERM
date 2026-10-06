/*
 * PRTERM - shell tool
 * tnc_probe.c - find the TNC, determine the profile, look at raw data.
 *
 *   prterm-probe [DEVICE]                 profile sweep (default /dev/ttyUSB0)
 *   prterm-probe --all                    query all serial nodes
 *   prterm-probe --dump DEVICE BAUD LINE  look at a connection (hexdump)
 *   prterm-probe --raw DEVICE BAUD LINE   only listen, send nothing
 *
 * ONLY serial talking happens here - no PTT, no transmission.
 * The sweep sends exclusively reading commands.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "prterm_compat.h"

#include "probe.h"
#include "serial.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int printer(void *ud, const char *msg)
{
    (void)ud;
    printf("%s\n", msg);
    return 0;
}

/* "8N1" / "7E1" -> data bits, parity, stop bits  */
static int parse_line(const char *s, int *databits, int *parity, int *stopbits)
{
    if (s == NULL || strlen(s) != 3)
        return -1;
    if (s[0] != '7' && s[0] != '8')
        return -1;
    *databits = s[0] - '0';
    *parity = (s[1] == 'E') ? PR_PAR_EVEN :
              (s[1] == 'O') ? PR_PAR_ODD  : PR_PAR_NONE;
    *stopbits = s[2] - '0';
    return 0;
}

static void hexdump(const unsigned char *buf, size_t len)
{
    for (size_t i = 0; i < len; i += 16) {
        printf("    %04zx  ", i);
        for (size_t k = 0; k < 16; k++) {
            if (i + k < len) printf("%02x ", buf[i + k]);
            else             printf("   ");
        }
        printf(" |");
        for (size_t k = 0; k < 16 && i + k < len; k++) {
            unsigned char c = buf[i + k];
            putchar((c >= 0x20 && c < 0x7f) ? (int)c : '.');
        }
        printf("|\n");
    }
}

/* Sends an exactly specified byte sequence (hex) and shows the response. */
static int do_send(const char *dev, long baud, const char *line, const char *hexstr)
{
    int databits = 8, parity = PR_PAR_NONE, stopbits = 1;
    if (parse_line(line, &databits, &parity, &stopbits) != 0) {
        fprintf(stderr, "line format \"%s\" not understood (e.g. 8N1)\n", line);
        return 2;
    }

    unsigned char out[512];
    size_t olen = 0;
    const char *p = hexstr;
    while (*p != '\0' && olen < sizeof out) {
        while (*p == ' ' || *p == ',' || *p == ':') p++;
        if (*p == '\0') break;
        int hi = -1, lo = -1;
        if (sscanf(p, "%1x%1x", &hi, &lo) != 2 || lo < 0) {
            fprintf(stderr, "invalid hex at \"%s\"\n", p);
            return 2;
        }
        out[olen++] = (unsigned char)((hi << 4) | lo);
        p += 2;
    }

    char err[256];
    pr_serial s;
    if (pr_serial_open(&s, dev, baud, databits, parity, stopbits, true,
                       err, sizeof err) != 0) {
        fprintf(stderr, "ERROR: %s\n", err);
        return 1;
    }

    printf("Port %s open: %ld %s\n", dev, baud, line);
    printf("-> sending %u byte(s):", (unsigned)olen);
    for (size_t i = 0; i < olen; i++)
        printf(" %02x", out[i]);
    printf("\n\n");

    if (pr_serial_write(&s, out, olen, err, sizeof err) != 0) {
        fprintf(stderr, "ERROR: %s\n", err);
        pr_serial_close(&s);
        return 1;
    }

    printf("-> receiving (4 s):\n\n");
    unsigned char all[4096];
    size_t total = 0;
    for (int i = 0; i < 20; i++) {
        unsigned char chunk[256];
        long n = pr_serial_read(&s, chunk, sizeof chunk, 200, err, sizeof err);
        if (n > 0 && total + (size_t)n < sizeof all) {
            memcpy(all + total, chunk, (size_t)n);
            total += (size_t)n;
        }
    }

    if (total == 0) {
        printf("        (nothing received)\n");
    } else {
        printf("    %u bytes:\n\n", (unsigned)total);
        hexdump(all, total);
        printf("\n    as text:\n    |");
        for (size_t i = 0; i < total; i++) {
            unsigned char c = all[i];
            if (c == '\r')      printf("\\r");
            else if (c == '\n') printf("\\n");
            else if (c == 0)    printf("\\0");
            else if (c >= 0x20 && c < 0x7f) putchar((int)c);
            else                printf(".");
        }
        printf("|\n");
    }

    pr_serial_close(&s);
    return 0;
}

/*
 * Mode:
 *   quiet  only listen, send nothing
 *   probe  send reading commands (default)
 *   reset  reset sequence first, then reading commands
 */
static int do_dump(const char *dev, long baud, const char *line, const char *mode, int seconds)
{
    int databits = 8, parity = PR_PAR_NONE, stopbits = 1;
    if (parse_line(line, &databits, &parity, &stopbits) != 0) {
        fprintf(stderr, "line format \"%s\" not understood (e.g. 8N1)\n", line);
        return 2;
    }

    char err[256];
    pr_serial s;
    if (pr_serial_open(&s, dev, baud, databits, parity, stopbits, true,
                       err, sizeof err) != 0) {
        fprintf(stderr, "ERROR: %s\n", err);
        return 1;
    }

    printf("Port %s open: %ld %s  (mode: %s)\n\n", dev, baud, line, mode);

    if (strcmp(mode, "reset") == 0) {
        printf("-> reset sequence (leave KISS/host mode)\n");
        pr_probe_reset(&s, NULL, 0);
    }

    if (strcmp(mode, "quiet") != 0) {
        printf("-> read-only probes\n");
        static const char *const cmds[] = { "\r", "INFO\r", "HELP\r", "?\r" };
        size_t ncmd = (strcmp(mode, "reset") == 0)
                        ? (sizeof cmds / sizeof cmds[0]) : 1;
        for (size_t i = 0; i < ncmd; i++) {
            char e[64];
            printf("   sending");
            for (const char *p = cmds[i]; *p != '\0'; p++)
                printf(" %02x", (unsigned char)*p);
            printf("\n");
            pr_serial_write(&s, cmds[i], strlen(cmds[i]), e, sizeof e);
            usleep(250000);
        }
    }

    printf("\n-> receiving (%d s):\n\n", seconds);
    unsigned char all[4096];
    size_t total = 0;

    for (int i = 0; i < seconds * 5; i++) {
        unsigned char chunk[256];
        char e[64];
        long n = pr_serial_read(&s, chunk, sizeof chunk, 200, e, sizeof e);
        if (n > 0 && total + (size_t)n < sizeof all) {
            memcpy(all + total, chunk, (size_t)n);
            total += (size_t)n;
        }
    }

    if (total == 0) {
        printf("        (nothing received)\n");
    } else {
        printf("    %u bytes received:\n\n", (unsigned)total);
        hexdump(all, total);

        printf("\n        as text:\n    |");
        for (size_t i = 0; i < total; i++) {
            unsigned char c = all[i];
            if (c == '\r')      printf("\\r");
            else if (c == '\n') printf("\\n");
            else if (c == '\t') printf("\\t");
            else if (c == 0)    printf("\\0");
            else if (c >= 0x20 && c < 0x7f) putchar((int)c);
            else                printf(".");
        }
        printf("|\n");
    }

    pr_serial_close(&s);
    return total > 0 ? 0 : 1;
}

static int probe_one(const char *dev, pr_probe_result *best,
                     char *err, size_t errlen)
{
    printf("\n--- %s ---\n", dev);
    if (pr_probe_device(dev, best, printer, NULL, err, errlen) != 0) {
        printf("Result: %s\n", err);
        return -1;
    }

    char pf[32];
    pr_probe_format(best, pf, sizeof pf);

    printf("\n  RESULT for %s\n", dev);
    printf("    Profile : %s\n", pf);
    printf("    Score   : %d\n", best->score);
    printf("    Banner  : %s\n", best->banner ? "yes" : "no");
    printf("    Echo    : %s\n", best->echo_only ? "echo only (echo was on)" : "no");
    printf("    Link    : %s\n", best->clean_link ? "clean" : "unreliable");
    if (best->answer[0] != '\0')
        printf("    Reply   : %.160s\n", best->answer);

    printf("\n  Entry for prterm.ini:\n");
    printf("    port  = %s\n", dev);
    printf("    baud  = %ld\n", best->baud);
    return 0;
}

/*
 * Boot interception. The port is opened and stays open - DTR/RTS are
 * asserted while the device is powered on. That is exactly the point
 * that makes the difference between "responds" and "stays silent".
 */
static int do_bootwait(const char *dev, long baud, const char *line, int seconds)
{
    int databits = 8, parity = PR_PAR_NONE, stopbits = 1;
    if (parse_line(line, &databits, &parity, &stopbits) != 0) {
        fprintf(stderr, "line format \"%s\" not understood (e.g. 7E1)\n", line);
        return 2;
    }

    char err[256];
    pr_probe_result best;

    printf("\n*** SWITCH THE DEVICE ON NOW ***\n\n");
    if (pr_probe_bootwait(dev, baud, databits, parity, stopbits, seconds,
                          &best, printer, NULL, err, sizeof err) != 0) {
        printf("\nResult: %s\n", err);
        if (best.fd >= 0) close(best.fd);
        return 1;
    }

    printf("\n  RESULT\n");
    printf("    Banner  : %s\n", best.banner ? "yes" : "no");
    printf("    Score   : %d\n", best.score);
    printf("    Reply   : %.200s\n", best.answer);

    printf("\n  The port stays open so DTR does not fall.\n");
    printf("  Press ENTER to close\n");
    (void)getchar();

    if (best.fd >= 0) close(best.fd);
    return 0;
}

int main(int argc, char **argv)
{
    char err[256];

    if (argc < 2 || strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        printf("prterm-probe - detect and inspect the TNC\n\n");
        printf("  prterm-probe [DEVICE]                  profile sweep\n");
        printf("  prterm-probe --all                     query all nodes\n");
        printf("  prterm-probe --dump DEVICE BAUD LINE [MODE]\n");
        printf("  prterm-probe --raw  DEVICE BAUD LINE\n\n");
        printf("Mode for --dump:\n");
        printf("  probe   send read-only commands (default)\n");
        printf("  quiet   only listen\n");
        printf("  reset   reset sequence, then commands\n\n");
        printf("Examples:\n");
        printf("  prterm-probe /dev/ttyUSB0\n");
        printf("  prterm-probe --dump /dev/ttyUSB0 19200 8N1\n\n");
        printf("Sends only serial commands, no radio transmission.\n");
        return argc < 2 ? 2 : 0;
    }

    if (strcmp(argv[1], "--dump") == 0 || strcmp(argv[1], "--raw") == 0) {
        if (argc < 5) {
            fprintf(stderr, "Usage: prterm-probe %s DEVICE BAUD LINE [MODE]\n",
                    argv[1]);
            return 2;
        }
        const char *mode;
        if (strcmp(argv[1], "--raw") == 0)      mode = "quiet";
        else if (argc > 5)                      mode = argv[5];
        else                                    mode = "probe";

        if (strcmp(mode, "probe") != 0 && strcmp(mode, "quiet") != 0 &&
            strcmp(mode, "reset") != 0) {
            fprintf(stderr, "mode must be probe, quiet or reset\n");
            return 2;
        }
        int secs = (argc > 6) ? atoi(argv[6]) : 5;
        return do_dump(argv[2], atol(argv[3]), argv[4], mode, secs);
    }

    if (strcmp(argv[1], "--bootwait") == 0) {
        if (argc < 5) {
            fprintf(stderr, "Usage: prterm-probe --bootwait DEVICE BAUD LINE [SEC]\n"
                            "Open the port, assert DTR, THEN switch the device on.\n");
            return 2;
        }
        int secs = (argc > 5) ? atoi(argv[5]) : 45;
        return do_bootwait(argv[2], atol(argv[3]), argv[4], secs);
    }

    if (strcmp(argv[1], "--send") == 0) {
        if (argc < 6) {
            fprintf(stderr,
                    "Usage: prterm-probe --send DEVICE BAUD LINE HEX\n"
                    "e.g.   prterm-probe --send /dev/ttyUSB0 19200 8N1 \"c0 ff c0\"\n");
            return 2;
        }
        return do_send(argv[2], atol(argv[3]), argv[4], argv[5]);
    }

    if (strcmp(argv[1], "--all") == 0) {
        static const char *const devs[] = {
            "/dev/ttyUSB0", "/dev/ttyUSB1", "/dev/ttyUSB2", "/dev/ttyUSB3",
            "/dev/ttyACM0", "/dev/ttyACM1",
            "/dev/cuaU0",   "/dev/cuaU1",
        };
        int found = 0;
        for (size_t i = 0; i < sizeof devs / sizeof devs[0]; i++) {
            if (!pr_file_exists(devs[i]))
                continue;
            pr_probe_result best;
            if (probe_one(devs[i], &best, err, sizeof err) == 0)
                found++;
        }
        printf("\n%d device(s) with reply\n", found);
        return found > 0 ? 0 : 1;
    }

    const char *dev = argv[1];
    pr_probe_result best;
    if (probe_one(dev, &best, err, sizeof err) != 0)
        return 1;
    return 0;
}
