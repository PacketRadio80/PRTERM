/*
 * PRTERM - CB & Amateur Radio Terminal
 * tncd.c - Keeps the serial ports open and holds KISS mode.
 *
 * Why a separate process
 * ======================
 * A CGI ends after every request. When the last file descriptor goes
 * away with it, USB serial adapters reset - even without HUPCL. The
 * TNC2C then leaves KISS and enters an echo-only state. That was the
 * cause for PTT no longer working and the "unacknowledged data" LED
 * staying on permanently.
 *
 * This daemon keeps the ports open, enters KISS once and holds it.
 * The CGI never touches the device again afterwards.
 *
 * How KISS is held (the model of the MAX25-Stack, kiss_bridge.py)
 * ===============================================================
 *   - The port is opened ONCE and never closed while the daemon runs.
 *   - KISS is entered ONCE: terminal probe, recovery ladder only when
 *     the device is deaf or only echoes, MYCALL, KISS entry and the
 *     KISS parameters. From then on the device speaks frames only.
 *   - While KISS is held ONLY KISS frames are written. Every other
 *     byte would go on the air as data - in KISS mode there are no
 *     commands any more.
 *   - There is no "leave KISS for transmitting". Leaving and
 *     re-entering is what resets TheFirmware and shows up as the
 *     start pattern in the LEDs (status + connected 2-3 s).
 *   - Repair is the MAX25 "stabilize_session": leave KISS (a control
 *     frame, nothing goes on the air), probe the terminal, run the
 *     ladder when it only echoes, set MYCALL, enter KISS. The port
 *     stays open the whole time - a DTR drop would undo the repair.
 *   - The watch only acts when something is wrong. A held KISS is
 *     not poked at - poking is what breaks it.
 *
 * Layout
 * ======
 *   prterm-tncd  keeps port open  ->  TNC2C / PK-TNC2
 *        ^
 *        |  Unix socket per station
 *   prterm.cgi   only sends commands
 *
 * Commands, line by line, answer "OK ..." or "ERR ...":
 *   PING            is the daemon still alive
 *   TX <hex>        send one KISS frame
 *   RX              fetch received bytes (KISS frames)
 *   STATUS          state
 *   CHECKUP         repair: ensure KISS mode
 *   QUIT            close connection
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "prterm_compat.h"

#include "config.h"
#include "kiss.h"
#include "radio.h"
#include "serial.h"
#include "callsign.h"
#include "tncsock.h"
#include "util.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "probe.h"

#define TNCD_MAX_STATIONS 4
#define TNCD_RX_BUFFER   65536

/*
 * Host-side pacing (MAX25-Stack tx_pace.py): at least this much quiet
 * between two frames on the air. The TNC switches the carrier with the
 * frame itself - hammering it would only produce gaps and collisions.
 */
#define TNCD_MIN_TX_GAP_S 1.5

/*
 * Serial watch (MAX25-Stack max25d.py): how often the link state is
 * looked at, and how rarely a repair may run. A held KISS needs no
 * care - the watch exists for the case that it was lost.
 */
#define TNCD_WATCH_S           60
#define TNCD_REPAIR_COOLDOWN_S 20

/* KISS parameters - see docs/TNC-INIT.md section 4            */
#define TNCD_TXDELAY  50
#define TNCD_SLOTTIME 10
#define TNCD_PERSIST  255   /* CB: enforced. Lower values make the
                             * access unreliable on a busy channel. */
#define TNCD_TXTAIL   10

typedef struct tncd_station {
    char        name[32];
    char        socket_path[512];
    pr_config   cfg;          /* Device settings of this station      */

    pr_serial   ser;
    bool        open;

    int         listen_fd;

    /* Receive buffer for the CGI  */
    unsigned char rx[TNCD_RX_BUFFER];
    size_t        rx_len;

    bool        kiss_active;   /* KISS is entered and being held      */
    char        detail[64];    /* what the watch is doing / last finding */
    time_t      last_watch;
    time_t      last_repair;
    double      last_tx;       /* pacing, monotonic                   */
} tncd_station;

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void station_pump(tncd_station *st);

