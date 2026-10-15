/*
 * PRTERM - Test: prterm-tncd + tmodem driver against a FAKE T-Modem
 *
 * No hardware, no RF. The "device" is a PTY and the test plays the
 * T-Modem-c1224/s1224 half-TNC: a Pico that is in KISS mode from
 * power-up and never leaves it. What is checked here is exactly the
 * contract the real firmware provides (CONTINUE.md section 16):
 *
 *   - kiss_init = native: the daemon writes NOTHING at startup -
 *     no terminal probe, no MYCALL, no KISS entry, no parameters.
 *     The record must stay EMPTY until the first TX.
 *   - TX relays the KISS frame unchanged and writes NOTHING else.
 *   - RX frames arrive WITHOUT FCS (the firmware validates and strips
 *     the trailer itself) - the tmodem driver must accept them,
 *     while the tnc2 FCS logic would drop every single frame.
 *   - A source labelled "CALL@station" is logged with the label while
 *     the frame on the air keeps the base call.
 *   - shutdown writes C0 FF C0 (harmless for this firmware: a
 *     non-DATA frame, ignored, no transmission).
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
#include "state.h"
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

/* ======================================================================= */
/* The fake T-Modem - a child process on the PTY master                    */
/* ======================================================================= */

/*
 * The T-Modem is ALWAYS in KISS mode. Terminal probes (ESC V, INFO)
 * get no answer - the firmware's parser drops bytes outside frames.
 * A complete KISS DATA frame triggers a "transmission": the fake
 * answers with a canned received frame - WITHOUT FCS, like the real
 * firmware's hdlc.c which checks the CRC on air and strips it.
 */
static void fake_tmodem_run(int master, const char *recpath)
{
    int rec = open(recpath, O_WRONLY | O_CREAT | O_APPEND, 0644);

    /* What the fake "hears" on the channel: DX1ABC -> CQ, "moin2"    */
    unsigned char ui[64];
    size_t uilen = ax25_ui_frame(ui, sizeof ui, "DX1ABC", "CQ",
                                 (const unsigned char *)"moin2", 5);
    unsigned char rxframe[128];
    size_t rxlen = 0;
    if (uilen > 0)
        rxlen = kiss_encode(rxframe, sizeof rxframe, 0, KISS_CMD_DATA,
                            ui, uilen);

    unsigned char carry[512];
    size_t clen = 0;                       /* partial frame carry-over */

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

        /* Re-assemble across read boundaries, then look for complete
         * KISS DATA frames (FEND 0x00 ... FEND). */
        for (ssize_t i = 0; i < n; i++) {
            if (clen < sizeof carry)
                carry[clen++] = buf[i];
            if (buf[i] != KISS_FEND || clen < 3)
                continue;
            if (carry[0] == KISS_FEND && carry[clen - 1] == KISS_FEND &&
                (carry[1] & 0x0Fu) == KISS_CMD_DATA) {
                /* "Transmit" - and the channel delivers a frame back,
                 * without FCS, like the real firmware.               */
                if (rxlen > 0)
                    (void)write(master, rxframe, rxlen);
            }
            clen = 0;
        }
    }

    if (rec >= 0)
        close(rec);
    _exit(0);
}

/* ======================================================================= */
/* INI for one fake station                                                */
/* ======================================================================= */

