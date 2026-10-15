/*
 * PRTERM - CB & Amateur Radio Terminal
 * tmodem.c - Driver for the T-Modem half-TNC class (c1224, s1224).
 *
 * Protocol: KISS over USB CDC. The device is a Raspberry Pi Pico that
 * presents a CDC ACM serial port (/dev/ttyACM0 on Linux, /dev/cuaU0 on
 * FreeBSD). HDLC + KISS framing live in the firmware; AFSK lives on a
 * TCM3105-class modem chip (c1224) or in software (s1224, PWM out /
 * ADC in). It is a half-TNC, not a BayCom derivative.
 *
 * Field experience that shapes this driver (verified 2026-10-14,
 * see CONTINUE.md section 16):
 *
 *   - THE DEVICE IS ALWAYS IN KISS MODE. There is no command mode to
 *     leave and no entry sequence ("ESC @K" / "kiss on" do not exist).
 *     The station therefore runs with kiss_init = native: prterm-tncd
 *     opens the port, writes NOTHING at startup and holds the link.
 *     This driver never touches the device - it talks KISS frames to
 *     the daemon over the station socket, exactly like tnc2.c.
 *
 *   - THE FIRMWARE VALIDATES THE FCS ITSELF AND STRIPS IT. A received
 *     frame arrives as AX.25 WITHOUT the FCS trailer (firmware hdlc.c:
 *     CRC check on the air frame, then a copy of len-2 bytes). This is
 *     the opposite of TheFirmware/TNC2C, which delivers frames WITH
 *     FCS. A driver that checked and stripped here would drop every
 *     single received frame - do not "unify" this with tnc2.
 *
 *   - TX: the host sends the AX.25 frame WITHOUT FCS; the firmware
 *     appends the FCS during HDLC encoding (same convention as the
 *     TNC2 class on the wire).
 *
 *   - PTT keys on any KISS DATA frame. There is no MYCALL gate as in
 *     TheFirmware, and no CSMA parameter set - the firmware has none.
 *
 *   - The USB CDC also carries an ASCII service channel (HELP, STATUS,
 *     ID, PTT ON/OFF, INI...). It is only parsed when the line does
 *     not sit inside a KISS frame, so both can share the port. The
 *     KISS decoder here simply ignores those lines; with the firmware
 *     default quiet=1 there are no unsolicited lines anyway.
 *
 *   - KISS parameter frames (TXDELAY/PERSIST/... / FULLDUPLEX) are
 *     silently ignored by the firmware (only port 0 DATA is acted on).
 *     Writing them would be harmless but pointless - this driver
 *     deliberately writes nothing but DATA frames.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "prterm_compat.h"

#include "radio.h"
#include "callsign.h"
#include "kiss.h"
#include "lang.h"
#include "serial.h"
#include "tncsock.h"
#include "state.h"
#include "trace.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TMODEM_MAX_PENDING 32

typedef struct tmodem_impl {
    pr_tncsock   sock;   /* to the daemon that keeps the port open */
    pr_rig_state st;
    kiss_decoder dec;

    pr_msg       pending[TMODEM_MAX_PENDING];
    size_t       npending;

    char         mycall[16];
    /*
     * Name of the station this device belongs to. Taken from the
     * configuration at open time, so every received message can name
     * its device - the "All" view shows that.
     */
    char         station[32];
} tmodem_impl;

/* ======================================================================= */
/* Save state                                                              */
/* ======================================================================= */

static void tmodem_note(pr_rig *r, char kind, const char *from,
                        const char *text)
{
    pr_msg m;
    memset(&m, 0, sizeof m);
    m.kind = kind;
    pr_strlcpy(m.from, from, sizeof m.from);
    pr_strlcpy(m.text, text, sizeof m.text);
    /*
     * Own entries carry the device too - that is what the device
     * filter of the RX/TX menu needs to show a transmission under
     * the device that sent it.
     */
    if (r->impl != NULL) {
        tmodem_impl *t = r->impl;
        pr_strlcpy(m.station, t->station, sizeof m.station);
    }
    m.ts = pr_now_s();
    char err[128];
    (void)pr_log_append(r->cfg, &m, err, sizeof err);
}

static void tmodem_save(pr_rig *r, tmodem_impl *t)
{
    char err[128];
    (void)pr_state_save(r->cfg, &t->st, err, sizeof err);
}

/* ======================================================================= */
/* Capturing a received message                                            */
/* ======================================================================= */

