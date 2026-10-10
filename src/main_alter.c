/*
 * PRTERM Alter - CB & Amateur Radio Terminal (retro CGI)
 * main_alter.c — Entry point for the alternative UI.
 *
 * Same core as prterm.cgi, different page rendering.
 * Works in text browsers (lynx, links, w3m) and on retro computers.
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
/* Find the configuration file (same logic as main.c)                      */
/* ======================================================================= */

static void find_ini(int argc, char **argv, char *dst, size_t dstlen)
{
    const char *env = getenv("PRTERM_INI");
    if (env != NULL && env[0] != '\0') {
        pr_strlcpy(dst, env, dstlen);
        return;
    }

    {
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
/* CLI (same as main.c, but with "alter" branding)                         */
/* ======================================================================= */

static void usage(FILE *f)
{
    fprintf(f,
"PRTERM Alter %s — Retro UI for CB & Amateur Radio Terminal\n"
"\n"
"Same core as prterm.cgi, alternative rendering for text browsers\n"
"and retro computers (lynx, links, w3m).\n"
"\n"
"Usage:\n"
"  prterm-alter.cgi                  run as CGI from the webserver\n"
"  prterm-alter.cgi [OPTION]         run as a command line tool\n"
"\n"
"Options:\n"
"  -h, --help                this help\n"
"  -V, --version             version\n"
"  --check-ini [FILE]        check configuration\n"
"  --print-config [FILE]     show effective configuration\n"
"  --gen-ini [FILE]          generate sample configuration\n"
"  --hash-password PASS      password hash for [admin] pass_hash\n"
"  --channels [BANDPLAN]     show channel table\n"
"\n"
"Environment:\n"
"  PRTERM_INI                path to prterm.ini\n",
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
    fprintf(stderr, "Add it to prterm.ini under [admin] pass_hash.\n");
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
        printf("  %5d  %6.3f MHz  %-8s\n",
               c->num, c->freq_hz / 1000000.0, modes);
    }
}

static int cli(int argc, char **argv, const char *ini_path)
{
    const char *cmd = argv[1];

    if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0) {
        usage(stdout);
        return 0;
    }
    if (strcmp(cmd, "-V") == 0 || strcmp(cmd, "--version") == 0) {
        printf("PRTERM Alter %s\n", PRTERM_VERSION);
        return 0;
    }
    if (strcmp(cmd, "--check-ini") == 0) {
        return cmd_check_ini(argc > 2 ? argv[2] : ini_path);
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
    if (strcmp(cmd, "--gen-ini") == 0) {
        const char *path = argc > 2 ? argv[2] : "prterm.ini";
        char err[256];
        pr_config cfg;
        ini *i;

        pr_config_defaults(&cfg);
        i = ini_new();
        if (i == NULL) {
            fprintf(stderr, "ERROR: out of memory\n");
            pr_config_free(&cfg);
            return 1;
        }
        pr_config_write(&cfg, i);
        if (ini_save(i, path, err, sizeof err) != 0) {
            fprintf(stderr, "ERROR: %s\n", err);
            ini_free(i);
            pr_config_free(&cfg);
            return 1;
        }
        printf("wrote sample configuration: %s\n", path);
        ini_free(i);
        pr_config_free(&cfg);
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

    if (!pr_is_cgi()) {
        if (argc > 1)
            return cli(argc, argv, ini_path);
        usage(stdout);
        return 0;
    }

    char err[256];
    pr_config cfg;
    if (pr_config_load(&cfg, ini_path, err, sizeof err) != 0) {
        pr_response res;
        pr_response_init(&res);
        pr_response_text(&res, 500);
        pr_buf_addf(&res.body, "PRTERM Alter: configuration error\n\n%s\n\n"
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