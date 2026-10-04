/*
 * PRTERM - Shell-Werkzeug
 * prterm_ini.c - prterm.ini von der Kommandozeile lesen und schreiben.
 *
 * Nutzlich, um Konfiguration aus Skripten zu aendern, ohne das CGI oder
 * einen Webserver zu benoetigen. Schreibt kommentar- und reihenfolgetreu.
 *
 *   prterm-ini list        [SEKTION]        Schluessel anzeigen
 *   prterm-ini get         SEKTION SCHLUESSEL
 *   prterm-ini set         SEKTION SCHLUESSEL WERT
 *   prterm-ini del         SEKTION SCHLUESSEL
 *   prterm-ini has-section SEKTION
 *
 * SPDX-License-Identifier: MIT
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
"prterm-ini - prterm.ini von der Kommandozeile\n"
"\n"
"  prterm-ini list        [SEKTION]\n"
"  prterm-ini get         SEKTION SCHLUESSEL\n"
"  prterm-ini set         SEKTION SCHLUESSEL WERT\n"
"  prterm-ini del         SEKTION SCHLUESSEL\n"
"  prterm-ini has-section SEKTION\n"
"\n"
"Die Datei wird ueber PRTERM_INI oder das Argument --file angegeben.\n"
"Speichern ist kommentar- und reihenfolgetreu.\n");
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

    /* Optionen vorweg */
    while (argi < argc && pr_starts_with(argv[argi], "--")) {
        if (strcmp(argv[argi], "--file") == 0 && argi + 1 < argc) {
            g_path = argv[++argi];
            argi++;
        } else if (strcmp(argv[argi], "--help") == 0 ||
                   strcmp(argv[argi], "-h") == 0) {
            usage(stdout);
            return 0;
        } else {
            fprintf(stderr, "prterm-ini: unbekannte Option %s\n", argv[argi]);
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
        /* set darf auch eine neue Datei anlegen */
        if (strcmp(cmd, "set") != 0) {
            fprintf(stderr, "prterm-ini: %s\n", err);
            return 1;
        }
        i = ini_new();
        if (i == NULL) {
            fprintf(stderr, "prterm-ini: Speicher erschoepft\n");
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
            fprintf(stderr, "prterm-ini: nicht gefunden\n");
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
            fprintf(stderr, "prterm-ini: nicht gefunden\n");
            rc = 1;
        } else if (ini_save(i, g_path, err, sizeof err) != 0) {
            fprintf(stderr, "prterm-ini: %s\n", err);
            rc = 1;
        }

    } else if (strcmp(cmd, "has-section") == 0 && argi < argc) {
        if (ini_has_section(i, argv[argi])) {
            printf("ja\n");
        } else {
            printf("nein\n");
            rc = 1;
        }

    } else {
        usage(stderr);
        rc = 2;
    }

    ini_free(i);
    return rc;
}
