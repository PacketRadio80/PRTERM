/*
 * PRTERM - CB & Amateur Radio Terminal
 * serial.c - portable serial interface (POSIX.1-2008).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "prterm_compat.h"

#include "serial.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#ifdef TIOCMGET
#include <sys/ioctl.h>
#endif

/* ======================================================================= */
/* Baud rate                                                                */
/* ======================================================================= */

/*
 * Via B* constants instead of cfsetspeed(): that is the POSIX way.
 * Rarer rates are additionally behind #ifdef because they are not
 * declared everywhere.
 */
static int baud_to_flag(long baud, speed_t *out)
{
    switch (baud) {
    case 50:      *out = B50;      return 0;
    case 75:      *out = B75;      return 0;
    case 110:     *out = B110;     return 0;
    case 134:     *out = B134;     return 0;
    case 150:     *out = B150;     return 0;
    case 200:     *out = B200;     return 0;
    case 300:     *out = B300;     return 0;
    case 600:     *out = B600;     return 0;
    case 1200:    *out = B1200;    return 0;
    case 1800:    *out = B1800;    return 0;
    case 2400:    *out = B2400;    return 0;
    case 4800:    *out = B4800;    return 0;
    case 9600:    *out = B9600;    return 0;
    case 19200:   *out = B19200;   return 0;
    case 38400:   *out = B38400;   return 0;
#ifdef B57600
    case 57600:   *out = B57600;   return 0;
#endif
#ifdef B115200
    case 115200:  *out = B115200;  return 0;
#endif
#ifdef B230400
    case 230400:  *out = B230400;  return 0;
#endif
#ifdef B460800
    case 460800:  *out = B460800;  return 0;
#endif
#ifdef B921600
    case 921600:  *out = B921600;  return 0;
#endif
    default:
        return -1;
    }
}

/* ======================================================================= */
/* Opening                                                                 */
/* ======================================================================= */

int pr_serial_open(pr_serial *s, const char *dev,
                   long baud, int databits, int parity, int stopbits,
                   bool rts_dtr, char *err, size_t errlen)
{
    memset(s, 0, sizeof *s);
    s->fd = -1;

    if (dev == NULL || dev[0] == '\0') {
        snprintf(err, errlen, "no device given");
        return -1;
    }
    pr_strlcpy(s->dev, dev, sizeof s->dev);
    s->baud     = baud;
    s->databits = databits;
    s->parity   = parity;
    s->stopbits = stopbits;
    s->rts_dtr  = rts_dtr;

    speed_t sp;
    if (baud_to_flag(baud, &sp) != 0) {
        snprintf(err, errlen, "baud rate %ld is not supported", baud);
        return -1;
    }
    if (databits != 7 && databits != 8) {
        snprintf(err, errlen, "data bits must be 7 or 8");
        return -1;
    }
    if (stopbits != 1 && stopbits != 2) {
        snprintf(err, errlen, "stop bits must be 1 or 2");
        return -1;
    }

    /*
     * O_NOCTTY  - the port must not become a terminal for this process
     * O_NONBLOCK - access goes through poll() with a timeout
     */
    int fd = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        snprintf(err, errlen, "cannot open %s: %s", dev, strerror(errno));
        return -1;
    }

    struct termios t;
    if (tcgetattr(fd, &t) != 0) {
        snprintf(err, errlen, "tcgetattr on %s failed: %s",
                 dev, strerror(errno));
        close(fd);
        return -1;
    }

    /*
     * Raw mode. cfmakeraw() is BSD/GNU and not POSIX - the fields are
     * set by hand instead. Identical on Linux and FreeBSD.
     */
    t.c_iflag &= ~(unsigned)(IGNBRK | BRKINT | PARMRK | ISTRIP |
                             INLCR  | IGNCR  | ICRNL   | IXON);
    t.c_oflag &= ~(unsigned)OPOST;
    t.c_lflag &= ~(unsigned)(ECHO | ECHONL | ICANON | ISIG | IEXTEN);

    /* Line format  */
    t.c_cflag &= ~(unsigned)(CSIZE | PARENB | PARODD | CSTOPB);

    /*
     * HUPCL OFF: closing the port must NOT drop DTR.
     *
     * A CGI opens and closes the port on every call - the state poll
     * runs every second. If DTR dropped there, it would tear the TNC2C
     * out of KISS mode into an echo-only state on every call. That is
     * exactly why PTT stopped working.
     *
     * KISS mode is meant to be PERMANENT, even when no process is
     * attached to the port between two requests.
     */
    t.c_cflag &= ~(unsigned)HUPCL;

    t.c_cflag |= (unsigned)(CLOCAL | CREAD);
    t.c_cflag |= (unsigned)(databits == 7 ? CS7 : CS8);
    if (parity == PR_PAR_EVEN) {
        t.c_cflag |= (unsigned)PARENB;
    } else if (parity == PR_PAR_ODD) {
        t.c_cflag |= (unsigned)(PARENB | PARODD);
    }
    if (stopbits == 2)
        t.c_cflag |= (unsigned)CSTOPB;

    /* No hardware flow control - usual for TNCs and safer.           */
