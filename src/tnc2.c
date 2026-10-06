/*
 * PRTERM - CB & Amateur Radio Terminal
 * tnc2.c - Driver for TNC2 class (Landolt TNC2C, PK-TNC2).
 *
 * Protocol: KISS over a serial line.
 *
 * Field experience that shapes this driver:
 *
 *   - The daemon (prterm-tncd) holds the port and holds KISS. This
 *     driver never touches the device - it talks to the daemon and
 *     speaks KISS frames, nothing else.
 *
 *   - DO NOT CLOSE THE PORT while the device is in command mode.
 *     A falling DTR puts a TNC2C into an echo-only state. In KISS
 *     operation this is uncritical - the normal operating state.
 *
 *   - C0 FF C0 resets the firmware on TheFirmware and makes the
 *     banner appear. It is NOT just "leave KISS".
 *
 *   - ESC V (1B 56 0D) is the probe, not INFO or HELP.
 *
 *   - ESC I <call>\r sets MYCALL (tfb.c Icmd). Without MYCALL the
 *     firmware does not key the transmitter on KISS DATA at all.
 *
 *   - ESC @K (1B 40 4B) switches to KISS mode WITHOUT line end.
 *
 *   - In KISS mode the device speaks frames only, no commands.
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
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TNC2_MAX_PENDING 32

typedef struct tnc2_impl {
    pr_tncsock   sock;   /* to the daemon that keeps the port open */
    pr_rig_state st;
    kiss_decoder dec;

    pr_msg       pending[TNC2_MAX_PENDING];
    size_t       npending;

    char         mycall[16];
    /*
     * Name of the station this device belongs to. Taken from the
     * configuration at open time, so every received message can name
     * its device - the "All" view shows that.
     */
    char         station[32];
} tnc2_impl;

/* ======================================================================= */
/* Save state                                                              */
/* ======================================================================= */

static void tnc2_note(pr_rig *r, char kind, const char *from, const char *text)
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
        tnc2_impl *t = r->impl;
        pr_strlcpy(m.station, t->station, sizeof m.station);
    }
    m.ts = pr_now_s();
    char err[128];
    (void)pr_log_append(r->cfg, &m, err, sizeof err);
}

static void tnc2_save(pr_rig *r, tnc2_impl *t)
{
    char err[128];
    (void)pr_state_save(r->cfg, &t->st, err, sizeof err);
}

/* ======================================================================= */
/* Capturing a received message                                            */
/* ======================================================================= */

static void tnc2_push_rx(tnc2_impl *t, const char *to, const char *from,
                         const char *text, int db)
{
    if (t->npending >= TNC2_MAX_PENDING) {
        memmove(&t->pending[0], &t->pending[1],
                (TNC2_MAX_PENDING - 1) * sizeof t->pending[0]);
        t->npending = TNC2_MAX_PENDING - 1;
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
    m->db = db;
    m->ts = pr_now_s();
}

/*
 * One received KISS DATA frame = one AX.25 frame.
 *
 * Layout: dest (7) | src (7) | Control (1) | PID (1) | info
 *
 * Only UI frames with no-layer-3 PID are interesting here. Everything
 * else (connected traffic, digipeater hops) is not part of this
 * terminal and is dropped deliberately.
 */
static void tnc2_handle_frame(tnc2_impl *t, const unsigned char *frame, size_t len)
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

    tnc2_push_rx(t, to, from, text, t->st.rx_db);
    t->st.rx_count++;
    pr_strlcpy(t->st.last_rx_from, from, sizeof t->st.last_rx_from);
    pr_strlcpy(t->st.last_rx_text, text, sizeof t->st.last_rx_text);
    t->st.last_rx_ts = pr_now_s();
}


/* ======================================================================= */
/* VTable                                                                  */
/* ======================================================================= */

