/*
 * PRTERM - Test: TX arbitration for multiple TNCs
 *
 * When several devices sit on the same channel, only one may transmit
 * at a time. The lock is a file per frequency and works across
 * processes - so the test needs a SECOND process to hold it: advisory
 * locks are per process, a process can always re-lock its own file.
 *
 * Locks in what matters on air:
 *   - a free channel is reported free (the probe used a write lock on
 *     a read-only descriptor before and always said "busy")
 *   - while somebody transmits, the channel is busy and the owner is
 *     known
 *   - a second transmission is refused - with the name of the one
 *     who is transmitting
 *   - another frequency is independent
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "prterm_compat.h"

#include "arbiter.h"
#include "testutil.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define TEST_DIR "test-arbiter.runtime"
#define FREQ_A   27235000L
#define FREQ_B   27245000L

int main(void)
{
    char err[256], owner[PR_ARBITER_OWNER_LEN];

    printf("== TX arbitration ==\n");

    (void)mkdir(TEST_DIR, 0777);

    /* ---- the lock file per frequency ------------------------------- */
    {
        char path[600];
        pr_arbiter_path(TEST_DIR, FREQ_A, path, sizeof path);
        CHECK(strstr(path, "tx-27235000.lock") != NULL);
    }

    /* ---- nobody transmitting: the channel is free ------------------ */
    owner[0] = '\0';
    CHECK(!pr_arbiter_busy(TEST_DIR, FREQ_A, owner, sizeof owner));

    /* ---- a second process transmits -------------------------------- */
    int pfd[2];
    CHECK(pipe(pfd) == 0);

    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        close(pfd[0]);
        char cerr[128];
        int fd = pr_arbiter_acquire(TEST_DIR, FREQ_A, "DL1ABC", 2000,
                                    cerr, sizeof cerr);
        if (fd < 0)
            _exit(1);
        (void)write(pfd[1], "x", 1);      /* holding the channel     */
        sleep(4);
        pr_arbiter_release(fd);
        _exit(0);
    }
    close(pfd[1]);

    char sig = 0;
    CHECK(read(pfd[0], &sig, 1) == 1);    /* the child holds it      */

    /* ---- the channel is busy, and we know who is transmitting ----- */
    CHECK(pr_arbiter_busy(TEST_DIR, FREQ_A, owner, sizeof owner));
    CHECK_STR(owner, "DL1ABC");

    /* ---- a second transmission is refused ------------------------- */
    {
        int fd = pr_arbiter_acquire(TEST_DIR, FREQ_A, "DL2XYZ", 200,
                                    err, sizeof err);
        CHECK(fd < 0);
        CHECK(strstr(err, "DL1ABC") != NULL);   /* who is on the air */
    }
    {
        /* no waiting at all                                     */
        int fd = pr_arbiter_acquire(TEST_DIR, FREQ_A, "DL2XYZ", 0,
                                    err, sizeof err);
        CHECK(fd < 0);
    }

    /* ---- another frequency is independent ------------------------- */
    {
        int fd = pr_arbiter_acquire(TEST_DIR, FREQ_B, "DL2XYZ", 200,
                                    err, sizeof err);
        CHECK(fd >= 0);
        /*
         * The probe cannot see OUR OWN lock - advisory locks never
         * conflict with the holding process (POSIX). Every CGI is a
         * process of its own, so on the way to the operator this
         * never matters.
         */
        CHECK(!pr_arbiter_busy(TEST_DIR, FREQ_B, owner, sizeof owner));
        pr_arbiter_release(fd);
        CHECK(!pr_arbiter_busy(TEST_DIR, FREQ_B, owner, sizeof owner));
    }

    /* ---- after the transmission the channel is free again --------- */
    (void)waitpid(pid, NULL, 0);
    CHECK(!pr_arbiter_busy(TEST_DIR, FREQ_A, owner, sizeof owner));

    /* ---- and a transmission of our own works                      */
    {
        int fd = pr_arbiter_acquire(TEST_DIR, FREQ_A, "DL2XYZ", 200,
                                    err, sizeof err);
        CHECK(fd >= 0);
        CHECK(!pr_arbiter_busy(TEST_DIR, FREQ_A, owner, sizeof owner));
        pr_arbiter_release(fd);
        CHECK(!pr_arbiter_busy(TEST_DIR, FREQ_A, owner, sizeof owner));
    }

    TEST_SUMMARY("arbiter");
}
