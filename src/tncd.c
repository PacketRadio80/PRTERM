/*
 * PRTERM - CB & Amateur Radio Terminal
 * tncd.c - Keeps the serial ports open and runs KISS mode.
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
 * Layout
 * ======
 *   prterm-tncd  keeps port open  ->  TNC2C / PK-TNC2
 *        ^
 *        |  Unix socket per station
 *   prterm.cgi   only sends commands
 *
 * Commands, line by line, answer "OK ..." or "ERR ...":
 *   PING            is the daemon still alive
 *   TX <hex>        send bytes
 *   RX              fetch received bytes
 *   STATUS          state
 *   CHECKUP         ensure KISS
 *   QUIT            close connection
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "prterm_compat.h"

#include "config.h"
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
#include <sys/ioctl.h>
#include <sys/un.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define TNCD_MAX_STATIONS 4
#define TNCD_RX_BUFFER   65536

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

    bool        kiss_ok;
    time_t      last_check;
} tncd_station;

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

/* ---- Ensure KISS        -------------------------------------------- */

/*
 * The order is crucial: leave KISS first. That is a control frame
 * and does NOT go on the air. Only then one may write commands -
 * otherwise one transmits oneself.
 */
static bool enter_command_mode(tncd_station *st, char *err, size_t errlen)
{
    char e2[128];
    (void)errlen;

    /* 1. Leave KISS     */
    {
        static const unsigned char leave[] = { 0xC0, 0xFF, 0xC0 };
        (void)pr_serial_write(&st->ser, leave, sizeof leave, e2, sizeof e2);
        usleep(1500000);
    }

    /* 2. Flush buffer, leave host mode     */
    {
        static const unsigned char flush[] = { 0x11, 0x18 };
        (void)pr_serial_write(&st->ser, flush, sizeof flush, e2, sizeof e2);
        usleep(150000);

        unsigned char nuls[300];
        memset(nuls, 0, sizeof nuls);
        (void)pr_serial_write(&st->ser, nuls, sizeof nuls, e2, sizeof e2);
        usleep(150000);

        static const unsigned char jhost[] = {
            0x00, 0x01, 0x06, 'J', 'H', 'O', 'S', 'T', ' ', '0', '\r'
        };
        (void)pr_serial_write(&st->ser, jhost, sizeof jhost, e2, sizeof e2);
        usleep(400000);
    }

    /* 3. Probe - in command mode a response must come      */
    {
        static const unsigned char probe[] = { 0x1B, 0x56, 0x0D };
        (void)pr_serial_write(&st->ser, probe, sizeof probe, e2, sizeof e2);
        usleep(400000);

        unsigned char junk[512];
        (void)pr_serial_read_quiet(&st->ser, junk, sizeof junk, 800, 200,
                                   e2, sizeof e2);
    }

    /*
     * 4. Set MYCALL. The TNC must know its own identity - without
     *    that the answerback ID does not work, and some firmware
     *    accepts no frame at all without MYCALL.
     */
    {
        char myc[32];
        snprintf(myc, sizeof myc, "%.1sI %.9s\r",
                 "\x1b", st->cfg.callerid);
        (void)pr_serial_write(&st->ser, myc, strlen(myc), e2, sizeof e2);
        usleep(400000);
        unsigned char junk[128];
        (void)pr_serial_read_quiet(&st->ser, junk, sizeof junk, 400, 150,
                                   e2, sizeof e2);
    }

    /*
     * 5. Permanent command mode.
     *
     * KISS is deliberately NOT entered. Anyone entering KISS has to
     * leave it again for transmitting - and exactly that exit and
     * re-entry is what triggers the firmware reset on TheFirmware
     * and thus the start pattern in the LEDs.
     *
     * Instead:
     *   TX      -> UNPROTO <dest> 0 <text>
     *   RX      -> monitor text
     * This way the device never has to be switched, and a reset
     * stays the exception instead of the rule.
     */
    {
        static const unsigned char mon[] = "MONITOR ON\r";
        (void)pr_serial_write(&st->ser, mon, sizeof mon - 1, e2, sizeof e2);
        usleep(300000);
        unsigned char junk[256];
        (void)pr_serial_read_quiet(&st->ser, junk, sizeof junk, 300, 150,
                                   e2, sizeof e2);
    }
    {
        static const unsigned char mall[] = "MALL ON\r";
        (void)pr_serial_write(&st->ser, mall, sizeof mall - 1, e2, sizeof e2);
        usleep(300000);
        unsigned char junk[256];
        (void)pr_serial_read_quiet(&st->ser, junk, sizeof junk, 300, 150,
                                   e2, sizeof e2);
    }

    st->kiss_ok = true;   /* here: ready for operation, not KISS */
    st->last_check = time(NULL);
    if (err) err[0] = '\0';
    return true;
}