static int tnc2_open(pr_rig *r, char *err, size_t errlen)
{
    const pr_config *cfg = r->cfg;

    int databits = 8, parity = PR_PAR_NONE, stopbits = 1;
    if (!pr_serial_parse_line(cfg->serial_line, &databits, &parity, &stopbits)) {
        pr_trf(err, errlen, "line format \"%s\" is invalid (e.g. 8n1)",
                 cfg->serial_line);
        return -1;
    }

    tnc2_impl *t = calloc(1, sizeof *t);
    if (t == NULL) {
        pr_trf(err, errlen, "out of memory");
        return -1;
    }

    /*
     * The port is NOT opened here. prterm-tncd does that, keeping it
     * open and running KISS. A CGI that opens the port itself tears
     * the TNC2C out of KISS on every call.
     */
    {
        char sock[512];
        snprintf(sock, sizeof sock, "%.400s/tnc-%.32s.sock",
                 cfg->runtime_dir,
                 cfg->active_station[0] ? cfg->active_station : "default");
        if (pr_tncsock_open(&t->sock, sock, err, errlen) != 0) {
            free(t);
            return -1;
        }
    }
    (void)databits; (void)parity; (void)stopbits;

    kiss_decoder_init(&t->dec);

    /*
     * DELIBERATELY NO commands to the device.
     *
     * A CGI opens the port on EVERY call - the state poll runs
     * every second. Formerly the KISS entry was written every
     * single time. But if the TNC is already in KISS mode, every
     * written byte is A TRANSMISSION. That led to a permanent
     * transmitter.
     *
     * Opening a device must not change its state. Only --checkup /
     * --reset-tnc lead into KISS mode, deliberately following the
     * principle: leave KISS first (control frame, sends nothing),
     * then write in command mode, then enter KISS. The order is
     * deliberate.
     */

    if (pr_state_load(cfg, &t->st, err, errlen) != 0) {
        pr_tncsock_close(&t->sock);
        free(t);
        return -1;
    }

    pr_strlcpy(t->st.device, cfg->port, sizeof t->st.device);
    pr_strlcpy(t->st.status, "running", sizeof t->st.status);
    pr_strlcpy(t->st.detail, "TNC2 / KISS", sizeof t->st.detail);
    t->st.link_ok = true;
    t->st.duplex  = cfg->duplex;
    pr_strlcpy(t->mycall, cfg->callerid, sizeof t->mycall);
    pr_strlcpy(t->station, cfg->active_station, sizeof t->station);

    r->impl = t;

    /*
     * Report only on the VERY FIRST start. The port is reopened on
     * every CGI call - one message per call would flood the terminal.
     * It is noise, not status.
     */
    {
        char state_file[640];
        pr_state_path(cfg, state_file, sizeof state_file);
        if (!pr_file_exists(state_file))
            tnc2_note(r, PR_MSG_SYS, "SYS", "TNC2 connected (KISS)");
    }
    return 0;
}

static void tnc2_close(pr_rig *r)
{
    tnc2_impl *t = r->impl;
    if (t == NULL)
        return;

    tnc2_save(r, t);
    /*
     * The port is being closed - only in KISS operation is this
     * uncritical. In command mode the DTR would trigger echo-only.
     */
    pr_tncsock_close(&t->sock);
    free(t);
    r->impl = NULL;
}

static int tnc2_refresh(pr_rig *r, char *err, size_t errlen)
{
    tnc2_impl *t = r->impl;
    if (t == NULL) {
        pr_trf(err, errlen, "TNC not connected");
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
         * The daemon holds KISS - reception arrives as KISS frames.
         * Everything that is not framed (prompts, banners of a
         * repairing device) is ignored by the decoder on purpose.
         */
        size_t frames = kiss_decoder_feed_buf(&t->dec, buf, (size_t)n);
        for (size_t i = 0; i < frames; i++) {
            unsigned char frame[KISS_FRAME_MAX];
            size_t flen = kiss_decoder_take(&t->dec, frame, sizeof frame);
            if (flen > 0)
                tnc2_handle_frame(t, frame, flen);
        }
    }
    return 0;
}

