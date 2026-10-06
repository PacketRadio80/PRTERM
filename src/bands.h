/*
 * PRTERM - CB & Amateur Radio Terminal
 * bands.h - Band plan + compliance layer.
 *
 * This layer sits BEFORE the transmit path. What it rejects does not go
 * on the air - not even as a KISS DATA frame.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PRTERM_BANDS_H
#define PRTERM_BANDS_H

#include <stdbool.h>
#include <stddef.h>

/* ---- Operating modes ------------------------------------------------------ */
#define PR_BAND_FM  0x01u
#define PR_BAND_AM  0x02u
#define PR_BAND_SSB 0x04u

/* ---- Channel properties -------------------------------------------------------- */
#define PR_CH_F_DATA    0x01u  /* also intended for digital data        */
#define PR_CH_F_GATEWAY 0x02u  /* Interconnection / unattended stations  */

typedef struct pr_channel {
    int      num;        /* 1..80 */
    long     freq_hz;    /* Center frequency */
    unsigned modes;      /* PR_BAND_* bit mask */
    unsigned flags;      /* PR_CH_F_* bit mask */
} pr_channel;

typedef struct pr_bandplan {
    const char       *id;        /* "cb-de" */
    const char       *name;
    const char       *country;   /* "DE" */
    const char       *source;    /* Legal basis     */
    long              bw_hz;     /* Channel bandwidth */
    const pr_channel *ch;
    size_t            nch;
} pr_bandplan;

/* ---- Band plans -------------------------------------------------------- */
const pr_bandplan *pr_bandplan_default(void);      /* cb-de */
const pr_bandplan *pr_bandplan_by_id(const char *id);
size_t             pr_bandplan_count(void);
const pr_bandplan *pr_bandplan_at(size_t idx);

/* ---- Channel lookup ---------------------------------------------------------- */
const pr_channel *pr_bandplan_channel(const pr_bandplan *bp, int num);
const pr_channel *pr_bandplan_at_freq(const pr_bandplan *bp, long freq_hz);

/* ---- Power limits (milliwatts ERP or PEP)      ----------------------- */
long pr_bandplan_max_power_mw(const pr_bandplan *bp, int num, unsigned mode);

/* ---- Checks  --------------------------------------------------------- */
/* Checks frequency, channel spacing, mode and power.
 * Returns true if transmitting is allowed; otherwise false + reason. */
bool pr_bandplan_tx_allowed(const pr_bandplan *bp,
                            long freq_hz,
                            unsigned mode,
                            long power_mw,
                            char *err, size_t errlen);

/* Without power value (frequency + mode only).       */
bool pr_bandplan_tx_freq_ok(const pr_bandplan *bp,
                            long freq_hz, unsigned mode,
                            char *err, size_t errlen);

/* ---- Name conversion     --------------------------------------------- */
const char *pr_band_mode_name(unsigned mode);   /* Single bit -> "fm"/"am"/"ssb" */
unsigned    pr_band_mode_from_name(const char *s);
/* Multiple bits -> "FM/AM/SSB" into the buffer */
void        pr_band_modes_str(unsigned modes, char *dst, size_t dstlen);

#endif /* PRTERM_BANDS_H */
