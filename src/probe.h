/*
 * PRTERM - CB & Amateur Radio Terminal
 * probe.h - TNC-Erkennung und Boot-Abfang.
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
    bool echo_only;        /* Geraet spiegelt nur - Echo war an */
    bool banner;           /* Firmware-Banner erkannt */
    bool clean_link;       /* Banner, Echo oder klare Antwort - kein Muell */
    char answer[512];      /* echte Antwort nach Echo-Entfernung */
    int  fd;               /* bei pr_probe_bootwait: OFFEN lassen! */
} pr_probe_result;

/* Fortschritt melden; Rueckgabe != 0 bricht ab. */
typedef int (*pr_probe_cb)(void *ud, const char *msg);

/*
 * Faeht die Profile gegen ein Geraet durch.
 *
 * Der Port wird dabei GENAU EINMAL geoeffnet und bis zum Schluss offen
 * gehalten - ein Schliessen laesst DTR fallen und versetzt einen TNC2C in
 * einen Zustand ohne Antwort.
 *
 * Rueckgabe 0 wenn mindestens ein Profil geantwortet hat.
 */
int pr_probe_device(const char *dev, pr_probe_result *best,
                    pr_probe_cb progress, void *ud,
                    char *err, size_t errlen);

/*
 * Faengt den Boot-Banner eines Geraets ab.
 *
 * Aufruf: Port oeffnen, DTR/RTS anlegen, DANN das Geraet einschalten und
 * hier warten. Es wird nur gelesen, nichts gesendet.
 *
 * WICHTIG: bei Erfolg ist best->fd offen - NICHT schliessen, solange das
 * Geraet weiter betrieben werden soll.
 */
int pr_probe_bootwait(const char *dev, long baud, int databits, int parity,
                      int stopbits, int seconds,
                      pr_probe_result *best, pr_probe_cb progress, void *ud,
                      char *err, size_t errlen);

/*
 * Fuehrt das Geraet in einen bekannten Zustand zurueck und liest die
 * Antwort mit. Der Port bleibt dabei offen.
 *
 * Reihenfolge aus tnc_serial_recovery.py:
 *
 *   11 18                       Puffer leeren (^Q^X)
 *   300 x 00 + JHOST 0          WA8DED-Hostmode verlassen
 *   C0 FF C0                    KISS verlassen - bei TheFirmware zugleich
 *                               FIRMWARE-RESET, der den Banner ausloest
 *   ESC V                       Probe
 *
 * WICHTIG: die Antwort wird NICHT verworfen. Ein frueherer Entwurf hat am
 * Ende gecleart und damit den ausgeloesten Boot-Banner weggeworfen.
 *
 * Liefert die Laenge der gelesenen Antwort.
 */
size_t pr_probe_reset(pr_serial *s, unsigned char *out, size_t outcap);

/* Zeilenformat als Text, z.B. "19200 7E1". */
void pr_probe_format(const pr_probe_result *r, char *dst, size_t dstlen);

/* ---- Reine Bewertungsfunktionen --------------------------------------
 * Bewusst oeffentlich, damit sie sich direkt testen lassen. */

size_t pr_probe_remove_bytes(unsigned char *buf, size_t len,
                             const unsigned char *needle, size_t nlen);
size_t pr_probe_strip_echo(unsigned char *buf, size_t len);
double pr_probe_printable_ratio(const unsigned char *buf, size_t len);
bool   pr_probe_has_content(const unsigned char *buf, size_t len);
int    pr_probe_score(const unsigned char *buf, size_t len);

/* Firmware-Marker erkennen - das ist das eigentliche Kriterium fuer
 * "hier spricht ein TNC", unabhaengig von der Punktzahl. */
bool   pr_probe_has_banner(const unsigned char *buf, size_t len);

#endif /* PRTERM_PROBE_H */
