/*
 * PRTERM - CB & Amateur Radio Terminal
 * ini.c - INI parser/writer, comment- and order-preserving.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "prterm_compat.h"

#include "ini.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct ini_entry {
    char *section;
    char *key;
    char *value;
    long  line;        /* Index into lines[], -1 = new entry   */
    bool  dirty;       /* changed via ini_set -> reformat the line       */
} ini_entry;

struct ini {
    ini_entry *ents;
    size_t     n;
    size_t     cap;

    char     **lines;  /* raw lines; NULL = deleted     */
    size_t     nlines;
    size_t     caplines;
};

/* ======================================================================= */
/* internal helpers                                                          */
/* ======================================================================= */

static bool sec_eq(const char *a, const char *b)
{
    return pr_str_eq_ci(a != NULL ? a : "", b != NULL ? b : "");
}

static size_t ini_find(const ini *i, const char *section, const char *key)
{
    for (size_t k = 0; k < i->n; k++) {
        if (sec_eq(i->ents[k].section, section) &&
            pr_str_eq_ci(i->ents[k].key, key))
            return k;
    }
    return (size_t)-1;
}

static void entry_free(ini_entry *e)
{
    free(e->section);
    free(e->key);
    free(e->value);
    e->section = e->key = e->value = NULL;
}

static bool ents_reserve(ini *i, size_t need)
{
    if (i->n + need <= i->cap)
        return true;
    size_t ncap = i->cap != 0 ? i->cap : 16;
    while (ncap < i->n + need)
        ncap *= 2;
    ini_entry *p = realloc(i->ents, ncap * sizeof *p);
    if (p == NULL)
        return false;
    i->ents = p;
    i->cap = ncap;
    return true;
}

static bool lines_push(ini *i, char *line)
{
    if (i->nlines + 1 > i->caplines) {
        size_t ncap = i->caplines != 0 ? i->caplines * 2 : 32;
        char **p = realloc(i->lines, ncap * sizeof *p);
        if (p == NULL)
            return false;
        i->lines = p;
        i->caplines = ncap;
    }
    i->lines[i->nlines++] = line;
    return true;
}

static bool add_entry(ini *i, const char *section, const char *key,
                      const char *value, long line)
{
    if (!ents_reserve(i, 1))
        return false;
    ini_entry *e = &i->ents[i->n];
    e->section = pr_strdup(section != NULL ? section : "");
    e->key     = pr_strdup(key);
    e->value   = pr_strdup(value);
    e->line    = line;
    e->dirty   = false;
    if (e->section == NULL || e->key == NULL || e->value == NULL) {
        entry_free(e);
        return false;
    }
    i->n++;
    return true;
}

/* Removes a trailing inline comment ( ; or # ), provided it is not
 * inside quotes and is preceded by whitespace. */
static void strip_inline_comment(char *s)
{
    bool in_dq = false, in_sq = false;
    for (size_t k = 0; s[k] != '\0'; k++) {
        char c = s[k];
        if (c == '"' && !in_sq) {
            in_dq = !in_dq;
        } else if (c == '\'' && !in_dq) {
            in_sq = !in_sq;
        } else if (!in_dq && !in_sq && (c == ';' || c == '#')) {
            if (k == 0 || s[k - 1] == ' ' || s[k - 1] == '\t') {
                s[k] = '\0';
                return;
            }
        }
    }
}

static void unquote(char *s)
{
    size_t n = strlen(s);
    if (n >= 2 && ((s[0] == '"' && s[n - 1] == '"') ||
                   (s[0] == '\'' && s[n - 1] == '\''))) {
        memmove(s, s + 1, n - 2);
        s[n - 2] = '\0';
    }
}

/* Splits "<key> = <value>"; true on success.  */
static bool split_kv(char *line, char **key, char **val)
{
    char *p = line;
    while (*p == ' ' || *p == '\t')
        p++;

    char *eq = NULL;
    bool in_dq = false, in_sq = false;
    for (char *q = p; *q != '\0'; q++) {
        if (*q == '"' && !in_sq)      in_dq = !in_dq;
        else if (*q == '\'' && !in_dq) in_sq = !in_sq;
        else if (!in_dq && !in_sq && (*q == '=' || *q == ':')) { eq = q; break; }
    }
    if (eq == NULL)
        return false;

    *eq = '\0';
    char *k = p;
    char *e = eq - 1;
    while (e >= k && (*e == ' ' || *e == '\t')) { *e = '\0'; e--; }

    char *v = eq + 1;
    while (*v == ' ' || *v == '\t')
        v++;
    pr_trim(v);
    strip_inline_comment(v);
    pr_trim(v);
    unquote(v);

    if (*k == '\0')
        return false;

    *key = k;
    *val = v;
    return true;
}

/* ======================================================================= */
/* Lifecycle                                                               */
/* ======================================================================= */

ini *ini_new(void)
{
    ini *i = calloc(1, sizeof *i);
    return i;
}

