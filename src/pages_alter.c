/*
 * PRTERM - CB & Amateur Radio Terminal
 * pages_alter.c — Retro/alternative UI.
 *
 * Simple HTML 4.01 with tables and forms. Works in text browsers
 * (lynx, links, w3m) and on retro computers. No JavaScript required.
 * Progressive enhancement loads via prterm-alter.js if available.
 *
 * 100% feature parity with prterm.cgi — same actions, same API.
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
#include "kiss.h"
#include "lang.h"
#include "tncsock.h"
#include "state.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/* ======================================================================= */
/* Helpers                                                                 */
/* ======================================================================= */

static const char *T(const pr_config *cfg, const char *english)
{
    return pr_tr(cfg->language, english);
}

static void json_err(pr_response *res, const char *msg)
{
    pr_response_json(res, 400);
    pr_buf_addf(&res->body, "{\"ok\":false,\"error\":\"%s\"}", msg);
}

static void json_ok(pr_response *res)
{
    pr_response_json(res, 200);
    pr_buf_add(&res->body, "{\"ok\":true}");
}

/* ======================================================================= */
/* App / Rig layer                                                         */
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
    char err[256];
    pr_runtime_init(cfg, err, sizeof err);

    if (pr_rig_open(&a->rig, cfg, a->err, sizeof a->err) != 0) {
        a->rig_ok = false;
        return -1;
    }
    a->rig_ok = true;
    a->rig.vtbl->refresh(&a->rig, a->err, sizeof a->err);
    size_t ndrain = 0;
    pr_msg tmp[64];
    a->rig.vtbl->drain(&a->rig, tmp, 64, &ndrain);
    a->rig.vtbl->get_state(&a->rig, &a->st);
    return 0;
}

static void app_stop(app *a)
{
    if (a->rig_ok) pr_rig_close(&a->rig);
}

static int app_tx_gate(app *a, const pr_session *sess,
                       char *err, size_t errlen)
{
    if (!a->rig_ok) {
        snprintf(err, errlen, "%s", T(a->cfg, "device not ready"));
        return -1;
    }
    if (a->st.monitor) {
        snprintf(err, errlen, "%s", T(a->cfg, "monitor mode — transmit blocked"));
        return -1;
    }
    if (pr_config_is_banned(a->cfg, a->cfg->callerid)) {
        snprintf(err, errlen, "%s", T(a->cfg, "your callsign is banned"));
        return -1;
    }
    if (!pr_bandplan_tx_allowed(a->cfg->bandplan, a->st.freq_hz,
                                a->cfg->mode, a->cfg->tx_power_mw,
                                err, errlen)) {
        return -1;
    }
    if (sess == NULL && !a->cfg->allow_guest_tx) {
        snprintf(err, errlen, "%s", T(a->cfg, "guest transmit is disabled"));
        return -1;
    }
    return 0;
}

static void app_tx_hold_airtime(const pr_config *cfg, size_t textlen)
{
    long baud = cfg->baud > 0 ? cfg->baud : 2400;
    size_t frame = 16 + textlen + 2;
    double sec = (double)frame * 8.0 / (double)baud;
    if (sec < 0.05) sec = 0.05;
    struct timespec ts;
    ts.tv_sec = (time_t)sec;
    ts.tv_nsec = (long)((sec - ts.tv_sec) * 1e9);
    (void)nanosleep(&ts, NULL);
}

static int app_tx(app *a, const char *from, const char *to,
                  const char *text, const pr_session *sess,
                  char *err, size_t errlen)
{
    if (app_tx_gate(a, sess, err, errlen) != 0) return -1;
    int lock_fd = pr_arbiter_acquire(a->cfg->runtime_dir, a->st.freq_hz,
                                     "alter-cgi", 5000, err, errlen);
    if (lock_fd < 0) return -1;
    int rc = a->rig.vtbl->send(&a->rig, from, to, text, err, errlen);
    if (rc == 0) app_tx_hold_airtime(a->cfg, strlen(text));
    pr_arbiter_release(lock_fd);
    return rc;
}

/* ======================================================================= */
/* Retro page rendering — Topbar                                           */
/* ======================================================================= */

static void render_topbar(pr_buf *out, const pr_config *cfg,
                          const pr_rig_state *st)
{
    pr_buf_addf(out,
        "<div class=\"topbar\">\n"
        " <span class=\"brand\">%s</span>\n"
        " <span class=\"chip\">%s: %s</span>\n"
        " <span class=\"chip\">QRG: <span id=\"s-freq\">%.3f MHz</span></span>\n"
        " <span class=\"chip\">%s: <span id=\"s-mode\">%s</span></span>\n",
        cfg->site_name,
        T(cfg, "Station"), cfg->callerid,
        st->freq_hz / 1000000.0,
        T(cfg, "Mode"), pr_band_mode_name(st->mode));

    pr_buf_add(out, " <span class=\"chip\">");
    if (st->duplex == PR_DUPLEX_FULL)
        pr_buf_addf(out, "%s", T(cfg, "FULL-DUPLEX"));
    else
        pr_buf_addf(out, "%s", T(cfg, "HALF-DUPLEX"));
    pr_buf_add(out, "</span>\n");

    if (cfg->nstations > 1) {
        pr_buf_addf(out, " <span class=\"chip\">%s: <span id=\"s-device\">%s</span></span>\n",
                    T(cfg, "Active"), st->device);
    }

    /* Navigation */
    pr_buf_addf(out,
        " | <a href=\"prterm-alter.cgi\">%s</a>\n",
        T(cfg, "Terminal"));
    if (cfg->mailboxd_enabled) {
        pr_buf_addf(out,
            " | <a href=\"prterm-alter.cgi?action=mailbox\">%s</a>\n",
            T(cfg, "Mailbox"));
    }
    pr_buf_addf(out,
        " | <a href=\"prterm-alter.cgi?action=admin\">%s</a>\n",
        T(cfg, "Administration"));

    pr_buf_add(out, "</div>\n");
}

/* ======================================================================= */
/* Terminal view                                                           */
/* ======================================================================= */