static bool write_ini(const char *inipath, const char *slave,
                      const char *station, const char *callerid,
                      const char *rundir)
{
    FILE *f = fopen(inipath, "w");
    if (f == NULL)
        return false;

    fprintf(f,
        "[site]\n"
        "name = Test\n\n"
        "[radio]\n"
        "duplex = half\n"
        "driver = tmodem\n"
        "port = %s\n"
        "baud = 115200\n"
        "radio_baud = 2400\n"
        "line = 8n1\n"
        "kiss_init = native\n"
        "freq_hz = 26675000\n"
        "mode = fm\n\n"
        "[station:%s]\n"
        "driver = tmodem\n"
        "port = %s\n"
        "baud = 115200\n"
        "radio_baud = 2400\n"
        "modem = softmodem\n"
        "line = 8n1\n"
        "kiss_init = native\n"
        "callerid = %s\n"
        "enabled = true\n\n"
        "[paths]\n"
        "runtime_dir = %s\n",
        slave, station, slave, callerid, rundir);

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
/* The scenario                                                            */
/* ======================================================================= */

static void run_scenario(const char *daemon, const char *tag,
                         const char *callerid)
{
    char dir[128], inipath[192], recpath[192], sockpath[192];
    snprintf(dir, sizeof dir, "test-tmodem.runtime-%s", tag);
    snprintf(inipath, sizeof inipath, "%.150s/prterm.ini", dir);
    snprintf(recpath, sizeof recpath, "%.150s/record.bin", dir);
    snprintf(sockpath, sizeof sockpath, "%.150s/tnc-%.16s.sock", dir, tag);

    printf("\n== %s (driver = tmodem, kiss_init = native) ==\n", tag);

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
    CHECK(write_ini(inipath, slave, tag, callerid, dir));

    /* ---- the fake T-Modem ------------------------------------------- */
    pid_t tnc_pid = fork();
    CHECK(tnc_pid >= 0);
    if (tnc_pid == 0)
        fake_tmodem_run(master, recpath);

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
     * native mode: no probe, no entry - the socket comes up fast. */
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

        /* 1. THE CONTRACT: with kiss_init=native the daemon wrote
         *    NOTHING to the device at startup - no probe, no MYCALL,
         *    no KISS entry, no parameters. The record is still empty. */
        usleep(200000);
        {
            size_t rlen = 0;
            unsigned char *rec = read_record(recpath, &rlen);
            CHECK(rec != NULL && rlen == 0);
        }

        /* ---- the way the terminal uses it: the tmodem driver ------- */
        pr_config cfg;
        CHECK(pr_config_load(&cfg, inipath, err, sizeof err) == 0);
        CHECK(pr_config_apply_station(&cfg, tag) != NULL);

        pr_rig rig;
        CHECK(pr_rig_open(&rig, &cfg, err, sizeof err) == 0);
        if (rig.vtbl != NULL) {

            /* TX: the driver builds the frame, the daemon relays it
             * unchanged - and NOTHING else is written.               */
            CHECK(rig.vtbl->send(&rig, callerid, "CQ", "moin",
                                 err, sizeof err) == 0);

            unsigned char ui1[64];
            size_t uilen1 = ax25_ui_frame(ui1, sizeof ui1, callerid, "CQ",
                                          (const unsigned char *)"moin", 4);
            unsigned char frame1[128];
            size_t flen1 = kiss_encode(frame1, sizeof frame1, 0,
                                       KISS_CMD_DATA, ui1, uilen1);
            CHECK(uilen1 > 0 && flen1 > 0);
            usleep(300000);
            {
                size_t rlen = 0;
                unsigned char *rec = read_record(recpath, &rlen);
                CHECK(rec != NULL && memfind(rec, rlen,
                                             (const char *)frame1,
                                             flen1) != NULL);
            }

            /* RX: every TX makes the fake answer with a received frame
             * - WITHOUT FCS, like the real firmware. The tmodem driver
             * must deliver it as a message; the tnc2 FCS check would
             * have dropped it. Fetch the answer of the first TX now,
             * before the next send queues a second one. */
            usleep(400000);
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

            /* A source labelled "CALL@device" names the device in
             * the message log - the frame keeps the plain base call. */
            {
                char label[64];
                snprintf(label, sizeof label, "%s@%s", callerid, tag);
                CHECK(rig.vtbl->send(&rig, label, "CQ", "moin3",
                                     err, sizeof err) == 0);

                unsigned char ui2[64];
                size_t uilen2 = ax25_ui_frame(ui2, sizeof ui2, callerid,
                                              "CQ",
                                              (const unsigned char *)"moin3",
                                              5);
                unsigned char frame2[128];
                size_t flen2 = kiss_encode(frame2, sizeof frame2, 0,
                                           KISS_CMD_DATA, ui2, uilen2);
                CHECK(uilen2 > 0 && flen2 > 0);
                usleep(300000);
                size_t rlen = 0;
                unsigned char *rec = read_record(recpath, &rlen);
                CHECK(rec != NULL && memfind(rec, rlen,
                                             (const char *)frame2,
                                             flen2) != NULL);

                pr_msg log[16];
                size_t nlog = 0;
                CHECK(pr_log_tail(&cfg, log, 16, &nlog,
                                  err, sizeof err) == 0);
                bool seen = false;
                for (size_t i = 0; i < nlog; i++)
                    if (log[i].kind == PR_MSG_TX &&
                        strcmp(log[i].from, label) == 0 &&
                        strcmp(log[i].text, "moin3") == 0)
                        seen = true;
                CHECK(seen);
            }

            /* PTT bookkeeping + monitor lock */
            CHECK(rig.vtbl->set_ptt(&rig, true, err, sizeof err) == 0);
            pr_rig_state st;
            CHECK(rig.vtbl->get_state(&rig, &st) == 0);
            CHECK(st.ptt == true);
            CHECK(rig.vtbl->set_ptt(&rig, false, err, sizeof err) == 0);
            CHECK(rig.vtbl->set_monitor(&rig, true, err, sizeof err) == 0);
            CHECK(rig.vtbl->send(&rig, callerid, "CQ", "blocked",
                                 err, sizeof err) != 0);
            CHECK(rig.vtbl->set_monitor(&rig, false, err, sizeof err) == 0);

            pr_rig_close(&rig);
        }
        pr_config_free(&cfg);
    }

    /* ---- shutdown ---------------------------------------------------- */
    pr_tncsock_close(&c);
    if (up)
        (void)kill(dmn_pid, SIGTERM);
    else
        (void)kill(dmn_pid, SIGKILL);
    (void)waitpid(dmn_pid, NULL, 0);
    usleep(400000);          /* the fake T-Modem still drains the port  */
    (void)kill(tnc_pid, SIGTERM);
    (void)waitpid(tnc_pid, NULL, 0);
    close(master);

    /* ---- what the daemon wrote to the device ------------------------- */
    size_t rlen = 0;
    unsigned char *rec = read_record(recpath, &rlen);
    CHECK(rec != NULL && rlen > 0);
    if (rec == NULL || rlen == 0)
        return;

    /* The startup must have stayed silent: the FIRST bytes in the
     * record can only be the driver's TX frames (C0 00 ... C0). No
     * probe, no MYCALL, no KISS entry anywhere in the record. */
    static const unsigned char esc_v[]   = { 0x1B, 'V' };
    static const unsigned char at_k[]    = { 0x1B, 0x40, 0x4B };
    static const unsigned char info[]    = { 'I', 'N', 'F', 'O' };
    static const unsigned char kiss_on[] = { 'k','i','s','s',' ','o','n' };
    static const unsigned char mycall[]  = { 'M','Y','C','A','L','L' };
    static const unsigned char txdelay[] = { 0xC0, 0x01, 0x32, 0xC0 };
    CHECK(memfind(rec, rlen, (const char *)esc_v, 2) == NULL);
    CHECK(memfind(rec, rlen, (const char *)at_k, 3) == NULL);
    CHECK(memfind(rec, rlen, (const char *)info, 4) == NULL);
    CHECK(memfind(rec, rlen, (const char *)kiss_on, 7) == NULL);
    CHECK(memfind(rec, rlen, (const char *)mycall, 6) == NULL);
    CHECK(memfind(rec, rlen, (const char *)txdelay, 4) == NULL);

    /* the very first byte written must be a KISS FEND */
    CHECK(rec[0] == KISS_FEND);

    /* Shutdown leaves KISS with the return frame - harmless for the
     * T-Modem (a non-DATA frame, ignored by the firmware). */
    static const unsigned char leave[] = { 0xC0, 0xFF, 0xC0 };
    CHECK(rlen >= 3 && memcmp(rec + rlen - 3, leave, 3) == 0);
}

int main(int argc, char **argv)
{
    const char *daemon = argc > 1 ? argv[1] : "./prterm-tncd";

    printf("== prterm-tncd + tmodem driver against a fake T-Modem ==\n");

    run_scenario(daemon, "tm", "TEST-5");

    TEST_SUMMARY("tmodem");
}
