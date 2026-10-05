/*
 * PRTERM - CB & Amateur Radio Terminal
 * max25.c - Treiber fuer MAX25-Stack-TNCs ueber das M25/1-Zeilenprotokoll.
 *
 * M25/1 ist zeilenorientiert, ein Kommando je \n. Die Keywords sind
 * case-sensitive (Ausnahme: die Flags von SET AX25_UI).
 *
 * Ablauf beim Aufbau:
 *   <- OK
 *   <- STATUS ...
 *   <- AUTH required        (nur bei gesetztem tcp_password)
 *   -> AUTH <password>
 *
 * Kommandos:
 *   PING | GET STATUS | GET DEVICES
 *   SET DEVICE <id> | SET CALLERID <id> | SET CALLID <id> | SET AX25_UI on|off
 *   CONNECT | DISCONNECT | SEND <text> | MONITOR on|off
 *   BAN <call> | UNBAN <call> | BANS
 *
 * Antworten:
 *   OK | ERR <msg> | STATUS ... | DEVICE ... | RX device=<id> <text>
 *   EVENT connected|disconnected
 *
 * SPDX-License-Identifier: MIT
 */
#include "prterm_compat.h"

#include "radio.h"
#include "state.h"
#include "util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define MAX25_LINE 2048
#define MAX25_MAX_PENDING 32

typedef struct max25_impl {
    int          fd;
    char         endpoint[256];
    bool         is_unix;

    char         rbuf[MAX25_LINE];
    size_t       rlen;

    pr_rig_state st;
    pr_msg       pending[MAX25_MAX_PENDING];
    size_t       npending;

    char         device[64];
    char         callid[16];
} max25_impl;

/* ======================================================================= */
/* Verbindung                                                              */
/* ======================================================================= */

static void max25_note(pr_rig *r, char kind, const char *from, const char *text)
{
    pr_msg m;
    memset(&m, 0, sizeof m);
    m.kind = kind;
    pr_strlcpy(m.from, from, sizeof m.from);
    pr_strlcpy(m.text, text, sizeof m.text);
    m.ts = pr_now_s();
    char err[128];
    (void)pr_log_append(r->cfg, &m, err, sizeof err);
}

static void max25_save(pr_rig *r, max25_impl *m)
{
    char err[128];
    (void)pr_state_save(r->cfg, &m->st, err, sizeof err);
}

static void max25_push_rx(max25_impl *m, const char *from, const char *text)
{
    if (m->npending >= MAX25_MAX_PENDING) {
        memmove(&m->pending[0], &m->pending[1],
                (MAX25_MAX_PENDING - 1) * sizeof m->pending[0]);
        m->npending = MAX25_MAX_PENDING - 1;
    }
    pr_msg *p = &m->pending[m->npending++];
    memset(p, 0, sizeof *p);
    p->kind = PR_MSG_RX;
    pr_strlcpy(p->from, from, sizeof p->from);
    pr_strlcpy(p->text, text, sizeof p->text);
    p->db = m->st.rx_db;
    p->ts = pr_now_s();
}

static int max25_connect_unix(const char *path, char *err, size_t errlen)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        snprintf(err, errlen, "Socket: %s", strerror(errno));
        return -1;
    }

    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    pr_strlcpy(sa.sun_path, path, sizeof sa.sun_path);

    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        snprintf(err, errlen, "kann %s nicht erreichen: %s", path, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static int max25_connect_tcp(const char *host, int port, char *err, size_t errlen)
{
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);

    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
        snprintf(err, errlen, "ungueltige Adresse \"%s\"", host);
        return -1;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        snprintf(err, errlen, "Socket: %s", strerror(errno));
        return -1;
    }
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        snprintf(err, errlen, "%s:%d nicht erreichbar: %s", host, port, strerror(errno));
        close(fd);
        return -1;
    }

    int on = 1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
    return fd;
}

/* ======================================================================= */
/* Zeilenweise lesen und schreiben                                         */
/* ======================================================================= */

static int max25_send_line(max25_impl *m, const char *line, char *err, size_t errlen)
{
    size_t n = strlen(line);
    char buf[MAX25_LINE];
    if (n + 2 > sizeof buf) {
        snprintf(err, errlen, "Kommando zu lang");
        return -1;
    }
    memcpy(buf, line, n);
    buf[n++] = '\n';

    size_t done = 0;
    while (done < n) {
        ssize_t k = send(m->fd, buf + done, n - done, MSG_NOSIGNAL);
        if (k > 0) {
            done += (size_t)k;
            continue;
        }
        if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pfd = { m->fd, POLLOUT, 0 };
            if (poll(&pfd, 1, 1000) <= 0) {
                snprintf(err, errlen, "Schreibzeitlimit");
                return -1;
            }
            continue;
        }
        snprintf(err, errlen, "Schreibfehler: %s", strerror(errno));
        return -1;
    }
    return 0;
}

