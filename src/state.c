/*
 * PRTERM - CB & Amateur Radio Terminal
 * state.c - Persistence of the rig state and the message log.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "prterm_compat.h"

#include "state.h"
#include "ini.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ======================================================================= */
/* Paths                                                                   */
/* ======================================================================= */

void pr_state_path(const pr_config *cfg, char *dst, size_t dstlen)
{
    snprintf(dst, dstlen, "%.480s/state.ini", cfg->runtime_dir);
}

void pr_log_path(const pr_config *cfg, char *dst, size_t dstlen)
{
    snprintf(dst, dstlen, "%.480s/log", cfg->runtime_dir);
}

void pr_session_dir(const pr_config *cfg, char *dst, size_t dstlen)
{
    snprintf(dst, dstlen, "%.480s/sessions", cfg->runtime_dir);
}

void pr_lock_path(const pr_config *cfg, char *dst, size_t dstlen)
{
    snprintf(dst, dstlen, "%.480s/lock", cfg->runtime_dir);
}

/* ======================================================================= */
/* Runtime directory                                                       */
/* ======================================================================= */

int pr_runtime_init(const pr_config *cfg, char *err, size_t errlen)
{
    if (pr_mkdirs(cfg->runtime_dir, err, errlen) != 0)
        return -1;

    char sd[1024];
    pr_session_dir(cfg, sd, sizeof sd);
    if (pr_mkdirs(sd, err, errlen) != 0)
        return -1;

    return 0;
}

int pr_state_lock(const pr_config *cfg, char *err, size_t errlen)
{
    char p[1024];
    pr_lock_path(cfg, p, sizeof p);
    return pr_lock_file(p, err, errlen);
}

void pr_state_unlock(int fd)
{
    pr_unlock_file(fd);
}

/* ======================================================================= */
/* Rig state                                                               */
/* ======================================================================= */

int pr_state_load(const pr_config *cfg, pr_rig_state *st, char *err, size_t errlen)
{
    memset(st, 0, sizeof *st);

    /* Defaults from the configuration   */
    st->freq_hz = cfg->freq_hz;
    st->mode    = cfg->mode;
    st->duplex  = cfg->duplex;
    st->ptt     = false;
    st->monitor = false;
    pr_strlcpy(st->status, "stopped", sizeof st->status);

    char path[1024];
    pr_state_path(cfg, path, sizeof path);
    if (!pr_file_exists(path))
        return 0;                       /* no state saved yet            */

    ini *i = ini_load(path, err, errlen);
    if (i == NULL)
        return -1;

    st->freq_hz     = ini_get_int(i, "rig", "freq_hz", st->freq_hz);
    st->mode        = (unsigned)ini_get_int(i, "rig", "mode", (long)st->mode);
    st->ptt         = ini_get_bool(i, "rig", "ptt", false);
    st->monitor     = ini_get_bool(i, "rig", "monitor", false);
    st->rx_muted    = ini_get_bool(i, "rig", "rx_muted", false);
    st->squelch_open = ini_get_bool(i, "rig", "squelch_open", false);
    st->rx_db       = (int)ini_get_int(i, "rig", "rx_db", -120);
    st->tx_db       = (int)ini_get_int(i, "rig", "tx_db", 0);
    st->duplex      = pr_duplex_from_name(ini_get(i, "rig", "duplex",
                                                 pr_duplex_name(cfg->duplex)));

    pr_strlcpy(st->last_rx_from, ini_get(i, "rig", "last_rx_from", ""),
               sizeof st->last_rx_from);
    pr_strlcpy(st->last_rx_text, ini_get(i, "rig", "last_rx_text", ""),
               sizeof st->last_rx_text);
    st->last_rx_ts = ini_get_int(i, "rig", "last_rx_ts", 0);
    st->last_tx_ts = ini_get_int(i, "rig", "last_tx_ts", 0);
    st->rx_count   = ini_get_int(i, "stats", "rx_count", 0);
    st->tx_count   = ini_get_int(i, "stats", "tx_count", 0);

    pr_strlcpy(st->device, ini_get(i, "rig", "device", cfg->port),
               sizeof st->device);
    pr_strlcpy(st->status, ini_get(i, "rig", "status", "stopped"),
               sizeof st->status);
    st->link_ok = ini_get_bool(i, "rig", "link_ok", false);
    pr_strlcpy(st->detail, ini_get(i, "rig", "detail", ""), sizeof st->detail);

    ini_free(i);
    return 0;
}

