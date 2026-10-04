/*
 * PRTERM - CB & Amateur Radio Terminal
 * probe.h - TNC-Erkennung.
 *
 * Es gibt keine Baud-Unterhandlung am TNC: die Werte stehen in der INI und
 * werden offline ermittelt. Dieses Modul faehrt die ueblichen Profile ab
 * und bewertet die Antworten.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PRTERM_PROBE_H
#define PRTERM_PROBE_H

#include "serial.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct pr_probe_result {
    char dev[256];
    long baud;
    int  databits;
    int  parity;
    int  stopbits;
    int  score;
    bool responds;
    bool echo_only;        /* TNC spiegelt nur - Echo ist noch an */
    bool clean_link;       /* sauberes Echo ODER echte Antwort, kein Muell */
    char banner[512];      /* Rohantwort zum Anschauen */
} pr_probe_result;

/* Fortschritt melden; Rueckgabe != 0 bricht ab. */
typedef int (*pr_probe_cb)(void *ud, const char *msg);

/*
 * Faeht die Profile gegen ein Geraet durch und liefert das beste.
 * Rueckgabe 0 wenn mindestens ein Profil geantwortet hat.
 */
int pr_probe_device(const char *dev, pr_probe_result *best,
                    pr_probe_cb progress, void *ud,
                    char *err, size_t errlen);

/* Zeilenformat als Text, z.B. "19200 8N1". */
void pr_probe_format(const pr_probe_result *r, char *dst, size_t dstlen);

/*
 * Fuehrt das Geraet in einen bekannten Zustand zurueck (KISS verlassen,
 * Hostmode verlassen). Das ist noetig, weil ein TNC im KISS- oder Hostmode
 * auf Textkommandos schlicht nicht antwortet.
 */
void pr_probe_reset(pr_serial *s);

/* ---- Reine Bewertungsfunktionen ---------------------------------------
 * Bewusst oeffentlich, damit sie sich direkt testen lassen. Das
 * Echo-Stripping war zweimal falsch und hat sich im Betrieb bemerkbar
 * gemacht - genau dieses Verhalten gehoert in einen Test. */

/* Entfernt alle Vorkommen eines Musters; liefert die neue Laenge. */
size_t pr_probe_remove_bytes(unsigned char *buf, size_t len,
                             const unsigned char *needle, size_t nlen);

/* Entfernt die gesendeten Sondierungen aus der Antwort. */
size_t pr_probe_strip_echo(unsigned char *buf, size_t len);

/* Anteil druckbarer Zeichen - Muell liegt deutlich unter 1. */
double pr_probe_printable_ratio(const unsigned char *buf, size_t len);

/* Bleibt nach Entfernen von Weissschraum und NUL etwas Substanzielles? */
bool pr_probe_has_content(const unsigned char *buf, size_t len);

/* Bewertet eine echte Antwort (ohne Echo). 0 bei Muell oder nichts. */
int pr_probe_score(const unsigned char *buf, size_t len);

#endif /* PRTERM_PROBE_H */
