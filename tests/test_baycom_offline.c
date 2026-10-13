/*
 * Offline HDLC/CRC/NRZI/PLL test for prterm-baycom (no UART, no root).
 *
 * Pulls tools/prterm-baycom.c in via #include — the tool is a single
 * translation unit — and stubs the platform pieces. Verifies:
 *   1. CRC-CCITT append/check against known values
 *   2. HDLC TX→RX roundtrip including bit stuffing and CRC
 *   3. Full SER12 wire-level roundtrip: NRZI encode via bcpr_ser12_tick,
 *      NRZI decode via the Soft-DCD PLL (ser12_rx) at bit boundaries
 *   4. TOT trip on max key duration (TX protection)
 *   5. PTT watchdog drops MCR RTS during the pause window
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <string.h>

/* Pull in the tool as a translation unit. __PRTERM_BAYCOM_TEST__ makes
 * the tool compile the offline stubs (no I/O ports) and skip its own
 * main() + runtime block. */
#define __PRTERM_BAYCOM_TEST__ 1

#include "../tools/prterm-baycom.c"

/* ── 1. CRC ────────────────────────────────────────────────── */

static int test_crc(void)
{
    uint8_t buf[16];
    memcpy(buf, "TEST", 4);
    bcpr_append_crc_ccitt(buf, 4);
    if (!bcpr_check_crc_ccitt(buf, 6)) {
        fprintf(stderr, "FAIL: CRC check\n");
        return 1;
    }
    buf[0] ^= 0xff;
    if (bcpr_check_crc_ccitt(buf, 6)) {
        fprintf(stderr, "FAIL: CRC should fail on corrupt\n");
        return 1;
    }
    return 0;
}

/* ── 2. HDLC roundtrip (frame level) ───────────────────────── */

static uint8_t g_rx[BCPR_MAXFLEN + 4];
static int g_rx_len;
static int g_got;

static void hdlc_rx_flag(bcpr_hdlc_t *h)
{
    if (h->rx_len < 4)
        return;
    if (!bcpr_check_crc_ccitt(h->rx_buffer, h->rx_len))
        return;
    /* Deliver WITHOUT FCS (payload compare); real code keeps it. */
    if (h->rx_len - 2 <= (int)sizeof(g_rx)) {
        memcpy(g_rx, h->rx_buffer, (size_t)(h->rx_len - 2));
        g_rx_len = h->rx_len - 2;
        g_got = 1;
    }
}

static int test_hdlc_roundtrip(void)
{
    bcpr_hdlc_t tx, rx;
    bcpr_channel_t ch;
    uint8_t kiss[32];
    int i, words = 0;

    ch.tx_delay = 2; ch.tx_tail = 1; ch.slottime = 1;
    ch.ppersist = 255; ch.fulldup = 1;
    bcpr_hdlc_init(&tx, 1200, &ch);
    bcpr_hdlc_init(&rx, 1200, &ch);

    kiss[0] = 0; /* KISS DATA cmd */
    memcpy(kiss + 1, "HELLO", 5);
    if (bcpr_hdlc_queue_kiss(&tx, kiss, 6) != 0) {
        fprintf(stderr, "FAIL: queue\n");
        return 1;
    }
    bcpr_hdlc_arbitrate(&tx);
    if (!bcpr_hdlc_ptt(&tx)) {
        fprintf(stderr, "FAIL: PTT not set\n");
        return 1;
    }

    g_got = 0;
    g_rx_len = 0;
    for (i = 0; i < 4096 && !g_got; i++) {
        unsigned w;
        bcpr_hdlc_transmitter(&tx);
        w = bcpr_hdlc_getbits(&tx);
        if (w) {
            bcpr_hdlc_putbits(&rx, w);
            words++;
            bcpr_hdlc_receiver(&rx);
        } else if (!bcpr_hdlc_ptt(&tx)) {
            bcpr_hdlc_transmitter(&tx);
            while (!g_got) {
                w = bcpr_hdlc_getbits(&tx);
                if (!w)
                    break;
                bcpr_hdlc_putbits(&rx, w);
                bcpr_hdlc_receiver(&rx);
            }
            break;
        }
    }

    if (!g_got) {
        fprintf(stderr, "FAIL: no RX frame (words=%d)\n", words);
        return 1;
    }
    if (g_rx_len < 5 || memcmp(g_rx, "HELLO", 5) != 0) {
        fprintf(stderr, "FAIL: payload mismatch len=%d\n", g_rx_len);
        return 1;
    }
    return 0;
}

/* ── 3. SER12 wire-level roundtrip (NRZI + Soft-DCD PLL) ───── */

/*
 * TX side: bcpr_ser12_tick() shifts NRZI-encoded bits and reports the
 * MCR DTR data bit each tick. RX side: feed the DTR bit level into a
 * second ser12 state's ser12_rx() at exact baud_us boundaries — the
 * PLL latches transitions and delivers 16-bit words to hdlc_receiver.
 */