/* Liest eine Zeile ohne \n. Liefert 0 bei Erfolg, 1 bei Zeitlimit, -1 bei Fehler. */
static int max25_read_line(max25_impl *m, char *out, size_t outcap,
                           int timeout_ms, char *err, size_t errlen)
{
    for (;;) {
        /* Puffer nach Zeilen absuchen */
        for (size_t i = 0; i < m->rlen; i++) {
            if (m->rbuf[i] != '\n')
                continue;
            size_t n = i;
            if (n > 0 && m->rbuf[n - 1] == '\r')
                n--;
            if (n >= outcap)
                n = outcap - 1;
            memcpy(out, m->rbuf, n);
            out[n] = '\0';

            memmove(m->rbuf, m->rbuf + i + 1, m->rlen - i - 1);
            m->rlen -= i + 1;
            return 0;
        }

        struct pollfd pfd = { m->fd, POLLIN, 0 };
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr == 0)
            return 1;
        if (pr < 0) {
            if (errno == EINTR)
                return 1;
            snprintf(err, errlen, "poll: %s", strerror(errno));
            return -1;
        }

        if (m->rlen >= sizeof m->rbuf - 1) {
            snprintf(err, errlen, "Zeile zu lang");
            return -1;
        }
        ssize_t k = recv(m->fd, m->rbuf + m->rlen, sizeof m->rbuf - 1 - m->rlen, 0);
        if (k == 0) {
            snprintf(err, errlen, "Verbindung geschlossen");
            return -1;
        }
        if (k < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                continue;
            snprintf(err, errlen, "Lesefehler: %s", strerror(errno));
            return -1;
        }
        m->rlen += (size_t)k;
    }
}

/* ======================================================================= */
/* Protokollzeilen auswerten                                                */
/* ======================================================================= */

static void max25_push(max25_impl *m, const char *from, const char *text)
{
    max25_push_rx(m, from, text);
    m->st.rx_count++;
    pr_strlcpy(m->st.last_rx_from, from, sizeof m->st.last_rx_from);
    pr_strlcpy(m->st.last_rx_text, text, sizeof m->st.last_rx_text);
    m->st.last_rx_ts = pr_now_s();
}

/*
 * Wertet eine Protokollzeile aus. RX-Zeilen tragen die Nachricht:
 *   RX device=<id> <text>
 */
static void max25_handle_line(max25_impl *m, const char *line)
{
    if (pr_starts_with(line, "RX ")) {
        const char *p = line + 3;
        const char *sp = strchr(p, ' ');
        if (sp == NULL)
            return;

        /* device=<id> ueberspringen */
        const char *text = sp + 1;
        char dev[64];
        size_t n = (size_t)(sp - p);
        if (n >= sizeof dev) n = sizeof dev - 1;
        memcpy(dev, p, n);
        dev[n] = '\0';

        char from[16];
        pr_strlcpy(from, m->callid[0] != '\0' ? m->callid : "?", sizeof from);
        max25_push(m, from, text);
        return;
    }

    if (pr_starts_with(line, "STATUS ")) {
        /* STATUS hardware=... device=... mode=... callerid=... callid=...
         *      ax25_ui=... connected=... stack=... serial=...
         *      error=valid|invalid voice=valid|invalid */
        static const char *const keys[] = {
            "device=", "callerid=", "callid=", "connected=",
            "stack=", "serial=", "error=", "voice="
        };
        for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
            const char *f = strstr(line, keys[i]);
            if (f == NULL)
                continue;
            f += strlen(keys[i]);
            char val[64];
            size_t w = 0;
            while (*f != '\0' && *f != ' ' && w + 1 < sizeof val)
                val[w++] = *f++;
            val[w] = '\0';

            if (strcmp(keys[i], "device=") == 0)      pr_strlcpy(m->device, val, sizeof m->device);
            else if (strcmp(keys[i], "callid=") == 0) pr_strlcpy(m->callid, val, sizeof m->callid);
            else if (strcmp(keys[i], "connected=") == 0) m->st.link_ok = (strcmp(val, "true") == 0);
            else if (strcmp(keys[i], "error=") == 0)  m->st.squelch_open = (strcmp(val, "valid") == 0);
        }
    }
}

/* ======================================================================= */
/* VTable                                                                  */
/* ======================================================================= */

