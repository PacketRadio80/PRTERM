/*
 * PRTERM - CB & Amateur Radio Terminal
 * pages.c - Seitenkomposition und Request-Handling.
 *
 * SPDX-License-Identifier: MIT
 */
#include "prterm_compat.h"

#include "pages.h"

#include "admin.h"
#include "bands.h"
#include "callsign.h"
#include "html.h"
#include "state.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ======================================================================= */
/* Helfer                                                                  */
/* ======================================================================= */

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
    pr_json_escape(&res->body, msg != NULL ? msg : "Fehler");
    pr_buf_add(&res->body, "\"}");
}

static void json_ok(pr_response *res)
{
    pr_response_json(res, 200);
    pr_buf_add(&res->body, "{\"ok\":true}");
}

/* ======================================================================= */
/* Betrieb - Rig fahren                                                    */
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
    memset(a, 0, sizeof *a);
    a->cfg = cfg;

    if (pr_runtime_init(cfg, a->err, sizeof a->err) != 0)
        return -1;

    if (pr_rig_open(&a->rig, cfg, a->err, sizeof a->err) != 0) {
        /* kein Rig -> zumindest Zustand zeigen */
        pr_state_load(cfg, &a->st, a->err, sizeof a->err);
        return -1;
    }
    a->rig_ok = true;

    if (a->rig.vtbl->refresh)
        a->rig.vtbl->refresh(&a->rig, a->err, sizeof a->err);

    /* Neue RX-Nachrichten ins Log uebernehmen */
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
    if (a->rig_ok)
        pr_rig_close(&a->rig);
    a->rig_ok = false;
}

/*
 * Sendeweg mit Compliance-Gate.
 *
 * Die Pruefung liegt VOR dem KISS-Framing: was hier abgelehnt wird, geht
 * nicht auf die Luft. Siehe docs/REGULATIONS.md.
 */
static int app_tx(app *a, const char *text, const pr_session *sess,
                  char *err, size_t errlen)
{
    if (!a->rig_ok) {
        snprintf(err, errlen, "kein Rig verbunden");
        return -1;
    }
    if (a->st.monitor) {
        snprintf(err, errlen, "Monitorbetrieb: Senden ist gesperrt");
        return -1;
    }

    /* Rufzeichen */
    const char *from = a->cfg->callerid;
    if (pr_config_is_banned(a->cfg, from)) {
        snprintf(err, errlen, "CALLERID %s ist gebannt", from);
        return -1;
    }

    /* Compliance: Frequenz, Betriebsart, Leistung */
    if (!pr_bandplan_tx_allowed(a->cfg->bandplan, a->st.freq_hz, a->st.mode,
                                a->cfg->tx_power_mw, err, errlen))
        return -1;

    if (sess != NULL && !sess->valid && !a->cfg->allow_guest_tx) {
        snprintf(err, errlen, "Senden erfordert eine Anmeldung");
        return -1;
    }

    return a->rig.vtbl->send(&a->rig, from, text, err, errlen);
}

/* Kanal zu einer Frequenz - aus dem Rig-Zustand, nicht aus der Config.
 * Die Config haelt die Startfrequenz, der Zustand die laufende. */
static int channel_of(const pr_config *cfg, long freq_hz)
{
    if (cfg->bandplan == NULL)
        return -1;
    const pr_channel *c = pr_bandplan_at_freq(cfg->bandplan, freq_hz);
    return c != NULL ? c->num : -1;
}

