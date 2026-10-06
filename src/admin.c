/*
 * PRTERM - CB & Amateur Radio Terminal
 * admin.c - Admin actions.
 *
 * Each action writes back only its OWN keys. This keeps comments and
 * order in the rest of prterm.ini intact - the file is documented and
 * is also maintained by hand.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "prterm_compat.h"

#include "admin.h"

#include "callsign.h"
#include "html.h"
#include "ini.h"
#include "lang.h"
#include "radio.h"
#include "session.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ======================================================================= */
/* Helpers                                                                 */
/* ======================================================================= */

static void json_ok(pr_response *res)
{
    pr_response_json(res, 200);
    pr_buf_add(&res->body, "{\"ok\":true}");
}

static void json_err(pr_response *res, const char *msg)
{
    pr_response_json(res, 400);
    pr_buf_add(&res->body, "{\"ok\":false,\"error\":\"");
    pr_json_escape(&res->body, msg != NULL ? msg : "error");
    pr_buf_add(&res->body, "\"}");
}

/* Saves the INI and keeps the model consistent.      */
static bool persist(pr_config *cfg, pr_response *res)
{
    if (cfg->raw == NULL) {
        json_err(res, "no configuration loaded");
        return false;
    }
    char err[256];
    if (ini_save(cfg->raw, cfg->ini_path, err, sizeof err) != 0) {
        json_err(res, err);
        return false;
    }
    /* Re-read so validation and defaults take effect       */
    pr_config fresh;
    if (pr_config_load(&fresh, cfg->ini_path, err, sizeof err) != 0) {
        json_err(res, err);
        pr_config_free(&fresh);
        return false;
    }
    pr_config_free(cfg);
    *cfg = fresh;
    return true;
}

static const char *arg(pr_request *req, const char *k)
{
    const char *v = pr_req_param(req, k);
    return v != NULL ? v : "";
}

/* ======================================================================= */
/* Actions                                                                 */
/* ======================================================================= */

static void act_save_site(pr_request *req, pr_response *res, pr_config *cfg)
{
    ini_set(cfg->raw, "site", "name", arg(req, "site_name"));
    ini_set(cfg->raw, "site", "subtitle", arg(req, "subtitle"));
    /* Only a language PRTERM ships - anything else would silently
     * leave the interface in limbo. */
    {
        const char *lang = arg(req, "language");
        if (lang != NULL && lang[0] != '\0' && pr_lang_supported(lang))
            ini_set(cfg->raw, "site", "language", lang);
    }
    if (persist(cfg, res)) json_ok(res);
}

static void act_save_station(pr_request *req, pr_response *res, pr_config *cfg)
{
    char call[PR_CALLSIGN_MAX];
    if (!callerid_normalize(call, sizeof call, arg(req, "callerid"), &cfg->callsign)) {
        json_err(res, "CALLERID is invalid (base max. 6 characters, "
                      "SSID \"-<digit>\", total max. 8)");
        return;
    }
    ini_set(cfg->raw, "station", "callerid", call);
    ini_set(cfg->raw, "station", "qth", arg(req, "qth"));
    ini_set(cfg->raw, "station", "locator", arg(req, "locator"));
    if (persist(cfg, res)) json_ok(res);
}

static void act_save_radio(pr_request *req, pr_response *res, pr_config *cfg)
{
    char err[256];

    const char *driver = arg(req, "driver");
    if (pr_rig_find(driver) == NULL) {
        json_err(res, "unknown rig driver");
        return;
    }

    const char *duplex = arg(req, "duplex");
    if (strcmp(duplex, "full") != 0 && strcmp(duplex, "half") != 0) {
        json_err(res, "duplex must be \"full\" or \"half\"");
        return;
    }

    /* FM/AM/SSB - the mode is a station setting and lives in the admin */
    const char *mode = arg(req, "mode");
    unsigned mode_bit = pr_band_mode_from_name(mode);
    if (mode_bit == 0) {
        json_err(res, "mode must be \"fm\", \"am\" or \"ssb\"");
        return;
    }

    long freq = pr_parse_long(arg(req, "freq_hz"), 0, NULL);
    long pwr  = pr_parse_long(arg(req, "tx_power_mw"), 0, NULL);
    long baud = pr_parse_long(arg(req, "baud"), 0, NULL);

    /* Compliance BEFORE saving     */
    if (freq > 0) {
        const pr_channel *ch = pr_bandplan_at_freq(cfg->bandplan, freq);
        if (ch == NULL) {
            snprintf(err, sizeof err,
                     "%.3f MHz is not on an allocated channel",
                     freq / 1000000.0);
            json_err(res, err);
            return;
        }
        if (pwr > 0 && !pr_bandplan_tx_allowed(cfg->bandplan, freq,
                                               ch->modes & mode_bit ? mode_bit : PR_BAND_FM,
                                               pwr, err, sizeof err)) {
            json_err(res, err);
            return;
        }
    }

    ini_set(cfg->raw, "radio", "driver", driver);
    ini_set(cfg->raw, "radio", "port", arg(req, "port"));
    ini_set(cfg->raw, "radio", "duplex", duplex);
    ini_set(cfg->raw, "radio", "mode", mode);
    if (baud >= 300 && baud <= 4000000)
        ini_set_int(cfg->raw, "radio", "baud", baud);
    if (freq > 0)
        ini_set_int(cfg->raw, "radio", "freq_hz", freq);
    ini_set_int(cfg->raw, "radio", "tx_power_mw", pwr);

    if (persist(cfg, res)) json_ok(res);
}