#ifdef CRTSCTS
    t.c_cflag &= ~(unsigned)CRTSCTS;
#endif

    t.c_cc[VMIN]  = 0;
    t.c_cc[VTIME] = 5;     /* 0.5 s between bytes  */

    if (cfsetispeed(&t, sp) != 0 || cfsetospeed(&t, sp) != 0) {
        snprintf(err, errlen, "baud rate %ld could not be set: %s",
                 baud, strerror(errno));
        close(fd);
        return -1;
    }

    if (tcsetattr(fd, TCSANOW, &t) != 0) {
        snprintf(err, errlen, "tcsetattr on %s failed: %s",
                 dev, strerror(errno));
        close(fd);
        return -1;
    }
    tcflush(fd, TCIOFLUSH);

    /*
     * Modem lines. Important for TNC2C clones: without DTR/RTS the TNC
     * stays in a state where it does not respond.
     *
     * IMPORTANT: setting is done with TIOCMBIS (set bits) - that
     * works WITHOUT reading first. An earlier draft called TIOCMGET
     * first and silently skipped setting on failure; on this port
     * TIOCMGET is not available, so the lines never came up and the
     * TNC did not answer.
     */
    if (rts_dtr) {
#if defined(TIOCMBIS)
        int bits = TIOCM_RTS | TIOCM_DTR;
        (void)ioctl(fd, TIOCMBIS, &bits);
#elif defined(TIOCMSET)
        int lines = TIOCM_RTS | TIOCM_DTR;
        (void)ioctl(fd, TIOCMSET, &lines);
#endif
        /*
         * Extra attempt via the read/write path. If it fails, that is
         * not fatal - TIOCMBIS has already done the work.
         */
#ifdef TIOCMGET
        {
            int cur = 0;
            if (ioctl(fd, TIOCMGET, &cur) == 0) {
                cur |= TIOCM_RTS | TIOCM_DTR;
                (void)ioctl(fd, TIOCMSET, &cur);
            }
        }
#endif
    }

    s->fd = fd;
    s->open = true;
    return 0;
}

void pr_serial_close(pr_serial *s)
{
    if (s == NULL)
        return;
    if (s->fd >= 0) {
        /* We drop the lines - that is the normal case when detaching          */
        close(s->fd);
    }
    s->fd = -1;
    s->open = false;
}

bool pr_serial_ok(const pr_serial *s)
{
    return s != NULL && s->open && s->fd >= 0;
}

bool pr_serial_parse_line(const char *s, int *databits, int *parity, int *stopbits)
{
    if (s == NULL || strlen(s) != 3)
        return false;
    if (s[0] != '7' && s[0] != '8')
        return false;
    if (s[1] != 'n' && s[1] != 'N' && s[1] != 'e' && s[1] != 'E' &&
        s[1] != 'o' && s[1] != 'O')
        return false;
    if (s[2] != '1' && s[2] != '2')
        return false;

    if (databits != NULL) *databits = s[0] - '0';
    if (parity != NULL) {
        char c = s[1];
        if (c == 'e' || c == 'E')      *parity = PR_PAR_EVEN;
        else if (c == 'o' || c == 'O') *parity = PR_PAR_ODD;
        else                            *parity = PR_PAR_NONE;
    }
    if (stopbits != NULL) *stopbits = s[2] - '0';
    return true;
}

/* ======================================================================= */
/* Reading and writing                                                     */
/* ======================================================================= */

int pr_serial_write(pr_serial *s, const void *buf, size_t len,
                    char *err, size_t errlen)
{
    if (!pr_serial_ok(s)) {
        snprintf(err, errlen, "interface not open");
        return -1;
    }

    const unsigned char *p = buf;
    size_t done = 0;

    while (done < len) {
        ssize_t n = write(s->fd, p + done, len - done);
        if (n > 0) {
            done += (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pfd;
            pfd.fd = s->fd;
            pfd.events = POLLOUT;
            if (poll(&pfd, 1, 1000) <= 0) {
                snprintf(err, errlen, "write timeout on %s", s->dev);
                return -1;
            }
            continue;
        }
        snprintf(err, errlen, "write error on %s: %s", s->dev, strerror(errno));
        return -1;
    }

    /* Wait for the actual drain - important for TNCs           */
    (void)tcdrain(s->fd);
    return 0;
}

long pr_serial_read(pr_serial *s, void *buf, size_t cap,
                    int timeout_ms, char *err, size_t errlen)
{
    if (!pr_serial_ok(s)) {
        if (errlen > 0) snprintf(err, errlen, "interface not open");
        return -1;
    }
    if (cap == 0)
        return 0;

    struct pollfd pfd;
    pfd.fd = s->fd;
    pfd.events = POLLIN;

    int pr = poll(&pfd, 1, timeout_ms);
    if (pr == 0)
        return 0;                       /* Timeout   */
    if (pr < 0) {
        if (errno == EINTR)
            return 0;
        if (errlen > 0)
            snprintf(err, errlen, "poll on %s: %s", s->dev, strerror(errno));
        return -1;
    }

    ssize_t n = read(s->fd, buf, cap);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return 0;
        if (errlen > 0)
            snprintf(err, errlen, "read error on %s: %s", s->dev, strerror(errno));
        return -1;
    }
    return (long)n;
}