static int max25_open(pr_rig *r, char *err, size_t errlen)
{
    const pr_config *cfg = r->cfg;

    max25_impl *m = calloc(1, sizeof *m);
    if (m == NULL) {
        snprintf(err, errlen, "Speicher erschoepft");
        return -1;
    }
    m->fd = -1;
    pr_strlcpy(m->endpoint, cfg->port, sizeof m->endpoint);

    /*
     * Endpunkt: Unix-Socket wenn der Pfad mit / beginnt und kein
     * Doppelpunkt vorkommt, sonst host:port.
     */
    if (cfg->port[0] == '/' && strchr(cfg->port, ':') == NULL) {
        m->is_unix = true;
        m->fd = max25_connect_unix(cfg->port, err, errlen);
    } else {
        char host[128];
        pr_strlcpy(host, cfg->port, sizeof host);
        char *colon = strchr(host, ':');
        int port = 7325;
        if (colon != NULL) {
            *colon = '\0';
            port = atoi(colon + 1);
            if (port <= 0) port = 7325;
        }
        m->fd = max25_connect_tcp(host, port, err, errlen);
    }

    if (m->fd < 0) {
        free(m);
        return -1;
    }

    if (pr_state_load(cfg, &m->st, err, errlen) != 0) {
        close(m->fd);
        free(m);
        return -1;
    }

    pr_strlcpy(m->st.device, cfg->port, sizeof m->st.device);
    pr_strlcpy(m->st.status, "running", sizeof m->st.status);
    pr_strlcpy(m->st.detail, "M25/1", sizeof m->st.detail);
    m->st.duplex = cfg->duplex;
    pr_strlcpy(m->callid, cfg->callerid, sizeof m->callid);

    /* Begruesung: OK + STATUS, bei Bedarf AUTH */
    char line[MAX25_LINE];
    for (int i = 0; i < 4; i++) {
        if (max25_read_line(m, line, sizeof line, 1500, err, errlen) != 0)
            break;
        max25_handle_line(m, line);
        if (strcmp(line, "AUTH required") == 0) {
            char cmd[384];
            snprintf(cmd, sizeof cmd, "AUTH %.160s", cfg->admin_pass_hash);
            /* Klartextpasswort erwartet - siehe docs/PROTOCOL.md */
            (void)max25_send_line(m, cmd, err, errlen);
        }
    }

    /* Eigene Identitaet setzen */
    if (cfg->callerid[0] != '\0') {
        char cmd[64];
        snprintf(cmd, sizeof cmd, "SET CALLERID %s", cfg->callerid);
        (void)max25_send_line(m, cmd, err, errlen);
    }

    r->impl = m;

    /* Nur beim ersten Start melden - siehe Kommentar in tnc2.c. */
    {
        char state_file[640];
        pr_state_path(cfg, state_file, sizeof state_file);
        if (!pr_file_exists(state_file))
            max25_note(r, PR_MSG_SYS, "SYS", "MAX25-Stack angebunden (M25/1)");
    }
    return 0;
}

static void max25_close(pr_rig *r)
{
    max25_impl *m = r->impl;
    if (m == NULL)
        return;
    max25_save(r, m);
    if (m->fd >= 0) {
        (void)max25_send_line(m, "DISCONNECT", (char[64]){ 0 }, 64);
        close(m->fd);
    }
    free(m);
    r->impl = NULL;
}

static int max25_refresh(pr_rig *r, char *err, size_t errlen)
{
    max25_impl *m = r->impl;
    if (m == NULL) {
        snprintf(err, errlen, "MAX25 nicht verbunden");
        return -1;
    }

    m->st.rx_muted = (m->st.duplex == PR_DUPLEX_HALF) && m->st.ptt;
    if (m->st.rx_muted)
        return 0;

    /* Anliegende Protokollzeilen auswerten */
    char line[MAX25_LINE];
    for (int i = 0; i < 32; i++) {
        int rc = max25_read_line(m, line, sizeof line, 20, err, errlen);
        if (rc == 1)
            break;                      /* nichts mehr */
        if (rc < 0)
            return -1;
        max25_handle_line(m, line);
    }
    return 0;
}

static int max25_get_state(pr_rig *r, pr_rig_state *out)
{
    max25_impl *m = r->impl;
    if (m == NULL)
        return -1;
    *out = m->st;
    return 0;
}

static int max25_simple_cmd(pr_rig *r, const char *cmd, char *err, size_t errlen)
{
    max25_impl *m = r->impl;
    if (m == NULL) {
        snprintf(err, errlen, "MAX25 nicht verbunden");
        return -1;
    }
    return max25_send_line(m, cmd, err, errlen);
}