static int tnc2_get_state(pr_rig *r, pr_rig_state *out)
{
    tnc2_impl *t = r->impl;
    if (t == NULL)
        return -1;
    *out = t->st;
    return 0;
}

static int tnc2_set_freq(pr_rig *r, long freq_hz, char *err, size_t errlen)
{
    tnc2_impl *t = r->impl;
    if (t == NULL) {
        pr_trf(err, errlen, "TNC not connected");
        return -1;
    }
    (void)errlen;
    t->st.freq_hz = freq_hz;
    tnc2_save(r, t);
    return 0;
}

static int tnc2_set_mode(pr_rig *r, unsigned mode, char *err, size_t errlen)
{
    tnc2_impl *t = r->impl;
    if (t == NULL) {
        pr_trf(err, errlen, "TNC not connected");
        return -1;
    }
    (void)errlen;
    t->st.mode = mode;
    tnc2_save(r, t);
    return 0;
}

static int tnc2_set_ptt(pr_rig *r, bool on, char *err, size_t errlen)
{
    tnc2_impl *t = r->impl;
    if (t == NULL) {
        pr_trf(err, errlen, "TNC not connected");
        return -1;
    }

    if (on && t->st.monitor) {
        pr_trf(err, errlen, "monitor mode: transmitting is locked");
        return -1;
    }
    (void)errlen;

    /*
     * With KISS, PTT is handled by the hardware - the TNC switches
     * itself when sending a frame. Here we only note the state.
     */
    t->st.ptt = on;
    t->st.rx_muted = (t->st.duplex == PR_DUPLEX_HALF) && on;
    tnc2_save(r, t);
    return 0;
}

static int tnc2_set_duplex(pr_rig *r, pr_duplex d, char *err, size_t errlen)
{
    tnc2_impl *t = r->impl;
    if (t == NULL) {
        pr_trf(err, errlen, "TNC not connected");
        return -1;
    }
    (void)errlen;

    t->st.duplex = d;
    t->st.rx_muted = (d == PR_DUPLEX_HALF) && t->st.ptt;

    /*
     * Report the KISS parameter FULLDUPLEX (0x05) to the device.
     * That is the lever via which the hardware can do full duplex.
     * The value goes as the payload of the parameter frame.
     */
    unsigned char frame[8];
    unsigned char v = (d == PR_DUPLEX_FULL) ? 1 : 0;
    size_t n = kiss_encode(frame, sizeof frame, 0, KISS_CMD_FULLDUPLEX, &v, 1);
    if (n > 0) {
        char e2[64];
        (void)pr_tncsock_tx(&t->sock, frame, n, e2, sizeof e2);
    }

    tnc2_save(r, t);
    return 0;
}

static int tnc2_set_monitor(pr_rig *r, bool on, char *err, size_t errlen)
{
    tnc2_impl *t = r->impl;
    if (t == NULL) {
        pr_trf(err, errlen, "TNC not connected");
        return -1;
    }
    (void)errlen;
    t->st.monitor = on;
    if (on) {
        t->st.ptt = false;
        t->st.rx_muted = false;
    }
    tnc2_save(r, t);
    return 0;
}