static void render_terminal(pr_buf *out, const pr_config *cfg,
                            const pr_session *sess, const pr_rig_state *st,
                            const pr_msg *msgs, size_t nmsg)
{
    pr_buf_addf(out, "<h2>%s</h2>\n", T(cfg, "Terminal"));

    /* Station selector (if multi-station) */
    if (cfg->nstations > 1) {
        pr_buf_addf(out, "<form method=\"get\" action=\"prterm-alter.cgi\">\n"
                        "<label>%s: <select name=\"station\" onchange=\"this.form.submit()\">\n",
                    T(cfg, "Station"));
        for (size_t i = 0; i < cfg->nstations; i++) {
            const pr_station *s = &cfg->stations[i];
            if (!s->enabled) continue;
            pr_buf_addf(out, "<option value=\"%s\"%s>%s (%ld baud)</option>\n",
                        s->name,
                        strcmp(s->name, cfg->active_station) == 0 ? " selected" : "",
                        s->name, s->radio_baud);
        }
        pr_buf_add(out, "</select></label>\n"
                        "<noscript><input type=\"submit\" value=\"OK\"></noscript>\n"
                        "</form>\n");
    }

    /* Receive log */
    pr_buf_addf(out, "<pre class=\"term\" id=\"term\">\n");
    for (size_t i = 0; i < nmsg; i++) {
        const pr_msg *m = &msgs[i];
        time_t t = (time_t)m->ts;
        struct tm *tm = localtime(&t);
        char timebuf[16];
        strftime(timebuf, sizeof timebuf, "%H:%M:%S", tm);

        const char *cls = "ln-sys";
        if (m->kind == PR_MSG_RX) cls = "ln-rx";
        else if (m->kind == PR_MSG_TX) cls = "ln-tx";
        else if (m->kind == PR_MSG_WARN) cls = "ln-warn";
        else if (m->kind == PR_MSG_ERR) cls = "ln-err";

        pr_buf_addf(out, "<div class=\"%s\">%s", cls, timebuf);
        if (m->from[0] != '\0') {
            pr_buf_addf(out, " %s", m->from);
            if (m->to[0] != '\0')
                pr_buf_addf(out, " &gt; %s", m->to);
        }
        pr_buf_addf(out, "  %s</div>\n", m->text);
    }
    pr_buf_add(out, "</pre>\n");

    /* Send form — works without JS */
    pr_buf_addf(out,
        "<form method=\"post\" action=\"prterm-alter.cgi\" id=\"txform\" class=\"txbar\">\n"
        "<input type=\"hidden\" name=\"action\" value=\"tx\">\n");
    html_csrf(out, sess);
    pr_buf_addf(out,
        "<input type=\"text\" name=\"text\" id=\"txtext\" size=\"60\" "
        "placeholder=\"%s\">\n"
        "<input type=\"text\" name=\"to\" size=\"10\" placeholder=\"CQ\" value=\"CQ\">\n"
        "<input type=\"submit\" value=\"%s\">\n"
        "</form>\n",
        T(cfg, "Enter message"),
        T(cfg, "Send"));

    /* Monitor toggle */
    pr_buf_addf(out,
        "<form method=\"post\" action=\"prterm-alter.cgi\" class=\"inline\">\n"
        "<input type=\"hidden\" name=\"action\" value=\"monitor\">\n");
    html_csrf(out, sess);
    pr_buf_addf(out,
        "<input type=\"hidden\" name=\"on\" value=\"%s\">\n"
        "<input type=\"submit\" value=\"%s\">\n"
        "</form>\n",
        st->monitor ? "0" : "1",
        st->monitor ? T(cfg, "Resume RX+TX") : T(cfg, "Monitor only (RX)"));

    /* Status table */
    pr_buf_addf(out,
        "<table>\n"
        "<tr><td>%s</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td></tr>\n"
        "<tr>"
        "<td>%.3f MHz</td>"
        "<td>%s</td>"
        "<td>Ch %d</td>"
        "<td>%ld baud</td>"
        "<td>%s</td>"
        "</tr>\n"
        "</table>\n",
        T(cfg, "Frequency"), T(cfg, "Mode"), T(cfg, "Channel"),
        T(cfg, "Baud"), T(cfg, "Status"),
        st->freq_hz / 1000000.0,
        pr_band_mode_name(st->mode),
        pr_config_channel(cfg),
        cfg->radio_baud,
        st->link_ok ? "OK" : "ERROR");
}

/* ======================================================================= */
/* MailboxD view (retro)                                                   */
/* ======================================================================= */

static void render_mailbox(pr_buf *out, const pr_config *cfg)
{
    if (!cfg->mailboxd_enabled) return;

    pr_buf_addf(out, "<h2>%s — %s</h2>\n", "MailboxD", T(cfg, "local mailbox"));

    /* Login form */
    pr_buf_addf(out,
        "<div id=\"mbox-login\">\n"
        "<h3>%s</h3>\n"
        "<form method=\"post\" action=\"prterm-alter.cgi\">\n"
        "<input type=\"hidden\" name=\"action\" value=\"mbox_login\">\n"
        "<table>\n"
        "<tr><td>%s:</td><td><input type=\"text\" name=\"user\" size=\"20\"></td></tr>\n"
        "<tr><td>%s:</td><td><input type=\"password\" name=\"pass\" size=\"20\"></td></tr>\n"
        "<tr><td></td><td><input type=\"submit\" value=\"%s\"></td></tr>\n"
        "</table>\n</form>\n</div>\n",
        T(cfg, "Login"),
        T(cfg, "User"), T(cfg, "Password"), T(cfg, "Log in"));

    /* MailboxD terminal output */
    pr_buf_addf(out, "<pre class=\"term\" id=\"mbox-term\"></pre>\n");

    /* Command form */
    pr_buf_addf(out,
        "<form method=\"post\" action=\"prterm-alter.cgi\" class=\"txbar\">\n"
        "<input type=\"hidden\" name=\"action\" value=\"mbox_run\">\n"
        "<input type=\"text\" name=\"cmd\" size=\"60\" placeholder=\"%s\">\n"
        "<input type=\"submit\" value=\"%s\">\n"
        "</form>\n",
        T(cfg, "Enter command"),
        T(cfg, "Send"));

    /* Logout */
    pr_buf_addf(out,
        "<form method=\"post\" action=\"prterm-alter.cgi\" class=\"inline\">\n"
        "<input type=\"hidden\" name=\"action\" value=\"mbox_logout\">\n"
        "<input type=\"submit\" value=\"%s\">\n"
        "</form>\n",
        T(cfg, "Log out"));

    /* RF Broadcast (if rig available) */
    pr_buf_addf(out,
        "<hr>\n"
        "<h3>%s</h3>\n"
        "<form method=\"post\" action=\"prterm-alter.cgi\">\n"
        "<input type=\"hidden\" name=\"action\" value=\"mbox_broadcast\">\n"
        "<table>\n"
        "<tr><td>%s:</td><td><input type=\"text\" name=\"msg\" size=\"60\"></td></tr>\n"
        "<tr><td></td><td><input type=\"submit\" value=\"%s\"></td></tr>\n"
        "</table>\n</form>\n",
        T(cfg, "RF Broadcast"),
        T(cfg, "Message"),
        T(cfg, "Transmit"));
}