int pr_state_save(const pr_config *cfg, const pr_rig_state *st,
                  char *err, size_t errlen)
{
    /*
     * The mutation lock: several CGI processes write this file and
     * the log. The writes themselves are atomic (temp file + rename),
     * the lock keeps append and compaction of the log from losing a
     * line. Taken here for exactly this write - do NOT hold the lock
     * around this call.
     */
    int lfd = pr_state_lock(cfg, err, errlen);
    if (lfd < 0)
        return -1;

    ini *i = ini_new();
    if (i == NULL) {
        pr_state_unlock(lfd);
        snprintf(err, errlen, "out of memory");
        return -1;
    }

    ini_set_int(i, "rig", "freq_hz", st->freq_hz);
    ini_set_int(i, "rig", "mode", (long)st->mode);
    ini_set(i, "rig", "duplex", pr_duplex_name(st->duplex));
    ini_set_bool(i, "rig", "ptt", st->ptt);
    ini_set_bool(i, "rig", "monitor", st->monitor);
    ini_set_bool(i, "rig", "rx_muted", st->rx_muted);
    ini_set_bool(i, "rig", "squelch_open", st->squelch_open);
    ini_set_int(i, "rig", "rx_db", st->rx_db);
    ini_set_int(i, "rig", "tx_db", st->tx_db);
    ini_set(i, "rig", "last_rx_from", st->last_rx_from);
    ini_set(i, "rig", "last_rx_text", st->last_rx_text);
    ini_set_int(i, "rig", "last_rx_ts", st->last_rx_ts);
    ini_set_int(i, "rig", "last_tx_ts", st->last_tx_ts);
    ini_set(i, "rig", "device", st->device);
    ini_set(i, "rig", "status", st->status);
    ini_set_bool(i, "rig", "link_ok", st->link_ok);
    ini_set(i, "rig", "detail", st->detail);

    ini_set_int(i, "stats", "rx_count", st->rx_count);
    ini_set_int(i, "stats", "tx_count", st->tx_count);

    char path[1024];
    pr_state_path(cfg, path, sizeof path);
    int rc = ini_save(i, path, err, errlen);
    ini_free(i);
    pr_state_unlock(lfd);
    return rc;
}

/* ======================================================================= */
/* Message log                                                             */
/* ======================================================================= */

/* Remove tabs and line breaks from the text - the log is tabular.             */
static void sanitize(char *dst, size_t dstlen, const char *src)
{
    size_t w = 0;
    for (const char *p = src; *p != '\0' && w + 1 < dstlen; p++) {
        char c = *p;
        if (c == '\t' || c == '\r' || c == '\n')
            c = ' ';
        dst[w++] = c;
    }
    dst[w] = '\0';
}

static void parse_line(pr_msg *m, char *line)
{
    memset(m, 0, sizeof *m);

    char *p = line;
    char *f;

    f = strchr(p, '\t');
    if (f == NULL) return;
    *f = '\0'; m->ts = (long long)atoll(p); p = f + 1;

    f = strchr(p, '\t');
    if (f == NULL) return;
    *f = '\0'; m->kind = p[0]; p = f + 1;

    f = strchr(p, '\t');
    if (f == NULL) return;
    *f = '\0'; pr_strlcpy(m->from, p, sizeof m->from); p = f + 1;

    f = strchr(p, '\t');
    if (f == NULL) return;
    *f = '\0'; m->db = atoi(p); p = f + 1;

    pr_strlcpy(m->text, p, sizeof m->text);
}

static size_t count_lines(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return 0;
    size_t n = 0;
    int ch;
    while ((ch = fgetc(f)) != EOF)
        if (ch == '\n') n++;
    fclose(f);
    return n;
}

