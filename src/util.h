/*
 * PRTERM - CB & Amateur Radio Terminal
 * util.h - portable helpers (C11 + POSIX.1-2008/XSI, no GNU extensions).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PRTERM_UTIL_H
#define PRTERM_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/* ---- bounded string helpers (strlcpy/strlcat are not in glibc) --------- */
void  pr_strlcpy(char *dst, const char *src, size_t dstsize);
void  pr_strlcat(char *dst, const char *src, size_t dstsize);
char *pr_strdup(const char *s);

void  pr_trim(char *s);
void  pr_upper(char *s);
void  pr_lower(char *s);
bool  pr_str_eq_ci(const char *a, const char *b);
bool  pr_starts_with(const char *s, const char *prefix);
bool  pr_ends_with(const char *s, const char *suffix);

/* ---- dynamic string buffer -------------------------------------------- */
typedef struct pr_buf {
    char  *data;   /* always NUL terminated while len >= 0 */
    size_t len;
    size_t cap;
    bool   oom;
} pr_buf;

void  pr_buf_init(pr_buf *b);
void  pr_buf_free(pr_buf *b);
void  pr_buf_reset(pr_buf *b);
bool  pr_buf_ok(const pr_buf *b);

void  pr_buf_addc(pr_buf *b, char c);
void  pr_buf_addn(pr_buf *b, const char *s, size_t n);
void  pr_buf_add(pr_buf *b, const char *s);
void  pr_buf_addf(pr_buf *b, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

/* hand ownership of the string to the caller; the buffer is reset */
char *pr_buf_take(pr_buf *b);

/* ---- escaping --------------------------------------------------------- */
void  pr_url_decode(char *s);
void  pr_url_encode(pr_buf *out, const char *s);
void  pr_html_escape(pr_buf *out, const char *s);
void  pr_attr_escape(pr_buf *out, const char *s);
/* JSON string body, quotes and control chars escaped, NOT wrapped in " */
void  pr_json_escape(pr_buf *out, const char *s);

/* ---- files ------------------------------------------------------------ */
bool  pr_file_exists(const char *path);
int   pr_read_file(const char *path, char **out, size_t *len,
                   char *err, size_t errlen);
int   pr_write_file(const char *path, const char *data, size_t len,
                    char *err, size_t errlen);
int   pr_write_file_atomic(const char *path, const char *data, size_t len,
                           char *err, size_t errlen);
int   pr_mkdirs(const char *path, char *err, size_t errlen);

/* POSIX fcntl() lock; returns fd (>= 0) or -1. Always unlock with pr_unlock. */
int   pr_lock_file(const char *path, char *err, size_t errlen);
void  pr_unlock_file(int fd);

/* ---- misc ------------------------------------------------------------- */
void      pr_random_hex(char *dst, size_t dstlen); /* dst must hold 2*n+1 */
long long pr_now_ms(void);
long long pr_now_s(void);
bool      pr_parse_bool(const char *s, bool dflt);
long      pr_parse_long(const char *s, long dflt, bool *ok);
/* wildcard: '*' any run, '?' exactly one char (case sensitive) */
bool      pr_wildcard_match(const char *pattern, const char *text);
/* sha256$<salt>$<hash> style splitting, used by session + config */
bool      pr_split3(const char *s, char sep, char *a, size_t alen,
                    char *b, size_t blen, char *c, size_t clen);

#endif /* PRTERM_UTIL_H */
