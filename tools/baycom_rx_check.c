/*
 * baycom_rx_check.c — Read MSR/CTS via direct UART register access.
 *
 * BayCom/Compa PC-COM SER12: demodulated RX bits arrive on CTS (MSR bit4).
 * This tool samples CTS to verify the modem's RX path works.
 *
 * Requires root (FreeBSD: /dev/io, Linux: ioperm).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _BSD_SOURCE
#define _BSD_SOURCE
#endif
#ifndef __BSD_VISIBLE
#define __BSD_VISIBLE 1
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>

#if defined(__FreeBSD__)
#include <machine/cpufunc.h>
#elif defined(__linux__)
#include <sys/io.h>
#endif

#define MSR_OFFSET 6  /* MSR = iobase + 6 */

static unsigned long parse_iobase(const char *s)
{
    if (s == NULL || s[0] == '\0')
        return 0x3f8;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        return strtoul(s, NULL, 16);
    /* Try /dev/cuaUx -> look up uart N -> iobase, or accept decimal */
    return strtoul(s, NULL, 0);
}

int main(int argc, char *argv[])
{
    unsigned long iobase = 0x3f8;
    int seconds = 10;
    int counts[2] = {0, 0};
    int transitions = 0;
    int last = -1;
    struct timespec start, now;
    double elapsed;
    int total;
    int fd_io = -1;

    if (argc > 1)
        iobase = parse_iobase(argv[1]);
    if (argc > 2)
        seconds = atoi(argv[2]);

#if defined(__FreeBSD__)
    fd_io = open("/dev/io", O_RDWR);
    if (fd_io < 0) {
        perror("open /dev/io (need root)");
        return 1;
    }
#elif defined(__linux__)
    if (ioperm(iobase, 8, 1) != 0) {
        perror("ioperm (need root)");
        return 1;
    }
#endif

    printf("BayCom RX check: iobase=0x%lx, MSR=0x%lx, %d seconds\n",
           iobase, iobase + MSR_OFFSET, seconds);
    printf("Open squelch -> expect DSR transitions from TCM3105 demodulator\n\n");

    clock_gettime(CLOCK_MONOTONIC, &start);

    for (;;) {
        unsigned char msr;
        int cts;

        clock_gettime(CLOCK_MONOTONIC, &now);
        elapsed = (now.tv_sec - start.tv_sec) +
                  (now.tv_nsec - start.tv_nsec) / 1e9;
        if (elapsed >= seconds)
            break;

#if defined(__FreeBSD__)
        /* FreeBSD: inb() via inline asm, fd keeps /dev/io open */
        (void)fd_io;
        msr = inb(iobase + MSR_OFFSET);
#elif defined(__linux__)
        msr = inb(iobase + MSR_OFFSET);
#else
        msr = 0;
#endif

        cts = (msr >> 5) & 1;  /* DSR bit5 — Albrecht PC-COM uses DSR, not CTS */
        counts[cts]++;
        if (last >= 0 && cts != last)
            transitions++;
        last = cts;

        usleep(1000); /* 1ms -> ~1000 samples/sec */
    }

    elapsed = (now.tv_sec - start.tv_sec) +
              (now.tv_nsec - start.tv_nsec) / 1e9;
    total = counts[0] + counts[1];

    printf("Samples:     %d in %.1fs (%d/s)\n", total, elapsed,
           elapsed > 0 ? (int)(total / elapsed) : 0);
    printf("DSR=0 (low): %d (%d%%)\n", counts[0],
           total ? counts[0] * 100 / total : 0);
    printf("DSR=1 (high):%d (%d%%)\n", counts[1],
           total ? counts[1] * 100 / total : 0);
    printf("Transitions: %d\n\n", transitions);

    if (transitions > 50)
        printf("==> RX OK! DSR toggling -> TCM3105 demodulates.\n");
    else if (transitions > 0)
        printf("==> Few transitions -> weak/noisy signal.\n");
    else if (counts[1] > 0)
        printf("==> DSR stuck HIGH -> check wiring.\n");
    else
        printf("==> DSR stuck LOW -> no demodulated signal.\n"
               "    Check: DSR pin (6) on cable, TCM3105 RX stage,\n"
               "    audio path from radio to modem.\n");

#if defined(__FreeBSD__)
    if (fd_io >= 0)
        close(fd_io);
#endif

    return 0;
}