static void act_save_callsign(pr_request *req, pr_response *res, pr_config *cfg)
{
    long a = pr_parse_long(arg(req, "callid_max_len"), 6, NULL);
    long b = pr_parse_long(arg(req, "callerid_base_len"), 6, NULL);
    long c = pr_parse_long(arg(req, "callerid_max_total"), 8, NULL);
    long d = pr_parse_long(arg(req, "callerid_ssid_digits"), 1, NULL);

    if (a < 1 || a > 10 || b < 1 || b > 10 || c < 1 || c > 16 || d < 0 || d > 2) {
        json_err(res, "callsign rules outside the allowed limits");
        return;
    }
    if (c < b) {
        json_err(res, "total length must not be smaller than the base");
        return;
    }

    ini_set_int(cfg->raw, "callsign", "callid_max_len", a);
    ini_set_int(cfg->raw, "callsign", "callerid_base_len", b);
    ini_set_int(cfg->raw, "callsign", "callerid_max_total", c);
    ini_set_int(cfg->raw, "callsign", "callerid_ssid_digits", d);
    ini_set_bool(cfg->raw, "callsign", "callerid_allow_ssid",
                 pr_parse_bool(arg(req, "callerid_allow_ssid"), true));

    if (persist(cfg, res)) {
        /* Check whether our own CALLERID is still valid under the new rules */
        if (!callerid_valid(cfg->callerid, &cfg->callsign)) {
            json_err(res, "the new rule makes your own CALLERID invalid");
            return;
        }
        json_ok(res);
    }
}

static void act_save_ui(pr_request *req, pr_response *res, pr_config *cfg)
{
    const char *font = arg(req, "font_file");

    if (font[0] != '\0') {
        char mime[32];
        if (!html_font_mime(font, mime, sizeof mime)) {
            json_err(res, "font file must be .ttf, .otf, .woff or .woff2");
            return;
        }
    }

    long size = pr_parse_long(arg(req, "font_size"), 14, NULL);
    long lh   = pr_parse_long(arg(req, "line_height"), 120, NULL);
    if (size < 6 || size > 96) {
        json_err(res, "font size must be between 6 and 96");
        return;
    }
    if (lh < 100 || lh > 300) {
        json_err(res, "line height must be between 100% and 300%");
        return;
    }

    const char *density = arg(req, "density");
    if (strcmp(density, "compact") != 0 && strcmp(density, "normal") != 0) {
        json_err(res, "density must be \"compact\" or \"normal\"");
        return;
    }
    const char *theme = arg(req, "theme");
    if (strcmp(theme, "silver") != 0 && strcmp(theme, "dark") != 0) {
        json_err(res, "theme must be \"silver\" or \"dark\"");
        return;
    }

    ini_set(cfg->raw, "ui", "font_file", font);
    ini_set_int(cfg->raw, "ui", "font_size", size);
    ini_set_int(cfg->raw, "ui", "line_height", lh / 100);
    ini_set(cfg->raw, "ui", "density", density);
    ini_set(cfg->raw, "ui", "theme", theme);

    if (persist(cfg, res)) json_ok(res);
}

