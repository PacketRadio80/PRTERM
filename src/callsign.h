/*
 * PRTERM - CB & Amateur Radio Terminal
 * callsign.h - CALLID/CALLERID validation and ban matching.
 *
 * PRTERM is DELIBERATELY stricter than AX.25 here:
 *
 *   CALLID   : base <= 6 chars [A-Z0-9], no suffix
 *   CALLERID : base <= 6 chars + optional "-<digit>"
 *              total <= 8 chars  ("6 + 2")
 *
 * The limits are configurable (see [callsign] in prterm.ini);
 * anyone needing AX.25 conformance up to -15 sets ssid_digits = 2.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PRTERM_CALLSIGN_H
#define PRTERM_CALLSIGN_H

#include <stdbool.h>
#include <stddef.h>

#define PR_CALLSIGN_MAX 16

typedef struct pr_call_rules {
    int  callid_max_len;       /* Base length CALLID            (6) */
    int  callerid_base_len;    /* Base length CALLERID          (6) */
    int  callerid_max_total;   /* Total length incl. SSID       (8) */
    bool allow_ssid;           /* "-<n>" allowed                   */
    int  ssid_digits;          /* Digits after '-'              (1) */
} pr_call_rules;

void pr_call_rules_default(pr_call_rules *r);

/* ---- CALLID: without suffix ------------------------------------------------ */
bool callid_valid(const char *s, const pr_call_rules *r);
/* Normalizes (uppercase, strip blanks). True if valid afterwards.        */
bool callid_normalize(char *dst, size_t dstlen, const char *src,
                      const pr_call_rules *r);

/* ---- CALLERID: with optional SSID  ----------------------------------- */
bool callerid_valid(const char *s, const pr_call_rules *r);
bool callerid_normalize(char *dst, size_t dstlen, const char *src,
                        const pr_call_rules *r);
/* Splits "DL1ABC-1" into base and SSID. ssid = -1 if no SSID.      */
bool callerid_split(const char *s, char *base, size_t baselen, int *ssid);

/* Accept both (e.g. for ban entries and destination callsigns).    */
bool anyid_valid(const char *s, const pr_call_rules *r);

/* ---- Patterns ------------------------------------------------------------ */
/* '*' = any number of chars, '?' = exactly one char.     */
bool call_pattern_match(const char *pattern, const char *id);
/* Matches against a pattern list; returns the first matching pattern.       */
const char *call_pattern_list_match(const char *const *patterns, size_t npatterns,
                                    const char *id);

/* ---- AX.25 wire encoding (for the later TNC hookup)        ----------- */
/* Splits and encodes an address to 7 bytes shifted ASCII.
 * dst must hold 7 bytes. False for an invalid callsign. */
bool call_to_ax25(const char *id, unsigned char dst[7]);
/* Reverse direction; dst receives "CALL-SSID" or "CALL".   */
bool call_from_ax25(const unsigned char src[7], char *dst, size_t dstlen);

/* ---- AX.25 frames ---------------------------------------------------- */
/*
 * Builds a UI frame (without FCS - KISS gets the frame without FCS,
 * or with it, depending on the driver; see kiss.c).
 *
 * Layout: dest addr (7) | src addr (7) | Control 0x03 | PID 0xF0 | Info
 *
 * Returns the length, 0 on buffer shortage or invalid callsign.
 */
size_t ax25_ui_frame(unsigned char *out, size_t outcap,
                     const char *from, const char *to,
                     const unsigned char *info, size_t infolen);

#endif /* PRTERM_CALLSIGN_H */