/* ======================================================================= */
/* Small helpers                                                           */
/* ======================================================================= */

static double mono_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Write and wait until the bytes have really left the interface. */
static bool dev_write(tncd_station *st, const void *data, size_t len)
{
    char e2[128];
    if (!st->open)
        return false;
    (void)pr_serial_hold_dtr(&st->ser, e2, sizeof e2);
    if (pr_serial_write(&st->ser, data, len, e2, sizeof e2) != 0)
        return false;
    if (st->ser.fd >= 0)
        (void)tcdrain(st->ser.fd);
    return true;
}

/* Read and discard - leftovers from switching confuse every probe. */
static void dev_drain(tncd_station *st, int timeout_ms, int quiet_ms)
{
    unsigned char junk[2048];
    char e2[128];
    (void)pr_serial_read_quiet(&st->ser, junk, sizeof junk,
                               timeout_ms, quiet_ms, e2, sizeof e2);
}

/*
 * "The device speaks" = a firmware banner or a meaningful reply.
 * Echo of our own probe bytes does NOT count - an echo-only TNC2C is
 * exactly the state that has to be repaired.
 */
static bool device_speaks(const unsigned char *buf, size_t len)
{
    unsigned char tmp[2048];
    size_t n = len < sizeof tmp ? len : sizeof tmp;
    if (n == 0)
        return false;
    memcpy(tmp, buf, n);
    n = pr_probe_strip_echo(tmp, n);
    if (!pr_probe_has_content(tmp, n))
        return false;
    return pr_probe_has_banner(tmp, n) || pr_probe_score(tmp, n) > 0;
}

typedef enum dev_health {
    DEV_SILENT = 0,    /* nothing at all - wrong port, baud, power   */
    DEV_ALIVE  = 1,    /* terminal mode, a reply came back           */
    DEV_ECHO   = 2     /* only mirrors - DTR was lost, ladder needed */
} dev_health;

/*
 * ESC V (1B 56 0D) - the probe of the TNC2 class. Not INFO, not HELP.
 */
static dev_health probe_terminal(tncd_station *st,
                                 unsigned char *out, size_t outcap,
                                 size_t *outlen)
{
    static const unsigned char esc_v[] = { 0x1B, 'V', 0x0D };
    char e2[128];

    if (outlen != NULL)
        *outlen = 0;
    if (!dev_write(st, esc_v, sizeof esc_v))
        return DEV_SILENT;
    usleep(400000);

    unsigned char buf[2048];
    long n = pr_serial_read_quiet(&st->ser, buf, sizeof buf, 3000, 600,
                                  e2, sizeof e2);
    if (n <= 0)
        return DEV_SILENT;

    if (out != NULL && outcap > 0) {
        size_t k = (size_t)n < outcap ? (size_t)n : outcap;
        memcpy(out, buf, k);
        if (outlen != NULL)
            *outlen = k;
    }

    /* Pure echo of the probe? Then the terminal is not really there. */
    unsigned char tmp[2048];
    memcpy(tmp, buf, (size_t)n);
    size_t k = pr_probe_strip_echo(tmp, (size_t)n);
    if (!pr_probe_has_content(tmp, k))
        return DEV_ECHO;

    return DEV_ALIVE;
}

/* ======================================================================= */
/* KISS parameters                                                         */
/* ======================================================================= */

static void kiss_param(tncd_station *st, unsigned cmd, unsigned char value)
{
    unsigned char frame[16];
    size_t n = kiss_encode(frame, sizeof frame, 0, cmd, &value, 1);
    if (n > 0)
        (void)dev_write(st, frame, n);
}

/*
 * CSMA parameters of the channel access. Without them a TNC falls back
 * to whatever its EPROM says - that was one of the reasons why KISS
 * "did not work" here.
 */
