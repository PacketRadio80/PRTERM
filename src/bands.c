/*
 * PRTERM - CB & Amateur Radio Terminal
 * bands.c - Bandplan-Daten + Compliance-Schicht.
 *
 * Rechtsgrundlage des enthaltenen Plans:
 *   Bundesnetzagentur, Allgemeinzuteilung von Frequenzen fuer den CB-Funk,
 *   Vfg. Nr. 21/2021 (sowie Nachfolgeregelungen).
 *
 * Kanalbandbreite 10 kHz (§ 2 Abs. 1).
 *
 *   F3E/G3E (FM/PM)  4 W ERP    Kanal 1..80
 *   A3E     (AM)      4 W ERP    Kanal 1..40
 *   J3E     (SSB)    12 W PEP    Kanal 1..40
 *
 * WICHTIG - Lage der Bereiche:
 *   Kanal  1..40 : 26.965 .. 27.405 MHz   (CEPT, harmonisiert)
 *   Kanal 41..80 : 26.565 .. 26.955 MHz   (nationaler Erweiterungsbereich)
 * Die Zusatzkanäle liegen UNTERHALB der CEPT-Kanäle.
 *
 * Kanaldreher: 22 = 27.225 -> 23 = 27.255 -> 24 = 27.235 -> 25 = 27.245
 *
 * SPDX-License-Identifier: MIT
 */
#include "prterm_compat.h"

#include "bands.h"

#include <stdio.h>
#include <string.h>

/* ======================================================================= */
/* Kanaltabelle CB-Funk Deutschland                                        */
/* ======================================================================= */

/* Hilfsmakros fuer die Tabelle */
#define M_ALL  (PR_BAND_FM | PR_BAND_AM | PR_BAND_SSB)  /* Kanal 1..40  */
#define M_FM   (PR_BAND_FM)                             /* Kanal 41..80 */

#define F_DATA    (PR_CH_F_DATA)
#define F_GW      (PR_CH_F_GATEWAY)
#define F_DG      (PR_CH_F_DATA | PR_CH_F_GATEWAY)
#define F_NONE    (0u)

