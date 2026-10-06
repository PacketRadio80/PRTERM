/*
 * PRTERM - Test: prterm-tncd against a FAKE TNC on a pseudo terminal
 *
 * No hardware, no RF. The "device" is a PTY and the test plays the
 * TNC2C: it answers the ESC V probe with a firmware banner and records
 * every byte the daemon writes to the device. What is checked here is
 * exactly the repair of the TNC2C - KISS held like in the MAX25-Stack:
 *
 *   - KISS is entered ONCE and held: leave KISS (C0 FF C0) -> MYCALL
 *     -> ESC @K -> KISS parameters, in that order
 *   - TX relays the KISS frame unchanged and writes NOTHING else -
 *     in KISS mode every written byte would be a transmission
 *   - RX passes received bytes through unchanged
 *   - CHECKUP repairs in place: leave KISS again, re-enter, the port
 *     is never closed (a closing fd drops DTR)
 *   - shutdown leaves KISS with the return frame
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "prterm_compat.h"

/*
 * posix_openpt & friends are XSI - glibc only declares them with
 * _XOPEN_SOURCE. The project baseline is POSIX.1-2008/XSI, so this is
 * the right lever. Must stand before the first system header.
 */
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

#define TEST_DIR   "test-tncd.runtime"
#define TEST_INI   TEST_DIR "/prterm.ini"
#define TEST_REC   TEST_DIR "/record.bin"
#define TEST_SOCK  TEST_DIR "/tnc-fake1.sock"

/* What TheFirmware answers to a probe / reset          */
static const char fake_banner[] = "TheFirmware Version 2.7\r\ncmd: ";

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
 * answers the two probes like the TNC2C firmware does. No more - a real
 * TNC in command mode says nothing to anything else either.
 */
static void fake_tnc_run(int master, const char *recpath)
{
    int rec = open(recpath, O_WRONLY | O_CREAT | O_APPEND, 0644);

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

        /* ESC V: the probe. C0 FF C0: KISS return - on TheFirmware a
         * firmware reset, which brings the banner. */
        bool probe = memfind(buf, (size_t)n, "\x1bV", 2) != NULL;
        bool reset = memfind(buf, (size_t)n, "\xc0\xff\xc0", 3) != NULL;
        if (probe || reset)
            (void)write(master, fake_banner, sizeof fake_banner - 1);
    }

    if (rec >= 0)
        close(rec);
    _exit(0);
}

/* ======================================================================= */
/* INI for one fake station                                                */
/* ======================================================================= */

static bool write_ini(const char *slave)
{
    FILE *f = fopen(TEST_INI, "w");
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
        "kiss_init = esc\n"
        "freq_hz = 27235000\n"
        "mode = am\n\n"
        "[station:fake1]\n"
        "driver = tnc2\n"
        "port = %s\n"
        "baud = 19200\n"
        "radio_baud = 2400\n"
        "line = 8n1\n"
        "kiss_init = esc\n"
        "callerid = TEST-1\n"
        "enabled = true\n\n"
        "[paths]\n"
        "runtime_dir = %s\n",
        slave, slave, TEST_DIR);

    fclose(f);
    return true;
}

/* ======================================================================= */
/* Recording                                                               */
/* ======================================================================= */

static unsigned char *read_record(size_t *len)
{
    *len = 0;
    FILE *f = fopen(TEST_REC, "rb");
    if (f == NULL)
        return NULL;

    static unsigned char buf[65536];
    *len = fread(buf, 1, sizeof buf, f);
    fclose(f);
    return buf;
}

