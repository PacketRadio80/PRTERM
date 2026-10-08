/*
 * PRTERM - CB & Amateur Radio Terminal
 * pages.c - Page composition and request handling.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "prterm_compat.h"

#include "pages.h"

#include "admin.h"
#include "arbiter.h"
#include "bands.h"
#include "callsign.h"
#include "html.h"
#include "inieditor.h"
#include "lang.h"
#include "state.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ======================================================================= */
/* Helpers                                                                 */
/* ======================================================================= */

/*
 * Text in the language of the installation ([site] language).
 *
 * The English text is the key and the fallback - see lang.h. Every
 * string the operator can read goes through this, so a language
 * without a translation degrades to English instead of breaking.
 */
static const char *T(const pr_config *cfg, const char *english)
{
    return pr_tr(cfg->language, english);
}

static bool wants_json(const pr_request *req)
{
    const char *v = pr_cgi_env("HTTP_X_REQUESTED_WITH", NULL);
    (void)req;
    return v != NULL && pr_str_eq_ci(v, "PRTERM");
}

static void json_begin(pr_buf *b, bool ok)
{
    pr_buf_addf(b, "{\"ok\":%s", ok ? "true" : "false");
}

static void json_kv_str(pr_buf *b, const char *k, const char *v)
{
    pr_buf_addf(b, ",\"%s\":\"", k);
    pr_json_escape(b, v != NULL ? v : "");
    pr_buf_addc(b, '"');
}

static void json_kv_int(pr_buf *b, const char *k, long long v)
{
    pr_buf_addf(b, ",\"%s\":%lld", k, v);
}

static void json_kv_bool(pr_buf *b, const char *k, bool v)
{
    pr_buf_addf(b, ",\"%s\":%s", k, v ? "true" : "false");
}

static void json_err(pr_response *res, const char *msg)
{
    pr_response_json(res, 400);
    pr_buf_add(&res->body, "{\"ok\":false,\"error\":\"");
    pr_json_escape(&res->body, msg != NULL ? msg : "error");
    pr_buf_add(&res->body, "\"}");
}

static void json_ok(pr_response *res)
{
    pr_response_json(res, 200);
    pr_buf_add(&res->body, "{\"ok\":true}");
}

/* ======================================================================= */
/* Operation - running the rig                                             */
/* ======================================================================= */

typedef struct app {
    pr_config     *cfg;
    pr_rig         rig;
    pr_rig_state   st;
    bool           rig_ok;
    char           err[256];
} app;

static int app_start(app *a, pr_config *cfg)
{
    /*
     * NO-AUTO-TX contract (2026-10-08).
     *
     * app_start runs on EVERY HTTP request before the page renders.
     * The user typed "jedes mal wenn du das Terminal neu kopierst
     * senden die TNCs" — the terminal copying / reloading was
     * transmitting on the air. This function MUST NOT transmit
     * anything, ever, regardless of driver.
     *
     * The pieces that DO write to the serial port on startup
     * belong to the DAEMON (prterm-tncd), and only via ops that
     * the user explicitly asked for (`action=tx`, the watchdog's
     * KISS repair). CGI-side rig_open is read-only by contract.
     */
    memset(a, 0, sizeof *a);
    a->cfg = cfg;

    if (pr_runtime_init(cfg, a->err, sizeof a->err) != 0)
        return -1;

    if (pr_rig_open(&a->rig, cfg, a->err, sizeof a->err) != 0) {
        /* no rig -> at least show state        */
        pr_state_load(cfg, &a->st, a->err, sizeof a->err);
        return -1;
    }
    a->rig_ok = true;

    /*
     * refresh() is defined per driver. It may update derived state
     * (rx_muted, signal level) and it MAY do nothing - it must NOT
     * transmit. Drivers that want to do anything else must do so
     * only via set_freq/set_mode/set_ptt, which the caller reaches
     * through their explicit actions, never through app_start.
     */
    if (a->rig.vtbl->refresh)
        a->rig.vtbl->refresh(&a->rig, a->err, sizeof a->err);

    /*
     * drain() reads pending RX frames out of the daemon's buffer
     * - those came from the AIR, we don't push anything. Equipment
     * is read-only.
     */
    pr_msg buf[32];
    size_t n = 0;
    if (a->rig.vtbl->drain && a->rig.vtbl->drain(&a->rig, buf, 32, &n) == 0) {
        for (size_t i = 0; i < n; i++) {
            char e2[128];
            (void)pr_log_append(cfg, &buf[i], e2, sizeof e2);
        }
    }

    a->rig.vtbl->get_state(&a->rig, &a->st);
    return 0;
}

static void app_stop(app *a)
{
    /*
     * NO-AUTO-TX contract (2026-10-08).
     *
     * app_stop is the symmetric counterpart of app_start. The driver
     * close() may drop DTR briefly on some hardware; here it is a
     * local socket close to the daemon, no side effects on the air.
     */
    if (a->rig_ok)
        pr_rig_close(&a->rig);
    a->rig_ok = false;
}

/*
 * Transmit path with compliance gate.
 *
 * The check sits BEFORE the KISS framing: what is rejected here does
 * not go on the air. See docs/REGULATIONS.md.
 *
 * Applies to EVERY transmission - also to the test carrier. An empty
 * carrier is a transmission and falls under the same rules.
 */
static int app_tx_gate(app *a, const pr_session *sess,
                       char *err, size_t errlen)
{
    if (!a->rig_ok) {
        snprintf(err, errlen, "%s",
                 pr_tr(a->cfg->language, "no rig connected"));
        return -1;
    }
    if (a->st.monitor) {
        snprintf(err, errlen, "%s",
                 pr_tr(a->cfg->language, "monitor mode: transmitting is locked"));
        return -1;
    }

    /* Callsign   */
    const char *from = a->cfg->callerid;
    if (pr_config_is_banned(a->cfg, from)) {
        snprintf(err, errlen, "%s (%s)",
                 pr_tr(a->cfg->language, "CALLERID is banned"), from);
        return -1;
    }

    /* Compliance: frequency, mode, power          */
    if (!pr_bandplan_tx_allowed(a->cfg->bandplan, a->st.freq_hz, a->st.mode,
                                a->cfg->tx_power_mw, err, errlen))
        return -1;

    if (sess != NULL && !sess->valid && !a->cfg->allow_guest_tx) {
        snprintf(err, errlen, "%s",
                 pr_tr(a->cfg->language, "transmitting requires login"));
        return -1;
    }
    return 0;
}

/*
 * TX arbitration: when several devices sit on the same channel, only
 * one may transmit at a time - two carriers destroy each other (see
 * arbiter.h). The lock has to cover the AIRTIME of the frame and not
 * only the call: the daemon accepts the frame long before it has left
 * the antenna.
 */
static void tx_hold_airtime(const pr_config *cfg, size_t textlen)
{
    long baud = cfg->radio_baud > 0 ? cfg->radio_baud : 1200;

    /* AX.25: 16 header + info + 2 FCS, plus 2 bytes KISS framing. */
    double sec = (double)(18 + (long)textlen) * 8.0 / (double)baud;

    /* and the quiet gap the daemon keeps between frames anyway */
    if (sec < 1.5)
        sec = 1.5;

    if (sec > 30.0)
        sec = 30.0;

    struct timespec ts;
    ts.tv_sec  = (time_t)sec;
    ts.tv_nsec = (long)((sec - (double)ts.tv_sec) * 1e9);
    (void)nanosleep(&ts, NULL);
}

static int app_tx(app *a, const char *to, const char *text,
                  const pr_session *sess, char *err, size_t errlen)
{
    if (app_tx_gate(a, sess, err, errlen) != 0)
        return -1;

    int lock = pr_arbiter_acquire(a->cfg->runtime_dir, a->st.freq_hz,
                                  a->cfg->callerid, 3000, err, errlen);
    if (lock < 0)
        return -1;

    int rc = a->rig.vtbl->send(&a->rig, a->cfg->callerid, to, text,
                               err, errlen);
    if (rc == 0)
        tx_hold_airtime(a->cfg, text != NULL ? strlen(text) : 0);
    pr_arbiter_release(lock);
    return rc;
}