static const pr_channel cb_de_channels[] = {
    /* ---- CEPT-Bereich, Kanal 1..40 (26.965 .. 27.405 MHz) ------------ */
    {  1, 26965000L, M_ALL, F_NONE },  /* Anrufkanal FM                   */
    {  2, 26975000L, M_ALL, F_NONE },  /* Berg-DX (inoffiziell)           */
    {  3, 26985000L, M_ALL, F_NONE },
    {  4, 27005000L, M_ALL, F_NONE },  /* Anrufkanal AM                   */
    {  5, 27015000L, M_ALL, F_NONE },
    {  6, 27025000L, M_ALL, F_DATA },  /* Datenkanal                      */
    {  7, 27035000L, M_ALL, F_DATA },  /* Datenkanal                      */
    {  8, 27055000L, M_ALL, F_NONE },
    {  9, 27065000L, M_ALL, F_NONE },  /* Notrufkanal                     */
    { 10, 27075000L, M_ALL, F_NONE },
    { 11, 27085000L, M_ALL, F_GW   },  /* Gateway                         */
    { 12, 27105000L, M_ALL, F_NONE },
    { 13, 27115000L, M_ALL, F_NONE },
    { 14, 27125000L, M_ALL, F_NONE },
    { 15, 27135000L, M_ALL, F_NONE },
    { 16, 27155000L, M_ALL, F_NONE },
    { 17, 27165000L, M_ALL, F_NONE },
    { 18, 27175000L, M_ALL, F_NONE },
    { 19, 27185000L, M_ALL, F_NONE },
    { 20, 27205000L, M_ALL, F_NONE },
    { 21, 27215000L, M_ALL, F_NONE },
    { 22, 27225000L, M_ALL, F_NONE },
    { 23, 27255000L, M_ALL, F_NONE },  /* <== Kanaldreher                 */
    { 24, 27235000L, M_ALL, F_DATA },  /* <== Kanaldreher                 */
    { 25, 27245000L, M_ALL, F_DATA },  /* <== Kanaldreher                 */
    { 26, 27265000L, M_ALL, F_NONE },
    { 27, 27275000L, M_ALL, F_NONE },
    { 28, 27285000L, M_ALL, F_NONE },
    { 29, 27295000L, M_ALL, F_GW   },  /* Gateway                         */
    { 30, 27305000L, M_ALL, F_NONE },  /* DX (inoffiziell)                */
    { 31, 27315000L, M_ALL, F_NONE },  /* DX (inoffiziell)                */
    { 32, 27325000L, M_ALL, F_NONE },
    { 33, 27335000L, M_ALL, F_NONE },
    { 34, 27345000L, M_ALL, F_GW   },  /* Gateway                         */
    { 35, 27355000L, M_ALL, F_NONE },
    { 36, 27365000L, M_ALL, F_NONE },
    { 37, 27375000L, M_ALL, F_NONE },
    { 38, 27385000L, M_ALL, F_NONE },  /* DX LSB (inoffiziell)            */
    { 39, 27395000L, M_ALL, F_GW   },  /* Gateway                         */
    { 40, 27405000L, M_ALL, F_GW   },  /* Gateway                         */

    /* ---- Nationaler Erweiterungsbereich, Kanal 41..80 ---------------- */
    /*      Nur FM/PM zulaessig.                                       */
    { 41, 26565000L, M_FM, F_GW   },  /* Gateway, DX (inoffiziell)       */
    { 42, 26575000L, M_FM, F_NONE },  /* DX (inoffiziell)                */
    { 43, 26585000L, M_FM, F_NONE },
    { 44, 26595000L, M_FM, F_NONE },
    { 45, 26605000L, M_FM, F_NONE },
    { 46, 26615000L, M_FM, F_NONE },
    { 47, 26625000L, M_FM, F_NONE },
    { 48, 26635000L, M_FM, F_NONE },
    { 49, 26645000L, M_FM, F_NONE },
    { 50, 26655000L, M_FM, F_NONE },
    { 51, 26665000L, M_FM, F_NONE },
    { 52, 26675000L, M_FM, F_DATA },  /* Datenkanal                      */
    { 53, 26685000L, M_FM, F_DATA },  /* Datenkanal                      */
    { 54, 26695000L, M_FM, F_NONE },
    { 55, 26705000L, M_FM, F_NONE },
    { 56, 26715000L, M_FM, F_NONE },
    { 57, 26725000L, M_FM, F_NONE },
    { 58, 26735000L, M_FM, F_NONE },
    { 59, 26745000L, M_FM, F_NONE },
    { 60, 26755000L, M_FM, F_NONE },
    { 61, 26765000L, M_FM, F_GW   },  /* Gateway                         */
    { 62, 26775000L, M_FM, F_NONE },
    { 63, 26785000L, M_FM, F_NONE },
    { 64, 26795000L, M_FM, F_NONE },
    { 65, 26805000L, M_FM, F_NONE },
    { 66, 26815000L, M_FM, F_NONE },
    { 67, 26825000L, M_FM, F_NONE },
    { 68, 26835000L, M_FM, F_NONE },
    { 69, 26845000L, M_FM, F_NONE },
    { 70, 26855000L, M_FM, F_NONE },
    { 71, 26865000L, M_FM, F_GW   },  /* Gateway                         */
    { 72, 26875000L, M_FM, F_NONE },
    { 73, 26885000L, M_FM, F_NONE },
    { 74, 26895000L, M_FM, F_NONE },
    { 75, 26905000L, M_FM, F_NONE },
    { 76, 26915000L, M_FM, F_DATA },  /* Datenkanal                      */
    { 77, 26925000L, M_FM, F_DATA },  /* Datenkanal                      */
    { 78, 26935000L, M_FM, F_NONE },
    { 79, 26945000L, M_FM, F_NONE },
    { 80, 26955000L, M_FM, F_GW   },  /* Gateway                         */
};

