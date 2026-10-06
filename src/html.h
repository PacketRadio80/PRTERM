/*
 * PRTERM - CB & Amateur Radio Terminal
 * html.h - Layout, embedded assets, form building blocks.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PRTERM_HTML_H
#define PRTERM_HTML_H

#include "cgi.h"
#include "config.h"
#include "session.h"

#include <stdbool.h>
#include <stddef.h>

/* Embedded assets (generated from web/ at build time).          */
const char *html_css(void);
const char *html_js(void);

/* Opens the document: <!doctype> up to <body> including @font-face
 * and the dynamic CSS variables from [ui]. */
void html_open(pr_buf *out, const pr_config *cfg, const pr_session *sess,
               const char *title);
void html_close(pr_buf *out, const pr_config *cfg);

/* ---- Building blocks -------------------------------------------------------------- */
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

/* ---- Font file    ----------------------------------------------------- */
/* Checks the extension and returns the MIME type. False if not allowed.  */
bool html_font_mime(const char *path, char *mime, size_t mimelen);

/* ---- Miscellaneous ---------------------------------------------------- */
void html_note(pr_buf *out, const char *kind, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

#endif /* PRTERM_HTML_H */
