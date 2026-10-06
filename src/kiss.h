/*
 * PRTERM - CB & Amateur Radio Terminal
 * kiss.h - KISS framing (RFC-style exercise, TNC2 class).
 *
 *   FEND  0xC0   frame start / end
 *   FESC  0xDB   escape
 *   TFEND 0xDC   FEND after escape
 *   TFESC 0xDD   FESC after escape
 *
 * Frame:  FEND | (port<<4)|cmd | escaped payload | FEND
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PRTERM_KISS_H
#define PRTERM_KISS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KISS_FEND  0xC0u
#define KISS_FESC  0xDBu
#define KISS_TFEND 0xDCu
#define KISS_TFESC 0xDDu

/* Command in the high nibble, port in the low nibble */
#define KISS_CMD_DATA       0x00u
#define KISS_CMD_TXDELAY    0x01u
#define KISS_CMD_PERSIST    0x02u
#define KISS_CMD_SLOTTIME   0x03u
#define KISS_CMD_TXTAIL     0x04u
#define KISS_CMD_FULLDUPLEX 0x05u
#define KISS_CMD_SETHARDWARE 0x06u
#define KISS_CMD_RETURN     0xFFu   /* back to command mode        */

/* --- AX.25 FCS ---------------------------------------------------------
 * Reflected polynomial 0x8408, init 0xFFFF, final XOR 0xFFFF.
 * Needed to strip the FCS before transmitting. */
uint16_t kiss_fcs(const unsigned char *data, size_t len);
bool     kiss_fcs_ok(const unsigned char *frame, size_t len);

/* --- Encoding ---------------------------------------------------------- */
/*
 * Builds a frame. The FCS is NOT added - KISS gets the AX.25 frame
 * without FCS.
 *
 *   out      target buffer
 *   outcap   its size
 *   port     0..15
 *   cmd      KISS_CMD_*
 *   payload  payload (for DATA the AX.25 frame without FCS)
 *
 * Returns the frame length, 0 if the buffer is too small.
 */
size_t kiss_encode(unsigned char *out, size_t outcap,
                   unsigned port, unsigned cmd,
                   const unsigned char *payload, size_t plen);

/* Escapes individual bytes; returns the length. */
size_t kiss_escape(unsigned char *out, size_t outcap,
                   const unsigned char *in, size_t inlen);

/* --- Decoding --------------------------------------------------------
 * One decoder can hold several frames arriving in ONE read. A single
 * target frame would be overwritten by the next one. */

#define KISS_MAX_PENDING  8
#define KISS_FRAME_MAX    640

typedef struct kiss_decoder {
    /* running frame    */
    unsigned char cur[KISS_FRAME_MAX];
    size_t        curlen;
    bool          in_frame;
    bool          esc;

    /* finished frames */
    unsigned char done[KISS_MAX_PENDING][KISS_FRAME_MAX];
    size_t        done_len[KISS_MAX_PENDING];
    size_t        qhead;
    size_t        qcount;

    unsigned      cmd;
    unsigned      port;
} kiss_decoder;

void kiss_decoder_init(kiss_decoder *d);

/* Feeds one byte. True if this completed a frame.                     */
bool kiss_decoder_feed(kiss_decoder *d, unsigned char byte);

/* Feeds a block. Returns the number of NEW completed frames.            */
size_t kiss_decoder_feed_buf(kiss_decoder *d, const unsigned char *buf, size_t len);

/* How many frames are waiting for pickup. */
size_t kiss_decoder_ready(const kiss_decoder *d);

/*
 * Returns the oldest finished frame WITHOUT the type byte and removes
 * it from the queue. 0 if none waits or the buffer is too small.
 */
size_t kiss_decoder_take(kiss_decoder *d, unsigned char *out, size_t outcap);

#endif /* PRTERM_KISS_H */
