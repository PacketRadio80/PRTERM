/*
 * prterm-baycom — BayCom/Compa SER12 modem daemon for PRTERM
 *
 * Userspace port of the BayCom SER12 bit-bang algorithm (Thomas Sailer,
 * GPL-2.0).  Drives a TCM3105 AFSK modem via the serial port's UART
 * registers (RTS = PTT, DTR = TX data bit, THR 0x00 = charge pump,
 * CTS/DSR = RX data bit).
 *
 * SER12 bit-bang, Soft-DCD PLL and HDLC framing algorithms are derived
 * from the Linux kernel sources baycom_ser_fdx.c and hdlcdrv.c (Thomas
 * Sailer et al.), via the MAX25-Stack userspace port (max25-bcpr),
 * which is the verified ground truth for this hardware.
 *
 * Exposes a KISS PTY at <runtime_dir>/kiss-<tag> so prterm-tncd can
 * connect to it via the existing tnc2 KISS driver (kiss_init = native).
 *
 * TX protection (radio hardware safety):
 *   - PTT watchdog: drops RTS every 14.5 s for 500 ms (FlexNet SER12.doc)
 *   - TOT: force-unkey after 25 s single key, 3 consecutive or 8 total
 *     bursts; a TOT trip latches until daemon restart
 *   - CSMA p-persistence: listen before transmit, never keys a busy channel
 *   - PTT is keyed ONLY while HDLC data is being shifted out; an idle
 *     channel NEVER holds the carrier
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _BSD_SOURCE
#define _BSD_SOURCE
#endif
#ifndef __BSD_VISIBLE
#define __BSD_VISIBLE 1
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <time.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <pthread.h>
#include <pwd.h>
#include <grp.h>

/* ── Platform I/O port access ───────────────────────────────── */

#if defined(__PRTERM_BAYCOM_TEST__)
/* Offline test: no real I/O ports. openpty() stays declared (never
 * called) so the tool translation unit still compiles. */
#if defined(__FreeBSD__)
#include <libutil.h>
#elif defined(__linux__)
#include <pty.h>
#endif
static int bcpr_ioperm(unsigned long port, unsigned long len, int turn_on)
{
    (void)port; (void)len; (void)turn_on;
    return 0;
}
#define INB(port)        ((void)(port), (unsigned char)0)
#define OUTB(val, port)  ((void)(val), (void)(port))
#elif defined(__FreeBSD__)
#include <machine/cpufunc.h>
#include <machine/sysarch.h>
#include <libutil.h>
#include <sys/ttycom.h>
static int g_io_fd = -1;  /* /dev/io fd — must stay open for inb/outb */
static int bcpr_ioperm(unsigned long port, unsigned long len, int turn_on)
{
    (void)port;
    (void)len;
    if (turn_on) {
        if (g_io_fd < 0) {
            g_io_fd = open("/dev/io", O_RDWR | O_CLOEXEC);
            if (g_io_fd < 0)
                return -1;
        }
        return 0;
    }
    if (g_io_fd >= 0) {
        close(g_io_fd);
        g_io_fd = -1;
    }
    return 0;
}
#define INB(port)        inb(port)
#define OUTB(val, port)  outb(port, val)
#elif defined(__linux__)
#include <sys/io.h>
#include <pty.h>
#include <sched.h>
#include <sys/mman.h>
static int bcpr_ioperm(unsigned long port, unsigned long len, int turn_on)
{
    return ioperm(port, len, turn_on);
}
#define INB(port)        inb(port)
#define OUTB(val, port)  outb(port, val)
#else
#error "BayCom: unsupported platform (need FreeBSD or Linux)"
#endif

#ifndef TIOCMGET
#define TIOCMGET 0x5415
#endif
#ifndef TIOCMBIS
#define TIOCMBIS 0x5416
#endif

/* ── UART 8250/16550 registers ──────────────────────────────── */

#define UART_THR  0   /* Transmit Holding Register (W) */
#define UART_IER  1   /* Interrupt Enable Register */
#define UART_FCR  2   /* FIFO Control Register (W) */
#define UART_LCR  3   /* Line Control Register */
#define UART_MCR  4   /* Modem Control Register */
#define UART_LSR  5   /* Line Status Register */
#define UART_MSR  6   /* Modem Status Register */
#define UART_DLL  0   /* Divisor Latch Low (when DLAB=1) */
#define UART_DLM  1   /* Divisor Latch High (when DLAB=1) */

#define MCR_DTR   0x01
#define MCR_RTS   0x02
#define MCR_OUT1  0x04
#define MCR_OUT2  0x08

#define MSR_CTS   0x10
#define MSR_DSR   0x20
#define MSR_RI    0x40
#define MSR_DCD   0x80

/* ── KISS constants ─────────────────────────────────────────── */

#define KISS_FEND  0xC0
#define KISS_FESC  0xDB
#define KISS_TFEND 0xDC
#define KISS_TFESC 0xDD

/* ── Sizing (max25-bcpr/bcpr.h) ─────────────────────────────── */

#define BCPR_HDLC_BUF 32
#define BCPR_MAXFLEN  400

/* ── Defaults (max25-bcpr/bcpr_config.c — FlexNet SER12.doc) ── */

#define BCPR_DEFAULT_BAUD            1200
#define BCPR_DEFAULT_TX_DELAY        35
#define BCPR_DEFAULT_TX_TAIL         2
#define BCPR_DEFAULT_SLOTTIME        10
#define BCPR_DEFAULT_PERSIST         40
#define BCPR_DEFAULT_PTT_WD_KEY_MS   14500
#define BCPR_DEFAULT_PTT_WD_PAUSE_MS 500
#define BCPR_DEFAULT_TOT_MAX_KEY_MS  25000
#define BCPR_DEFAULT_TOT_MIN_GAP_MS  1500
#define BCPR_DEFAULT_TOT_MAX_CONSEC  3
#define BCPR_DEFAULT_TOT_MAX_BURSTS  8

/* Idle longer than this resets consecutive-burst counting (new session). */
#define TOT_SESSION_RESET_US 120000000u

/* ── Calibration modes (bcpr_ser12.h) ───────────────────────── */

enum {
    BCPR_CAL_OFF = 0,
    BCPR_CAL_HIGH = 1,
    BCPR_CAL_LOW = 2,
    BCPR_CAL_ALT = 3
};

/* ── CRC-CCITT (bcpr_crc.c, reflected poly 0x8408) ──────────── */