/* ======================================================================= */
/* Kopfleiste                                                              */
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
     * Stationsreiter: <Frequenz>@<Funk-Baudrate>, ausgelesen aus der INI.
     * Bewusst kein Gerätepfad und kein Rufzeichen - der Pfad ist
     * installationsabhaengig, das Rufzeichen steht im Adminbereich.
     */
    pr_buf_add(out, "    <nav class=\"station-tabs\" id=\"station-tabs\">\n");
    if (cfg->nstations > 0) {
        for (size_t k = 0; k < cfg->nstations; k++) {
            const pr_station *sta = &cfg->stations[k];
            if (!sta->enabled)
                continue;
            pr_buf_addf(out,
                "      <button type=\"button\" class=\"stab%s\" data-station=\"%s\">"
                "%.3f@%ld</button>\n",
                k == 0 ? " is-current" : "",
                sta->name,
                cfg->freq_hz / 1000000.0,
                sta->radio_baud);
        }
    } else {
        pr_buf_addf(out,
            "      <button type=\"button\" class=\"stab is-current\">"
            "%.3f@%ld</button>\n",
            cfg->freq_hz / 1000000.0, cfg->radio_baud);
    }
    pr_buf_add(out, "    </nav>\n");

    pr_buf_add(out, "    <span class=\"chip\"><b>QRG</b> <span class=\"num\" id=\"s-freq\">");
    pr_buf_addf(out, "%.3f MHz", st->freq_hz / 1000000.0);
    pr_buf_add(out, "</span></span>\n");

    pr_buf_add(out, "    <span class=\"chip\"><b>Kanal</b> <span class=\"num\" id=\"s-channel\">");
    {
        int ch = channel_of(cfg, st->freq_hz);
        if (ch > 0) pr_buf_addf(out, "%d", ch);
        else        pr_buf_add(out, "&#8212;");
    }
    pr_buf_add(out, "</span></span>\n");

    pr_buf_add(out, "    <span class=\"chip\"><b>Mode</b> <span id=\"s-mode\">");
    pr_html_escape(out, pr_band_mode_name(st->mode));
    pr_buf_add(out, "</span></span>\n");

    pr_buf_addf(out,
        "    <span class=\"chip %s\" id=\"s-duplex\">%s</span>\n",
        st->duplex == PR_DUPLEX_FULL ? "is-duplex-full" : "is-duplex-half",
        st->duplex == PR_DUPLEX_FULL ? "FULL-DUPLEX" : "HALB-DUPLEX");

    pr_buf_add(out, "  </div>\n");

    /* Navigation */
    pr_buf_add(out,
        "  <nav class=\"nav\">\n"
        "    <button type=\"button\" data-goto=\"terminal\" class=\"is-active\">"
        "Terminal</button>\n"
        "    <button type=\"button\" data-goto=\"admin\">Administration</button>\n"
        "  </nav>\n</header>\n");
}

/* ======================================================================= */
/* Terminalansicht                                                         */
/* ======================================================================= */

static void render_terminal(pr_buf *out, const pr_config *cfg,
                            const pr_rig_state *st)
{
    (void)cfg;
    pr_buf_add(out, "<section class=\"view is-active\" data-view=\"terminal\">\n");

    /*
     * Bewusst KEINE Kanalleiste hier. Die Bedienung gehoert ins Terminal,
     * die Kanalwahl in den Adminbereich - sonst frisst das Raster den
     * Platz, der fuer Nachrichten sein soll.
     */

    /* Empfangslog */
    pr_buf_add(out,
        "<div class=\"term-wrap\">\n"
        "  <pre class=\"term\" id=\"term\" role=\"log\" aria-live=\"polite\"></pre>\n"
        "</div>\n");

    /* Steuerzeile */
    pr_buf_add(out, "<div id=\"flash\" class=\"note\" hidden></div>\n");

    pr_buf_add(out, "<div class=\"note\" id=\"duplexnote\">");
    pr_buf_add(out, st->duplex == PR_DUPLEX_FULL
        ? "Vollduplex &#8212; der Empfang l&#228;uft w&#228;hrend des Sendens weiter."
        : "Halbduplex &#8212; w&#228;hrend des Sendens wird nicht empfangen.");
    pr_buf_add(out, "</div>\n");

    pr_buf_add(out,
        "<form class=\"txbar\" id=\"txform\" autocomplete=\"off\">\n"
        "  <input class=\"tx-input\" type=\"text\" id=\"txtext\" name=\"text\" "
        "placeholder=\"Nachricht eingeben &#8230;  [Enter] senden  "
        "[Esc] PTT\" enterkeyhint=\"send\" spellcheck=\"false\">\n"
        "  <button type=\"submit\" class=\"primary\">Senden</button>\n"
        "  <button type=\"button\" id=\"pttbtn\">PTT</button>\n"
        "  <select id=\"selmode\" title=\"Betriebsart\">");

    static const char *const modes[] = { "fm", "am", "ssb" };
    static const char *const mode_lbl[] = { "FM", "AM", "SSB" };
    for (size_t i = 0; i < 3; i++) {
        pr_buf_addf(out, "<option value=\"%s\"%s>%s</option>",
                    modes[i],
                    st->mode == pr_band_mode_from_name(modes[i]) ? " selected" : "",
                    mode_lbl[i]);
    }
    pr_buf_add(out, "</select>\n");

    pr_buf_addf(out, "<select id=\"selduplex\" title=\"Duplex\">"
                     "<option value=\"full\"%s>Vollduplex</option>"
                     "<option value=\"half\"%s>Halbduplex</option></select>\n",
                st->duplex == PR_DUPLEX_FULL ? " selected" : "",
                st->duplex != PR_DUPLEX_FULL ? " selected" : "");

    pr_buf_add(out, "  <span class=\"chip\" id=\"gridinfo\">&#8212;</span>\n");
    pr_buf_add(out, "</form>\n");

    pr_buf_add(out, "</section>\n");
}

