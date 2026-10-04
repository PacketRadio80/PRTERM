/*
 * PRTERM - Test: KISS-Framing und AX.25-Rahmenbau
 *
 * SPDX-License-Identifier: MIT
 */
#include "prterm_compat.h"

#include "callsign.h"
#include "kiss.h"
#include "testutil.h"

#include <string.h>

int main(void)
{
    printf("== AX.25-FCS ==\n");
    {
        unsigned char d[] = "123456789";
        uint16_t crc = kiss_fcs(d, 9);

        /* FCS anhaengen (little-endian) und pruefen */
        unsigned char frame[16];
        memcpy(frame, d, 9);
        frame[9]  = (unsigned char)(crc & 0xff);
        frame[10] = (unsigned char)(crc >> 8);
        CHECK(kiss_fcs_ok(frame, 11));

        /* Ein vertauschtes Bit muss auffallen */
        frame[3] ^= 0xff;
        CHECK(!kiss_fcs_ok(frame, 11));

        /* Zu kurze Rahmen sind ungueltig */
        CHECK(!kiss_fcs_ok(frame, 1));
    }

    printf("\n== KISS-Escape ==\n");
    {
        unsigned char in[]  = { KISS_FEND, KISS_FESC, 0x41 };
        unsigned char out[16];
        size_t n = kiss_escape(out, sizeof out, in, sizeof in);
        CHECK_INT(n, 5);
        CHECK_INT(out[0], KISS_FESC);
        CHECK_INT(out[1], KISS_TFEND);
        CHECK_INT(out[2], KISS_FESC);
        CHECK_INT(out[3], KISS_TFESC);
        CHECK_INT(out[4], 0x41);
    }

    printf("\n== KISS-Rahmen bauen ==\n");
    {
        unsigned char payload[] = { 0x01, 0x02, KISS_FEND, 0x03 };
        unsigned char frame[64];
        size_t n = kiss_encode(frame, sizeof frame, 0, KISS_CMD_DATA,
                               payload, sizeof payload);
        CHECK(n > 0);
        CHECK_INT(frame[0], KISS_FEND);
        CHECK_INT(frame[n - 1], KISS_FEND);
        CHECK_INT(frame[1], KISS_CMD_DATA);
        /* Das FEND im Payload muss escaped sein */
        CHECK(n == 1 + 1 + sizeof payload + 1 + 1);  /* +1 fuer das Escape */
    }

    printf("\n== KISS Roundtrip ==\n");
    {
        unsigned char payload[256];
        for (size_t i = 0; i < sizeof payload; i++)
            payload[i] = (unsigned char)(i & 0xff);   /* enthaelt FEND/FESC */

        unsigned char frame[512];
        size_t n = kiss_encode(frame, sizeof frame, 2, KISS_CMD_DATA,
                               payload, sizeof payload);
        CHECK(n > 0);

        kiss_decoder d;
        kiss_decoder_init(&d);
        size_t frames = kiss_decoder_feed_buf(&d, frame, n);
        CHECK_INT(frames, 1);

        unsigned char back[512];
        size_t m = kiss_decoder_take(&d, back, sizeof back);
        CHECK_INT(m, sizeof payload);
        CHECK(memcmp(back, payload, sizeof payload) == 0);
    }

    printf("\n== KISS Mehrere Rahmen ==\n");
    {
        unsigned char stream[256];
        size_t len = 0;

        unsigned char p1[] = { 0x11, 0x22 };
        unsigned char p2[] = { 0x33 };
        len += kiss_encode(stream + len, sizeof stream - len, 0,
                           KISS_CMD_DATA, p1, sizeof p1);
        len += kiss_encode(stream + len, sizeof stream - len, 0,
                           KISS_CMD_DATA, p2, sizeof p2);

        kiss_decoder d;
        kiss_decoder_init(&d);
        CHECK_INT(kiss_decoder_feed_buf(&d, stream, len), 2);

        unsigned char back[16];
        CHECK_INT(kiss_decoder_take(&d, back, sizeof back), 2);
        CHECK_INT(back[0], 0x11);
        CHECK_INT(kiss_decoder_take(&d, back, sizeof back), 1);
        CHECK_INT(back[0], 0x33);
    }

    printf("\n== KISS-Parameter ==\n");
    {
        unsigned char frame[16];
        unsigned char v = 1;
        size_t n = kiss_encode(frame, sizeof frame, 0, KISS_CMD_FULLDUPLEX, &v, 1);
        CHECK(n == 4);                    /* FEND | typ | wert | FEND */
        CHECK_INT(frame[1], KISS_CMD_FULLDUPLEX);
        CHECK_INT(frame[2], 1);
    }

    printf("\n== AX.25-UI-Rahmen ==\n");
    {
        unsigned char frame[128];
        const char *info = "Hallo";
        size_t n = ax25_ui_frame(frame, sizeof frame, "DL1ABC-1", "CQ",
                                 (const unsigned char *)info, strlen(info));
        CHECK_INT(n, 16 + 5);

        /* Zieladresse CQ, ohne Endekennung */
        char to[16], from[16];
        CHECK(call_from_ax25(frame, to, sizeof to));
        CHECK_STR(to, "CQ");
        CHECK((frame[6] & 0x01) == 0);     /* kein Ende */

        /* Quelladresse mit Endekennung */
        CHECK(call_from_ax25(frame + 7, from, sizeof from));
        CHECK_STR(from, "DL1ABC-1");
        CHECK((frame[13] & 0x01) == 1);    /* Ende gesetzt */

        /* Control und PID */
        CHECK_INT(frame[14], 0x03);
        CHECK_INT(frame[15], 0xF0);

        /* Nutzdaten */
        CHECK(memcmp(frame + 16, info, 5) == 0);
    }

    printf("\n== AX.25-Adresse codieren/decodieren ==\n");
    {
        unsigned char a[7];
        CHECK(call_to_ax25("DL1ABC-1", a));
        CHECK_INT(a[0], (unsigned char)('D' << 1));

        char back[16];
        CHECK(call_from_ax25(a, back, sizeof back));
        CHECK_STR(back, "DL1ABC-1");
    }

    TEST_SUMMARY("kiss");
}
