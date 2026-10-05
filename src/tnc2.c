/*
 * PRTERM - CB & Amateur Radio Terminal
 * tnc2.c - Treiber fuer TNC2-Klasse (Landolt TNC2C, PK-TNC2).
 *
 * Protokoll: KISS ueber serielle Leitung.
 *
 * Betriebserfahrungen, die diesen Treiber bestimmen:
 *
 *   - DEN PORT NICHT SCHLIESSEN, solange das Geraet im Command-Mode ist.
 *     Ein fallendes DTR versetzt einen TNC2C in einen Echo-only-Zustand.
 *     Im KISS-Betrieb ist das unkritisch - der normale Betriebszustand.
 *
 *   - C0 FF C0 setzt bei TheFirmware die Firmware zurueck und laesst den
 *     Banner erscheinen. Es ist NICHT nur "KISS verlassen".
 *
 *   - ESC V (1B 56 0D) ist der Probe, nicht INFO oder HELP.
 *
 *   - ESC I <call>\r setzt MYCALL (tfb.c Icmd).
 *
 *   - ESC @K (1B 40 4B) schaltet in den KISS-Modus, OHNE Zeilenende.
 *
 *   - Im KISS-Modus spricht das Geraet nur Rahmen, keine Kommandos.
 *
 * SPDX-License-Identifier: MIT
 */
#include "prterm_compat.h"

#include "radio.h"
#include "callsign.h"
#include "kiss.h"
#include "serial.h"
#include "state.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TNC2_MAX_PENDING 32

typedef struct tnc2_impl {
    pr_serial    ser;
    pr_rig_state st;
    kiss_decoder dec;

    pr_msg       pending[TNC2_MAX_PENDING];
    size_t       npending;

    bool         in_kiss;
    char         mycall[16];
} tnc2_impl;

/* ======================================================================= */
/* Zustand sichern                                                         */
/* ======================================================================= */

static void tnc2_note(pr_rig *r, char kind, const char *from, const char *text)
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

static void tnc2_save(pr_rig *r, tnc2_impl *t)
{
    char err[128];
    (void)pr_state_save(r->cfg, &t->st, err, sizeof err);
}

/* ======================================================================= */
/* Aufnehmen einer empfangenen Nachricht                                   */
/* ======================================================================= */

static void tnc2_push_rx(tnc2_impl *t, const char *from, const char *text, int db)
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
    pr_strlcpy(m->text, text, sizeof m->text);
    m->db = db;
    m->ts = pr_now_s();
}

/*
 * Zerlegt einen empfangenen AX.25-Rahmen und erzeugt daraus eine
 * Nachricht. UI-Rahmen tragen die Information nach Control+PID.
 */
static void tnc2_handle_frame(tnc2_impl *t, const unsigned char *frame, size_t len)
{
    /* Adressfelder: Ziel (7), Quelle (7), dann Control, PID */
    if (len < 16)
        return;

    char to[16], from[16];
    if (!call_from_ax25(frame, to, sizeof to))
        return;
    if (!call_from_ax25(frame + 7, from, sizeof from))
        return;

    unsigned char ctrl = frame[14];
    unsigned char pid  = frame[15];

    /* Nur UI-Rahmen sind hier interessant */
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

    tnc2_push_rx(t, from, text, t->st.rx_db);
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
        snprintf(err, errlen, "Zeilenformat \"%s\" ist ungueltig (z.B. 8n1)",
                 cfg->serial_line);
        return -1;
    }

    tnc2_impl *t = calloc(1, sizeof *t);
    if (t == NULL) {
        snprintf(err, errlen, "Speicher erschoepft");
        return -1;
    }

    if (pr_serial_open(&t->ser, cfg->port, cfg->baud, databits, parity, stopbits,
                       true, err, errlen) != 0) {
        free(t);
        return -1;
    }

    kiss_decoder_init(&t->dec);

    if (pr_state_load(cfg, &t->st, err, errlen) != 0) {
        pr_serial_close(&t->ser);
        free(t);
        return -1;
    }

    pr_strlcpy(t->st.device, cfg->port, sizeof t->st.device);
    pr_strlcpy(t->st.status, "running", sizeof t->st.status);
    pr_strlcpy(t->st.detail, "TNC2 / KISS", sizeof t->st.detail);
    t->st.link_ok = true;
    t->st.duplex  = cfg->duplex;
    pr_strlcpy(t->mycall, cfg->callerid, sizeof t->mycall);

    r->impl = t;

    /*
     * Nur beim ALLERERSTEN Start melden. Der Port wird bei jedem
     * CGI-Aufruf neu geoeffnet - eine Meldung pro Aufruf wuerde das
     * Terminal fluten. Es ist Rauschen, kein Status.
     */
    {
        char state_file[640];
        pr_state_path(cfg, state_file, sizeof state_file);
        if (!pr_file_exists(state_file))
            tnc2_note(r, PR_MSG_SYS, "SYS", "TNC2 angebunden (KISS)");
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
     * Der Port wird geschlossen - erst im KISS-Betrieb ist das unkritisch.
     * Im Command-Mode wuerde das DTR den Echo-only-Zustand ausloesen.
     */
    pr_serial_close(&t->ser);
    free(t);
    r->impl = NULL;
}

