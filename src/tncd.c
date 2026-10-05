/*
 * PRTERM - CB & Amateur Radio Terminal
 * tncd.c - Haelt die seriellen Ports offen und fuehrt den KISS-Modus.
 *
 * Warum ein eigener Prozess
 * =========================
 * Ein CGI endet nach jeder Anfrage. Faellt damit der letzte
 * Dateideskriptor, setzen USB-Serienadapter zurueck - auch ohne HUPCL.
 * Der TNC2C verlaesst daraufhin KISS und geht in einen Echo-only-
 * Zustand. Das war die Ursache dafuer, dass kein PTT mehr zustande
 * kam und die LED "unbestaetigte Daten" dauerhaft leuchtete.
 *
 * Dieser Daemon haelt die Ports offen, betritt KISS einmal und haelt
 * es. Das CGI beruehrt das Geraet danach ueberhaupt nicht mehr.
 *
 * Aufbau
 * ======
 *   prterm-tncd  haelt Port offen  ->  TNC2C / PK-TNC2
 *        ^
 *        |  Unix-Socket je Station
 *   prterm.cgi   schreibt nur noch Befehle
 *
 * Befehle, zeilenweise, Antwort "OK ..." oder "ERR ...":
 *   PING            lebt der Daemon noch
 *   TX <hex>        Bytes senden
 *   RX              empfangene Bytes abholen
 *   STATUS          Zustand
 *   CHECKUP         KISS sicherstellen
 *   QUIT            Verbindung schliessen
 *
 * SPDX-License-Identifier: MIT
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
    pr_config   cfg;          /* Geraete-Einstellungen dieser Station */

    pr_serial   ser;
    bool        open;

    int         listen_fd;

    /* Empfangspuffer fuer das CGI */
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

/* ---- KISS sicherstellen -------------------------------------------- */

/*
 * Reihenfolge ist entscheidend: zuerst KISS verlassen. Das ist ein
 * Kontrollrahmen und geht NICHT auf die Luft. Erst danach darf man
 * Kommandos schreiben - sonst sendet man selbst.
 */
static bool enter_kiss(tncd_station *st, char *err, size_t errlen)
{
    char e2[128];
    (void)errlen;

    /* 1. KISS verlassen */
    {
        static const unsigned char leave[] = { 0xC0, 0xFF, 0xC0 };
        (void)pr_serial_write(&st->ser, leave, sizeof leave, e2, sizeof e2);
        usleep(1500000);
    }

    /* 2. Puffer leeren, Hostmode verlassen */
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

    /* 3. Probe - im Kommandomodus muss eine Antwort kommen */
    {
        static const unsigned char probe[] = { 0x1B, 0x56, 0x0D };
        (void)pr_serial_write(&st->ser, probe, sizeof probe, e2, sizeof e2);
        usleep(400000);

        unsigned char junk[512];
        (void)pr_serial_read_quiet(&st->ser, junk, sizeof junk, 800, 200,
                                   e2, sizeof e2);
    }

    /*
     * 4. MYCALL setzen. Das TNC muss seine eigene Kennung kennen -
     *    ohne das geht die Ruecklaufkennung nicht, und manche Firmware
     *    laesst ohne MYCALL gar keinen Rahmen zu.
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

    /* 5. KISS betreten */
    {
        if (pr_str_eq_ci(st->cfg.kiss_init, "tapr")) {
            static const unsigned char kiss_on[] = {
                'k','i','s','s',' ','o','n','\r'
            };
            (void)pr_serial_write(&st->ser, kiss_on, sizeof kiss_on,
                                  e2, sizeof e2);
        } else {
            static const unsigned char kiss_on[] = { 0x1B, 0x40, 0x4B };
            (void)pr_serial_write(&st->ser, kiss_on, sizeof kiss_on,
                                  e2, sizeof e2);
        }
        usleep(250000);

        unsigned char junk[256];
        (void)pr_serial_read_quiet(&st->ser, junk, sizeof junk, 250, 100,
                                   e2, sizeof e2);
    }

    /*
     * 6. KISS-Parameter setzen. Werte wie im CB-Betrieb ueblich:
     *    TXDELAY 50  - PTT-Vorlauf in 10ms-Schritten
     *    PERSIST 255 - immer senden, kein ALOHA-Zufall (Sondernutzung)
     *    SLOTTIME 10 - Sperrzeit
     *    TXTAIL 1    - PTT-Nachlauf
     *    FULLDUPLEX 0
     */
    {
        static const struct { unsigned char cmd, val; } kp[] = {
            { 0x01, 50 },   /* TXDELAY  */
            { 0x02, 255 },  /* PERSIST  */
            { 0x03, 10 },   /* SLOTTIME */
            { 0x04, 1 },    /* TXTAIL   */
            { 0x05, 0 }     /* FULLDUPLEX */
        };
        for (size_t i = 0; i < sizeof kp / sizeof kp[0]; i++) {
            unsigned char f[6];
            f[0] = 0xC0; f[1] = kp[i].cmd; f[2] = kp[i].val; f[3] = 0xC0;
            (void)pr_serial_write(&st->ser, f, 4, e2, sizeof e2);
        }
        usleep(200000);
    }

    st->kiss_ok = true;
    st->last_check = time(NULL);
    if (err) err[0] = '\0';
    return true;
}

