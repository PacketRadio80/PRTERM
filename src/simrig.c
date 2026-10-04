/*
 * PRTERM - CB & Amateur Radio Terminal
 * simrig.c - Simuliertes Rig. Kein Hardware-Zugriff.
 *
 * Zweck: den gesamten Betrieb - einschliesslich Vollduplex - ohne Geraet
 * durchspielen zu koennen. Der Zustand lebt in state.ini, damit er ueber
 * die einzelnen CGI-Aufrufe hinweg erhalten bleibt.
 *
 * Vollduplex im Modell:
 *   full -> der Empfang laeuft waehrend ptt=1 ungehindert weiter
 *   half -> waehrend ptt=1 wird nicht empfangen (rx_muted=1)
 *
 * SPDX-License-Identifier: MIT
 */
#include "prterm_compat.h"

#include "radio.h"
#include "state.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

#define SIM_MAX_PENDING 32

typedef struct sim_impl {
    pr_rig_state st;
    pr_msg       pending[SIM_MAX_PENDING];
    size_t       npending;
    long long    last_gen;
    unsigned     rng;
} sim_impl;

/* ======================================================================= */
/* Simulierte Gegenstationen                                               */
/* ======================================================================= */

static const char *const sim_stations[] = {
    "DL1ABC-1", "DL3XYZ",   "DO7ABC",  "DK9ZZ-2",  "PA2DEF",
    "OK1KQ-3",  "F5ABC",    "G4XYZ",   "I2ABC",    "EA3ABC",
    "SM5ABC",   "OZ1KQ",    "HB9ABC",  "OE1XYZ",   "S51ABC",
};

static const char *const sim_texts[] = {
    "CQ CQ CQ hier @, hoert mich jemand?",
    "@, Du kommst gut an, 5 und 9.",
    "QRM auf dem Kanal, bitte warten.",
    "Roger @, alles verstanden.",
    "Kurze Pause, bin gleich wieder da.",
    "Antennenprobleme, Signal ist schwach.",
    "Gutes Signal heute, Bedingungen stimmen.",
    "@, wie ist Deine QTH?",
    "Alles klar, bis spaeter 73.",
    "Bleibt ruhig auf der Frequenz.",
    "Sonde: 4 Watt ERP reicht hier vollkommen.",
    "Kanal ist frei, kann wer senden.",
};

#define SIM_STATIONS (sizeof sim_stations / sizeof sim_stations[0])
#define SIM_TEXTS    (sizeof sim_texts    / sizeof sim_texts[0])

static unsigned sim_rand(sim_impl *s)
{
    /* kleiner xorshift - deterministisch, ohne libc-Abhaengigkeit */
    unsigned x = s->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s->rng = x != 0 ? x : 0x2545F491u;
    return s->rng;
}

/* ======================================================================= */
/* Hilfen                                                                  */
/* ======================================================================= */