static const uint16_t bcpr_crc_table[256] = {
    0x0000, 0x1189, 0x2312, 0x329b, 0x4624, 0x57ad, 0x6536, 0x74bf, 0x8c48,
    0x9dc1, 0xaf5a, 0xbed3, 0xca6c, 0xdbe5, 0xe97e, 0xf8f7, 0x1081, 0x0108,
    0x3393, 0x221a, 0x56a5, 0x472c, 0x75b7, 0x643e, 0x9cc9, 0x8d40, 0xbfdb,
    0xae52, 0xdaed, 0xcb64, 0xf9ff, 0xe876, 0x2102, 0x308b, 0x0210, 0x1399,
    0x6726, 0x76af, 0x4434, 0x55bd, 0xad4a, 0xbcc3, 0x8e58, 0x9fd1, 0xeb6e,
    0xfae7, 0xc87c, 0xd9f5, 0x3183, 0x200a, 0x1291, 0x0318, 0x77a7, 0x662e,
    0x54b5, 0x453c, 0xbdcb, 0xac42, 0x9ed9, 0x8f50, 0xfbef, 0xea66, 0xd8fd,
    0xc974, 0x4204, 0x538d, 0x6116, 0x709f, 0x0420, 0x15a9, 0x2732, 0x36bb,
    0xce4c, 0xdfc5, 0xed5e, 0xfcd7, 0x8868, 0x99e1, 0xab7a, 0xbaf3, 0x5285,
    0x430c, 0x7197, 0x601e, 0x14a1, 0x0528, 0x37b3, 0x263a, 0xdecd, 0xcf44,
    0xfddf, 0xec56, 0x98e9, 0x8960, 0xbbfb, 0xaa72, 0x6306, 0x728f, 0x4014,
    0x519d, 0x2522, 0x34ab, 0x0630, 0x17b9, 0xef4e, 0xfec7, 0xcc5c, 0xddd5,
    0xa96a, 0xb8e3, 0x8a78, 0x9bf1, 0x7387, 0x620e, 0x5095, 0x411c, 0x35a3,
    0x242a, 0x16b1, 0x0738, 0xffcf, 0xee46, 0xdcdd, 0xcd54, 0xb9eb, 0xa862,
    0x9af9, 0x8b70, 0x8408, 0x9581, 0xa71a, 0xb693, 0xc22c, 0xd3a5, 0xe13e,
    0xf0b7, 0x0840, 0x19c9, 0x2b52, 0x3adb, 0x4e64, 0x5fed, 0x6d76, 0x7cff,
    0x9489, 0x8500, 0xb79b, 0xa612, 0xd2ad, 0xc324, 0xf1bf, 0xe036, 0x18c1,
    0x0948, 0x3bd3, 0x2a5a, 0x5ee5, 0x4f6c, 0x7df7, 0x6c7e, 0xa50a, 0xb483,
    0x8618, 0x9791, 0xe32e, 0xf2a7, 0xc03c, 0xd1b5, 0x2942, 0x38cb, 0x0a50,
    0x1bd9, 0x6f66, 0x7eef, 0x4c74, 0x5dfd, 0xb58b, 0xa402, 0x9699, 0x8710,
    0xf3af, 0xe226, 0xd0bd, 0xc134, 0x39c3, 0x284a, 0x1ad1, 0x0b58, 0x7fe7,
    0x6e6e, 0x5cf5, 0x4d7c, 0xc60c, 0xd785, 0xe51e, 0xf497, 0x8028, 0x91a1,
    0xa33a, 0xb2b3, 0x4a44, 0x5bcd, 0x6956, 0x78df, 0x0c60, 0x1de9, 0x2f72,
    0x3efb, 0xd68d, 0xc704, 0xf59f, 0xe416, 0x90a9, 0x8120, 0xb3bb, 0xa232,
    0x5ac5, 0x4b4c, 0x79d7, 0x685e, 0x1ce1, 0x0d68, 0x3ff3, 0x2e7a, 0xe70e,
    0xf687, 0xc41c, 0xd595, 0xa12a, 0xb0a3, 0x8238, 0x93b1, 0x6b46, 0x7acf,
    0x4854, 0x59dd, 0x2d62, 0x3ceb, 0x0e70, 0x1ff9, 0xf78f, 0xe606, 0xd49d,
    0xc514, 0xb1ab, 0xa022, 0x92b9, 0x8330, 0x7bc7, 0x6a4e, 0x58d5, 0x495c,
    0x3de3, 0x2c6a, 0x1ef1, 0x0f78
};

static uint16_t bcpr_crc_ccitt(uint16_t crc, const uint8_t *buf, size_t len)
{
    while (len--)
        crc = (uint16_t)((crc >> 8) ^ bcpr_crc_table[(crc ^ *buf++) & 0xff]);
    return crc;
}

static void bcpr_append_crc_ccitt(uint8_t *buffer, int len)
{
    unsigned crc = (unsigned)bcpr_crc_ccitt(0xffff, buffer, (size_t)len) ^ 0xffffu;
    buffer += len;
    *buffer++ = (uint8_t)(crc & 0xff);
    *buffer++ = (uint8_t)(crc >> 8);
}

static int bcpr_check_crc_ccitt(const uint8_t *buf, int cnt)
{
    return (bcpr_crc_ccitt(0xffff, buf, (size_t)cnt) & 0xffff) == 0xf0b8;
}

/* ── HDLC (bcpr_hdlc.c 1:1 — hdlcdrv derived) ───────────────── */

typedef struct bcpr_hbuf {
    unsigned rd, wr;
    uint16_t buf[BCPR_HDLC_BUF];
} bcpr_hbuf_t;

typedef struct bcpr_channel {
    int tx_delay, tx_tail, slottime, ppersist, fulldup;
} bcpr_channel_t;

typedef struct bcpr_hdlc {
    int bitrate;
    bcpr_channel_t ch;
    bcpr_hbuf_t rx_hbuf;
    bcpr_hbuf_t tx_hbuf;
    int rx_state;
    unsigned bitstream, bitbuf;
    int numbits;
    int dcd;
    int rx_len;
    uint8_t *rx_bp;
    uint8_t rx_buffer[BCPR_MAXFLEN + 2];
    int tx_state;
    int numflags;
    unsigned tx_bitstream;
    int ptt;
    int slotcnt;
    unsigned tx_bitbuf;
    int tx_numbits;
    int tx_len;
    uint8_t *tx_bp;
    uint8_t tx_buffer[BCPR_MAXFLEN + 2];
    /* pending KISS payload (without FEND/cmd) queued for TX */
    uint8_t pending[BCPR_MAXFLEN];
    int pending_len;
    int have_pending;
} bcpr_hdlc_t;

static int hbuf_empty(const bcpr_hbuf_t *hb)
{
    return hb->rd == hb->wr;
}

static int hbuf_full(const bcpr_hbuf_t *hb)
{
    return !((BCPR_HDLC_BUF - 1 + hb->rd - hb->wr) % BCPR_HDLC_BUF);
}

static void hbuf_put(bcpr_hbuf_t *hb, uint16_t val)
{
    unsigned newp = (hb->wr + 1) % BCPR_HDLC_BUF;
    if (newp != hb->rd) {
        hb->buf[hb->wr] = val;
        hb->wr = newp;
    }
}

static uint16_t hbuf_get(bcpr_hbuf_t *hb)
{
    uint16_t val;
    if (hb->rd == hb->wr)
        return 0;
    val = hb->buf[hb->rd];
    hb->rd = (hb->rd + 1) % BCPR_HDLC_BUF;
    return val;
}

#define tenms_to_2flags(h, tenms) (((tenms) * (h)->bitrate) / 100 / 16)

static void bcpr_hdlc_init(bcpr_hdlc_t *h, int bitrate, const bcpr_channel_t *ch)
{
    memset(h, 0, sizeof(*h));
    h->bitrate = bitrate > 0 ? bitrate : 1200;
    if (ch) {
        h->ch = *ch;
    } else {
        h->ch.tx_delay = BCPR_DEFAULT_TX_DELAY;
        h->ch.tx_tail = BCPR_DEFAULT_TX_TAIL;
        h->ch.slottime = BCPR_DEFAULT_SLOTTIME;
        h->ch.ppersist = BCPR_DEFAULT_PERSIST;
        h->ch.fulldup = 0;
    }
    h->slotcnt = 1;
    h->rx_bp = h->rx_buffer;
    h->tx_bp = h->tx_buffer;
}

static void bcpr_hdlc_putbits(bcpr_hdlc_t *h, unsigned bits)
{
    hbuf_put(&h->rx_hbuf, (uint16_t)(bits & 0xffff));
}

static unsigned bcpr_hdlc_getbits(bcpr_hdlc_t *h)
{
    if (hbuf_empty(&h->tx_hbuf)) {
        h->ptt = 0;
        return 0;
    }
    return hbuf_get(&h->tx_hbuf);
}

static int bcpr_hdlc_ptt(const bcpr_hdlc_t *h)
{
    return h->ptt;
}

static void bcpr_hdlc_abort_tx(bcpr_hdlc_t *h)
{
    if (!h)
        return;
    h->ptt = 0;
    h->tx_state = 0;
    h->numflags = 0;
    h->tx_bitstream = 0;
    h->tx_bitbuf = 0;
    h->tx_numbits = 0;
    h->tx_len = 0;
    h->tx_bp = h->tx_buffer;
    h->have_pending = 0;
    h->pending_len = 0;
    h->tx_hbuf.rd = h->tx_hbuf.wr;
}

static int bcpr_hdlc_queue_kiss(bcpr_hdlc_t *h, const uint8_t *kiss, int len)
{
    if (!kiss || len < 2 || len - 1 > BCPR_MAXFLEN)
        return -1;
    if (h->have_pending || h->ptt)
        return -1;
    /* strip KISS command byte */
    memcpy(h->pending, kiss + 1, (size_t)(len - 1));
    h->pending_len = len - 1;
    h->have_pending = 1;
    return 0;
}

static int hdlc_rx_add_bytes(bcpr_hdlc_t *h, unsigned bits, int num)
{
    int added = 0;
    while (h->rx_state && num >= 8) {
        if (h->rx_len >= (int)sizeof(h->rx_buffer)) {
            h->rx_state = 0;
            return 0;
        }
        *h->rx_bp++ = (uint8_t)(bits >> (32 - num));
        h->rx_len++;
        num -= 8;
        added += 8;
    }
    return added;
}

/* Forward decl — RX frame delivery is device glue, defined below. */
static void hdlc_rx_flag(bcpr_hdlc_t *h);

