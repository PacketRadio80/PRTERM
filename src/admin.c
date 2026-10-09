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
#include "mailboxdsock.h"
#include "lang.h"
#include "radio.h"
#include "session.h"
#include "tncsock.h"
#include "util.h"

#include <stdarg.h>
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

static void json_err_fmt(pr_response *res, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    json_err(res, buf);
}

/* Saves the INI and keeps the model consistent.      */
static bool persist(pr_config *cfg, pr_response *res)
{
    if (cfg->raw == NULL) {
        json_err(res, pr_tr(cfg->language, "no configuration loaded"));
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
        json_err(res, pr_tr(cfg->language, "CALLERID is invalid (base max. 6 characters, "
                      "SSID \"-<digit>\", total max. 8)"));
        return;
    }
    ini_set(cfg->raw, "station", "callerid", call);
    ini_set(cfg->raw, "station", "qth", arg(req, "qth"));
    ini_set(cfg->raw, "station", "locator", arg(req, "locator"));
    if (persist(cfg, res)) json_ok(res);
}

/*
 * Per-device save: writes ONE [station:NAME] section in prterm.ini.
 *
 * The Radio card on the admin page still edits the global [radio] defaults
 * (duplex / freq_hz / tx_power_mw — these are channel-level, shared by every
 * device on the bus). Devices are station-level: each [station:NAME] has its
 * own driver, port, baud, modem, line, kiss_init, mode, callerid, antenna.
 *
 * Form fields:
 *   name         — the station key ([station:NAME])
 *   driver       — driver name from pr_rig_find()
 *   port         — serial device path
 *   baud         — 300..4_000_000
 *   modem        — "tcm3105", "afsk", ...
 *   serial_line  — "8n1"
 *   kiss_init    — "esc" | "auto" | "tapr"
 *   mode         — "fm" | "am" | "ssb" | "" (inherit the [radio] mode)
 *   freq_hz      — per-station frequency in Hz; 0/empty means inherit [radio]
 *   callerid     — per-device source callsign (validated if non-empty)
 *   antenna      — free-form description (display only)
 *   enabled      — "yes"/"no"
 */
static void act_save_station_radio(pr_request *req, pr_response *res, pr_config *cfg)
{
    char err[256];

    const char *name = arg(req, "name");
    if (name[0] == '\0') {
        json_err(res, pr_tr(cfg->language, "missing station name"));
        return;
    }
    size_t sidx = (size_t)-1;
    for (size_t i = 0; i < cfg->nstations; i++) {
        if (pr_str_eq_ci(cfg->stations[i].name, name)) { sidx = i; break; }
    }
    if (sidx == (size_t)-1) {
        json_err(res, pr_tr(cfg->language, "unknown station"));
        return;
    }

    const char *driver = arg(req, "driver");
    if (pr_rig_find(driver) == NULL) {
        json_err(res, pr_tr(cfg->language, "unknown rig driver"));
        return;
    }
    const char *kiss = arg(req, "kiss_init");
    if (strcmp(kiss, "esc") != 0 && strcmp(kiss, "auto") != 0 && strcmp(kiss, "tapr") != 0) {
        json_err(res, pr_tr(cfg->language, "kiss_init must be \"esc\", \"auto\" or \"tapr\""));
        return;
    }
    const char *mode = arg(req, "mode");
    bool inherit_mode = (mode[0] == '\0' || strcmp(mode, "inherit") == 0);
    if (!inherit_mode && pr_band_mode_from_name(mode) == 0u) {
        json_err(res, pr_tr(cfg->language, "mode must be \"fm\", \"am\", \"ssb\" or \"inherit\""));
        return;
    }

    long baud = pr_parse_long(arg(req, "baud"), 0, NULL);
    if (baud != 0 && (baud < 300 || baud > 4000000)) {
        json_err(res, pr_tr(cfg->language, "baud must be between 300 and 4 000 000"));
        return;
    }

    /* Per-station frequency (Hz). 0 = inherit the global [radio]
     * freq_hz. Empty form input also reads as 0. Compliance for the
     * typed value is checked at TX time, not save time - same reasoning
     * as in act_save_radio, just per station. */
    long want_freq = pr_parse_long(arg(req, "freq_hz"), 0, NULL);
    if (want_freq != 0) {
        const pr_channel *ch = pr_bandplan_at_freq(cfg->bandplan, want_freq);
        if (ch == NULL) {
            pr_trf(err, sizeof err,
                    "%.3f MHz is not on an allocated channel",
                    want_freq / 1000000.0);
            json_err(res, err);
            return;
        }
    }

    char sec[64];
    snprintf(sec, sizeof sec, "station:%s", name);

    ini_set(cfg->raw, sec, "rig_driver", driver);
    ini_set(cfg->raw, sec, "port", arg(req, "port"));
    if (baud != 0)
        ini_set_int(cfg->raw, sec, "baud", baud);
    ini_set(cfg->raw, sec, "modem", arg(req, "modem"));
    ini_set(cfg->raw, sec, "serial_line", arg(req, "serial_line"));
    ini_set(cfg->raw, sec, "kiss_init", kiss);
    if (inherit_mode)
        ini_del(cfg->raw, sec, "mode");
    else
        ini_set(cfg->raw, sec, "mode", mode);

    /*
     * Per-station frequency. Empty string or "0" in the form means
     * "inherit [radio] freq_hz" - we DELETE the key so the parser
     * sees absence and falls back. A real typed value is stored verbatim.
     */
    if (want_freq == 0)
        ini_del(cfg->raw, sec, "freq_hz");
    else
        ini_set_int(cfg->raw, sec, "freq_hz", want_freq);

    const char *cid = arg(req, "callerid");
    if (cid[0] != '\0') {
        char call[PR_CALLSIGN_MAX];
        if (!callerid_normalize(call, sizeof call, cid, &cfg->callsign)) {
            json_err(res, pr_tr(cfg->language, "CALLERID for this device is invalid"));
            return;
        }
        ini_set(cfg->raw, sec, "callerid", call);
    }

    /* "antenne" matches the parser in config.c — that spelling was carried
     * through every shipped sample. Renaming would orphan existing INI files;
     * we keep the typo at the wire level and correct it in the UI label only. */
    ini_set(cfg->raw, sec, "antenne", arg(req, "antenna"));
    ini_set_bool(cfg->raw, sec, "enabled",
                 pr_parse_bool(arg(req, "enabled"), cfg->stations[sidx].enabled));

    if (persist(cfg, res)) json_ok(res);
}