/* ======================================================================= */
/* Admin view (retro — all cards as tables)                                */
/* ======================================================================= */

static void render_admin(pr_buf *out, const pr_config *cfg,
                         const pr_session *sess, const pr_rig_state *st)
{
    if (sess == NULL || !sess->valid) {
        pr_buf_addf(out,
            "<h2>%s</h2>\n"
            "<p>%s %s</p>\n"
            "<form method=\"post\" action=\"prterm-alter.cgi\">\n"
            "<input type=\"hidden\" name=\"action\" value=\"login\">\n"
            "<table>\n"
            "<tr><td>%s:</td><td><input type=\"text\" name=\"user\" size=\"20\"></td></tr>\n"
            "<tr><td>%s:</td><td><input type=\"password\" name=\"pass\" size=\"20\"></td></tr>\n"
            "<tr><td></td><td><input type=\"submit\" value=\"%s\"></td></tr>\n"
            "</table>\n</form>\n",
            T(cfg, "Administration"),
            T(cfg, "The admin area is locked."),
            T(cfg, "Log in directly here in the terminal."),
            T(cfg, "User"), T(cfg, "Password"), T(cfg, "Log in"));
        return;
    }

    pr_buf_addf(out, "<h2>%s</h2>\n", T(cfg, "Administration"));

    /* ---- General ---- */
    pr_buf_addf(out,
        "<h3>%s</h3>\n"
        "<form method=\"post\" action=\"prterm-alter.cgi\">\n"
        "<input type=\"hidden\" name=\"action\" value=\"save_site\">\n",
        T(cfg, "General"));
    html_csrf(out, sess);
    pr_buf_addf(out,
        "<table>\n"
        "<tr><td>%s:</td><td><input type=\"text\" name=\"site_name\" value=\"%s\" size=\"30\"></td></tr>\n"
        "<tr><td>%s:</td><td><input type=\"text\" name=\"subtitle\" value=\"%s\" size=\"40\"></td></tr>\n"
        "<tr><td>%s:</td><td><select name=\"language\">\n",
        T(cfg, "Name"), cfg->site_name,
        T(cfg, "Subtitle"), cfg->site_subtitle,
        T(cfg, "Language"));
    for (size_t i = 0; i < pr_lang_count(); i++) {
        pr_buf_addf(out, "<option value=\"%s\"%s>%s</option>\n",
                    pr_lang_code(i),
                    strcmp(cfg->language, pr_lang_code(i)) == 0 ? " selected" : "",
                    pr_lang_name(i));
    }
    pr_buf_addf(out, "</select></td></tr>\n"
                    "<tr><td></td><td><input type=\"submit\" value=\"%s\"></td></tr>\n"
                    "</table>\n</form>\n", T(cfg, "Save"));

    /* ---- Station ---- */
    pr_buf_addf(out,
        "<h3>%s</h3>\n"
        "<form method=\"post\" action=\"prterm-alter.cgi\">\n"
        "<input type=\"hidden\" name=\"action\" value=\"save_station\">\n",
        T(cfg, "Station"));
    html_csrf(out, sess);
    pr_buf_addf(out,
        "<table>\n"
        "<tr><td>CALLERID:</td><td><input type=\"text\" name=\"callerid\" value=\"%s\" size=\"12\"></td></tr>\n"
        "<tr><td>QTH:</td><td><input type=\"text\" name=\"qth\" value=\"%s\" size=\"20\"></td></tr>\n"
        "<tr><td>%s:</td><td><input type=\"text\" name=\"locator\" value=\"%s\" size=\"10\"></td></tr>\n"
        "<tr><td></td><td><input type=\"submit\" value=\"%s\"></td></tr>\n"
        "</table>\n</form>\n",
        cfg->callerid, cfg->qth,
        T(cfg, "Locator"), cfg->locator,
        T(cfg, "Save"));

    /* ---- Per-station devices ---- */
    for (size_t i = 0; i < cfg->nstations; i++) {
        const pr_station *st = &cfg->stations[i];
        pr_buf_addf(out,
            "<h3>%s: %s</h3>\n"
            "<form method=\"post\" action=\"prterm-alter.cgi\">\n"
            "<input type=\"hidden\" name=\"action\" value=\"save_station_radio\">\n"
            "<input type=\"hidden\" name=\"name\" value=\"%s\">\n",
            T(cfg, "Device"), st->name, st->name);
        html_csrf(out, sess);
        pr_buf_addf(out,
            "<table>\n"
            "<tr><td>%s:</td><td><select name=\"driver\">\n",
            T(cfg, "Driver"));
        for (size_t k = 0; k < pr_rig_count(); k++) {
            const pr_rig_vtbl *v = pr_rig_at(k);
            if (v == NULL) continue;
            pr_buf_addf(out, "<option value=\"%s\"%s>%s</option>\n",
                        v->name,
                        strcmp(st->rig_driver, v->name) == 0 ? " selected" : "",
                        v->description);
        }
        pr_buf_addf(out,
            "</select></td></tr>\n"
            "<tr><td>%s:</td><td><input type=\"text\" name=\"port\" value=\"%s\" size=\"40\"></td></tr>\n"
            "<tr><td>%s:</td><td><input type=\"text\" name=\"baud\" value=\"%ld\" size=\"10\"></td></tr>\n"
            "<tr><td>%s:</td><td><select name=\"kiss_init\">\n",
            T(cfg, "Serial interface"), st->port,
            T(cfg, "Baud rate"), st->baud,
            T(cfg, "KISS entry"));
        const char *kv[] = { "esc", "auto", "tapr" };
        const char *kl[] = { "ESC @K (TheFirmware)", "AUTO (retry)", "TAPR \"kiss on\"" };
        for (int k = 0; k < 3; k++) {
            pr_buf_addf(out, "<option value=\"%s\"%s>%s</option>\n",
                        kv[k], strcmp(st->kiss_init, kv[k]) == 0 ? " selected" : "", kl[k]);
        }
        pr_buf_addf(out,
            "</select></td></tr>\n"
            "<tr><td>%s:</td><td><input type=\"text\" name=\"modem\" value=\"%s\" size=\"20\"></td></tr>\n"
            "<tr><td>%s:</td><td><input type=\"text\" name=\"serial_line\" value=\"%s\" size=\"8\"></td></tr>\n"
            "<tr><td>%s:</td><td><input type=\"text\" name=\"freq_hz\" value=\"%s\" size=\"15\"></td></tr>\n"
            "<tr><td>%s:</td><td><select name=\"mode\">\n",
            T(cfg, "Modem"), st->modem,
            T(cfg, "Serial line"), st->serial_line,
            T(cfg, "Frequency (Hz)"),
            st->freq_hz > 0 ? "" : "",
            T(cfg, "Mode"));
        const char *mv[] = { "inherit", "fm", "am", "ssb" };
        const char *ml[] = { T(cfg, "inherit [radio] mode"), "FM", "AM", "SSB" };
        for (int k = 0; k < 4; k++) {
            char current[8];
            snprintf(current, sizeof current, "%s",
                     st->mode == 0u ? "inherit" : pr_band_mode_name(st->mode));
            pr_buf_addf(out, "<option value=\"%s\"%s>%s</option>\n",
                        mv[k], strcmp(current, mv[k]) == 0 ? " selected" : "", ml[k]);
        }
        pr_buf_addf(out,
            "</select></td></tr>\n"
            "<tr><td>CALLERID:</td><td><input type=\"text\" name=\"callerid\" value=\"%s\" size=\"12\"></td></tr>\n"
            "<tr><td>%s:</td><td><input type=\"text\" name=\"antenna\" value=\"%s\" size=\"20\"></td></tr>\n"
            "<tr><td>%s:</td><td><input type=\"checkbox\" name=\"enabled\" value=\"1\"%s></td></tr>\n",
            st->callerid,
            T(cfg, "Antenna"), st->antenna,
            T(cfg, "Enabled"), st->enabled ? " checked" : "");
        if (st->radio_baud != 0) {
            pr_buf_addf(out, "<tr><td>%s:</td><td>%ld %s</td></tr>\n",
                        T(cfg, "Radio baud (hardware)"), st->radio_baud,
                        T(cfg, "- fixed, not changeable"));
        }
        pr_buf_addf(out, "<tr><td></td><td><input type=\"submit\" value=\"%s\"></td></tr>\n"
                        "</table>\n</form>\n", T(cfg, "Save"));
    }

    /* ---- Callsign ---- */
    pr_buf_addf(out,
        "<h3>%s</h3>\n"
        "<form method=\"post\" action=\"prterm-alter.cgi\">\n"
        "<input type=\"hidden\" name=\"action\" value=\"save_callsign\">\n",
        T(cfg, "Callsign"));
    html_csrf(out, sess);
    pr_buf_addf(out,
        "<table>\n"
        "<tr><td>%s:</td><td><input type=\"text\" name=\"callid_max_len\" value=\"%d\" size=\"5\"></td></tr>\n"
        "<tr><td>%s:</td><td><input type=\"text\" name=\"callerid_base_len\" value=\"%d\" size=\"5\"></td></tr>\n"
        "<tr><td>%s:</td><td><input type=\"text\" name=\"callerid_max_total\" value=\"%d\" size=\"5\"></td></tr>\n"
        "<tr><td>%s:</td><td><input type=\"text\" name=\"callerid_ssid_digits\" value=\"%d\" size=\"5\"></td></tr>\n"
        "<tr><td>%s:</td><td><input type=\"checkbox\" name=\"callerid_allow_ssid\" value=\"1\"%s></td></tr>\n"
        "<tr><td></td><td><input type=\"submit\" value=\"%s\"></td></tr>\n"
        "</table>\n</form>\n",
        T(cfg, "CALLID max. length"), cfg->callsign.callid_max_len,
        T(cfg, "CALLERID base"), cfg->callsign.callerid_base_len,
        T(cfg, "CALLERID total"), cfg->callsign.callerid_max_total,
        T(cfg, "SSID digits"), cfg->callsign.ssid_digits,
        T(cfg, "allow SSID"), cfg->callsign.allow_ssid ? " checked" : "",
        T(cfg, "Save"));

    /* ---- Blocked stations ---- */
    pr_buf_addf(out, "<h3>%s</h3>\n", T(cfg, "Blocked stations"));
    if (cfg->nbans > 0) {
        pr_buf_addf(out, "<table><tr><th>%s</th><th>%s</th><th></th></tr>\n",
                    T(cfg, "Pattern"), T(cfg, "Reason"));
        for (size_t i = 0; i < cfg->nbans; i++) {
            pr_buf_addf(out, "<tr><td><code>%s</code></td><td>%s</td><td>",
                        cfg->bans[i].pattern, cfg->bans[i].reason);
            pr_buf_addf(out,
                "<form method=\"post\" action=\"prterm-alter.cgi\" class=\"inline\">"
                "<input type=\"hidden\" name=\"action\" value=\"ban_del\">"
                "<input type=\"hidden\" name=\"pattern\" value=\"%s\">",
                cfg->bans[i].pattern);
            html_csrf(out, sess);
            pr_buf_addf(out, "<input type=\"submit\" value=\"%s\"></form></td></tr>\n",
                        T(cfg, "Remove"));
        }
        pr_buf_add(out, "</table>\n");
    } else {
        pr_buf_addf(out, "<p>%s</p>\n", T(cfg, "no blocks"));
    }
    pr_buf_addf(out,
        "<form method=\"post\" action=\"prterm-alter.cgi\">\n"
        "<input type=\"hidden\" name=\"action\" value=\"ban_add\">\n",
        T(cfg, "Block"));
    html_csrf(out, sess);
    pr_buf_addf(out,
        "<table>\n"
        "<tr><td>%s:</td><td><input type=\"text\" name=\"pattern\" size=\"20\" placeholder=\"DL9*\"></td></tr>\n"
        "<tr><td>%s:</td><td><input type=\"text\" name=\"reason\" size=\"30\"></td></tr>\n"
        "<tr><td></td><td><input type=\"submit\" value=\"%s\"></td></tr>\n"
        "</table>\n</form>\n",
        T(cfg, "Pattern"), T(cfg, "Reason"), T(cfg, "Block"));

    /* ---- Font & display ---- */
    pr_buf_addf(out,
        "<h3>%s</h3>\n"
        "<form method=\"post\" action=\"prterm-alter.cgi\">\n"
        "<input type=\"hidden\" name=\"action\" value=\"save_ui\">\n",
        T(cfg, "Font &amp; display"));
    html_csrf(out, sess);
    pr_buf_addf(out,
        "<table>\n"
        "<tr><td>%s:</td><td><input type=\"text\" name=\"font_file\" value=\"%s\" size=\"40\"></td></tr>\n"
        "<tr><td>%s:</td><td><input type=\"text\" name=\"font_size\" value=\"%d\" size=\"5\"></td></tr>\n"
        "<tr><td>%s:</td><td><input type=\"text\" name=\"line_height\" value=\"%d\" size=\"5\"></td></tr>\n"
        "<tr><td>%s:</td><td><select name=\"density\">\n",
        T(cfg, "Font file"), cfg->font_file,
        T(cfg, "Font size (px)"), cfg->font_size,
        T(cfg, "Line height (%)"), cfg->line_height_pct,
        T(cfg, "Density"));
    const char *dv[] = { "compact", "normal" };
    const char *dl[] = { T(cfg, "Compact"), T(cfg, "Normal") };
    for (int k = 0; k < 2; k++) {
        pr_buf_addf(out, "<option value=\"%s\"%s>%s</option>\n",
                    dv[k], strcmp(cfg->ui_density, dv[k]) == 0 ? " selected" : "", dl[k]);
    }
    pr_buf_addf(out, "</select></td></tr>\n"
                    "<tr><td>%s:</td><td><select name=\"theme\">\n",
                T(cfg, "Color scheme"));
    const char *tv[] = { "silver", "dark" };
    const char *tl[] = { T(cfg, "Silver (default)"), T(cfg, "Dark") };
    for (int k = 0; k < 2; k++) {
        pr_buf_addf(out, "<option value=\"%s\"%s>%s</option>\n",
                    tv[k], strcmp(cfg->ui_theme, tv[k]) == 0 ? " selected" : "", tl[k]);
    }
    pr_buf_addf(out, "</select></td></tr>\n"
                    "<tr><td></td><td><input type=\"submit\" value=\"%s\"></td></tr>\n"
                    "</table>\n</form>\n", T(cfg, "Save"));

    /* ---- Security ---- */
    pr_buf_addf(out,
        "<h3>%s</h3>\n"
        "<form method=\"post\" action=\"prterm-alter.cgi\">\n"
        "<input type=\"hidden\" name=\"action\" value=\"pass_change\">\n",
        T(cfg, "Security"));
    html_csrf(out, sess);
    pr_buf_addf(out,
        "<table>\n"
        "<tr><td>%s:</td><td><input type=\"password\" name=\"old\" size=\"20\"></td></tr>\n"
        "<tr><td>%s:</td><td><input type=\"password\" name=\"new\" size=\"20\"></td></tr>\n"
        "<tr><td>%s:</td><td><input type=\"password\" name=\"new2\" size=\"20\"></td></tr>\n"
        "<tr><td>%s:</td><td><input type=\"checkbox\" name=\"allow_guest_tx\" value=\"1\"%s></td></tr>\n"
        "<tr><td></td><td><input type=\"submit\" value=\"%s\"></td></tr>\n"
        "</table>\n</form>\n",
        T(cfg, "Old password"), T(cfg, "New password"), T(cfg, "Repeat"),
        T(cfg, "allow transmitting without login"),
        cfg->allow_guest_tx ? " checked" : "",
        T(cfg, "Change password"));

    /* ---- INI editors ---- */
    pr_buf_addf(out, "<h3>%s</h3>\n<ul>\n", T(cfg, "Configuration files"));
    pr_buf_addf(out, "<li><a href=\"prterm-alter.cgi?action=ini_editor&amp;target=prterm\">%s</a> (<code>%s</code>)</li>\n",
                T(cfg, "Edit prterm.ini"), cfg->ini_path);
    if (cfg->mailboxd_enabled) {
        char mpath[512];
        snprintf(mpath, sizeof mpath, "%s/mailboxd.ini", cfg->mailboxd_dir);
        pr_buf_addf(out, "<li><a href=\"prterm-alter.cgi?action=ini_editor&amp;target=mailboxd\">%s</a> (<code>%s</code>)</li>\n",
                    T(cfg, "Edit mailboxd.ini"), mpath);
    }
    pr_buf_add(out, "</ul>\n");

    /* ---- MailboxD user management ---- */
    if (cfg->mailboxd_enabled) {
        pr_buf_addf(out,
            "<h3>%s</h3>\n"
            "<form method=\"post\" action=\"prterm-alter.cgi\">\n"
            "<input type=\"hidden\" name=\"action\" value=\"mbox_usercreate\">\n",
            T(cfg, "MailboxD user management"));
        html_csrf(out, sess);
        pr_buf_addf(out,
            "<p>%s</p>\n"
            "<table>\n"
            "<tr><td>%s:</td><td><input type=\"text\" name=\"name\" size=\"20\"></td></tr>\n"
            "<tr><td>%s:</td><td><input type=\"text\" name=\"full\" size=\"30\"></td></tr>\n"
            "<tr><td>%s:</td><td><input type=\"text\" name=\"country\" size=\"5\"></td></tr>\n"
            "<tr><td>%s:</td><td><input type=\"text\" name=\"location\" size=\"15\"></td></tr>\n"
            "<tr><td>%s:</td><td><input type=\"text\" name=\"email\" size=\"30\"></td></tr>\n"
            "<tr><td></td><td><input type=\"submit\" value=\"%s\"></td></tr>\n"
            "</table>\n</form>\n",
            T(cfg, "Create a new user."),
            T(cfg, "Username"), T(cfg, "Full name"), T(cfg, "Country"),
            T(cfg, "Location"), T(cfg, "Email"),
            T(cfg, "Create user"));
    }

    /* ---- Session / Logout ---- */
    pr_buf_addf(out,
        "<h3>%s</h3>\n"
        "<form method=\"post\" action=\"prterm-alter.cgi\">\n"
        "<input type=\"hidden\" name=\"action\" value=\"logout\">\n",
        T(cfg, "Session"));
    html_csrf(out, sess);
    pr_buf_addf(out, "<p>%s <b>%s</b></p>\n", T(cfg, "Logged in as"), sess->user);
    pr_buf_addf(out, "<input type=\"submit\" value=\"%s\">\n</form>\n",
                T(cfg, "Log out"));
}

