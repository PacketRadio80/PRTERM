/*
 * PRTERM - CB & Amateur Radio Terminal
 * kiss.c - KISS-Framing.
 *
 * SPDX-License-Identifier: MIT
 */
#include "prterm_compat.h"

#include "kiss.h"

#include <string.h>

/* ======================================================================= */
/* AX.25-FCS                                                               */
/* ======================================================================= */

/*
 * CRC-16/AX.25: reflektiertes Polynom 0x8408, Init 0xFFFF, Final-XOR 0xFFFF.
 * Bitweise implementiert - portabel und ohne Nachschlagetabelle.
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
    /* FCS steht am Ende, little-endian */
    uint16_t want = (uint16_t)(frame[len - 2] | ((uint16_t)frame[len - 1] << 8));
    return kiss_fcs(frame, len - 2) == want;
}

/* ======================================================================= */
/* Codieren                                                                */
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
/* Decodieren                                                              */
/* ======================================================================= */

void kiss_decoder_init(kiss_decoder *d)
{
    memset(d, 0, sizeof *d);
}

bool kiss_decoder_feed(kiss_decoder *d, unsigned char byte)
{
    if (byte == KISS_FEND) {
        if (d->in_frame && d->len > 0) {
            /* Rahmen vollstaendig */
            d->ready = true;
            d->in_frame = false;
            d->esc = false;
            return true;
        }
        /* neuer Rahmen beginnt */
        d->in_frame = true;
        d->len = 0;
        d->esc = false;
        return false;
    }

    if (!d->in_frame)
        return false;               /* ausserhalb eines Rahmens */

    if (d->esc) {
        d->esc = false;
        if (byte == KISS_TFEND)      byte = KISS_FEND;
        else if (byte == KISS_TFESC) byte = KISS_FESC;
        /* sonst: ungueltig, Byte wird trotzdem uebernommen */
    } else if (byte == KISS_FESC) {
        d->esc = true;
        return false;
    }

    if (d->len == 0) {
        /* erstes Byte ist die Typangabe */
        d->cmd  = byte & 0x0fu;
        d->port = (unsigned)(byte >> 4);
    }

    if (d->len < sizeof d->buf)
        d->buf[d->len++] = byte;

    return false;
}

size_t kiss_decoder_feed_buf(kiss_decoder *d, const unsigned char *buf, size_t len)
{
    size_t frames = 0;
    for (size_t i = 0; i < len; i++) {
        if (kiss_decoder_feed(d, buf[i]))
            frames++;
    }
    return frames;
}

size_t kiss_decoder_take(kiss_decoder *d, unsigned char *out, size_t outcap)
{
    if (!d->ready)
        return 0;

    /*
     * Das erste Byte in buf ist die Typangabe - der Nutzen beginnt danach.
     * Bei DATA ist das der AX.25-Rahmen.
     */
    size_t n = d->len > 0 ? d->len - 1 : 0;
    if (n > outcap)
        n = outcap;
    if (out != NULL && n > 0)
        memcpy(out, d->buf + 1, n);

    d->ready = false;
    d->len = 0;
    return n;
}
