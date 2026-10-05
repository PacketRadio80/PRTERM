/*
 * PRTERM - CB & Amateur Radio Terminal
 * locator.h - Maidenhead-Gitterfeld (QTH-Locator).
 *
 * Der Locator ist im Amateur- und CB-Funk die gaengige Positionsangabe.
 * Er wird aus den geografischen Koordinaten berechnet:
 *
 *   JN49VL
 *   |||||+- Subsquare  (24 x 24, ~2,5 x 1,2 km)
 *   ||||+- Subsquare
 *   |||+- Quadrat zweite Ziffer (10 x 10, ~5 x 2,5 km)
 *   ||+- Quadrat erste Ziffer
 *   |+- Feld zweiter Buchstabe (10 Grad)
 *   +- Feld erster Buchstabe (20 Grad)
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PRTERM_LOCATOR_H
#define PRTERM_LOCATOR_H

#include <stdbool.h>
#include <stddef.h>

/* Quadrat (4 Zeichen, z.B. "JN49") und Subsquare (6 Zeichen, "JN49VL"). */
bool pr_locator4(double lat, double lon, char out[5]);
bool pr_locator6(double lat, double lon, char out[7]);

/* Umgekehrt: Mittelpunkt eines Quadrats bestimmen. */
bool pr_locator_center(const char *loc, double *lat, double *lon);

/* Prueft die Form, nicht die Lage. */
bool pr_locator_valid(const char *loc);

#endif /* PRTERM_LOCATOR_H */