static void tmodem_push_rx(tmodem_impl *t, const char *to, const char *from,
                           const char *text, int db)
{
    if (t->npending >= TMODEM_MAX_PENDING) {
        memmove(&t->pending[0], &t->pending[1],
                (TMODEM_MAX_PENDING - 1) * sizeof t->pending[0]);
        t->npending = TMODEM_MAX_PENDING - 1;
    }

    pr_msg *m = &t->pending[t->npending++];
    memset(m, 0, sizeof *m);
    m->kind = PR_MSG_RX;
    pr_strlcpy(m->from, from, sizeof m->from);
    if (to != NULL)
        pr_strlcpy(m->to, to, sizeof m->to);
    /* Which device caught it - that is what "All" shows as origin. */
    pr_strlcpy(m->station, t->station, sizeof m->station);
    pr_strlcpy(m->text, text, sizeof m->text);

    /* Sanitize: strip control characters from received text */
    for (unsigned char *p = (unsigned char *)m->text; *p; p++)
        if (*p < 0x20 || *p == 0x7f) *p = ' ';
    /* Trim trailing spaces */
    size_t tlen = strlen(m->text);
    while (tlen > 0 && m->text[tlen-1] == ' ') m->text[--tlen] = '\0';
    m->db = db;
    m->ts = pr_now_s();
}

/*
 * One received KISS DATA frame = one AX.25 frame.
 *
 * Layout: dest (7) | src (7) | Control (1) | PID (1) | info
 *
 * THE T-Modem FIRMWARE DELIVERS THE FRAME WITHOUT FCS - it checked the
 * CRC on the air frame itself and stripped the trailer (see the file
 * header). So there is deliberately NO kiss_fcs_ok() check here, unlike
 * tnc2.c where TheFirmware passes the trailer through.
 *
 * Only UI frames with no-layer-3 PID are interesting here. Everything
 * else (connected traffic, digipeater hops) is not part of this
 * terminal and is dropped deliberately.
 */
static void tmodem_handle_frame(tmodem_impl *t, const unsigned char *frame,
                                size_t len)
{
    if (len < 16)
        return;

    char to[16], from[16];
    if (!call_from_ax25(frame, to, sizeof to))
        return;
    if (!call_from_ax25(frame + 7, from, sizeof from))
        return;

    unsigned char ctrl = frame[14];
    unsigned char pid  = frame[15];

    /* Only UI frames */
    if (ctrl != 0x03u)
        return;
    if (pid != 0xF0u)
        return;

    char text[PR_MSG_TEXT];
    size_t n = len - 16;
    if (n >= sizeof text)
        n = sizeof text - 1;
    memcpy(text, frame + 16, n);
    text[n] = '\0';

    tmodem_push_rx(t, to, from, text, t->st.rx_db);
    t->st.rx_count++;
    pr_strlcpy(t->st.last_rx_from, from, sizeof t->st.last_rx_from);
    pr_strlcpy(t->st.last_rx_text, text, sizeof t->st.last_rx_text);
    t->st.last_rx_ts = pr_now_s();
}


/* ======================================================================= */
/* VTable                                                                  */
/* ======================================================================= */