/* ---- Port ---------------------------------------------------------- */

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

/* ---- Collect reception  -------------------------------------------- */


/*
 * Ensure DTR/RTS are asserted and wait until the transmit buffer is
 * really drained.
 *
 * Two things that were missing here:
 *   - DTR is re-asserted on EVERY write. If it drops in between, the
 *     TNC2C leaves KISS mode.
 *   - tcdrain() waits until the bytes have left the interface.
 *     Without it the caller believes the transmission is over while
 *     the TNC is still transmitting.
 */
static void tx_prepare(pr_serial *ser)
{
#ifdef TIOCMGET
    int flags = 0;
    if (ioctl(ser->fd, TIOCMGET, &flags) == 0) {
        flags |= TIOCM_RTS | TIOCM_DTR;
        (void)ioctl(ser->fd, TIOCMSET, &flags);
    }
#else
    (void)ser;
#endif
}

static void tx_finish(pr_serial *ser)
{
    if (ser->fd >= 0)
        (void)tcdrain(ser->fd);
}


/*
 * UNPROTO transmit path.
 *
 * TheFirmware (TNC2 class) often ignores KISS data frames on hybrid
 * setups, while UNPROTO from command mode switches the carrier
 * reliably. That is exactly the symptom "PTT only sometimes".
 *
 * Sequence: leave KISS -> send UNPROTO -> wait -> re-enter KISS.
 * The frame is evaluated to get destination and text; whatever else
 * is in it is dropped.
 */
static int send_unproto(tncd_station *st, const unsigned char *frame, size_t len,
                        char *err, size_t errlen)
{
    /* AX.25 UI: dest(7) src(7) Control(1) PID(1) payload      */
    if (len < 16) {
        snprintf(err, errlen, "frame too short");
        return -1;
    }

    char dst[16], src[16];
    if (!call_from_ax25(frame, dst, sizeof dst) ||
        !call_from_ax25(frame + 7, src, sizeof src)) {
        snprintf(err, errlen, "callsign not readable");
        return -1;
    }

    char text[PR_MSG_TEXT];
    size_t n = len - 16;
    if (n >= sizeof text) n = sizeof text - 1;
    memcpy(text, frame + 16, n);
    text[n] = '\0';

    char e2[128];
    tx_prepare(&st->ser);

    /*
     * The device stays in command mode permanently - nothing has to
     * be switched and thus nothing has to be reset. We only set the
     * identity to be safe.
     */
    {
        char myc[32];
        snprintf(myc, sizeof myc, "%.1sI %.9s\r", "\x1b", st->cfg.callerid);
        (void)pr_serial_write(&st->ser, myc, strlen(myc), e2, sizeof e2);
        usleep(300000);
        unsigned char jj[128];
        (void)pr_serial_read_quiet(&st->ser, jj, sizeof jj, 300, 150, e2, sizeof e2);
    }

    /* 3. UNPROTO <dest> 0 <text> */
    char cmd[PR_MSG_TEXT + 64];
    int k = snprintf(cmd, sizeof cmd, "UNPROTO %.9s 0 %s\r", dst, text);
    if (k <= 0 || (size_t)k >= sizeof cmd) {
        snprintf(err, errlen, "message too long");
        return -1;
    }
    if (pr_serial_write(&st->ser, cmd, (size_t)k, err, errlen) != 0)
        return -1;
    tx_finish(&st->ser);

    /* 4. wait until the TNC has transmitted */
    usleep(1200000);

    /*
     * DELIBERATELY no re-entry into KISS.
     *
     * The device stays in command mode permanently. If one sent ESC @K
     * at the end, the device would be in KISS again for the next
     * transmission - and "UNPROTO ..." would go on the air as data
     * instead of as a command. Exactly that made the first
     * transmission work and all following ones fail.
     */
    return 0;
}

static void station_pump(tncd_station *st)
{
    unsigned char buf[2048];
    char err[128];

    long n = pr_serial_read_quiet(&st->ser, buf, sizeof buf, 20, 10,
                                  err, sizeof err);
    if (n <= 0)
        return;

    /* Make room      */
    if (st->rx_len + (size_t)n > sizeof st->rx) {
        size_t drop = st->rx_len + (size_t)n - sizeof st->rx;
        memmove(st->rx, st->rx + drop, st->rx_len - drop);
        st->rx_len -= drop;
    }
    memcpy(st->rx + st->rx_len, buf, (size_t)n);
    st->rx_len += (size_t)n;
}