static void bcpr_hdlc_receiver(bcpr_hdlc_t *h)
{
    int i;
    unsigned mask1, mask2, mask3, mask4, mask5, mask6, word;

    while (!hbuf_empty(&h->rx_hbuf)) {
        word = hbuf_get(&h->rx_hbuf);
        h->bitstream >>= 16;
        h->bitstream |= word << 16;
        h->bitbuf >>= 16;
        h->bitbuf |= word << 16;
        h->numbits += 16;
        for (i = 15, mask1 = 0x1fc00, mask2 = 0x1fe00, mask3 = 0x0fc00,
            mask4 = 0x1f800, mask5 = 0xf800, mask6 = 0xffff;
             i >= 0; i--, mask1 <<= 1, mask2 <<= 1, mask3 <<= 1, mask4 <<= 1,
                       mask5 <<= 1, mask6 = (mask6 << 1) | 1) {
            if ((h->bitstream & mask1) == mask1) {
                h->rx_state = 0;
            } else if ((h->bitstream & mask2) == mask3) {
                if (h->rx_state) {
                    hdlc_rx_add_bytes(h, h->bitbuf << (8 + i),
                                      h->numbits - 8 - i);
                    hdlc_rx_flag(h);
                }
                h->rx_len = 0;
                h->rx_bp = h->rx_buffer;
                h->rx_state = 1;
                h->numbits = i;
            } else if ((h->bitstream & mask4) == mask5) {
                h->numbits--;
                h->bitbuf = (h->bitbuf & (~mask6)) |
                            ((h->bitbuf & mask6) << 1);
            }
        }
        h->numbits -= hdlc_rx_add_bytes(h, h->bitbuf, h->numbits);
    }
}

static void bcpr_hdlc_transmitter(bcpr_hdlc_t *h)
{
    unsigned mask1, mask2, mask3;
    int i;

    for (;;) {
        if (h->tx_numbits >= 16) {
            if (hbuf_full(&h->tx_hbuf))
                return;
            hbuf_put(&h->tx_hbuf, (uint16_t)(h->tx_bitbuf & 0xffff));
            h->tx_bitbuf >>= 16;
            h->tx_numbits -= 16;
        }
        switch (h->tx_state) {
        default:
            return;
        case 0:
        case 1:
            if (h->numflags) {
                h->numflags--;
                h->tx_bitbuf |= 0x7e7e << h->tx_numbits;
                h->tx_numbits += 16;
                break;
            }
            if (h->tx_state == 1)
                return;
            if (!h->have_pending) {
                int flgs = tenms_to_2flags(h, h->ch.tx_tail);
                if (flgs < 2)
                    flgs = 2;
                h->tx_state = 1;
                h->numflags = flgs;
                break;
            }
            if (h->pending_len > BCPR_MAXFLEN || h->pending_len < 2) {
                h->have_pending = 0;
                h->tx_state = 0;
                h->numflags = 1;
                break;
            }
            memcpy(h->tx_buffer, h->pending, (size_t)h->pending_len);
            h->have_pending = 0;
            h->tx_bp = h->tx_buffer;
            bcpr_append_crc_ccitt(h->tx_buffer, h->pending_len);
            h->tx_len = h->pending_len + 2;
            h->tx_state = 2;
            h->tx_bitstream = 0;
            break;
        case 2:
            if (!h->tx_len) {
                h->tx_state = 0;
                h->numflags = 1;
                break;
            }
            h->tx_len--;
            h->tx_bitbuf |= (unsigned)(*h->tx_bp) << h->tx_numbits;
            h->tx_bitstream >>= 8;
            h->tx_bitstream |= (unsigned)(*h->tx_bp++) << 16;
            mask1 = 0x1f000;
            mask2 = 0x10000;
            mask3 = 0xffffffffu >> (31 - h->tx_numbits);
            h->tx_numbits += 8;
            for (i = 0; i < 8;
                 i++, mask1 <<= 1, mask2 <<= 1, mask3 = (mask3 << 1) | 1) {
                if ((h->tx_bitstream & mask1) != mask1)
                    continue;
                h->tx_bitstream &= ~mask2;
                h->tx_bitbuf = (h->tx_bitbuf & mask3) |
                               ((h->tx_bitbuf & (~mask3)) << 1);
                h->tx_numbits++;
                mask3 = (mask3 << 1) | 1;
            }
            break;
        }
    }
}

static void bcpr_hdlc_arbitrate(bcpr_hdlc_t *h)
{
    if (h->ptt || !h->have_pending)
        return;
    if (h->ch.fulldup) {
        h->tx_state = 0;
        h->numflags = tenms_to_2flags(h, h->ch.tx_delay);
        h->tx_bitbuf = h->tx_bitstream = 0;
        h->tx_numbits = 0;
        bcpr_hdlc_transmitter(h);
        h->ptt = 1;
        return;
    }
    if (!h->dcd && (--h->slotcnt <= 0)) {
        h->slotcnt = h->ch.slottime;
        if ((rand() % 256) > h->ch.ppersist)
            return;
        h->tx_state = 0;
        h->numflags = tenms_to_2flags(h, h->ch.tx_delay);
        h->tx_bitbuf = h->tx_bitstream = 0;
        h->tx_numbits = 0;
        bcpr_hdlc_transmitter(h);
        h->ptt = 1;
    }
}

/* ── SER12 (bcpr_ser12.c 1:1 — baycom_ser_fdx derived) ──────── */

typedef struct bcpr_ser12 {
    unsigned baud;
    unsigned baud_us;
    int opt_dcd; /* 0 soft, 1 hard, -1 hard inv */
    unsigned char tx_bit;
    unsigned char last_rxbit;
    int dcd_sum0, dcd_sum1, dcd_sum2;
    int dcd_time;
    unsigned pll_time;
    unsigned txshreg;
    unsigned shreg;
    int ptt_hw; /* modem PTT keyed (logical; stays 1 during WD pause) */
    int cal_mode;
    /* FlexNet SER12 PTT WD */
    int ptt_wd;
    unsigned ptt_wd_key_us;
    unsigned ptt_wd_pause_us;
    unsigned ptt_wd_phase_start_us;
    int ptt_wd_pausing;
} bcpr_ser12_t;

static void bcpr_ser12_init(bcpr_ser12_t *s, unsigned baud, int opt_dcd)
{
    memset(s, 0, sizeof(*s));
    s->baud = baud ? baud : 1200;
    s->baud_us = 1000000u / s->baud;
    s->opt_dcd = opt_dcd;
    s->shreg = 0x10000;
    s->dcd_sum0 = 2;
    s->dcd_time = 120;
    s->cal_mode = BCPR_CAL_OFF;
    s->ptt_wd = 1;
    s->ptt_wd_key_us = BCPR_DEFAULT_PTT_WD_KEY_MS * 1000u;
    s->ptt_wd_pause_us = BCPR_DEFAULT_PTT_WD_PAUSE_MS * 1000u;
}

static void bcpr_ser12_force_unkey(bcpr_ser12_t *s)
{
    if (!s)
        return;
    s->ptt_hw = 0;
    s->ptt_wd_pausing = 0;
    s->ptt_wd_phase_start_us = 0;
    s->txshreg = 0;
}

static void bcpr_ser12_set_cal(bcpr_ser12_t *s, int cal_mode)
{
    if (!s)
        return;
    if (cal_mode < BCPR_CAL_OFF || cal_mode > BCPR_CAL_ALT)
        cal_mode = BCPR_CAL_OFF;
    s->cal_mode = cal_mode;
    if (cal_mode != BCPR_CAL_OFF) {
        s->ptt_hw = 1;
        if (cal_mode == BCPR_CAL_HIGH)
            s->tx_bit = 1;
        else
            s->tx_bit = 0;
        s->txshreg = 1;
        s->ptt_wd_pausing = 0;
        s->ptt_wd_phase_start_us = 0;
    }
}

/*
 * FlexNet SER12.doc / PAR96: during long calibrate, drop PTT every ~14.5 s
 * for ~500 ms so the radio PTT watchdog can discharge. Logical ptt_hw stays
 * 1 (keep baud divisor / telemetry); only MCR RTS is cleared for the pause.
 */
