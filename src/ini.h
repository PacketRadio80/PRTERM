/*
 * PRTERM - CB & Amateur Radio Terminal
 * ini.h - INI parser/writer.
 *
 * The property that matters for PRTERM: saving preserves comments,
 * order and indentation of prterm.ini. The file is the only config
 * and is edited both from the admin pages and from the shell - a
 * rewrite must not delete the documentation.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
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

/* ---- Access  --------------------------------------------------------- */
/* Section "" is the area before the first [header].
 * Key and section comparison are case-insensitive, the spelling of
 * the original is preserved. */
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

/* ---- Output  --------------------------------------------------------- */
int   ini_save(const ini *i, const char *path, char *err, size_t errlen);
/* Serializes with comments preserved; caller frees.            */
char *ini_dump(const ini *i);

#endif /* PRTERM_INI_H */