/*
 * Test carrier of the administration - a device test, not operation.
 *
 * Intentionally REMOVED. Empty carriers are transmissions under every
 * legal framework PRTERM targets. The operator triggers a real CQ
 * broadcast (action=tx with bcast=1) for a smoke test - same TX LED,
 * same channel observation, no legal grey area. The driver
 * carrier_test() entry in pr_rig_vtbl is kept for future on-driver
 * tuning that does not go on the air (loopback etc).
 */

/* Channel for a frequency - from the rig state, not the config.
 * The config holds the start frequency, the state the current one. */
static int channel_of(const pr_config *cfg, long freq_hz)
{
    if (cfg->bandplan == NULL)
        return -1;
    const pr_channel *c = pr_bandplan_at_freq(cfg->bandplan, freq_hz);
    return c != NULL ? c->num : -1;
}

/* ======================================================================= */
/* Header bar                                                              */
/* ======================================================================= */

static void render_topbar(pr_buf *out, const pr_config *cfg,
                          const pr_session *sess, const pr_rig_state *st)
{
    (void)sess;

    pr_buf_add(out,
        "<header class=\"topbar\">\n"
        "  <div class=\"brand\">\n"
        "    <span class=\"brand-name grad\">");
    pr_html_escape(out, cfg->site_name);
    pr_buf_add(out, "</span>\n    <span class=\"brand-sub\">");
    pr_html_escape(out, cfg->site_subtitle);
    pr_buf_add(out, "</span>\n  </div>\n");

    /* Status */
    pr_buf_add(out, "  <div class=\"status\">\n");

    /*
     * Operating controls: whom to call, and with which device.
     *
     * The RX/TX menu replaces BOTH earlier controls - the station tabs
     * up here and the device menu in the send bar. The choice existed
     * twice, and that was the confusion. One menu now decides reception
     * filter and transmitting device at the same time:
     *
     *   All          hear every device; CQ/broadcast goes out over the
     *                device chosen in the CQ menu
     *   <device>     hear this device, transmit with it
     *
     * Entry format: <devicename>@<frequency>@<radio baud>Baud - the
     * name, because the two TNCs differ in modem and rate, and the
     * rate is what decides whether they understand each other.
     */
    pr_buf_add(out, "  <div class=\"opctl\">\n");
    pr_buf_addf(out,
        "    <label class=\"call-lbl\" for=\"callto\" title=\"%s\">"
        "CALL:</label>\n"
        "    <input class=\"call-input\" type=\"text\" id=\"callto\" name=\"callto\" "
        "placeholder=\"CQ\" maxlength=\"9\" spellcheck=\"false\" "
        "autocapitalize=\"characters\" autocomplete=\"off\">\n",
        T(cfg, "Station to call"));

    pr_buf_addf(out,
        "    <label class=\"call-lbl\" for=\"rxtx\" "
        "title=\"%s\">"
        "RX/TX:</label>\n"
        "    <select id=\"rxtx\" class=\"tx-dev\" title=\"%s\">"
        "<option value=\"\" selected>%s</option>",
        T(cfg, "All devices or exactly one - reception filter and "
               "transmitting device in one menu"),
        T(cfg, "Device"),
        T(cfg, "All"));
    if (cfg->nstations > 0) {
        for (size_t k = 0; k < cfg->nstations; k++) {
            const pr_station *sta = &cfg->stations[k];
            if (!sta->enabled)
                continue;
            pr_buf_addf(out, "<option value=\"%s\">%s@%.3f@%ldBaud</option>",
                        sta->name, sta->name,
                        st->freq_hz / 1000000.0, sta->radio_baud);
        }
    } else {
        pr_buf_addf(out, "<option value=\"\">radio@%.3f@%ldBaud</option>",
                    st->freq_hz / 1000000.0, cfg->radio_baud);
    }
    pr_buf_add(out, "</select>\n");

    /*
     * CQ/broadcast device - shown only under "All". Reception there
     * runs over every device, so the transmitting device has to be
     * chosen explicitly: 1200 and 2400 baud are different modems and
     * do not understand each other.
     */
    pr_buf_addf(out,
        "    <label class=\"call-lbl\" id=\"cqlbl\" for=\"cqdev\" "
        "title=\"%s\">"
        "CQ:</label>\n"
        "    <select id=\"cqdev\" class=\"tx-dev\" "
        "title=\"%s\">",
        T(cfg, "CQ / broadcast: which device transmits"),
        T(cfg, "Device for CQ/broadcast"));
    if (cfg->nstations > 0) {
        for (size_t k = 0; k < cfg->nstations; k++) {
            const pr_station *sta = &cfg->stations[k];
            if (!sta->enabled)
                continue;
            pr_buf_addf(out, "<option value=\"%s\"%s>%s@%.3f@%ldBaud</option>",
                        sta->name, k == 0 ? " selected" : "",
                        sta->name,
                        st->freq_hz / 1000000.0, sta->radio_baud);
        }
    } else {
        pr_buf_addf(out, "<option value=\"\">radio@%.3f@%ldBaud</option>",
                    st->freq_hz / 1000000.0, cfg->radio_baud);
    }
    pr_buf_add(out, "</select>\n  </div>\n");

    pr_buf_add(out, "    <span class=\"chip\"><b>QRG</b> <span class=\"num\" id=\"s-freq\">");
    pr_buf_addf(out, "%.3f MHz", st->freq_hz / 1000000.0);
    pr_buf_add(out, "</span></span>\n");

    /*
     * Channel chip REMOVED 2026-10-08.
     *
     * Originally rendered only when the current frequency coincided
     * with a CB band channel; that coupling still bought us the
     * ï¿½Channel 21" surface on the topbar in the terminal area.
     * The operator wants pure frequency display - "PRTERM zeigt den
     * Kanal auch nicht mehr an, ... Danke!". The bandplan lookup
     * remains in code (channel_of) and the JSON state field is still
     * emitted for logs / scripts, but no UI element surfaces it.
     */

    pr_buf_addf(out, "    <span class=\"chip\"><b>%s</b> <span id=\"s-mode\">",
                T(cfg, "Mode"));
    pr_html_escape(out, pr_band_mode_name(st->mode));
    pr_buf_add(out, "</span></span>\n");

    pr_buf_addf(out,
        "    <span class=\"chip %s\" id=\"s-duplex\">%s</span>\n",
        st->duplex == PR_DUPLEX_FULL ? "is-duplex-full" : "is-duplex-half",
        T(cfg, st->duplex == PR_DUPLEX_FULL ? "FULL-DUPLEX" : "HALF-DUPLEX"));

    pr_buf_add(out, "  </div>\n");

    /* Navigation */
    pr_buf_add(out, "  <nav class=\"nav\">\n");
    pr_buf_addf(out,
        "    <button type=\"button\" data-goto=\"terminal\" class=\"is-active\">"
        "%s</button>\n", T(cfg, "Terminal"));
    /*
     * The Mailbox tab exists only when MailboxD is switched on in the INI.
     * At disabled it is not rendered at all - no greyed-out button, no hint.
     * An operator who never installed MailboxD should not be asked about it.
     */
    if (cfg->mailboxd_enabled) {
        pr_buf_addf(out,
            "    <button type=\"button\" data-goto=\"mailbox\">"
            "%s</button>\n", T(cfg, "Mailbox"));
    }
    pr_buf_addf(out,
        "    <button type=\"button\" data-goto=\"admin\">%s</button>\n"
        "  </nav>\n</header>\n", T(cfg, "Administration"));
}

/* ======================================================================= */
/* Terminal view                                                           */
/* ======================================================================= */

