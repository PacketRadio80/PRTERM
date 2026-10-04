/*
 * PRTERM - CB & Amateur Radio Terminal
 * html.h - Layout, eingebettete Assets, Formularbausteine.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PRTERM_HTML_H
#define PRTERM_HTML_H

#include "cgi.h"
#include "config.h"
#include "session.h"

#include <stdbool.h>
#include <stddef.h>

/* Eingebettete Assets (werden zur Build-Zeit aus web/ erzeugt). */
const char *html_css(void);
const char *html_js(void);

/* Oeffnet das Dokument: <!doctype> bis <body> einschliesslich @font-face
 * und der dynamischen CSS-Variablen aus [ui]. */
void html_open(pr_buf *out, const pr_config *cfg, const pr_session *sess,
               const char *title);
void html_close(pr_buf *out, const pr_config *cfg);

/* ---- Bausteine -------------------------------------------------------- */
void html_input_text(pr_buf *out, const char *name, const char *value,
                     const char *placeholder, const char *label,
                     const char *hint);
void html_input_hidden(pr_buf *out, const char *name, const char *value);
void html_input_number(pr_buf *out, const char *name, long value,
                       long lo, long hi, const char *label, const char *hint);
void html_select(pr_buf *out, const char *name,
                 const char *const *values, const char *const *labels,
                 size_t n, const char *selected,
                 const char *label, const char *hint);
void html_checkbox(pr_buf *out, const char *name, bool checked,
                   const char *label, const char *hint);
void html_csrf(pr_buf *out, const pr_session *sess);

/* ---- Schriftdatei ----------------------------------------------------- */
/* Prueft die Endung und liefert den MIME-Typ. false bei nicht zulaessig. */
bool html_font_mime(const char *path, char *mime, size_t mimelen);

/* ---- Kleinigkeiten ---------------------------------------------------- */
void html_note(pr_buf *out, const char *kind, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

#endif /* PRTERM_HTML_H */
