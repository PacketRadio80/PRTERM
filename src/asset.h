/*
 * PRTERM - CB & Amateur Radio Terminal
 * asset.h - embedded static assets (CSS/JS) as byte arrays.
 *
 * The arrays are generated at build time by tools/embed.c from the
 * sources in web/, so the CGI stays a single self-contained binary:
 * copy prterm.cgi + prterm.ini and it works.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PRTERM_ASSET_H
#define PRTERM_ASSET_H

/* web/prterm.css - NUL terminated, len excludes the terminator. */
extern const unsigned char prterm_css[];
extern const unsigned long prterm_css_len;

/* web/prterm.js  - NUL terminated, len excludes the terminator. */
extern const unsigned char prterm_js[];
extern const unsigned long prterm_js_len;

#endif /* PRTERM_ASSET_H */