ini *ini_parse(const char *text, char *err, size_t errlen)
{
    ini *i = ini_new();
    if (i == NULL) {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }

    char cursec[128] = "";
    const char *p = text;
    long lineno = 0;

    while (*p != '\0') {
        const char *nl = strchr(p, '\n');
        size_t len = (nl != NULL) ? (size_t)(nl - p) : strlen(p);

        char *raw = malloc(len + 1);
        if (raw == NULL) {
            snprintf(err, errlen, "out of memory (line %ld)", lineno + 1);
            ini_free(i);
            return NULL;
        }
        memcpy(raw, p, len);
        raw[len] = '\0';
        if (len > 0 && raw[len - 1] == '\r')
            raw[len - 1] = '\0';

        if (!lines_push(i, raw)) {
            free(raw);
            snprintf(err, errlen, "out of memory (line %ld)", lineno + 1);
            ini_free(i);
            return NULL;
        }

        /* Parsing happens on a COPY. raw must stay unchanged: exactly this
         * line is written out again later so that comments, indentation
         * and spelling survive. */
        char *work = pr_strdup(raw);
        if (work == NULL) {
            snprintf(err, errlen, "out of memory (line %ld)", lineno + 1);
            ini_free(i);
            return NULL;
        }
        pr_trim(work);

        if (*work == '\0' || *work == ';' || *work == '#') {
            /* Comment / blank line - leave unchanged      */
        } else if (*work == '[') {
            char *end = strchr(work, ']');
            if (end != NULL) {
                *end = '\0';
                pr_strlcpy(cursec, work + 1, sizeof cursec);
                pr_trim(cursec);
            }
        } else {
            char *k = NULL, *v = NULL;
            if (split_kv(work, &k, &v)) {
                if (!add_entry(i, cursec, k, v, lineno)) {
                    free(work);
                    snprintf(err, errlen, "out of memory (line %ld)", lineno + 1);
                    ini_free(i);
                    return NULL;
                }
            }
        }
        free(work);

        lineno++;
        if (nl == NULL)
            break;
        p = nl + 1;
    }

    return i;
}

ini *ini_load(const char *path, char *err, size_t errlen)
{
    char *text = NULL;
    size_t len = 0;
    if (pr_read_file(path, &text, &len, err, errlen) != 0)
        return NULL;

    ini *i = ini_parse(text, err, errlen);
    free(text);
    return i;
}

void ini_free(ini *i)
{
    if (i == NULL)
        return;
    for (size_t k = 0; k < i->n; k++)
        entry_free(&i->ents[k]);
    free(i->ents);
    for (size_t k = 0; k < i->nlines; k++)
        free(i->lines[k]);
    free(i->lines);
    free(i);
}

/* ======================================================================= */
/* reading                                                                   */
/* ======================================================================= */

const char *ini_get(const ini *i, const char *section, const char *key,
                    const char *dflt)
{
    if (i == NULL)
        return dflt;
    size_t idx = ini_find(i, section, key);
    return idx == (size_t)-1 ? dflt : i->ents[idx].value;
}

long ini_get_int(const ini *i, const char *section, const char *key, long dflt)
{
    const char *v = ini_get(i, section, key, NULL);
    if (v == NULL)
        return dflt;
    bool ok = false;
    long n = pr_parse_long(v, dflt, &ok);
    return ok ? n : dflt;
}

bool ini_get_bool(const ini *i, const char *section, const char *key, bool dflt)
{
    const char *v = ini_get(i, section, key, NULL);
    if (v == NULL)
        return dflt;
    return pr_parse_bool(v, dflt);
}

bool ini_has_section(const ini *i, const char *section)
{
    if (i == NULL)
        return false;
    for (size_t k = 0; k < i->n; k++)
        if (sec_eq(i->ents[k].section, section))
            return true;
    return false;
}

size_t ini_section_count(const ini *i, const char *section)
{
    size_t c = 0;
    if (i == NULL)
        return 0;
    for (size_t k = 0; k < i->n; k++)
        if (sec_eq(i->ents[k].section, section))
            c++;
    return c;
}

/* ======================================================================= */
/* writing                                                                  */
/* ======================================================================= */

void ini_set(ini *i, const char *section, const char *key, const char *value)
{
    if (i == NULL || key == NULL)
        return;
    const char *sec = section != NULL ? section : "";
    const char *val = value != NULL ? value : "";

    size_t idx = ini_find(i, sec, key);
    if (idx != (size_t)-1) {
        char *nv = pr_strdup(val);
        if (nv == NULL)
            return;
        free(i->ents[idx].value);
        i->ents[idx].value = nv;
        i->ents[idx].dirty = true;
        return;
    }
    (void)add_entry(i, sec, key, val, -1);
}

void ini_set_int(ini *i, const char *section, const char *key, long value)
{
    char buf[32];
    snprintf(buf, sizeof buf, "%ld", value);
    ini_set(i, section, key, buf);
}