static int tnc2_send(pr_rig *r, const char *from, const char *to,
                 const char *text,
                     char *err, size_t errlen)
{
    tnc2_impl *t = r->impl;
    if (t == NULL) {
        pr_trf(err, errlen, "TNC not connected");
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
     * Build an AX.25 UI frame. "to" is the station to call; empty or
     * CQ stands for a broadcast without a fixed partner.
     */
    if (to == NULL || to[0] == '\0')
        to = "CQ";

    unsigned char ui[512];
    size_t uilen = ax25_ui_frame(ui, sizeof ui, from, to,
                                 (const unsigned char *)text, strlen(text));
    if (uilen == 0) {
        pr_trf(err, errlen, "frame could not be built");
        return -1;
    }

    /* KISS frame: FEND | 0x00 | escaped payload | FEND
     * The FCS is NOT included - the TNC computes and adds it. */
    unsigned char frame[640];
    size_t flen = kiss_encode(frame, sizeof frame, 0, KISS_CMD_DATA, ui, uilen);
    if (flen == 0) {
        pr_trf(err, errlen, "KISS frame too large");
        return -1;
    }

    if (pr_tncsock_tx(&t->sock, frame, flen, err, errlen) != 0)
        return -1;

    tnc2_note(r, PR_MSG_TX, from, text);
    t->st.tx_count++;
    t->st.last_tx_ts = pr_now_s();
    tnc2_save(r, t);
    return 0;
}

/*
 * Test carrier for the admin area.
 *
 * With KISS the hardware switches the transmission with the frame
 * itself - there is no command for "carrier only without content".
 * The test duration is therefore mapped via the FRAME LENGTH: the
 * payload part is sized so that the transmission takes roughly the
 * desired time. The content is null padding, so meaningless.
 *
 * This is more honest than a "PTT on", which with KISS only sets a
 * variable and puts nothing on the air.
 */
/*
 * Test carrier for the admin area.
 *
 * With KISS the hardware switches the transmission with the frame
 * itself - there is no command for "carrier only without content".
 * The duration is mapped via the number of frames.
 *
 * IMPORTANT: every frame stays at the size usual for AX.25 (PACLEN,
 * here 256 bytes of payload). A single inflated frame is rejected by
 * the TNC or stays stuck in memory - the "unacknowledged data" LED
 * then stays on although there is nothing to acknowledge. Exactly
 * that happened here.
 *
 * The content is null padding, so meaningless.
 */
#define TNC2_PACLEN 256

static int tnc2_carrier_test(pr_rig *r, unsigned seconds,
                             char *err, size_t errlen)
{
    tnc2_impl *t = r->impl;
    if (t == NULL) {
        pr_trf(err, errlen, "TNC not connected");
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

    /*
     * How many frames do we need? Each frame holds the transmission
     * for its duration. The order is deliberately chosen so that PTT
     * does not drop in between.
     */
    long baud = r->cfg->radio_baud > 0 ? r->cfg->radio_baud : 1200;
    long bits_needed = (long)seconds * baud;
    long bytes_needed = bits_needed / 8;
    /* minus AX.25 header (16) and FCS (2) per frame    */
    long per_frame = TNC2_PACLEN + 18;
    long frames = (bytes_needed + per_frame - 1) / per_frame;
    if (frames < 1) frames = 1;
    if (frames > 40) frames = 40;

    unsigned char pad[TNC2_PACLEN];
    memset(pad, 0, sizeof pad);

    for (long k = 0; k < frames; k++) {
        unsigned char ui[320];
        size_t uilen = ax25_ui_frame(ui, sizeof ui, r->cfg->callerid, "CQ",
                                     pad, TNC2_PACLEN);
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
            t->st.ptt = false;
            t->st.rx_muted = false;
            return -1;
        }
    }

    /*
     * With KISS the transmission ends with the last frame - the TNC
     * switches itself and lets go again.
     */
    t->st.ptt = false;
    t->st.rx_muted = false;

    tnc2_note(r, PR_MSG_TX, r->cfg->callerid,
              pr_trs("[empty test carrier]"));
    t->st.tx_count++;
    t->st.last_tx_ts = pr_now_s();
    tnc2_save(r, t);
    return 0;
}

static int tnc2_drain(pr_rig *r, pr_msg *out, size_t cap, size_t *n)
{
    tnc2_impl *t = r->impl;
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

const pr_rig_vtbl pr_rig_tnc2 = {
    "tnc2",
    "TNC2 class (Landolt TNC2C, PK-TNC2) via KISS",
    tnc2_open,
    tnc2_close,
    tnc2_refresh,
    tnc2_get_state,
    tnc2_set_freq,
    tnc2_set_mode,
    tnc2_set_ptt,
    tnc2_set_duplex,
    tnc2_set_monitor,
    tnc2_send,
    tnc2_carrier_test,
    tnc2_drain
};