/* ======================================================================= */
/* Full page render                                                        */
/* ======================================================================= */

void page_render(pr_buf *out, const pr_config *cfg, const pr_session *sess,
                 const pr_rig_state *st, const pr_msg *msgs, size_t nmsg,
                 const char *flash_kind, const char *flash_msg)
{
    pr_buf_addf(out,
        "<!DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 4.01 Transitional//EN\">\n"
        "<html>\n<head>\n"
        "<meta http-equiv=\"Content-Type\" content=\"text/html; charset=utf-8\">\n"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
        "<title>%s — %s</title>\n"
        "<link rel=\"stylesheet\" href=\"prterm-alter.cgi?action=css\">\n"
        "</head>\n<body>\n",
        cfg->site_name, T(cfg, "Terminal"));

    render_topbar(out, cfg, st);

    if (flash_msg != NULL && flash_msg[0] != '\0') {
        const char *cls = "flash flash-ok";
        if (flash_kind != NULL && strcmp(flash_kind, "err") == 0) cls = "flash flash-err";
        else if (flash_kind != NULL && strcmp(flash_kind, "warn") == 0) cls = "flash flash-warn";
        pr_buf_addf(out, "<div class=\"%s\">%s</div>\n", cls, flash_msg);
    }

    render_terminal(out, cfg, sess, st, msgs, nmsg);

    pr_buf_add(out,
        "<script src=\"prterm-alter.cgi?action=js\"></script>\n"
        "</body>\n</html>\n");
}