/* ---- Port ---------------------------------------------------------- */

static bool station_open(tncd_station *st, char *err, size_t errlen)
{
    int db, par, sb;
    if (!pr_serial_parse_line(st->cfg.serial_line, &db, &par, &sb)) {
        snprintf(err, errlen, "Line \"%s\" ungueltig", st->cfg.serial_line);
        return false;
    }

    if (pr_serial_open(&st->ser, st->cfg.port, st->cfg.baud, db, par, sb,
                       true, err, errlen) != 0)
        return false;

    st->open = true;
    return true;
}

/* ---- Empfang einsammeln -------------------------------------------- */


/*
 * Sichern, dass DTR/RTS anliegen, und warten bis der Sendepuffer
 * wirklich raus ist.
 *
 * Zwei Dinge, die der MAX25-Stack so macht und die hier gefehlt haben:
 *   - DTR wird bei JEDEM Schreibvorgang erneut gesetzt. Faellt es
 *     zwischendurch, verlaesst der TNC2C den KISS-Modus.
 *   - tcdrain() wartet, bis die Bytes die Schnittstelle verlassen
 *     haben. Ohne das glaubt der Aufrufer, die Sendung sei vorbei,
 *     waehrend das TNC noch sendet.
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
 * UNPROTO-Sendeweg.
 *
 * TheFirmware (TNC2-Klasse) ignoriert auf hybriden Aufbauten oft
 * KISS-Datenrahmen, waehrend UNPROTO aus dem Kommandomodus den Traeger
 * zuverlaessig schaltet. Das ist genau das Symptom "PTT nur manchmal".
 *
 * Ablauf: KISS verlassen -> UNPROTO senden -> abwarten -> KISS wieder
 * betreten. Der Rahmen wird dabei ausgewertet, um Ziel und Text zu
 * bekommen; was sonst darin steckt, entfaellt.
 */
