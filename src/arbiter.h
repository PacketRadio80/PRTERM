/*
 * PRTERM - CB & Amateur Radio Terminal
 * arbiter.h - TX arbitration for multiple TNCs.
 *
 * When several devices sit on the same channel, ALWAYS only one may
 * transmit. If two go on the air at once, they destroy each other's
 * signals - on the same channel there is no second option.
 *
 * This layer is a hard rule, not a recommendation: the transmit path
 * goes through it or not at all.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PRTERM_ARBITER_H
#define PRTERM_ARBITER_H

#include <stdbool.h>
#include <stddef.h>

#define PR_ARBITER_OWNER_LEN 64

/*
 * Holds the transmit lock for a channel.
 *
 * Procedure:
 *   fd = pr_arbiter_acquire(...);   blocks until free
 *   ... transmit ...
 *   pr_arbiter_release(fd);
 *
 * The lock is a file in runtime_dir - so it also works across
 * multiple CGI processes.
 */
int  pr_arbiter_acquire(const char *runtime_dir, long freq_hz,
                        const char *owner, int timeout_ms,
                        char *err, size_t errlen);
void pr_arbiter_release(int fd);

/* Asks whether a transmission is in progress. True if busy. */
bool pr_arbiter_busy(const char *runtime_dir, long freq_hz,
                     char *owner, size_t ownerlen);

/* Returns the lock path (for display and diagnostics).      */
void pr_arbiter_path(const char *runtime_dir, long freq_hz,
                     char *dst, size_t dstlen);

#endif /* PRTERM_ARBITER_H */