static void send_kiss_params(tncd_station *st)
{
    kiss_param(st, KISS_CMD_TXDELAY,  TNCD_TXDELAY);
    usleep(100000);
    kiss_param(st, KISS_CMD_SLOTTIME, TNCD_SLOTTIME);
    usleep(100000);
    kiss_param(st, KISS_CMD_PERSIST,  TNCD_PERSIST);
    usleep(100000);
    kiss_param(st, KISS_CMD_TXTAIL,   TNCD_TXTAIL);
    usleep(100000);

    /* FULLDUPLEX is the lever via which the hardware may do full duplex. */
    kiss_param(st, KISS_CMD_FULLDUPLEX,
               st->cfg.duplex == PR_DUPLEX_FULL ? 1 : 0);
    usleep(100000);
    dev_drain(st, 250, 100);
}

/* ESC I <call>\r - the TNC must know its own identity. KISS DATA is
 * only keyed when MYCALL is set (MAX25 note on PTT). */
static void set_mycall(tncd_station *st)
{
    char cmd[32];
    unsigned char esc = 0x1B;

    snprintf(cmd, sizeof cmd, "I %.9s\r", st->cfg.callerid);
    (void)dev_write(st, &esc, 1);
    (void)dev_write(st, cmd, strlen(cmd));
    usleep(400000);

    unsigned char reply[128];
    char e2[128];
    long n = pr_serial_read_quiet(&st->ser, reply, sizeof reply, 400, 150,
                                  e2, sizeof e2);
    if (n > 0 && memchr(reply, '?', 32 < (size_t)n ? 32 : (size_t)n) != NULL)
        pr_strlcpy(st->detail, "MYCALL not accepted", sizeof st->detail);
}

/* ======================================================================= */
/* Recovery ladder (tnc_serial_recovery.py, docs/TNC-INIT.md)              */
/* ======================================================================= */

/*
 * Everything happens with the port OPEN - the ladder is pointless when
 * DTR drops in between and puts the TNC2C back into echo-only.
 *
 * The ladder stops at the first sign of life. "ESC QRES" is a cold
 * boot from EPROM - the mains power cycle that the manual demands is
 * not needed.
 */
static bool recover_terminal(tncd_station *st)
{
    unsigned char buf[2048];
    char e2[128];
    long n;

    pr_strlcpy(st->detail, "recovery ladder", sizeof st->detail);

    /* 0. Passive listen - sometimes the banner is already there     */
    n = pr_serial_read_quiet(&st->ser, buf, sizeof buf, 1500, 600,
                             e2, sizeof e2);
    if (n > 0 && device_speaks(buf, (size_t)n))
        return true;

    /*
     * 1. KISS return (C0 FF C0) is DELIBERATELY not sent again here -
     *    the caller has just sent it to leave KISS. On TheFirmware it
     *    resets the firmware, a second one would only cost another
     *    boot.
     */

    /* 2. Flush buffer, leave WA8DED host mode            */
    {
        static const unsigned char flush[] = { 0x11, 0x18 };
        (void)dev_write(st, flush, sizeof flush);
        usleep(200000);

        unsigned char nuls[300];
        memset(nuls, 0, sizeof nuls);
        (void)dev_write(st, nuls, sizeof nuls);
        usleep(150000);

        static const unsigned char jhost[] = {
            0x00, 0x01, 0x06, 'J', 'H', 'O', 'S', 'T', ' ', '0', '\r'
        };
        (void)dev_write(st, jhost, sizeof jhost);
        usleep(800000);

        n = pr_serial_read_quiet(&st->ser, buf, sizeof buf, 1000, 400,
                                 e2, sizeof e2);
        if (n > 0 && device_speaks(buf, (size_t)n))
            return true;
    }

    /* 3. ESC V                                        */
    if (probe_terminal(st, NULL, 0, NULL) == DEV_ALIVE)
        return true;

    /* 4. ESC QRES - cold boot from EPROM, DTR stays high      */
    {
        static const unsigned char qres[] = { 0x1B, 'Q', 'R', 'E', 'S', '\r' };
        (void)dev_write(st, qres, sizeof qres);
        n = pr_serial_read_quiet(&st->ser, buf, sizeof buf, 3000, 800,
                                 e2, sizeof e2);
        if (n > 0 && device_speaks(buf, (size_t)n))
            return true;
    }

    /* 5. ESC V after the cold boot                     */
    if (probe_terminal(st, NULL, 0, NULL) == DEV_ALIVE)
        return true;

    /* 6. ESC E 0 - echo off, then probe again          */
    {
        static const unsigned char eoff[] = { 0x1B, 'E', '0', '\r' };
        (void)dev_write(st, eoff, sizeof eoff);
        usleep(300000);
    }
    if (probe_terminal(st, NULL, 0, NULL) == DEV_ALIVE)
        return true;

    /* 7. Second ESC QRES + probe - the manual's "restart"        */
    {
        static const unsigned char qres[] = { 0x1B, 'Q', 'R', 'E', 'S', '\r' };
        (void)dev_write(st, qres, sizeof qres);
        n = pr_serial_read_quiet(&st->ser, buf, sizeof buf, 3000, 800,
                                 e2, sizeof e2);
        if (n > 0 && device_speaks(buf, (size_t)n))
            return true;
    }
    if (probe_terminal(st, NULL, 0, NULL) == DEV_ALIVE)
        return true;

    /* 8. Last resort - TAPR class: "kiss off" + "INFO"           */
    {
        static const unsigned char tapr[] = "kiss off\rINFO\r";
        (void)dev_write(st, tapr, sizeof tapr - 1);
        n = pr_serial_read_quiet(&st->ser, buf, sizeof buf, 3000, 800,
                                 e2, sizeof e2);
        if (n > 0 && device_speaks(buf, (size_t)n))
            return true;
    }

    pr_strlcpy(st->detail, "device does not answer", sizeof st->detail);
    return false;
}