/*
 * Keeps the log file at no more than max_log lines.
 *
 * Lines are deliberately counted exactly instead of being estimated by
 * a byte size: the limit is a promise, not a tendency. The file is
 * limited to max_log lines anyway, so the effort is limited and small.
 */
static void maybe_compact(const pr_config *cfg, const char *path)
{
    long max = cfg->max_log > 0 ? cfg->max_log : 500;

    size_t lines = count_lines(path);
    if (lines <= (size_t)max)
        return;

    char *text = NULL;
    size_t len = 0;
    char err[128];
    if (pr_read_file(path, &text, &len, err, sizeof err) != 0)
        return;

    /* The count might have grown in the meantime       */
    lines = 0;
    for (size_t i = 0; i < len; i++)
        if (text[i] == '\n') lines++;

    if (lines <= (size_t)max) {
        free(text);
        return;
    }

    size_t skip = lines - (size_t)max;
    size_t seen = 0, start = 0;
    for (size_t i = 0; i < len; i++) {
        if (text[i] == '\n') {
            seen++;
            if (seen == skip) {
                start = i + 1;
                break;
            }
        }
    }
    (void)pr_write_file_atomic(path, text + start, len - start, err, sizeof err);
    free(text);
}

int pr_log_append(const pr_config *cfg, const pr_msg *m, char *err, size_t errlen)
{
    char path[1024];
    pr_log_path(cfg, path, sizeof path);

    char from[PR_CALLSIGN_MAX];
    char text[PR_MSG_TEXT * 2];
    sanitize(from, sizeof from, m->from);
    sanitize(text, sizeof text, m->text);

    char line[512];
    int n = snprintf(line, sizeof line, "%lld\t%c\t%s\t%d\t%s\n",
                     m->ts, m->kind, from, m->db, text);
    if (n < 0 || (size_t)n >= sizeof line) {
        snprintf(err, errlen, "message too long");
        return -1;
    }

    /* See pr_state_save: append and compaction share this lock.     */
    int lfd = pr_state_lock(cfg, err, errlen);
    if (lfd < 0)
        return -1;

    FILE *f = fopen(path, "ab");
    if (f == NULL) {
        pr_state_unlock(lfd);
        snprintf(err, errlen, "log not writable: %s", path);
        return -1;
    }
    size_t w = fwrite(line, 1, (size_t)n, f);
    if (fclose(f) != 0 || w != (size_t)n) {
        pr_state_unlock(lfd);
        snprintf(err, errlen, "log not writable: %s", path);
        return -1;
    }

    maybe_compact(cfg, path);
    pr_state_unlock(lfd);
    return 0;
}

int pr_log_tail(const pr_config *cfg, pr_msg *out, size_t cap, size_t *n,
                char *err, size_t errlen)
{
    *n = 0;
    if (cap == 0)
        return 0;

    char path[1024];
    pr_log_path(cfg, path, sizeof path);
    if (!pr_file_exists(path))
        return 0;

    char *text = NULL;
    size_t len = 0;
    if (pr_read_file(path, &text, &len, err, errlen) != 0)
        return -1;

    /* Count lines and limit to the last cap               */
    size_t lines = 0;
    for (size_t i = 0; i < len; i++)
        if (text[i] == '\n') lines++;

    size_t skip = (lines > cap) ? lines - cap : 0;
    size_t seen = 0;
    size_t i = 0;

    while (i < len && seen < skip) {
        if (text[i] == '\n') seen++;
        i++;
    }

    size_t k = 0;
    while (i < len && k < cap) {
        size_t start = i;
        while (i < len && text[i] != '\n') i++;
        text[i] = '\0';
        if (i > start) {
            parse_line(&out[k], text + start);
            k++;
        }
        i++;
    }

    *n = k;
    free(text);
    return 0;
}

long pr_log_count(const pr_config *cfg)
{
    char path[1024];
    pr_log_path(cfg, path, sizeof path);

    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return 0;

    long c = 0;
    int ch;
    while ((ch = fgetc(f)) != EOF)
        if (ch == '\n') c++;
    fclose(f);
    return c;
}