static int tmodem_open(pr_rig *r, char *err, size_t errlen)
{
    const pr_config *cfg = r->cfg;

    int databits = 8, parity = PR_PAR_NONE, stopbits = 1;
    if (!pr_serial_parse_line(cfg->serial_line, &databits, &parity, &stopbits)) {
        pr_trf(err, errlen, "line format \"%s\" is invalid (e.g. 8n1)",
                 cfg->serial_line);
        return -1;
    }

    tmodem_impl *t = calloc(1, sizeof *t);
    if (t == NULL) {
        pr_trf(err, errlen, "out of memory");
        return -1;
    }

    /*
     * The port is NOT opened here. prterm-tncd does that, keeping it
     * open permanently - a CGI that opened the port itself would race
     * the daemon for every received byte.
     */
    {
        char sock[512];
        snprintf(sock, sizeof sock, "%.400s/tnc-%.32s.sock",
                 cfg->runtime_dir,
                 cfg->active_station[0] ? cfg->active_station : "default");
        pr_trace_init((pr_trace_level)cfg->debug_level, "cgi");
        pr_trace(PR_TR_DEBUG, "tmodem open - daemon socket %s", sock);
        if (pr_tncsock_open(&t->sock, sock, err, errlen) != 0) {
            free(t);
            return -1;
        }
    }

    /* The daemon's own view of the link - makes a lost KISS visible
     * on every request instead of only in the journal. */
    {
        char out[PR_TNCSOCK_MAX_LINE], e3[128];
        if (pr_tncsock_cmd(&t->sock, PR_TNC_CMD_STATUS, out, sizeof out,
                           e3, sizeof e3) == 0) {
            pr_trace(PR_TR_DEBUG, "daemon: %s", out);
            if (strstr(out, "kiss=lost") != NULL)
                pr_trace(PR_TR_WARN,
                         "daemon reports KISS lost - watch will repair");
        } else {
            pr_trace(PR_TR_WARN, "daemon STATUS failed: %s", e3);
        }
    }
    (void)databits; (void)parity; (void)stopbits;

    kiss_decoder_init(&t->dec);

    /*
     * DELIBERATELY NO commands to the device.
     *
     * The T-Modem is in KISS mode from power-up and never leaves it.
     * A "STATUS" or "ID" line written here would be answered on the
     * shared CDC and would pollute the frame stream of the daemon's
     * RX buffer. Opening a device must not change its state.
     */

    if (pr_state_load(cfg, &t->st, err, errlen) != 0) {
        pr_tncsock_close(&t->sock);
        free(t);
        return -1;
    }

    pr_strlcpy(t->st.device, cfg->port, sizeof t->st.device);
    pr_strlcpy(t->st.status, "running", sizeof t->st.status);
    pr_strlcpy(t->st.detail, "T-Modem / KISS half-TNC", sizeof t->st.detail);
    t->st.link_ok = true;
    t->st.duplex  = cfg->duplex;
    /* Per-station mode from the INI is authoritative (like duplex);
     * the compliance gate and the state view must agree with it. */
    t->st.mode    = cfg->mode;
    pr_strlcpy(t->mycall, cfg->callerid, sizeof t->mycall);
    pr_strlcpy(t->station, cfg->active_station, sizeof t->station);

    r->impl = t;

    /*
     * Report only on the VERY FIRST start. The port is reopened on
     * every CGI call - one message per call would flood the terminal.
     */
    {
        char state_file[640];
        pr_state_path(cfg, state_file, sizeof state_file);
        if (!pr_file_exists(state_file))
            tmodem_note(r, PR_MSG_SYS, "SYS", "T-Modem connected (KISS)");
    }
    return 0;
}

static void tmodem_close(pr_rig *r)
{
    tmodem_impl *t = r->impl;
    if (t == NULL)
        return;

    tmodem_save(r, t);
    pr_tncsock_close(&t->sock);
    free(t);
    r->impl = NULL;
}

static int tmodem_refresh(pr_rig *r, char *err, size_t errlen)
{
    tmodem_impl *t = r->impl;
    if (t == NULL) {
        pr_trf(err, errlen, "T-Modem not connected");
        return -1;
    }

    /* Half duplex: nothing is received while ptt=1    */
    t->st.rx_muted = (t->st.duplex == PR_DUPLEX_HALF) && t->st.ptt;

    if (t->st.rx_muted)
        return 0;

    /* Fetch received frames     */
    unsigned char buf[2048];
    long n = pr_tncsock_rx(&t->sock, buf, sizeof buf, err, errlen);
    if (n < 0)
        return -1;

    if (n > 0) {
        /*
         * The daemon holds the port - reception arrives as KISS
         * frames. Everything that is not framed (the one-shot boot
         * banner, stray ASCII service lines) is ignored by the
         * decoder on purpose.
         */
        pr_trace_hex(PR_TR_TRACE, "KISS<< (daemon)", buf, (size_t)n);
        size_t frames = kiss_decoder_feed_buf(&t->dec, buf, (size_t)n);
        if (frames > 0)
            pr_trace(PR_TR_DEBUG, "RX %zu KISS frame(s) decoded", frames);
        for (size_t i = 0; i < frames; i++) {
            unsigned char frame[KISS_FRAME_MAX];
            size_t flen = kiss_decoder_take(&t->dec, frame, sizeof frame);
            if (flen > 0)
                tmodem_handle_frame(t, frame, flen);
        }
    }
    return 0;
}

static int tmodem_get_state(pr_rig *r, pr_rig_state *out)
{
    tmodem_impl *t = r->impl;
    if (t == NULL)
        return -1;
    *out = t->st;
    return 0;
}

static int tmodem_set_freq(pr_rig *r, long freq_hz, char *err, size_t errlen)
{
    tmodem_impl *t = r->impl;
    if (t == NULL) {
        pr_trf(err, errlen, "T-Modem not connected");
        return -1;
    }
    (void)errlen;
    t->st.freq_hz = freq_hz;
    tmodem_save(r, t);
    return 0;
}

static int tmodem_set_mode(pr_rig *r, unsigned mode, char *err, size_t errlen)
{
    tmodem_impl *t = r->impl;
    if (t == NULL) {
        pr_trf(err, errlen, "T-Modem not connected");
        return -1;
    }
    (void)errlen;
    t->st.mode = mode;
    tmodem_save(r, t);
    return 0;
}

