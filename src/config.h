/*
 * PRTERM - CB & Amateur Radio Terminal
 * config.h - typed configuration model on top of prterm.ini.
 *
 * The INI remains the only persistence; this model is just the
 * parsed, validated access to it.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
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

/* Ban entry: pattern in [ban], value = reason */
typedef struct pr_ban {
    char pattern[PR_CALLSIGN_MAX + 4];
    char reason[128];
} pr_ban;

/*
 * One station = TNC + own radio + own antenna.
 *
 * Each station is self-contained. radio_baud is HARDWARE and is
 * deliberately not sent to the device - it is a property, not a
 * command.
 */
typedef struct pr_station {
    char name[32];                 /* Section name: [station:NAME]     */
    char rig_driver[32];
    char port[PR_CFG_PATH];
    long baud;                     /* serial to the TNC                   */
    long radio_baud;               /* FIXED - hardware, not changeable   */
    char modem[32];
    char serial_line[8];
    char kiss_init[16];           /* "esc" or "tapr" - see radio.h     */
    /*
     * How transmission works:
     *   kiss     KISS data frames
     *   unproto  leave KISS, send "UNPROTO <dest> 0 <text>",
     *            re-enter KISS
     *
     * TheFirmware (TNC2 class) ignores KISS data on hybrid setups,
     * while UNPROTO from command mode switches the carrier reliably.
     * That is why "unproto" is the safe choice.
     */
    char tx_mode[16];
    char callerid[PR_CALLSIGN_MAX];
    char antenna[64];              /* Description, for display             */
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
    /*
     * Name of the currently active station. Set by
     * pr_config_apply_station so the driver can assign received
     * messages to the device that picked them up.
     */
    char active_station[32];

    /* [radio] */
    pr_duplex duplex;
    char rig_driver[32];
    char port[PR_CFG_PATH];
    long baud;                    /* serial: Host <-> TNC             */
    long radio_baud;              /* on the channel: 2400 / 1200       */
    char modem[32];               /* modem type in the TNC, e.g. tcm3105   */
    char serial_line[8];          /* "8n1", "7e1", ... */
    /*
     * How the TNC is put into KISS mode. The devices differ here,
     * see docs/TNC-INIT.md:
     *   esc   1B 40 4B  (ESC @K, no \r)     - Landolt TNC2C
     *   tapr  "kiss on\r"                   - PK-TNC2, TAPR class
     */
    char kiss_init[16];
    char tx_mode[16];
    long freq_hz;
    unsigned mode;            /* PR_BAND_FM / _AM / _SSB */
    long tx_power_mw;         /* TX power for the compliance check          */
    int  rx_poll_ms;
    int  max_log;

    /* [admin] */
    bool admin_enabled;
    char admin_user[PR_CFG_STR];
    char admin_pass_hash[160];
    int  session_ttl_min;
    bool allow_guest_tx;

    /*
     * [mailboxd]
     *
     * MailboxD is a separate daemon that PRTERM can drive locally. When this
     * is off the "Mailbox" tab is not rendered at all - the button must not
     * even be discoverable, so there is nothing to explain to an operator who
     * never installed it.
     */
    bool mailboxd_enabled;
    /*
     * Installation directory. MailboxD runs entirely from its own directory;
     * only service and start scripts are exposed to the system. Empty means
     * use the built-in search order
     * (/var/mailboxd, /usr/mailboxd, /usr/local/mailboxd).
     */
    char mailboxd_dir[PR_CFG_PATH];

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

    /* [station:*] - several complete stations      */
    pr_station stations[PR_MAX_STATIONS];
    size_t nstations;

    /* [paths] */
    char runtime_dir[PR_CFG_PATH];

    /* internal */
    char ini_path[PR_CFG_PATH];
    const pr_bandplan *bandplan;
    ini *raw;
} pr_config;

/* ---- Lifecycle    ----------------------------------------------------- */
void pr_config_defaults(pr_config *cfg);
int  pr_config_load(pr_config *cfg, const char *path, char *err, size_t errlen);
void pr_config_free(pr_config *cfg);

/* Adopt values from an already loaded INI (validation included).         */
int  pr_config_apply(pr_config *cfg, const ini *i, char *err, size_t errlen);

/* Write current values back into an INI.       */
void pr_config_write(const pr_config *cfg, ini *i);

/* ---- Ban list  -------------------------------------------------------- */
bool pr_config_is_banned(const pr_config *cfg, const char *id);
int  pr_config_add_ban(pr_config *cfg, const char *pattern, const char *reason);
bool pr_config_del_ban(pr_config *cfg, const char *pattern);
const pr_ban *pr_config_find_ban(const pr_config *cfg, const char *pattern);

/* ---- Helper functions --------------------------------------------------- */
const char *pr_duplex_name(pr_duplex d);
pr_duplex   pr_duplex_from_name(const char *s);

/* ---- Stations  ------------------------------------------------------- */
/*
 * Applies a station's device settings to the configuration.
 *
 * Each station is a complete unit: own TNC, own radio, own antenna.
 * When a station tab is selected in the browser, port, baud and
 * radio baud of that STATION must apply - not the global values
 * from [radio].
 *
 * Returns the station or NULL if unknown.
 */
const pr_station *pr_config_apply_station(pr_config *cfg, const char *name);
const pr_station *pr_config_station(const pr_config *cfg, const char *name);
/* First activated station, if none was selected.           */
const pr_station *pr_config_default_station(const pr_config *cfg);
/* Channel number for the current frequency, -1 if none. */
int         pr_config_channel(const pr_config *cfg);
/* Set frequency for a channel number; false if invalid.       */
bool        pr_config_set_channel(pr_config *cfg, int num);

#endif /* PRTERM_CONFIG_H */
