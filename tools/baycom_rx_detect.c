/*
 * baycom_rx_detect.c — Auto-detect which MSR pin carries BayCom RX signal.
 *
 * Opens the serial port, asserts DTR+RTS, monitors all 4 MSR input pins
 * (CTS, DSR, RI, DCD) for transitions. The pin with the most transitions
 * is the RX signal from the TCM3105 demodulator.
 *
 * Works on FreeBSD (via ioctl TIOCMGET) and Linux (via ioctl or /dev/io).
 *
 * Usage: ./baycom_rx_detect /dev/cuau0 [seconds]
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <time.h>

#if defined(__FreeBSD__)
#include <sys/ttycom.h>
#elif defined(__linux__)
#include <sys/ioctl.h>
#include <asm/ioctls.h>
#endif

#ifndef TIOCMGET
#define TIOCMGET 0x5415
#endif
#ifndef TIOCMBIS
#define TIOCMBIS 0x5416
#endif
#ifndef TIOCM_DTR
#define TIOCM_DTR 0x002
#endif
#ifndef TIOCM_RTS
#define TIOCM_RTS 0x004
#endif
#ifndef TIOCM_CTS
#define TIOCM_CTS 0x020
#endif
#ifndef TIOCM_DSR
#define TIOCM_DSR 0x100
#endif
#ifndef TIOCM_RI
#define TIOCM_RI  0x040
#endif
#ifndef TIOCM_CD
#define TIOCM_CD  0x080
#endif

typedef struct {
    const char *name;
    int mask;
    int counts[2];
    int transitions;
    int last;
} pin_stat_t;

int main(int argc, char *argv[])
{
    const char *device = "/dev/cuau0";
    int seconds = 3;
    int fd;
    struct timespec start, now;
    pin_stat_t pins[4] = {
        {"CTS", TIOCM_CTS, {0,0}, 0, -1},
        {"DSR", TIOCM_DSR, {0,0}, 0, -1},
        {"RI",  TIOCM_RI,  {0,0}, 0, -1},
        {"DCD", TIOCM_CD,  {0,0}, 0, -1},
    };

    if (argc > 1) device = argv[1];
    if (argc > 2) seconds = atoi(argv[2]);

    fd = open(device, O_RDWR | O_NOCTTY);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    /* Set raw mode */
    {
        struct termios tio;
        if (tcgetattr(fd, &tio) == 0) {
            cfmakeraw(&tio);
            tio.c_cflag |= CLOCAL;
            tcsetattr(fd, TCSANOW, &tio);
        }
    }

    /* Assert DTR + RTS */
    {
        int bits = TIOCM_DTR | TIOCM_RTS;
        ioctl(fd, TIOCMBIS, &bits);
    }

    printf("BayCom RX auto-detect on %s (%d seconds)\n", device, seconds);
    printf("Asserted DTR+RTS. Monitoring all MSR input pins...\n");
    printf("Open squelch on radio for best results.\n\n");

    clock_gettime(CLOCK_MONOTONIC, &start);

    for (;;) {
        int bits = 0;
        int i;

        clock_gettime(CLOCK_MONOTONIC, &now);
        if ((now.tv_sec - start.tv_sec) +
            (now.tv_nsec - start.tv_nsec) / 1e9 >= seconds)
            break;

        if (ioctl(fd, TIOCMGET, &bits) < 0)
            break;

        for (i = 0; i < 4; i++) {
            int val = (bits & pins[i].mask) ? 1 : 0;
            pins[i].counts[val]++;
            if (pins[i].last >= 0 && val != pins[i].last)
                pins[i].transitions++;
            pins[i].last = val;
        }

        usleep(1000);
    }

    close(fd);

    /* Results */
    printf("Pin   0-count  1-count  Transitions\n");
    printf("----  -------  -------  -----------\n");

    int best = 0;
    for (int i = 0; i < 4; i++) {
        pin_stat_t *p = &pins[i];
        printf("%-4s  %7d  %7d  %11d", p->name, p->counts[0], p->counts[1],
               p->transitions);
        if (p->transitions > 50)
            printf("  *** SIGNAL ***");
        printf("\n");
        if (p->transitions > pins[best].transitions)
            best = i;
    }

    printf("\n");
    if (pins[best].transitions > 50) {
        printf("==> RX detected on %s (%d transitions)\n",
               pins[best].name, pins[best].transitions);
        printf("    prterm-baycom should use MSR_%s for RX.\n",
               pins[best].name);
        return 0;
    } else {
        printf("==> No RX signal detected on any pin.\n");
        printf("    Check: cable wiring, TCM3105 RX stage, audio path.\n");
        return 1;
    }
}