void page_csrf_meta(pr_buf *out, const pr_session *sess)
{
    (void)out; (void)sess;
}

/* ======================================================================= */
/* JSON state (for JS polling)                                             */
/* ======================================================================= */

static void json_state(pr_response *res, const pr_config *cfg,
                       const pr_rig_state *st, const pr_msg *msgs,
                       size_t nmsg, long long since)
{
    pr_response_json(res, 200);
    pr_buf_addf(&res->body, "{\"ok\":true");
    pr_buf_addf(&res->body, ",\"callerid\":\"%s\"", cfg->callerid);
    pr_buf_addf(&res->body, ",\"freq_hz\":%ld", st->freq_hz);
    pr_buf_addf(&res->body, ",\"channel\":%d", pr_config_channel(cfg));
    pr_buf_addf(&res->body, ",\"mode\":\"%s\"", pr_band_mode_name(st->mode));
    pr_buf_addf(&res->body, ",\"duplex\":\"%s\"", pr_duplex_name(st->duplex));
    pr_buf_addf(&res->body, ",\"ptt\":%s", st->ptt ? "true" : "false");
    pr_buf_addf(&res->body, ",\"monitor\":%s", st->monitor ? "true" : "false");
    pr_buf_addf(&res->body, ",\"rx_count\":%ld", st->rx_count);
    pr_buf_addf(&res->body, ",\"tx_count\":%ld", st->tx_count);
    pr_buf_addf(&res->body, ",\"device\":\"%s\"", st->device);
    pr_buf_addf(&res->body, ",\"link_ok\":%s", st->link_ok ? "true" : "false");
    pr_buf_addf(&res->body, ",\"last_rx_ts\":%lld", (long long)st->last_rx_ts);
    pr_buf_addf(&res->body, ",\"last_tx_ts\":%lld", (long long)st->last_tx_ts);

    pr_buf_add(&res->body, ",\"messages\":[");
    bool first = true;
    for (size_t i = 0; i < nmsg; i++) {
        const pr_msg *m = &msgs[i];
        if (m->ts <= since) continue;
        if (pr_config_is_banned(cfg, m->from)) continue;
        if (!first) pr_buf_add(&res->body, ",");
        first = false;
        pr_buf_addf(&res->body,
            "{\"kind\":\"%c\",\"ts\":%lld,\"db\":%d,"
            "\"from\":\"%s\",\"to\":\"%s\",\"station\":\"%s\","
            "\"text\":\"%s\"}",
            m->kind, (long long)m->ts, m->db,
            m->from, m->to, m->station, m->text);
    }
    pr_buf_add(&res->body, "]");
    pr_buf_add(&res->body, "}");
}