static int tmodem_set_ptt(pr_rig *r, bool on, char *err, size_t errlen)
{
    tmodem_impl *t = r->impl;
    if (t == NULL) {
        pr_trf(err, errlen, "T-Modem not connected");
        return -1;
    }

    if (on && t->st.monitor) {
        pr_trf(err, errlen, "monitor mode: transmitting is locked");
        return -1;
    }
    (void)errlen;

    /*
     * With KISS the hardware switches itself when a frame goes out.
     * Here we only note the state.
     */
    t->st.ptt = on;
    t->st.rx_muted = (t->st.duplex == PR_DUPLEX_HALF) && on;
    tmodem_save(r, t);
    return 0;
}

static int tmodem_set_duplex(pr_rig *r, pr_duplex d, char *err, size_t errlen)
{
    tmodem_impl *t = r->impl;
    if (t == NULL) {
        pr_trf(err, errlen, "T-Modem not connected");
        return -1;
    }
    (void)errlen;

    /*
     * The firmware has no KISS parameter set (only port 0 DATA is
     * acted on), so no FULLDUPLEX frame is written - duplex is a
     * property of the radio, not of the half-TNC. Local state only.
     */
    t->st.duplex = d;
    t->st.rx_muted = (d == PR_DUPLEX_HALF) && t->st.ptt;
    tmodem_save(r, t);
    return 0;
}

static int tmodem_set_monitor(pr_rig *r, bool on, char *err, size_t errlen)
{
    tmodem_impl *t = r->impl;
    if (t == NULL) {
        pr_trf(err, errlen, "T-Modem not connected");
        return -1;
    }
    (void)errlen;
    t->st.monitor = on;
    if (on) {
        t->st.ptt = false;
        t->st.rx_muted = false;
    }
    tmodem_save(r, t);
    return 0;
}

static int tmodem_send(pr_rig *r, const char *from, const char *to,
                       const char *text,
                       char *err, size_t errlen)
{
    tmodem_impl *t = r->impl;
    if (t == NULL) {
        pr_trf(err, errlen, "T-Modem not connected");
        return -1;
    }
    if (t->st.monitor) {
        pr_trf(err, errlen, "monitor mode: transmitting is locked");
        return -1;
    }
    if (text == NULL || text[0] == '\0') {
        pr_trf(err, errlen, "empty message");
        return -1;
    }

    /*
     * A source like "CALL@device" names the transmitting device for
     * the message log - the MailboxD beacon sends once per station and
     * the browser shows which one carried it.  AX.25 addresses have no
     * "@", so the frame goes out with the base call while the log
     * entry keeps the label.
     */
    char call[PR_CALLSIGN_MAX];
    {
        const char *at = strchr(from, '@');
        if (at != NULL && at != from) {
            size_t clen = (size_t)(at - from);
            if (clen >= sizeof call)
                clen = sizeof call - 1;
            memcpy(call, from, clen);
            call[clen] = '\0';
        } else {
            pr_strlcpy(call, from, sizeof call);
        }
    }

    /*
     * Build an AX.25 UI frame. "to" is the station to call; empty or
     * CQ stands for a broadcast without a fixed partner.
     */
    if (to == NULL || to[0] == '\0')
        to = "CQ";

    unsigned char ui[512];
    size_t uilen = ax25_ui_frame(ui, sizeof ui, call, to,
                                 (const unsigned char *)text, strlen(text));
    if (uilen == 0) {
        pr_trf(err, errlen, "frame could not be built");
        return -1;
    }
    pr_trace_hex(PR_TR_DEBUG, "AX25 UI frame", ui, uilen);

    /* KISS frame: FEND | 0x00 | escaped payload | FEND
     *
     * The FCS is NOT included - the T-Modem firmware computes and
     * appends it during HDLC encoding, exactly like the TNC2 class
     * convention on this wire.
     */
    unsigned char frame[640];
    size_t flen = kiss_encode(frame, sizeof frame, 0, KISS_CMD_DATA, ui, uilen);
    if (flen == 0) {
        pr_trf(err, errlen, "KISS frame too large");
        return -1;
    }
    pr_trace_hex(PR_TR_TRACE, "KISS>> DATA", frame, flen);

    if (pr_tncsock_tx(&t->sock, frame, flen, err, errlen) != 0) {
        pr_trace(PR_TR_ERROR, "TX failed: %s", err);
        return -1;
    }
    pr_trace(PR_TR_INFO, "TX %.9s -> %.9s: %zu bytes handed to the daemon",
             from, to, flen);

    tmodem_note(r, PR_MSG_TX, from, text);
    t->st.tx_count++;
    t->st.last_tx_ts = pr_now_s();
    tmodem_save(r, t);
    return 0;
}

