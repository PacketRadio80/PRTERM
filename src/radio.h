/*
 * PRTERM - CB & Amateur Radio Terminal
 * radio.h - Rig abstraction.
 *
 * All devices sit behind one VTable. The terminal knows only this
 * interface; TNC2 clones, T-Modem and the simulation are swappable.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PRTERM_RADIO_H
#define PRTERM_RADIO_H

#include "bands.h"
#include "callsign.h"
#include "config.h"

#include <stdbool.h>
#include <stddef.h>

#define PR_MSG_TEXT 240

/* Message type   */
#define PR_MSG_RX   'R'
#define PR_MSG_TX   'T'
#define PR_MSG_SYS  'S'
#define PR_MSG_WARN 'W'
#define PR_MSG_ERR  'E'

typedef struct pr_msg {
    char      kind;
    char      from[PR_CALLSIGN_MAX];   /* Sender   */
    char      to[PR_CALLSIGN_MAX];     /* Destination - empty = broadcast */
    char      station[32];             /* which device picked it up      */
    char      text[PR_MSG_TEXT];
    int       db;
    long long ts;
} pr_msg;

typedef struct pr_rig_state {
    long      freq_hz;
    unsigned  mode;
    bool      ptt;
    bool      monitor;       /* RX only     */
    bool      rx_muted;      /* half: RX switched during TX     */
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
    const char *name;         /* INI value for [radio] driver */
    const char *description;

    int  (*open)(pr_rig *r, char *err, size_t errlen);
    void (*close)(pr_rig *r);

    /* Poll the device / advance the simulation.
     * Returns new RX messages via drain(). */
    int  (*refresh)(pr_rig *r, char *err, size_t errlen);
    int  (*get_state)(pr_rig *r, pr_rig_state *out);

    int  (*set_freq)(pr_rig *r, long freq_hz, char *err, size_t errlen);
    int  (*set_mode)(pr_rig *r, unsigned mode, char *err, size_t errlen);
    int  (*set_ptt)(pr_rig *r, bool on, char *err, size_t errlen);
    int  (*set_duplex)(pr_rig *r, pr_duplex d, char *err, size_t errlen);
    int  (*set_monitor)(pr_rig *r, bool on, char *err, size_t errlen);

    /* Transmit. The compliance check happens BEFORE, in the caller.
     * "to" is the station to call, empty or "CQ" = broadcast. */
    int  (*send)(pr_rig *r, const char *from, const char *to,
                 const char *text, char *err, size_t errlen);

    /* Test carrier: keeps the transmitter open for a given time.
     *
     * This is a DEVICE TEST for the admin area - not operation.
     * For KISS the hardware switches with the frame itself - there
     * is no "carrier only" command. A KISS driver must map the
     * time via the frame length.
     *
     * The compliance check happens BEFORE, in the caller. */
    int  (*carrier_test)(pr_rig *r, unsigned seconds, char *err, size_t errlen);

    /* Fetch new RX messages (since the last call).           */
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

/* ---- Convenience dispatch ----------------------------------------------------- */
int  pr_rig_open(pr_rig *r, const pr_config *cfg, char *err, size_t errlen);
void pr_rig_close(pr_rig *r);

/* ---- Drivers ---------------------------------------------------------- */
/* The declarations are deliberately unconditional: they are just
 * prototypes, the actual wiring happens in radio.c depending on the
 * PRTERM_RIG_* switches. So -Wmissing-prototypes always finds one. */
extern const pr_rig_vtbl pr_rig_sim;
extern const pr_rig_vtbl pr_rig_tnc2;
extern const pr_rig_vtbl pr_rig_tmodem;

#endif /* PRTERM_RADIO_H */