static int max25_set_freq(pr_rig *r, long freq_hz, char *err, size_t errlen)
{
    max25_impl *m = r->impl;
    if (m == NULL) {
        snprintf(err, errlen, "MAX25 nicht verbunden");
        return -1;
    }
    (void)errlen;
    m->st.freq_hz = freq_hz;
    max25_save(r, m);
    return 0;
}

static int max25_set_mode(pr_rig *r, unsigned mode, char *err, size_t errlen)
{
    max25_impl *m = r->impl;
    if (m == NULL) {
        snprintf(err, errlen, "MAX25 nicht verbunden");
        return -1;
    }
    (void)errlen;
    m->st.mode = mode;
    max25_save(r, m);
    return 0;
}

static int max25_set_ptt(pr_rig *r, bool on, char *err, size_t errlen)
{
    max25_impl *m = r->impl;
    if (m == NULL) {
        snprintf(err, errlen, "MAX25 nicht verbunden");
        return -1;
    }
    if (on && m->st.monitor) {
        snprintf(err, errlen, "Monitorbetrieb: Senden ist gesperrt");
        return -1;
    }
    (void)errlen;
    m->st.ptt = on;
    m->st.rx_muted = (m->st.duplex == PR_DUPLEX_HALF) && on;
    max25_save(r, m);
    return 0;
}

static int max25_set_duplex(pr_rig *r, pr_duplex d, char *err, size_t errlen)
{
    max25_impl *m = r->impl;
    if (m == NULL) {
        snprintf(err, errlen, "MAX25 nicht verbunden");
        return -1;
    }
    m->st.duplex = d;
    m->st.rx_muted = (d == PR_DUPLEX_HALF) && m->st.ptt;
    max25_save(r, m);
    return max25_simple_cmd(r, d == PR_DUPLEX_FULL ? "SET AX25_UI on"
                                                   : "SET AX25_UI on",
                            err, errlen);
}

static int max25_set_monitor(pr_rig *r, bool on, char *err, size_t errlen)
{
    max25_impl *m = r->impl;
    if (m == NULL) {
        snprintf(err, errlen, "MAX25 nicht verbunden");
        return -1;
    }
    if (on) {
        m->st.ptt = false;
        m->st.rx_muted = false;
    }
    m->st.monitor = on;
    max25_save(r, m);

    char cmd[32];
    snprintf(cmd, sizeof cmd, "MONITOR %s", on ? "on" : "off");
    return max25_simple_cmd(r, cmd, err, errlen);
}

static int max25_send(pr_rig *r, const char *from, const char *text,
                      char *err, size_t errlen)
{
    max25_impl *m = r->impl;
    if (m == NULL) {
        snprintf(err, errlen, "MAX25 nicht verbunden");
        return -1;
    }
    if (m->st.monitor) {
        snprintf(err, errlen, "Monitorbetrieb: Senden ist gesperrt");
        return -1;
    }
    if (text == NULL || text[0] == '\0') {
        snprintf(err, errlen, "leere Nachricht");
        return -1;
    }

    /* Quellrufzeichen setzen, damit die Identifikation stimmt */
    if (from != NULL && from[0] != '\0') {
        char cmd[64];
        snprintf(cmd, sizeof cmd, "SET CALLERID %s", from);
        if (max25_send_line(m, cmd, err, errlen) != 0)
            return -1;
    }

    char cmd[PR_MSG_TEXT + 16];
    snprintf(cmd, sizeof cmd, "SEND %s", text);
    if (max25_send_line(m, cmd, err, errlen) != 0)
        return -1;

    max25_note(r, PR_MSG_TX, from, text);
    m->st.tx_count++;
    m->st.last_tx_ts = pr_now_s();
    max25_save(r, m);
    return 0;
}

static int max25_drain(pr_rig *r, pr_msg *out, size_t cap, size_t *n)
{
    max25_impl *m = r->impl;
    if (m == NULL) {
        *n = 0;
        return -1;
    }
    size_t k = 0;
    while (k < cap && m->npending > 0) {
        out[k++] = m->pending[0];
        memmove(&m->pending[0], &m->pending[1],
                (m->npending - 1) * sizeof m->pending[0]);
        m->npending--;
    }
    *n = k;
    return 0;
}

const pr_rig_vtbl pr_rig_max25 = {
    "max25",
    "MAX25-Stack ueber M25/1 (TCP oder Unix-Socket)",
    max25_open,
    max25_close,
    max25_refresh,
    max25_get_state,
    max25_set_freq,
    max25_set_mode,
    max25_set_ptt,
    max25_set_duplex,
    max25_set_monitor,
    max25_send,
    max25_drain
};
