/*
 * PRTERM - CB & Amateur Radio Terminal
 * probe.h - TNC detection and boot interception.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PRTERM_PROBE_H
#define PRTERM_PROBE_H

#include "serial.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct pr_probe_result {
    char dev[256];
    long baud;
    int  databits;
    int  parity;
    int  stopbits;
    int  score;
    bool responds;
    bool echo_only;        /* Device only mirrors - echo was on */
    bool banner;           /* Firmware banner recognized */
    bool clean_link;       /* Banner, echo or clean response - no garbage  */
    char answer[512];      /* real response after echo removal   */
    int  fd;               /* with pr_probe_bootwait: keep it OPEN! */
} pr_probe_result;

/* Report progress; return != 0 aborts.          */
typedef int (*pr_probe_cb)(void *ud, const char *msg);

/*
 * Runs the profiles against a device.
 *
 * The port is opened EXACTLY ONCE and kept open until the end -
 * closing drops DTR and puts a TNC2C into a state where it no
 * longer responds.
 *
 * Returns 0 if at least one profile responded.
 */
int pr_probe_device(const char *dev, pr_probe_result *best,
                    pr_probe_cb progress, void *ud,
                    char *err, size_t errlen);

/*
 * Intercepts the boot banner of a device.
 *
 * Usage: open the port, assert DTR/RTS, THEN power on the device and
 * wait here. Only reading is done, nothing is sent.
 *
 * IMPORTANT: on success best->fd is open - do NOT close it as long as
 * the device is to stay in operation.
 */
int pr_probe_bootwait(const char *dev, long baud, int databits, int parity,
                      int stopbits, int seconds,
                      pr_probe_result *best, pr_probe_cb progress, void *ud,
                      char *err, size_t errlen);

/*
 * Brings the device back to a known state and reads the response
 * along. The port stays open.
 *
 * Sequence from tnc_serial_recovery.py:
 *
 *   11 18                       flush buffer (^Q^X)
 *   300 x 00 + JHOST 0          leave WA8DED host mode
 *   C0 FF C0                    leave KISS - on TheFirmware also a
 *                               FIRMWARE-RESET that triggers the banner
 *   ESC V                       probe
 *
 * IMPORTANT: the response is NOT discarded. An earlier draft cleared
 * at the end and threw away the triggered boot banner.
 *
 * Returns the length of the response read.
 */
size_t pr_probe_reset(pr_serial *s, unsigned char *out, size_t outcap);

/* Line format as text, e.g. "19200 7E1".   */
void pr_probe_format(const pr_probe_result *r, char *dst, size_t dstlen);

/* ---- Pure scoring functions --------------------------------------
 * Deliberately public so they can be tested directly. */

size_t pr_probe_remove_bytes(unsigned char *buf, size_t len,
                             const unsigned char *needle, size_t nlen);
size_t pr_probe_strip_echo(unsigned char *buf, size_t len);
double pr_probe_printable_ratio(const unsigned char *buf, size_t len);
bool   pr_probe_has_content(const unsigned char *buf, size_t len);
int    pr_probe_score(const unsigned char *buf, size_t len);

/* Detect firmware marker - that is the real criterion for "a TNC
 * speaks here", regardless of the score. */
bool   pr_probe_has_banner(const unsigned char *buf, size_t len);

#endif /* PRTERM_PROBE_H */