/* ======================================================================= */
/* Administrationsbereich - anklickbar, ohne eigene URL                    */
/* ======================================================================= */

static void render_admin(pr_buf *out, const pr_config *cfg,
                         const pr_session *sess, const pr_rig_state *st)
{
    pr_buf_add(out, "<section class=\"view\" data-view=\"admin\">\n");

    if (!sess->valid) {
        pr_buf_add(out,
            "<div class=\"card\"><h2 class=\"grad\">Administration</h2>\n"
            "<p>Der Administrationsbereich ist gesperrt. "
            "Anmeldung erfolgt direkt hier im Terminal.</p>\n"
            "<div class=\"card-actions\">"
            "<button type=\"button\" class=\"primary\" data-open-login>"
            "Anmelden</button></div></div>\n");
        pr_buf_add(out, "</section>\n");
        return;
    }

    pr_buf_add(out, "<div class=\"cards\">\n");

    /* --- Allgemein --- */
    pr_buf_add(out, "<form class=\"card\" method=\"post\" action=\"\">"
                    "<h2 class=\"grad\">Allgemein</h2>\n");
    html_input_hidden(out, "action", "save_site");
    html_csrf(out, sess);
    html_input_text(out, "site_name", cfg->site_name, "", "Name", "");
    html_input_text(out, "subtitle", cfg->site_subtitle, "", "Untertitel", "");
    pr_buf_add(out, "<div class=\"card-actions\">"
                    "<button type=\"submit\" class=\"primary\">Speichern</button>"
                    "</div></form>\n");

    /* --- Station --- */
    pr_buf_add(out, "<form class=\"card\" method=\"post\" action=\"\">"
                    "<h2 class=\"grad\">Station</h2>\n");
    html_input_hidden(out, "action", "save_station");
    html_csrf(out, sess);
    html_input_text(out, "callerid", cfg->callerid, "DL1ABC-1", "CALLERID",
                    "Basis max. 6 Zeichen + SSID \"-<Ziffer>\", gesamt max. 8");
    html_input_text(out, "qth", cfg->qth, "", "QTH", "");
    html_input_text(out, "locator", cfg->locator, "", "Locator", "");
    pr_buf_add(out, "<div class=\"card-actions\">"
                    "<button type=\"submit\" class=\"primary\">Speichern</button>"
                    "</div></form>\n");

    /* --- Funk --- */
    pr_buf_add(out, "<form class=\"card\" method=\"post\" action=\"\">"
                    "<h2 class=\"grad\">Funk</h2>\n");
    html_input_hidden(out, "action", "save_radio");
    html_csrf(out, sess);

    {
        const char *vals[8];
        const char *lbl[8];
        size_t n = 0;
        for (size_t i = 0; i < pr_rig_count(); i++) {
            const pr_rig_vtbl *v = pr_rig_at(i);
            if (v == NULL) continue;
            vals[n] = v->name;
            lbl[n]  = v->description;
            n++;
        }
        html_select(out, "driver", vals, lbl, n, cfg->rig_driver, "Treiber", "");
    }

    html_input_text(out, "port", cfg->port, "/dev/ttyUSB0", "Serielle Schnittstelle",
                    "Linux: /dev/ttyUSB0, /dev/ttyACM0 - FreeBSD: /dev/cuaU0");
    html_input_number(out, "baud", cfg->baud, 300, 4000000, "Baudrate", "");

    {
        const char *dv[] = { "full", "half" };
        const char *dl[] = { "Vollduplex", "Halbduplex" };
        html_select(out, "duplex", dv, dl, 2, pr_duplex_name(cfg->duplex),
                    "Duplex", "");
    }
    html_input_number(out, "freq_hz", cfg->freq_hz, 26565000L, 27405000L,
                      "Frequenz (Hz)", "");
    html_input_number(out, "tx_power_mw", cfg->tx_power_mw, 0, 12000,
                      "Sendeleistung (mW)", "Pruefung gegen die Zuteilung");

    pr_buf_add(out, "<div class=\"card-actions\">"
                    "<button type=\"submit\" class=\"primary\">Speichern</button>"
                    "</div></form>\n");

    /* --- Rufzeichen & Bans --- */
    pr_buf_add(out, "<form class=\"card\" method=\"post\" action=\"\">"
                    "<h2 class=\"grad\">Rufzeichen</h2>\n");
    html_input_hidden(out, "action", "save_callsign");
    html_csrf(out, sess);
    html_input_number(out, "callid_max_len", cfg->callsign.callid_max_len, 1, 10,
                      "CALLID max. L&#228;nge", "");
    html_input_number(out, "callerid_base_len", cfg->callsign.callerid_base_len, 1, 10,
                      "CALLERID Basis", "");
    html_input_number(out, "callerid_max_total", cfg->callsign.callerid_max_total, 1, 16,
                      "CALLERID gesamt", "6 + 2 = 8 wie im Funk &#252;blich");
    html_input_number(out, "callerid_ssid_digits", cfg->callsign.ssid_digits, 0, 2,
                      "SSID-Ziffern", "1 = -0..-9, 2 = -0..-15 (AX.25)");
    html_checkbox(out, "callerid_allow_ssid", cfg->callsign.allow_ssid,
                  "SSID zulassen", "");
    pr_buf_add(out, "<div class=\"card-actions\">"
                    "<button type=\"submit\" class=\"primary\">Speichern</button>"
                    "</div></form>\n");

    /* --- Kanalwahl --- */
    pr_buf_add(out, "<div class=\"card\"><h2 class=\"grad\">Kanalwahl</h2>\n");
    pr_buf_add(out, "<div class=\"channels\">");
    if (cfg->bandplan != NULL) {
        for (size_t k = 0; k < cfg->bandplan->nch; k++) {
            const pr_channel *c = &cfg->bandplan->ch[k];
            bool cur = (c->freq_hz == st->freq_hz);
            pr_buf_addf(out,
                "<div class=\"ch%s%s%s\" data-ch=\"%d\" title=\"%.3f MHz\">%d</div>",
                cur ? " is-current" : "",
                (c->flags & PR_CH_F_GATEWAY) ? " is-gw" : "",
                (c->flags & PR_CH_F_DATA) ? " is-data" : "",
                c->num, c->freq_hz / 1000000.0, c->num);
        }
    }
    pr_buf_add(out, "</div>\n"
        "<p class=\"hint\">Kanal anklicken zum Umschalten. "
        "<b>&#8727;</b> Gateway &#183; <b>&#9632;</b> Daten</p></div>\n");

    /* --- Bans --- */
    pr_buf_add(out, "<div class=\"card\"><h2 class=\"grad\">Gesperrte Stationen</h2>\n");
    pr_buf_add(out, "<table><thead><tr><th>Muster</th><th>Grund</th><th></th>"
                    "</tr></thead><tbody>\n");
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
        pr_buf_add(out, "<button type=\"submit\" class=\"btn\">Entfernen</button>"
                        "</form></td></tr>\n");
    }
    if (cfg->nbans == 0)
        pr_buf_add(out, "<tr><td colspan=\"3\" style=\"color:var(--fg-faint)\">"
                        "keine Sperren</td></tr>\n");
    pr_buf_add(out, "</tbody></table>\n");

    pr_buf_add(out, "<form method=\"post\" action=\"\" class=\"row\">");
    html_input_hidden(out, "action", "ban_add");
    html_csrf(out, sess);
    html_input_text(out, "pattern", "", "DL9* oder KB1ABC-3", "Muster",
                    "Platzhalter * und ?");
    html_input_text(out, "reason", "", "", "Grund", "");
    pr_buf_add(out, "<div class=\"card-actions\">"
                    "<button type=\"submit\">Sperren</button></div></form>\n");
    pr_buf_add(out, "</div>\n");

    /* --- Schrift & Darstellung --- */
    pr_buf_add(out, "<form class=\"card\" method=\"post\" action=\"\">"
                    "<h2 class=\"grad\">Schrift &amp; Darstellung</h2>\n");
    html_input_hidden(out, "action", "save_ui");
    html_csrf(out, sess);
    html_input_text(out, "font_file", cfg->font_file, "./fonts/prterm.ttf",
                    "Schriftdatei", ".otf oder .ttf, relativ zur prterm.ini");
    html_input_number(out, "font_size", cfg->font_size, 6, 96, "Schriftgr&#246;&#223;e (px)", "");
    html_input_number(out, "line_height", cfg->line_height_pct, 100, 300,
                      "Zeilenabstand (%)", "120 = 1.2");

    {
        const char *dv[] = { "compact", "normal" };
        const char *dl[] = { "Kompakt (max. Zeichenraum)", "Normal" };
        html_select(out, "density", dv, dl, 2, cfg->ui_density, "Dichte", "");
    }
    {
        const char *dv[] = { "silver", "dark" };
        const char *dl[] = { "Silber (Standard)", "Dunkel" };
        html_select(out, "theme", dv, dl, 2, cfg->ui_theme, "Farbschema", "");
    }
    pr_buf_add(out, "<div class=\"card-actions\">"
                    "<button type=\"submit\" class=\"primary\">Speichern</button>"
                    "</div></form>\n");

    /* --- Sicherheit --- */
    pr_buf_add(out, "<form class=\"card\" method=\"post\" action=\"\">"
                    "<h2 class=\"grad\">Sicherheit</h2>\n");
    html_input_hidden(out, "action", "pass_change");
    html_csrf(out, sess);
    html_input_text(out, "old", "", "", "Altes Passwort", "");
    html_input_text(out, "new", "", "", "Neues Passwort", "");
    html_input_text(out, "new2", "", "", "Wiederholen", "");
    html_checkbox(out, "allow_guest_tx", cfg->allow_guest_tx,
                  "Senden ohne Anmeldung erlauben", "");
    pr_buf_add(out, "<div class=\"card-actions\">"
                    "<button type=\"submit\" class=\"primary\">Passwort &#228;ndern</button>"
                    "</div></form>\n");

    /* --- Roh-INI --- */
    pr_buf_add(out, "<form class=\"card\" method=\"post\" action=\"\">"
                    "<h2 class=\"grad\">Konfiguration (prterm.ini)</h2>\n");
    html_input_hidden(out, "action", "config_save");
    html_csrf(out, sess);

    if (cfg->raw != NULL) {
        char *dump = ini_dump(cfg->raw);
        pr_buf_add(out, "<div class=\"field\"><textarea name=\"text\" rows=\"14\" "
                        "style=\"width:100%;font-family:var(--pr-font);"
                        "white-space:pre\">");
        pr_html_escape(out, dump != NULL ? dump : "");
        pr_buf_add(out, "</textarea></div>\n");
        free(dump);
    }
    pr_buf_add(out, "<div class=\"card-actions\">"
                    "<button type=\"submit\" class=\"primary\">Speichern</button>"
                    "</div></form>\n");

    /* --- Sitzung --- */
    pr_buf_add(out, "<form class=\"card\" method=\"post\" action=\"\">"
                    "<h2 class=\"grad\">Sitzung</h2>\n");
    html_input_hidden(out, "action", "logout");
    html_csrf(out, sess);
    pr_buf_addf(out, "<p>Angemeldet als <b>%s</b></p>\n", "");
    pr_html_escape(out, sess->user);
    pr_buf_add(out, "<div class=\"card-actions\">"
                    "<button type=\"submit\">Abmelden</button></div></form>\n");

    pr_buf_add(out, "</div>\n</section>\n");
}

