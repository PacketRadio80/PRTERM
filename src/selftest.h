/*
 * PRTERM - CB & Amateur Radio Terminal
 * selftest.h - Zustandspruefung der TNCs und Notfall-Ruecksetzung.
 *
 * Der Test ist bewusst TEIL des Programms und kein externes Skript:
 * wer betreibt, soll im Zweifel genau wissen, an welcher Stelle es hakt -
 * und im Notfall einen Reset ausloesen koennen, ohne Shell-Zugriff.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PRTERM_SELFTEST_H
#define PRTERM_SELFTEST_H

#include "config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

typedef enum pr_test_status {
    PR_TEST_PASS = 0,
    PR_TEST_WARN = 1,
    PR_TEST_FAIL = 2,
    PR_TEST_SKIP = 3
} pr_test_status;

typedef struct pr_test_result {
    char name[64];
    int  status;          /* pr_test_status */
    char detail[256];
} pr_test_result;

#define PR_SELFTEST_MAX 32

typedef struct pr_selftest {
    pr_test_result items[PR_SELFTEST_MAX];
    size_t n;
    bool   overall_ok;
    char   firmware[128];   /* erkannte Firmware, falls vorhanden */
} pr_selftest;

/*
 * Prueft das Geraet aus der Konfiguration:
 *   - existiert die Schnittstelle
 *   - laesst sich der Port oeffnen
 *   - liegen DTR/RTS an
 *   - antwortet das Geraet (ESC V)
 *   - wird die Firmware erkannt
 *   - passt das Profil zur Konfiguration
 *
 * Rueckgabe 0 wenn alles in Ordnung, sonst Anzahl der Fehler.
 */
int pr_selftest_run(const pr_config *cfg, pr_selftest *out);

/*
 * Notfall-Ruecksetzung: fuehrt das Geraet ueber die bekannte
 * Ruecksetzfolge in einen definierten Zustand zurueck und prueft danach
 * erneut.
 *
 *   11 18                    Puffer leeren (^Q^X)
 *   300 x 00 + JHOST 0       WA8DED-Hostmode verlassen
 *   C0 FF C0                 KISS verlassen / Firmware-Ruecksetz
 *   ESC V                    Probe
 *
 * Rueckgabe 0 wenn das Geraet danach spricht.
 */
int pr_selftest_reset(const pr_config *cfg, pr_selftest *out);

/*
 * Wiederherstellung: stellt sicher, dass das Geraet im KISS-Modus
 * steht und keinen Speicher voll haengender Rahmen mit sich traegt.
 *
 * ACHTUNG - die Reihenfolge ist entscheidend. Solange ein TNC im
 * KISS-Modus steht, wird JEDES geschriebene Byte gesendet. Ein "mal
 * eben nachfragen" wuerde also selbst senden. Darum:
 *
 *   1. C0 FF C0        KISS verlassen - Kontrollrahmen, geht NICHT auf
 *                       die Luft
 *   2. Puffer leeren,   jetzt ist man im Kommandomodus und darf
 *      Hostmode raus    schreiben, ohne zu senden
 *   3. ESC V           Probe - bestaetigt den Kommandomodus
 *   4. KISS betreten    ESC @K bzw. "kiss on\r" je nach Profil
 *
 * Rueckgabe 0 wenn alles in Ordnung, sonst Anzahl der Fehler.
 */
int pr_checkup(const pr_config *cfg, pr_selftest *out);

/* Ausgabe fuer die Kommandozeile. */
void pr_selftest_print(const pr_selftest *st, FILE *f);

/* Kurzform als eine Zeile. */
const char *pr_test_status_name(int status);

#endif /* PRTERM_SELFTEST_H */
