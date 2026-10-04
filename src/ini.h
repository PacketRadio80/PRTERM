/*
 * PRTERM - CB & Amateur Radio Terminal
 * ini.h - INI parser/writer.
 *
 * Eigenschaft, die fuer PRTERM wichtig ist: beim Speichern bleiben
 * Kommentare, Reihenfolge und Einrueckung der prterm.ini erhalten. Die Datei
 * ist die einzige Konfiguration und wird sowohl vom Admin-Bereich als auch
 * von der Shell editiert - ein Neuschreiben duerfte die Doku nicht loeschen.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PRTERM_INI_H
#define PRTERM_INI_H

#include <stdbool.h>
#include <stddef.h>

typedef struct ini ini;

ini  *ini_new(void);
ini  *ini_load(const char *path, char *err, size_t errlen);
ini  *ini_parse(const char *text, char *err, size_t errlen);
void  ini_free(ini *i);

/* ---- Zugriff --------------------------------------------------------- */
/* Sektion "" ist der Bereich vor dem ersten [header].
 * Schluessel- und Sektionsvergleich sind case-insensitiv, die Schreibweise
 * des Originals bleibt erhalten. */
const char *ini_get(const ini *i, const char *section, const char *key,
                    const char *dflt);
long        ini_get_int(const ini *i, const char *section, const char *key,
                        long dflt);
bool        ini_get_bool(const ini *i, const char *section, const char *key,
                         bool dflt);

void ini_set(ini *i, const char *section, const char *key, const char *value);
void ini_set_int(ini *i, const char *section, const char *key, long value);
void ini_set_bool(ini *i, const char *section, const char *key, bool value);
bool ini_del(ini *i, const char *section, const char *key);

/* ---- Iteration ------------------------------------------------------- */
size_t      ini_count(const ini *i);
const char *ini_section_at(const ini *i, size_t idx);
const char *ini_key_at(const ini *i, size_t idx);
const char *ini_value_at(const ini *i, size_t idx);

typedef bool (*ini_iter_fn)(void *ud, const char *section,
                            const char *key, const char *value);
void ini_foreach(const ini *i, const char *section, ini_iter_fn fn, void *ud);

bool   ini_has_section(const ini *i, const char *section);
size_t ini_section_count(const ini *i, const char *section);

/* ---- Ausgabe --------------------------------------------------------- */
int   ini_save(const ini *i, const char *path, char *err, size_t errlen);
/* Serialisiert mit erhaltenen Kommentaren; Aufrufer gibt frei. */
char *ini_dump(const ini *i);

#endif /* PRTERM_INI_H */
