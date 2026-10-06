/*
 * PRTERM - shell tool
 * prterm_ini.c - read and write prterm.ini from the command line.
 *
 * Useful to change configuration from scripts without the CGI or a
 * webserver. Writes comment- and order-preserving.
 *
 *   prterm-ini list        [SECTION]        show keys
 *   prterm-ini get         SECTION KEY
 *   prterm-ini set         SECTION KEY VALUE
 *   prterm-ini del         SECTION KEY
 *   prterm-ini has-section SECTION
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "prterm_compat.h"

#include "ini.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(FILE *f)
{
    fprintf(f,
"prterm-ini - prterm.ini from the command line\n"
"\n"
"  prterm-ini list        [SECTION]\n"
"  prterm-ini get         SECTION KEY\n"
"  prterm-ini set         SECTION KEY VALUE\n"
"  prterm-ini del         SECTION KEY\n"
"  prterm-ini has-section SECTION\n"
"\n"
"The file is given via PRTERM_INI or the --file argument.\n"
"Saving preserves comments and order.\n");
}

static const char *g_path = "prterm.ini";

static bool list_cb(void *ud, const char *sec, const char *key, const char *val)
{
    const char *filter = ud;
    if (filter != NULL && *filter != '\0' && !pr_str_eq_ci(sec, filter))
        return true;
    printf("%s.%s = %s\n", sec, key, val);
    return true;
}

int main(int argc, char **argv)
{
    int argi = 1;

    /* Options first   */
    while (argi < argc && pr_starts_with(argv[argi], "--")) {
        if (strcmp(argv[argi], "--file") == 0 && argi + 1 < argc) {
            g_path = argv[++argi];
            argi++;
        } else if (strcmp(argv[argi], "--help") == 0 ||
                   strcmp(argv[argi], "-h") == 0) {
            usage(stdout);
            return 0;
        } else {
            fprintf(stderr, "prterm-ini: unknown option %s\n", argv[argi]);
            return 2;
        }
    }

    if (argi >= argc) {
        usage(stderr);
        return 2;
    }

    const char *cmd = argv[argi++];
    char err[256];

    ini *i = ini_load(g_path, err, sizeof err);
    if (i == NULL) {
        /* set may also create a new file        */
        if (strcmp(cmd, "set") != 0) {
            fprintf(stderr, "prterm-ini: %s\n", err);
            return 1;
        }
        i = ini_new();
        if (i == NULL) {
            fprintf(stderr, "prterm-ini: out of memory\n");
            return 1;
        }
    }

    int rc = 0;

    if (strcmp(cmd, "list") == 0) {
        ini_foreach(i, argi < argc ? argv[argi] : "", list_cb,
                    (void *)(argi < argc ? argv[argi] : ""));

    } else if (strcmp(cmd, "get") == 0 && argi + 1 < argc) {
        const char *v = ini_get(i, argv[argi], argv[argi + 1], NULL);
        if (v == NULL) {
            fprintf(stderr, "prterm-ini: not found\n");
            rc = 1;
        } else {
            printf("%s\n", v);
        }

    } else if (strcmp(cmd, "set") == 0 && argi + 2 < argc) {
        ini_set(i, argv[argi], argv[argi + 1], argv[argi + 2]);
        if (ini_save(i, g_path, err, sizeof err) != 0) {
            fprintf(stderr, "prterm-ini: %s\n", err);
            rc = 1;
        }

    } else if (strcmp(cmd, "del") == 0 && argi + 1 < argc) {
        if (!ini_del(i, argv[argi], argv[argi + 1])) {
            fprintf(stderr, "prterm-ini: not found\n");
            rc = 1;
        } else if (ini_save(i, g_path, err, sizeof err) != 0) {
            fprintf(stderr, "prterm-ini: %s\n", err);
            rc = 1;
        }

    } else if (strcmp(cmd, "has-section") == 0 && argi < argc) {
        if (ini_has_section(i, argv[argi])) {
            printf("yes\n");
        } else {
            printf("no\n");
            rc = 1;
        }

    } else {
        usage(stderr);
        rc = 2;
    }

    ini_free(i);
    return rc;
}
