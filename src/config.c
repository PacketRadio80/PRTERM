/*
 * PRTERM - CB & Amateur Radio Terminal
 * config.c - typed configuration model on top of prterm.ini.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "prterm_compat.h"

#include "config.h"
#include "lang.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *PR_DEFAULT_INI = "prterm.ini";

/* ======================================================================= */
/* Helpers                                                                 */
/* ======================================================================= */

static void copy_str(char *dst, size_t dstlen, const ini *i,
                     const char *sec, const char *key, const char *dflt)
{
    const char *v = ini_get(i, sec, key, NULL);
    pr_strlcpy(dst, v != NULL ? v : (dflt != NULL ? dflt : ""), dstlen);
    pr_trim(dst);
}

static long clamp_long(long v, long lo, long hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* ======================================================================= */
/* Defaults                                                                */
/* ======================================================================= */

void pr_config_defaults(pr_config *cfg)
{
    memset(cfg, 0, sizeof *cfg);

    pr_strlcpy(cfg->site_name, "PRTERM", sizeof cfg->site_name);
    pr_strlcpy(cfg->site_subtitle, "CB & Amateur Radio Terminal", sizeof cfg->site_subtitle);
    pr_strlcpy(cfg->language, "en", sizeof cfg->language);

    pr_strlcpy(cfg->callerid, "PRTERM-1", sizeof cfg->callerid);
    cfg->qth[0] = '\0';
    cfg->locator[0] = '\0';

    cfg->duplex     = PR_DUPLEX_FULL;
    pr_strlcpy(cfg->rig_driver, "sim", sizeof cfg->rig_driver);
    pr_strlcpy(cfg->port, "/dev/ttyUSB0", sizeof cfg->port);
    cfg->baud       = 115200;
    cfg->radio_baud = 1200;
    pr_strlcpy(cfg->modem, "", sizeof cfg->modem);
    pr_strlcpy(cfg->serial_line, "8n1", sizeof cfg->serial_line);
    pr_strlcpy(cfg->kiss_init, "esc", sizeof cfg->kiss_init);
    cfg->freq_hz    = 27235000L;
    cfg->mode       = PR_BAND_FM;       /* Default: FM */
    cfg->tx_power_mw = 4000;            /* 4 W ERP - CB limit     */
    cfg->rx_poll_ms = 250;
    cfg->max_log    = 500;

    cfg->admin_enabled   = true;
    pr_strlcpy(cfg->admin_user, "admin", sizeof cfg->admin_user);
    cfg->admin_pass_hash[0] = '\0';     /* empty = login impossible   */
    cfg->session_ttl_min = 480;
    cfg->allow_guest_tx  = true;

    /* [mailboxd] - off until the operator turns it on */
    cfg->mailboxd_enabled = false;
    pr_strlcpy(cfg->mailboxd_dir, "/var/mailboxd", sizeof cfg->mailboxd_dir);

    pr_call_rules_default(&cfg->callsign);

    cfg->font_file[0] = '\0';
    cfg->font_size    = 14;
    cfg->line_height_pct = 120;
    pr_strlcpy(cfg->ui_density, "compact", sizeof cfg->ui_density);
    cfg->ui_rows    = 0;
    cfg->ui_columns = 0;
    pr_strlcpy(cfg->ui_theme, "silver", sizeof cfg->ui_theme);

    pr_strlcpy(cfg->runtime_dir, "./prterm.runtime", sizeof cfg->runtime_dir);

    cfg->bandplan = pr_bandplan_default();
}

/* ======================================================================= */
/* Apply from INI                                                          */
/* ======================================================================= */

static void apply_bans(pr_config *cfg, const ini *i)
{
    cfg->nbans = 0;

    size_t n = ini_count(i);
    for (size_t k = 0; k < n; k++) {
        const char *sec = ini_section_at(i, k);
        if (sec == NULL || !pr_str_eq_ci(sec, "ban"))
            continue;
        const char *pattern = ini_key_at(i, k);
        const char *reason  = ini_value_at(i, k);
        if (pattern == NULL || pattern[0] == '\0')
            continue;
        if (pr_config_add_ban(cfg, pattern, reason != NULL ? reason : "") != 0)
            break;
    }
}

/*
 * Read stations from [station:NAME].
 *
 * Each station is a complete device: own TNC, own radio, own antenna.
 * radio_baud is read and displayed, but never sent to the device - the
 * rate is fixed hardware and cannot be changed.
 */
static void apply_stations(pr_config *cfg, const ini *i)
{
    cfg->nstations = 0;

    size_t n = ini_count(i);
    for (size_t k = 0; k < n && cfg->nstations < PR_MAX_STATIONS; k++) {
        const char *sec = ini_section_at(i, k);
        if (sec == NULL || !pr_starts_with(sec, "station:"))
            continue;

        /* Apply each section only once       */
        bool known = false;
        for (size_t s = 0; s < cfg->nstations; s++) {
            if (pr_str_eq_ci(cfg->stations[s].name, sec + 8)) {
                known = true;
                break;
            }
        }
        if (known)
            continue;

        pr_station *st = &cfg->stations[cfg->nstations];
        memset(st, 0, sizeof *st);
        pr_strlcpy(st->name, sec + 8, sizeof st->name);

        copy_str(st->rig_driver, sizeof st->rig_driver, i, sec, "driver", "tnc2");
        copy_str(st->port, sizeof st->port, i, sec, "port", "");
        st->baud = clamp_long(ini_get_int(i, sec, "baud", 19200), 300, 4000000);
        st->radio_baud = clamp_long(ini_get_int(i, sec, "radio_baud", 1200), 50, 9600);
        copy_str(st->modem, sizeof st->modem, i, sec, "modem", "");
        copy_str(st->serial_line, sizeof st->serial_line, i, sec, "line", "8n1");
        copy_str(st->kiss_init, sizeof st->kiss_init, i, sec, "kiss_init", "esc");
        pr_lower(st->kiss_init);
        copy_str(st->antenna, sizeof st->antenna, i, sec, "antenne", "");
        st->enabled = ini_get_bool(i, sec, "enabled", true);

        char call[PR_CALLSIGN_MAX];
        copy_str(call, sizeof call, i, sec, "callerid", cfg->callerid);
        pr_upper(call);
        pr_strlcpy(st->callerid, call, sizeof st->callerid);

        cfg->nstations++;
    }
}

int pr_config_apply(pr_config *cfg, const ini *i, char *err, size_t errlen)
{
    pr_config_defaults(cfg);

    /* [site] */
    copy_str(cfg->site_name, sizeof cfg->site_name, i, "site", "name", "PRTERM");
    copy_str(cfg->site_subtitle, sizeof cfg->site_subtitle, i, "site", "subtitle", "");
    copy_str(cfg->language, sizeof cfg->language, i, "site", "language", "en");
    pr_lower(cfg->language);
    /* Only what PRTERM ships - see lang.h. Everything else stays English. */
    if (!pr_lang_supported(cfg->language))
        pr_strlcpy(cfg->language, "en", sizeof cfg->language);

    /* [station] */
    copy_str(cfg->qth, sizeof cfg->qth, i, "station", "qth", "");
    copy_str(cfg->locator, sizeof cfg->locator, i, "station", "locator", "");

    char call[PR_CALLSIGN_MAX];
    copy_str(call, sizeof call, i, "station", "callerid", "PRTERM-1");
    pr_upper(call);
    /* Rules are fixed only later; store first, then validate.        */
    pr_strlcpy(cfg->callerid, call, sizeof cfg->callerid);

    /* [callsign] - read before validating the CALLERID    */
    cfg->callsign.callid_max_len =
        (int)clamp_long(ini_get_int(i, "callsign", "callid_max_len", 6), 1, 10);
    cfg->callsign.callerid_base_len =
        (int)clamp_long(ini_get_int(i, "callsign", "callerid_base_len", 6), 1, 10);
    cfg->callsign.callerid_max_total =
        (int)clamp_long(ini_get_int(i, "callsign", "callerid_max_total", 8), 1, 16);
    cfg->callsign.allow_ssid =
        ini_get_bool(i, "callsign", "callerid_allow_ssid", true);
    cfg->callsign.ssid_digits =
        (int)clamp_long(ini_get_int(i, "callsign", "callerid_ssid_digits", 1), 0, 2);

    if (!callerid_valid(cfg->callerid, &cfg->callsign)) {
        snprintf(err, errlen,
                 "[station] callerid=\"%s\" is invalid "
                 "(base max. %d characters, SSID \"-<digit>\", total max. %d)",
                 cfg->callerid, cfg->callsign.callerid_base_len,
                 cfg->callsign.callerid_max_total);
        return -1;
    }

    /* [radio] */
    copy_str(cfg->rig_driver, sizeof cfg->rig_driver, i, "radio", "driver", "sim");
    pr_lower(cfg->rig_driver);
    copy_str(cfg->port, sizeof cfg->port, i, "radio", "port", "/dev/ttyUSB0");
    cfg->baud = clamp_long(ini_get_int(i, "radio", "baud", 115200), 300, 4000000);
    /* Radio baud rate: data rate on the channel, NOT to the interface     */
    cfg->radio_baud = clamp_long(ini_get_int(i, "radio", "radio_baud", 1200),
                                 50, 9600);
    copy_str(cfg->modem, sizeof cfg->modem, i, "radio", "modem", "");
    copy_str(cfg->serial_line, sizeof cfg->serial_line, i, "radio", "line", "8n1");
    pr_lower(cfg->serial_line);
    copy_str(cfg->kiss_init, sizeof cfg->kiss_init, i, "radio", "kiss_init", "esc");
    pr_lower(cfg->kiss_init);
    cfg->duplex = pr_duplex_from_name(ini_get(i, "radio", "duplex", "full"));
    cfg->freq_hz = ini_get_int(i, "radio", "freq_hz", 27125000L);
    cfg->mode = pr_band_mode_from_name(ini_get(i, "radio", "mode", "am"));
    cfg->tx_power_mw = clamp_long(ini_get_int(i, "radio", "tx_power_mw", 4000),
                                  0, 12000);
    cfg->rx_poll_ms = (int)clamp_long(ini_get_int(i, "radio", "rx_poll_ms", 250), 50, 10000);
    cfg->max_log = (int)clamp_long(ini_get_int(i, "radio", "max_log", 500), 20, 20000);

    if (cfg->mode == 0) {
        snprintf(err, errlen, "[radio] mode invalid (allowed: fm, am, ssb)");
        return -1;
    }

    /* [admin] */
    cfg->admin_enabled = ini_get_bool(i, "admin", "enabled", true);
    copy_str(cfg->admin_user, sizeof cfg->admin_user, i, "admin", "user", "admin");
    copy_str(cfg->admin_pass_hash, sizeof cfg->admin_pass_hash, i, "admin", "pass_hash", "");
    cfg->session_ttl_min =
        (int)clamp_long(ini_get_int(i, "admin", "session_ttl_min", 480), 1, 10080);
    cfg->allow_guest_tx = ini_get_bool(i, "admin", "allow_guest_tx", true);

    /* [mailboxd] */
    cfg->mailboxd_enabled = ini_get_bool(i, "mailboxd", "enabled", false);
    copy_str(cfg->mailboxd_dir, sizeof cfg->mailboxd_dir,
             i, "mailboxd", "dir", "/var/mailboxd");

    /* [ui] */
    copy_str(cfg->font_file, sizeof cfg->font_file, i, "ui", "font_file", "");
    cfg->font_size = (int)clamp_long(ini_get_int(i, "ui", "font_size", 14), 6, 96);

    /* line_height can be "1.2" - mapped to percent         */
    {
        const char *lh = ini_get(i, "ui", "line_height", "1.2");
        double d = 1.2;
        if (lh != NULL) {
            char *end = NULL;
            double v = strtod(lh, &end);
            if (end != lh && v > 0.0)
                d = v;
        }
        cfg->line_height_pct = (int)clamp_long((long)(d * 100.0 + 0.5), 100, 300);
    }

    copy_str(cfg->ui_density, sizeof cfg->ui_density, i, "ui", "density", "compact");
    cfg->ui_rows    = (int)clamp_long(ini_get_int(i, "ui", "rows", 0), 0, 500);
    cfg->ui_columns = (int)clamp_long(ini_get_int(i, "ui", "columns", 0), 0, 500);
    copy_str(cfg->ui_theme, sizeof cfg->ui_theme, i, "ui", "theme", "silver");

    /* [paths] */
    copy_str(cfg->runtime_dir, sizeof cfg->runtime_dir, i, "paths", "runtime_dir",
             "./prterm.runtime");

    /* [ban] */
    apply_bans(cfg, i);

    /* [station:*] - complete stations, one TNC + radio + antenna each    */
    apply_stations(cfg, i);

    cfg->bandplan = pr_bandplan_default();
    cfg->raw = NULL;
    return 0;
}

int pr_config_load(pr_config *cfg, const char *path, char *err, size_t errlen)
{
    pr_config_defaults(cfg);

    ini *i = ini_load(path, err, errlen);
    if (i == NULL)
        return -1;

    if (pr_config_apply(cfg, i, err, errlen) != 0) {
        ini_free(i);
        return -1;
    }

    pr_strlcpy(cfg->ini_path, path, sizeof cfg->ini_path);
    cfg->raw = i;
    return 0;
}

void pr_config_free(pr_config *cfg)
{
    if (cfg == NULL)
        return;
    ini_free(cfg->raw);
    cfg->raw = NULL;
    free(cfg->bans);
    cfg->bans = NULL;
    cfg->nbans = cfg->cap_bans = 0;
}

/* ======================================================================= */
/* Write back                                                              */
/* ======================================================================= */

void pr_config_write(const pr_config *cfg, ini *i)
{
    char buf[64];

    ini_set(i, "site", "name", cfg->site_name);
    ini_set(i, "site", "subtitle", cfg->site_subtitle);
    ini_set(i, "site", "language", cfg->language);

    ini_set(i, "station", "callerid", cfg->callerid);
    ini_set(i, "station", "qth", cfg->qth);
    ini_set(i, "station", "locator", cfg->locator);

    ini_set(i, "radio", "duplex", pr_duplex_name(cfg->duplex));
    ini_set(i, "radio", "driver", cfg->rig_driver);
    ini_set(i, "radio", "port", cfg->port);
    ini_set_int(i, "radio", "baud", cfg->baud);
    ini_set_int(i, "radio", "radio_baud", cfg->radio_baud);
    ini_set(i, "radio", "modem", cfg->modem);
    ini_set(i, "radio", "line", cfg->serial_line);
    ini_set_int(i, "radio", "freq_hz", cfg->freq_hz);
    ini_set(i, "radio", "mode", pr_band_mode_name(cfg->mode));
    ini_set_int(i, "radio", "tx_power_mw", cfg->tx_power_mw);
    /* Default mode is FM             */
    ini_set_int(i, "radio", "rx_poll_ms", cfg->rx_poll_ms);
    ini_set_int(i, "radio", "max_log", cfg->max_log);

    ini_set_bool(i, "admin", "enabled", cfg->admin_enabled);
    ini_set(i, "admin", "user", cfg->admin_user);
    ini_set(i, "admin", "pass_hash", cfg->admin_pass_hash);
    ini_set_int(i, "admin", "session_ttl_min", cfg->session_ttl_min);
    ini_set_bool(i, "admin", "allow_guest_tx", cfg->allow_guest_tx);

    ini_set_bool(i, "mailboxd", "enabled", cfg->mailboxd_enabled);
    ini_set(i, "mailboxd", "dir", cfg->mailboxd_dir);

    ini_set_int(i, "callsign", "callid_max_len", cfg->callsign.callid_max_len);
    ini_set_int(i, "callsign", "callerid_base_len", cfg->callsign.callerid_base_len);
    ini_set_int(i, "callsign", "callerid_max_total", cfg->callsign.callerid_max_total);
    ini_set_bool(i, "callsign", "callerid_allow_ssid", cfg->callsign.allow_ssid);
    ini_set_int(i, "callsign", "callerid_ssid_digits", cfg->callsign.ssid_digits);

    ini_set(i, "ui", "font_file", cfg->font_file);
    ini_set_int(i, "ui", "font_size", cfg->font_size);
    snprintf(buf, sizeof buf, "%.2f", cfg->line_height_pct / 100.0);
    ini_set(i, "ui", "line_height", buf);
    ini_set(i, "ui", "density", cfg->ui_density);
    ini_set_int(i, "ui", "rows", cfg->ui_rows);
    ini_set_int(i, "ui", "columns", cfg->ui_columns);
    ini_set(i, "ui", "theme", cfg->ui_theme);

    ini_set(i, "paths", "runtime_dir", cfg->runtime_dir);
}

/* ======================================================================= */
/* Ban list                                                                */
/* ======================================================================= */

int pr_config_add_ban(pr_config *cfg, const char *pattern, const char *reason)
{
    if (cfg == NULL || pattern == NULL || pattern[0] == '\0')
        return -1;

    if (pr_config_find_ban(cfg, pattern) != NULL)
        return 0;                       /* already present   */

    if (cfg->nbans + 1 > cfg->cap_bans) {
        size_t ncap = cfg->cap_bans != 0 ? cfg->cap_bans * 2 : 8;
        pr_ban *p = realloc(cfg->bans, ncap * sizeof *p);
        if (p == NULL)
            return -1;
        cfg->bans = p;
        cfg->cap_bans = ncap;
    }

    pr_ban *b = &cfg->bans[cfg->nbans];
    pr_strlcpy(b->pattern, pattern, sizeof b->pattern);
    pr_trim(b->pattern);
    pr_upper(b->pattern);
    pr_strlcpy(b->reason, reason != NULL ? reason : "", sizeof b->reason);
    pr_trim(b->reason);
    cfg->nbans++;
    return 0;
}

const pr_ban *pr_config_find_ban(const pr_config *cfg, const char *pattern)
{
    if (cfg == NULL || pattern == NULL)
        return NULL;
    for (size_t i = 0; i < cfg->nbans; i++) {
        if (pr_str_eq_ci(cfg->bans[i].pattern, pattern))
            return &cfg->bans[i];
    }
    return NULL;
}

bool pr_config_del_ban(pr_config *cfg, const char *pattern)
{
    if (cfg == NULL || pattern == NULL)
        return false;
    for (size_t i = 0; i < cfg->nbans; i++) {
        if (!pr_str_eq_ci(cfg->bans[i].pattern, pattern))
            continue;
        memmove(&cfg->bans[i], &cfg->bans[i + 1],
                (cfg->nbans - i - 1) * sizeof *cfg->bans);
        cfg->nbans--;
        return true;
    }
    return false;
}

bool pr_config_is_banned(const pr_config *cfg, const char *id)
{
    if (cfg == NULL || id == NULL)
        return false;
    for (size_t i = 0; i < cfg->nbans; i++) {
        if (call_pattern_match(cfg->bans[i].pattern, id))
            return true;
    }
    return false;
}

/* ======================================================================= */
/* Helper functions                                                        */
/* ======================================================================= */

const char *pr_duplex_name(pr_duplex d)
{
    return d == PR_DUPLEX_FULL ? "full" : "half";
}

pr_duplex pr_duplex_from_name(const char *s)
{
    if (s == NULL)
        return PR_DUPLEX_HALF;
    return pr_str_eq_ci(s, "full") ? PR_DUPLEX_FULL : PR_DUPLEX_HALF;
}

/* ======================================================================= */
/* Stations                                                                */
/* ======================================================================= */

const pr_station *pr_config_station(const pr_config *cfg, const char *name)
{
    if (cfg == NULL || name == NULL || name[0] == '\0')
        return NULL;
    for (size_t i = 0; i < cfg->nstations; i++) {
        if (pr_str_eq_ci(cfg->stations[i].name, name))
            return &cfg->stations[i];
    }
    return NULL;
}

const pr_station *pr_config_default_station(const pr_config *cfg)
{
    if (cfg == NULL)
        return NULL;
    for (size_t i = 0; i < cfg->nstations; i++) {
        if (cfg->stations[i].enabled)
            return &cfg->stations[i];
    }
    return cfg->nstations > 0 ? &cfg->stations[0] : NULL;
}

const pr_station *pr_config_apply_station(pr_config *cfg, const char *name)
{
    const pr_station *st = pr_config_station(cfg, name);
    if (st == NULL)
        st = pr_config_default_station(cfg);
    if (st == NULL)
        return NULL;

    /*
     * Take over only the device settings. Frequency and channel
     * apply to all stations on the same channel.
     */
    pr_strlcpy(cfg->port, st->port, sizeof cfg->port);
    cfg->baud = st->baud;
    cfg->radio_baud = st->radio_baud;
    pr_strlcpy(cfg->modem, st->modem, sizeof cfg->modem);
    pr_strlcpy(cfg->serial_line, st->serial_line, sizeof cfg->serial_line);
    pr_strlcpy(cfg->kiss_init, st->kiss_init, sizeof cfg->kiss_init);
    pr_strlcpy(cfg->rig_driver, st->rig_driver, sizeof cfg->rig_driver);
    pr_strlcpy(cfg->callerid, st->callerid, sizeof cfg->callerid);
    pr_strlcpy(cfg->active_station, st->name, sizeof cfg->active_station);
    return st;
}

int pr_config_channel(const pr_config *cfg)
{
    if (cfg == NULL || cfg->bandplan == NULL)
        return -1;
    const pr_channel *ch = pr_bandplan_at_freq(cfg->bandplan, cfg->freq_hz);
    return ch != NULL ? ch->num : -1;
}

bool pr_config_set_channel(pr_config *cfg, int num)
{
    if (cfg == NULL || cfg->bandplan == NULL)
        return false;
    const pr_channel *ch = pr_bandplan_channel(cfg->bandplan, num);
    if (ch == NULL)
        return false;
    cfg->freq_hz = ch->freq_hz;
    /* Keep the mode if it is allowed on this channel           */
    if ((ch->modes & cfg->mode) == 0) {
        if (ch->modes & PR_BAND_FM)      cfg->mode = PR_BAND_FM;
        else if (ch->modes & PR_BAND_AM) cfg->mode = PR_BAND_AM;
        else                             cfg->mode = PR_BAND_SSB;
    }
    return true;
}