/*
 * Test carrier for the admin area.
 *
 * The T-Modem keys the transmitter per KISS DATA frame and lets go
 * after the last one - there is no "carrier only" command and no
 * parameter set. The duration is therefore mapped via the NUMBER of
 * frames, each at the usual AX.25 size (PACLEN 256). A single
 * inflated frame would be rejected by the firmware; normal-sized
 * frames keep the transmitter cycling for the desired time.
 *
 * NOTE: the firmware unkeys between two frames (~20 ms), so the test
 * carrier is a rapid sequence of bursts, not one continuous keying.
 * That is honest for this hardware and visible on a wattmeter as a
 * train of deflections.
 *
 * The content is null padding, so meaningless.
 */
#define TMODEM_PACLEN 256

static int tmodem_carrier_test(pr_rig *r, unsigned seconds,
                               char *err, size_t errlen)
{
    tmodem_impl *t = r->impl;
    if (t == NULL) {
        pr_trf(err, errlen, "T-Modem not connected");
        return -1;
    }
    if (seconds == 0 || seconds > 10) {
        pr_trf(err, errlen, "duration must be between 1 and 10 seconds");
        return -1;
    }
    if (t->st.monitor) {
        pr_trf(err, errlen, "monitor mode: transmitting is locked");
        return -1;
    }

    long baud = r->cfg->radio_baud > 0 ? r->cfg->radio_baud : 1200;
    long bits_needed = (long)seconds * baud;
    long bytes_needed = bits_needed / 8;
    /* minus AX.25 header (16) per frame - no FCS on this path     */
    long per_frame = TMODEM_PACLEN + 16;
    long frames = (bytes_needed + per_frame - 1) / per_frame;
    if (frames < 1) frames = 1;
    if (frames > 40) frames = 40;
    pr_trace(PR_TR_DEBUG, "carrier test %u s at %ld baud - %ld frame(s)",
             seconds, baud, frames);

    unsigned char pad[TMODEM_PACLEN];
    memset(pad, 0, sizeof pad);

    for (long k = 0; k < frames; k++) {
        unsigned char ui[320];
        size_t uilen = ax25_ui_frame(ui, sizeof ui, r->cfg->callerid, "CQ",
                                     pad, TMODEM_PACLEN);
        if (uilen == 0) {
            pr_trf(err, errlen, "test frame could not be built");
            return -1;
        }

        unsigned char frame[700];
        size_t flen = kiss_encode(frame, sizeof frame, 0, KISS_CMD_DATA,
                                  ui, uilen);
        if (flen == 0) {
            pr_trf(err, errlen, "KISS frame too large");
            return -1;
        }

        if (pr_tncsock_tx(&t->sock, frame, flen, err, errlen) != 0) {
            pr_trace(PR_TR_ERROR, "carrier test frame %ld failed: %s",
                     k + 1, err);
            t->st.ptt = false;
            t->st.rx_muted = false;
            return -1;
        }
    }

    /*
     * With KISS the transmission ends with the last frame - the
     * T-Modem switches itself and lets go again.
     */
    t->st.ptt = false;
    t->st.rx_muted = false;

    tmodem_note(r, PR_MSG_TX, r->cfg->callerid,
              pr_trs("[empty test carrier]"));
    t->st.tx_count++;
    t->st.last_tx_ts = pr_now_s();
    tmodem_save(r, t);
    return 0;
}

static int tmodem_drain(pr_rig *r, pr_msg *out, size_t cap, size_t *n)
{
    tmodem_impl *t = r->impl;
    if (t == NULL) {
        *n = 0;
        return -1;
    }
    size_t k = 0;
    while (k < cap && t->npending > 0) {
        out[k++] = t->pending[0];
        memmove(&t->pending[0], &t->pending[1],
                (t->npending - 1) * sizeof t->pending[0]);
        t->npending--;
    }
    *n = k;
    return 0;
}

const pr_rig_vtbl pr_rig_tmodem = {
    "tmodem",
    "T-Modem half-TNC (c1224 / s1224) via USB CDC KISS",
    tmodem_open,
    tmodem_close,
    tmodem_refresh,
    tmodem_get_state,
    tmodem_set_freq,
    tmodem_set_mode,
    tmodem_set_ptt,
    tmodem_set_duplex,
    tmodem_set_monitor,
    tmodem_send,
    tmodem_carrier_test,
    tmodem_drain
};