static void render_terminal(pr_buf *out, const pr_config *cfg,
                            const pr_rig_state *st)
{
    pr_buf_add(out, "<section class=\"view is-active\" data-view=\"terminal\">\n");

    /*
     * Deliberately NO channel bar here. Operation belongs in the
     * terminal, channel selection in the admin area - otherwise the
     * grid eats the space that is meant for messages.
     */

    /* Receive log */
    pr_buf_add(out,
        "<div class=\"term-wrap\">\n"
        "  <pre class=\"term\" id=\"term\" role=\"log\" aria-live=\"polite\"></pre>\n"
        "</div>\n");

    /* Control line */
    pr_buf_add(out, "<div id=\"flash\" class=\"note\" hidden></div>\n");

    pr_buf_addf(out, "<div class=\"note\" id=\"duplexnote\">%s</div>\n",
        T(cfg, st->duplex == PR_DUPLEX_FULL
            ? "Full duplex — reception continues while transmitting."
            : "Half duplex — no reception while transmitting."));

    /*
     * The send bar carries exactly what is needed to send: the message
     * line, Send, and the character grid readout on the right.
     *
     * CALL: and the device choice are in the top bar, mode and duplex
     * are set in the administration - nothing of that appears twice.
     */
    pr_buf_addf(out,
        "<form class=\"txbar\" id=\"txform\" autocomplete=\"off\">\n"
        "  <input class=\"tx-input\" type=\"text\" id=\"txtext\" name=\"text\" "
        "placeholder=\"%s\" "
        "enterkeyhint=\"send\" spellcheck=\"false\">\n"
        "  <button type=\"submit\" class=\"primary\">%s</button>\n"
        "  <span class=\"chip\" id=\"gridinfo\">&#8212;</span>\n"
        "</form>\n",
        T(cfg, "Enter message &#8230;  [Enter] to send"),
        T(cfg, "Send"));

    pr_buf_add(out, "</section>\n");
}

/* ======================================================================= */
/* Mailbox view (MailboxD)                                                  */
/* ======================================================================= */

/*
 * The Mailbox view is PRTERM's own interface, inverted.
 *
 * MailboxD is a separate daemon that PRTERM drives locally - the mailbox and
 * BBS side of the station, nothing to do with the radio path. Since it is a
 * different service it must never be mistaken for the radio terminal while
 * someone is looking at it, so the whole area is drawn with the palette
 * reversed: dark where PRTERM is light, light where PRTERM is dark. Same
 * layout, opposite colours - recognisable at a glance, without learning a
 * second interface.
 *
 * Structurally it mirrors PRTERM: a tab strip that includes the LOCAL mailbox
 * login, and an Administration button. The login here is deliberately separate
 * from the PRTERM admin login - the mailbox has its own accounts.
 */
static void render_mailbox(pr_buf *out, const pr_config *cfg)
{
    if (!cfg->mailboxd_enabled)
        return;

    pr_buf_add(out, "<section class=\"view view-invert\" data-view=\"mailbox\">\n");

    /* Chrome: brand, tabs, administration - the same shape as the topbar */
    pr_buf_addf(out,
        "<header class=\"topbar mbox-bar\">\n"
        "  <div class=\"brand\">\n"
        "    <span class=\"brand-name grad\">MailboxD</span>\n"
        "    <span class=\"brand-sub\">%s</span>\n"
        "  </div>\n", T(cfg, "local mailbox"));

    pr_buf_add(out, "    <nav class=\"station-tabs\" id=\"mbox-tabs\">\n");
    pr_buf_addf(out,
        "      <button type=\"button\" class=\"stab is-active\" data-mbox=\"term\">"
        "%s</button>\n"
        "      <button type=\"button\" class=\"stab\" data-mbox=\"login\">"
        "%s</button>\n", T(cfg, "Terminal"), T(cfg, "Login"));
    pr_buf_add(out, "    </nav>\n");

    pr_buf_addf(out,
        "  <nav class=\"nav\">\n"
        "    <button type=\"button\" data-goto=\"admin\">%s</button>\n"
        "  </nav>\n</header>\n", T(cfg, "Administration"));

    /* Mailbox output - same treatment as the radio terminal */
    pr_buf_add(out,
        "<div class=\"term-wrap\">\n"
        "  <pre class=\"term\" id=\"mbox-term\" role=\"log\" "
        "aria-live=\"polite\"></pre>\n"
        "</div>\n");

    pr_buf_add(out, "<div id=\"mbox-flash\" class=\"note\" hidden></div>\n");

    /* Local mailbox login - shown when the Login tab is active */
    pr_buf_add(out, "<form class=\"mbox-login\" id=\"mbox-loginform\" "
                    "autocomplete=\"off\" hidden>\n");
    pr_buf_addf(out,
        "  <label class=\"call-lbl\" for=\"mbox-user\">%s</label>\n"
        "  <input class=\"call-input\" type=\"text\" id=\"mbox-user\" "
        "name=\"user\" maxlength=\"32\" spellcheck=\"false\" "
        "autocomplete=\"username\">\n"
        "  <label class=\"call-lbl\" for=\"mbox-pass\">%s</label>\n"
        "  <input class=\"call-input\" type=\"password\" id=\"mbox-pass\" "
        "name=\"pass\" autocomplete=\"current-password\">\n"
        "  <button type=\"submit\" class=\"btn\">%s</button>\n",
        T(cfg, "User:"), T(cfg, "Password:"), T(cfg, "Log in"));
    pr_buf_add(out, "</form>\n");

    /* Command line — same layout as the main terminal's send bar. */
    pr_buf_addf(out,
        "<form class=\"txbar\" id=\"mbox-form\" autocomplete=\"off\">\n"
        "  <input class=\"tx-input\" type=\"text\" id=\"mbox-cmd\" "
        "name=\"cmd\" placeholder=\"%s\" "
        "enterkeyhint=\"send\" spellcheck=\"false\" "
        "autocapitalize=\"off\" autocomplete=\"off\">\n"
        "  <button type=\"submit\" class=\"primary\">%s</button>\n"
        "</form>\n",
        T(cfg, "Enter command \u2026  [Enter] to send"), T(cfg, "Send"));

    pr_buf_add(out, "</section>\n");
}

/* ======================================================================= */
/* Administration area - clickable, no own URL                             */
/* ======================================================================= */