long pr_serial_read_quiet(pr_serial *s, void *buf, size_t cap,
                          int timeout_ms, int quiet_ms,
                          char *err, size_t errlen)
{
    unsigned char *p = buf;
    size_t total = 0;
    long first = 1;

    while (total < cap) {
        int wait = first ? timeout_ms : quiet_ms;
        long n = pr_serial_read(s, p + total, cap - total, wait, err, errlen);
        if (n < 0)
            return -1;
        if (n == 0) {
            if (first && total == 0)
                return 0;               /* nothing arrived at all */
            break;                      /* Quiet: response is complete    */
        }
        total += (size_t)n;
        first = 0;
    }
    return (long)total;
}

int pr_serial_flush(pr_serial *s, bool input, bool output)
{
    if (!pr_serial_ok(s))
        return -1;

    int sel;
    if (input && output)      sel = TCIOFLUSH;
    else if (input)           sel = TCIFLUSH;
    else                      sel = TCOFLUSH;

    return tcflush(s->fd, sel) == 0 ? 0 : -1;
}

int pr_serial_reconfigure(pr_serial *s, long baud, int databits,
                          int parity, int stopbits, char *err, size_t errlen)
{
    if (!pr_serial_ok(s)) {
        snprintf(err, errlen, "interface not open");
        return -1;
    }

    speed_t sp;
    if (baud_to_flag(baud, &sp) != 0) {
        snprintf(err, errlen, "baud rate %ld is not supported", baud);
        return -1;
    }

    struct termios t;
    if (tcgetattr(s->fd, &t) != 0) {
        snprintf(err, errlen, "tcgetattr: %s", strerror(errno));
        return -1;
    }

    t.c_cflag &= ~(unsigned)(CSIZE | PARENB | PARODD | CSTOPB);

    /*
     * HUPCL OFF: closing the port must NOT drop DTR.
     *
     * A CGI opens and closes the port on every call - the state poll
     * runs every second. If DTR dropped there, it would tear the TNC2C
     * out of KISS mode into an echo-only state on every call. That is
     * exactly why PTT stopped working.
     *
     * KISS mode is meant to be PERMANENT, even when no process is
     * attached to the port between two requests.
     */
    t.c_cflag &= ~(unsigned)HUPCL;

    t.c_cflag |= (unsigned)(CLOCAL | CREAD);
    t.c_cflag |= (unsigned)(databits == 7 ? CS7 : CS8);
    if (parity == PR_PAR_EVEN)      t.c_cflag |= (unsigned)PARENB;
    else if (parity == PR_PAR_ODD)  t.c_cflag |= (unsigned)(PARENB | PARODD);
    if (stopbits == 2)              t.c_cflag |= (unsigned)CSTOPB;

    if (cfsetispeed(&t, sp) != 0 || cfsetospeed(&t, sp) != 0) {
        snprintf(err, errlen, "baud rate %ld not settable: %s", baud, strerror(errno));
        return -1;
    }
    if (tcsetattr(s->fd, TCSANOW, &t) != 0) {
        snprintf(err, errlen, "tcsetattr: %s", strerror(errno));
        return -1;
    }

    s->baud = baud;
    s->databits = databits;
    s->parity = parity;
    s->stopbits = stopbits;
    return 0;
}

int pr_serial_hold_dtr(pr_serial *s, char *err, size_t errlen)
{
    if (!pr_serial_ok(s)) {
        snprintf(err, errlen, "interface not open");
        return -1;
    }
#if defined(TIOCMBIS)
    {
        int bits = TIOCM_RTS | TIOCM_DTR;
        if (ioctl(s->fd, TIOCMBIS, &bits) != 0) {
            snprintf(err, errlen, "DTR/RTS not settable: %s", strerror(errno));
            return -1;
        }
    }
#endif
    return 0;
}

int pr_serial_modem_lines(pr_serial *s)
{
    if (!pr_serial_ok(s))
        return -1;
#ifdef TIOCMGET
    int lines = 0;
    if (ioctl(s->fd, TIOCMGET, &lines) != 0)
        return -1;
    return lines;
#else
    (void)s;
    return -1;
#endif
}
