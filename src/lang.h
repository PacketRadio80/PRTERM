/*
 * PRTERM - CB & Amateur Radio Terminal
 * lang.h - UI languages.
 *
 * The interface speaks the big five: English, German, Spanish,
 * Portuguese and French. English is the SOURCE language - the English
 * text is the key of every entry and doubles as the fallback. A text
 * without a translation simply stays English, so nothing breaks when
 * the catalog grows.
 *
 * Only what the operator sees is translated: labels, headings,
 * buttons, hints, notes and the messages of the browser script. CLI
 * output and log files stay English - they are for the operator to
 * read and to grep.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PRTERM_LANG_H
#define PRTERM_LANG_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Translation of one text into the given language.
 *
 *   lang   language code from [site] language: en, de, es, pt, fr
 *   text   the English text - also the key and the fallback
 *
 * Returns the translation, or the text itself when the language is
 * unknown or the entry is missing. Never NULL for a non-NULL text.
 */
const char *pr_tr(const char *lang, const char *text);

/*
 * The same, for layers that have no configuration at hand (band plan,
 * drivers): pr_lang_set() is called once per request with the
 * language of the installation, pr_trs()/pr_trf() then use it.
 * pr_trf() is a snprintf with a TRANSLATED format string - for
 * messages that carry numbers and names.
 */
void pr_lang_set(const char *lang);
const char *pr_trs(const char *text);
int  pr_trf(char *dst, size_t dstlen, const char *fmt, ...);

/* Is this language code shipped? ("en", "DE", "Es", ... case-insensitive) */
bool pr_lang_supported(const char *lang);

/* The shipped languages, for the selection in the administration. */
size_t      pr_lang_count(void);
const char *pr_lang_code(size_t idx);   /* "en", "de", "es", "pt", "fr"   */
const char *pr_lang_name(size_t idx);   /* endonym: "Deutsch", "Espanol"  */

#endif /* PRTERM_LANG_H */