static void render_admin(pr_buf *out, const pr_config *cfg,
                         const pr_session *sess, const pr_rig_state *st)
{
    pr_buf_add(out, "<section class=\"view\" data-view=\"admin\">\n");

    if (!sess->valid) {
        pr_buf_addf(out,
            "<div class=\"card\"><h2 class=\"grad\">%s</h2>\n"
            "<p>%s "
            "%s</p>\n"
            "<div class=\"card-actions\">"
            "<button type=\"button\" class=\"primary\" data-open-login>"
            "%s</button></div></div>\n",
            T(cfg, "Administration"),
            T(cfg, "The admin area is locked."),
            T(cfg, "Log in directly here in the terminal."),
            T(cfg, "Log in"));
        pr_buf_add(out, "</section>\n");
        return;
    }

    pr_buf_add(out, "<div class=\"cards\">\n");

    /*
     * "Set a password" warning card REMOVED 2026-10-08.
     *
     * The built-in password is a service hatch for the FIRST install —
     * the operator sees that one warning in the journal / journalctl,
     * not in the public UI. The folded hint originally here gave away
     * to a stranger that the backdoor exists, and "international first"
     * means we don't anchor the install to a specific threat model.
     * If the operator wants to set a password, the Security card below
     * is always there.
     */

    /* --- General   --- */
    pr_buf_addf(out, "<form class=\"card\" method=\"post\" action=\"\">"
                     "<h2 class=\"grad\">%s</h2>\n", T(cfg, "General"));
    html_input_hidden(out, "action", "save_site");
    html_csrf(out, sess);
    html_input_text(out, "site_name", cfg->site_name, "", T(cfg, "Name"), "");
    html_input_text(out, "subtitle", cfg->site_subtitle, "", T(cfg, "Subtitle"), "");

    /*
     * The language of the interface. The selection is the list PRTERM
     * actually ships - the big five. Everything else stays English.
     */
    {
        const char *codes[8];
        const char *names[8];
        size_t n = 0;
        for (size_t i = 0; i < pr_lang_count() && n < 8; i++) {
            codes[n] = pr_lang_code(i);
            names[n] = pr_lang_name(i);
            n++;
        }
        html_select(out, "language", codes, names, n, cfg->language,
                    T(cfg, "Language"), T(cfg, "Interface language"));
    }
    pr_buf_addf(out, "<div class=\"card-actions\">"
                    "<button type=\"submit\" class=\"primary\">%s</button>"
                    "</div></form>\n", T(cfg, "Save"));

    /* --- Station --- */
    pr_buf_addf(out, "<form class=\"card\" method=\"post\" action=\"\">"
                    "<h2 class=\"grad\">%s</h2>\n", T(cfg, "Station"));
    html_input_hidden(out, "action", "save_station");
    html_csrf(out, sess);
    html_input_text(out, "callerid", cfg->callerid, "DL1ABC-1", "CALLERID",
                    T(cfg, "base max. 6 characters + SSID \"-<digit>\", total max. 8"));
    html_input_text(out, "qth", cfg->qth, "", "QTH", "");
    html_input_text(out, "locator", cfg->locator, "", T(cfg, "Locator"), "");
    pr_buf_addf(out, "<div class=\"card-actions\">"
                    "<button type=\"submit\" class=\"primary\">%s</button>"
                    "</div></form>\n", T(cfg, "Save"));

    /* --- Radio ---- intentionally removed 2026-10-08.
     *
     * Every field on this card had also become editable per-station
     * (driver / port / baud / mode all live on [station:NAME] now), so
     * the global [radio] form was redundant. The handler in admin.c
     * stays for the API/CLI path; the dispatcher entry kept too, so
     * external tools (prterm-ini wrappers, scripted installs) can
     * still POST to action=save_radio and get a JSON answer. */

    /* --- Devices (per-station) ---
     *
     * The Radio card above edits the channel-level values (duplex / freq / TX
     * power), which are SHARED across the bus. This block edits EACH device
     * independently: driver, port, baud, modem, line, kiss_init, mode,
     * callerid, antenna, enabled. Each [station:NAME] block has its own form
     * and its own Save button. */
    for (size_t i = 0; i < cfg->nstations; i++) {
        const pr_station *st = &cfg->stations[i];
        pr_buf_addf(out, "<form class=\"card station\" method=\"post\" action=\"\">"
                        "<h2 class=\"grad\">%s: %s</h2>\n",
                    T(cfg, "Device"), st->name);

        html_input_hidden(out, "action", "save_station_radio");
        html_input_hidden(out, "name", st->name);
        html_csrf(out, sess);

        /* Driver dropdown */
        {
            const char *vals[8];
            const char *lbl[8];
            size_t n = 0;
            for (size_t k = 0; k < pr_rig_count() && n < 8; k++) {
                const pr_rig_vtbl *v = pr_rig_at(k);
                if (v == NULL) continue;
                vals[n] = v->name;
                lbl[n]  = pr_trs(v->description);
                n++;
            }
            html_select(out, "driver", vals, lbl, n, st->rig_driver,
                        T(cfg, "Driver"), "");
        }
        html_input_text(out, "port", st->port, "/dev/ttyUSB0",
                        T(cfg, "Serial interface"),
                        "Linux: /dev/ttyUSB0, /dev/ttyACM0 - FreeBSD: /dev/cuaU0");
        html_input_number(out, "baud", st->baud, 300, 4000000,
                          T(cfg, "Baud rate"), "");

        /* KISS entry mode */
        {
            const char *kv[] = { "esc", "auto", "tapr" };
            const char *kl[] = {
                "ESC @K (TheFirmware TNC2)",
                "AUTO (retry when firmware echoes)",
                "TAPR \"kiss on\""
            };
            html_select(out, "kiss_init", kv, kl, 3, st->kiss_init,
                        T(cfg, "KISS entry"), "");
        }
        html_input_text(out, "modem", st->modem, "tcm3105",
                        T(cfg, "Modem"), "tcm3105, afsk, ...");
        html_input_text(out, "serial_line", st->serial_line, "8n1",
                        T(cfg, "Serial line"), "8n1, 7e1, 8n2, ...");

        /*
         * Per-station frequency, typed in by the operator. 0 = inherit
         * the global [radio] freq_hz - the input is then empty (no
         * "0" placed in the field, since 0 here means "inherit"). When
         * the form is saved the handler deletes the key in this case.
         */
        {
            char fstr[16];
            if (st->freq_hz > 0)
                snprintf(fstr, sizeof fstr, "%ld", st->freq_hz);
            else
                snprintf(fstr, sizeof fstr, "%s", "");
            html_input_text(out, "freq_hz", fstr, "",
                            T(cfg, "Frequency (Hz)"),
                            T(cfg, "leave empty to inherit [radio]"));
        }

        /* Mode with the inherit option (mode=0 in struct == "inherit"). */
        {
            const char *mv[]  = { "inherit", "fm", "am", "ssb" };
            const char *ml[]  = {
                T(cfg, "inherit [radio] mode"),
                "FM", "AM", "SSB"
            };
            char current[8];
            snprintf(current, sizeof current, "%s",
                     st->mode == 0u ? "inherit" : pr_band_mode_name(st->mode));
            html_select(out, "mode", mv, ml, 4, current,
                        T(cfg, "Mode"),
                        T(cfg, "FM/AM/SSB - inherit falls back to [radio] mode"));
        }

        html_input_text(out, "callerid", st->callerid, "DL1ABC-1",
                        T(cfg, "CALLERID"), "");
        html_input_text(out, "antenna", st->antenna, "",
                        T(cfg, "Antenna"), "");
        html_checkbox(out, "enabled", st->enabled,
                      T(cfg, "Enabled"), "");

        if (st->radio_baud != 0) {
            char rbaud[32];
            snprintf(rbaud, sizeof rbaud, "%ld", st->radio_baud);
            pr_buf_addf(out, "<p class=\"hint\">%s: <b>%s</b> %s</p>\n",
                        T(cfg, "Radio baud (hardware)"), rbaud,
                        T(cfg, "- fixed, not changeable"));
        }

        pr_buf_addf(out, "<div class=\"card-actions\">"
                        "<button type=\"submit\" class=\"primary\">%s</button>"
                        "</div></form>\n", T(cfg, "Save"));
    }

    /* --- Callsign & bans   --- */
    pr_buf_addf(out, "<form class=\"card\" method=\"post\" action=\"\">"
                    "<h2 class=\"grad\">%s</h2>\n", T(cfg, "Callsign"));
    html_input_hidden(out, "action", "save_callsign");
    html_csrf(out, sess);
    html_input_number(out, "callid_max_len", cfg->callsign.callid_max_len, 1, 10,
                      T(cfg, "CALLID max. length"), "");
    html_input_number(out, "callerid_base_len", cfg->callsign.callerid_base_len, 1, 10,
                      T(cfg, "CALLERID base"), "");
    html_input_number(out, "callerid_max_total", cfg->callsign.callerid_max_total, 1, 16,
                      T(cfg, "CALLERID total"), T(cfg, "6 + 2 = 8 as usual in radio"));
    html_input_number(out, "callerid_ssid_digits", cfg->callsign.ssid_digits, 0, 2,
                      T(cfg, "SSID digits"), "1 = -0..-9, 2 = -0..-15 (AX.25)");
    html_checkbox(out, "callerid_allow_ssid", cfg->callsign.allow_ssid,
                  T(cfg, "allow SSID"), "");
    pr_buf_addf(out, "<div class=\"card-actions\">"
                    "<button type=\"submit\" class=\"primary\">%s</button>"
                    "</div></form>\n", T(cfg, "Save"));

    /* --- Channel selection ----------- 2026-10-08: removed. ---
     *
     * The 80-button grid and the "Device channel: NAME" cards below it
     * were misread: the operator wants to work in FREQUENCY only, not
     * in channels. Channel numbers leak the German CB bandplan into
     * an interface that should be international — a non-CB frequency
     * (e.g. amateur, experimental, ISM) has no meaningful channel
     * number to display, and the picker offered the wrong mental
     * model anyway.
     *
     * The band plan in src/bands.c is still consulted internally for:
     * - compliance gates           (pr_bandplan_*) — frequency-based
     * - the per-frequency Hz key   ([station:NAME] freq_hz)
     * - the "channel" field in JSON state (visible in logs / scripts)
     *
     * The topbar still shows a "Channel N" chip when the operative
     * frequency happens to coincide with a CB channel number, so a
     * CB operator still sees the number they expect — it just isn't
     * pickable any more.
     */

    /* --- Device test --- intentionally removed. ---
     *
     * The 3-second test carrier is gone from the admin area. The previous
     * justification ("not operation, just a smoke test") was wrong: an
     * empty carrier is a transmission under every legal framework
     * PRTERM targets. The operator triggers a real CQ instead, which
     * reaches actual stations on the channel and lights up the TX LED
     * the same way. */

    /* --- Bans --- */
    pr_buf_addf(out, "<div class=\"card\"><h2 class=\"grad\">%s</h2>\n",
                T(cfg, "Blocked stations"));
    pr_buf_addf(out, "<table><thead><tr><th>%s</th><th>%s</th><th></th>"
                    "</tr></thead><tbody>\n",
                T(cfg, "Pattern"), T(cfg, "Reason"));
    for (size_t i = 0; i < cfg->nbans; i++) {
        pr_buf_add(out, "<tr><td><span class=\"badge badge-ban\">");
        pr_html_escape(out, cfg->bans[i].pattern);
        pr_buf_add(out, "</span></td><td>");
        pr_html_escape(out, cfg->bans[i].reason);
        pr_buf_add(out, "</td><td>");
        pr_buf_add(out, "<form method=\"post\" action=\"\" style=\"display:inline\">");
        html_input_hidden(out, "action", "ban_del");
        html_input_hidden(out, "pattern", cfg->bans[i].pattern);
        html_csrf(out, sess);
        pr_buf_addf(out, "<button type=\"submit\" class=\"btn\">%s</button>"
                        "</form></td></tr>\n", T(cfg, "Remove"));
    }
    if (cfg->nbans == 0)
        pr_buf_addf(out, "<tr><td colspan=\"3\" style=\"color:var(--fg-faint)\">"
                        "%s</td></tr>\n", T(cfg, "no blocks"));
    pr_buf_add(out, "</tbody></table>\n");

    pr_buf_add(out, "<form method=\"post\" action=\"\" class=\"row\">");
    html_input_hidden(out, "action", "ban_add");
    html_csrf(out, sess);
    html_input_text(out, "pattern", "", "DL9* or KB1ABC-3", T(cfg, "Pattern"),
                    T(cfg, "wildcards * and ?"));
    html_input_text(out, "reason", "", "", T(cfg, "Reason"), "");
    pr_buf_addf(out, "<div class=\"card-actions\">"
                    "<button type=\"submit\">%s</button></div></form>\n",
                T(cfg, "Block"));
    pr_buf_add(out, "</div>\n");

    /* --- Font & display        --- */
    pr_buf_addf(out, "<form class=\"card\" method=\"post\" action=\"\">"
                    "<h2 class=\"grad\">%s</h2>\n", T(cfg, "Font &amp; display"));
    html_input_hidden(out, "action", "save_ui");
    html_csrf(out, sess);
    html_input_text(out, "font_file", cfg->font_file, "./fonts/prterm.ttf",
                    T(cfg, "Font file"), T(cfg, ".otf or .ttf, relative to prterm.ini"));
    html_input_number(out, "font_size", cfg->font_size, 6, 96,
                      T(cfg, "Font size (px)"), "");
    html_input_number(out, "line_height", cfg->line_height_pct, 100, 300,
                      T(cfg, "Line height (%)"), "120 = 1.2");

    {
        const char *dv[] = { "compact", "normal" };
        const char *dl[] = { T(cfg, "Compact (maximum text space)"), T(cfg, "Normal") };
        html_select(out, "density", dv, dl, 2, cfg->ui_density,
                    T(cfg, "Density"), "");
    }
    {
        const char *dv[] = { "silver", "dark" };
        const char *dl[] = { T(cfg, "Silver (default)"), T(cfg, "Dark") };
        html_select(out, "theme", dv, dl, 2, cfg->ui_theme,
                    T(cfg, "Color scheme"), "");
    }
    pr_buf_addf(out, "<div class=\"card-actions\">"
                    "<button type=\"submit\" class=\"primary\">%s</button>"
                    "</div></form>\n", T(cfg, "Save"));

    /* --- Security   --- */
    pr_buf_addf(out, "<form class=\"card\" method=\"post\" action=\"\">"
                    "<h2 class=\"grad\">%s</h2>\n", T(cfg, "Security"));
    html_input_hidden(out, "action", "pass_change");
    html_csrf(out, sess);
    html_input_text(out, "old", "", "", T(cfg, "Old password"), "");
    html_input_text(out, "new", "", "", T(cfg, "New password"), "");
    html_input_text(out, "new2", "", "", T(cfg, "Repeat"), "");
    html_checkbox(out, "allow_guest_tx", cfg->allow_guest_tx,
                  T(cfg, "allow transmitting without login"), "");
    pr_buf_addf(out, "<div class=\"card-actions\">"
                    "<button type=\"submit\" class=\"primary\">%s</button>"
                    "</div></form>\n", T(cfg, "Change password"));

    /* --- Raw INI --- intentionally removed. ---
     *
     * The textarea "Configuration (prterm.ini)" dumped every key with
     * escaped HTML and flattened the document's comments and ordering.
     * Edit prterm.ini with ./prterm-ini (set / del / has-section) or
     * with your editor; it stays canonical. */

    /* --- PRTERM .ini editor (full-window; the textarea is rendered
     * separately by action=ini_editor). The link here is a nav
     * shortcut - the operator opens the editor in a new tab. */
    pr_buf_addf(out, "<form class=\"card\" method=\"get\" action=\"\">"
                    "<h2 class=\"grad\">%s</h2>\n", T(cfg, "PRTERM .ini"));
    pr_buf_add(out, "<p class=\"hint\">");
    pr_buf_addf(out, "%s: <code>%s</code><br>", T(cfg, "File"), cfg->ini_path);
    pr_buf_addf(out, "%s: %s</p>", T(cfg, "Open the editor"),
                T(cfg, "Read the whole file, edit, then Save or Dismiss."));
    pr_buf_addf(out,
        "<div class=\"card-actions\">"
        "<a class=\"btn\" target=\"_blank\" href=\"?action=ini_editor&amp;target=prterm\">%s</a>"
        "</div></form>\n", T(cfg, "Open editor"));

    /* --- MailboxD card: editor + user management ---
     *
     * This is the only MailboxD interface in the PRTERM admin area.
     * The editor and the user-management form are independent;
     * the editor opens in a new tab via the link.
     */
    {
        pr_buf_addf(out, "<form class=\"card\" method=\"get\" action=\"\">"
                        "<h2 class=\"grad\">%s</h2>\n", T(cfg, "MailboxD"));
        pr_buf_addf(out, "<p class=\"hint\">");
        if (cfg->mailboxd_enabled) {
            char sock[PR_CFG_PATH];
            snprintf(sock, sizeof sock, "%s/prterm.sock",
                     cfg->mailboxd_dir);
            pr_buf_addf(out, "%s: <code>%s/mailboxd.ini</code><br>",
                        T(cfg, "File"), cfg->mailboxd_dir);
            pr_buf_addf(out, "%s: <code>%s</code><br>", T(cfg, "Link socket"), sock);
        } else {
            pr_buf_addf(out, "%s: %s<br>",
                        T(cfg, "Status"),
                        T(cfg, "MailboxD is not enabled in this prterm.ini."));
        }
        pr_buf_addf(out, "%s: %s</p>",
                    T(cfg, "Open the editor"),
                    T(cfg, "Read the whole file, edit, then Save or Dismiss."));
        pr_buf_addf(out,
            "<div class=\"card-actions\">"
            "<a class=\"btn\" target=\"_blank\" href=\"?action=ini_editor&amp;target=mailboxd\">%s</a>"
            "</div></form>\n", T(cfg, "Open editor"));
    }

    /* --- MailboxD user management card --- */
    {
        pr_buf_addf(out, "<form class=\"card\" method=\"post\" action=\"\">"
                        "<h2 class=\"grad\">%s</h2>\n",
                    T(cfg, "MailboxD user management"));
        html_input_hidden(out, "action", "mbox_usercreate");
        html_csrf(out, sess);
        pr_buf_addf(out, "<p class=\"hint\">%s</p>",
                    T(cfg, "Create a new user. The MailboxD /usercreate "
                       "command requires Sysop/Admin privilege - the link "
                       "must be authenticated as such for the call to succeed."));
        html_input_text(out, "name", "", "DL1ABC-1",
                        T(cfg, "Username"), "");
        html_input_text(out, "full", "", "Sample User",
                        T(cfg, "Full name"), "");
        html_input_text(out, "country", "", "DE",
                        T(cfg, "Country"), "");
        html_input_text(out, "location", "", "MGH",
                        T(cfg, "Location"), "");
        html_input_text(out, "email", "", "user@example.com",
                        T(cfg, "Email"), "");
        pr_buf_addf(out, "<div class=\"card-actions\">"
                        "<button type=\"submit\" class=\"primary\">%s</button>"
                        "</div></form>\n",
                    T(cfg, "Create user"));
    }

    /* --- Session --- */
    pr_buf_addf(out, "<form class=\"card\" method=\"post\" action=\"\">"
                    "<h2 class=\"grad\">%s</h2>\n", T(cfg, "Session"));
    html_input_hidden(out, "action", "logout");
    html_csrf(out, sess);
    pr_buf_addf(out, "<p>%s <b>", T(cfg, "Logged in as"));
    pr_html_escape(out, sess->user);
    pr_buf_add(out, "</b></p>\n");
    pr_buf_addf(out, "<div class=\"card-actions\">"
                    "<button type=\"submit\">%s</button></div></form>\n",
                T(cfg, "Log out"));

    pr_buf_add(out, "</div>\n</section>\n");
}