int main(int argc, char **argv)
{
    const char *daemon = argc > 1 ? argv[1] : "./prterm-tncd";
    char err[256];

    printf("== prterm-tncd against a fake TNC ==\n");

    /* ---- PTY = the device ------------------------------------------- */
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    CHECK(master >= 0);
    if (master < 0)
        TEST_SUMMARY("tncd");
    CHECK(grantpt(master) == 0);
    CHECK(unlockpt(master) == 0);
    const char *slave = ptsname(master);
    CHECK(slave != NULL);
    if (slave == NULL)
        TEST_SUMMARY("tncd");
    printf("  device: %s\n", slave);

    /* ---- runtime directory + INI ------------------------------------ */
    (void)mkdir(TEST_DIR, 0777);
    unlink(TEST_REC);
    unlink(TEST_SOCK);
    CHECK(write_ini(slave));

    /* ---- the fake TNC ------------------------------------------------ */
    pid_t tnc_pid = fork();
    CHECK(tnc_pid >= 0);
    if (tnc_pid == 0) {
        fake_tnc_run(master, TEST_REC);
    }

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
        execl(daemon, daemon, TEST_INI, (char *)NULL);
        _exit(127);
    }

    /* ---- wait for the socket ----------------------------------------
     * The daemon opens KISS first and only then offers its socket -
     * that takes a few seconds (leave KISS, probe, MYCALL, entry). */
    pr_tncsock c;
    memset(&c, 0, sizeof c);
    c.fd = -1;
    bool up = false;
    for (int i = 0; i < 300 && !up; i++) {
        usleep(100000);
        if (pr_tncsock_open(&c, TEST_SOCK, err, sizeof err) == 0)
            up = true;
    }
    CHECK(up);
    if (!up)
        goto done;

    /* ---- commands ---------------------------------------------------- */
    {
        char out[PR_TNCSOCK_MAX_LINE];

        /* PING */
        CHECK(pr_tncsock_cmd(&c, "PING", out, sizeof out,
                             err, sizeof err) == 0);
        CHECK_STR(out, "pong");

        /* STATUS - KISS must be held */
        CHECK(pr_tncsock_cmd(&c, "STATUS", out, sizeof out,
                             err, sizeof err) == 0);
        CHECK(strstr(out, "kiss=held") != NULL);
        CHECK(strstr(out, "station=fake1") != NULL);

        /* TX: one KISS DATA frame, relayed unchanged        */
        unsigned char ui[64];
        size_t uilen = ax25_ui_frame(ui, sizeof ui, "TEST-1", "CQ",
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
            unsigned char *rec = read_record(&rlen);
            CHECK(rec != NULL && rlen > 0);
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
    }

    /* ---- the way the terminal uses it: the tnc2 driver -------------
     * The driver builds the AX.25/KISS frame itself and must also
     * turn received frames back into messages. */
    {
        pr_config cfg;
        CHECK(pr_config_load(&cfg, TEST_INI, err, sizeof err) == 0);
        CHECK(pr_config_apply_station(&cfg, "fake1") != NULL);

        pr_rig rig;
        CHECK(pr_rig_open(&rig, &cfg, err, sizeof err) == 0);
        if (rig.vtbl != NULL) {
            /* send: driver -> KISS frame -> daemon -> device          */
            CHECK(rig.vtbl->send(&rig, "TEST-1", "CQ", "moin",
                                 err, sizeof err) == 0);

            unsigned char ui3[64];
            size_t uilen3 = ax25_ui_frame(ui3, sizeof ui3, "TEST-1", "CQ",
                                          (const unsigned char *)"moin", 4);
            unsigned char frame3[128];
            size_t flen3 = kiss_encode(frame3, sizeof frame3, 0,
                                       KISS_CMD_DATA, ui3, uilen3);
            CHECK(uilen3 > 0 && flen3 > 0);
            usleep(300000);
            {
                size_t rlen = 0;
                unsigned char *rec = read_record(&rlen);
                CHECK(rec != NULL && memfind(rec, rlen,
                                             (const char *)frame3,
                                             flen3) != NULL);
            }

            /* receive: a UI frame has to arrive as a message          */
            pr_msg old[8];
            size_t nold = 0;
            (void)rig.vtbl->refresh(&rig, err, sizeof err);
            (void)rig.vtbl->drain(&rig, old, 8, &nold);

            unsigned char ui4[64];
            size_t uilen4 = ax25_ui_frame(ui4, sizeof ui4, "DX1ABC", "CQ",
                                          (const unsigned char *)"moin2", 5);
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
                CHECK_STR(msgs[0].station, "fake1");
            }

            pr_rig_close(&rig);
        }
        pr_config_free(&cfg);
    }

done:
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
    {
        size_t rlen = 0;
        unsigned char *rec = read_record(&rlen);
        CHECK(rec != NULL && rlen > 0);
        if (rec != NULL && rlen > 0) {
            static const unsigned char leave[]  = { 0xC0, 0xFF, 0xC0 };
            static const unsigned char mycall[] = {
                0x1B, 'I', ' ', 'T', 'E', 'S', 'T', '-', '1', '\r'
            };
            static const unsigned char at_k[]   = { 0x1B, 0x40, 0x4B };
            static const unsigned char txdelay[]= { 0xC0, 0x01, 0x32, 0xC0 };
            static const unsigned char slottime[]={ 0xC0, 0x03, 0x0A, 0xC0 };
            static const unsigned char persist[]= { 0xC0, 0x02, 0xFF, 0xC0 };
            static const unsigned char txtail[] = { 0xC0, 0x04, 0x0A, 0xC0 };
            static const unsigned char fulld[]  = { 0xC0, 0x05, 0x00, 0xC0 };

            size_t o_leave = find_at(rec, rlen, (const char *)leave,  3, 0);
            size_t o_call  = find_at(rec, rlen, (const char *)mycall, sizeof mycall, 0);
            size_t o_atk   = find_at(rec, rlen, (const char *)at_k,   3, 0);
            size_t o_txd   = find_at(rec, rlen, (const char *)txdelay, 4, 0);
            size_t o_slot  = find_at(rec, rlen, (const char *)slottime, 4, 0);
            size_t o_pers  = find_at(rec, rlen, (const char *)persist, 4, 0);
            size_t o_tail  = find_at(rec, rlen, (const char *)txtail, 4, 0);
            size_t o_fd    = find_at(rec, rlen, (const char *)fulld,  4, 0);

            /* 1. leave KISS first - a control frame, nothing on the air */
            CHECK(o_leave != (size_t)-1);
            /* 2. MYCALL - KISS DATA is only keyed with an identity     */
            CHECK(o_call != (size_t)-1);
            /* 3. then KISS entry                                       */
            CHECK(o_atk != (size_t)-1);
            CHECK(o_leave < o_call && o_call < o_atk);

            /* 4. then the channel access parameters                    */
            CHECK(o_txd != (size_t)-1);
            CHECK(o_slot != (size_t)-1);
            CHECK(o_pers != (size_t)-1);
            CHECK(o_tail != (size_t)-1);
            CHECK(o_fd != (size_t)-1);
            CHECK(o_atk < o_txd && o_txd < o_slot && o_slot < o_pers &&
                  o_pers < o_tail && o_tail < o_fd);

            /* CHECKUP ran again: leave KISS + entry a second time       */
            size_t o_leave2 = find_at(rec, rlen, (const char *)leave, 3,
                                      o_leave + 3);
            size_t o_atk2   = find_at(rec, rlen, (const char *)at_k, 3,
                                      o_atk + 3);
            CHECK(o_leave2 != (size_t)-1);
            CHECK(o_atk2 != (size_t)-1);

            /* Shutdown: the daemon leaves KISS with the return frame    */
            CHECK(rlen >= 3);
            CHECK(memcmp(rec + rlen - 3, leave, 3) == 0);
        }
    }

    TEST_SUMMARY("tncd");
}
