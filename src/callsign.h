/*
 * PRTERM - CB & Amateur Radio Terminal
 * callsign.h - CALLID/CALLERID-Validierung und Ban-Matching.
 *
 * PRTERM ist hier BEWUSST strenger als AX.25:
 *
 *   CALLID   : Basis <= 6 Zeichen [A-Z0-9], kein Suffix
 *   CALLERID : Basis <= 6 Zeichen + optional "-<Ziffer>"
 *              Gesamt <= 8 Zeichen  ("6 + 2")
 *
 * Die Grenzen sind konfigurierbar (siehe [callsign] in der prterm.ini),
 * wer AX.25-konform bis -15 braucht, setzt ssid_digits = 2.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PRTERM_CALLSIGN_H
#define PRTERM_CALLSIGN_H

#include <stdbool.h>
#include <stddef.h>

#define PR_CALLSIGN_MAX 16

typedef struct pr_call_rules {
    int  callid_max_len;       /* Basislaenge CALLID            (6) */
    int  callerid_base_len;    /* Basislaenge CALLERID          (6) */
    int  callerid_max_total;   /* Gesamtlaenge inkl. SSID       (8) */
    bool allow_ssid;           /* "-<n>" zulassen                  */
    int  ssid_digits;          /* Ziffern nach '-'              (1) */
} pr_call_rules;

void pr_call_rules_default(pr_call_rules *r);

/* ---- CALLID: ohne Suffix --------------------------------------------- */
bool callid_valid(const char *s, const pr_call_rules *r);
/* Normalisiert (Grossschreibung, Blanks raus). true wenn danach gueltig. */
bool callid_normalize(char *dst, size_t dstlen, const char *src,
                      const pr_call_rules *r);

/* ---- CALLERID: mit optionaler SSID ----------------------------------- */
bool callerid_valid(const char *s, const pr_call_rules *r);
bool callerid_normalize(char *dst, size_t dstlen, const char *src,
                        const pr_call_rules *r);
/* Zerlegt "DL1ABC-1" in Basis und SSID. ssid = -1 wenn keine SSID. */
bool callerid_split(const char *s, char *base, size_t baselen, int *ssid);

/* Beides akzeptieren (z.B. fuer Ban-Eintraege und Zielrufzeichen). */
bool anyid_valid(const char *s, const pr_call_rules *r);

/* ---- Muster ---------------------------------------------------------- */
/* '*' = beliebig viele Zeichen, '?' = genau ein Zeichen. */
bool call_pattern_match(const char *pattern, const char *id);
/* Gleicht ab gegen eine Liste von Mustern; liefert das erste Treffermuster. */
const char *call_pattern_list_match(const char *const *patterns, size_t npatterns,
                                    const char *id);

/* ---- AX.25-Wire-Encoding (fuer die spaetere TNC-Anbindung) ----------- */
/* Zerlegt und kodiert eine Adresse auf 7 Byte shifted ASCII.
 * dst muss 7 Byte fassen. false bei ungueltigem Rufzeichen. */
bool call_to_ax25(const char *id, unsigned char dst[7]);
/* Umgekehrte Richtung; dst faengt "CALL-SSID" bzw. "CALL". */
bool call_from_ax25(const unsigned char src[7], char *dst, size_t dstlen);

#endif /* PRTERM_CALLSIGN_H */
