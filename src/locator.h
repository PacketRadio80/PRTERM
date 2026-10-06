/*
 * PRTERM - CB & Amateur Radio Terminal
 * locator.h - Maidenhead grid square (QTH locator).
 *
 * The locator is the common position format in amateur and CB radio.
 * It is computed from the geographic coordinates:
 *
 *   JN49VL
 *   |||||+- Subsquare  (24 x 24, ~2.5 x 1.2 km)
 *   ||||+- Subsquare
 *   |||+- Square second digit (10 x 10, ~5 x 2.5 km)
 *   ||+- Square first digit
 *   |+- Field second letter (10 degrees)
 *   +- Field first letter (20 degrees)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PRTERM_LOCATOR_H
#define PRTERM_LOCATOR_H

#include <stdbool.h>
#include <stddef.h>

/* Square (4 chars, e.g. "JN49") and subsquare (6 chars, "JN49VL").      */
bool pr_locator4(double lat, double lon, char out[5]);
bool pr_locator6(double lat, double lon, char out[7]);

/* Reverse: determine the center of a square.       */
bool pr_locator_center(const char *loc, double *lat, double *lon);

/* Checks the shape, not the position. */
bool pr_locator_valid(const char *loc);

#endif /* PRTERM_LOCATOR_H */
