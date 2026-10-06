/*
 * PRTERM - CB & Amateur Radio Terminal
 * tncsock.c - Client side to the TNC daemon.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "prterm_compat.h"

#include "tncsock.h"
#include "util.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* ---- Hex ------------------------------------------------------------ */

size_t pr_tncsock_hex_encode(char *dst, size_t dstlen,
                             const unsigned char *src, size_t len)
{
    static const char digits[] = "0123456789abcdef";
    size_t w = 0;

    for (size_t i = 0; i < len; i++) {
        if (w + 2 >= dstlen)
            break;
        dst[w++] = digits[src[i] >> 4];
        dst[w++] = digits[src[i] & 0x0f];
    }
    if (w < dstlen)
        dst[w] = '\0';
    return w;
}

size_t pr_tncsock_hex_decode(unsigned char *dst, size_t dstlen,
                             const char *src)
{
    size_t w = 0;
    int hi = -1;

    for (const char *p = src; *p != '\0'; p++) {
        int v;
        if (*p >= '0' && *p <= '9')      v = *p - '0';
        else if (*p >= 'a' && *p <= 'f') v = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'F') v = *p - 'A' + 10;
        else continue;          /* Skip separators            */

        if (hi < 0) {
            hi = v;
        } else {
            if (w < dstlen)
                dst[w++] = (unsigned char)((hi << 4) | v);
            hi = -1;
        }
    }
    return w;
}

/* ---- Connection ----------------------------------------------------- */

int pr_tncsock_open(pr_tncsock *c, const char *socket_path,
                    char *err, size_t errlen)
{
    if (c == NULL || socket_path == NULL) {
        if (err) snprintf(err, errlen, "no socket path");
        return -1;
    }
    memset(c, 0, sizeof *c);
    c->fd = -1;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        if (err) snprintf(err, errlen, "Socket: %s", strerror(errno));
        return -1;
    }

    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    if (strlen(socket_path) >= sizeof sa.sun_path) {
        if (err) snprintf(err, errlen, "socket path too long");
        close(fd);
        return -1;
    }
    pr_strlcpy(sa.sun_path, socket_path, sizeof sa.sun_path);

    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        if (err) snprintf(err, errlen,
                          "prterm-tncd not reachable (%s): %s",
                          socket_path, strerror(errno));
        close(fd);
        return -1;
    }

    c->fd = fd;
    return 0;
}

void pr_tncsock_close(pr_tncsock *c)
{
    if (c == NULL)
        return;
    if (c->fd >= 0)
        close(c->fd);
    c->fd = -1;
}

/* ---- Commands --------------------------------------------------------- */

/* Reads one line up to '\n'. Returns the length without the newline. */
static long read_line(int fd, char *dst, size_t dstlen)
{
    size_t w = 0;

    while (w + 1 < dstlen) {
        char c;
        ssize_t n = read(fd, &c, 1);
        if (n <= 0)
            break;
        if (c == '\n') {
            dst[w] = '\0';
            return (long)w;
        }
        dst[w++] = c;
    }
    dst[w] = '\0';
    return (long)w;
}

int pr_tncsock_cmd(pr_tncsock *c, const char *cmd,
                   char *out, size_t outlen, char *err, size_t errlen)
{
    if (c == NULL || c->fd < 0) {
        if (err) snprintf(err, errlen, "no connection to the daemon");
        return -1;
    }
    if (out && outlen > 0)
        out[0] = '\0';

    char line[PR_TNCSOCK_MAX_LINE];
    int n = snprintf(line, sizeof line, "%s\n", cmd);
    if (n <= 0 || (size_t)n >= sizeof line) {
        if (err) snprintf(err, errlen, "command too long");
        return -1;
    }

    if (write(c->fd, line, (size_t)n) != n) {
        if (err) snprintf(err, errlen, "sending to daemon: %s", strerror(errno));
        return -1;
    }

    char resp[PR_TNCSOCK_MAX_LINE];
    if (read_line(c->fd, resp, sizeof resp) <= 0) {
        if (err) snprintf(err, errlen, "no reply from the daemon");
        return -1;
    }

    if (strncmp(resp, "OK", 2) == 0) {
        if (out && outlen > 0) {
            const char *p = resp + 2;
            while (*p == ' ') p++;
            pr_strlcpy(out, p, outlen);
        }
        return 0;
    }

    const char *p = strncmp(resp, "ERR", 3) == 0 ? resp + 3 : resp;
    while (*p == ' ') p++;
    if (err) snprintf(err, errlen, "%.200s", p);
    pr_strlcpy(c->last_err, p, sizeof c->last_err);
    return -1;
}

int pr_tncsock_ping(pr_tncsock *c, char *err, size_t errlen)
{
    return pr_tncsock_cmd(c, PR_TNC_CMD_PING, NULL, 0, err, errlen);
}

int pr_tncsock_tx(pr_tncsock *c, const unsigned char *data, size_t len,
                  char *err, size_t errlen)
{
    if (len > 4000) {
        if (err) snprintf(err, errlen, "TX packet too large");
        return -1;
    }

    char hex[2 * 4000 + 2];
    pr_tncsock_hex_encode(hex, sizeof hex, data, len);

    char cmd[PR_TNCSOCK_MAX_LINE];
    int n = snprintf(cmd, sizeof cmd, "%s %s", PR_TNC_CMD_TX, hex);
    if (n <= 0 || (size_t)n >= sizeof cmd) {
        if (err) snprintf(err, errlen, "TX packet too large");
        return -1;
    }
    return pr_tncsock_cmd(c, cmd, NULL, 0, err, errlen);
}

long pr_tncsock_rx(pr_tncsock *c, unsigned char *out, size_t cap,
                   char *err, size_t errlen)
{
    char resp[PR_TNCSOCK_MAX_LINE];
    if (pr_tncsock_cmd(c, PR_TNC_CMD_RX, resp, sizeof resp, err, errlen) != 0)
        return -1;

    size_t n = pr_tncsock_hex_decode(out, cap, resp);
    return (long)n;
}

int pr_tncsock_checkup(pr_tncsock *c, char *err, size_t errlen)
{
    return pr_tncsock_cmd(c, PR_TNC_CMD_CHECKUP, NULL, 0, err, errlen);
}