/* ======================================================================= */
/* Login dialog                                                            */
/* ======================================================================= */

static void render_login(pr_buf *out, const pr_config *cfg, const pr_session *sess)
{
    (void)sess;
    pr_buf_add(out,
        "<dialog id=\"logindlg\" style=\"border:1px solid var(--line-2);"
        "border-radius:var(--radius);background:var(--panel);color:var(--fg);"
        "padding:18px 20px;min-width:min(340px,90vw)\">\n"
        "<form id=\"loginform\" method=\"post\" action=\"\">\n");
    pr_buf_addf(out, "<h2 class=\"grad\" style=\"margin-top:0\">%s</h2>\n",
                T(cfg, "Log in"));
    pr_buf_add(out,
        "<div id=\"loginmsg\" class=\"note note-err\" hidden></div>\n");
    pr_buf_addf(out, "<div class=\"field\"><label>%s</label>"
        "<input type=\"text\" id=\"loginuser\" name=\"user\" value=\"\" "
        "placeholder=\"admin\" autocomplete=\"off\" spellcheck=\"false\"></div>\n",
        T(cfg, "User"));
    pr_buf_addf(out, "<div class=\"field\"><label>%s</label>"
                    "<input type=\"password\" id=\"loginpass\" name=\"pass\" "
                    "autocomplete=\"current-password\"></div>\n",
                T(cfg, "Password"));
    pr_buf_addf(out, "<div class=\"card-actions\">"
                    "<button type=\"button\" data-close-login>%s</button>"
                    "<button type=\"submit\" class=\"primary\">%s</button>"
                    "</div>\n</form>\n</dialog>\n",
                T(cfg, "Cancel"), T(cfg, "Log in"));
}

