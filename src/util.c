/*
 * PRTERM - CB & Amateur Radio Terminal
 * util.c - portable helpers (C11 + POSIX.1-2008/XSI).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "prterm_compat.h"

#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* ======================================================================= */
/* bounded string helpers                                                  */
/* ======================================================================= */

void pr_strlcpy(char *dst, const char *src, size_t dstsize)
{
    if (dst == NULL || dstsize == 0)
        return;
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    size_t n = strlen(src);
    if (n >= dstsize) {
        memcpy(dst, src, dstsize - 1);
        dst[dstsize - 1] = '\0';
    } else {
        memcpy(dst, src, n + 1);
    }
}

void pr_strlcat(char *dst, const char *src, size_t dstsize)
{
    if (dst == NULL || dstsize == 0)
        return;
    size_t dlen = strlen(dst);
    if (dlen >= dstsize - 1)
        return;
    pr_strlcpy(dst + dlen, src != NULL ? src : "", dstsize - dlen);
}

char *pr_strdup(const char *s)
{
    if (s == NULL)
        return NULL;
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p == NULL)
        return NULL;
    memcpy(p, s, n);
    return p;
}

void pr_trim(char *s)
{
    if (s == NULL)
        return;
    char *start = s;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n')
        start++;
    size_t n = strlen(start);
    while (n > 0 && (start[n - 1] == ' ' || start[n - 1] == '\t' ||
                     start[n - 1] == '\r' || start[n - 1] == '\n'))
        n--;
    if (start != s)
        memmove(s, start, n);
    s[n] = '\0';
}

void pr_upper(char *s)
{
    if (s == NULL)
        return;
    for (; *s != '\0'; s++)
        if (*s >= 'a' && *s <= 'z')
            *s = (char)(*s - 'a' + 'A');
}

void pr_lower(char *s)
{
    if (s == NULL)
        return;
    for (; *s != '\0'; s++)
        if (*s >= 'A' && *s <= 'Z')
            *s = (char)(*s - 'A' + 'a');
}

bool pr_str_eq_ci(const char *a, const char *b)
{
    if (a == NULL || b == NULL)
        return a == b;
    while (*a != '\0' && *b != '\0') {
        int ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb)
            return false;
        a++;
        b++;
    }
    return *a == *b;
}

