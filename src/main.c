/*
 * PRTERM - CB & Amateur Radio Terminal
 * main.c - Einstiegspunkt.
 *
 * Zwei Betriebsformen:
 *
 *   1. CGI  (GATEWAY_INTERFACE gesetzt)
 *      -> eine Anfrage bearbeiten, HTML oder JSON ausgeben.
 *
 *   2. Kommandozeile (kein Webserver)
 *      -> Konfiguration pruefen, Passwort-Hash erzeugen, Bandplan zeigen.
 *         Das ist wichtig fuer "keine Installation": alles laeuft auch
 *         ohne Browser und ohne Webserver.
 *
 * SPDX-License-Identifier: MIT
 */
#include "prterm_compat.h"

#include "bands.h"
#include "callsign.h"
#include "cgi.h"
#include "config.h"
#include "html.h"
#include "ini.h"
#include "pages.h"
#include "selftest.h"
#include "session.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PRTERM_VERSION "0.1.0"

/* ======================================================================= */
/* Konfigurationsdatei finden                                               */
/* ======================================================================= */

/*
 * Reihenfolge:
 *   1. Kommandozeilenargument
 *   2. Umgebungsvariable PRTERM_INI
 *   3. ./prterm.ini
 *   4. prterm.ini neben der ausfuehrbaren Datei (aus argv[0] abgeleitet)
 */
static void find_ini(int argc, char **argv, char *dst, size_t dstlen)
{
    const char *env = getenv("PRTERM_INI");
    if (env != NULL && env[0] != '\0') {
        pr_strlcpy(dst, env, dstlen);
        return;
    }

    if (pr_file_exists("prterm.ini")) {
        pr_strlcpy(dst, "prterm.ini", dstlen);
        return;
    }

    if (argc > 0 && argv[0] != NULL) {
        char base[1024];
        pr_strlcpy(base, argv[0], sizeof base);
        char *slash = strrchr(base, '/');
        if (slash != NULL) {
            *slash = '\0';
            snprintf(dst, dstlen, "%.480s/prterm.ini", base);
            if (pr_file_exists(dst))
                return;
        }
    }

    pr_strlcpy(dst, "prterm.ini", dstlen);
}

/* ======================================================================= */
/* Kommandozeile                                                           */
/* ======================================================================= */

static void usage(FILE *f)
{
    fprintf(f,
"PRTERM %s - CB & Amateurfunk Terminal (CGI)\n"
"\n"
"Aufruf:\n"
"  prterm.cgi                      als CGI vom Webserver ausfuehren\n"
"  prterm.cgi [OPTION]             als Werkzeug auf der Kommandozeile\n"
"\n"
"Optionen:\n"
"  -h, --help                diese Hilfe\n"
"  -V, --version             Version\n"
"  --check-ini [DATEI]       Konfiguration pruefen\n"
"  --selftest [DATEI]        Geraete-Zustand pruefen\n"
"  --reset-tnc [DATEI]       Notfall-Ruecksetzung des TNC ausloesen\n"
"  --print-config [DATEI]    wirksame Konfiguration anzeigen\n"
"  --gen-ini [DATEI]         Beispiel-konfiguration erzeugen\n"
"  --hash-password PASS      Passwort-Hash fuer [admin] pass_hash\n"
"  --add-ban MUSTER [GRUND]  Rufzeichen sperren\n"
"  --del-ban MUSTER          Sperre aufheben\n"
"  --channels [BANDPLAN]     Kanaltabelle anzeigen\n"
"\n"
"Umgebung:\n"
"  PRTERM_INI                Pfad zur prterm.ini\n"
"\n"
"Installation: prterm.cgi + prterm.ini in einen CGI-Ordner kopieren.\n",
        PRTERM_VERSION);
}