/* ======================================================================= */
/* Ensure KISS - enter once, hold, repair when lost                        */
/* ======================================================================= */

/*
 * The order is crucial: leave KISS first. That is a control frame
 * and does NOT go on the air. Only then one may write commands -
 * otherwise one transmits oneself.
 */
static bool ensure_kiss(tncd_station *st, bool force, char *err, size_t errlen)
{
    if (!st->open) {
        snprintf(err, errlen, "device not open");
        return false;
    }

    /* Held and healthy - do not poke at it.               */
    if (st->kiss_active && !force)
        return true;

    /*
     * 1. Leave KISS. C0 FF C0 is a control frame, nothing goes on
     *    the air. On TheFirmware it additionally resets the firmware,
     *    which is why the boot is waited out below instead of
     *    counting down a fixed time.
     */
    {
        static const unsigned char leave[] = { 0xC0, 0xFF, 0xC0 };
        if (!dev_write(st, leave, sizeof leave)) {
            snprintf(err, errlen, "write to the device failed");
            return false;
        }
        usleep(1500000);
        dev_drain(st, 2500, 800);
    }

    /* 2. Terminal probe - the ladder only when it is needed     */
    if (probe_terminal(st, NULL, 0, NULL) != DEV_ALIVE) {
        if (!recover_terminal(st)) {
            st->kiss_active = false;
            snprintf(err, errlen,
                     "device does not answer - check port, baud rate and DTR");
            return false;
        }
    }

    /* 3. MYCALL - KISS DATA is only keyed with an identity     */
    set_mycall(st);

    /* 4. Enter KISS - per profile (docs/TNC-INIT.md section 2) */
    if (pr_str_eq_ci(st->cfg.kiss_init, "tapr")) {
        static const unsigned char kiss_on[] = "kiss on\r";
        (void)dev_write(st, kiss_on, sizeof kiss_on - 1);
    } else {
        static const unsigned char kiss_on[] = { 0x1B, 0x40, 0x4B };
        (void)dev_write(st, kiss_on, sizeof kiss_on);
    }
    usleep(500000);
    dev_drain(st, 300, 150);

    /* 5. KISS parameters - channel access must be defined      */
    send_kiss_params(st);

    st->kiss_active = true;
    st->last_repair = time(NULL);
    pr_strlcpy(st->detail, "KISS held", sizeof st->detail);
    if (err != NULL)
        err[0] = '\0';
    return true;
}