static int test_ser12_wire_roundtrip(void)
{
    bcpr_ser12_t s_tx, s_rx;
    bcpr_hdlc_t h_tx, h_rx;
    bcpr_channel_t ch;
    uint8_t kiss[32];
    int mcr, do_thr;
    unsigned t_us = 0;
    unsigned baud_us = 1000000u / 1200u;
    int i;

    ch.tx_delay = 2; ch.tx_tail = 1; ch.slottime = 1;
    ch.ppersist = 255; ch.fulldup = 1;

    bcpr_ser12_init(&s_tx, 1200, 1);   /* hard DCD: ignore soft dcd */
    bcpr_ser12_init(&s_rx, 1200, 1);
    s_tx.ptt_wd = 0;                    /* no watchdog in wire test */
    bcpr_hdlc_init(&h_tx, 1200, &ch);
    bcpr_hdlc_init(&h_rx, 1200, &ch);
    h_rx.dcd = 0;

    kiss[0] = 0;
    memcpy(kiss + 1, "WIRE1", 5);
    if (bcpr_hdlc_queue_kiss(&h_tx, kiss, 6) != 0) {
        fprintf(stderr, "FAIL: wire queue\n");
        return 1;
    }

    g_got = 0;
    g_rx_len = 0;

    for (i = 0; i < 20000 && !g_got; i++) {
        t_us += baud_us;

        bcpr_ser12_tick(&s_tx, &h_tx, 0, &mcr, &do_thr, t_us);
        if (!do_thr) {
            fprintf(stderr, "FAIL: do_thr00 not set\n");
            return 1;
        }

        /* RX PLL samples the TX line level (DTR bit from MCR). */
        ser12_rx(&s_rx, &h_rx, (mcr & 0x01) ? 1u : 0u, t_us);
        bcpr_hdlc_receiver(&h_rx);

        if (!s_tx.ptt_hw && i > 10 && h_tx.tx_state == 1 && h_tx.numflags == 0)
            break;  /* frame fully sent incl. tail */
    }

    if (!g_got) {
        fprintf(stderr, "FAIL: wire roundtrip — no RX frame after %d ticks\n", i);
        return 1;
    }
    if (g_rx_len < 5 || memcmp(g_rx, "WIRE1", 5) != 0) {
        fprintf(stderr, "FAIL: wire payload mismatch len=%d\n", g_rx_len);
        return 1;
    }
    return 0;
}

/* ── 4. TOT: max key duration force-unkeys ─────────────────── */

static int test_tot_max_key(void)
{
    bcpr_device_t d;

    memset(&d, 0, sizeof(d));
    d.tot_enabled = 1;
    d.tot_max_key_ms = 100;     /* short for the test */
    d.tot_min_gap_ms = BCPR_DEFAULT_TOT_MIN_GAP_MS;
    d.tot_max_consecutive = BCPR_DEFAULT_TOT_MAX_CONSEC;
    d.tot_max_bursts = BCPR_DEFAULT_TOT_MAX_BURSTS;
    bcpr_ser12_init(&d.ser12, 1200, 1);
    d.ser12.ptt_wd = 0;

    /* Simulate a keyed transmitter past the max key time. */
    d.ser12.ptt_hw = 1;
    d.ptt_was = 1;
    d.tot_key_start_us = 1000u;
    tot_check_key_duration(&d, 1000u + 200u * 1000u);

    if (!d.tot_tripped) {
        fprintf(stderr, "FAIL: TOT did not trip on max key\n");
        return 1;
    }
    if (d.ser12.ptt_hw) {
        fprintf(stderr, "FAIL: TOT trip did not unkey PTT\n");
        return 1;
    }
    return 0;
}

/* ── 5. PTT watchdog drops RTS during pause ────────────────── */

static int test_ptt_watchdog(void)
{
    bcpr_ser12_t s;
    int mcr = 0x0f;

    bcpr_ser12_init(&s, 1200, 1);
    s.ptt_wd = 1;
    s.ptt_wd_key_us = 1000u * 1000u;    /* 1 s key */
    s.ptt_wd_pause_us = 100u * 1000u;   /* 100 ms pause */
    s.ptt_hw = 1;

    /* First tick arms the phase. */
    ser12_apply_ptt_wd(&s, &mcr, 1u);
    if (s.ptt_wd_pausing) {
        fprintf(stderr, "FAIL: WD pausing immediately\n");
        return 1;
    }
    /* Before key time: no pause. */
    ser12_apply_ptt_wd(&s, &mcr, 500u * 1000u);
    if (s.ptt_wd_pausing) {
        fprintf(stderr, "FAIL: WD pausing early\n");
        return 1;
    }
    /* After key time: pause, MCR forced idle (RTS clear). */
    ser12_apply_ptt_wd(&s, &mcr, 1100u * 1000u);
    if (!s.ptt_wd_pausing) {
        fprintf(stderr, "FAIL: WD did not pause after key time\n");
        return 1;
    }
    if (mcr != 0x0d) {
        fprintf(stderr, "FAIL: WD pause MCR is 0x%02x, want 0x0d\n", mcr);
        return 1;
    }
    /* After pause time: resume (logical PTT stays 1). */
    ser12_apply_ptt_wd(&s, &mcr, 1250u * 1000u);
    if (s.ptt_wd_pausing) {
        fprintf(stderr, "FAIL: WD did not resume after pause\n");
        return 1;
    }
    if (!s.ptt_hw) {
        fprintf(stderr, "FAIL: WD cleared logical PTT\n");
        return 1;
    }
    return 0;
}

/* ── main (shadows the tool's main) ────────────────────────── */

int main(void)
{
    int rc = 0;

    rc |= test_crc();
    rc |= test_hdlc_roundtrip();
    rc |= test_ser12_wire_roundtrip();
    rc |= test_tot_max_key();
    rc |= test_ptt_watchdog();

    if (rc == 0)
        puts("OK: baycom offline (CRC, HDLC, SER12 wire, TOT, PTT-WD)");
    return rc;
}