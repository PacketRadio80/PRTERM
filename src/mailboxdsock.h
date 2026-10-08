/*
 * PRTERM - CB & Amateur Radio Terminal
 * mailboxdsock.h - PRTERM<->MailboxD unix-domain socket client.
 *
 * The MailboxD side is a plugin (mailboxd_prterm) that listens on the
 * socket, opens a MailboxD session per accepted client and runs the
 * PRTERM-issued MailboxD command. The protocol is line-based:
 *
 *   PRTERM -> MAILBOXD        MAILBOXD -> PRTERM
 *   -----------------------------  ------------------------------
 *   HELLO <ver>\n            OK MAILBOXD <ver>\n
 *   PING\n                    PONG\n
 *   RUN <line>\n              OUT <line>\n ... END ok\n
 *                            (or) END err <reason>\n
 *   QUIT\n                    (close)
 *
 * A connection is one-shot: open, handshake, N requests, close. The
 * CGI process is short-lived so we don't bother with a long-lived
 * connection. The single-flight accept on the MailboxD side keeps
 * things sane; if a second PRTERM hits an existing client MailboxD
 * drops the new arrival.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PRTERM_MAILBOXDSOCK_H
#define PRTERM_MAILBOXDSOCK_H

#include <stdbool.h>
#include <stddef.h>

/* Same value as the MailboxD plugin's MAILBOXD_PRTERM_LINK_LINE_MAX. */
#define PR_MAILBOXDSOCK_MAX_LINE 2048

/* Accumulator for a single RUN/command reply: many OUT lines followed
 * by one END line. Plain-text NUL-terminated. PRTERM reads it once. */
typedef struct pr_mailboxdsock {
    int fd;
    char last_err[256];
} pr_mailboxdsock;

/* Open the unix-domain socket at the given path. */
int  pr_mailboxdsock_open(pr_mailboxdsock *c, const char *socket_path,
                          char *err, size_t errlen);
void pr_mailboxdsock_close(pr_mailboxdsock *c);

/* HELLO handshake. Returns 0 on OK, -1 otherwise; the daemon's
 * banner (e.g. "OK MAILBOXD 1") is written into @p banner. */
int  pr_mailboxdsock_hello(pr_mailboxdsock *c, int version,
                           char *banner, size_t bannerlen,
                           char *err, size_t errlen);

/* PING. Returns 0 on PONG. */
int  pr_mailboxdsock_ping(pr_mailboxdsock *c, char *err, size_t errlen);

/*
 * RUN <command-line>\n
 *
 * Send a single MailboxD command line, return the captured text
 * reply. The reply format is "OUT <text>\n" per line, terminated
 * by a final "END ok\n" or "END err <reason>\n". This function
 * strips the framing and writes ONLY the textual payload lines
 * (without the "OUT " prefix, no END line) into @p out, separated
 * by '\n' and NUL-terminated. The number of lines is in
 * @p out_lines (if not NULL).
 *
 * Returns 0 on END ok, -1 on END err or transport failure.
 */
int  pr_mailboxdsock_run(pr_mailboxdsock *c, const char *cmdline,
                         char *out, size_t outlen, size_t *out_lines,
                         char *err, size_t errlen);

#endif /* PRTERM_MAILBOXDSOCK_H */