static void ser12_apply_ptt_wd(bcpr_ser12_t *s, int *mcr_out, unsigned now_us)
{
    unsigned elapsed;

    if (!s->ptt_wd || !s->ptt_hw) {
        s->ptt_wd_pausing = 0;
        s->ptt_wd_phase_start_us = 0;
        return;
    }
    if (s->ptt_wd_phase_start_us == 0) {
        s->ptt_wd_phase_start_us = now_us ? now_us : 1u;
        s->ptt_wd_pausing = 0;
        return;
    }
    elapsed = now_us - s->ptt_wd_phase_start_us;
    if (!s->ptt_wd_pausing) {
        if (elapsed >= s->ptt_wd_key_us) {
            s->ptt_wd_pausing = 1;
            s->ptt_wd_phase_start_us = now_us ? now_us : 1u;
            *mcr_out = 0x0d; /* RTS clear — PTT off */
        }
        return;
    }
    /* In pause: force idle MCR regardless of cal/HDLC bit. */
    *mcr_out = 0x0d;
    if (elapsed >= s->ptt_wd_pause_us) {
        s->ptt_wd_pausing = 0;
        s->ptt_wd_phase_start_us = now_us ? now_us : 1u;
    }
}

static void ser12_rx(bcpr_ser12_t *s, bcpr_hdlc_t *h, unsigned curs,
                     unsigned now_us)
{
    int timediff;
    int bdus8 = (int)(s->baud_us >> 3);
    int bdus4 = (int)(s->baud_us >> 2);
    int bdus2 = (int)(s->baud_us >> 1);

    timediff = (int)(now_us - s->pll_time);
    while (timediff >= 500000)
        timediff -= 1000000;
    while (timediff <= -500000)
        timediff += 1000000;
    while (timediff >= bdus2) {
        timediff -= (int)s->baud_us;
        s->pll_time += s->baud_us;
        s->dcd_time--;
        if (s->shreg & 1) {
            bcpr_hdlc_putbits(h, (s->shreg >> 1) ^ 0xffffu);
            s->shreg = 0x10000;
        }
        s->shreg >>= 1;
    }
    if (s->dcd_time <= 0) {
        if (!s->opt_dcd)
            h->dcd = (s->dcd_sum0 + s->dcd_sum1 + s->dcd_sum2) < 0;
        s->dcd_sum2 = s->dcd_sum1;
        s->dcd_sum1 = s->dcd_sum0;
        s->dcd_sum0 = 2;
        s->dcd_time += 120;
    }
    if (s->last_rxbit != (unsigned char)curs) {
        s->last_rxbit = (unsigned char)curs;
        s->shreg |= 0x10000;
        if (timediff > 0)
            s->pll_time += (unsigned)bdus8;
        else
            s->pll_time += 1000000u - (unsigned)bdus8;
        if (abs(timediff) > bdus4)
            s->dcd_sum0 += 4;
        else
            s->dcd_sum0--;
    }
    while (s->pll_time >= 1000000u)
        s->pll_time -= 1000000u;
}

static void bcpr_ser12_tick(bcpr_ser12_t *s, bcpr_hdlc_t *h, int cts,
                            int *mcr_out, int *do_thr00, unsigned now_us)
{
    *do_thr00 = 1;

    /* DOS cal.exe: continuous PTT + sticky/toggle DTR; no HDLC. */
    if (s->cal_mode != BCPR_CAL_OFF) {
        s->ptt_hw = 1;
        if (s->cal_mode == BCPR_CAL_HIGH)
            s->tx_bit = 1;
        else if (s->cal_mode == BCPR_CAL_LOW)
            s->tx_bit = 0;
        else
            s->tx_bit = (unsigned char)!s->tx_bit;
        *mcr_out = 0x0e | (!!s->tx_bit);
        ser12_apply_ptt_wd(s, mcr_out, now_us);
        return;
    }

    /*
     * While PTT: skip Soft-DCD RX PLL — PC-COM charge-pump needs unbroken
     * THR 0x00 @ baud; RX work causes multi-ms TXD gaps → underpowered AFSK.
     */
    if (!s->ptt_hw)
        ser12_rx(s, h, cts ? 1u : 0u, now_us);

    if (s->ptt_hw)
        *mcr_out = 0x0e | (!!s->tx_bit);
    else
        *mcr_out = 0x0d;

    if (s->ptt_hw) {
        if (s->txshreg <= 1) {
            s->txshreg = 0x10000u | bcpr_hdlc_getbits(h);
            if (!bcpr_hdlc_ptt(h)) {
                s->ptt_hw = 0;
                *mcr_out = 0x0d;
                ser12_apply_ptt_wd(s, mcr_out, now_us);
                return;
            }
        }
        /* NRZI: toggle on 0, hold on 1 */
        s->tx_bit = (unsigned char)(!(s->tx_bit ^ (s->txshreg & 1)));
        s->txshreg >>= 1;
    } else {
        bcpr_hdlc_arbitrate(h);
        if (bcpr_hdlc_ptt(h)) {
            s->txshreg = 1;
            s->ptt_hw = 1;
        }
    }
    bcpr_hdlc_transmitter(h);
    ser12_apply_ptt_wd(s, mcr_out, now_us);
}

/* ── UART helpers ───────────────────────────────────────────── */

static unsigned long g_iobase;

static inline void uart_outb(unsigned char val, unsigned port)
{
    OUTB(val, port);
}

static inline unsigned char uart_inb(unsigned port)
{
    return INB(port);
}

static void bcpr_uart_set_divisor(unsigned iobase, unsigned divisor)
{
    unsigned char lcr = uart_inb(iobase + UART_LCR);
    uart_outb((unsigned char)(lcr | 0x80), iobase + UART_LCR); /* DLAB */
    uart_outb((unsigned char)(divisor & 0xff), iobase + UART_DLL);
    uart_outb((unsigned char)((divisor >> 8) & 0xff), iobase + UART_DLM);
    uart_outb((unsigned char)(lcr & 0x7f), iobase + UART_LCR);
}

static void bcpr_uart_open_ser12(unsigned iobase)
{
    /* Match baycom_ser_fdx open: FIFO off, 6-bit word, IER THRE+MSR. */
    uart_outb(0x00, iobase + UART_FCR);
    uart_outb(0x01, iobase + UART_LCR);  /* 6N1 */
    uart_outb(0x0a, iobase + UART_IER);
    uart_outb(0x0d, iobase + UART_MCR);  /* idle */
    uart_outb(0x00, iobase + UART_THR);  /* prime charge pump */
}

static void bcpr_uart_close_ser12(unsigned iobase)
{
    uart_outb(0x00, iobase + UART_IER);
    uart_outb(0x01, iobase + UART_MCR);
}

/* ── Device ─────────────────────────────────────────────────── */

typedef struct bcpr_device {
    char          tag[32];
    char          serial_path[256];
    int           serial_fd;
    unsigned long iobase;
    bcpr_ser12_t  ser12;
    bcpr_hdlc_t   hdlc;
    /* TOT state (bcpr_engine.c) */
    int           tot_enabled;
    unsigned      tot_max_key_ms;
    unsigned      tot_min_gap_ms;
    unsigned      tot_max_consecutive;
    unsigned      tot_max_bursts;
    int           tot_tripped;
    unsigned      tot_key_start_us;
    unsigned      tot_last_off_us;
    unsigned      tot_consecutive;
    unsigned      tot_burst_total;
    /* Engine state */
    int           ptt_was;
    int           tx_div_set;
    int64_t       ptt_on_ns;
    unsigned      thr_writes;
    unsigned      max_tick_gap_us;
    unsigned      gap_sum_us;
    unsigned      tick_count;
    unsigned      gaps_gt_2x;
    unsigned      last_tick_us;
    /* RX counters (diagnostics) */
    unsigned long rx_frames;
    unsigned long rx_crc_err;
    unsigned long tx_frames;
    /* KISS PTY */
    int           kiss_master;
    char          kiss_path[256];
} bcpr_device_t;

/* ── Globals ────────────────────────────────────────────────── */

static volatile sig_atomic_t g_stop;
static char g_state_dir[256] = "/var/prterm/prterm.runtime";
static FILE *g_diag;
static bcpr_device_t g_dev;

#if defined(__FreeBSD__)
/* RX pin arrives on a modem line read via ioctl (uart(4) owns the MSR). */
static int g_rx_pin_mask = TIOCM_CTS;  /* SER12 default: CTS */
#else
/* Linux: MSR bit carrying the RX signal (auto-detected, default CTS). */
static int g_linux_rx_msr_bit = MSR_CTS;
#endif

static unsigned now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned)((unsigned long long)ts.tv_sec * 1000000ull +
                      (unsigned long long)ts.tv_nsec / 1000ull);
}

static int64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

static void diag(const char *fmt, ...)
{
    va_list ap;
    if (!g_diag)
        return;
    va_start(ap, fmt);
    vfprintf(g_diag, fmt, ap);
    va_end(ap);
    fflush(g_diag);
}

/* ── MSR read (platform) ────────────────────────────────────── */