bool pr_starts_with(const char *s, const char *prefix)
{
    if (s == NULL || prefix == NULL)
        return false;
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

bool pr_ends_with(const char *s, const char *suffix)
{
    if (s == NULL || suffix == NULL)
        return false;
    size_t sl = strlen(s), pl = strlen(suffix);
    if (pl > sl)
        return false;
    return memcmp(s + (sl - pl), suffix, pl) == 0;
}

/* ======================================================================= */
/* dynamic buffer                                                          */
/* ======================================================================= */

void pr_buf_init(pr_buf *b)
{
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    b->oom = false;
}

void pr_buf_free(pr_buf *b)
{
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
    b->oom = false;
}

void pr_buf_reset(pr_buf *b)
{
    b->len = 0;
    b->oom = false;
    if (b->data != NULL)
        b->data[0] = '\0';
}

bool pr_buf_ok(const pr_buf *b)
{
    return !b->oom;
}

static bool pr_buf_reserve(pr_buf *b, size_t need)
{
    if (b->oom)
        return false;
    if (b->len + need + 1 <= b->cap)
        return true;

    size_t ncap = b->cap != 0 ? b->cap : 128;
    while (ncap < b->len + need + 1) {
        if (ncap > (size_t)-1 / 2) {
            b->oom = true;
            return false;
        }
        ncap *= 2;
    }
    char *p = realloc(b->data, ncap);
    if (p == NULL) {
        b->oom = true;
        return false;
    }
    b->data = p;
    b->cap = ncap;
    return true;
}

void pr_buf_addc(pr_buf *b, char c)
{
    if (!pr_buf_reserve(b, 1))
        return;
    b->data[b->len++] = c;
    b->data[b->len] = '\0';
}

void pr_buf_addn(pr_buf *b, const char *s, size_t n)
{
    if (s == NULL || n == 0)
        return;
    if (!pr_buf_reserve(b, n))
        return;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

void pr_buf_add(pr_buf *b, const char *s)
{
    if (s != NULL)
        pr_buf_addn(b, s, strlen(s));
}

void pr_buf_addf(pr_buf *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) {
        va_end(ap2);
        b->oom = true;
        return;
    }
    if (!pr_buf_reserve(b, (size_t)n)) {
        va_end(ap2);
        return;
    }
    vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    b->len += (size_t)n;
}

char *pr_buf_take(pr_buf *b)
{
    char *p = b->data;
    if (p == NULL) {
        p = malloc(1);
        if (p != NULL)
            p[0] = '\0';
    }
    b->data = NULL;
    b->len = b->cap = 0;
    b->oom = false;
    return p;
}

/* ======================================================================= */
/* escaping                                                                */
/* ======================================================================= */

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void pr_url_decode(char *s)
{
    if (s == NULL)
        return;
    char *r = s, *w = s;
    while (*r != '\0') {
        if (*r == '+') {
            *w++ = ' ';
            r++;
        } else if (*r == '%' && r[1] != '\0' && r[2] != '\0') {
            int h = hexval(r[1]), l = hexval(r[2]);
            if (h >= 0 && l >= 0) {
                *w++ = (char)((h << 4) | l);
                r += 3;
            } else {
                *w++ = *r++;
            }
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

static void pct_encode(pr_buf *out, const char *s, bool form)
{
    if (s == NULL)
        return;
    for (; *s != '\0'; s++) {
        unsigned char c = (unsigned char)*s;
        bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                    c == '.' || c == '~';
        if (form && c == ' ') {
            pr_buf_addc(out, '+');
        } else if (safe) {
            pr_buf_addc(out, (char)c);
        } else {
            pr_buf_addf(out, "%%%02X", c);
        }
    }
}

void pr_url_encode(pr_buf *out, const char *s)
{
    pct_encode(out, s, true);
}

void pr_html_escape(pr_buf *out, const char *s)
{
    if (s == NULL)
        return;
    for (; *s != '\0'; s++) {
        switch (*s) {
        case '&':  pr_buf_add(out, "&amp;");  break;
        case '<':  pr_buf_add(out, "&lt;");   break;
        case '>':  pr_buf_add(out, "&gt;");   break;
        case '"':  pr_buf_add(out, "&quot;"); break;
        case '\'': pr_buf_add(out, "&#39;");  break;
        default:   pr_buf_addc(out, *s);      break;
        }
    }
}

void pr_attr_escape(pr_buf *out, const char *s)
{
    /* same set as html, plus backticks and newlines become harmless */
    if (s == NULL)
        return;
    for (; *s != '\0'; s++) {
        switch (*s) {
        case '&':  pr_buf_add(out, "&amp;");  break;
        case '<':  pr_buf_add(out, "&lt;");   break;
        case '>':  pr_buf_add(out, "&gt;");   break;
        case '"':  pr_buf_add(out, "&quot;"); break;
        case '\'': pr_buf_add(out, "&#39;");  break;
        case '`':  pr_buf_add(out, "&#96;");  break;
        case '\n': pr_buf_add(out, "&#10;");  break;
        case '\r': break;
        default:   pr_buf_addc(out, *s);      break;
        }
    }
}

void pr_json_escape(pr_buf *out, const char *s)
{
    if (s == NULL)
        return;
    for (; *s != '\0'; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
        case '"':  pr_buf_add(out, "\\\""); break;
        case '\\': pr_buf_add(out, "\\\\"); break;
        case '\b': pr_buf_add(out, "\\b");  break;
        case '\f': pr_buf_add(out, "\\f");  break;
        case '\n': pr_buf_add(out, "\\n");  break;
        case '\r': pr_buf_add(out, "\\r");  break;
        case '\t': pr_buf_add(out, "\\t");  break;
        default:
            if (c < 0x20)
                pr_buf_addf(out, "\\u%04x", c);
            else
                pr_buf_addc(out, (char)c);
            break;
        }
    }
}

/* ======================================================================= */
/* files                                                                   */
/* ======================================================================= */

bool pr_file_exists(const char *path)
{
    struct stat st;
    return path != NULL && stat(path, &st) == 0;
}

int pr_read_file(const char *path, char **out, size_t *len, char *err, size_t errlen)
{
    *out = NULL;
    if (len != NULL)
        *len = 0;

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        snprintf(err, errlen, "cannot open %s: %s", path, strerror(errno));
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        snprintf(err, errlen, "cannot seek %s: %s", path, strerror(errno));
        fclose(f);
        return -1;
    }
    long sz = ftell(f);
    if (sz < 0) {
        snprintf(err, errlen, "cannot tell %s: %s", path, strerror(errno));
        fclose(f);
        return -1;
    }
    rewind(f);

    char *buf = malloc((size_t)sz + 1);
    if (buf == NULL) {
        snprintf(err, errlen, "out of memory reading %s", path);
        fclose(f);
        return -1;
    }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';

    *out = buf;
    if (len != NULL)
        *len = n;
    return 0;
}

int pr_write_file(const char *path, const char *data, size_t len, char *err, size_t errlen)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        snprintf(err, errlen, "cannot open %s: %s", path, strerror(errno));
        return -1;
    }
    size_t n = fwrite(data, 1, len, f);
    if (n != len || fflush(f) != 0) {
        snprintf(err, errlen, "cannot write %s: %s", path, strerror(errno));
        fclose(f);
        return -1;
    }
    if (fclose(f) != 0) {
        snprintf(err, errlen, "cannot close %s: %s", path, strerror(errno));
        return -1;
    }
    return 0;
}