static const pr_bandplan bandplans[] = {
    {
        "cb-de",
        "CB-Funk Deutschland (80 Kanäle)",
        "DE",
        "BNetzA Allgemeinzuteilung CB-Funk, Vfg. Nr. 21/2021",
        10000L,
        cb_de_channels,
        sizeof cb_de_channels / sizeof cb_de_channels[0]
    },
};

#define BANDPLAN_COUNT (sizeof bandplans / sizeof bandplans[0])

/* ======================================================================= */
/* Zugriff                                                                 */
/* ======================================================================= */

const pr_bandplan *pr_bandplan_default(void)
{
    return &bandplans[0];
}

size_t pr_bandplan_count(void)
{
    return BANDPLAN_COUNT;
}

const pr_bandplan *pr_bandplan_at(size_t idx)
{
    if (idx >= BANDPLAN_COUNT)
        return NULL;
    return &bandplans[idx];
}

const pr_bandplan *pr_bandplan_by_id(const char *id)
{
    if (id == NULL)
        return NULL;
    for (size_t i = 0; i < BANDPLAN_COUNT; i++) {
        if (strcmp(bandplans[i].id, id) == 0)
            return &bandplans[i];
    }
    return NULL;
}

const pr_channel *pr_bandplan_channel(const pr_bandplan *bp, int num)
{
    if (bp == NULL)
        return NULL;
    for (size_t i = 0; i < bp->nch; i++) {
        if (bp->ch[i].num == num)
            return &bp->ch[i];
    }
    return NULL;
}

const pr_channel *pr_bandplan_at_freq(const pr_bandplan *bp, long freq_hz)
{
    if (bp == NULL)
        return NULL;
    for (size_t i = 0; i < bp->nch; i++) {
        if (bp->ch[i].freq_hz == freq_hz)
            return &bp->ch[i];
    }
    return NULL;
}

/* ======================================================================= */
/* Leistungsgrenzen                                                        */
/* ======================================================================= */

/*
 * Sendearten und zulaessige Spitzenleistung, je Kanalbereich.
 *
 *   Kanal 1..40 :  FM/PM 4 W ERP   |  AM 4 W ERP   |  SSB 12 W PEP
 *   Kanal 41..80:  FM/PM 4 W ERP   |  (AM/SSB unzulaessig)
 */
#define MW_FM_CEPT    4000L
#define MW_AM_CEPT    4000L
#define MW_SSB_CEPT  12000L
#define MW_FM_EXT    4000L

long pr_bandplan_max_power_mw(const pr_bandplan *bp, int num, unsigned mode)
{
    if (pr_bandplan_channel(bp, num) == NULL)
        return 0;

    if (num >= 1 && num <= 40) {
        switch (mode) {
        case PR_BAND_FM:  return MW_FM_CEPT;
        case PR_BAND_AM:  return MW_AM_CEPT;
        case PR_BAND_SSB: return MW_SSB_CEPT;
        default:          return 0;
        }
    }
    /* 41..80 */
    return (mode == PR_BAND_FM) ? MW_FM_EXT : 0;
}

/* ======================================================================= */
/* Betriebsarten                                                           */
/* ======================================================================= */

const char *pr_band_mode_name(unsigned mode)
{
    switch (mode) {
    case PR_BAND_FM:  return "fm";
    case PR_BAND_AM:  return "am";
    case PR_BAND_SSB: return "ssb";
    default:          return "unbekannt";
    }
}