static int tnc2_refresh(pr_rig *r, char *err, size_t errlen)
{
    tnc2_impl *t = r->impl;
    if (t == NULL) {
        snprintf(err, errlen, "TNC nicht verbunden");
        return -1;
    }

    /* Halbduplex: waehrend ptt=1 wird nicht empfangen */
    t->st.rx_muted = (t->st.duplex == PR_DUPLEX_HALF) && t->st.ptt;

    if (t->st.rx_muted)
        return 0;

    /* Empfangene Rahmen abholen */
    unsigned char buf[2048];
    long n = pr_serial_read(&t->ser, buf, sizeof buf, 50, err, errlen);
    if (n < 0)
        return -1;

    if (n > 0) {
        size_t frames = kiss_decoder_feed_buf(&t->dec, buf, (size_t)n);
        for (size_t i = 0; i < frames; i++) {
            unsigned char frame[1024];
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
        snprintf(err, errlen, "TNC nicht verbunden");
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
        snprintf(err, errlen, "TNC nicht verbunden");
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
        snprintf(err, errlen, "TNC nicht verbunden");
        return -1;
    }

    if (on && t->st.monitor) {
        snprintf(err, errlen, "Monitorbetrieb: Senden ist gesperrt");
        return -1;
    }
    (void)errlen;

    /*
     * PTT wird bei KISS von der Hardware gefuehrt - beim Senden eines
     * Rahmens schaltet das TNC selbst. Hier merken wir nur den Zustand.
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
        snprintf(err, errlen, "TNC nicht verbunden");
        return -1;
    }
    (void)errlen;

    t->st.duplex = d;
    t->st.rx_muted = (d == PR_DUPLEX_HALF) && t->st.ptt;

    /*
     * KISS-Parameter FULLDUPLEX (0x05) an das Geraet melden.
     * Das ist der Hebel, ueber den die Hardware Vollduplex kann.
     */
    unsigned char frame[8];
    size_t n = kiss_encode(frame, sizeof frame, 0, KISS_CMD_FULLDUPLEX,
                           NULL, 0);
    if (n == 0) {
        /* Wert muss als Payload uebermittelt werden */
        unsigned char v = (d == PR_DUPLEX_FULL) ? 1 : 0;
        n = kiss_encode(frame, sizeof frame, 0, KISS_CMD_FULLDUPLEX, &v, 1);
    }
    if (n > 0) {
        char e2[64];
        (void)pr_serial_write(&t->ser, frame, n, e2, sizeof e2);
    }

    tnc2_save(r, t);
    return 0;
}

static int tnc2_set_monitor(pr_rig *r, bool on, char *err, size_t errlen)
{
    tnc2_impl *t = r->impl;
    if (t == NULL) {
        snprintf(err, errlen, "TNC nicht verbunden");
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
        snprintf(err, errlen, "TNC nicht verbunden");
        return -1;
    }
    if (t->st.monitor) {
        snprintf(err, errlen, "Monitorbetrieb: Senden ist gesperrt");
        return -1;
    }
    if (text == NULL || text[0] == '\0') {
        snprintf(err, errlen, "leere Nachricht");
        return -1;
    }

    /*
     * AX.25-UI-Rahmen bauen. "to" ist die anzurufende Station; leer oder
     * CQ steht fuer einen Rundruf ohne festen Partner.
     */
    if (to == NULL || to[0] == '\0')
        to = "CQ";

    unsigned char ui[512];
    size_t uilen = ax25_ui_frame(ui, sizeof ui, from, to,
                                 (const unsigned char *)text, strlen(text));
    if (uilen == 0) {
        snprintf(err, errlen, "Rahmen konnte nicht gebaut werden");
        return -1;
    }

    /* KISS-Rahmen: FEND | 0x00 | escaped Payload | FEND
     * Der FCS wird NICHT mitgeliefert - den berechnet und ergaenzt das TNC. */
    unsigned char frame[640];
    size_t flen = kiss_encode(frame, sizeof frame, 0, KISS_CMD_DATA, ui, uilen);
    if (flen == 0) {
        snprintf(err, errlen, "KISS-Rahmen zu gross");
        return -1;
    }

    if (pr_serial_write(&t->ser, frame, flen, err, errlen) != 0)
        return -1;

    tnc2_note(r, PR_MSG_TX, from, text);
    t->st.tx_count++;
    t->st.last_tx_ts = pr_now_s();
    tnc2_save(r, t);
    return 0;
}

/*
 * Pruef-Trager fuer den Adminbereich.
 *
 * Bei KISS schaltet die Hardware die Sendung beim Rahmen selbst - es
 * gibt keinen Befehl fuer "nur Traeger ohne Inhalt". Die Testdauer wird
 * daher ueber die RAHMENLAENGE abgebildet: der Nutzdatenanteil wird so
 * bemessen, dass die Uebertragung ungefaehr die gewuenschte Zeit
 * dauert. Inhalt ist Null-Padding, also ohne jede Bedeutung.
 *
 * Das ist ehrlicher als ein "PTT an", das bei KISS nur eine Variable
 * setzt und nichts auf die Luft gibt.
 */
static int tnc2_carrier_test(pr_rig *r, unsigned seconds,
                             char *err, size_t errlen)
{
    tnc2_impl *t = r->impl;
    if (t == NULL) {
        snprintf(err, errlen, "TNC nicht verbunden");
        return -1;
    }
    if (seconds == 0 || seconds > 10) {
        snprintf(err, errlen, "Dauer muss zwischen 1 und 10 Sekunden liegen");
        return -1;
    }
    if (t->st.monitor) {
        snprintf(err, errlen, "Monitorbetrieb: Senden ist gesperrt");
        return -1;
    }

    /* Bits auf der Luft / 8 = Bytes gesamt, abzueglich AX.25-Kopf (16) + FCS (2). */
    long baud = r->cfg->radio_baud > 0 ? r->cfg->radio_baud : 1200;
    long total = (baud * (long)seconds) / 8;
    long payload = total - 18;
    if (payload < 32)
        payload = 32;
    if (payload > 4000)
        payload = 4000;

    unsigned char pad[4000];
    memset(pad, 0, (size_t)payload);

    unsigned char ui[4200];
    size_t uilen = ax25_ui_frame(ui, sizeof ui, r->cfg->callerid, "CQ",
                                 pad, (size_t)payload);
    if (uilen == 0) {
        snprintf(err, errlen, "Pruefrahmen konnte nicht gebaut werden");
        return -1;
    }

    unsigned char frame[8400];
    size_t flen = kiss_encode(frame, sizeof frame, 0, KISS_CMD_DATA, ui, uilen);
    if (flen == 0) {
        snprintf(err, errlen, "KISS-Rahmen zu gross");
        return -1;
    }

    if (pr_serial_write(&t->ser, frame, flen, err, errlen) != 0)
        return -1;

    tnc2_note(r, PR_MSG_TX, r->cfg->callerid,
              "[Pruef-Trager ohne Inhalt]");
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
    "TNC2-Klasse (Landolt TNC2C, PK-TNC2) ueber KISS",
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