/*
 * Linux: direct MSR register read via inb (ioperm granted).
 * FreeBSD: uart(4) owns the chip; the modem lines are read through
 * TIOCMGET instead. The detected RX pin mask maps to the CTS bit so
 * the SER12 code stays identical on both platforms.
 */
static unsigned char dev_read_msr(bcpr_device_t *d)
{
#if defined(__FreeBSD__)
    int bits = 0;
    unsigned char msr = 0;
    if (d->serial_fd < 0)
        return 0;
    if (ioctl(d->serial_fd, TIOCMGET, &bits) < 0)
        return 0;
    if (bits & g_rx_pin_mask)
        msr |= MSR_CTS;
    if (bits & TIOCM_CD)
        msr |= MSR_DCD;
    return msr;
#else
    unsigned char raw = uart_inb(g_iobase + UART_MSR);
    unsigned char msr = 0;
    (void)d;
    /* Map the detected RX pin onto the CTS position. */
    if (raw & g_linux_rx_msr_bit)
        msr |= MSR_CTS;
    if (raw & MSR_DCD)
        msr |= MSR_DCD;
    return msr;
#endif
}

/* ── RX frame delivery → KISS PTY ───────────────────────────── */

static void kiss_write_frame(int fd, const uint8_t *data, int len)
{
    uint8_t out[BCPR_MAXFLEN * 2 + 4];
    int o = 0;

    if (fd < 0 || !data || len <= 0)
        return;
    out[o++] = KISS_FEND;
    for (int i = 0; i < len && o < (int)sizeof(out) - 2; i++) {
        if (data[i] == KISS_FEND) {
            out[o++] = KISS_FESC;
            out[o++] = KISS_TFEND;
        } else if (data[i] == KISS_FESC) {
            out[o++] = KISS_FESC;
            out[o++] = KISS_TFESC;
        } else {
            out[o++] = data[i];
        }
    }
    out[o++] = KISS_FEND;
    (void)write(fd, out, (size_t)o);
}

/*
 * One valid HDLC frame arrived. Deliver it as KISS DATA:
 *   [0]=cmd 0x00, [1..]=AX.25 frame INCLUDING the FCS trailer.
 * PRTERM's tnc2 driver validates and strips the FCS on RX (same
 * convention as TheFirmware TNCs) — see docs/TNC-INIT.md.
 *
 * The offline test supplies its own body (guarded) to capture frames.
 */
#ifndef __PRTERM_BAYCOM_TEST__
static void hdlc_rx_flag(bcpr_hdlc_t *h)
{
    uint8_t kiss[BCPR_MAXFLEN + 3];
    int pkt_len;

    if (h->rx_len < 4)
        return;
    if (!bcpr_check_crc_ccitt(h->rx_buffer, h->rx_len)) {
        g_dev.rx_crc_err++;
        return;
    }
    /* keep the 2 CRC bytes: PRTERM strips+validates FCS itself */
    pkt_len = h->rx_len + 1;
    kiss[0] = 0;
    memcpy(kiss + 1, h->rx_buffer, (size_t)(pkt_len - 1));
    g_dev.rx_frames++;
    kiss_write_frame(g_dev.kiss_master, kiss, pkt_len);
}
#endif

/* ── TOT (bcpr_engine.c 1:1) ────────────────────────────────── */

static void tot_write_trip_file(bcpr_device_t *d, const char *reason)
{
    char path[320];
    FILE *f;

    if (g_state_dir[0] == '\0')
        return;
    snprintf(path, sizeof(path), "%s/tot-trip-%s", g_state_dir, d->tag);
    f = fopen(path, "w");
    if (!f)
        return;
    fprintf(f,
            "reason=%s\nburst_total=%u\nconsecutive=%u\ntripped=1\n"
            "max_key_ms=%u\nmax_bursts=%u\nmax_consecutive=%u\n",
            reason ? reason : "unknown", d->tot_burst_total, d->tot_consecutive,
            d->tot_max_key_ms, d->tot_max_bursts, d->tot_max_consecutive);
    fclose(f);
}

static void tot_trip(bcpr_device_t *d, const char *reason)
{
    if (d->tot_tripped)
        return;
    d->tot_tripped = 1;
    bcpr_ser12_force_unkey(&d->ser12);
    bcpr_hdlc_abort_tx(&d->hdlc);
    fprintf(stderr,
            "prterm-baycom: %s TOT TRIP reason=%s bursts=%u consecutive=%u\n",
            d->tag, reason ? reason : "unknown", d->tot_burst_total,
            d->tot_consecutive);
    diag("TOT TRIP reason=%s bursts=%u consecutive=%u\n",
         reason ? reason : "unknown", d->tot_burst_total, d->tot_consecutive);
    tot_write_trip_file(d, reason);
}

static void tot_on_ptt_rise(bcpr_device_t *d, unsigned t_us)
{
    unsigned gap_us;

    if (!d->tot_enabled || d->tot_tripped)
        return;
    d->tot_key_start_us = t_us;
    if (d->tot_last_off_us == 0u) {
        d->tot_consecutive = 1;
        return;
    }
    gap_us = t_us - d->tot_last_off_us;
    if (gap_us >= d->tot_min_gap_ms * 1000u) {
        if (gap_us < TOT_SESSION_RESET_US)
            d->tot_consecutive++;
        else {
            d->tot_consecutive = 1;
            d->tot_burst_total = 0;
        }
    }
    if (d->tot_consecutive > d->tot_max_consecutive)
        tot_trip(d, "max_consecutive");
}

static void tot_on_ptt_fall(bcpr_device_t *d, unsigned t_us)
{
    if (!d->tot_enabled || d->tot_tripped)
        return;
    d->tot_last_off_us = t_us;
    d->tot_burst_total++;
    if (d->tot_burst_total >= d->tot_max_bursts)
        tot_trip(d, "max_bursts");
}

static void tot_check_key_duration(bcpr_device_t *d, unsigned t_us)
{
    unsigned elapsed_us;
    unsigned max_us;

    if (!d->tot_enabled || d->tot_tripped || !d->ptt_was)
        return;
    if (d->tot_key_start_us == 0u) {
        d->tot_key_start_us = t_us;
        return;
    }
    max_us = d->tot_max_key_ms * 1000u;
    elapsed_us = t_us - d->tot_key_start_us;
    if (elapsed_us >= max_us) {
        bcpr_ser12_force_unkey(&d->ser12);
        bcpr_hdlc_abort_tx(&d->hdlc);
        tot_on_ptt_fall(d, t_us);
        tot_trip(d, "max_key");
    }
}

/* ── TX telemetry (state_dir/tx-last-<tag>) ─────────────────── */

/*
 * Everything below is the daemon runtime (engine loop, PTY, RT
 * scheduling, RX-pin detection, main). The offline test exercises the
 * algorithm core above (CRC/HDLC/SER12/TOT) and supplies its own main,
 * so this block is compiled out under __PRTERM_BAYCOM_TEST__.
 */
#ifndef __PRTERM_BAYCOM_TEST__

static void emit_tx_telemetry(bcpr_device_t *d, int64_t ptt_off_ns)
{
    char path[320];
    FILE *f;
    int64_t dur_ns;
    unsigned mean_gap = 0;
    double thr_rate = 0.0;
    double ptt_ms;

    if (g_state_dir[0] == '\0' || d->ptt_on_ns == 0)
        return;
    dur_ns = ptt_off_ns - d->ptt_on_ns;
    ptt_ms = (double)dur_ns / 1e6;
    if (d->tick_count > 0)
        mean_gap = d->gap_sum_us / d->tick_count;
    if (ptt_ms > 0.0)
        thr_rate = (double)d->thr_writes / (ptt_ms / 1000.0);
    snprintf(path, sizeof(path), "%s/tx-last-%s", g_state_dir, d->tag);
    f = fopen(path, "w");
    if (!f)
        return;
    fprintf(f,
            "ptt_on_ns=%lld\nptt_off_ns=%lld\nptt_ms=%.1f\nthr_writes=%u\n"
            "thr_rate=%.0f\nmax_tick_gap_us=%u\nmean_gap_us=%u\n"
            "gaps_gt_2x=%u\nbaud_us=%u\ntick_count=%u\n",
            (long long)d->ptt_on_ns, (long long)ptt_off_ns, ptt_ms,
            d->thr_writes, thr_rate, d->max_tick_gap_us, mean_gap,
            d->gaps_gt_2x, d->ser12.baud_us, d->tick_count);
    fclose(f);
    diag("TX done: ptt=%.1fms thr_writes=%u thr_rate=%.0f max_gap=%uus gaps2x=%u\n",
         ptt_ms, d->thr_writes, thr_rate, d->max_tick_gap_us, d->gaps_gt_2x);
}