/* ======================================================================= */
/* Document open/close helpers                                             */
/* ======================================================================= */

static void alter_doc_open(pr_buf *out, const pr_config *cfg,
                           const pr_session *sess, const char *title)
{
    pr_buf_addf(out,
        "<!DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 4.01 Transitional//EN\">\n"
        "<html>\n<head>\n"
        "<meta http-equiv=\"Content-Type\" content=\"text/html; charset=utf-8\">\n"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
        "<title>%s</title>\n"
        "<link rel=\"stylesheet\" href=\"prterm-alter.cgi?action=css\">\n"
        "</head>\n<body>\n", title);
}

static void alter_doc_close(pr_buf *out)
{
    pr_buf_add(out,
        "<script src=\"prterm-alter.cgi?action=js\"></script>\n"
        "</body>\n</html>\n");
}

/* ======================================================================= */
/* Request handler                                                         */
/* ======================================================================= */

int pr_handle(pr_request *req, pr_response *res, pr_config *cfg)
{
    const char *action = pr_req_param(req, "action");

    /* ---- Static assets (no rig needed) -------------------------------- */
    if (action != NULL && strcmp(action, "css") == 0) {
        pr_response_binary(res, 200, "text/css");
        pr_buf_add(&res->body, html_css());
        return 0;
    }
    if (action != NULL && strcmp(action, "js") == 0) {
        pr_response_binary(res, 200, "application/javascript");
        pr_buf_add(&res->body, html_js());
        return 0;
    }

    /* ---- INI editor (no rig needed) ----------------------------------- */
    if (action != NULL && strcmp(action, "ini_editor") == 0) {
        const char *target = pr_req_param(req, "target");
        pr_session sess;
        if (!pr_session_from_request(cfg, req, &sess) || !sess.valid) {
            pr_response_redirect(res, 302, "prterm-alter.cgi?action=admin");
            return 0;
        }
        pr_inieditor_render(res, cfg, &sess, target, NULL);
        return 0;
    }
    if (action != NULL && strcmp(action, "ini_editor_save") == 0) {
        pr_session sess;
        if (!pr_session_from_request(cfg, req, &sess) || !sess.valid) {
            json_err(res, T(cfg, "login required"));
            return 0;
        }
        char flash[256] = "";
        char err[256] = "";
        const char *target = pr_req_param(req, "target");
        if (pr_inieditor_save(req, res, cfg, target, flash, sizeof flash, err, sizeof err) != 0) {
            json_err(res, err);
        } else {
            char loc[512];
            snprintf(loc, sizeof loc, "prterm-alter.cgi?action=ini_editor&target=%s", target);
            pr_response_redirect(res, 302, loc);
        }
        return 0;
    }

    /* ---- Session + rig init ------------------------------------------- */
    pr_session sess;
    bool has_session = pr_session_from_request(cfg, req, &sess);

    if (cfg->active_station[0] == '\0')
        pr_config_apply_station(cfg, NULL);

    app a;
    app_start(&a, cfg);
    bool rig_started = a.rig_ok;

    pr_msg log[400];
    size_t nlog = 0;
    char logerr[256];
    pr_log_tail(cfg, log, 400, &nlog, logerr, sizeof logerr);

    /* ---- POST actions ------------------------------------------------- */
    if (strcmp(req->method, "POST") == 0) {
        /* CSRF check — skip for login (no session yet) and public actions */
        bool need_csrf = true;
        if (action != NULL && (strcmp(action, "login") == 0 ||
                               strcmp(action, "mbox_login") == 0 ||
                               strcmp(action, "mbox_run") == 0 ||
                               strcmp(action, "mbox_logout") == 0))
            need_csrf = false;

        if (need_csrf && !pr_session_check_csrf(&sess, pr_req_param(req, "csrf"))) {
            json_err(res, T(cfg, "invalid or expired session"));
            app_stop(&a);
            return 0;
        }

        /* ---- Transmit ------------------------------------------------- */
        if (action != NULL && strcmp(action, "tx") == 0) {
            const char *text = pr_req_post(req, "text");
            const char *to   = pr_req_post(req, "to");
            if (text == NULL || text[0] == '\0') {
                json_err(res, T(cfg, "message is empty"));
                app_stop(&a);
                return 0;
            }
            if (to == NULL || to[0] == '\0') to = "CQ";
            char err[256];
            if (app_tx(&a, cfg->callerid, to, text, &sess, err, sizeof err) != 0) {
                json_err(res, err);
            } else {
                pr_response_redirect(res, 302, "prterm-alter.cgi");
            }
            app_stop(&a);
            return 0;
        }

        /* ---- Monitor toggle ------------------------------------------- */
        if (action != NULL && strcmp(action, "monitor") == 0) {
            const char *on = pr_req_post(req, "on");
            bool enabled = (on != NULL && strcmp(on, "1") == 0);
            if (a.rig_ok)
                a.rig.vtbl->set_monitor(&a.rig, enabled, a.err, sizeof a.err);
            pr_response_redirect(res, 302, "prterm-alter.cgi");
            app_stop(&a);
            return 0;
        }

        /* ---- Login ---------------------------------------------------- */
        if (action != NULL && strcmp(action, "login") == 0) {
            const char *user = pr_req_post(req, "user");
            const char *pass = pr_req_post(req, "pass");
            char err[256];
            pr_session newsess;
            if (!pr_session_login(cfg, user, pass, &newsess, err, sizeof err)) {
                pr_response_html(res, 200);
                char title[128];
                snprintf(title, sizeof title, "%s — %s", cfg->site_name, T(cfg, "Login failed"));
                alter_doc_open(&res->body, cfg, NULL, title);
                render_topbar(&res->body, cfg, &a.st);
                pr_buf_addf(&res->body, "<div class=\"flash flash-err\">%s</div>\n", err);
                render_admin(&res->body, cfg, &sess, &a.st);
                alter_doc_close(&res->body);
            } else {
                pr_response_set_cookie(res, "PRTERM_SID", newsess.sid, 28800, true);
                pr_response_redirect(res, 302, "prterm-alter.cgi?action=admin");
            }
            app_stop(&a);
            return 0;
        }

        /* ---- Logout --------------------------------------------------- */
        if (action != NULL && strcmp(action, "logout") == 0) {
            if (has_session)
                pr_session_destroy(cfg, sess.sid);
            pr_response_clear_cookie(res, "PRTERM_SID");
            pr_response_redirect(res, 302, "prterm-alter.cgi");
            app_stop(&a);
            return 0;
        }

        /* ---- MailboxD login ------------------------------------------- */
        if (action != NULL && strcmp(action, "mbox_login") == 0) {
            pr_mbox_login(req, res, cfg);
            app_stop(&a);
            return 0;
        }

        /* ---- MailboxD run command ------------------------------------- */
        if (action != NULL && strcmp(action, "mbox_run") == 0) {
            pr_mbox_run(req, res, cfg);
            app_stop(&a);
            return 0;
        }

        /* ---- MailboxD logout ------------------------------------------ */
        if (action != NULL && strcmp(action, "mbox_logout") == 0) {
            pr_mbox_logout(req, res);
            app_stop(&a);
            return 0;
        }

        /* ---- MailboxD RF broadcast ------------------------------------ */
        if (action != NULL && strcmp(action, "mbox_broadcast") == 0) {
            const char *msg = pr_req_param(req, "msg");
            if (msg == NULL || msg[0] == '\0') {
                json_err(res, T(cfg, "broadcast message is empty"));
                app_stop(&a);
                return 0;
            }
            if (!rig_started) {
                json_err(res, a.err[0] ? a.err : T(cfg, "no rig connected"));
                app_stop(&a);
                return 0;
            }
            if (a.st.monitor) {
                json_err(res, T(cfg, "monitor mode: transmitting is locked"));
                app_stop(&a);
                return 0;
            }
            long long now = pr_now_s();
            long long last_activity = a.st.last_rx_ts;
            if (a.st.last_tx_ts > last_activity) last_activity = a.st.last_tx_ts;
            long long silent = (last_activity > 0) ? (now - last_activity) : 999;
            if (silent < 150) {
                long remaining = 150 - (long)silent;
                char msg_buf[256];
                snprintf(msg_buf, sizeof msg_buf,
                         T(cfg, "channel not clear — wait %ld s (need 150 s silence)"),
                         remaining);
                json_err(res, msg_buf);
                app_stop(&a);
                return 0;
            }
            char err[256];
            if (app_tx(&a, cfg->callerid, "", msg, &sess, err, sizeof err) != 0) {
                json_err(res, err);
            } else {
                json_ok(res);
            }
            app_stop(&a);
            return 0;
        }

        /* ---- MailboxD CQ beacon cycle --------------------------------- */
        if (action != NULL && strcmp(action, "mbox_cqbeacon") == 0) {
            const char *cid = pr_req_param(req, "callerid");
            const char *msg = pr_req_param(req, "msg");
            if (cid == NULL || cid[0] == '\0') {
                json_err(res, T(cfg, "callerid is required"));
                app_stop(&a);
                return 0;
            }
            if (msg == NULL || msg[0] == '\0') {
                json_err(res, T(cfg, "broadcast message is empty"));
                app_stop(&a);
                return 0;
            }
            if (!rig_started) {
                json_err(res, a.err[0] ? a.err : T(cfg, "no rig connected"));
                app_stop(&a);
                return 0;
            }
            long long now = pr_now_s();
            bool transmitted = false;
            char used_device[64] = "";
            for (size_t si = 0; si < cfg->nstations && !transmitted; si++) {
                const pr_station *sta = &cfg->stations[si];
                if (!sta->enabled) continue;
                long long last = a.st.last_rx_ts;
                if (a.st.last_tx_ts > last) last = a.st.last_tx_ts;
                long long silent = (last > 0) ? (now - last) : 999;
                if (silent < 150) continue;
                char err[256];
                if (app_tx(&a, cid, "CQ", msg, &sess, err, sizeof err) == 0) {
                    transmitted = true;
                    pr_strlcpy(used_device, sta->name, sizeof used_device);
                }
            }
            if (transmitted) {
                pr_response_json(res, 200);
                pr_buf_addf(&res->body, "{\"ok\":true,\"device\":\"%s\"}", used_device);
            } else {
                json_err(res, T(cfg, "no device available (all busy or disabled)"));
            }
            app_stop(&a);
            return 0;
        }

        /* ---- MailboxD RX poll ----------------------------------------- */
        if (action != NULL && strcmp(action, "mbox_rxpoll") == 0) {
            const char *cid = pr_req_param(req, "callerid");
            if (cid == NULL || cid[0] == '\0') {
                json_err(res, T(cfg, "callerid is required"));
                app_stop(&a);
                return 0;
            }
            pr_response_json(res, 200);
            pr_buf_add(&res->body, "{\"ok\":true,\"frames\":[");
            int nout = 0;
            long long now_ts = pr_now_s();
            for (size_t si = 0; si < cfg->nstations; si++) {
                const pr_station *sta = &cfg->stations[si];
                if (!sta->enabled) continue;
                char sock_path[512];
                snprintf(sock_path, sizeof sock_path, "%s/tnc-%.32s.sock",
                         cfg->runtime_dir, sta->name);
                int sfd = socket(AF_UNIX, SOCK_STREAM, 0);
                if (sfd < 0) continue;
                struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
                setsockopt(sfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
                setsockopt(sfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
                struct sockaddr_un sa;
                memset(&sa, 0, sizeof sa);
                sa.sun_family = AF_UNIX;
                pr_strlcpy(sa.sun_path, sock_path, sizeof sa.sun_path);
                if (connect(sfd, (struct sockaddr *)&sa, sizeof sa) != 0) {
                    close(sfd);
                    continue;
                }
                if (send(sfd, "RX\n", 3, MSG_NOSIGNAL) != 3) {
                    close(sfd);
                    continue;
                }
                char resp[4096];
                size_t roff = 0;
                for (;;) {
                    if (roff + 1 >= sizeof resp) break;
                    char c;
                    ssize_t r = recv(sfd, &c, 1, 0);
                    if (r <= 0) break;
                    if (c == '\n') break;
                    if (c != '\r') resp[roff++] = c;
                }
                resp[roff] = '\0';
                close(sfd);
                if (strncmp(resp, "OK ", 3) != 0) continue;
                const char *hex = resp + 3;
                if (hex[0] == '\0') continue;
                unsigned char kiss_raw[1024];
                size_t kiss_len = pr_tncsock_hex_decode(kiss_raw, sizeof kiss_raw, hex);
                if (kiss_len == 0) continue;
                kiss_decoder kd;
                kiss_decoder_init(&kd);
                kiss_decoder_feed_buf(&kd, kiss_raw, kiss_len);
                if (kiss_decoder_ready(&kd) == 0) continue;
                unsigned char ax25[KISS_FRAME_MAX];
                size_t axlen = kiss_decoder_take(&kd, ax25, sizeof ax25);
                if (axlen < 16) continue;
                if (axlen >= 18 && kiss_fcs_ok(ax25, axlen)) axlen -= 2;
                if (axlen < 16) continue;
                if (ax25[14] != 0x03u || ax25[15] != 0xF0u) continue;
                char to[16], from[16];
                if (!call_from_ax25(ax25, to, sizeof to)) continue;
                if (!call_from_ax25(ax25 + 7, from, sizeof from)) continue;
                if (strcmp(to, cid) != 0) continue;
                char text[512];
                size_t tlen = axlen - 16;
                if (tlen >= sizeof text) tlen = sizeof text - 1;
                memcpy(text, ax25 + 16, tlen);
                text[tlen] = '\0';
                if (nout > 0) pr_buf_addc(&res->body, ',');
                pr_buf_addf(&res->body, "{\"from\":\"%s\",\"to\":\"%s\","
                            "\"text\":\"%s\",\"station\":\"%s\",\"ts\":%lld}",
                            from, to, text, sta->name, now_ts);
                nout++;
            }
            pr_buf_add(&res->body, "]}");
            app_stop(&a);
            return 0;
        }

        /* ---- Admin actions -------------------------------------------- */
        if (has_session && sess.valid) {
            int rc = pr_admin_action(req, res, cfg, &sess);
            if (rc == 0) {
                pr_response_redirect(res, 302, "prterm-alter.cgi?action=admin");
                app_stop(&a);
                return 0;
            }
        }

        /* Unknown POST action */
        json_err(res, T(cfg, "unknown action"));
        app_stop(&a);
        return 0;
    }

    /* ---- GET actions --------------------------------------------------- */

    /* JSON state (for JS polling) */
    if (action != NULL && strcmp(action, "state") == 0) {
        long long since = 0;
        const char *s = pr_req_get(req, "since");
        if (s != NULL && s[0] != '\0') since = atoll(s);
        json_state(res, cfg, &a.st, log, nlog, since);
        app_stop(&a);
        return 0;
    }

    /* JSON log */
    if (action != NULL && strcmp(action, "log") == 0) {
        pr_response_json(res, 200);
        pr_buf_add(&res->body, "{\"ok\":true,\"messages\":[");
        for (size_t i = 0; i < nlog; i++) {
            const pr_msg *m = &log[i];
            if (i > 0) pr_buf_add(&res->body, ",");
            pr_buf_addf(&res->body,
                "{\"kind\":\"%c\",\"ts\":%lld,\"from\":\"%s\",\"to\":\"%s\",\"text\":\"%s\"}",
                m->kind, (long long)m->ts, m->from, m->to, m->text);
        }
        pr_buf_add(&res->body, "]}");
        app_stop(&a);
        return 0;
    }

    /* MailboxD page */
    if (action != NULL && strcmp(action, "mailbox") == 0) {
        pr_response_html(res, 200);
        char title[128];
        snprintf(title, sizeof title, "%s — MailboxD", cfg->site_name);
        alter_doc_open(&res->body, cfg, &sess, title);
        render_topbar(&res->body, cfg, &a.st);
        render_mailbox(&res->body, cfg);
        alter_doc_close(&res->body);
        app_stop(&a);
        return 0;
    }

    /* Admin page */
    if (action != NULL && strcmp(action, "admin") == 0) {
        pr_response_html(res, 200);
        char title[128];
        snprintf(title, sizeof title, "%s — %s", cfg->site_name, T(cfg, "Administration"));
        alter_doc_open(&res->body, cfg, &sess, title);
        render_topbar(&res->body, cfg, &a.st);
        render_admin(&res->body, cfg, &sess, &a.st);
        alter_doc_close(&res->body);
        app_stop(&a);
        return 0;
    }

    /* ---- Default: render terminal page -------------------------------- */
    pr_response_html(res, 200);
    page_render(&res->body, cfg, &sess, &a.st, log, nlog, NULL, NULL);

    app_stop(&a);
    return 0;
}