/* ======================================================================= */
/* Port                                                                    */
/* ======================================================================= */

static bool station_open(tncd_station *st, char *err, size_t errlen)
{
    int db, par, sb;
    if (!pr_serial_parse_line(st->cfg.serial_line, &db, &par, &sb)) {
        snprintf(err, errlen, "line \"%s\" is invalid", st->cfg.serial_line);
        return false;
    }

    if (pr_serial_open(&st->ser, st->cfg.port, st->cfg.baud, db, par, sb,
                       true, err, errlen) != 0)
        return false;

    st->open = true;
    return true;
}

/* ======================================================================= */
/* Collect reception                                                       */
/* ======================================================================= */

static void station_pump(tncd_station *st)
{
    unsigned char buf[2048];
    char err[128];

    long n = pr_serial_read_quiet(&st->ser, buf, sizeof buf, 20, 10,
                                  err, sizeof err);
    if (n <= 0)
        return;

    /*
     * Drop-out detection: while KISS is held the device speaks frames
     * only. A firmware banner means it rebooted or was reset - from
     * then on every byte written would be a transmission. The repair
     * is started by the watch or before the next frame.
     */
    if (st->kiss_active && pr_probe_has_banner(buf, (size_t)n)) {
        st->kiss_active = false;
        pr_strlcpy(st->detail, "device left KISS", sizeof st->detail);
    }

    /* Make room      */
    if (st->rx_len + (size_t)n > sizeof st->rx) {
        size_t drop = st->rx_len + (size_t)n - sizeof st->rx;
        memmove(st->rx, st->rx + drop, st->rx_len - drop);
        st->rx_len -= drop;
    }
    memcpy(st->rx + st->rx_len, buf, (size_t)n);
    st->rx_len += (size_t)n;
}

/*
 * Minimum quiet between two frames on the air. Reception keeps running
 * while waiting - the daemon must not go deaf during the gap.
 */
static void tx_pace(tncd_station *st)
{
    double gap = mono_s() - st->last_tx;
    while (gap < TNCD_MIN_TX_GAP_S && !g_stop) {
        station_pump(st);
        usleep(100000);
        gap = mono_s() - st->last_tx;
    }
    st->last_tx = mono_s();
}

/* ======================================================================= */
/* Commands                                                                */
/* ======================================================================= */

static void answer(int fd, const char *fmt, ...)
{
    char line[PR_TNCSOCK_MAX_LINE];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n <= 0 || (size_t)n >= sizeof line)
        return;
    line[n++] = '\n';
    if (write(fd, line, (size_t)n) != n) {
        /* Peer gone - will show up in the next poll()            */
    }
}

