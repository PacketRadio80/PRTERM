/*
 * PRTERM - CB & Amateur Radio Terminal
 * mailboxdsock.c - PRTERM<->MailboxD unix-domain socket client.
 *
 * The line format follows the plugin's wire protocol exactly. Every
 * function on the public side opens (or expects) a fresh connection
 * because the MailboxD plugin drops late arrivals.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "mailboxdsock.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* ---------------------------------------------------------------------- */
/* Low-level helpers                                                     */
/* ---------------------------------------------------------------------- */

static int set_err(char *err, size_t errlen, const char *fmt, ...)
{
    if (err == NULL || errlen == 0) {
        return -1;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
    return -1;
}

static int connect_unix(const char *path, char *err, size_t errlen)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return set_err(err, errlen, "socket: %s", strerror(errno));
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof addr.sun_path) {
        close(fd);
        return set_err(err, errlen, "socket path too long");
    }
    strncpy(addr.sun_path, path, sizeof addr.sun_path - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        int e = errno;
        close(fd);
        return set_err(err, errlen, "connect %s: %s", path, strerror(e));
    }

    /*
     * The MailboxD plugin writes 1 reply line per request (HELLO, PING,
     * END ok, END err). Each line is short (well under MTU) and the
     * plugin is a single accept thread, so a 200 ms timeout is plenty
     * and prevents the CGI from ever hanging for the full default 75 s
     * SO_RCVTIMEO. 2 s for the rare slow first connect is fine too.
     */
    struct timeval to = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &to, sizeof to);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof to);
    return fd;
}

/* Send a single line (with trailing '\n'). */
static int send_line(int fd, const char *line, char *err, size_t errlen)
{
    size_t len = strlen(line);
    if (len > PR_MAILBOXDSOCK_MAX_LINE - 2) {
        return set_err(err, errlen, "line too long");
    }
    char buf[PR_MAILBOXDSOCK_MAX_LINE];
    memcpy(buf, line, len);
    buf[len++] = '\n';
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, buf + off, len - off, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return set_err(err, errlen, "send: %s", strerror(errno));
        }
        off += (size_t)n;
    }
    return 0;
}

/*
 * Read a single line terminated by '\n'. Returns 0 on success,
 * -1 on transport failure. The line is NUL-terminated without
 * the trailing '\n'.
 */
static int recv_line(int fd, char *out, size_t outlen, char *err, size_t errlen)
{
    size_t off = 0;
    while (off + 1 < outlen) {
        char ch;
        ssize_t n = recv(fd, &ch, 1, 0);
        if (n == 0) {
            if (off == 0) {
                return set_err(err, errlen, "connection closed");
            }
            return set_err(err, errlen, "unexpected EOF mid-line");
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* partial line in the buffer, no more bytes within the
                 * recv timeout; treat as a broken line. */
                return set_err(err, errlen, "recv timeout (got %zu bytes)", off);
            }
            return set_err(err, errlen, "recv: %s", strerror(errno));
        }
        if (ch == '\n') {
            out[off] = '\0';
            return 0;
        }
        /* CRs are stripped (MailboxD may emit CRLF on banner). */
        if (ch == '\r') {
            continue;
        }
        out[off++] = ch;
    }
    return set_err(err, errlen, "line too long (max %zu)", outlen - 1);
}

/* ---------------------------------------------------------------------- */
/* Lifecycle                                                              */
/* ---------------------------------------------------------------------- */

int pr_mailboxdsock_open(pr_mailboxdsock *c, const char *socket_path,
                          char *err, size_t errlen)
{
    if (c == NULL || socket_path == NULL) {
        return set_err(err, errlen, "invalid argument");
    }
    memset(c, 0, sizeof *c);
    c->fd = -1; /* explicit: not connected yet */
    int fd = connect_unix(socket_path, err, errlen);
    if (fd < 0) {
        return -1;
    }
    c->fd = fd;
    return 0;
}

void pr_mailboxdsock_close(pr_mailboxdsock *c)
{
    if (c == NULL) {
        return;
    }
    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }
}

/* ---------------------------------------------------------------------- */
/* HELLO handshake                                                       */
/* ---------------------------------------------------------------------- */

