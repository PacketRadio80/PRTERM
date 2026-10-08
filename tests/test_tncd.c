/*
 * PRTERM - Test: prterm-tncd against a FAKE TNC on a pseudo terminal
 *
 * No hardware, no RF. The "device" is a PTY and the test plays the
 * TNC2C: it answers the probe with a firmware banner and records every
 * byte the daemon writes to the device. What is checked here is
 * exactly the repair of the TNC2C - KISS held like in the MAX25-Stack:
 *
 *   - KISS is entered ONCE and held: probe -> MYCALL -> KISS entry
 *     -> entry VERIFIED (the probe must stay silent in KISS mode)
 *     -> KISS parameters, in that order
 *   - NO blind C0 FF C0 at cold start - on TheFirmware that is a
 *     firmware reset, and commands sent during the boot are lost
 *     (the false "KISS held" with a deaf PTT)
 *   - CHECKUP of a held KISS repairs in place: leave (C0 FF C0) ->
 *     boot settle -> probe -> MYCALL -> entry -> verify -> parameters,
 *     the port is never closed (a closing fd drops DTR)
 *   - TX relays the KISS frame unchanged and writes NOTHING else -
 *     in KISS mode every written byte would be a transmission
 *   - RX passes received bytes through unchanged
 *   - shutdown leaves KISS with the return frame
 *
 * The fake TNC models KISS mode: after the entry of its class it
 * ignores terminal probes (like the real firmware, whose KISS parser
 * drops bytes outside frames); only C0 FF C0 brings the banner back.
 *
 * Both device classes are run, because they do not speak the same
 * command language (docs/TNC-INIT.md):
 *
 *   esc   TheFirmware (Landolt TNC2C): probe ESC V,  MYCALL as "ESC I"
 *   tapr  TAPR class:                  probe INFO,  MYCALL as command
 *
 * The fake TNC answers only the probe of ITS class and never the other
 * one - a daemon with the wrong probe gets no answer and would have to
 * crawl through the recovery ladder, which the recorded bytes would
 * show (the JHOST sequence of the ladder must never appear).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "prterm_compat.h"

#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include "callsign.h"
#include "config.h"
#include "kiss.h"
#include "radio.h"
#include "tncsock.h"
#include "util.h"
#include "testutil.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* What TheFirmware answers to a probe / reset          */
static const char fake_banner[] = "TheFirmware Version 2.7\r\ncmd: ";

/* The ladder of the recovery - it must never run in these tests. */
static const unsigned char ladder_jhost[] = {
    0x00, 0x01, 0x06, 'J', 'H', 'O', 'S', 'T'
};

/* ======================================================================= */
/* Helpers                                                                 */
/* ======================================================================= */

static const unsigned char *memfind(const unsigned char *hay, size_t hlen,
                                    const char *needle, size_t nlen)
{
    if (nlen == 0 || nlen > hlen)
        return NULL;
    for (size_t i = 0; i + nlen <= hlen; i++) {
        if (memcmp(hay + i, needle, nlen) == 0)
            return hay + i;
    }
    return NULL;
}

/* Offset of a needle in a buffer, (size_t)-1 when absent.                */
static size_t find_at(const unsigned char *buf, size_t len,
                      const char *needle, size_t nlen, size_t from)
{
    if (from >= len)
        return (size_t)-1;
    const unsigned char *p = memfind(buf + from, len - from, needle, nlen);
    return p == NULL ? (size_t)-1 : (size_t)(p - buf);
}

/* ======================================================================= */
/* The fake TNC - a child process on the PTY master                        */
/* ======================================================================= */

/*
 * Reads what the daemon writes to the device, records every byte and
 * answers the probe of ITS device class - and only that one, and only
 * in terminal mode. In KISS mode the firmware ignores terminal bytes
 * (no FEND, no frame); C0 FF C0 resets it back to the banner.
 */