static void act_save_radio(pr_request *req, pr_response *res, pr_config *cfg)
{
    const char *driver = arg(req, "driver");
    if (pr_rig_find(driver) == NULL) {
        json_err(res, pr_tr(cfg->language, "unknown rig driver"));
        return;
    }

    /* FM/AM/SSB - the mode is a station setting and lives in the admin */
    const char *mode = arg(req, "mode");
    unsigned mode_bit = pr_band_mode_from_name(mode);
    if (mode_bit == 0) {
        json_err(res, pr_tr(cfg->language, "mode must be \"fm\", \"am\" or \"ssb\""));
        return;
    }

    long baud = pr_parse_long(arg(req, "baud"), 0, NULL);

    /*
     * Frequency and TX power are NOT edited via this form. The form
     * fields for [radio] freq_hz / tx_power_mw were removed from the UI
     * on 2026-10-08. Compliance for those values now happens at TX time
     * (app_tx_gate) - the loaded INI value is what counts, and a wrong
     * value never reaches the air. We therefore do not read or write
     * freq_hz / tx_power_mw here.
     *
     * Duplex is NOT edited via this form either (2026-10-08). The
     * field on the global [radio] section is still kept in the struct
     * and the INI file (operators can change it with prterm-ini), but
     * no admin form exposes it any more. The same compliance story
     * applies: values that go on air are checked at TX time against
     * the loaded INI.
     */

    ini_set(cfg->raw, "radio", "driver", driver);
    ini_set(cfg->raw, "radio", "port", arg(req, "port"));
    ini_set(cfg->raw, "radio", "mode", mode);
    if (baud >= 300 && baud <= 4000000)
        ini_set_int(cfg->raw, "radio", "baud", baud);

    if (persist(cfg, res)) json_ok(res);
}