/* ======================================================================= */
/* Full page                                                               */
/* ======================================================================= */

void page_render(pr_buf *out, const pr_config *cfg, const pr_session *sess,
                 const pr_rig_state *st, const pr_msg *msgs, size_t nmsg,
                 const char *flash_kind, const char *flash_msg)
{
    char title[128];
    snprintf(title, sizeof title, "%s", cfg->site_name);

    html_open(out, cfg, sess, title);
    page_csrf_meta(out, sess);

    pr_buf_add(out, "<div class=\"app\">\n");
    render_topbar(out, cfg, sess, st);
    pr_buf_add(out, "<main class=\"views\">\n");

    if (flash_msg != NULL && flash_msg[0] != '\0')
        html_note(out, flash_kind != NULL ? flash_kind : "info", "%s", flash_msg);

    render_terminal(out, cfg, st);
    render_mailbox(out, cfg);
    render_admin(out, cfg, sess, st);
    pr_buf_add(out, "</main>\n</div>\n");

    render_login(out, cfg, sess);

    /*
     * Texts of the browser script.
     *
     * The script cannot ask the server for every word it says, so the
     * translations travel with the page - one map, English text as the
     * key, exactly like pr_tr() does it.
     */
    {
        static const char *const js_texts[] = {
            "MailboxD is not connected — the daemon is not linked yet.",
            "Please address a station — broadcast only under \"All\".",
            "sending failed",
            "enter user and password",
            "form incomplete — please reload",
            "login failed",
            "FULL-DUPLEX",
            "HALF-DUPLEX",
            "Full duplex — reception continues while transmitting.",
            "Half duplex — no reception while transmitting.",
            "connected",
            "disconnected",
            NULL
        };

        pr_buf_add(out, "<script>var PRTERM_L={");
        for (size_t i = 0; js_texts[i] != NULL; i++) {
            pr_buf_add(out, i > 0 ? "," : "");
            pr_buf_add(out, "\"");
            pr_json_escape(out, js_texts[i]);
            pr_buf_add(out, "\":\"");
            pr_json_escape(out, T(cfg, js_texts[i]));
            pr_buf_add(out, "\"");
        }
        pr_buf_add(out, "};</script>\n");
    }

    /* Initial data for the script: first log rendering server-side  */
    if (nmsg > 0) {
        pr_buf_add(out, "<script>\n(function(){var t=document.getElementById('term');");
        pr_buf_add(out, "if(!t)return;var h='';\n");
        for (size_t i = 0; i < nmsg; i++) {
            const pr_msg *m = &msgs[i];
            const char *cls =
                m->kind == PR_MSG_RX  ? "rx"  :
                m->kind == PR_MSG_TX  ? "tx"  :
                m->kind == PR_MSG_WARN ? "warn" :
                m->kind == PR_MSG_ERR ? "err" : "sys";
            pr_buf_addf(out, "h+='<span class=\"ln ln-%s\">", cls);
            pr_buf_addf(out, "<span class=\"t\">%02d:%02d:%02d</span> ",
                        0, 0, 0);
            if (m->from[0] != '\0') {
                pr_buf_add(out, "<span class=\"who\">");
                pr_html_escape(out, m->from);
                pr_buf_add(out, "</span> ");
            }
            pr_buf_add(out, "<span class=\"tx\">");
            pr_html_escape(out, m->text);
            pr_buf_add(out, "</span></span>';\n");
        }
        pr_buf_add(out, "t.innerHTML=h;t.scrollTop=t.scrollHeight;})();\n</script>\n");
    }

    html_close(out, cfg);
}

/*
 * The CSRF token must be in the document so the JavaScript can send
 * it along. Without it all fetch actions (PTT, sending, switching)
 * fail with "invalid token" for logged-in users - the forms
 * worked, the buttons did not.
 */
void page_csrf_meta(pr_buf *out, const pr_session *sess)
{
    if (sess == NULL || !sess->valid)
        return;
    pr_buf_add(out, "<meta name=\"csrf\" content=\"");
    pr_attr_escape(out, sess->csrf);
    pr_buf_add(out, "\">\n");
}

/* ======================================================================= */
/* JSON state                                                              */
/* ======================================================================= */