static int cmd_check_ini(const char *path)
{
    char err[256];
    pr_config cfg;

    if (pr_config_load(&cfg, path, err, sizeof err) != 0) {
        fprintf(stderr, "FEHLER: %s\n", err);
        pr_config_free(&cfg);
        return 1;
    }

    printf("prterm.ini ist gueltig: %s\n", path);
    printf("  Name          : %s\n", cfg.site_name);
    printf("  CALLERID      : %s\n", cfg.callerid);
    printf("  Treiber       : %s\n", cfg.rig_driver);
    printf("  Schnittstelle : %s @ %ld\n", cfg.port, cfg.baud);
    printf("  Duplex        : %s\n", pr_duplex_name(cfg.duplex));
    printf("  Frequenz      : %ld Hz (Kanal %d)\n",
           cfg.freq_hz, pr_config_channel(&cfg));
    printf("  Betriebsart   : %s\n", pr_band_mode_name(cfg.mode));
    printf("  Bandplan      : %s\n", cfg.bandplan->name);
    printf("  Bans          : %zu\n", cfg.nbans);
    printf("  Schrift       : %s (%d px)\n",
           cfg.font_file[0] != '\0' ? cfg.font_file : "System",
           cfg.font_size);
    printf("  Admin         : %s\n", cfg.admin_enabled ? "aktiv" : "gesperrt");
    printf("  Passwort      : %s\n",
           cfg.admin_pass_hash[0] != '\0' ? "gesetzt"
                                          : "KEINES - Login ist gesperrt");

    if (cfg.nstations > 0) {
        printf("\n  Stationen (%zu):\n", cfg.nstations);
        for (size_t k = 0; k < cfg.nstations; k++) {
            const pr_station *st = &cfg.stations[k];
            printf("    %-10s %s\n", st->name, st->enabled ? "" : "(gesperrt)");
            printf("               Geraet      : %s\n", st->port);
            printf("               seriell     : %ld %s\n", st->baud, st->serial_line);
            printf("               Funk        : %ld Baud%s%s\n", st->radio_baud,
                   st->modem[0] != '\0' ? " / " : "",
                   st->modem[0] != '\0' ? st->modem : "");
            printf("               CALLERID    : %s\n", st->callerid);
            if (st->antenna[0] != '\0')
                printf("               Antenne     : %s\n", st->antenna);
        }
    }

    pr_config_free(&cfg);
    return 0;
}

static int cmd_hash_password(const char *pass)
{
    char hash[160];
    if (pr_hash_password(pass, hash, sizeof hash) != 0) {
        fprintf(stderr, "FEHLER: Hash konnte nicht erzeugt werden\n");
        return 1;
    }
    printf("%s\n", hash);
    fprintf(stderr,
            "In prterm.ini unter [admin] pass_hash eintragen.\n");
    return 0;
}

static void cmd_channels(const char *id)
{
    const pr_bandplan *bp = (id != NULL) ? pr_bandplan_by_id(id)
                                         : pr_bandplan_default();
    if (bp == NULL) {
        fprintf(stderr, "FEHLER: unbekannter Bandplan \"%s\"\n", id);
        return;
    }

    printf("%s\nQuelle: %s\n", bp->name, bp->source);
    printf("Kanalbandbreite: %ld Hz\n\n", bp->bw_hz);
    printf("  Kanal   Frequenz      Modi      Merkmale\n");
    printf("  -----  -----------  --------  ----------------\n");

    for (size_t i = 0; i < bp->nch; i++) {
        const pr_channel *c = &bp->ch[i];
        char modes[24];
        pr_band_modes_str(c->modes, modes, sizeof modes);

        char flags[24];
        flags[0] = '\0';
        if (c->flags & PR_CH_F_GATEWAY) pr_strlcat(flags, "Gateway ", sizeof flags);
        if (c->flags & PR_CH_F_DATA)    pr_strlcat(flags, "Daten ",   sizeof flags);

        printf("  %5d  %6.3f MHz  %-8s  %s\n",
               c->num, c->freq_hz / 1000000.0, modes, flags);
    }

    printf("\nLeistungsgrenzen:\n");
    printf("  FM/PM  4 W ERP    Kanal 1..80\n");
    printf("  AM     4 W ERP    Kanal 1..40\n");
    printf("  SSB   12 W PEP    Kanal 1..40\n");
}