/* ======================================================================= */
/* Anmeldedialog                                                           */
/* ======================================================================= */

static void render_login(pr_buf *out, const pr_session *sess)
{
    (void)sess;
    pr_buf_add(out,
        "<dialog id=\"logindlg\" style=\"border:1px solid var(--line-2);"
        "border-radius:var(--radius);background:var(--panel);color:var(--fg);"
        "padding:18px 20px;min-width:min(340px,90vw)\">\n"
        "<form id=\"loginform\" method=\"post\" action=\"\">\n"
        "<h2 class=\"grad\" style=\"margin-top:0\">Anmeldung</h2>\n"
        "<div id=\"loginmsg\" class=\"note note-err\" hidden></div>\n"
        "<div class=\"field\"><label>Benutzer</label>"
        "<input type=\"text\" id=\"loginuser\" name=\"user\" value=\"\" "
        "placeholder=\"admin\" autocomplete=\"off\" spellcheck=\"false\"></div>\n");
    pr_buf_add(out, "<div class=\"field\"><label>Passwort</label>"
                    "<input type=\"password\" id=\"loginpass\" name=\"pass\" "
                    "autocomplete=\"current-password\"></div>\n");
    pr_buf_add(out, "<div class=\"card-actions\">"
                    "<button type=\"button\" data-close-login>Abbrechen</button>"
                    "<button type=\"submit\" class=\"primary\">Anmelden</button>"
                    "</div>\n</form>\n</dialog>\n");
}

