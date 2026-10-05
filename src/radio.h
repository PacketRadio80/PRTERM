/*
 * PRTERM - CB & Amateur Radio Terminal
 * radio.h - Rig-Abstraktion.
 *
 * Alle Geraete hinter einer VTable. Das Terminal kennt nur diese Schnitt-
 * stelle; TNC2-Klone, T-Modem, MAX25 und die Simulation sind austauschbar.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PRTERM_RADIO_H
#define PRTERM_RADIO_H

#include "bands.h"
#include "callsign.h"
#include "config.h"

#include <stdbool.h>
#include <stddef.h>

#define PR_MSG_TEXT 240

/* Nachrichtenart */
#define PR_MSG_RX   'R'
#define PR_MSG_TX   'T'
#define PR_MSG_SYS  'S'
#define PR_MSG_WARN 'W'
#define PR_MSG_ERR  'E'

typedef struct pr_msg {
    char      kind;
    char      from[PR_CALLSIGN_MAX];
    char      text[PR_MSG_TEXT];
    int       db;
    long long ts;
} pr_msg;

typedef struct pr_rig_state {
    long      freq_hz;
    unsigned  mode;
    bool      ptt;
    bool      monitor;       /* Nur-Empfang */
    bool      rx_muted;      /* half: RX waehrend TX geschaltet */
    bool      squelch_open;
    int       rx_db;
    int       tx_db;
    pr_duplex duplex;

    char      last_rx_from[PR_CALLSIGN_MAX];
    char      last_rx_text[PR_MSG_TEXT];
    long long last_rx_ts;
    long long last_tx_ts;

    long      rx_count;
    long      tx_count;

    char      device[64];
    char      status[16];    /* running | stopped | error */
    bool      link_ok;
    char      detail[128];
} pr_rig_state;

typedef struct pr_rig pr_rig;

typedef struct pr_rig_vtbl {
    const char *name;         /* INI-Wert fuer [radio] driver */
    const char *description;

    int  (*open)(pr_rig *r, char *err, size_t errlen);
    void (*close)(pr_rig *r);

    /* Geraet abfragen / Simulation fortschreiben.
     * Liefert neue RX-Nachrichten ueber drain(). */
    int  (*refresh)(pr_rig *r, char *err, size_t errlen);
    int  (*get_state)(pr_rig *r, pr_rig_state *out);

    int  (*set_freq)(pr_rig *r, long freq_hz, char *err, size_t errlen);
    int  (*set_mode)(pr_rig *r, unsigned mode, char *err, size_t errlen);
    int  (*set_ptt)(pr_rig *r, bool on, char *err, size_t errlen);
    int  (*set_duplex)(pr_rig *r, pr_duplex d, char *err, size_t errlen);
    int  (*set_monitor)(pr_rig *r, bool on, char *err, size_t errlen);

    /* Senden. Die Compliance-Pruefung liegt VORHER im Aufrufer. */
    int  (*send)(pr_rig *r, const char *from, const char *text,
                 char *err, size_t errlen);

    /* Pruef-Trager: haelt die Sendung fuer eine gegebene Zeit offen.
     *
     * Das ist ein GERAETETEST fuer den Adminbereich - kein Betrieb.
     * Wichtig fuer KISS: dort schaltet die Hardware beim Rahmen selbst,
     * es gibt keinen Befehl fuer "nur Traeger". Ein KISS-Treiber muss
     * die Zeit daher ueber die Rahmenlaenge abbilden.
     *
     * Die Compliance-Pruefung liegt wie beim Senden VORHER im Aufrufer. */
    int  (*carrier_test)(pr_rig *r, unsigned seconds, char *err, size_t errlen);

    /* Neue RX-Nachrichten abholen (seit dem letzten Aufruf). */
    int  (*drain)(pr_rig *r, pr_msg *out, size_t cap, size_t *n);
} pr_rig_vtbl;

struct pr_rig {
    const pr_rig_vtbl *vtbl;
    void              *impl;
    const pr_config   *cfg;
};

/* ---- Registry --------------------------------------------------------- */
size_t             pr_rig_count(void);
const pr_rig_vtbl *pr_rig_at(size_t idx);
const pr_rig_vtbl *pr_rig_find(const char *name);

/* ---- Komfort-Dispatch ------------------------------------------------- */
int  pr_rig_open(pr_rig *r, const pr_config *cfg, char *err, size_t errlen);
void pr_rig_close(pr_rig *r);

/* ---- Treiber ---------------------------------------------------------- */
/* Die Deklarationen sind bewusst unbedingt: sie sind nur Prototypen, die
 * tatsaechliche Verdrahtung passiert in radio.c abhaengig von den
 * PRTERM_RIG_*-Schaltern. So findet -Wmissing-prototypes immer ein Prototyp. */
extern const pr_rig_vtbl pr_rig_sim;
extern const pr_rig_vtbl pr_rig_tnc2;
extern const pr_rig_vtbl pr_rig_tmodem;
extern const pr_rig_vtbl pr_rig_max25;

#endif /* PRTERM_RADIO_H */