int pr_write_file_atomic(const char *path, const char *data, size_t len,
                         char *err, size_t errlen)
{
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s.tmp%ld", path, (long)getpid());

    if (pr_write_file(tmp, data, len, err, errlen) != 0)
        return -1;
    if (rename(tmp, path) != 0) {
        snprintf(err, errlen, "cannot rename %s -> %s: %s", tmp, path, strerror(errno));
        unlink(tmp);
        return -1;
    }
    return 0;
}

int pr_mkdirs(const char *path, char *err, size_t errlen)
{
    if (path == NULL || path[0] == '\0') {
        snprintf(err, errlen, "empty directory path");
        return -1;
    }
    char tmp[1024];
    pr_strlcpy(tmp, path, sizeof tmp);
    size_t n = strlen(tmp);
    if (n > 1 && tmp[n - 1] == '/')
        tmp[n - 1] = '\0';

    for (char *p = tmp + 1; *p != '\0'; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
            snprintf(err, errlen, "cannot mkdir %s: %s", tmp, strerror(errno));
            return -1;
        }
        *p = '/';
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        snprintf(err, errlen, "cannot mkdir %s: %s", tmp, strerror(errno));
        return -1;
    }
    return 0;
}

int pr_lock_file(const char *path, char *err, size_t errlen)
{
    int fd = open(path, O_RDWR | O_CREAT, 0600);
    if (fd < 0) {
        snprintf(err, errlen, "cannot open lock %s: %s", path, strerror(errno));
        return -1;
    }
    struct flock fl;
    memset(&fl, 0, sizeof fl);
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;

    if (fcntl(fd, F_SETLKW, &fl) != 0) {
        snprintf(err, errlen, "cannot lock %s: %s", path, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

void pr_unlock_file(int fd)
{
    if (fd < 0)
        return;
    struct flock fl;
    memset(&fl, 0, sizeof fl);
    fl.l_type = F_UNLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;
    fcntl(fd, F_SETLK, &fl);
    close(fd);
}

/* ======================================================================= */
/* misc                                                                    */
/* ======================================================================= */

void pr_random_hex(char *dst, size_t dstlen)
{
    if (dst == NULL || dstlen == 0)
        return;

    size_t n = (dstlen - 1) / 2;
    unsigned char raw[256];
    if (n > sizeof raw)
        n = sizeof raw;

    size_t got = 0;
    FILE *f = fopen("/dev/urandom", "rb");
    if (f != NULL) {
        got = fread(raw, 1, n, f);
        fclose(f);
    }
    if (got < n) {
        /* weak fallback, only reached if /dev/urandom is unavailable */
        static unsigned long seed;
        if (seed == 0)
            seed = (unsigned long)pr_now_ms() ^ ((unsigned long)getpid() << 16);
        while (got < n) {
            seed = seed * 1103515245UL + 12345UL;
            raw[got++] = (unsigned char)((seed >> 16) & 0xff);
        }
    }

    static const char hex[] = "0123456789abcdef";
    size_t w = 0;
    for (size_t i = 0; i < n && w + 2 < dstlen; i++) {
        dst[w++] = hex[(raw[i] >> 4) & 0x0f];
        dst[w++] = hex[raw[i] & 0x0f];
    }
    dst[w] = '\0';
}

long long pr_now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return (long long)time(NULL) * 1000;
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

long long pr_now_s(void)
{
    return pr_now_ms() / 1000;
}

bool pr_parse_bool(const char *s, bool dflt)
{
    if (s == NULL || s[0] == '\0')
        return dflt;
    if (pr_str_eq_ci(s, "1") || pr_str_eq_ci(s, "true") || pr_str_eq_ci(s, "yes") ||
        pr_str_eq_ci(s, "on") || pr_str_eq_ci(s, "an") || pr_str_eq_ci(s, "ein"))
        return true;
    if (pr_str_eq_ci(s, "0") || pr_str_eq_ci(s, "false") || pr_str_eq_ci(s, "no") ||
        pr_str_eq_ci(s, "off") || pr_str_eq_ci(s, "aus"))
        return false;
    return dflt;
}

long pr_parse_long(const char *s, long dflt, bool *ok)
{
    if (ok != NULL)
        *ok = false;
    if (s == NULL || s[0] == '\0')
        return dflt;
    errno = 0;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s || errno == ERANGE)
        return dflt;
    while (*end == ' ' || *end == '\t')
        end++;
    if (*end != '\0')
        return dflt;
    if (ok != NULL)
        *ok = true;
    return v;
}

bool pr_wildcard_match(const char *pattern, const char *text)
{
    if (pattern == NULL || text == NULL)
        return false;

    const char *p = pattern, *t = text;
    const char *star = NULL, *star_t = NULL;

    while (*t != '\0') {
        if (*p == '?') {
            p++;
            t++;
        } else if (*p == '*') {
            star = p++;
            star_t = t;
        } else if (*p == *t) {
            p++;
            t++;
        } else if (star != NULL) {
            p = star + 1;
            t = ++star_t;
        } else {
            return false;
        }
    }
    while (*p == '*')
        p++;
    return *p == '\0';
}

bool pr_split3(const char *s, char sep, char *a, size_t alen,
               char *b, size_t blen, char *c, size_t clen)
{
    if (s == NULL)
        return false;
    const char *p1 = strchr(s, sep);
    if (p1 == NULL)
        return false;
    const char *p2 = strchr(p1 + 1, sep);
    if (p2 == NULL)
        return false;

    size_t n1 = (size_t)(p1 - s);
    if (alen == 0)
        return false;
    if (n1 >= alen)
        n1 = alen - 1;
    memcpy(a, s, n1);
    a[n1] = '\0';

    size_t n2 = (size_t)(p2 - p1 - 1);
    if (blen == 0)
        return false;
    if (n2 >= blen)
        n2 = blen - 1;
    memcpy(b, p1 + 1, n2);
    b[n2] = '\0';

    pr_strlcpy(c, p2 + 1, clen);
    return true;
}
