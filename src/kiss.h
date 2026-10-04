/*
 * PRTERM - CB & Amateur Radio Terminal
 * kiss.h - KISS-Framing (RFC-artige Uebung, TNC2-Klasse).
 *
 *   FEND  0xC0   Rahmenanfang / -ende
 *   FESC  0xDB   Escape
 *   TFEND 0xDC   FEND nach Escape
 *   TFESC 0xDD   FESC nach Escape
 *
 * Rahmen:  FEND | (port<<4)|cmd | escaped payload | FEND
 *
 * SPDX-License-Identifier: MIT
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

/* Kommandos im Nibble high, Port im Nibble low */
#define KISS_CMD_DATA       0x00u
#define KISS_CMD_TXDELAY    0x01u
#define KISS_CMD_PERSIST    0x02u
#define KISS_CMD_SLOTTIME   0x03u
#define KISS_CMD_TXTAIL     0x04u
#define KISS_CMD_FULLDUPLEX 0x05u
#define KISS_CMD_SETHARDWARE 0x06u
#define KISS_CMD_RETURN     0xFFu   /* zurueck in den Command-Mode */

/* --- AX.25-FCS ---------------------------------------------------------
 * Reflektiertes Polynom 0x8408, Init 0xFFFF, Final-XOR 0xFFFF.
 * Wird gebraucht, um den FCS vor dem Senden zu entfernen. */
uint16_t kiss_fcs(const unsigned char *data, size_t len);
bool     kiss_fcs_ok(const unsigned char *frame, size_t len);

/* --- Codieren ---------------------------------------------------------- */
/*
 * Baut einen Rahmen. FCS wird NICHT ergaenzt - KISS bekommt den
 * AX.25-Rahmen ohne FCS.
 *
 *   out      Zielpuffer
 *   outcap   dessen Groesse
 *   port     0..15
 *   cmd      KISS_CMD_*
 *   payload  Nutzdaten (bei DATA der AX.25-Rahmen ohne FCS)
 *
 * Liefert die Laenge des Rahmens, 0 wenn der Puffer zu klein ist.
 */
size_t kiss_encode(unsigned char *out, size_t outcap,
                   unsigned port, unsigned cmd,
                   const unsigned char *payload, size_t plen);

/* Escape-Einzelbytes; liefert die Laenge. */
size_t kiss_escape(unsigned char *out, size_t outcap,
                   const unsigned char *in, size_t inlen);

/* --- Decodieren --------------------------------------------------------
 * Ein Decoder kann mehrere Rahmen aufnehmen, die in EINEM Lesevorgang
 * ankommen. Ein einzelner Zielrahmen wuerde vom naechsten ueberschrieben. */

#define KISS_MAX_PENDING  8
#define KISS_FRAME_MAX    640

typedef struct kiss_decoder {
    /* laufender Rahmen */
    unsigned char cur[KISS_FRAME_MAX];
    size_t        curlen;
    bool          in_frame;
    bool          esc;

    /* fertige Rahmen */
    unsigned char done[KISS_MAX_PENDING][KISS_FRAME_MAX];
    size_t        done_len[KISS_MAX_PENDING];
    size_t        qhead;
    size_t        qcount;

    unsigned      cmd;
    unsigned      port;
} kiss_decoder;

void kiss_decoder_init(kiss_decoder *d);

/* Fuettert ein Byte. true wenn dadurch ein Rahmen vollstaendig wurde. */
bool kiss_decoder_feed(kiss_decoder *d, unsigned char byte);

/* Fuettert einen Block. Liefert die Anzahl NEUER vollstaendiger Rahmen. */
size_t kiss_decoder_feed_buf(kiss_decoder *d, const unsigned char *buf, size_t len);

/* Wie viele Rahmen warten auf Abholung. */
size_t kiss_decoder_ready(const kiss_decoder *d);

/*
 * Liefert den aeltesten fertigen Rahmen OHNE Typbyte und nimmt ihn aus der
 * Warteschlange. 0 wenn keiner wartet oder der Puffer zu klein ist.
 */
size_t kiss_decoder_take(kiss_decoder *d, unsigned char *out, size_t outcap);

#endif /* PRTERM_KISS_H */
