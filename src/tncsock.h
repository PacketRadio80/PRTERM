/*
 * PRTERM - CB & Amateur Radio Terminal
 * tncsock.h - Connection to the TNC daemon (prterm-tncd).
 *
 * Background
 * ==========
 * A CGI process ends after every request. The last file descriptor
 * goes away with it, and USB serial adapters reset - DTR drops even
 * when HUPCL is off. The TNC2C then leaves KISS mode and enters an
 * echo-only state.
 *
 * That is exactly why a separate DAEMON keeps the ports open: one
 * process, one open port, permanently. The daemon enters KISS, holds
 * it and checks it.
 *
 * The CGI never touches the device again - it only writes commands to
 * the daemon. That is at the same time the most important safeguard
 * against unintended transmissions: nothing the CGI does goes
 * unfiltered onto the air.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PRTERM_TNCSOCK_H
#define PRTERM_TNCSOCK_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Protocol - line by line over a Unix domain socket.
 *
 * Request:   COMMAND [argument]\n
 * Reply:     OK [data]\n   or   ERR [text]\n
 *
 * Payload is encoded as hex so no character conflicts with the
 * protocol control characters can occur.
 */
#define PR_TNCSOCK_MAX_LINE 8192

/* Commands */
#define PR_TNC_CMD_PING     "PING"
#define PR_TNC_CMD_TX       "TX"        /* TX <hex> - send bytes   */
#define PR_TNC_CMD_RX       "RX"        /* fetch received bytes     */
#define PR_TNC_CMD_STATUS   "STATUS"
#define PR_TNC_CMD_CHECKUP  "CHECKUP"   /* ensure KISS        */
#define PR_TNC_CMD_CLOSE    "QUIT"

typedef struct pr_tncsock {
    int fd;
    char last_err[256];
} pr_tncsock;

/* Connection to the daemon of a station. */
int  pr_tncsock_open(pr_tncsock *c, const char *socket_path,
                     char *err, size_t errlen);
void pr_tncsock_close(pr_tncsock *c);

/* Raw command. Returns 0 on OK, otherwise -1; the reply is in out. */
int  pr_tncsock_cmd(pr_tncsock *c, const char *cmd,
                    char *out, size_t outlen, char *err, size_t errlen);

/* Convenience */
int  pr_tncsock_ping(pr_tncsock *c, char *err, size_t errlen);
int  pr_tncsock_tx(pr_tncsock *c, const unsigned char *data, size_t len,
                   char *err, size_t errlen);
/* Returns the number of bytes fetched, 0 if nothing is there.    */
long pr_tncsock_rx(pr_tncsock *c, unsigned char *out, size_t cap,
                   char *err, size_t errlen);
int  pr_tncsock_checkup(pr_tncsock *c, char *err, size_t errlen);

/* Helper functions the daemon also uses      */
size_t pr_tncsock_hex_encode(char *dst, size_t dstlen,
                             const unsigned char *src, size_t len);
size_t pr_tncsock_hex_decode(unsigned char *dst, size_t dstlen,
                             const char *src);

#endif /* PRTERM_TNCSOCK_H */
