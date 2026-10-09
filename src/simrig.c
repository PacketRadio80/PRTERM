/*
 * PRTERM - CB & Amateur Radio Terminal
 * simrig.c - Simulated rig. No hardware access.
 *
 * Purpose: play through the whole operation - including full duplex -
 * without a device. The state lives in state.ini so it survives the
 * individual CGI calls.
 *
 * Full duplex in the model:
 *   full -> reception continues unhindered while ptt=1
 *   half -> nothing is received while ptt=1 (rx_muted=1)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "prterm_compat.h"

#include "radio.h"
#include "lang.h"
#include "state.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SIM_MAX_PENDING 32

typedef struct sim_impl {
    pr_rig_state st;
    pr_msg       pending[SIM_MAX_PENDING];
    size_t       npending;
    long long    last_gen;
    unsigned     rng;
} sim_impl;

/* ======================================================================= */
/* Simulated counterparties                                                */
/* ======================================================================= */

static const char *const sim_stations[] = {
    "DL1ABC-1", "DL3XYZ",   "DO7ABC",  "DK9ZZ-2",  "PA2DEF",
    "OK1KQ-3",  "F5ABC",    "G4XYZ",   "I2ABC",    "EA3ABC",
    "SM5ABC",   "OZ1KQ",    "HB9ABC",  "OE1XYZ",   "S51ABC",
};

static const char *const sim_texts[] = {
    "CQ CQ CQ here @, anyone copy me?",
    "@, you are coming in well, 5 and 9.",
    "QRM on the channel, please wait.",
    "Roger @, all understood.",
    "Short break, be right back.",
    "Antenna problems, signal is weak.",
    "Good signal today, conditions are right.",
    "@, what is your QTH?",
    "All clear, see you later 73.",
    "Keep it quiet on the frequency.",
    "Test call: 4 watts ERP is plenty here.",
    "Channel is clear, anyone may transmit.",
};

#define SIM_STATIONS (sizeof sim_stations / sizeof sim_stations[0])
#define SIM_TEXTS    (sizeof sim_texts    / sizeof sim_texts[0])

static unsigned sim_rand(sim_impl *s)
{
    /* small xorshift - deterministic, without libc dependency     */
    unsigned x = s->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s->rng = x != 0 ? x : 0x2545F491u;
    return s->rng;
}

/* ======================================================================= */
/* Helpers                                                                  */
/* ======================================================================= */

/*
 * The placeholder in the text is '@' - explicitly NOT a printf format.
 * This way no format string injection can happen, should the templates
 * ever come from foreign sources.
 */
static void sim_format(char *dst, size_t dstlen, const char *tmpl, const char *who)
{
    size_t w = 0;
    for (const char *p = tmpl; *p != '\0' && w + 1 < dstlen; p++) {
        if (*p == '@') {
            for (const char *q = who; *q != '\0' && w + 1 < dstlen; q++)
                dst[w++] = *q;
        } else {
            dst[w++] = *p;
        }
    }
    dst[w] = '\0';
}

static void sim_push_rx(sim_impl *s, const char *from, const char *to,
                       const char *text, int db)
{
    if (s->npending >= SIM_MAX_PENDING) {
        memmove(&s->pending[0], &s->pending[1],
                (SIM_MAX_PENDING - 1) * sizeof s->pending[0]);
        s->npending = SIM_MAX_PENDING - 1;
    }

    pr_msg *m = &s->pending[s->npending++];
    memset(m, 0, sizeof *m);
    m->kind = PR_MSG_RX;
    pr_strlcpy(m->from, from, sizeof m->from);
    if (to && to[0])
        pr_strlcpy(m->to, to, sizeof m->to);
    pr_strlcpy(m->text, text, sizeof m->text);
    m->db = db;
    m->ts = pr_now_s();
}

static void sim_note(pr_rig *r, char kind, const char *from, const char *text)
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

static void sim_save(pr_rig *r, sim_impl *s)
{
    char err[128];
    (void)pr_state_save(r->cfg, &s->st, err, sizeof err);
}

/* ======================================================================= */
/* VTable                                                                  */
/* ======================================================================= */

