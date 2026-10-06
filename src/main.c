/*
 * PRTERM - CB & Amateur Radio Terminal
 * main.c - Entry point.
 *
 * Two operating modes:
 *
 *   1. CGI  (GATEWAY_INTERFACE set)
 *      -> handle one request, output HTML or JSON.
 *
 *   2. Command line (no webserver)
 *      -> check configuration, create password hash, show band plan.
 *         This matters for "no installation": everything also runs
 *         without a browser and without a webserver.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
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
/* Find the configuration file                                               */
/* ======================================================================= */

/*
 * Order:
 *   1. Command line argument
 *   2. Environment variable PRTERM_INI
 *   3. ./prterm.ini
 *   4. prterm.ini next to the executable (derived from argv[0])
 */
static void find_ini(int argc, char **argv, char *dst, size_t dstlen)
{
    const char *env = getenv("PRTERM_INI");
    if (env != NULL && env[0] != '\0') {
        pr_strlcpy(dst, env, dstlen);
        return;
    }

    /*
     * Next to the CGI. In web operation this is the reliable location:
     * the webserver has a different working directory than the caller,
     * and a prterm.ini lying there would otherwise take precedence.
     */
    {
        /*
         * Not every webserver sets SCRIPT_FILENAME. PATH_TRANSLATED is
         * set more reliably for CGI - both are tried.
         */
        const char *sf = getenv("SCRIPT_FILENAME");
        if (sf == NULL || sf[0] == '\0')
            sf = getenv("PATH_TRANSLATED");
        if (sf != NULL && sf[0] != '\0') {
            char base[512];
            pr_strlcpy(base, sf, sizeof base);
            char *slash = strrchr(base, '/');
            if (slash != NULL) {
                *slash = '\0';
                snprintf(dst, dstlen, "%.480s/prterm.ini", base);
                if (pr_file_exists(dst))
                    return;
            }
        }
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
/* Command line                                                            */
/* ======================================================================= */

static void usage(FILE *f)
{
    fprintf(f,
"PRTERM %s - CB & Amateur Radio Terminal (CGI)\n"
"\n"
"Usage:\n"
"  prterm.cgi                      run as CGI from the webserver\n"
"  prterm.cgi [OPTION]             run as a command line tool\n"
"\n"
"Options:\n"
"  -h, --help                this help\n"
"  -V, --version             version\n"
"  --check-ini [FILE]        check configuration\n"
"  --checkup [FILE] [STATION]    ensure KISS mode, clear memory\n"
"  --selftest [FILE] [STATION]   check device state\n"
"  --reset-tnc [FILE] [STATION]  trigger emergency TNC reset\n"
"  --print-config [FILE]     show effective configuration\n"
"  --gen-ini [FILE]          generate sample configuration\n"
"  --hash-password PASS      password hash for [admin] pass_hash\n"
"  --add-ban PATTERN [REASON]    block callsign\n"
"  --del-ban PATTERN         remove block\n"
"  --channels [BANDPLAN]     show channel table\n"
"\n"
"Environment:\n"
"  PRTERM_INI                path to prterm.ini\n"
"\n"
"Installation: copy prterm.cgi + prterm.ini into a CGI directory.\n",
        PRTERM_VERSION);
}

static int cmd_check_ini(const char *path)
{
    char err[256];
    pr_config cfg;

    if (pr_config_load(&cfg, path, err, sizeof err) != 0) {
        fprintf(stderr, "ERROR: %s\n", err);
        pr_config_free(&cfg);
        return 1;
    }

    printf("prterm.ini is valid: %s\n", path);
    printf("  Name          : %s\n", cfg.site_name);
    printf("  CALLERID      : %s\n", cfg.callerid);
    printf("  Driver        : %s\n", cfg.rig_driver);
    printf("  Interface     : %s @ %ld\n", cfg.port, cfg.baud);
    printf("  Duplex        : %s\n", pr_duplex_name(cfg.duplex));
    printf("  Frequency     : %ld Hz (channel %d)\n",
           cfg.freq_hz, pr_config_channel(&cfg));
    printf("  Mode          : %s\n", pr_band_mode_name(cfg.mode));
    printf("  Band plan     : %s\n", cfg.bandplan->name);
    printf("  Bans          : %zu\n", cfg.nbans);
    printf("  Font          : %s (%d px)\n",
           cfg.font_file[0] != '\0' ? cfg.font_file : "System",
           cfg.font_size);
    printf("  Admin         : %s\n", cfg.admin_enabled ? "enabled" : "disabled");
    printf("  Password      : %s\n",
           cfg.admin_pass_hash[0] != '\0' ? "set"
                                         : "NONE - login disabled");

    if (cfg.nstations > 0) {
        printf("\n  Stations (%zu):\n", cfg.nstations);
        for (size_t k = 0; k < cfg.nstations; k++) {
            const pr_station *st = &cfg.stations[k];
            printf("    %-10s %s\n", st->name, st->enabled ? "" : "(disabled)");
            printf("               Device      : %s\n", st->port);
            printf("               Serial      : %ld %s\n", st->baud, st->serial_line);
            printf("               Radio       : %ld baud%s%s\n", st->radio_baud,
                   st->modem[0] != '\0' ? " / " : "",
                   st->modem[0] != '\0' ? st->modem : "");
            printf("               CALLERID    : %s\n", st->callerid);
            if (st->antenna[0] != '\0')
                printf("               Antenna     : %s\n", st->antenna);
        }
    }

    pr_config_free(&cfg);
    return 0;
}

static int cmd_hash_password(const char *pass)
{
    char hash[160];
    if (pr_hash_password(pass, hash, sizeof hash) != 0) {
        fprintf(stderr, "ERROR: could not create hash\n");
        return 1;
    }
    printf("%s\n", hash);
    fprintf(stderr,
            "Add it to prterm.ini under [admin] pass_hash.\n");
    return 0;
}

static void cmd_channels(const char *id)
{
    const pr_bandplan *bp = (id != NULL) ? pr_bandplan_by_id(id)
                                         : pr_bandplan_default();
    if (bp == NULL) {
        fprintf(stderr, "ERROR: unknown band plan \"%s\"\n", id);
        return;
    }

    printf("%s\nSource: %s\n", bp->name, bp->source);
    printf("Channel spacing: %ld Hz\n\n", bp->bw_hz);
    printf("  Channel  Frequency  Modes       Features\n");
    printf("  -----  -----------  --------  ----------------\n");

    for (size_t i = 0; i < bp->nch; i++) {
        const pr_channel *c = &bp->ch[i];
        char modes[24];
        pr_band_modes_str(c->modes, modes, sizeof modes);

        char flags[24];
        flags[0] = '\0';
        if (c->flags & PR_CH_F_GATEWAY) pr_strlcat(flags, "Gateway ", sizeof flags);
        if (c->flags & PR_CH_F_DATA)    pr_strlcat(flags, "data ",   sizeof flags);

        printf("  %5d  %6.3f MHz  %-8s  %s\n",
               c->num, c->freq_hz / 1000000.0, modes, flags);
    }

    printf("\nPower limits:\n");
    printf("  FM/PM  4 W ERP    Channel 1..80\n");
    printf("  AM     4 W ERP    Channel 1..40\n");
    printf("  SSB   12 W PEP    Channel 1..40\n");
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
    if (strcmp(cmd, "--checkup") == 0) {
        const char *path = argc > 2 ? argv[2] : ini_path;
        char err[256];
        pr_config cfg;
        if (pr_config_load(&cfg, path, err, sizeof err) != 0) {
            fprintf(stderr, "ERROR: %s\n", err);
            pr_config_free(&cfg);
            return 1;
        }
        if (argc > 3 && argv[3][0] != '\0') {
            if (pr_config_apply_station(&cfg, argv[3]) == NULL) {
                fprintf(stderr, "ERROR: unknown station \"%s\"\n", argv[3]);
                fprintf(stderr, "  available:");
                for (size_t k = 0; k < cfg.nstations; k++)
                    fprintf(stderr, " %s", cfg.stations[k].name);
                fprintf(stderr, "\n");
                pr_config_free(&cfg);
                return 1;
            }
        }
        printf("PRTERM checkup\n");
        printf("  Device : %s\n", cfg.port);
        printf("  Station: %s\n\n",
               cfg.active_station[0] ? cfg.active_station : "(global)");

        pr_selftest st;
        int fails = pr_checkup(&cfg, &st);
        pr_selftest_print(&st, stdout);
        pr_config_free(&cfg);
        return fails == 0 ? 0 : 1;
    }
    if (strcmp(cmd, "--selftest") == 0) {
        const char *path = argc > 2 ? argv[2] : ini_path;
        char err[256];
        pr_config cfg;
        if (pr_config_load(&cfg, path, err, sizeof err) != 0) {
            fprintf(stderr, "ERROR: %s\n", err);
            pr_config_free(&cfg);
            return 1;
        }
        /*
         * Select a station. Without argument the first activated one
         * applies - but with several TNCs one must be able to say WHICH
         * device is checked or reset.
         */
        if (argc > 3 && argv[3][0] != '\0') {
            if (pr_config_apply_station(&cfg, argv[3]) == NULL) {
                fprintf(stderr, "ERROR: unknown station \"%s\"\n", argv[3]);
                fprintf(stderr, "  available:");
                for (size_t k = 0; k < cfg.nstations; k++)
                    fprintf(stderr, " %s", cfg.stations[k].name);
                fprintf(stderr, "\n");
                pr_config_free(&cfg);
                return 1;
            }
        }
        printf("PRTERM self-test\n");
        printf("  Device : %s\n", cfg.port);
        printf("  Driver : %s\n\n", cfg.rig_driver);

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
            fprintf(stderr, "ERROR: %s\n", err);
            pr_config_free(&cfg);
            return 1;
        }
        if (argc > 3 && argv[3][0] != '\0') {
            if (pr_config_apply_station(&cfg, argv[3]) == NULL) {
                fprintf(stderr, "ERROR: unknown station \"%s\"\n", argv[3]);
                fprintf(stderr, "  available:");
                for (size_t k = 0; k < cfg.nstations; k++)
                    fprintf(stderr, " %s", cfg.stations[k].name);
                fprintf(stderr, "\n");
                pr_config_free(&cfg);
                return 1;
            }
        }
        printf("PRTERM emergency reset for %s\n\n", cfg.port);

        pr_selftest st;
        int fails = pr_selftest_reset(&cfg, &st);
        pr_selftest_print(&st, stdout);
        pr_config_free(&cfg);
        return fails == 0 ? 0 : 1;
    }
    if (strcmp(cmd, "--hash-password") == 0) {
        if (argc < 3) {
            fprintf(stderr, "ERROR: --hash-password needs a password\n");
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
            fprintf(stderr, "ERROR: %s\n", err);
            return 1;
        }
        fwrite(text, 1, len, stdout);
        free(text);
        return 0;
    }

    fprintf(stderr, "ERROR: unknown option \"%s\"\n\n", cmd);
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
        pr_buf_addf(&res.body, "ERROR: %s\n", err);
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

    /* No webserver: tool mode         */
    if (!pr_is_cgi()) {
        if (argc > 1)
            return cli(argc, argv, ini_path);

        /* No arguments and no CGI: short help, no error         */
        usage(stdout);
        return 0;
    }

    /* CGI: load configuration  */
    char err[256];
    pr_config cfg;
    if (pr_config_load(&cfg, ini_path, err, sizeof err) != 0) {
        pr_response res;
        pr_response_init(&res);
        pr_response_text(&res, 500);
        pr_buf_addf(&res.body, "PRTERM: configuration error\n\n%s\n\n"
                               "(expected: %s)\n", err, ini_path);
        pr_response_emit(&res);
        pr_response_free(&res);
        pr_config_free(&cfg);
        return 1;
    }

    int rc = run_cgi(&cfg);
    pr_config_free(&cfg);
    return rc;
}