/* ---- Commands --------------------------------------------------------- */

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
        char err[256];
        {
            if (send_unproto(st, data, n, err, sizeof err) != 0) {
                answer(fd, "ERR %.200s", err);
                return;
            }
            answer(fd, "OK %zu (unproto)", n);
            return;
        }
        tx_prepare(&st->ser);
        /*
         * Re-set PERSIST before EVERY frame. If the parameters get
         * lost (e.g. after an unwanted reset), transmission would be
         * unreliable otherwise.
         */
        {
            unsigned char pf[4] = { 0xC0, 0x02, 255, 0xC0 };
            (void)pr_serial_write(&st->ser, pf, 4, err, sizeof err);
        }
        if (pr_serial_write(&st->ser, data, n, err, sizeof err) != 0) {
            answer(fd, "ERR %.200s", err);
            return;
        }
        tx_finish(&st->ser);
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
        answer(fd, "OK station=%s kiss=%s open=%s port=%s",
               st->name,
               st->kiss_ok ? "yes" : "no",
               st->open ? "yes" : "no",
               st->cfg.port);

    } else if (pr_str_eq_ci(cmd, PR_TNC_CMD_CHECKUP)) {
        if (!st->open) {
            answer(fd, "ERR device not open");
            return;
        }
        char err[256];
        if (!enter_command_mode(st, err, sizeof err)) {
            answer(fd, "ERR %.200s", err);
            return;
        }
        answer(fd, "OK");

    } else {
        answer(fd, "ERR unknown command %.60s", cmd);
    }
}

/* ---- Socket --------------------------------------------------------- */

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

/* ---- Main loop     --------------------------------------------------- */

static int run_daemon(tncd_station *stations, size_t nst)
{
    char err[256];

    while (!g_stop) {
        struct pollfd pfd[TNCD_MAX_STATIONS * 2];
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

        int r = poll(pfd, np, 250);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        for (size_t i = 0; i < nst; i++) {
            tncd_station *st = &stations[i];

            /* Collect reception from the device */
            if (st->open)
                station_pump(st);

            /* Accept a new connection and process it immediately */
            if (st->listen_fd >= 0) {
                struct pollfd one = { st->listen_fd, POLLIN, 0 };
                if (poll(&one, 1, 0) > 0 && (one.revents & POLLIN)) {
                    int cfd = accept(st->listen_fd, NULL, NULL);
                    if (cfd >= 0) {
                        char line[PR_TNCSOCK_MAX_LINE];
                        ssize_t n;
                        while ((n = read(cfd, line, sizeof line - 1)) > 0) {
                            line[n] = '\0';
                            char *nl = strchr(line, '\n');
                            if (nl) *nl = '\0';
                            if (pr_str_eq_ci(line, PR_TNC_CMD_CLOSE))
                                break;
                            handle_command(st, cfd, line);
                        }
                        close(cfd);
                    }
                }
            }
        }

        /*
         * Maintain KISS periodically.
         *
         * IMPORTANT: the re-check must NOT disturb operation.
         * enter_kiss() takes about 3 seconds and writes to the port
         * during that time. If that fell into a transmit sequence, that
         * transmission was lost - one saw only single PTT instead of all.
         *
         * Therefore: only re-check when there has been enough quiet
         * since the last transmission, and generally rarely. The daemon
         * keeps the port open, so KISS is not lost by itself.
         */
        for (size_t i = 0; i < nst; i++) {
            tncd_station *st = &stations[i];
            if (!st->open)
                continue;
            time_t idle = time(NULL) - st->last_check;
            if (idle > 600 && st->rx_len == 0) {
                (void)enter_command_mode(st, err, sizeof err);
            }
        }
    }

    for (size_t i = 0; i < nst; i++) {
        if (stations[i].listen_fd >= 0) {
            close(stations[i].listen_fd);
            unlink(stations[i].socket_path);
        }
    }
    return 0;
}

/* ---- Start ----------------------------------------------------------- */

static void usage(void)
{
    printf("prterm-tncd - keeps the TNC ports open and runs KISS\n\n");
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
        if (!enter_command_mode(st, err, sizeof err)) {
            fprintf(stderr, "ERROR %s: %s\n", st->name, err);
            pr_serial_close(&st->ser);
            continue;
        }

        st->listen_fd = make_listener(st->socket_path, err, sizeof err);
        if (st->listen_fd < 0) {
            fprintf(stderr, "ERROR %s: %s\n", st->name, err);
            pr_serial_close(&st->ser);
            continue;
        }

        printf("  %-10s KISS active, socket %s\n", st->name, st->socket_path);
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
        if (stations[i].open)
            pr_serial_close(&stations[i].ser);
    }
    pr_config_free(&base);
    return rc;
}
