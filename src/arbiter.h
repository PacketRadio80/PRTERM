/*
 * PRTERM - CB & Amateur Radio Terminal
 * arbiter.h - Senderegelung bei mehreren TNCs.
 *
 * Wenn mehrere Geraete am selben Kanal haengen, darf IMMER nur eines
 * senden. Gehen zwei gleichzeitig auf Sendung, zerstoeren sich beide
 * Signale - auf demselben Kanal gibt es keine zweite Moeglichkeit.
 *
 * Diese Schicht ist eine harte Regel, keine Empfehlung: der Sendeweg
 * geht durch sie hindurch oder gar nicht.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PRTERM_ARBITER_H
#define PRTERM_ARBITER_H

#include <stdbool.h>
#include <stddef.h>

#define PR_ARBITER_OWNER_LEN 64

/*
 * Haelt die Sendesperre fuer einen Kanal.
 *
 * Vorgehen:
 *   fd = pr_arbiter_acquire(...);   blockiert bis frei
 *   ... senden ...
 *   pr_arbiter_release(fd);
 *
 * Die Sperre ist eine Datei im runtime_dir - sie wirkt damit auch
 * ueber mehrere CGI-Prozesse hinweg.
 */
int  pr_arbiter_acquire(const char *runtime_dir, long freq_hz,
                        const char *owner, int timeout_ms,
                        char *err, size_t errlen);
void pr_arbiter_release(int fd);

/* Fragt ab, ob gerade gesendet wird. true wenn belegt. */
bool pr_arbiter_busy(const char *runtime_dir, long freq_hz,
                     char *owner, size_t ownerlen);

/* Zeigt den Pfad der Sperre an (fuer Anzeige und Diagnose). */
void pr_arbiter_path(const char *runtime_dir, long freq_hz,
                     char *dst, size_t dstlen);

#endif /* PRTERM_ARBITER_H */