static void act_save_callsign(pr_request *req, pr_response *res, pr_config *cfg)
{
    long a = pr_parse_long(arg(req, "callid_max_len"), 6, NULL);
    long b = pr_parse_long(arg(req, "callerid_base_len"), 6, NULL);
    long c = pr_parse_long(arg(req, "callerid_max_total"), 8, NULL);
    long d = pr_parse_long(arg(req, "callerid_ssid_digits"), 1, NULL);

    if (a < 1 || a > 10 || b < 1 || b > 10 || c < 1 || c > 16 || d < 0 || d > 2) {
        json_err(res, pr_tr(cfg->language, "callsign rules outside the allowed limits"));
        return;
    }
    if (c < b) {
        json_err(res, pr_tr(cfg->language, "total length must not be smaller than the base"));
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
            json_err(res, pr_tr(cfg->language, "the new rule makes your own CALLERID invalid"));
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
            json_err(res, pr_tr(cfg->language, "font file must be .ttf, .otf, .woff or .woff2"));
            return;
        }
    }

    long size = pr_parse_long(arg(req, "font_size"), 14, NULL);
    long lh   = pr_parse_long(arg(req, "line_height"), 120, NULL);
    if (size < 6 || size > 96) {
        json_err(res, pr_tr(cfg->language, "font size must be between 6 and 96"));
        return;
    }
    if (lh < 100 || lh > 300) {
        json_err(res, pr_tr(cfg->language, "line height must be between 100% and 300%"));
        return;
    }

    const char *density = arg(req, "density");
    if (strcmp(density, "compact") != 0 && strcmp(density, "normal") != 0) {
        json_err(res, pr_tr(cfg->language, "density must be \"compact\" or \"normal\""));
        return;
    }
    const char *theme = arg(req, "theme");
    if (strcmp(theme, "silver") != 0 && strcmp(theme, "dark") != 0) {
        json_err(res, pr_tr(cfg->language, "theme must be \"silver\" or \"dark\""));
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
        json_err(res, pr_tr(cfg->language, "pattern must not be empty"));
        return;
    }
    if (strlen(pattern) >= sizeof pattern) {
        json_err(res, pr_tr(cfg->language, "pattern too long"));
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
        json_err(res, pr_tr(cfg->language, "no such ban entry"));
        return;
    }
    if (persist(cfg, res)) json_ok(res);
}

/*
 * Forward a /usercreate call to MailboxD over the prterm<->mailboxd
 * unix-domain socket.
 *
 * The form posts: name (username), full, country, location, email.
 * The MailboxD plugin-side `/usercreate` command takes exactly those
 * five args in the same order (see mailboxd/src/core/command.c).
 *
 * If the link is not up the action reports a JSON error and the page
 * stays on the user-management card. The admin still gets a useful
 * hint, not a generic warning.
 */