/*
 * Platzhalter im Text ist '@' - ausdruecklich KEIN printf-Format. Damit kann
 * kein Format-String-Injection entstehen, falls die Vorlagen jemals aus
 * fremden Quellen kommen.
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

static void sim_push_rx(sim_impl *s, const char *from, const char *text, int db)
{
    if (s->npending >= SIM_MAX_PENDING) {
        /* aelteste verwerfen */
        memmove(&s->pending[0], &s->pending[1],
                (SIM_MAX_PENDING - 1) * sizeof s->pending[0]);
        s->npending = SIM_MAX_PENDING - 1;
    }

    pr_msg *m = &s->pending[s->npending++];
    memset(m, 0, sizeof *m);
    m->kind = PR_MSG_RX;
    pr_strlcpy(m->from, from, sizeof m->from);
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
        snprintf(err, errlen, "Speicher erschoepft");
        return -1;
    }

    if (pr_state_load(r->cfg, &s->st, err, errlen) != 0) {
        free(s);
        return -1;
    }

    /* Nur beim allerersten Start melden - der Prozess wird bei jedem
     * CGI-Aufruf neu aufgemacht, das waere sonst Dauer-Rauschen im Log. */
    char state_file[640];
    pr_state_path(r->cfg, state_file, sizeof state_file);
    bool first_time = !pr_file_exists(state_file);

    pr_strlcpy(s->st.device, "sim", sizeof s->st.device);
    pr_strlcpy(s->st.status, "running", sizeof s->st.status);
    pr_strlcpy(s->st.detail, "Simulation", sizeof s->st.detail);
    s->st.link_ok = true;
    s->st.duplex  = r->cfg->duplex;

    s->last_gen = pr_now_s();
    s->rng = 0x2545F491u ^ (unsigned)pr_now_ms();

    r->impl = s;

    if (first_time) {
        sim_note(r, PR_MSG_SYS, "SYS",
                 s->st.duplex == PR_DUPLEX_FULL
                     ? "Simulation gestartet (Vollduplex: Empfang laeuft waehrend des Sendens weiter)"
                     : "Simulation gestartet (Halbduplex: waehrend des Sendens wird nicht empfangen)");
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
 * Fuehrt die Simulation fort. Im Vollduplex entsteht waehrend des Sendens
 * weiterhin Empfang - das ist genau der Fall, den PRTERM zeigen soll.
 */
static int sim_refresh(pr_rig *r, char *err, size_t errlen)
{
    sim_impl *s = r->impl;
    if (s == NULL) {
        snprintf(err, errlen, "Simulation nicht offen");
        return -1;
    }
    (void)errlen;

    long long now = pr_now_s();
    long long idle = now - s->last_gen;

    /* Halbduplex: waehrend ptt=1 wird nicht empfangen */
    s->st.rx_muted = (s->st.duplex == PR_DUPLEX_HALF) && s->st.ptt;

    /* Signalpegel leicht bewegen */
    s->st.rx_db += (int)(sim_rand(s) % 5) - 2;
    if (s->st.rx_db < -110) s->st.rx_db = -110;
    if (s->st.rx_db > -25)  s->st.rx_db = -25;
    s->st.squelch_open = s->st.rx_db > -85 && !s->st.rx_muted;

    if (s->st.monitor)
        s->st.rx_muted = false;

    /* Neue Nachrichten alle 2-5 Sekunden */
    long long interval = 2 + (long long)(sim_rand(s) % 4);
    if (idle < interval)
        return 0;

    s->last_gen = now;

    if (s->st.rx_muted) {
        /* Halbduplex: keine RX-Nachrichten waehrend des Sendens */
        return 0;
    }

    size_t n = 1 + sim_rand(s) % 2;
    for (size_t i = 0; i < n; i++) {
        const char *from = sim_stations[sim_rand(s) % SIM_STATIONS];
        char text[PR_MSG_TEXT];
        sim_format(text, sizeof text, sim_texts[sim_rand(s) % SIM_TEXTS], from);
        sim_push_rx(s, from, text, s->st.rx_db);
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
        snprintf(err, errlen, "Simulation nicht offen");
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
        snprintf(err, errlen, "Simulation nicht offen");
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
        snprintf(err, errlen, "Simulation nicht offen");
        return -1;
    }
    (void)errlen;

    if (on && s->st.monitor) {
        snprintf(err, errlen, "Monitorbetrieb: Senden ist gesperrt");
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
        snprintf(err, errlen, "Simulation nicht offen");
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
        snprintf(err, errlen, "Simulation nicht offen");
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

static int sim_send(pr_rig *r, const char *from, const char *text,
                    char *err, size_t errlen)
{
    sim_impl *s = r->impl;
    if (s == NULL) {
        snprintf(err, errlen, "Simulation nicht offen");
        return -1;
    }

    if (s->st.monitor) {
        snprintf(err, errlen, "Monitorbetrieb: Senden ist gesperrt");
        return -1;
    }
    if (text == NULL || text[0] == '\0') {
        snprintf(err, errlen, "leere Nachricht");
        return -1;
    }

    /* Im Vollduplex laeuft der Empfang waehrenddessen weiter. */
    sim_note(r, PR_MSG_TX, from, text);

    s->st.tx_count++;
    s->st.last_tx_ts = pr_now_s();
    sim_save(r, s);

    /* Vollduplex: sofort wieder neuer Empfang */
    if (s->st.duplex == PR_DUPLEX_FULL && !s->st.rx_muted) {
        char echo[PR_MSG_TEXT];
        snprintf(echo, sizeof echo, "Roger %s, kopiert.", from);
        sim_push_rx(s, sim_stations[sim_rand(s) % SIM_STATIONS], echo, s->st.rx_db);
    }
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
    "Simulation (ohne Hardware)",
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
    sim_drain
};