static void handle_command(tncd_station *st, int fd, const char *line)
{
    char cmd[64];
    const char *arg = strchr(line, ' ');
    if (arg != NULL) {
        size_t k = (size_t)(arg - line);
        if (k >= sizeof cmd) k = sizeof cmd - 1;
        memcpy(cmd, line, k);
        cmd[k] = '\0';
        arg++;
    } else {
        pr_strlcpy(cmd, line, sizeof cmd);
        arg = "";
    }

    if (pr_str_eq_ci(cmd, PR_TNC_CMD_PING)) {
        answer(fd, "OK pong");

    } else if (pr_str_eq_ci(cmd, PR_TNC_CMD_TX)) {
        if (!st->open) {
            answer(fd, "ERR device not open");
            return;
        }
        unsigned char data[4200];
        size_t n = pr_tncsock_hex_decode(data, sizeof data, arg);
        if (n == 0) {
            answer(fd, "ERR no data");
            return;
        }

        /*
         * ONLY KISS frames may go to the device. In KISS mode every
         * written byte is transmitted - a stray character would key
         * the transmitter.
         */
        if (n < 2 || data[0] != KISS_FEND || data[n - 1] != KISS_FEND) {
            answer(fd, "ERR not a KISS frame");
            return;
        }

        /* The frame is passed through unchanged - the driver has
         * already built it (FCS included in the AX.25 sense: none,
         * the TNC computes it). */
        if (!st->kiss_active) {
            char err[256];
            if (!ensure_kiss(st, true, err, sizeof err)) {
                answer(fd, "ERR %.200s", err);
                return;
            }
        }

        tx_pace(st);
        if (!dev_write(st, data, n)) {
            st->kiss_active = false;
            pr_strlcpy(st->detail, "write failed", sizeof st->detail);
            answer(fd, "ERR write to the device failed");
            return;
        }
        answer(fd, "OK %zu", n);

    } else if (pr_str_eq_ci(cmd, PR_TNC_CMD_RX)) {
        if (st->rx_len == 0) {
            answer(fd, "OK");
            return;
        }
        char hex[2 * TNCD_RX_BUFFER + 2];
        pr_tncsock_hex_encode(hex, sizeof hex, st->rx, st->rx_len);
        st->rx_len = 0;
        answer(fd, "OK %s", hex);

    } else if (pr_str_eq_ci(cmd, PR_TNC_CMD_STATUS)) {
        answer(fd, "OK station=%s kiss=%s open=%s port=%s detail=\"%.40s\"",
               st->name,
               st->kiss_active ? "held" : "lost",
               st->open ? "yes" : "no",
               st->cfg.port,
               st->detail);

    } else if (pr_str_eq_ci(cmd, PR_TNC_CMD_CHECKUP)) {
        if (!st->open) {
            answer(fd, "ERR device not open");
            return;
        }
        char err[256];
        if (!ensure_kiss(st, true, err, sizeof err)) {
            answer(fd, "ERR %.200s", err);
            return;
        }
        answer(fd, "OK KISS held");

    } else {
        answer(fd, "ERR unknown command %.60s", cmd);
    }
}

/* ======================================================================= */
/* Socket                                                                  */
/* ======================================================================= */