static void act_mbox_usercreate(pr_request *req, pr_response *res,
                                pr_config *cfg, pr_session *sess)
{
    (void)sess;
    if (!cfg->mailboxd_enabled) {
        json_err(res, pr_tr(cfg->language, "MailboxD is disabled in prterm.ini"));
        return;
    }
    const char *name     = arg(req, "name");
    const char *full     = arg(req, "full");
    const char *country  = arg(req, "country");
    const char *location = arg(req, "location");
    const char *email    = arg(req, "email");
    if (name[0] == '\0' || full[0] == '\0') {
        json_err(res, pr_tr(cfg->language, "name and full are required"));
        return;
    }

    char sock[PR_CFG_PATH];
    snprintf(sock, sizeof sock, "%s/prterm.sock", cfg->mailboxd_dir);

    pr_mailboxdsock m;
    char err[256];
    if (pr_mailboxdsock_open(&m, sock, err, sizeof err) != 0) {
        json_err_fmt(res, "mailboxd link down: %s", err);
        return;
    }
    char banner[64];
    if (pr_mailboxdsock_hello(&m, 1, banner, sizeof banner, err, sizeof err) != 0) {
        pr_mailboxdsock_close(&m);
        json_err_fmt(res, "mailboxd HELLO: %s", err);
        return;
    }
    /*
     * Build the /usercreate line for MailboxD. 5 positional args:
     * username, full-name, country, location, email. The MailboxD
     * command dispatcher is responsible for privilege checking
     * (Sysop/Admin only). Without a Sysop login, MailboxD replies
     * END err dispatch-failed--7 (MAILBOXD_ERR_DENIED) and we map
     * that to a JSON "permission denied" so the admin can see why.
     */
    char cmdline[1024];
    int n = snprintf(cmdline, sizeof cmdline,
                     "/usercreate %s %s %s %s %s",
                     name, full, country, location, email);
    if (n < 0 || (size_t)n >= sizeof cmdline) {
        pr_mailboxdsock_close(&m);
        json_err(res, pr_tr(cfg->language, "input too long"));
        return;
    }

    char out[4096];
    size_t lines = 0;
    int rc = pr_mailboxdsock_run(&m, cmdline, out, sizeof out, &lines,
                                 err, sizeof err);
    pr_mailboxdsock_close(&m);
    if (rc != 0) {
        if (strstr(err, "dispatch-failed--7") != NULL) {
            json_err(res, pr_tr(cfg->language,
                "mailboxd refused: Sysop/Admin login required (link_token not yet bound to a Sysop user)"));
        } else {
            json_err(res, err);
        }
        return;
    }
    json_ok(res);
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
        json_err(res, pr_tr(cfg->language, "old password is wrong"));
        return;
    }
    if (strlen(newp) < 8) {
        json_err(res, pr_tr(cfg->language, "new password must have at least 8 characters"));
        return;
    }
    if (strcmp(newp, new2) != 0) {
        json_err(res, pr_tr(cfg->language, "password repetition does not match"));
        return;
    }

    char hash[160];
    if (pr_hash_password(newp, hash, sizeof hash) != 0) {
        json_err(res, pr_tr(cfg->language, "hash could not be created"));
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

/* act_config_save REMOVED 2026-10-08.
 *
 * The raw INI save action was tied to the textarea in the admin UI. That
 * textarea is gone — editing prterm.ini with ./prterm-ini (set/del/has-
 * section) or directly with an editor is the canonical path. The handler
 * would silently accept any malformed text and overwrite the file
 * contents, which is exactly what the textarea dump had already prepared:
 * the comments and ordering were flattened on the roundtrip.
 */
/* act_set_station_freq REMOVED 2026-10-08.
 *
 * Sat an die per-device "Device channel:" -Karten im Admin-Bereich,
 * die wiederum die Kanalnummer als UI-Element verfügbar machten. Beide
 * wurden entfernt: PRTERM arbeitet nur noch in Frequenzen, die Bandplan-
 * Nummern sind weiterhin intern im Compliance-Code aktiv (arithmetische
 * Modus/TX-Power-Prüfung gegen die in [station:NAME] freq_hz geladene
 * Frequenz), aber sie werden nicht mehr als UI-Werte exponiert.
 */

/* ======================================================================= */
/* MailboxD bridge — public actions (no admin session required)            */
/* ======================================================================= */

/* Hex-encode a NUL-terminated string. */
static size_t hex_enc(const char *src, char *dst, size_t dstlen)
{
    static const char hx[] = "0123456789abcdef";
    size_t w = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p; p++) {
        if (w + 2 >= dstlen) break;
        dst[w++] = hx[*p >> 4];
        dst[w++] = hx[*p & 0x0f];
    }
    dst[w] = '\0';
    return w;
}

/* Hex-decode a hex string into raw bytes. */
static size_t hex_dec(const char *src, char *dst, size_t dstlen)
{
    size_t w = 0;
    int hi = -1;
    for (const char *p = src; *p; p++) {
        int v = -1;
        if (*p >= '0' && *p <= '9') v = *p - '0';
        else if (*p >= 'a' && *p <= 'f') v = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'F') v = *p - 'A' + 10;
        if (v < 0) continue;
        if (hi < 0) { hi = v; continue; }
        if (w + 1 >= dstlen) break;
        dst[w++] = (char)((hi << 4) | v);
        hi = -1;
    }
    dst[w] = '\0';
    return w;
}

/* Append a newline-separated output buffer as a JSON string array.
 * Each line in `out` is appended as a JSON-escaped element. */
static void json_lines_array(pr_buf *b, const char *out)
{
    const char *p = out;
    int n = 0;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        /* Skip empty trailing line. */
        if (len == 0 && !nl) break;
        if (n > 0) pr_buf_addc(b, ',');
        pr_buf_addc(b, '"');
        char tmp[1024];
        size_t cplen = len < sizeof(tmp)-1 ? len : sizeof(tmp)-1;
        memcpy(tmp, p, cplen);
        tmp[cplen] = '\0';
        pr_json_escape(b, tmp);
        pr_buf_addc(b, '"');
        n++;
        p += len + (nl ? 1 : 0);
    }
}