static int sim_open(pr_rig *r, char *err, size_t errlen)
{
    sim_impl *s = calloc(1, sizeof *s);
    if (s == NULL) {
        pr_trf(err, errlen, "out of memory");
        return -1;
    }

    if (pr_state_load(r->cfg, &s->st, err, errlen) != 0) {
        free(s);
        return -1;
    }

    /* Report only on the very first start - the process is recreated on
     * every CGI call, otherwise it would be permanent noise in the log. */
    char state_file[640];
    pr_state_path(r->cfg, state_file, sizeof state_file);
    bool first_time = !pr_file_exists(state_file);

    pr_strlcpy(s->st.device, "sim", sizeof s->st.device);
    pr_strlcpy(s->st.status, "running", sizeof s->st.status);
    pr_strlcpy(s->st.detail, "Simulation", sizeof s->st.detail);
    s->st.link_ok = true;
    s->st.duplex  = r->cfg->duplex;
    s->st.mode    = r->cfg->mode;   /* per-station INI mode is authoritative */

    s->last_gen = pr_now_s();
    s->rng = 0x2545F491u ^ (unsigned)pr_now_ms();

    r->impl = s;

    if (first_time) {
        sim_note(r, PR_MSG_SYS, "SYS",
                 s->st.duplex == PR_DUPLEX_FULL
                     ? pr_trs("simulation started (full duplex: reception continues while transmitting)")
                     : pr_trs("simulation started (half duplex: no reception while transmitting)"));
    }
    return 0;
}

static void sim_close(pr_rig *r)
{
    sim_impl *s = r->impl;
    if (s == NULL)
        return;
    sim_save(r, s);
    free(s);
    r->impl = NULL;
}

/*
 * Advances the simulation. In full duplex reception continues during
 * transmission - that is exactly the case PRTERM is meant to show.
 */
static int sim_refresh(pr_rig *r, char *err, size_t errlen)
{
    sim_impl *s = r->impl;
    if (s == NULL) {
        pr_trf(err, errlen, "simulation not open");
        return -1;
    }
    (void)errlen;

    long long now = pr_now_s();
    long long idle = now - s->last_gen;

    /* Half duplex: nothing is received while ptt=1    */
    s->st.rx_muted = (s->st.duplex == PR_DUPLEX_HALF) && s->st.ptt;

    /* Move the signal level slightly */
    s->st.rx_db += (int)(sim_rand(s) % 5) - 2;
    if (s->st.rx_db < -110) s->st.rx_db = -110;
    if (s->st.rx_db > -25)  s->st.rx_db = -25;
    s->st.squelch_open = s->st.rx_db > -85 && !s->st.rx_muted;

    if (s->st.monitor)
        s->st.rx_muted = false;

    /* New messages every 2-5 seconds     */
    long long interval = 2 + (long long)(sim_rand(s) % 4);
    if (idle < interval)
        return 0;

    s->last_gen = now;

    if (s->st.rx_muted) {
        /* Half duplex: no RX messages during transmission       */
        return 0;
    }

    size_t n = 1 + sim_rand(s) % 2;
    for (size_t i = 0; i < n; i++) {
        const char *from = sim_stations[sim_rand(s) % SIM_STATIONS];
        char text[PR_MSG_TEXT];
        sim_format(text, sizeof text, sim_texts[sim_rand(s) % SIM_TEXTS], from);
        sim_push_rx(s, from, r->cfg->callerid, text, s->st.rx_db);
        s->st.rx_count++;
    }

    s->st.last_rx_ts = now;
    if (s->npending > 0) {
        pr_strlcpy(s->st.last_rx_from, s->pending[s->npending - 1].from,
                   sizeof s->st.last_rx_from);
        pr_strlcpy(s->st.last_rx_text, s->pending[s->npending - 1].text,
                   sizeof s->st.last_rx_text);
    }

    sim_save(r, s);
    return 0;
}

static int sim_get_state(pr_rig *r, pr_rig_state *out)
{
    sim_impl *s = r->impl;
    if (s == NULL)
        return -1;
    *out = s->st;
    return 0;
}

static int sim_set_freq(pr_rig *r, long freq_hz, char *err, size_t errlen)
{
    sim_impl *s = r->impl;
    if (s == NULL) {
        pr_trf(err, errlen, "simulation not open");
        return -1;
    }
    (void)errlen;
    s->st.freq_hz = freq_hz;
    sim_save(r, s);
    return 0;
}

static int sim_set_mode(pr_rig *r, unsigned mode, char *err, size_t errlen)
{
    sim_impl *s = r->impl;
    if (s == NULL) {
        pr_trf(err, errlen, "simulation not open");
        return -1;
    }
    (void)errlen;
    s->st.mode = mode;
    sim_save(r, s);
    return 0;
}