void ini_set_bool(ini *i, const char *section, const char *key, bool value)
{
    ini_set(i, section, key, value ? "true" : "false");
}

bool ini_del(ini *i, const char *section, const char *key)
{
    if (i == NULL)
        return false;
    size_t idx = ini_find(i, section, key);
    if (idx == (size_t)-1)
        return false;

    long ln = i->ents[idx].line;
    entry_free(&i->ents[idx]);
    memmove(&i->ents[idx], &i->ents[idx + 1],
            (i->n - idx - 1) * sizeof *i->ents);
    i->n--;

    if (ln >= 0 && (size_t)ln < i->nlines) {
        free(i->lines[ln]);
        i->lines[ln] = NULL;   /* skipped in the output              */
    }
    return true;
}

size_t ini_count(const ini *i)
{
    return i != NULL ? i->n : 0;
}

const char *ini_section_at(const ini *i, size_t idx)
{
    return (i != NULL && idx < i->n) ? i->ents[idx].section : NULL;
}

const char *ini_key_at(const ini *i, size_t idx)
{
    return (i != NULL && idx < i->n) ? i->ents[idx].key : NULL;
}

const char *ini_value_at(const ini *i, size_t idx)
{
    return (i != NULL && idx < i->n) ? i->ents[idx].value : NULL;
}

void ini_foreach(const ini *i, const char *section, ini_iter_fn fn, void *ud)
{
    if (i == NULL || fn == NULL)
        return;
    for (size_t k = 0; k < i->n; k++) {
        if (section != NULL && !sec_eq(i->ents[k].section, section))
            continue;
        if (!fn(ud, i->ents[k].section, i->ents[k].key, i->ents[k].value))
            return;
    }
}

/* ======================================================================= */
/* Serialization - comments are preserved                                  */
/* ======================================================================= */

static void emit_pending(pr_buf *out, const ini *i, unsigned char *done,
                         const char *section)
{
    for (size_t k = 0; k < i->n; k++) {
        if (done[k])
            continue;
        if (!sec_eq(i->ents[k].section, section))
            continue;
        pr_buf_addf(out, "%s = %s\n", i->ents[k].key, i->ents[k].value);
        done[k] = 1;
    }
}

static bool line_is_section(const char *line, char *sec, size_t seclen)
{
    const char *p = line;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p != '[')
        return false;
    const char *end = strchr(p, ']');
    if (end == NULL)
        return false;
    size_t n = (size_t)(end - p - 1);
    if (n >= seclen)
        n = seclen - 1;
    memcpy(sec, p + 1, n);
    sec[n] = '\0';
    pr_trim(sec);
    return true;
}

char *ini_dump(const ini *i)
{
    if (i == NULL)
        return pr_strdup("");

    pr_buf out;
    pr_buf_init(&out);

    unsigned char *done = calloc(i->n != 0 ? i->n : 1, 1);
    if (done == NULL) {
        pr_buf_free(&out);
        return NULL;
    }

    char cursec[128] = "";
    char secbuf[128];

    for (size_t ln = 0; ln < i->nlines; ln++) {
        const char *raw = i->lines[ln];
        if (raw == NULL)
            continue;                       /* deleted line     */

        /* Before a new section starts: emit new keys of the old one           */
        if (line_is_section(raw, secbuf, sizeof secbuf)) {
            emit_pending(&out, i, done, cursec);
            pr_strlcpy(cursec, secbuf, sizeof cursec);
            pr_buf_add(&out, raw);
            pr_buf_addc(&out, '\n');
            continue;
        }

        /* Find the entry for this line and mark it. Changed entries are
         * reformatted, unchanged ones are emitted as the original line
         * - this preserves comments and spelling. */
        bool replaced = false;
        for (size_t k = 0; k < i->n; k++) {
            if (i->ents[k].line != (long)ln || done[k])
                continue;
            if (i->ents[k].dirty)
                pr_buf_addf(&out, "%s = %s\n", i->ents[k].key, i->ents[k].value);
            else
                pr_buf_add(&out, raw);
            pr_buf_addc(&out, '\n');
            done[k] = 1;
            replaced = true;
            break;
        }
        if (!replaced) {
            pr_buf_add(&out, raw);
            pr_buf_addc(&out, '\n');
        }
    }

    /* Rest: sections that never appeared in the text */
    emit_pending(&out, i, done, cursec);
    for (size_t k = 0; k < i->n; k++) {
        if (done[k])
            continue;
        pr_buf_addf(&out, "\n[%s]\n%s = %s\n",
                    i->ents[k].section, i->ents[k].key, i->ents[k].value);
        done[k] = 1;
    }

    free(done);
    return pr_buf_take(&out);
}

int ini_save(const ini *i, const char *path, char *err, size_t errlen)
{
    char *text = ini_dump(i);
    if (text == NULL) {
        snprintf(err, errlen, "out of memory");
        return -1;
    }
    int rc = pr_write_file_atomic(path, text, strlen(text), err, errlen);
    free(text);
    return rc;
}