static void json_state(pr_response *res, const app *a, const pr_config *cfg,
                       const pr_session *sess, const pr_msg *msgs, size_t nmsg,
                       bool rig_started, const pr_station *station)
{
    pr_response_json(res, 200);
    pr_buf *b = &res->body;

    json_begin(b, true);
    json_kv_bool(b, "logged_in", sess->valid);
    /* Own callsign - so the client can filter what is addressed to me
     * and what is just overheard broadcast. */
    json_kv_str(b, "callerid", cfg->callerid);
    json_kv_str(b, "callid", "CQ");
    json_kv_int(b, "freq_hz", a->st.freq_hz);
    json_kv_int(b, "channel", channel_of(cfg, a->st.freq_hz));
    json_kv_str(b, "mode", pr_band_mode_name(a->st.mode));
    json_kv_str(b, "duplex", pr_duplex_name(a->st.duplex));
    json_kv_bool(b, "ptt", a->st.ptt);
    json_kv_bool(b, "monitor", a->st.monitor);
    json_kv_int(b, "rx_db", a->st.rx_db);
    json_kv_int(b, "rx_count", a->st.rx_count);
    json_kv_int(b, "tx_count", a->st.tx_count);
    json_kv_str(b, "device", a->st.device);
    if (station != NULL) {
        json_kv_str(b, "station", station->name);
        json_kv_int(b, "radio_baud", station->radio_baud);
    }
    json_kv_bool(b, "link_ok", a->st.link_ok);
    /* Reason why the rig did not come up - without it the problem
     * stays invisible: the page renders, but nothing works. */
    if (!rig_started && a->err[0] != '\0')
        json_kv_str(b, "rig_error", a->err);

    /*
     * MailboxD status — live probe via HELLO on the bridge socket.
     *   "disabled" — feature turned off in INI
     *   "unlinked" — enabled, but MailboxD not reachable
     *   "linked"   — MailboxD bridge responds to HELLO
     */
    json_kv_str(b, "mailboxd_status",
                !cfg->mailboxd_enabled ? "disabled"
                : pr_mbox_probe_linked(cfg) ? "linked" : "unlinked");

    pr_buf_add(b, ",\"messages\":[");
    size_t nout = 0;
    for (size_t i = 0; i < nmsg; i++) {
        /*
         * Banned stations appear in NO view - not even under "All".
         * There everything falling on the channel is deliberately
         * monitored, but whoever is blocked stays outside.
         */
        if (msgs[i].kind == PR_MSG_RX &&
            pr_config_is_banned(cfg, msgs[i].from))
            continue;

        if (nout > 0) pr_buf_addc(b, ',');
        nout++;
        pr_buf_addf(b, "{\"kind\":\"%c\",\"ts\":%lld,\"db\":%d,\"from\":\"",
                    msgs[i].kind, msgs[i].ts, msgs[i].db);
        pr_json_escape(b, msgs[i].from);
        pr_buf_add(b, "\",\"to\":\"");
        pr_json_escape(b, msgs[i].to);
        /* Which device picked it up - shown in "All".         */
        pr_buf_add(b, "\",\"station\":\"");
        pr_json_escape(b, msgs[i].station);
        pr_buf_add(b, "\",\"text\":\"");
        pr_json_escape(b, msgs[i].text);
        pr_buf_add(b, "\"}");
    }
    pr_buf_add(b, "]}");
}

/* ======================================================================= */
/* Request handling                                                        */
/* ======================================================================= */

static bool check_csrf(const pr_config *cfg, const pr_request *req,
                       const pr_session *sess, pr_response *res)
{
    (void)cfg;
    const char *tok = pr_req_param(req, "csrf");
    if (sess->valid && !pr_session_check_csrf(sess, tok)) {
        json_err(res, pr_tr(cfg->language, "session expired or invalid token"));
        return false;
    }
    return true;
}

