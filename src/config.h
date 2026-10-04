/*
 * PRTERM - CB & Amateur Radio Terminal
 * config.h - typisiertes Konfigurationsmodell ueber der prterm.ini.
 *
 * Die INI bleibt die einzige Persistenz; dieses Modell ist nur der
 * geparste, validierte Zugriff darauf.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PRTERM_CONFIG_H
#define PRTERM_CONFIG_H

#include "bands.h"
#include "callsign.h"
#include "ini.h"

#include <stdbool.h>
#include <stddef.h>

#define PR_CFG_STR  64
#define PR_CFG_PATH 512

/* Ban-Eintrag: Muster in [ban], Wert = Grund */
typedef struct pr_ban {
    char pattern[PR_CALLSIGN_MAX + 4];
    char reason[128];
} pr_ban;

/*
 * Eine Station = TNC + eigenes Funkgerät + eigene Antenne.
 *
 * Jede Station ist in sich geschlossen. radio_baud ist HARDWARE und wird
 * bewusst nicht an das Gerät gesendet - es ist eine Eigenschaft, keine
 * Anweisung.
 */
typedef struct pr_station {
    char name[32];                 /* Sektionsname: [station:NAME]     */
    char rig_driver[32];
    char port[PR_CFG_PATH];
    long baud;                     /* seriell zum TNC                   */
    long radio_baud;               /* FEST - Hardware, nicht änderbar   */
    char modem[32];
    char serial_line[8];
    char callerid[PR_CALLSIGN_MAX];
    char antenna[64];              /* Beschreibung, für die Anzeige     */
    bool enabled;
} pr_station;

#define PR_MAX_STATIONS 4

typedef enum pr_duplex {
    PR_DUPLEX_HALF = 0,
    PR_DUPLEX_FULL = 1
} pr_duplex;

typedef struct pr_config {
    /* [site] */
    char site_name[PR_CFG_STR];
    char site_subtitle[96];
    char language[8];

    /* [station] */
    char callerid[PR_CALLSIGN_MAX];
    char qth[PR_CFG_STR];
    char locator[16];

    /* [radio] */
    pr_duplex duplex;
    char rig_driver[32];
    char port[PR_CFG_PATH];
    long baud;                    /* seriell: Host <-> TNC            */
    long radio_baud;              /* auf dem Kanal: 2400 / 1200       */
    char modem[32];               /* Modem-Typ im TNC, z.B. tcm3105   */
    char serial_line[8];          /* "8n1", "7e1", ... */
    long freq_hz;
    unsigned mode;            /* PR_BAND_FM / _AM / _SSB */
    long tx_power_mw;         /* Sendeleistung fuer die Compliance-Pruefung */
    int  rx_poll_ms;
    int  max_log;

    /* [admin] */
    bool admin_enabled;
    char admin_user[PR_CFG_STR];
    char admin_pass_hash[160];
    int  session_ttl_min;
    bool allow_guest_tx;

    /* [callsign] */
    pr_call_rules callsign;

    /* [ui] */
    char font_file[PR_CFG_PATH];
    int  font_size;
    int  line_height_pct;     /* 120 == 1.2 */
    char ui_density[16];
    int  ui_rows;
    int  ui_columns;
    char ui_theme[16];

    /* [ban] */
    pr_ban *bans;
    size_t nbans;
    size_t cap_bans;

    /* [station:*] - mehrere vollständige Stationen */
    pr_station stations[PR_MAX_STATIONS];
    size_t nstations;

    /* [paths] */
    char runtime_dir[PR_CFG_PATH];

    /* intern */
    char ini_path[PR_CFG_PATH];
    const pr_bandplan *bandplan;
    ini *raw;
} pr_config;

/* ---- Lebenszyklus ----------------------------------------------------- */
void pr_config_defaults(pr_config *cfg);
int  pr_config_load(pr_config *cfg, const char *path, char *err, size_t errlen);
void pr_config_free(pr_config *cfg);

/* Werte aus einem bereits geladenen INI uebernehmen (Validierung inkl.). */
int  pr_config_apply(pr_config *cfg, const ini *i, char *err, size_t errlen);

/* Aktuelle Werte zurueck in ein INI schreiben. */
void pr_config_write(const pr_config *cfg, ini *i);

/* ---- Ban-Liste -------------------------------------------------------- */
bool pr_config_is_banned(const pr_config *cfg, const char *id);
int  pr_config_add_ban(pr_config *cfg, const char *pattern, const char *reason);
bool pr_config_del_ban(pr_config *cfg, const char *pattern);
const pr_ban *pr_config_find_ban(const pr_config *cfg, const char *pattern);

/* ---- Hilfsfunktionen -------------------------------------------------- */
const char *pr_duplex_name(pr_duplex d);
pr_duplex   pr_duplex_from_name(const char *s);
/* Kanalnummer zur aktuellen Frequenz, -1 wenn keiner. */
int         pr_config_channel(const pr_config *cfg);
/* Frequenz zu einer Kanalnummer setzen; false wenn ungueltig. */
bool        pr_config_set_channel(pr_config *cfg, int num);

#endif /* PRTERM_CONFIG_H */
