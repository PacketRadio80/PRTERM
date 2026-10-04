/*
 * PRTERM - CB & Amateur Radio Terminal
 * bands.h - Bandplan + Compliance-Schicht.
 *
 * Diese Schicht liegt VOR dem Sendepfad. Was sie ablehnt, geht nicht auf
 * die Luft - auch nicht als KISS DATA Frame.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PRTERM_BANDS_H
#define PRTERM_BANDS_H

#include <stdbool.h>
#include <stddef.h>

/* ---- Betriebsarten ---------------------------------------------------- */
#define PR_BAND_FM  0x01u
#define PR_BAND_AM  0x02u
#define PR_BAND_SSB 0x04u

/* ---- Kanalmerkmale --------------------------------------------------- */
#define PR_CH_F_DATA    0x01u  /* auch fuer digitale Daten vorgesehen   */
#define PR_CH_F_GATEWAY 0x02u  /* Zusammenschaltung / unbemannte Anlagen */

typedef struct pr_channel {
    int      num;        /* 1..80 */
    long     freq_hz;    /* Mittenfrequenz */
    unsigned modes;      /* PR_BAND_* Bitmaske */
    unsigned flags;      /* PR_CH_F_* Bitmaske */
} pr_channel;

typedef struct pr_bandplan {
    const char       *id;        /* "cb-de" */
    const char       *name;
    const char       *country;   /* "DE" */
    const char       *source;    /* Rechtsgrundlage */
    long              bw_hz;     /* Kanalbandbreite */
    const pr_channel *ch;
    size_t            nch;
} pr_bandplan;

/* ---- Bandpläne ------------------------------------------------------- */
const pr_bandplan *pr_bandplan_default(void);      /* cb-de */
const pr_bandplan *pr_bandplan_by_id(const char *id);
size_t             pr_bandplan_count(void);
const pr_bandplan *pr_bandplan_at(size_t idx);

/* ---- Kanalsuche ------------------------------------------------------ */
const pr_channel *pr_bandplan_channel(const pr_bandplan *bp, int num);
const pr_channel *pr_bandplan_at_freq(const pr_bandplan *bp, long freq_hz);

/* ---- Leistungsgrenzen (Milliwatt ERP bzw. PEP) ----------------------- */
long pr_bandplan_max_power_mw(const pr_bandplan *bp, int num, unsigned mode);

/* ---- Prüfung --------------------------------------------------------- */
/* Prueft Frequenz, Kanalraster, Betriebsart und Leistung.
 * Liefert true wenn Senden zulaessig; sonst false + Fehlergrund. */
bool pr_bandplan_tx_allowed(const pr_bandplan *bp,
                            long freq_hz,
                            unsigned mode,
                            long power_mw,
                            char *err, size_t errlen);

/* Ohne Leistungsangabe (nur Frequenz + Betriebsart). */
bool pr_bandplan_tx_freq_ok(const pr_bandplan *bp,
                            long freq_hz, unsigned mode,
                            char *err, size_t errlen);

/* ---- Namenskonvertierung --------------------------------------------- */
const char *pr_band_mode_name(unsigned mode);   /* Einzelbit -> "fm"/"am"/"ssb" */
unsigned    pr_band_mode_from_name(const char *s);
/* Mehrere Bits -> "FM/AM/SSB" in den Puffer */
void        pr_band_modes_str(unsigned modes, char *dst, size_t dstlen);

#endif /* PRTERM_BANDS_H */
