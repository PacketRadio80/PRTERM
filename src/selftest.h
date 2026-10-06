/*
 * PRTERM - CB & Amateur Radio Terminal
 * selftest.h - TNC health check and emergency reset.
 *
 * The test is deliberately PART of the program and not an external
 * script: whoever operates it should know exactly where things hang,
 * and be able to trigger a reset in an emergency without shell access.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PRTERM_SELFTEST_H
#define PRTERM_SELFTEST_H

#include "config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

typedef enum pr_test_status {
    PR_TEST_PASS = 0,
    PR_TEST_WARN = 1,
    PR_TEST_FAIL = 2,
    PR_TEST_SKIP = 3
} pr_test_status;

typedef struct pr_test_result {
    char name[64];
    int  status;          /* pr_test_status */
    char detail[256];
} pr_test_result;

#define PR_SELFTEST_MAX 32

typedef struct pr_selftest {
    pr_test_result items[PR_SELFTEST_MAX];
    size_t n;
    bool   overall_ok;
    char   firmware[128];   /* recognized firmware, if present    */
} pr_selftest;

/*
 * Checks the device from the configuration:
 *   - does the interface exist
 *   - can the port be opened
 *   - are DTR/RTS asserted
 *   - does the device respond (ESC V)
 *   - is the firmware recognized
 *   - does the profile match the configuration
 *
 * Returns 0 if everything is fine, otherwise the number of errors.
 */
int pr_selftest_run(const pr_config *cfg, pr_selftest *out);

/*
 * Emergency reset: brings the device back to a defined state
 * via the known reset sequence and then checks the device
 * again.
 *
 *   11 18                    flush buffer (^Q^X)
 *   300 x 00 + JHOST 0       leave WA8DED host mode
 *   C0 FF C0                 leave KISS / firmware reset
 *   ESC V                    probe
 *
 * Returns 0 if the device speaks afterwards.
 */
int pr_selftest_reset(const pr_config *cfg, pr_selftest *out);

/*
 * Recovery: makes sure the device is in KISS mode and does not
 * carry a memory full of stuck frames.
 *
 * CAUTION - the order is crucial. As long as a TNC is in KISS mode,
 * EVERY written byte is transmitted. A "quick query" would transmit
 * itself. Therefore:
 *
 *   1. C0 FF C0        leave KISS - control frame, does NOT go on
 *                       the air
 *   2. flush buffer,    now one is in command mode and may write
 *      leave host mode  without transmitting
 *   3. ESC V           probe - confirms command mode
 *   4. enter KISS      ESC @K or "kiss on\r" depending on the profile
 *
 * Returns 0 if everything is fine, otherwise the number of errors.
 */
int pr_checkup(const pr_config *cfg, pr_selftest *out);

/* Output for the command line.    */
void pr_selftest_print(const pr_selftest *st, FILE *f);

/* Short form as a single line. */
const char *pr_test_status_name(int status);

#endif /* PRTERM_SELFTEST_H */