/* ── Divisors (bcpr_engine.c) ───────────────────────────────── */

/*
 * TX: the 6N1 word clocks 8 bit-times per THR write. At 9600 baud that
 * is exactly one 1200-baud bit period — the charge pump gets one pulse
 * per air bit. (115200/8)/1200 = 12.
 */
static unsigned tx_baud_div(const bcpr_device_t *d)
{
    unsigned baud = d->ser12.baud ? d->ser12.baud : 1200u;
    unsigned div = (115200u / 8u) / baud;
    return div ? div : 1u;
}

/* Idle/RX: 800 baud sampling clock (115200/100/8 = 144). */
#define IDLE_DIVISOR (115200u / 100u / 8u)

/* ── Per-tick device processing (bcpr_engine.c tick_device) ─── */

static void tick_device(bcpr_device_t *d)
{
    int cts = 0;
    int mcr = 0x0d;
    int do_thr = 0;
    unsigned t = now_us();
    int ptt;
    int keyed = d->ptt_was; /* already in TX — keep path minimal */

    if (!keyed) {
        unsigned char msr = dev_read_msr(d);
        cts = (msr & MSR_CTS) ? 1 : 0;
        if (d->ser12.opt_dcd > 0)
            d->hdlc.dcd = (msr & MSR_DCD) ? 1 : 0;
        else if (d->ser12.opt_dcd < 0)
            d->hdlc.dcd = (msr & MSR_DCD) ? 0 : 1;
    }

    /* Tick-gap telemetry while PTT keyed. */
    if (d->ptt_was && d->last_tick_us) {
        unsigned gap = t - d->last_tick_us;
        unsigned lim2;
        if (gap > d->max_tick_gap_us)
            d->max_tick_gap_us = gap;
        d->gap_sum_us += gap;
        d->tick_count++;
        lim2 = d->ser12.baud_us * 2u;
        if (lim2 < 2u)
            lim2 = 2u;
        if (gap > lim2)
            d->gaps_gt_2x++;
    }
    d->last_tick_us = t;

    bcpr_ser12_tick(&d->ser12, &d->hdlc, cts, &mcr, &do_thr, t);
    ptt = d->ser12.ptt_hw ? 1 : 0;

    if (d->tot_tripped) {
        bcpr_ser12_force_unkey(&d->ser12);
        bcpr_hdlc_abort_tx(&d->hdlc);
        ptt = 0;
        mcr = 0x0d;
    } else {
        tot_check_key_duration(d, t);
        ptt = d->ser12.ptt_hw ? 1 : 0;
    }

    if (ptt && !d->ptt_was) {
        tot_on_ptt_rise(d, t);
        d->ptt_on_ns = now_ns();
        d->thr_writes = 0;
        d->max_tick_gap_us = 0;
        d->gap_sum_us = 0;
        d->tick_count = 0;
        d->gaps_gt_2x = 0;
        d->last_tick_us = t;
        d->tx_div_set = 0;
        d->tx_frames++;
        diag("PTT ON (frame #%lu)\n", d->tx_frames);
    } else if (!ptt && d->ptt_was) {
        if (!d->tot_tripped)
            tot_on_ptt_fall(d, t);
        /* Match baycom_ser_fdx: idle divisor only on PTT fall. */
        bcpr_uart_set_divisor(g_iobase, IDLE_DIVISOR);
        d->tx_div_set = 0;
        emit_tx_telemetry(d, now_ns());
        diag("PTT OFF\n");
    }
    d->ptt_was = ptt;

    /*
     * Set TX baud divisor ONCE on PTT rise (kernel ser12_fdx).
     * Re-writing the divisor every bit toggles DLAB mid-shift →
     * intermittent TXD charge-pump starve while MCR RTS still keys
     * (MCR PASS / no RF).
     */
    if (ptt && !d->tx_div_set) {
        bcpr_uart_set_divisor(g_iobase, tx_baud_div(d));
        d->tx_div_set = 1;
    }
    /* Kernel order: THR 0x00 first (charge-pump), then MCR bit+PTT. */
    if (do_thr) {
        uart_outb(0x00, g_iobase + UART_THR);
        if (ptt)
            d->thr_writes++;
    }
    uart_outb((unsigned char)mcr, g_iobase + UART_MCR);

    /* Defer HDLC RX drain while keyed — keeps bit deadline tight. */
    if (!ptt)
        bcpr_hdlc_receiver(&d->hdlc);
}

/* Publish Soft-/hard-DCD for status consumers (state_dir/dcd-<tag>). */
static void publish_dcd_status(bcpr_device_t *d)
{
    char path[320];
    FILE *f;

    if (g_state_dir[0] == '\0')
        return;
    snprintf(path, sizeof(path), "%s/dcd-%s", g_state_dir, d->tag);
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "dcd=%d\n", d->hdlc.dcd ? 1 : 0);
        fclose(f);
    }
}

/* ── Engine main loop (bcpr_engine.c bcpr_engine_run) ───────── */

static void engine_run(bcpr_device_t *d)
{
    struct timespec next;
    unsigned period_ns;
    unsigned tick = 0;
    int64_t bit_deadline_ns = 0;

    period_ns = d->ser12.baud_us * 1000u;
    if (period_ns < 100000u)
        period_ns = 833000u;

    clock_gettime(CLOCK_MONOTONIC, &next);
    while (!g_stop) {
        int any_ptt;
        unsigned baud_us = d->ser12.baud_us;

        tick_device(d);
        any_ptt = (d->ser12.ptt_hw || d->ptt_was) ? 1 : 0;

        /* ~100 ms at 1200 baud — skip file I/O while PTT (TXD gaps). */
        if ((++tick % 120u) == 0u && !any_ptt)
            publish_dcd_status(d);

        /*
         * While PTT: absolute bit deadline + busy-spin (not nanosleep).
         * THRE wait alone stacks with tick work → ~2× baud gaps.
         * Idle RX keeps the absolute nanosleep schedule.
         */
        if (any_ptt) {
            int64_t now;

            if (baud_us < 200u)
                baud_us = 200u;
            now = now_ns();
            if (bit_deadline_ns == 0 ||
                now > bit_deadline_ns + (int64_t)baud_us * 1000LL * 4) {
                bit_deadline_ns = now + (int64_t)baud_us * 1000LL;
            } else {
                bit_deadline_ns += (int64_t)baud_us * 1000LL;
            }
            while (now_ns() < bit_deadline_ns && !g_stop) {
            }
            clock_gettime(CLOCK_MONOTONIC, &next);
        } else {
            bit_deadline_ns = 0;
            next.tv_nsec += (long)period_ns;
            while (next.tv_nsec >= 1000000000L) {
                next.tv_nsec -= 1000000000L;
                next.tv_sec++;
            }
            while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next,
                                   NULL) == EINTR) {
                if (g_stop)
                    break;
            }
        }
    }
}

/* ── KISS PTY ───────────────────────────────────────────────── */

static int kiss_pty_open(bcpr_device_t *dev)
{
    int master, slave;
    char slave_name[256];

    if (openpty(&master, &slave, slave_name, NULL, NULL) < 0) {
        fprintf(stderr, "openpty: %s\n", strerror(errno));
        return -1;
    }

    snprintf(dev->kiss_path, sizeof(dev->kiss_path),
             "%s/kiss-%s", g_state_dir, dev->tag);
    unlink(dev->kiss_path);
    if (symlink(slave_name, dev->kiss_path) < 0)
        fprintf(stderr, "symlink %s -> %s: %s\n",
                dev->kiss_path, slave_name, strerror(errno));

    /*
     * prterm-tncd runs as lighttpd — grant access. MAX25 uses the
     * dialout/uucp/tty group; PRTERM additionally tries lighttpd.
     */
    if (fchmod(slave, 0660) == 0) {
        struct passwd *pw = getpwnam("lighttpd");
        struct group *gr = getgrnam("dialout");
        if (gr == NULL)
            gr = getgrnam("tty");
        if (pw != NULL && gr != NULL) {
            if (fchown(slave, pw->pw_uid, gr->gr_gid) != 0)
                perror("fchown kiss slave");
        } else if (pw != NULL) {
            if (fchown(slave, pw->pw_uid, pw->pw_gid) != 0)
                perror("fchown kiss slave");
        }
        if (pw != NULL)
            (void)lchown(dev->kiss_path, pw->pw_uid, pw->pw_gid);
    }
    close(slave);
    fcntl(master, F_SETFL, O_NONBLOCK);
    dev->kiss_master = master;

    printf("  KISS PTY: %s -> %s\n", dev->kiss_path, slave_name);
    return 0;
}