unsigned pr_band_mode_from_name(const char *s)
{
    if (s == NULL)
        return 0;
    if (strcmp(s, "fm") == 0 || strcmp(s, "FM") == 0)  return PR_BAND_FM;
    if (strcmp(s, "am") == 0 || strcmp(s, "AM") == 0)  return PR_BAND_AM;
    if (strcmp(s, "ssb") == 0 || strcmp(s, "SSB") == 0) return PR_BAND_SSB;
    /* TNC-Betriebsarten */
    if (strcmp(s, "lsb") == 0 || strcmp(s, "LSB") == 0) return PR_BAND_SSB;
    if (strcmp(s, "usb") == 0 || strcmp(s, "USB") == 0) return PR_BAND_SSB;
    return 0;
}

void pr_band_modes_str(unsigned modes, char *dst, size_t dstlen)
{
    if (dst == NULL || dstlen == 0)
        return;
    dst[0] = '\0';
    size_t used = 0;
    static const struct { unsigned bit; const char *name; } tbl[] = {
        { PR_BAND_FM,  "FM"  },
        { PR_BAND_AM,  "AM"  },
        { PR_BAND_SSB, "SSB" },
    };
    for (size_t i = 0; i < sizeof tbl / sizeof tbl[0]; i++) {
        if ((modes & tbl[i].bit) == 0)
            continue;
        if (used > 0 && used + 1 < dstlen)
            dst[used++] = '/';
        size_t n = strlen(tbl[i].name);
        if (used + n >= dstlen)
            break;
        memcpy(dst + used, tbl[i].name, n);
        used += n;
    }
    dst[used] = '\0';
}

/* ======================================================================= */
/* TX-Gate                                                                 */
/* ======================================================================= */

bool pr_bandplan_tx_freq_ok(const pr_bandplan *bp,
                            long freq_hz, unsigned mode,
                            char *err, size_t errlen)
{
    if (bp == NULL) {
        snprintf(err, errlen, "kein Bandplan aktiv");
        return false;
    }
    if (freq_hz <= 0) {
        snprintf(err, errlen, "ungueltige Frequenz");
        return false;
    }

    const pr_channel *ch = pr_bandplan_at_freq(bp, freq_hz);
    if (ch == NULL) {
        snprintf(err, errlen,
                 "Frequenz %ld Hz liegt nicht auf einem zugeteilten Kanal (%s)",
                 freq_hz, bp->name);
        return false;
    }

    if (mode != 0 && (ch->modes & mode) == 0) {
        char mbuf[32];
        pr_band_modes_str(ch->modes, mbuf, sizeof mbuf);
        if (ch->num >= 41 && ch->num <= 80) {
            snprintf(err, errlen,
                     "Kanal %d (%ld Hz): nur FM/PM zulaessig (nationaler "
                     "Erweiterungsbereich), nicht %s",
                     ch->num, freq_hz, pr_band_mode_name(mode));
        } else {
            snprintf(err, errlen,
                     "Kanal %d (%ld Hz): %s ist nicht zulaessig, erlaubt sind %s",
                     ch->num, freq_hz, pr_band_mode_name(mode), mbuf);
        }
        return false;
    }
    return true;
}

bool pr_bandplan_tx_allowed(const pr_bandplan *bp,
                            long freq_hz,
                            unsigned mode,
                            long power_mw,
                            char *err, size_t errlen)
{
    if (!pr_bandplan_tx_freq_ok(bp, freq_hz, mode, err, errlen))
        return false;

    const pr_channel *ch = pr_bandplan_at_freq(bp, freq_hz);
    if (ch == NULL) {
        snprintf(err, errlen, "Kanal nicht gefunden");
        return false;
    }

    if (power_mw > 0) {
        long max = pr_bandplan_max_power_mw(bp, ch->num, mode);
        if (max == 0) {
            snprintf(err, errlen,
                     "keine Leistungsgrenze fuer Kanal %d / %s",
                     ch->num, pr_band_mode_name(mode));
            return false;
        }
        if (power_mw > max) {
            snprintf(err, errlen,
                     "Leistung %ld mW uebersteigt %ld mW (Kanal %d, %s)",
                     power_mw, max, ch->num, pr_band_mode_name(mode));
            return false;
        }
    }
    return true;
}