int pr_mailboxdsock_hello(pr_mailboxdsock *c, int version,
                           char *banner, size_t bannerlen,
                           char *err, size_t errlen)
{
    char line[PR_MAILBOXDSOCK_MAX_LINE];
    snprintf(line, sizeof line, "HELLO %d", version);
    if (send_line(c->fd, line, err, errlen) != 0) {
        return -1;
    }
    if (recv_line(c->fd, line, sizeof line, err, errlen) != 0) {
        return -1;
    }
    /*
     * strncmp("OK MAILBOXD 1", "OK MAILBOXD", 12) is implementation-
     * defined: POSIX says stop at first '\0' in either string, but
     * glibc reads on and reports the difference at the byte after
     * the needle terminator. The MailboxD plugin sends 11 ASCII
     * characters + '\n' for the reply. We want a match on the
     * prefix only; do that with memcmp over a known length, or with
     * a "starts with" check.
     */
    if (strncmp(line, "OK MAILBOXD", 12) == 0 ||
        (strlen(line) >= 11 && strncmp(line, "OK MAILBOXD", 11) == 0)) {
        /* ok */
    } else {
        return set_err(err, errlen, "bad HELLO reply: %.64s", line);
    }
    if (banner != NULL && bannerlen > 0) {
        pr_strlcpy(banner, line, bannerlen);
    }
    return 0;
}

/* ---------------------------------------------------------------------- */
/* PING                                                                   */
/* ---------------------------------------------------------------------- */

int pr_mailboxdsock_ping(pr_mailboxdsock *c, char *err, size_t errlen)
{
    if (send_line(c->fd, "PING", err, errlen) != 0) {
        return -1;
    }
    char line[PR_MAILBOXDSOCK_MAX_LINE];
    if (recv_line(c->fd, line, sizeof line, err, errlen) != 0) {
        return -1;
    }
    if (strcmp(line, "PONG") != 0) {
        return set_err(err, errlen, "bad PING reply: %.64s", line);
    }
    return 0;
}

/* ---------------------------------------------------------------------- */
/* RUN <cmdline>                                                          */
/* ---------------------------------------------------------------------- */

int pr_mailboxdsock_run(pr_mailboxdsock *c, const char *cmdline,
                         char *out, size_t outlen, size_t *out_lines,
                         char *err, size_t errlen)
{
    if (out != NULL && outlen > 0) {
        out[0] = '\0';
    }
    if (out_lines != NULL) {
        *out_lines = 0;
    }

    char line[PR_MAILBOXDSOCK_MAX_LINE];
    int n = snprintf(line, sizeof line, "RUN %s", cmdline);
    if (n < 0 || (size_t)n >= sizeof line) {
        return set_err(err, errlen, "RUN line too long");
    }
    if (send_line(c->fd, line, err, errlen) != 0) {
        return -1;
    }

    /*
     * Read until END ok or END err. Each OUT line is one MailboxD
     * output line; the END line marks the end and carries the
     * status.
     */
    bool ok = false;
    size_t n_lines = 0;
    size_t out_used = 0;

    for (;;) {
        if (recv_line(c->fd, line, sizeof line, err, errlen) != 0) {
            return -1;
        }
        if (strncmp(line, "END ok", 6) == 0) {
            ok = true;
            break;
        }
        if (strncmp(line, "END err", 7) == 0) {
            /* MailboxD may have sent OUT lines before the END err
             * (e.g. the actual error text from cmd_ handlers like
             * "Unknown command" or "Access denied").  Keep them in
             * the output buffer so the caller can display them,
             * then set the err string from the END line. */
            if (out_lines != NULL) {
                *out_lines = n_lines;
            }
            return set_err(err, errlen, "%s", line + 8);
        }
        if (strncmp(line, "OUT ", 4) != 0) {
            return set_err(err, errlen, "unexpected line: %.64s", line);
        }
        const char *body = line + 4;
        size_t blen = strlen(body);
        if (out != NULL && outlen > 0) {
            size_t copy = blen;
            if (out_used + copy + 1 >= outlen) {
                copy = (outlen - 1 > out_used) ? (outlen - 1 - out_used) : 0;
            }
            if (copy > 0) {
                memcpy(out + out_used, body, copy);
                out_used += copy;
                out[out_used] = '\0';
            }
            if (out_used + 1 < outlen) {
                out[out_used++] = '\n';
                out[out_used] = '\0';
            }
        }
        n_lines++;
    }

    if (out_lines != NULL) {
        *out_lines = n_lines;
    }
    return ok ? 0 : -1;
}