static void kiss_pty_close(bcpr_device_t *dev)
{
    if (dev->kiss_master >= 0) {
        close(dev->kiss_master);
        dev->kiss_master = -1;
    }
    if (dev->kiss_path[0]) {
        unlink(dev->kiss_path);
        dev->kiss_path[0] = '\0';
    }
}

/* ── KISS feed (bcpr_daemon.c kiss_feed) + queue thread ─────── */

/* 1.5 s minimum inter-frame gap, enforced in the KISS thread. */
static int engine_queue_kiss(bcpr_device_t *d, const uint8_t *kiss, int len)
{
    static unsigned last_tx_us;
    unsigned now;
    unsigned elapsed;

    if (!kiss)
        return -1;
    now = now_us();
    if (last_tx_us != 0u) {
        elapsed = now - last_tx_us;
        if (elapsed < 1500000u)
            usleep(1500000u - elapsed);
    }
    last_tx_us = now_us();
    if (d->tot_tripped) {
        fprintf(stderr, "prterm-baycom: %s TOT drop queue_kiss (tripped)\n",
                d->tag);
        diag("TX REJECTED: TOT tripped\n");
        return -1;
    }
    return bcpr_hdlc_queue_kiss(&d->hdlc, kiss, len);
}

static void kiss_feed(bcpr_device_t *d)
{
    static uint8_t acc[BCPR_MAXFLEN + 8];
    static int alen;
    static int esc;
    static int in_frame;
    uint8_t buf[512];
    ssize_t n;
    int i;

    n = read(d->kiss_master, buf, sizeof(buf));
    if (n <= 0)
        return;
    diag("PTY read %zd bytes\n", n);
    for (i = 0; i < (int)n; i++) {
        uint8_t b = buf[i];
        if (!in_frame) {
            if (b == KISS_FEND) {
                in_frame = 1;
                alen = 0;
                esc = 0;
            }
            continue;
        }
        if (esc) {
            esc = 0;
            if (b == KISS_TFEND)
                b = KISS_FEND;
            else if (b == KISS_TFESC)
                b = KISS_FESC;
            if (alen < (int)sizeof(acc))
                acc[alen++] = b;
            continue;
        }
        if (b == KISS_FESC) {
            esc = 1;
            continue;
        }
        if (b == KISS_FEND) {
            if (alen >= 2) {
                int q, w;
                for (w = 0; w < 70; w++) {
                    q = engine_queue_kiss(d, acc, alen);
                    if (q == 0)
                        break;
                    usleep(50000);
                }
                if (q != 0) {
                    diag("TX REJECTED: busy (len=%d)\n", alen);
                    fprintf(stderr,
                            "prterm-baycom: queue_kiss drop %s len=%d (busy)\n",
                            d->tag, alen);
                } else {
                    diag("KISS QUEUED %d bytes\n", alen);
                }
            }
            in_frame = 0;
            alen = 0;
            continue;
        }
        if (alen < (int)sizeof(acc))
            acc[alen++] = b;
    }
}

static void *kiss_thread(void *arg)
{
    bcpr_device_t *d = (bcpr_device_t *)arg;

    while (!g_stop) {
        struct pollfd pf;
        int ret;

        if (d->kiss_master < 0) {
            usleep(50000);
            continue;
        }
        pf.fd = d->kiss_master;
        pf.events = POLLIN | POLLHUP | POLLERR;
        pf.revents = 0;
        ret = poll(&pf, 1, 50);
        if (ret <= 0)
            continue;
        if (pf.revents & POLLIN)
            kiss_feed(d);
        if (pf.revents & (POLLHUP | POLLERR | POLLNVAL))
            usleep(50000);
    }
    return NULL;
}

/* ── RT scheduling + mlockall (bcpr_engine.c try_rt) ────────── */

static void try_rt(void)
{
    struct sched_param sp;
    int rc;

    memset(&sp, 0, sizeof(sp));
#if defined(__linux__)
    /* Pin pages — fault during PTT = multi-ms TXD gap → pump collapse. */
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
        fprintf(stderr, "prterm-baycom: mlockall failed errno=%d\n", errno);
    {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(0, &set);
        if (sched_setaffinity(0, sizeof(set), &set) != 0)
            fprintf(stderr, "prterm-baycom: sched_setaffinity(0) errno=%d\n",
                    errno);
    }
#endif
    /* High FIFO while bit-clocking — charge pump cannot tolerate ms
     * preemption. Needs root. */
    sp.sched_priority = 80;
    rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    if (rc != 0) {
        sp.sched_priority = 50;
        rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    }
    if (rc != 0) {
        sp.sched_priority = 10;
        rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    }
    if (rc != 0)
        fprintf(stderr,
                "prterm-baycom: SCHED_FIFO failed errno=%d — expect TXD gaps "
                "(need root)\n", rc);
    else
        fprintf(stderr, "prterm-baycom: SCHED_FIFO ok prio=%d\n",
                sp.sched_priority);
}

/* ── Signal handler ─────────────────────────────────────────── */

static void on_sig(int sig)
{
    (void)sig;
    g_stop = 1;
}

/* ── RX pin auto-detect (FreeBSD ioctl / Linux MSR) ─────────── */

#if defined(__FreeBSD__)
static int detect_rx_pin_freebsd(int fd, int seconds)
{
    struct timespec start, now;
    int masks[4] = {TIOCM_CTS, TIOCM_DSR, TIOCM_RI, TIOCM_CD};
    const char *names[4] = {"CTS", "DSR", "RI", "DCD"};
    int trans[4] = {0, 0, 0, 0};
    int last[4] = {-1, -1, -1, -1};
    int best = -1;

    /*
     * Power the modem via DTR only — NEVER assert RTS here: RTS is the
     * PTT line and asserting it would put the radio on air with no data.
     */
    {
        int bits = TIOCM_DTR;
        ioctl(fd, TIOCMBIS, &bits);
    }

    fprintf(stderr, "  RX auto-detect: monitoring %d seconds...\n", seconds);
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        int bits = 0;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if ((now.tv_sec - start.tv_sec) >= seconds)
            break;
        if (ioctl(fd, TIOCMGET, &bits) < 0)
            break;
        for (int i = 0; i < 4; i++) {
            int val = (bits & masks[i]) ? 1 : 0;
            if (last[i] >= 0 && val != last[i])
                trans[i]++;
            last[i] = val;
        }
        usleep(500);
    }
    for (int i = 0; i < 4; i++) {
        fprintf(stderr, "  %s: transitions=%d\n", names[i], trans[i]);
        if (best < 0 || trans[i] > trans[best])
            best = i;
    }
    if (best >= 0 && trans[best] > 50) {
        fprintf(stderr, "  ==> RX on %s (%d transitions)\n",
                names[best], trans[best]);
        return masks[best];
    }
    fprintf(stderr, "  ==> No RX signal detected.\n");
    return 0;
}
#else
static int detect_rx_pin_linux(unsigned long iobase, int seconds)
{
    struct timespec start, now;
    int bits[4] = {MSR_CTS, MSR_DSR, MSR_RI, MSR_DCD};
    const char *names[4] = {"CTS", "DSR", "RI", "DCD"};
    int trans[4] = {0, 0, 0, 0};
    int last[4] = {-1, -1, -1, -1};
    int best = -1;

    fprintf(stderr, "  RX auto-detect: monitoring %d seconds...\n", seconds);
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        unsigned char msr;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if ((now.tv_sec - start.tv_sec) >= seconds)
            break;
        msr = uart_inb(iobase + UART_MSR);
        for (int i = 0; i < 4; i++) {
            int val = (msr & bits[i]) ? 1 : 0;
            if (last[i] >= 0 && val != last[i])
                trans[i]++;
            last[i] = val;
        }
        usleep(500);
    }
    for (int i = 0; i < 4; i++) {
        fprintf(stderr, "  %s: transitions=%d\n", names[i], trans[i]);
        if (best < 0 || trans[i] > trans[best])
            best = i;
    }
    if (best >= 0 && trans[best] > 50) {
        fprintf(stderr, "  ==> RX on %s (%d transitions)\n",
                names[best], trans[best]);
        return bits[best];
    }
    fprintf(stderr, "  ==> No RX signal detected.\n");
    return 0;
}
#endif