static void fake_tnc_run(int master, const char *recpath, bool tapr)
{
    int rec = open(recpath, O_WRONLY | O_CREAT | O_APPEND, 0644);
    bool in_kiss = false;

    for (;;) {
        unsigned char buf[512];
        ssize_t n = read(master, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;              /* EIO: the daemon closed the port       */
        }
        if (n == 0)
            break;

        if (rec >= 0)
            (void)write(rec, buf, (size_t)n);

        /* C0 FF C0 - the KISS return. On TheFirmware a firmware
         * reset: the banner comes and KISS mode is gone. */
        if (memfind(buf, (size_t)n, "\xc0\xff\xc0", 3) != NULL) {
            in_kiss = false;
            (void)write(master, fake_banner, sizeof fake_banner - 1);
            continue;
        }

        /* In KISS mode the device speaks frames only - terminal
         * probes are ignored. */
        if (in_kiss)
            continue;

        /* The KISS entry of the class switches the mode. */
        bool enter = tapr
            ? memfind(buf, (size_t)n, "kiss on\r", 8) != NULL
            : memfind(buf, (size_t)n, "\x1b@K", 3) != NULL;
        if (enter) {
            in_kiss = true;
            continue;
        }

        /* The probe of the class - answered in terminal mode only. */
        bool probe = tapr ? memfind(buf, (size_t)n, "INFO", 4) != NULL
                          : memfind(buf, (size_t)n, "\x1bV", 2) != NULL;
        if (probe)
            (void)write(master, fake_banner, sizeof fake_banner - 1);
    }

    if (rec >= 0)
        close(rec);
    _exit(0);
}

/* ======================================================================= */
/* INI for one fake station                                                */
/* ======================================================================= */

static bool write_ini(const char *inipath, const char *slave,
                      const char *station, const char *kiss_init,
                      const char *callerid, const char *rundir)
{
    FILE *f = fopen(inipath, "w");
    if (f == NULL)
        return false;

    fprintf(f,
        "[site]\n"
        "name = Test\n\n"
        "[radio]\n"
        "duplex = half\n"
        "driver = tnc2\n"
        "port = %s\n"
        "baud = 19200\n"
        "radio_baud = 2400\n"
        "line = 8n1\n"
        "kiss_init = %s\n"
        "freq_hz = 27235000\n"
        "mode = am\n\n"
        "[station:%s]\n"
        "driver = tnc2\n"
        "port = %s\n"
        "baud = 19200\n"
        "radio_baud = 2400\n"
        "line = 8n1\n"
        "kiss_init = %s\n"
        "callerid = %s\n"
        "enabled = true\n\n"
        "[paths]\n"
        "runtime_dir = %s\n",
        slave, kiss_init, station, slave, kiss_init, callerid, rundir);

    fclose(f);
    return true;
}

static unsigned char *read_record(const char *recpath, size_t *len)
{
    *len = 0;
    FILE *f = fopen(recpath, "rb");
    if (f == NULL)
        return NULL;

    static unsigned char buf[65536];
    *len = fread(buf, 1, sizeof buf, f);
    fclose(f);
    return buf;
}

/* ======================================================================= */
/* One scenario: daemon + fake TNC + commands + recorded bytes             */
/* ======================================================================= */