int pr_mbox_probe_linked(const pr_config *cfg)
{
    if (!cfg->mailboxd_enabled)
        return 0;
    char sock[PR_CFG_PATH];
    snprintf(sock, sizeof sock, "%s/prterm.sock", cfg->mailboxd_dir);
    pr_mailboxdsock m;
    char err[256];
    if (pr_mailboxdsock_open(&m, sock, err, sizeof err) != 0)
        return 0;
    char banner[64];
    int ok = (pr_mailboxdsock_hello(&m, 1, banner, sizeof banner, err, sizeof err) == 0);
    pr_mailboxdsock_close(&m);
    return ok;
}

void pr_mbox_login(pr_request *req, pr_response *res, pr_config *cfg)
{
    if (!cfg->mailboxd_enabled) {
        json_err(res, pr_tr(cfg->language, "MailboxD is disabled in prterm.ini"));
        return;
    }
    const char *user = arg(req, "user");
    const char *pass = arg(req, "pass");
    if (user[0] == '\0' || pass[0] == '\0') {
        json_err(res, pr_tr(cfg->language, "enter user and password"));
        return;
    }

    char sock[PR_CFG_PATH];
    snprintf(sock, sizeof sock, "%s/prterm.sock", cfg->mailboxd_dir);
    pr_mailboxdsock m;
    char err[256];
    if (pr_mailboxdsock_open(&m, sock, err, sizeof err) != 0) {
        json_err_fmt(res, "mailboxd link down: %s", err);
        return;
    }
    char banner[64];
    if (pr_mailboxdsock_hello(&m, 1, banner, sizeof banner, err, sizeof err) != 0) {
        json_err_fmt(res, "mailboxd HELLO: %s", err);
        pr_mailboxdsock_close(&m);
        return;
    }

    char logincmd[256];
    snprintf(logincmd, sizeof logincmd, "/login %s %s", user, pass);
    char out[4096];
    size_t lines = 0;
    int rc = pr_mailboxdsock_run(&m, logincmd, out, sizeof out, &lines, err, sizeof err);
    pr_mailboxdsock_close(&m);
    if (rc != 0) {
        json_err(res, err);
        return;
    }

    /* Check if login actually succeeded.  MailboxD's /login command
     * outputs a greeting on success (e.g. "Hello, <user>." or
     * "Good time, <user>."), "Invalid password." or
     * "Unknown user." on failure.  The RUN itself returns rc=0 in
     * both cases (the command was dispatched). */
    if (lines == 0 ||
        strstr(out, "Invalid password") != NULL ||
        strstr(out, "Unknown user") != NULL ||
        strstr(out, "already logged in") != NULL ||
        strstr(out, "activation") != NULL ||
        strstr(out, "no password") != NULL) {
        /* Extract first line as error message. */
        char firstline[256];
        const char *nl = strchr(out, '\n');
        size_t len = nl ? (size_t)(nl - out) : strlen(out);
        if (len >= sizeof firstline) len = sizeof firstline - 1;
        memcpy(firstline, out, len);
        firstline[len] = '\0';
        json_err(res, firstline);
        return;
    }

    /* Store hex-encoded credentials as cookies (HTTP-only). */
    char hu[256];
    char hp[256];
    hex_enc(user, hu, sizeof hu);
    hex_enc(pass, hp, sizeof hp);
    pr_response_set_cookie(res, "MBOX_U", hu, 86400 * 30, true);
    pr_response_set_cookie(res, "MBOX_P", hp, 86400 * 30, true);

    /* Return the login output to the frontend. */
    pr_response_json(res, 200);
    pr_buf_add(&res->body, "{\"ok\":true,\"lines\":[");
    json_lines_array(&res->body, out);
    pr_buf_add(&res->body, "]}");
}