int pr_handle(pr_request *req, pr_response *res, pr_config *cfg)
{
    const char *action = pr_req_param(req, "action");

    /* The language of this request - layers without a configuration
     * (band plan, drivers) translate their messages with pr_trs(). */
    pr_lang_set(cfg->language);

    /* ---- INI editor (early route - no rig state needed) -------- */
    if (action != NULL && strcmp(action, "ini_editor") == 0) {
        const char *target = pr_req_param(req, "target");
        const char *flash  = pr_req_param(req, "flash");
        pr_inieditor_render(res, cfg, NULL, target, flash);
        return 0;
    }
    if (action != NULL && strcmp(action, "ini_editor_save") == 0) {
        pr_session sess;
        pr_session_from_request(cfg, req, &sess);
        if (!sess.valid) {
            json_err(res, pr_tr(cfg->language, "login required"));
            return 0;
        }
        const char *target = pr_req_param(req, "target");
        char err[256];
        char flash[128];
        if (pr_inieditor_save(req, res, cfg, target, flash, sizeof flash,
                              err, sizeof err) != 0) {
            /* Re-render the editor with the parse error as flash. */
            pr_inieditor_render(res, cfg, &sess, target, err);
            return 0;
        }
        /* Redirect back to the editor with a flash string. The CGI
         * path is the same as our request so the Location is fine. */
        char loc[1024];
        snprintf(loc, sizeof loc,
                 "?action=ini_editor&target=%s&flash=%s",
                 target ? target : "",
                 flash);
        pr_response_redirect(res, 302, loc);
        return 0;
    }

    /* ---- Font file    ------------------------------------------------ */
    if (action != NULL && strcmp(action, "font") == 0) {
        char mime[32];
        if (!html_font_mime(cfg->font_file, mime, sizeof mime)) {
            pr_response_text(res, 404);
            pr_buf_add(&res->body, "no font configured\n");
            return 0;
        }
        char *data = NULL;
        size_t len = 0;
        char err[256];
        if (pr_read_file(cfg->font_file, &data, &len, err, sizeof err) != 0) {
            pr_response_text(res, 404);
            pr_buf_add(&res->body, "font file not readable\n");
            return 0;
        }
        pr_response_binary(res, 200, mime);
        pr_response_header(res, "Cache-Control: public, max-age=31536000, immutable");
        pr_buf_addn(&res->body, data, len);
        free(data);
        return 0;
    }

    /* ---- Session ----------------------------------------------------- */
    pr_session sess;
    pr_session_from_request(cfg, req, &sess);

    /*
     * RX/TX device selection: the chosen device decides WHICH
     * hardware is used. Without a selection ("All") the device of the
     * CQ menu applies, otherwise the first activated station.
     *
     * This is the core of multi-device operation: every station is a
     * complete unit with its own TNC, radio and antenna.
     */
    const pr_station *station = pr_config_apply_station(
        cfg, pr_req_param(req, "station"));

    app a;
    bool rig_started = (app_start(&a, cfg) == 0);

    /* ---- Reading the log --------------------------------------------------------- */
    pr_msg msgs[400];
    size_t nmsg = 0;
    {
        char err[128];
        (void)pr_log_tail(cfg, msgs, 400, &nmsg, err, sizeof err);
    }

    /* ---- RX only / sending / operation  ------------------------------ */
    if (action != NULL && req->method[0] == 'P') {
        if (!check_csrf(cfg, req, &sess, res)) {
            app_stop(&a);
            return 0;
        }

        if (strcmp(action, "tx") == 0) {
            const char *text = pr_req_param(req, "text");
            const char *to   = pr_req_param(req, "to");
            char err[256];

            /*
             * Broadcast (CQ) is only allowed under "All".
             *
             * The RX/TX menu with a single device stands for DIRECT
             * communication with one partner - a destination must be
             * given there. Under "All" the CQ menu decides which
             * device transmits the broadcast.
             *
             * The rule deliberately lives here and not in the browser:
             * what the server does not check, nobody keeps.
             */
            bool bcast_ok = pr_parse_bool(pr_req_param(req, "bcast"), false);
            bool is_bcast = (to == NULL || to[0] == '\0' ||
                             pr_str_eq_ci(to, "CQ"));
            if (is_bcast && !bcast_ok) {
                json_err(res,
                    pr_tr(cfg->language, "broadcast is only allowed under \"All\" - "
                    "with a single device please address a station"));
                app_stop(&a);
                return 0;
            }

            if (!rig_started) {
                json_err(res, a.err);
            } else if (app_tx(&a, to, text, &sess, err, sizeof err) != 0) {
                json_err(res, err);
            } else {
                json_ok(res);
            }
            app_stop(&a);
            return 0;
        }

        /* action=ptt intentionally removed - the 3-second test carrier
         * is gone. Operators that want to smoke-test the rig use a real
         * CQ broadcast (action=tx with bcast=1). */

        if (strcmp(action, "monitor") == 0) {
            bool on = pr_parse_bool(pr_req_param(req, "on"), false);
            char err[256];
            if (!rig_started) {
                json_err(res, a.err);
            } else if (a.rig.vtbl->set_monitor(&a.rig, on, err, sizeof err) != 0) {
                json_err(res, err);
            } else {
                json_ok(res);
            }
            app_stop(&a);
            return 0;
        }

        if (strcmp(action, "set") == 0) {
            char err[256];
            err[0] = '\0';
            bool ok = rig_started;

            if (!ok)
                pr_strlcpy(err, a.err, sizeof err);

            /* Determine the target state completely first, then check,
             * then apply. Otherwise an invalid combination like "AM on
             * channel 41" slips through, because the channel silently
             * reset the mode. */
            long     want_freq = a.st.freq_hz;
            unsigned want_mode = a.st.mode;
            pr_duplex want_duplex = a.st.duplex;

            const char *ch = pr_req_param(req, "channel");
            if (ok && ch != NULL && ch[0] != '\0') {
                const pr_channel *c =
                    pr_bandplan_channel(cfg->bandplan, atoi(ch));
                if (c == NULL) {
                    pr_trf(err, sizeof err, "unknown channel %s", ch);
                    ok = false;
                } else {
                    want_freq = c->freq_hz;
                }
            }

            const char *fq = pr_req_param(req, "freq_hz");
            if (ok && fq != NULL && fq[0] != '\0') {
                bool num_ok = false;
                long f = pr_parse_long(fq, 0, &num_ok);
                if (!num_ok || f <= 0) {
                    pr_trf(err, sizeof err, "invalid frequency");
                    ok = false;
                } else {
                    want_freq = f;
                }
            }

            const char *md = pr_req_param(req, "mode");
            if (ok && md != NULL && md[0] != '\0') {
                unsigned m = pr_band_mode_from_name(md);
                if (m == 0) {
                    pr_trf(err, sizeof err, "unknown mode");
                    ok = false;
                } else {
                    want_mode = m;
                }
            }

            const char *dx = pr_req_param(req, "duplex");
            if (ok && dx != NULL && dx[0] != '\0')
                want_duplex = pr_duplex_from_name(dx);

            /* Mode chosen deliberately? Then the full check applies.
             * If only the channel is selected, the mode is adjusted -
             * tuning itself is not restricted, only transmitting. */
            bool mode_explicit = (md != NULL && md[0] != '\0');

            /* Compliance */
            if (ok && mode_explicit) {
                if (!pr_bandplan_tx_freq_ok(cfg->bandplan, want_freq,
                                            want_mode, err, sizeof err))
                    ok = false;
            } else if (ok) {
                const pr_channel *c = pr_bandplan_at_freq(cfg->bandplan, want_freq);
                if (c != NULL && (c->modes & want_mode) == 0) {
                    /* switch to a mode permitted here               */
                    if (c->modes & PR_BAND_FM)      want_mode = PR_BAND_FM;
                    else if (c->modes & PR_BAND_AM) want_mode = PR_BAND_AM;
                    else                            want_mode = PR_BAND_SSB;
                }
            }

            if (ok) {
                cfg->freq_hz = want_freq;
                cfg->mode = want_mode;
                cfg->duplex = want_duplex;

                if (a.rig.vtbl->set_freq(&a.rig, want_freq, err, sizeof err) != 0)
                    ok = false;
                else if (a.rig.vtbl->set_mode(&a.rig, want_mode, err, sizeof err) != 0)
                    ok = false;
                else if (a.rig.vtbl->set_duplex(&a.rig, want_duplex, err, sizeof err) != 0)
                    ok = false;
            }

            if (!ok) json_err(res, err);
            else     json_ok(res);
            app_stop(&a);
            return 0;
        }

        /* ---- Login     ----------------------------------------------- */
        if (strcmp(action, "login") == 0) {
            const char *user = pr_req_param(req, "user");
            const char *pass = pr_req_param(req, "pass");
            pr_session s;
            char err[256];

            if (!pr_login_throttle(cfg, req->remote_addr)) {
                json_err(res, pr_tr(cfg->language, "too many failed attempts - please try again later"));
            } else if (!pr_session_login(cfg, user, pass, &s, err, sizeof err)) {
                pr_login_fail(cfg, req->remote_addr);
                json_err(res, err);
            } else {
                pr_login_ok(cfg, req->remote_addr);
                pr_response_set_cookie(res, "PRTERM_SID", s.sid,
                                       cfg->session_ttl_min * 60, true);
                pr_response_json(res, 200);
                pr_buf_add(&res->body, "{\"ok\":true,\"csrf\":\"");
                pr_json_escape(&res->body, s.csrf);
                pr_buf_add(&res->body, "\"}");
            }
            app_stop(&a);
            return 0;
        }

        if (strcmp(action, "logout") == 0) {
            if (sess.valid)
                pr_session_destroy(cfg, sess.sid);
            pr_response_clear_cookie(res, "PRTERM_SID");
            if (wants_json(req)) json_ok(res);
            else {
                pr_response_text(res, 303);
                pr_response_header(res, "Location: %s", req->script_name);
            }
            app_stop(&a);
            return 0;
        }

        /* ---- MailboxD bridge (public, no admin session required) --- */
        if (strcmp(action, "mbox_login") == 0) {
            pr_mbox_login(req, res, cfg);
            app_stop(&a);
            return 0;
        }
        if (strcmp(action, "mbox_run") == 0) {
            pr_mbox_run(req, res, cfg);
            app_stop(&a);
            return 0;
        }
        if (strcmp(action, "mbox_logout") == 0) {
            pr_mbox_logout(req, res);
            app_stop(&a);
            return 0;
        }

        /* ---- Configuration ------------------------------------------- */
        if (pr_starts_with(action, "save_") ||
            pr_starts_with(action, "ban_") ||
            strcmp(action, "pass_change") == 0 ||
            strcmp(action, "mbox_usercreate") == 0) {

            /* Note: action=config_save was here before - now removed.
             * The raw INI textarea dump is gone from admin; the operator
             * edits prterm.ini with ./prterm-ini or with their editor. */

            if (!sess.valid) {
                json_err(res, pr_tr(cfg->language, "login required"));
                app_stop(&a);
                return 0;
            }
            pr_admin_action(req, res, cfg, &sess);
            app_stop(&a);
            return 0;
        }
    }

    /* ---- State as JSON    -------------------------------------------- */
    if (action != NULL && strcmp(action, "state") == 0) {
        size_t want = (size_t)atoi(pr_req_get(req, "rows") != NULL
                                   ? pr_req_get(req, "rows") : "0");
        (void)want;
        /* new messages since the last poll would be better, but a
         * short window is enough for the display */
        /*
         * Deliver only NOT YET SEEN messages.
         *
         * The client sends "since" with the timestamp of the newest
         * message already shown. Without this filter the client appends
         * the same message again on every poll - a single message
         * appeared as often as it was just queried (15x in the same
         * second).
         */
        long long since = 0;
        {
            const char *sv = pr_req_param(req, "since");
            if (sv != NULL && sv[0] != '\0')
                since = atoll(sv);
        }
        const pr_msg *tail = msgs;
        size_t ntail = nmsg;
        while (ntail > 0 && tail->ts <= since) {
            tail++;
            ntail--;
        }
        json_state(res, &a, cfg, &sess, tail, ntail, rig_started, station);
        app_stop(&a);
        return 0;
    }

    if (action != NULL && strcmp(action, "log") == 0) {
        pr_response_json(res, 200);
        pr_buf_add(&res->body, "{\"ok\":true,\"messages\":[");
        for (size_t i = 0; i < nmsg; i++) {
            if (i > 0) pr_buf_addc(&res->body, ',');
            pr_buf_addf(&res->body, "{\"kind\":\"%c\",\"ts\":%lld,\"db\":%d,\"from\":\"",
                        msgs[i].kind, msgs[i].ts, msgs[i].db);
            pr_json_escape(&res->body, msgs[i].from);
            pr_buf_add(&res->body, "\",\"text\":\"");
            pr_json_escape(&res->body, msgs[i].text);
            pr_buf_add(&res->body, "\"}");
        }
        pr_buf_add(&res->body, "]}");
        app_stop(&a);
        return 0;
    }

    /* ---- Render page   ------------------------------------------------ */
    pr_response_html(res, 200);
    pr_buf body;
    pr_buf_init(&body);
    page_render(&body, cfg, &sess, &a.st, msgs, nmsg, NULL, NULL);
    pr_buf_addn(&res->body, body.data ? body.data : "", body.len);
    pr_buf_free(&body);

    app_stop(&a);
    return 0;
}