static int send_unproto(tncd_station *st, const unsigned char *frame, size_t len,
                        char *err, size_t errlen)
{
    /* AX.25-UI: Ziel(7) Quelle(7) Control(1) PID(1) Nutzdaten */
    if (len < 16) {
        snprintf(err, errlen, "Rahmen zu kurz");
        return -1;
    }

    char dst[16], src[16];
    if (!call_from_ax25(frame, dst, sizeof dst) ||
        !call_from_ax25(frame + 7, src, sizeof src)) {
        snprintf(err, errlen, "Rufzeichen nicht lesbar");
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
     * 1. KISS verlassen.
     *
     * ACHTUNG: C0 FF C0 setzt bei TheFirmware die FIRMWARE zurueck -
     * das ist der Grund, warum dabei der Banner erscheint und die LEDs
     * Status/Connected das Startmuster zeigen (2-3 Sekunden, siehe
     * Landolt-Handbuch). Nach dem Ruecksetz ist MYCALL weg, und
     * UNPROTO wuerde mit einer falschen Kennung senden.
     *
     * Darum: ausreichend warten UND die Kennung erneut setzen.
     */
    static const unsigned char leave[] = { 0xC0, 0xFF, 0xC0 };
    (void)pr_serial_write(&st->ser, leave, sizeof leave, e2, sizeof e2);

    /*
     * Warten, bis der Startvorgang WIRKLICH vorbei ist.
     *
     * Starr 3 Sekunden zu warten war zu wenig: der Banner laeuft dann
     * noch, und MYCALL/UNPROTO gingen mitten im Booten raus - das TNC
     * hat sie nicht verarbeitet, und es kam keine Sendung zustande.
     * Besser: den Auslauf abwarten, bis eine Weile nichts mehr kommt.
     */
    {
        unsigned char boot[2048];
        long total = 0;
        for (int round = 0; round < 12; round++) {
            long got = pr_serial_read_quiet(&st->ser, boot, sizeof boot,
                                           1000, 350, e2, sizeof e2);
            if (got <= 0)
                break;
            total += got;
        }
        (void)total;
    }
    usleep(500000);

    /* 2. MYCALL wieder setzen - sonst sendet UNPROTO mit der falschen
     *    Kennung auf die Luft. */
    {
        char myc[32];
        snprintf(myc, sizeof myc, "%.1sI %.9s\r", "\x1b", st->cfg.callerid);
        (void)pr_serial_write(&st->ser, myc, strlen(myc), e2, sizeof e2);
        usleep(400000);
        unsigned char jj[128];
        (void)pr_serial_read_quiet(&st->ser, jj, sizeof jj, 400, 150, e2, sizeof e2);
    }

    /* 3. UNPROTO <Ziel> 0 <Text> */
    char cmd[PR_MSG_TEXT + 64];
    int k = snprintf(cmd, sizeof cmd, "UNPROTO %.9s 0 %s\r", dst, text);
    if (k <= 0 || (size_t)k >= sizeof cmd) {
        snprintf(err, errlen, "Nachricht zu lang");
        return -1;
    }
    if (pr_serial_write(&st->ser, cmd, (size_t)k, err, errlen) != 0)
        return -1;
    tx_finish(&st->ser);

    /* 4. abwarten, bis das TNC gesendet hat */
    usleep(1200000);

    /* 5. KISS wieder betreten (inkl. Parameter) */
    {
        static const unsigned char kiss_on[] = { 0x1B, 0x40, 0x4B };
        if (pr_str_eq_ci(st->cfg.kiss_init, "tapr")) {
            static const unsigned char ko[] = { 'k','i','s','s',' ','o','n','\r' };
            (void)pr_serial_write(&st->ser, ko, sizeof ko, e2, sizeof e2);
        } else {
            (void)pr_serial_write(&st->ser, kiss_on, sizeof kiss_on, e2, sizeof e2);
        }
        usleep(300000);
        static const struct { unsigned char cmd, val; } kp[] = {
            { 0x01, 50 }, { 0x02, 255 }, { 0x03, 10 }, { 0x04, 1 }, { 0x05, 0 }
        };
        for (size_t i = 0; i < sizeof kp / sizeof kp[0]; i++) {
            unsigned char f[4] = { 0xC0, kp[i].cmd, kp[i].val, 0xC0 };
            (void)pr_serial_write(&st->ser, f, 4, e2, sizeof e2);
        }
    }
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

    /* Platz schaffen */
    if (st->rx_len + (size_t)n > sizeof st->rx) {
        size_t drop = st->rx_len + (size_t)n - sizeof st->rx;
        memmove(st->rx, st->rx + drop, st->rx_len - drop);
        st->rx_len -= drop;
    }
    memcpy(st->rx + st->rx_len, buf, (size_t)n);
    st->rx_len += (size_t)n;
}

/* ---- Befehle -------------------------------------------------------- */

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
        /* Gegenstelle weg - wird beim naechsten poll() auffallen */
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
            answer(fd, "ERR Geraet nicht offen");
            return;
        }
        unsigned char data[4200];
        size_t n = pr_tncsock_hex_decode(data, sizeof data, arg);
        if (n == 0) {
            answer(fd, "ERR keine Daten");
            return;
        }
        char err[256];
        if (pr_str_eq_ci(st->cfg.tx_mode, "unproto")) {
            if (send_unproto(st, data, n, err, sizeof err) != 0) {
                answer(fd, "ERR %.200s", err);
                return;
            }
            answer(fd, "OK %zu (unproto)", n);
            return;
        }
        tx_prepare(&st->ser);
        /*
         * PERSIST vor JEDEM Rahmen erneut setzen - so macht es der
         * MAX25-Stack. Gehen die Parameter verloren (etwa nach einem
         * ungewollten Ruecksetz), waere sonst die Sendung unzuverlaessig.
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
               st->kiss_ok ? "ja" : "nein",
               st->open ? "ja" : "nein",
               st->cfg.port);

    } else if (pr_str_eq_ci(cmd, PR_TNC_CMD_CHECKUP)) {
        if (!st->open) {
            answer(fd, "ERR Geraet nicht offen");
            return;
        }
        char err[256];
        if (!enter_kiss(st, err, sizeof err)) {
            answer(fd, "ERR %.200s", err);
            return;
        }
        answer(fd, "OK");

    } else {
        answer(fd, "ERR unbekannter Befehl %.60s", cmd);
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
        snprintf(err, errlen, "Socketpfad zu lang");
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
    /* Nur der Webserver-Zugriff braucht die Datei */
    (void)chmod(path, 0660);

    if (listen(fd, 4) != 0) {
        snprintf(err, errlen, "listen(%s): %s", path, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

/* ---- Hauptschleife --------------------------------------------------- */

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

            /* Empfang vom Geraet einsammeln */
            if (st->open)
                station_pump(st);

            /* Neue Verbindung annehmen und sofort abarbeiten */
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
         * KISS periodisch nachhalten.
         *
         * WICHTIG: die Nachpruefung darf den Betrieb NICHT stoeren.
         * enter_kiss() braucht rund 3 Sekunden und schreibt dabei auf
         * den Port. Fiel das in eine Sendefolge, ging genau diese
         * Sendung verloren - man sah nur einzelne PTT statt aller.
         *
         * Deshalb: nur nachhalten, wenn seit der letzten Sendung genug
         * Ruhe war, und grundsaetzlich selten. Der Daemon haelt den Port
         * offen, KISS geht darum nicht von selbst verloren.
         */
        for (size_t i = 0; i < nst; i++) {
            tncd_station *st = &stations[i];
            if (!st->open)
                continue;
            time_t idle = time(NULL) - st->last_check;
            if (idle > 600 && st->rx_len == 0) {
                (void)enter_kiss(st, err, sizeof err);
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
    printf("prterm-tncd - haelt die TNC-Ports offen und fuehrt KISS\n\n");
    printf("Aufruf:\n");
    printf("  prterm-tncd [PRTERM.INI]\n\n");
    printf("Je Station wird ein Unix-Socket gelegt:\n");
    printf("  <runtime_dir>/tnc-<station>.sock\n\n");
    printf("Der Daemon bleibt im Vordergrund. Fuer den Dauerbetrieb\n");
    printf("uebernimmt systemd den Start (siehe docs/).\n");
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
        fprintf(stderr, "FEHLER: %s\n", err);
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
            /* ueber einen Zwischenpuffer - sonst meckert der Compiler
             * wegen ueberlappender Zielobjekte im selben Struct */
            char tmp[512];
            snprintf(tmp, sizeof tmp, "%.400s/tnc-%.32s.sock",
                     st->cfg.runtime_dir, st->name);
            pr_strlcpy(st->socket_path, tmp, sizeof st->socket_path);
        }
        st->listen_fd = -1;

        printf("  %-10s %s\n", st->name, st->cfg.port);

        if (!station_open(st, err, sizeof err)) {
            fprintf(stderr, "FEHLER %s: %s\n", st->name, err);
            continue;
        }
        if (!enter_kiss(st, err, sizeof err)) {
            fprintf(stderr, "FEHLER %s: %s\n", st->name, err);
            pr_serial_close(&st->ser);
            continue;
        }

        st->listen_fd = make_listener(st->socket_path, err, sizeof err);
        if (st->listen_fd < 0) {
            fprintf(stderr, "FEHLER %s: %s\n", st->name, err);
            pr_serial_close(&st->ser);
            continue;
        }

        printf("  %-10s KISS aktiv, Socket %s\n", st->name, st->socket_path);
        nst++;
    }

    if (nst == 0) {
        fprintf(stderr, "FEHLER: keine Station konnte gestartet werden\n");
        pr_config_free(&base);
        return 1;
    }

    printf("\nprterm-tncd laeuft - %zu Station(en), Strg+C beendet\n", nst);
    fflush(stdout);

    int rc = run_daemon(stations, nst);

    for (size_t i = 0; i < nst; i++) {
        if (stations[i].open)
            pr_serial_close(&stations[i].ser);
    }
    pr_config_free(&base);
    return rc;
}