static void act_ban_add(pr_request *req, pr_response *res, pr_config *cfg)
{
    char pattern[64];
    pr_strlcpy(pattern, arg(req, "pattern"), sizeof pattern);
    pr_trim(pattern);
    pr_upper(pattern);

    if (pattern[0] == '\0') {
        json_err(res, "pattern must not be empty");
        return;
    }
    if (strlen(pattern) >= sizeof pattern) {
        json_err(res, "pattern too long");
        return;
    }

    ini_set(cfg->raw, "ban", pattern, arg(req, "reason"));
    if (persist(cfg, res)) json_ok(res);
}

static void act_ban_del(pr_request *req, pr_response *res, pr_config *cfg)
{
    char pattern[64];
    pr_strlcpy(pattern, arg(req, "pattern"), sizeof pattern);
    pr_trim(pattern);
    pr_upper(pattern);

    if (!ini_del(cfg->raw, "ban", pattern)) {
        json_err(res, "no such ban entry");
        return;
    }
    if (persist(cfg, res)) json_ok(res);
}

static void act_pass_change(pr_request *req, pr_response *res, pr_config *cfg,
                            pr_session *sess)
{
    const char *oldp = arg(req, "old");
    const char *newp = arg(req, "new");
    const char *new2 = arg(req, "new2");

    /*
     * The old password is always checked - against the operator hash when one
     * is set, against the built-in default otherwise. Changing a password must
     * never be possible without knowing the current one.
     */
    if (!pr_auth_check_password(cfg, oldp)) {
        json_err(res, "old password is wrong");
        return;
    }
    if (strlen(newp) < 8) {
        json_err(res, "new password must have at least 8 characters");
        return;
    }
    if (strcmp(newp, new2) != 0) {
        json_err(res, "password repetition does not match");
        return;
    }

    char hash[160];
    if (pr_hash_password(newp, hash, sizeof hash) != 0) {
        json_err(res, "hash could not be created");
        return;
    }
    ini_set(cfg->raw, "admin", "pass_hash", hash);
    ini_set_bool(cfg->raw, "admin", "allow_guest_tx",
                 pr_parse_bool(arg(req, "allow_guest_tx"), true));

    if (persist(cfg, res)) {
        (void)sess;
        json_ok(res);
    }
}

static void act_config_save(pr_request *req, pr_response *res, pr_config *cfg)
{
    const char *text = arg(req, "text");

    char err[256];
    ini *fresh = ini_parse(text, err, sizeof err);
    if (fresh == NULL) {
        json_err(res, err);
        return;
    }

    /* Validate first, then replace - otherwise a broken file remains.  */
    pr_config test;
    if (pr_config_apply(&test, fresh, err, sizeof err) != 0) {
        json_err(res, err);
        ini_free(fresh);
        pr_config_free(&test);
        return;
    }
    pr_config_free(&test);

    if (ini_save(fresh, cfg->ini_path, err, sizeof err) != 0) {
        json_err(res, err);
        ini_free(fresh);
        return;
    }
    ini_free(fresh);

    pr_config loaded;
    if (pr_config_load(&loaded, cfg->ini_path, err, sizeof err) != 0) {
        json_err(res, err);
        pr_config_free(&loaded);
        return;
    }
    pr_config_free(cfg);
    *cfg = loaded;
    json_ok(res);
}

/* ======================================================================= */
/* Dispatcher                                                              */
/* ======================================================================= */

int pr_admin_action(pr_request *req, pr_response *res,
                    pr_config *cfg, pr_session *sess)
{
    const char *action = pr_req_param(req, "action");
    if (action == NULL) {
        json_err(res, "no action");
        return 0;
    }

    if      (strcmp(action, "save_site")    == 0) act_save_site(req, res, cfg);
    else if (strcmp(action, "save_station") == 0) act_save_station(req, res, cfg);
    else if (strcmp(action, "save_radio")   == 0) act_save_radio(req, res, cfg);
    else if (strcmp(action, "save_callsign")== 0) act_save_callsign(req, res, cfg);
    else if (strcmp(action, "save_ui")      == 0) act_save_ui(req, res, cfg);
    else if (strcmp(action, "ban_add")      == 0) act_ban_add(req, res, cfg);
    else if (strcmp(action, "ban_del")      == 0) act_ban_del(req, res, cfg);
    else if (strcmp(action, "pass_change")  == 0) act_pass_change(req, res, cfg, sess);
    else if (strcmp(action, "config_save")  == 0) act_config_save(req, res, cfg);
    else json_err(res, "unknown action");

    return 0;
}