static void run_scenario(const char *daemon, const char *tag,
                         const char *kiss_init, const char *callerid,
                         bool with_driver)
{
    char dir[128], inipath[192], recpath[192], sockpath[192];
    snprintf(dir, sizeof dir, "test-tncd.runtime-%s", tag);
    snprintf(inipath, sizeof inipath, "%.150s/prterm.ini", dir);
    snprintf(recpath, sizeof recpath, "%.150s/record.bin", dir);
    snprintf(sockpath, sizeof sockpath, "%.150s/tnc-%.16s.sock", dir, tag);

    printf("\n== %s (kiss_init = %s) ==\n", tag, kiss_init);

    /* ---- PTY = the device ------------------------------------------- */
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    CHECK(master >= 0);
    if (master < 0)
        return;
    CHECK(grantpt(master) == 0);
    CHECK(unlockpt(master) == 0);
    const char *slave = ptsname(master);
    CHECK(slave != NULL);
    if (slave == NULL) {
        close(master);
        return;
    }

    /* ---- runtime directory + INI ------------------------------------ */
    (void)mkdir(dir, 0777);
    unlink(recpath);
    unlink(sockpath);
    CHECK(write_ini(inipath, slave, tag, kiss_init, callerid, dir));

    /* ---- the fake TNC ------------------------------------------------ */
    bool tapr = strcmp(kiss_init, "tapr") == 0;
    pid_t tnc_pid = fork();
    CHECK(tnc_pid >= 0);
    if (tnc_pid == 0)
        fake_tnc_run(master, recpath, tapr);

    /* ---- the daemon -------------------------------------------------- */
    pid_t dmn_pid = fork();
    CHECK(dmn_pid >= 0);
    if (dmn_pid == 0) {
        int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) {
            (void)dup2(dn, 1);
            (void)dup2(dn, 2);
            close(dn);
        }
        execl(daemon, daemon, inipath, (char *)NULL);
        _exit(127);
    }

    /* ---- wait for the socket ----------------------------------------
     * The daemon opens KISS first and only then offers its socket -
     * that takes a few seconds (probe, MYCALL, entry, verify). */
    pr_tncsock c;
    memset(&c, 0, sizeof c);
    c.fd = -1;
    bool up = false;
    char err[256];
    for (int i = 0; i < 300 && !up; i++) {
        usleep(100000);
        if (pr_tncsock_open(&c, sockpath, err, sizeof err) == 0)
            up = true;
    }
    CHECK(up);

    if (up) {
        char out[PR_TNCSOCK_MAX_LINE];

        /* PING */
        CHECK(pr_tncsock_cmd(&c, "PING", out, sizeof out, err, sizeof err) == 0);
        CHECK_STR(out, "pong");

        /* STATUS - KISS must be held */
        CHECK(pr_tncsock_cmd(&c, "STATUS", out, sizeof out, err, sizeof err) == 0);
        CHECK(strstr(out, "kiss=held") != NULL);
        CHECK(strstr(out, tag) != NULL);

        /* TX: one KISS DATA frame, relayed unchanged        */
        unsigned char ui[64];
        size_t uilen = ax25_ui_frame(ui, sizeof ui, callerid, "CQ",
                                     (const unsigned char *)"hallo", 5);
        CHECK(uilen > 0);
        unsigned char frame[128];
        size_t flen = kiss_encode(frame, sizeof frame, 0, KISS_CMD_DATA,
                                  ui, uilen);
        CHECK(flen > 0);

        char hex[512];
        pr_tncsock_hex_encode(hex, sizeof hex, frame, flen);
        char cmd[600];
        snprintf(cmd, sizeof cmd, "TX %s", hex);
        CHECK(pr_tncsock_cmd(&c, cmd, out, sizeof out, err, sizeof err) == 0);

        usleep(300000);
        {
            size_t rlen = 0;
            unsigned char *rec = read_record(recpath, &rlen);
            CHECK(rec != NULL && memfind(rec, rlen,
                                         (const char *)frame, flen) != NULL);
        }

        /* RX: what the device receives arrives unchanged. First drain
         * whatever the startup probes left behind. */
        unsigned char ui2[64];
        size_t uilen2 = ax25_ui_frame(ui2, sizeof ui2, "DX1ABC", "CQ",
                                      (const unsigned char *)"test", 4);
        CHECK(uilen2 > 0);
        unsigned char frame2[128];
        size_t flen2 = kiss_encode(frame2, sizeof frame2, 0, KISS_CMD_DATA,
                                   ui2, uilen2);
        CHECK(flen2 > 0);

        unsigned char rx[512];
        (void)pr_tncsock_rx(&c, rx, sizeof rx, err, sizeof err);

        CHECK(write(master, frame2, flen2) == (ssize_t)flen2);
        usleep(800000);

        long n = pr_tncsock_rx(&c, rx, sizeof rx, err, sizeof err);
        CHECK_INT(n, (long)flen2);
        if (n == (long)flen2)
            CHECK(memcmp(rx, frame2, flen2) == 0);

        /* CHECKUP: repair in place, the daemon never closes the port */
        CHECK(pr_tncsock_checkup(&c, err, sizeof err) == 0);

        /* ---- the way the terminal uses it: the tnc2 driver ----------
         * The driver builds the AX.25/KISS frame itself and must also
         * turn received frames back into messages. */
        if (with_driver) {
            pr_config cfg;
            CHECK(pr_config_load(&cfg, inipath, err, sizeof err) == 0);
            CHECK(pr_config_apply_station(&cfg, tag) != NULL);

            pr_rig rig;
            CHECK(pr_rig_open(&rig, &cfg, err, sizeof err) == 0);
            if (rig.vtbl != NULL) {
                CHECK(rig.vtbl->send(&rig, callerid, "CQ", "moin",
                                     err, sizeof err) == 0);

                unsigned char ui3[64];
                size_t uilen3 = ax25_ui_frame(ui3, sizeof ui3, callerid, "CQ",
                                              (const unsigned char *)"moin", 4);
                unsigned char frame3[128];
                size_t flen3 = kiss_encode(frame3, sizeof frame3, 0,
                                           KISS_CMD_DATA, ui3, uilen3);
                CHECK(uilen3 > 0 && flen3 > 0);
                usleep(300000);
                {
                    size_t rlen = 0;
                    unsigned char *rec = read_record(recpath, &rlen);
                    CHECK(rec != NULL && memfind(rec, rlen,
                                                 (const char *)frame3,
                                                 flen3) != NULL);
                }

                pr_msg old[8];
                size_t nold = 0;
                (void)rig.vtbl->refresh(&rig, err, sizeof err);
                (void)rig.vtbl->drain(&rig, old, 8, &nold);

                unsigned char ui4[64];
                size_t uilen4 = ax25_ui_frame(ui4, sizeof ui4, "DX1ABC", "CQ",
                                              (const unsigned char *)"moin2", 5);
                /* The device passes received frames WITH the FCS
                 * trailer - the driver must validate and strip it. */
                uint16_t fcs4 = kiss_fcs(ui4, uilen4);
                ui4[uilen4++] = (unsigned char)(fcs4 & 0xFF);
                ui4[uilen4++] = (unsigned char)(fcs4 >> 8);
                unsigned char frame4[128];
                size_t flen4 = kiss_encode(frame4, sizeof frame4, 0,
                                           KISS_CMD_DATA, ui4, uilen4);
                CHECK(uilen4 > 0 && flen4 > 0);
                CHECK(write(master, frame4, flen4) == (ssize_t)flen4);
                usleep(800000);

                CHECK(rig.vtbl->refresh(&rig, err, sizeof err) == 0);
                pr_msg msgs[8];
                size_t nmsg = 0;
                CHECK(rig.vtbl->drain(&rig, msgs, 8, &nmsg) == 0);
                CHECK_INT(nmsg, 1);
                if (nmsg == 1) {
                    CHECK_STR(msgs[0].from, "DX1ABC");
                    CHECK_STR(msgs[0].to, "CQ");
                    CHECK_STR(msgs[0].text, "moin2");
                    CHECK_STR(msgs[0].station, tag);
                }

                pr_rig_close(&rig);
            }
            pr_config_free(&cfg);
        }
    }

    /* ---- shutdown ---------------------------------------------------- */
    pr_tncsock_close(&c);
    if (up) {
        (void)kill(dmn_pid, SIGTERM);
        (void)waitpid(dmn_pid, NULL, 0);
    } else {
        (void)kill(dmn_pid, SIGKILL);
        (void)waitpid(dmn_pid, NULL, 0);
    }
    usleep(400000);          /* the fake TNC still drains the port      */
    (void)kill(tnc_pid, SIGTERM);
    (void)waitpid(tnc_pid, NULL, 0);
    close(master);

    /* ---- what the daemon wrote to the device ------------------------- */
    size_t rlen = 0;
    unsigned char *rec = read_record(recpath, &rlen);
    CHECK(rec != NULL && rlen > 0);
    if (rec == NULL || rlen == 0)
        return;

    /* the profile's MYCALL and the profile's KISS entry          */
    unsigned char mycall[32];
    size_t mlen;
    if (tapr) {
        mlen = (size_t)snprintf((char *)mycall, sizeof mycall,
                                "MYCALL %s\r", callerid);
    } else {
        mycall[0] = 0x1B;
        mycall[1] = 'I';
        mycall[2] = ' ';
        mlen = 3 + (size_t)snprintf((char *)mycall + 3, sizeof mycall - 3,
                                    "%s\r", callerid);
    }
    static const unsigned char at_k[]  = { 0x1B, 0x40, 0x4B };
    static const unsigned char kiss_on[] = { 'k','i','s','s',' ','o','n','\r' };
    static const unsigned char leave[]   = { 0xC0, 0xFF, 0xC0 };
    static const unsigned char txdelay[] = { 0xC0, 0x01, 0x32, 0xC0 };
    static const unsigned char slottime[]= { 0xC0, 0x03, 0x0A, 0xC0 };
    static const unsigned char persist[] = { 0xC0, 0x02, 0xFF, 0xC0 };
    static const unsigned char txtail[]  = { 0xC0, 0x04, 0x0A, 0xC0 };
    static const unsigned char fulld[]   = { 0xC0, 0x05, 0x00, 0xC0 };

    size_t o_call  = find_at(rec, rlen, (const char *)mycall, mlen, 0);
    size_t o_atk   = find_at(rec, rlen,
                             tapr ? (const char *)kiss_on : (const char *)at_k,
                             tapr ? sizeof kiss_on : sizeof at_k, 0);
    size_t o_txd   = find_at(rec, rlen, (const char *)txdelay, 4, 0);
    size_t o_slot  = find_at(rec, rlen, (const char *)slottime, 4, 0);
    size_t o_pers  = find_at(rec, rlen, (const char *)persist, 4, 0);
    size_t o_tail  = find_at(rec, rlen, (const char *)txtail, 4, 0);
    size_t o_fd    = find_at(rec, rlen, (const char *)fulld, 4, 0);

    /* 1. Cold start enters KISS WITHOUT a preceding C0 FF C0 - on
     *    TheFirmware that is a firmware reset, and resetting a
     *    terminal-mode device only buys a boot race. MYCALL comes
     *    in the language of the class, then the KISS entry. */
    CHECK(o_call != (size_t)-1);
    CHECK(o_atk != (size_t)-1);
    CHECK(o_call < o_atk);

    /* 2. then the channel access parameters                    */
    CHECK(o_txd != (size_t)-1);
    CHECK(o_slot != (size_t)-1);
    CHECK(o_pers != (size_t)-1);
    CHECK(o_tail != (size_t)-1);
    CHECK(o_fd != (size_t)-1);
    CHECK(o_atk < o_txd && o_txd < o_slot && o_slot < o_pers &&
          o_pers < o_tail && o_tail < o_fd);

    /* the probe of the class worked - the recovery ladder did
     * not have to run (its JHOST sequence is the giveaway)      */
    CHECK(memfind(rec, rlen, (const char *)ladder_jhost,
                  sizeof ladder_jhost) == NULL);

    /* 3. CHECKUP repairs a HELD KISS: the first C0 FF C0 in the
     *    record may only come from it - after the first entry.
     *    Then MYCALL and entry a second time, port never closed. */
    size_t o_leave = find_at(rec, rlen, (const char *)leave, 3, 0);
    CHECK(o_leave != (size_t)-1);
    CHECK(o_atk < o_leave);
    size_t o_call2 = find_at(rec, rlen, (const char *)mycall, mlen,
                             o_leave + 3);
    size_t o_atk2 = find_at(rec, rlen,
                            tapr ? (const char *)kiss_on : (const char *)at_k,
                            tapr ? sizeof kiss_on : sizeof at_k,
                            o_leave + 3);
    CHECK(o_call2 != (size_t)-1);
    CHECK(o_atk2 != (size_t)-1);
    CHECK(o_leave < o_call2 && o_call2 < o_atk2);

    /* 4. Shutdown: the daemon leaves KISS with the return frame */
    CHECK(rlen >= 3);
    CHECK(memcmp(rec + rlen - 3, leave, 3) == 0);
}

int main(int argc, char **argv)
{
    const char *daemon = argc > 1 ? argv[1] : "./prterm-tncd";

    printf("== prterm-tncd against a fake TNC ==\n");

    /* TheFirmware class: Landolt TNC2C          */
    run_scenario(daemon, "esc", "esc", "TEST-1", true);

    /* TAPR class: PK-TNC2                        */
    run_scenario(daemon, "tapr", "tapr", "TEST-2", false);

    TEST_SUMMARY("tncd");
}