static int sim_set_ptt(pr_rig *r, bool on, char *err, size_t errlen)
{
    sim_impl *s = r->impl;
    if (s == NULL) {
        pr_trf(err, errlen, "simulation not open");
        return -1;
    }
    (void)errlen;

    if (on && s->st.monitor) {
        pr_trf(err, errlen, "monitor mode: transmitting is locked");
        return -1;
    }

    if (on && !s->st.ptt) {
        s->st.ptt = true;
        s->st.tx_db = -6;
        if (s->st.duplex == PR_DUPLEX_HALF)
            s->st.rx_muted = true;
    } else if (!on && s->st.ptt) {
        s->st.ptt = false;
        s->st.rx_muted = false;
    }
    sim_save(r, s);
    return 0;
}

static int sim_set_duplex(pr_rig *r, pr_duplex d, char *err, size_t errlen)
{
    sim_impl *s = r->impl;
    if (s == NULL) {
        pr_trf(err, errlen, "simulation not open");
        return -1;
    }
    (void)errlen;
    s->st.duplex = d;
    s->st.rx_muted = (d == PR_DUPLEX_HALF) && s->st.ptt;
    sim_save(r, s);
    return 0;
}

static int sim_set_monitor(pr_rig *r, bool on, char *err, size_t errlen)
{
    sim_impl *s = r->impl;
    if (s == NULL) {
        pr_trf(err, errlen, "simulation not open");
        return -1;
    }
    (void)errlen;
    s->st.monitor = on;
    if (on) {
        s->st.ptt = false;
        s->st.rx_muted = false;
    }
    sim_save(r, s);
    return 0;
}

static int sim_send(pr_rig *r, const char *from, const char *to,
                 const char *text,
                    char *err, size_t errlen)
{
    (void)to;   /* The destination is irrelevant for this driver */
    sim_impl *s = r->impl;
    if (s == NULL) {
        pr_trf(err, errlen, "simulation not open");
        return -1;
    }

    if (s->st.monitor) {
        pr_trf(err, errlen, "monitor mode: transmitting is locked");
        return -1;
    }
    if (text == NULL || text[0] == '\0') {
        pr_trf(err, errlen, "leere Nachricht");
        return -1;
    }

    /* In full duplex reception continues meanwhile.           */
    sim_note(r, PR_MSG_TX, from, text);

    s->st.tx_count++;
    s->st.last_tx_ts = pr_now_s();
    sim_save(r, s);

    /* Full duplex: new reception immediately  */
    if (s->st.duplex == PR_DUPLEX_FULL && !s->st.rx_muted) {
        char echo[PR_MSG_TEXT];
        snprintf(echo, sizeof echo, "Roger %s, kopiert.", from);
        sim_push_rx(s, sim_stations[sim_rand(s) % SIM_STATIONS], r->cfg->callerid, echo, s->st.rx_db);
    }
    return 0;
}

/*
 * Test carrier: in the simulation the state is tracked and the time
 * waited out, so the test sequence is identical to the real device.
 */
static int sim_carrier_test(pr_rig *r, unsigned seconds,
                            char *err, size_t errlen)
{
    sim_impl *s = r->impl;
    if (s == NULL) {
        pr_trf(err, errlen, "simulation not connected");
        return -1;
    }
    if (seconds == 0 || seconds > 10) {
        pr_trf(err, errlen, "duration must be between 1 and 10 seconds");
        return -1;
    }
    (void)errlen;

    s->st.ptt = true;
    s->st.rx_muted = (s->st.duplex == PR_DUPLEX_HALF);
    sleep(seconds);
    s->st.ptt = false;
    s->st.rx_muted = false;
    s->st.tx_count++;
    s->st.last_tx_ts = pr_now_s();
    sim_save(r, s);
    return 0;
}

static int sim_drain(pr_rig *r, pr_msg *out, size_t cap, size_t *n)
{
    sim_impl *s = r->impl;
    if (s == NULL) {
        *n = 0;
        return -1;
    }
    size_t k = 0;
    while (k < cap && s->npending > 0) {
        out[k++] = s->pending[0];
        memmove(&s->pending[0], &s->pending[1],
                (s->npending - 1) * sizeof s->pending[0]);
        s->npending--;
    }
    *n = k;
    return 0;
}

const pr_rig_vtbl pr_rig_sim = {
    "sim",
    "Simulation (no hardware)",
    sim_open,
    sim_close,
    sim_refresh,
    sim_get_state,
    sim_set_freq,
    sim_set_mode,
    sim_set_ptt,
    sim_set_duplex,
    sim_set_monitor,
    sim_send,
    sim_carrier_test,
    sim_drain
};
