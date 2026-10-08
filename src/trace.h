/*
 * PRTERM - CB & Amateur Radio Terminal
 * trace.h - debug/trace logging for daemon, driver and tools.
 *
 * Everything goes to stderr with a timestamp and a context tag:
 *   prterm-tncd  ->  journalctl -u prterm-tncd
 *   prterm.cgi   ->  the webserver's error log
 *
 * The level comes from the INI:  [debug] level = off | error | warn |
 * info | debug | trace. "trace" additionally hexdumps every byte that
 * goes over the serial line or the socket - that is the level at
 * which the wire protocol itself becomes visible.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PRTERM_TRACE_H
#define PRTERM_TRACE_H

#include <stddef.h>

typedef enum pr_trace_level {
    PR_TR_OFF   = 0,
    PR_TR_ERROR = 1,
    PR_TR_WARN  = 2,
    PR_TR_INFO  = 3,
    PR_TR_DEBUG = 4,
    PR_TR_TRACE = 5
} pr_trace_level;

/* Name -> level. Unknown or empty names yield the fallback. */
pr_trace_level pr_trace_level_from_name(const char *name,
                                        pr_trace_level fallback);
/* Level -> name (for writing back into the INI). */
const char    *pr_trace_level_name(pr_trace_level lv);

/* Process-wide setup: level and context tag (e.g. "tncd", "cgi"). */
void           pr_trace_init(pr_trace_level lv, const char *ctx);
pr_trace_level pr_trace_get_level(void);

/* One line to stderr when the level is enabled. */
void pr_trace(pr_trace_level lv, const char *fmt, ...);

/*
 * Hexdump (16 bytes per line: offset, hex, ASCII), capped so a
 * runaway buffer cannot flood the journal.
 */
void pr_trace_hex(pr_trace_level lv, const char *label,
                  const unsigned char *data, size_t len);

#endif /* PRTERM_TRACE_H */
