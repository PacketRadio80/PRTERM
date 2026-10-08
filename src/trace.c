/*
 * PRTERM - CB & Amateur Radio Terminal
 * trace.c - debug/trace logging (stderr, timestamped).
 *
 * Single-threaded by design: the daemon is one loop, the CGI one
 * request. No locking, no allocation - a static context tag and
 * direct stderr writes keep this usable from signal-adjacent code
 * and from every layer alike.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "prterm_compat.h"

#include "trace.h"
#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static pr_trace_level g_level = PR_TR_WARN;
static char           g_ctx[64] = "prterm";

pr_trace_level pr_trace_level_from_name(const char *name,
                                        pr_trace_level fallback)
{
    static const struct { const char *name; pr_trace_level lv; } tab[] = {
        { "off",   PR_TR_OFF   },
        { "error", PR_TR_ERROR },
        { "warn",  PR_TR_WARN  },
        { "info",  PR_TR_INFO  },
        { "debug", PR_TR_DEBUG },
        { "trace", PR_TR_TRACE }
    };

    if (name == NULL || name[0] == '\0')
        return fallback;
    for (size_t i = 0; i < sizeof tab / sizeof tab[0]; i++)
        if (pr_str_eq_ci(name, tab[i].name))
            return tab[i].lv;
    return fallback;
}

const char *pr_trace_level_name(pr_trace_level lv)
{
    switch (lv) {
    case PR_TR_OFF:   return "off";
    case PR_TR_ERROR: return "error";
    case PR_TR_WARN:  return "warn";
    case PR_TR_INFO:  return "info";
    case PR_TR_DEBUG: return "debug";
    case PR_TR_TRACE: return "trace";
    }
    return "warn";
}

void pr_trace_init(pr_trace_level lv, const char *ctx)
{
    g_level = lv;
    if (ctx != NULL && ctx[0] != '\0')
        pr_strlcpy(g_ctx, ctx, sizeof g_ctx);
}

pr_trace_level pr_trace_get_level(void)
{
    return g_level;
}

static bool enabled(pr_trace_level lv)
{
    return lv != PR_TR_OFF && g_level != PR_TR_OFF && lv <= g_level;
}

static void stamp(char *buf, size_t len)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm t;
    localtime_r(&ts.tv_sec, &t);
    snprintf(buf, len, "%04d-%02d-%02d %02d:%02d:%02d.%03ld",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec, (long)(ts.tv_nsec / 1000000));
}

void pr_trace(pr_trace_level lv, const char *fmt, ...)
{
    if (!enabled(lv))
        return;

    char ts[40];
    stamp(ts, sizeof ts);
    fprintf(stderr, "[%s] %s %s: ", ts, g_ctx, pr_trace_level_name(lv));

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    fputc('\n', stderr);
    fflush(stderr);
}

#define TRACE_HEX_CAP 512

void pr_trace_hex(pr_trace_level lv, const char *label,
                  const unsigned char *data, size_t len)
{
    if (!enabled(lv))
        return;

    char ts[40];
    stamp(ts, sizeof ts);
    fprintf(stderr, "[%s] %s %s: %s - %zu byte%s\n", ts, g_ctx,
            pr_trace_level_name(lv), label != NULL ? label : "hex",
            len, len == 1 ? "" : "s");

    size_t shown = len < TRACE_HEX_CAP ? len : TRACE_HEX_CAP;
    for (size_t off = 0; off < shown; off += 16) {
        char hex[3 * 16 + 1];
        char asc[16 + 1];
        size_t hp = 0, ap = 0;
        for (size_t k = 0; k < 16; k++) {
            if (off + k < shown) {
                unsigned char c = data[off + k];
                hex[hp++] = "0123456789abcdef"[c >> 4];
                hex[hp++] = "0123456789abcdef"[c & 0x0fu];
                hex[hp++] = ' ';
                asc[ap++] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
            } else {
                hex[hp++] = ' ';
                hex[hp++] = ' ';
                hex[hp++] = ' ';
            }
        }
        hex[hp] = '\0';
        asc[ap] = '\0';
        fprintf(stderr, "    %04zx  %s |%s|\n", off, hex, asc);
    }
    if (shown < len)
        fprintf(stderr, "    ... (%zu bytes not shown)\n", len - shown);
    fflush(stderr);
}