void pr_mbox_run(pr_request *req, pr_response *res, pr_config *cfg)
{
    if (!cfg->mailboxd_enabled) {
        json_err(res, pr_tr(cfg->language, "MailboxD is disabled in prterm.ini"));
        return;
    }
    const char *cmd = arg(req, "cmd");
    if (cmd[0] != '/') {
        json_err(res, pr_tr(cfg->language, "command must start with /"));
        return;
    }

    char sock[PR_CFG_PATH];
    snprintf(sock, sizeof sock, "%s/prterm.sock", cfg->mailboxd_dir);
    pr_mailboxdsock m;
    char err[256];
    if (pr_mailboxdsock_open(&m, sock, err, sizeof err) != 0) {
        json_err_fmt(res, "mailboxd link down: %s", err);
        return;
    }
    char banner[64];
    if (pr_mailboxdsock_hello(&m, 1, banner, sizeof banner, err, sizeof err) != 0) {
        json_err_fmt(res, "mailboxd HELLO: %s", err);
        pr_mailboxdsock_close(&m);
        return;
    }

    /* Silent re-login if cookies present. The /login reply MUST be
     * fully drained before sending the next RUN — otherwise the
     * commands race on the same socket and the second one is
     * answered as a guest. pr_mailboxdsock_run already loops
     * until "END ok"/"END err", so once it returns the session
     * is logged in and ready. */
    char mbox_u_enc[256];
    char mbox_p_enc[256];
    if (pr_req_cookie(req, "MBOX_U", mbox_u_enc, sizeof mbox_u_enc) &&
        pr_req_cookie(req, "MBOX_P", mbox_p_enc, sizeof mbox_p_enc) &&
        mbox_u_enc[0] && mbox_p_enc[0]) {
        char user[128];
        char pass[128];
        hex_dec(mbox_u_enc, user, sizeof user);
        hex_dec(mbox_p_enc, pass, sizeof pass);
        char logincmd[256];
        snprintf(logincmd, sizeof logincmd, "/login %s %s", user, pass);
        char lout[2048];
        size_t llines = 0;
        pr_mailboxdsock_run(&m, logincmd, lout, sizeof lout, &llines, err, sizeof err);
    }

    char out[4096];
    size_t nlines = 0;
    int rc = pr_mailboxdsock_run(&m, cmd, out, sizeof out, &nlines, err, sizeof err);
    pr_mailboxdsock_close(&m);

    pr_response_json(res, 200);
    pr_buf_add(&res->body, "{\"ok\":");
    pr_buf_add(&res->body, rc == 0 ? "true" : "false");
    if (rc == 0) {
        pr_buf_add(&res->body, ",\"lines\":[");
        json_lines_array(&res->body, out);
        pr_buf_add(&res->body, "]}");
    } else {
        /* Use OUT lines as error message if MailboxD sent them
         * (cmd_ handlers like /users write "Unknown command" etc.
         * before the END err).  Fall back to a translated raw code. */
        const char *msg = out;
        if (out[0] == '\0') {
            /* Translate raw dispatch-failed codes to readable text. */
            if (strstr(err, "dispatch-failed--7") != NULL)
                msg = "Insufficient privileges.";
            else if (strstr(err, "dispatch-failed--3") != NULL)
                msg = "Command not found.";
            else if (strstr(err, "dispatch-failed--6") != NULL)
                msg = "MailboxD busy, try again.";
            else if (strstr(err, "dispatch-failed") != NULL)
                msg = "Command failed.";
            else if (strstr(err, "not-a-mailboxd-command") != NULL)
                msg = "Only /commands are allowed.";
            else if (strstr(err, "empty-command") != NULL)
                msg = "Enter a command.";
            else
                msg = err;
        }
        pr_buf_add(&res->body, ",\"error\":\"");
        pr_json_escape(&res->body, msg);
        pr_buf_add(&res->body, "\"}");
    }
}

void pr_mbox_logout(pr_request *req, pr_response *res)
{
    (void)req;
    pr_response_clear_cookie(res, "MBOX_U");
    pr_response_clear_cookie(res, "MBOX_P");
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
        json_err(res, pr_tr(cfg->language, "no action"));
        return 0;
    }

    if      (strcmp(action, "save_site")    == 0) act_save_site(req, res, cfg);
    else if (strcmp(action, "save_station") == 0) act_save_station(req, res, cfg);
    else if (strcmp(action, "save_radio")   == 0) act_save_radio(req, res, cfg);
    else if (strcmp(action, "save_station_radio") == 0) act_save_station_radio(req, res, cfg);
    else if (strcmp(action, "save_callsign")== 0) act_save_callsign(req, res, cfg);
    else if (strcmp(action, "save_ui")      == 0) act_save_ui(req, res, cfg);
    else if (strcmp(action, "ban_add")      == 0) act_ban_add(req, res, cfg);
    else if (strcmp(action, "ban_del")      == 0) act_ban_del(req, res, cfg);
    else if (strcmp(action, "pass_change")  == 0) act_pass_change(req, res, cfg, sess);
    else if (strcmp(action, "mbox_usercreate") == 0) act_mbox_usercreate(req, res, cfg, sess);
    /* action=config_save REMOVED: see comment above act_config_save. */
    else json_err(res, pr_tr(cfg->language, "unknown action"));

    return 0;
}