/* ======================================================================= */
/* Ganze Seite                                                             */
/* ======================================================================= */

void page_render(pr_buf *out, const pr_config *cfg, const pr_session *sess,
                 const pr_rig_state *st, const pr_msg *msgs, size_t nmsg,
                 const char *flash_kind, const char *flash_msg)
{
    char title[128];
    snprintf(title, sizeof title, "%s", cfg->site_name);

    html_open(out, cfg, sess, title);

    pr_buf_add(out, "<div class=\"app\">\n");
    render_topbar(out, cfg, sess, st);
    pr_buf_add(out, "<main class=\"views\">\n");

    if (flash_msg != NULL && flash_msg[0] != '\0')
        html_note(out, flash_kind != NULL ? flash_kind : "info", "%s", flash_msg);

    render_terminal(out, cfg, st);
    render_admin(out, cfg, sess, st);
    pr_buf_add(out, "</main>\n</div>\n");

    render_login(out, sess);

    /* Startdaten fuer das Skript: erstes Log-Rendering serverseitig */
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

/* ======================================================================= */
/* JSON-Zustand                                                            */
/* ======================================================================= */

static void json_state(pr_response *res, const app *a, const pr_config *cfg,
                       const pr_session *sess, const pr_msg *msgs, size_t nmsg,
                       bool rig_started)
{
    pr_response_json(res, 200);
    pr_buf *b = &res->body;

    json_begin(b, true);
    json_kv_bool(b, "logged_in", sess->valid);
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
    json_kv_bool(b, "link_ok", a->st.link_ok);
    /* Grund, warum das Rig nicht hochgekommen ist - ohne das bleibt das
     * Problem unsichtbar: die Seite rendert, aber nichts funktioniert. */
    if (!rig_started && a->err[0] != '\0')
        json_kv_str(b, "rig_error", a->err);

    pr_buf_add(b, ",\"messages\":[");
    for (size_t i = 0; i < nmsg; i++) {
        if (i > 0) pr_buf_addc(b, ',');
        pr_buf_addf(b, "{\"kind\":\"%c\",\"ts\":%lld,\"db\":%d,\"from\":\"",
                    msgs[i].kind, msgs[i].ts, msgs[i].db);
        pr_json_escape(b, msgs[i].from);
        pr_buf_add(b, "\",\"text\":\"");
        pr_json_escape(b, msgs[i].text);
        pr_buf_add(b, "\"}");
    }
    pr_buf_add(b, "]}");
}

/* ======================================================================= */
/* Request-Handling                                                        */
/* ======================================================================= */

static bool check_csrf(const pr_config *cfg, const pr_request *req,
                       const pr_session *sess, pr_response *res)
{
    (void)cfg;
    const char *tok = pr_req_param(req, "csrf");
    if (sess->valid && !pr_session_check_csrf(sess, tok)) {
        json_err(res, "Sitzung abgelaufen oder ungültiges Token");
        return false;
    }
    return true;
}

int pr_handle(pr_request *req, pr_response *res, pr_config *cfg)
{
    const char *action = pr_req_param(req, "action");

    /* ---- Schriftdatei ------------------------------------------------ */
    if (action != NULL && strcmp(action, "font") == 0) {
        char mime[32];
        if (!html_font_mime(cfg->font_file, mime, sizeof mime)) {
            pr_response_text(res, 404);
            pr_buf_add(&res->body, "keine Schrift konfiguriert\n");
            return 0;
        }
        char *data = NULL;
        size_t len = 0;
        char err[256];
        if (pr_read_file(cfg->font_file, &data, &len, err, sizeof err) != 0) {
            pr_response_text(res, 404);
            pr_buf_add(&res->body, "Schriftdatei nicht lesbar\n");
            return 0;
        }
        pr_response_binary(res, 200, mime);
        pr_response_header(res, "Cache-Control: public, max-age=31536000, immutable");
        pr_buf_addn(&res->body, data, len);
        free(data);
        return 0;
    }

    /* ---- Sitzung ----------------------------------------------------- */
    pr_session sess;
    pr_session_from_request(cfg, req, &sess);

    app a;
    bool rig_started = (app_start(&a, cfg) == 0);

    /* ---- Log lesen --------------------------------------------------- */
    pr_msg msgs[400];
    size_t nmsg = 0;
    {
        char err[128];
        (void)pr_log_tail(cfg, msgs, 400, &nmsg, err, sizeof err);
    }

    /* ---- Nur-Empfang / Senden / Betrieb ------------------------------ */
    if (action != NULL && req->method[0] == 'P') {
        if (!check_csrf(cfg, req, &sess, res)) {
            app_stop(&a);
            return 0;
        }

        if (strcmp(action, "tx") == 0) {
            const char *text = pr_req_param(req, "text");
            char err[256];
            if (!rig_started) {
                json_err(res, a.err);
            } else if (app_tx(&a, text, &sess, err, sizeof err) != 0) {
                json_err(res, err);
            } else {
                json_ok(res);
            }
            app_stop(&a);
            return 0;
        }

        if (strcmp(action, "ptt") == 0) {
            bool on = pr_parse_bool(pr_req_param(req, "on"), false);
            char err[256];
            if (!rig_started) {
                json_err(res, a.err);
            } else if (a.rig.vtbl->set_ptt(&a.rig, on, err, sizeof err) != 0) {
                json_err(res, err);
            } else {
                json_ok(res);
            }
            app_stop(&a);
            return 0;
        }

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

            /* Zielzustand erst vollstaendig bestimmen, dann pruefen,
             * dann anwenden. Sonst faellt eine ungültige Kombination wie
             * "AM auf Kanal 41" durch, weil der Kanal die Betriebsart
             * stillschweigend zurueckgesetzt hat. */
            long     want_freq = a.st.freq_hz;
            unsigned want_mode = a.st.mode;
            pr_duplex want_duplex = a.st.duplex;

            const char *ch = pr_req_param(req, "channel");
            if (ok && ch != NULL && ch[0] != '\0') {
                const pr_channel *c =
                    pr_bandplan_channel(cfg->bandplan, atoi(ch));
                if (c == NULL) {
                    snprintf(err, sizeof err, "unbekannter Kanal %s", ch);
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
                    snprintf(err, sizeof err, "ung\u00fcltige Frequenz");
                    ok = false;
                } else {
                    want_freq = f;
                }
            }

            const char *md = pr_req_param(req, "mode");
            if (ok && md != NULL && md[0] != '\0') {
                unsigned m = pr_band_mode_from_name(md);
                if (m == 0) {
                    snprintf(err, sizeof err, "unbekannte Betriebsart");
                    ok = false;
                } else {
                    want_mode = m;
                }
            }

            const char *dx = pr_req_param(req, "duplex");
            if (ok && dx != NULL && dx[0] != '\0')
                want_duplex = pr_duplex_from_name(dx);

            /* Betriebsart bewusst ausgewaehlt? Dann gilt die volle Pruefung.
             * Wird nur der Kanal gewaehlt, wird die Betriebsart angepasst -
             * das Abstimmen selbst ist nicht eingeschraenkt, nur das Senden. */
            bool mode_explicit = (md != NULL && md[0] != '\0');

            /* Compliance */
            if (ok && mode_explicit) {
                if (!pr_bandplan_tx_freq_ok(cfg->bandplan, want_freq,
                                            want_mode, err, sizeof err))
                    ok = false;
            } else if (ok) {
                const pr_channel *c = pr_bandplan_at_freq(cfg->bandplan, want_freq);
                if (c != NULL && (c->modes & want_mode) == 0) {
                    /* auf eine hier zulaessige Betriebsart wechseln */
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

        /* ---- Anmeldung ----------------------------------------------- */
        if (strcmp(action, "login") == 0) {
            const char *user = pr_req_param(req, "user");
            const char *pass = pr_req_param(req, "pass");
            pr_session s;
            char err[256];

            if (!pr_login_throttle(cfg, req->remote_addr)) {
                json_err(res, "zu viele Fehlversuche - bitte später erneut");
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

        /* ---- Konfiguration ------------------------------------------- */
        if (pr_starts_with(action, "save_") ||
            pr_starts_with(action, "ban_") ||
            strcmp(action, "pass_change") == 0 ||
            strcmp(action, "config_save") == 0) {

            if (!sess.valid) {
                json_err(res, "Anmeldung erforderlich");
                app_stop(&a);
                return 0;
            }
            pr_admin_action(req, res, cfg, &sess);
            app_stop(&a);
            return 0;
        }
    }

    /* ---- Zustand als JSON -------------------------------------------- */
    if (action != NULL && strcmp(action, "state") == 0) {
        size_t want = (size_t)atoi(pr_req_get(req, "rows") != NULL
                                   ? pr_req_get(req, "rows") : "0");
        (void)want;
        /* neue Nachrichten seit dem letzten Abruf waeren besser, aber ein
         * kurzes Fenster reicht fuer die Anzeige */
        size_t from = nmsg > 12 ? nmsg - 12 : 0;
        json_state(res, &a, cfg, &sess, msgs + from, nmsg - from, rig_started);
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

    /* ---- Seite rendern ------------------------------------------------ */
    pr_response_html(res, 200);
    pr_buf body;
    pr_buf_init(&body);
    page_render(&body, cfg, &sess, &a.st, msgs, nmsg, NULL, NULL);
    pr_buf_addn(&res->body, body.data ? body.data : "", body.len);
    pr_buf_free(&body);

    app_stop(&a);
    return 0;
}