static int make_listener(const char *path, char *err, size_t errlen)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        snprintf(err, errlen, "Socket: %s", strerror(errno));
        return -1;
    }

    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof sa.sun_path) {
        snprintf(err, errlen, "socket path too long");
        close(fd);
        return -1;
    }
    pr_strlcpy(sa.sun_path, path, sizeof sa.sun_path);

    unlink(path);

    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        snprintf(err, errlen, "bind(%s): %s", path, strerror(errno));
        close(fd);
        return -1;
    }
    /* Only webserver access needs the file        */
    (void)chmod(path, 0660);

    if (listen(fd, 4) != 0) {
        snprintf(err, errlen, "listen(%s): %s", path, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

/* ======================================================================= */
/* Main loop                                                               */
/* ======================================================================= */

/*
 * A client connection must NOT stop reception.
 *
 * The first draft handled a connection in a nested blocking read()
 * loop - as long as one client held its socket, the device was never
 * pumped and the daemon went deaf. The poll loop below knows three
 * things at once: the listener, the device and every open client.
 */
#define TNCD_MAX_CLIENTS 8

typedef struct tncd_client {
    int           fd;
    tncd_station *st;
    char          line[PR_TNCSOCK_MAX_LINE];
    size_t        len;
} tncd_client;

static void client_close(tncd_client *cl)
{
    if (cl->fd >= 0)
        close(cl->fd);
    cl->fd = -1;
    cl->st = NULL;
    cl->len = 0;
}

/* Feed one read chunk; every complete line becomes a command. */
static void client_feed(tncd_client *cl, const char *data, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        char c = data[i];
        if (c == '\n') {
            cl->line[cl->len] = '\0';
            if (pr_str_eq_ci(cl->line, PR_TNC_CMD_CLOSE)) {
                client_close(cl);
                return;
            }
            handle_command(cl->st, cl->fd, cl->line);
            cl->len = 0;
            continue;
        }
        if (cl->len + 1 < sizeof cl->line)
            cl->line[cl->len++] = c;
    }
}

/*
 * The watch is the MAX25 "serial watch": look at the link every now
 * and then, repair when it is broken - and touch nothing when KISS is
 * held. A periodic re-entry into KISS is exactly what used to disturb
 * transmissions and reset the firmware.
 */
static void station_watch(tncd_station *st)
{
    time_t now = time(NULL);

    if (now - st->last_watch < TNCD_WATCH_S)
        return;
    st->last_watch = now;

    if (st->kiss_active)
        return;                       /* held - nothing to do        */

    if (now - st->last_repair < TNCD_REPAIR_COOLDOWN_S)
        return;                       /* repair is running/just ran  */

    char err[256];
    if (ensure_kiss(st, true, err, sizeof err))
        printf("  %-10s repaired - %s\n", st->name, st->detail);
    else
        printf("  %-10s repair failed - %.120s\n", st->name, err);
    fflush(stdout);
}

static int run_daemon(tncd_station *stations, size_t nst)
{
    tncd_client clients[TNCD_MAX_CLIENTS];
    for (size_t i = 0; i < TNCD_MAX_CLIENTS; i++) {
        clients[i].fd = -1;
        clients[i].st = NULL;
        clients[i].len = 0;
    }

    while (!g_stop) {
        struct pollfd pfd[TNCD_MAX_STATIONS * 2 + TNCD_MAX_CLIENTS];
        size_t np = 0;

        for (size_t i = 0; i < nst; i++) {
            if (stations[i].listen_fd >= 0) {
                pfd[np].fd = stations[i].listen_fd;
                pfd[np].events = POLLIN;
                np++;
            }
            if (stations[i].open) {
                pfd[np].fd = stations[i].ser.fd;
                pfd[np].events = POLLIN;
                np++;
            }
        }
        for (size_t i = 0; i < TNCD_MAX_CLIENTS; i++) {
            if (clients[i].fd >= 0) {
                pfd[np].fd = clients[i].fd;
                pfd[np].events = POLLIN;
                np++;
            }
        }

        int r = poll(pfd, np, 250);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        /* Reception from the device - and never while a client is
         * busy: the pump runs first, in every loop iteration. */
        for (size_t i = 0; i < nst; i++) {
            if (stations[i].open)
                station_pump(&stations[i]);
        }

        /* New connections */
        for (size_t i = 0; i < nst; i++) {
            tncd_station *st = &stations[i];
            struct pollfd one = { st->listen_fd, POLLIN, 0 };
            if (st->listen_fd < 0 ||
                poll(&one, 1, 0) <= 0 || !(one.revents & POLLIN))
                continue;

            int cfd = accept(st->listen_fd, NULL, NULL);
            if (cfd < 0)
                continue;

            size_t slot = TNCD_MAX_CLIENTS;
            for (size_t k = 0; k < TNCD_MAX_CLIENTS; k++) {
                if (clients[k].fd < 0) {
                    slot = k;
                    break;
                }
            }
            if (slot == TNCD_MAX_CLIENTS) {
                close(cfd);       /* busy - the CGI retries        */
                continue;
            }
            clients[slot].fd  = cfd;
            clients[slot].st  = st;
            clients[slot].len = 0;
        }

        /* Commands of the connected clients                       */
        for (size_t i = 0; i < TNCD_MAX_CLIENTS; i++) {
            tncd_client *cl = &clients[i];
            if (cl->fd < 0)
                continue;

            struct pollfd one = { cl->fd, POLLIN, 0 };
            if (poll(&one, 1, 0) <= 0)
                continue;

            if (one.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                client_close(cl);
                continue;
            }

            char buf[2048];
            ssize_t n = read(cl->fd, buf, sizeof buf);
            if (n <= 0) {
                client_close(cl);
                continue;
            }
            client_feed(cl, buf, (size_t)n);
        }

        for (size_t i = 0; i < nst; i++) {
            if (stations[i].open)
                station_watch(&stations[i]);
        }
    }

    for (size_t i = 0; i < TNCD_MAX_CLIENTS; i++)
        client_close(&clients[i]);
    for (size_t i = 0; i < nst; i++) {
        if (stations[i].listen_fd >= 0) {
            close(stations[i].listen_fd);
            unlink(stations[i].socket_path);
        }
    }
    return 0;
}

/* ======================================================================= */
/* Start                                                                   */
/* ======================================================================= */

static void usage(void)
{
    printf("prterm-tncd - keeps the TNC ports open and holds KISS\n\n");
    printf("Usage:\n");
    printf("  prterm-tncd [PRTERM.INI]\n\n");
    printf("One Unix socket is created per station:\n");
    printf("  <runtime_dir>/tnc-<station>.sock\n\n");
    printf("The daemon stays in the foreground. For continuous operation\n");
    printf("systemd takes over the start (see docs/).\n");
}

int main(int argc, char **argv)
{
    const char *ini_path = "prterm.ini";

    if (argc > 1 && (pr_str_eq_ci(argv[1], "-h") ||
                     pr_str_eq_ci(argv[1], "--help"))) {
        usage();
        return 0;
    }
    if (argc > 1)
        ini_path = argv[1];

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    char err[512];
    pr_config base;
    if (pr_config_load(&base, ini_path, err, sizeof err) != 0) {
        fprintf(stderr, "ERROR: %s\n", err);
        return 1;
    }

    tncd_station stations[TNCD_MAX_STATIONS];
    memset(stations, 0, sizeof stations);
    size_t nst = 0;

    for (size_t k = 0; k < base.nstations && nst < TNCD_MAX_STATIONS; k++) {
        const pr_station *src = &base.stations[k];
        if (!src->enabled)
            continue;

        tncd_station *st = &stations[nst];
        st->cfg = base;
        if (pr_config_apply_station(&st->cfg, src->name) == NULL)
            continue;

        pr_strlcpy(st->name, src->name, sizeof st->name);
        {
            /* via an intermediate buffer - otherwise the compiler complains
             * about overlapping target objects in the same struct */
            char tmp[512];
            snprintf(tmp, sizeof tmp, "%.400s/tnc-%.32s.sock",
                     st->cfg.runtime_dir, st->name);
            pr_strlcpy(st->socket_path, tmp, sizeof st->socket_path);
        }
        st->listen_fd = -1;

        printf("  %-10s %s\n", st->name, st->cfg.port);

        if (!station_open(st, err, sizeof err)) {
            fprintf(stderr, "ERROR %s: %s\n", st->name, err);
            continue;
        }

        /* Enter KISS once - from here on it is only held.     */
        if (!ensure_kiss(st, false, err, sizeof err)) {
            fprintf(stderr, "ERROR %s: %s\n", st->name, err);
            fprintf(stderr,
                    "       the watch keeps trying every %d s\n",
                    TNCD_WATCH_S);
            /* The port stays OPEN - closing would drop DTR and make
             * the repair even harder. */
        }

        st->listen_fd = make_listener(st->socket_path, err, sizeof err);
        if (st->listen_fd < 0) {
            fprintf(stderr, "ERROR %s: %s\n", st->name, err);
            pr_serial_close(&st->ser);
            continue;
        }

        st->last_watch = time(NULL);
        printf("  %-10s %s, socket %s\n",
               st->name,
               st->kiss_active ? "KISS held" : "KISS pending",
               st->socket_path);
        nst++;
    }

    if (nst == 0) {
        fprintf(stderr, "ERROR: no station could be started\n");
        pr_config_free(&base);
        return 1;
    }

    printf("\nprterm-tncd running - %zu station(s), Ctrl+C to stop\n", nst);
    fflush(stdout);

    int rc = run_daemon(stations, nst);

    for (size_t i = 0; i < nst; i++) {
        tncd_station *st = &stations[i];
        if (!st->open)
            continue;

        /*
         * Leave KISS properly - a control frame, nothing goes on the
         * air. Then close; the device is out of operation anyway.
         */
        if (st->kiss_active) {
            static const unsigned char leave[] = { 0xC0, 0xFF, 0xC0 };
            (void)pr_serial_write(&st->ser, leave, sizeof leave,
                                  err, sizeof err);
            usleep(300000);
        }
        pr_serial_close(&st->ser);
    }
    pr_config_free(&base);
    return rc;
}