static int cli(int argc, char **argv, const char *ini_path)
{
    const char *cmd = argv[1];

    if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0) {
        usage(stdout);
        return 0;
    }
    if (strcmp(cmd, "-V") == 0 || strcmp(cmd, "--version") == 0) {
        printf("PRTERM %s\n", PRTERM_VERSION);
        return 0;
    }
    if (strcmp(cmd, "--check-ini") == 0) {
        return cmd_check_ini(argc > 2 ? argv[2] : ini_path);
    }
    if (strcmp(cmd, "--selftest") == 0) {
        const char *path = argc > 2 ? argv[2] : ini_path;
        char err[256];
        pr_config cfg;
        if (pr_config_load(&cfg, path, err, sizeof err) != 0) {
            fprintf(stderr, "FEHLER: %s\n", err);
            pr_config_free(&cfg);
            return 1;
        }
        printf("PRTERM Zustandspruefung\n");
        printf("  Geraet : %s\n", cfg.port);
        printf("  Treiber: %s\n\n", cfg.rig_driver);

        pr_selftest st;
        int fails = pr_selftest_run(&cfg, &st);
        pr_selftest_print(&st, stdout);
        pr_config_free(&cfg);
        return fails == 0 ? 0 : 1;
    }
    if (strcmp(cmd, "--reset-tnc") == 0) {
        const char *path = argc > 2 ? argv[2] : ini_path;
        char err[256];
        pr_config cfg;
        if (pr_config_load(&cfg, path, err, sizeof err) != 0) {
            fprintf(stderr, "FEHLER: %s\n", err);
            pr_config_free(&cfg);
            return 1;
        }
        printf("PRTERM Notfall-Ruecksetzung fuer %s\n\n", cfg.port);

        pr_selftest st;
        int fails = pr_selftest_reset(&cfg, &st);
        pr_selftest_print(&st, stdout);
        pr_config_free(&cfg);
        return fails == 0 ? 0 : 1;
    }
    if (strcmp(cmd, "--hash-password") == 0) {
        if (argc < 3) {
            fprintf(stderr, "FEHLER: --hash-password benoetigt ein Passwort\n");
            return 2;
        }
        return cmd_hash_password(argv[2]);
    }
    if (strcmp(cmd, "--channels") == 0) {
        cmd_channels(argc > 2 ? argv[2] : NULL);
        return 0;
    }
    if (strcmp(cmd, "--print-config") == 0) {
        const char *path = argc > 2 ? argv[2] : ini_path;
        char *text = NULL;
        size_t len = 0;
        char err[256];
        if (pr_read_file(path, &text, &len, err, sizeof err) != 0) {
            fprintf(stderr, "FEHLER: %s\n", err);
            return 1;
        }
        fwrite(text, 1, len, stdout);
        free(text);
        return 0;
    }

    fprintf(stderr, "FEHLER: unbekannte Option \"%s\"\n\n", cmd);
    usage(stderr);
    return 2;
}

/* ======================================================================= */
/* CGI                                                                     */
/* ======================================================================= */

static int run_cgi(pr_config *cfg)
{
    char err[256];
    pr_request req;
    pr_response res;

    if (pr_request_parse(&req, err, sizeof err) != 0) {
        pr_response_init(&res);
        pr_response_text(&res, 400);
        pr_buf_addf(&res.body, "FEHLER: %s\n", err);
        pr_response_emit(&res);
        pr_response_free(&res);
        return 1;
    }

    pr_response_init(&res);
    pr_handle(&req, &res, cfg);
    pr_response_emit(&res);

    pr_response_free(&res);
    pr_request_free(&req);
    return 0;
}

/* ======================================================================= */
/* main                                                                    */
/* ======================================================================= */

int main(int argc, char **argv)
{
    char ini_path[1024];
    find_ini(argc, argv, ini_path, sizeof ini_path);

    /* Ohne Webserver: Werkzeugbetrieb */
    if (!pr_is_cgi()) {
        if (argc > 1)
            return cli(argc, argv, ini_path);

        /* Ohne Argumente und ohne CGI: kurze Hilfe, kein Fehler */
        usage(stdout);
        return 0;
    }

    /* CGI: Konfiguration laden */
    char err[256];
    pr_config cfg;
    if (pr_config_load(&cfg, ini_path, err, sizeof err) != 0) {
        pr_response res;
        pr_response_init(&res);
        pr_response_text(&res, 500);
        pr_buf_addf(&res.body, "PRTERM: Konfigurationsfehler\n\n%s\n\n"
                               "(erwartet: %s)\n", err, ini_path);
        pr_response_emit(&res);
        pr_response_free(&res);
        pr_config_free(&cfg);
        return 1;
    }

    int rc = run_cgi(&cfg);
    pr_config_free(&cfg);
    return rc;
}
