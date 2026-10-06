/*
 * PRTERM - CB & Amateur Radio Terminal
 * kiss.c - KISS framing.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "prterm_compat.h"

#include "kiss.h"

#include <string.h>

/* ======================================================================= */
/* AX.25-FCS                                                               */
/* ======================================================================= */

/*
 * CRC-16/AX.25: reflected polynomial 0x8408, init 0xFFFF, final XOR 0xFFFF.
 * Implemented bitwise - portable and without a lookup table.
 */
uint16_t kiss_fcs(const unsigned char *data, size_t len)
{
    uint16_t fcs = 0xFFFFu;
    for (size_t i = 0; i < len; i++) {
        fcs ^= data[i];
        for (int b = 0; b < 8; b++) {
            if (fcs & 1u)
                fcs = (uint16_t)((fcs >> 1) ^ 0x8408u);
            else
                fcs = (uint16_t)(fcs >> 1);
        }
    }
    return (uint16_t)(fcs ^ 0xFFFFu);
}

bool kiss_fcs_ok(const unsigned char *frame, size_t len)
{
    if (frame == NULL || len < 2)
        return false;
    /* FCS is at the end, little-endian */
    uint16_t want = (uint16_t)(frame[len - 2] | ((uint16_t)frame[len - 1] << 8));
    return kiss_fcs(frame, len - 2) == want;
}

/* ======================================================================= */
/* Encoding                                                                */
/* ======================================================================= */

size_t kiss_escape(unsigned char *out, size_t outcap,
                   const unsigned char *in, size_t inlen)
{
    size_t w = 0;
    for (size_t i = 0; i < inlen; i++) {
        unsigned char c = in[i];
        if (c == KISS_FEND) {
            if (w + 2 > outcap) return 0;
            out[w++] = KISS_FESC;
            out[w++] = KISS_TFEND;
        } else if (c == KISS_FESC) {
            if (w + 2 > outcap) return 0;
            out[w++] = KISS_FESC;
            out[w++] = KISS_TFESC;
        } else {
            if (w + 1 > outcap) return 0;
            out[w++] = c;
        }
    }
    return w;
}

size_t kiss_encode(unsigned char *out, size_t outcap,
                   unsigned port, unsigned cmd,
                   const unsigned char *payload, size_t plen)
{
    if (outcap < 3)
        return 0;

    size_t w = 0;
    out[w++] = KISS_FEND;

    unsigned char type = (unsigned char)(((port & 0x0fu) << 4) | (cmd & 0x0fu));
    if (type == KISS_FEND || type == KISS_FESC) {
        size_t n = kiss_escape(out + w, outcap - w, &type, 1);
        if (n == 0) return 0;
        w += n;
    } else {
        out[w++] = type;
    }

    if (plen > 0) {
        size_t n = kiss_escape(out + w, outcap - w, payload, plen);
        if (n == 0) return 0;
        w += n;
    }

    if (w + 1 > outcap)
        return 0;
    out[w++] = KISS_FEND;
    return w;
}

/* ======================================================================= */
/* Decoding                                                                */
/* ======================================================================= */

void kiss_decoder_init(kiss_decoder *d)
{
    memset(d, 0, sizeof *d);
}

/* Closes the running frame and queues it.                                */
static bool kiss_push_frame(kiss_decoder *d)
{
    if (d->curlen == 0) {
        d->in_frame = false;
        d->esc = false;
        return false;
    }

    if (d->qcount < KISS_MAX_PENDING) {
        size_t slot = (d->qhead + d->qcount) % KISS_MAX_PENDING;
        memcpy(d->done[slot], d->cur, d->curlen);
        d->done_len[slot] = d->curlen;
        d->qcount++;
    }
    /* else: discard the frame, the device sends faster than we read      */

    d->curlen = 0;
    d->in_frame = false;
    d->esc = false;
    return true;
}

bool kiss_decoder_feed(kiss_decoder *d, unsigned char byte)
{
    if (byte == KISS_FEND) {
        if (d->in_frame && d->curlen > 0)
            return kiss_push_frame(d);

        /* new frame starts     */
        d->in_frame = true;
        d->curlen = 0;
        d->esc = false;
        return false;
    }

    if (!d->in_frame)
        return false;               /* outside a frame          */

    if (d->esc) {
        d->esc = false;
        if (byte == KISS_TFEND)      byte = KISS_FEND;
        else if (byte == KISS_TFESC) byte = KISS_FESC;
    } else if (byte == KISS_FESC) {
        d->esc = true;
        return false;
    }

    if (d->curlen == 0) {
        /* first byte is the type byte   */
        d->cmd  = byte & 0x0fu;
        d->port = (unsigned)(byte >> 4);
    }

    if (d->curlen < sizeof d->cur)
        d->cur[d->curlen++] = byte;

    return false;
}

size_t kiss_decoder_feed_buf(kiss_decoder *d, const unsigned char *buf, size_t len)
{
    size_t before = d->qcount;
    for (size_t i = 0; i < len; i++)
        (void)kiss_decoder_feed(d, buf[i]);
    return d->qcount - before;
}

size_t kiss_decoder_ready(const kiss_decoder *d)
{
    return d->qcount;
}

size_t kiss_decoder_take(kiss_decoder *d, unsigned char *out, size_t outcap)
{
    if (d->qcount == 0)
        return 0;

    /*
     * The first byte is the type byte - the payload starts after it.
     * For DATA that is the AX.25 frame.
     */
    size_t n = d->done_len[d->qhead] > 0 ? d->done_len[d->qhead] - 1 : 0;
    if (n > outcap)
        n = outcap;
    if (out != NULL && n > 0)
        memcpy(out, d->done[d->qhead] + 1, n);

    d->qhead = (d->qhead + 1) % KISS_MAX_PENDING;
    d->qcount--;
    return n;
}