/* ── Usage ──────────────────────────────────────────────────── */

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  -d <device>    Serial port (FreeBSD /dev/cuau0, Linux /dev/ttyS0)\n"
        "  -i <iobase>    UART I/O base (e.g. 0x3f8 for COM1)\n"
        "  -b <baud>      On-air baud rate (default: 1200)\n"
        "  -t <tag>       Device tag (default: bc0)\n"
        "  -s <dir>       State/runtime directory\n"
        "  -C             Calibration mode (continuous PTT + toggle tone)\n"
        "  -h             This help\n"
        "\n"
        "TX protection is always on: PTT watchdog 14.5s/500ms, TOT 25s key,\n"
        "3 consecutive / 8 total bursts (trip latches until restart), CSMA.\n",
        prog);
}

/* ── Main ───────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    bcpr_device_t *dev = &g_dev;
    int opt;
    unsigned long iobase = 0;
    unsigned baud = BCPR_DEFAULT_BAUD;
    const char *device = NULL;
    const char *tag = "bc0";
    int cal_mode = BCPR_CAL_OFF;
    pthread_t kthr;
    int kthr_ok = 0;

    memset(dev, 0, sizeof(*dev));
    dev->kiss_master = -1;
    dev->serial_fd = -1;

    while ((opt = getopt(argc, argv, "d:b:i:t:s:Ch")) != -1) {
        switch (opt) {
        case 'd':
            device = optarg;
            break;
        case 'b':
            baud = (unsigned)atoi(optarg);
            break;
        case 'i':
            iobase = strtoul(optarg, NULL, 0);
            break;
        case 't':
            tag = optarg;
            break;
        case 's':
            strncpy(g_state_dir, optarg, sizeof(g_state_dir) - 1);
            break;
        case 'C':
            cal_mode = BCPR_CAL_ALT;
            break;
        case 'h':
        default:
            usage(argv[0]);
            return (opt == 'h') ? 0 : 1;
        }
    }

    if (device == NULL) {
        fprintf(stderr, "Error: -d <device> is required\n");
        usage(argv[0]);
        return 1;
    }
    if (iobase == 0) {
        fprintf(stderr, "Error: -i <iobase> is required (e.g. 0x3f8)\n");
        return 1;
    }

    srand((unsigned)time(NULL));
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    signal(SIGPIPE, SIG_IGN);

    /* Open serial port (modem-line access; FreeBSD ioctl RX path). */
    dev->serial_fd = open(device, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (dev->serial_fd < 0) {
        fprintf(stderr, "open %s: %s\n", device, strerror(errno));
        return 1;
    }
    {
        struct termios tio;
        if (tcgetattr(dev->serial_fd, &tio) == 0) {
            cfmakeraw(&tio);
            tio.c_cflag |= CLOCAL;
            tio.c_cflag &= ~(unsigned)HUPCL;
            tcsetattr(dev->serial_fd, TCSANOW, &tio);
        }
    }

    /* Acquire I/O ports (root). */
    if (bcpr_ioperm(iobase, 8, 1) < 0) {
        fprintf(stderr, "ioperm(0x%lx): %s (need root)\n", iobase,
                strerror(errno));
        close(dev->serial_fd);
        return 1;
    }
    g_iobase = iobase;

    strncpy(dev->tag, tag, sizeof(dev->tag) - 1);
    strncpy(dev->serial_path, device, sizeof(dev->serial_path) - 1);
    dev->iobase = iobase;

    /* Diagnostic log */
    {
        char diag_path[400];
        snprintf(diag_path, sizeof(diag_path), "%s/baycom-diag.log",
                 g_state_dir);
        g_diag = fopen(diag_path, "a");
        if (g_diag) {
            time_t now = time(NULL);
            fprintf(g_diag, "=== prterm-baycom started %s", ctime(&now));
            fflush(g_diag);
        }
    }

#if defined(__FreeBSD__)
    /*
     * DTR via ioctl for modem power. NEVER RTS: RTS is PTT — the old
     * code asserted both here, which keyed the radio at daemon start
     * with no data on air. MCR writes (outb) own the PTT line from
     * here on.
     */
    {
        int bits = TIOCM_DTR;
        ioctl(dev->serial_fd, TIOCMBIS, &bits);
    }

    /* RX pin detection (ioctl). Default CTS (SER12 standard). */
    {
        int detected = detect_rx_pin_freebsd(dev->serial_fd, 3);
        if (detected > 0)
            g_rx_pin_mask = detected;
        else
            fprintf(stderr,
                    "  Warning: no RX signal detected, using CTS default.\n");
    }
#else
    /* RX pin detection (MSR). Default CTS (SER12 standard). */
    {
        int detected = detect_rx_pin_linux(iobase, 3);
        if (detected > 0)
            g_linux_rx_msr_bit = detected;
        else
            fprintf(stderr,
                    "  Warning: no RX signal detected, using CTS default.\n");
    }
#endif

    /* Init SER12 + HDLC + TOT (MAX25 defaults). */
    bcpr_ser12_init(&dev->ser12, baud, 0);   /* opt_dcd=0: Soft-DCD */
    {
        bcpr_channel_t ch;
        ch.tx_delay = BCPR_DEFAULT_TX_DELAY;
        ch.tx_tail = BCPR_DEFAULT_TX_TAIL;
        ch.slottime = BCPR_DEFAULT_SLOTTIME;
        ch.ppersist = BCPR_DEFAULT_PERSIST;
        ch.fulldup = 0;   /* half duplex: CSMA p-persistence */
        bcpr_hdlc_init(&dev->hdlc, (int)baud, &ch);
    }
    dev->tot_enabled = 1;
    dev->tot_max_key_ms = BCPR_DEFAULT_TOT_MAX_KEY_MS;
    dev->tot_min_gap_ms = BCPR_DEFAULT_TOT_MIN_GAP_MS;
    dev->tot_max_consecutive = BCPR_DEFAULT_TOT_MAX_CONSEC;
    dev->tot_max_bursts = BCPR_DEFAULT_TOT_MAX_BURSTS;

    if (cal_mode != BCPR_CAL_OFF)
        bcpr_ser12_set_cal(&dev->ser12, cal_mode);

    /* UART: idle divisor + SER12 register setup (baycom_ser_fdx). */
    bcpr_uart_set_divisor(iobase, IDLE_DIVISOR);
    bcpr_uart_open_ser12(iobase);

    /* KISS PTY for prterm-tncd (kiss_init = native). */
    if (cal_mode == BCPR_CAL_OFF) {
        if (kiss_pty_open(dev) < 0) {
            bcpr_uart_close_ser12(iobase);
            bcpr_ioperm(iobase, 8, 0);
            close(dev->serial_fd);
            return 1;
        }
    }

    try_rt();

    printf("prterm-baycom: %s @ %u baud, iobase=0x%lx, tag=%s\n",
           device, baud, iobase, tag);
    printf("  PTT watchdog: %s (%u/%u ms), TOT: %s (key=%ums consec=%u bursts=%u)\n",
           dev->ser12.ptt_wd ? "on" : "off",
           dev->ser12.ptt_wd_key_us / 1000u,
           dev->ser12.ptt_wd_pause_us / 1000u,
           dev->tot_enabled ? "on" : "off", dev->tot_max_key_ms,
           dev->tot_max_consecutive, dev->tot_max_bursts);
    printf("  CSMA: half-duplex p-persistence (slot=%dms p=%d/255)\n",
           dev->hdlc.ch.slottime, dev->hdlc.ch.ppersist);

    if (cal_mode != BCPR_CAL_OFF)
        fprintf(stderr,
                "prterm-baycom: *** WATCH NOW *** cal PTT+tone — Ctrl+C ends\n");

    if (dev->kiss_master >= 0 &&
        pthread_create(&kthr, NULL, kiss_thread, dev) == 0)
        kthr_ok = 1;

    engine_run(dev);

    /* Cleanup — never leave the radio keyed. */
    g_stop = 1;
    if (kthr_ok)
        pthread_join(kthr, NULL);
    bcpr_ser12_force_unkey(&dev->ser12);
    uart_outb(0x00, iobase + UART_IER);
    bcpr_uart_close_ser12(iobase);
    kiss_pty_close(dev);
    close(dev->serial_fd);
    bcpr_ioperm(iobase, 8, 0);
    diag("=== shutdown (rx=%lu crc_err=%lu tx=%lu)\n",
         dev->rx_frames, dev->rx_crc_err, dev->tx_frames);
    if (g_diag)
        fclose(g_diag);

    printf("prterm-baycom: shutdown (rx=%lu tx=%lu)\n",
           dev->rx_frames, dev->tx_frames);
    return 0;
}

#endif /* !__PRTERM_BAYCOM_TEST__ */