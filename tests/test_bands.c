/*
 * PRTERM - Test: Bandplan und Compliance-Gate
 *
 * Die Werte stammen aus der BNetzA-Allgemeinzuteilung fuer den CB-Funk,
 * Vfg. Nr. 21/2021. Diese Tests sind absichtlich streng: bei Vorgaben
 * darf nichts "fast stimmen".
 *
 * SPDX-License-Identifier: MIT
 */
#include "prterm_compat.h"

#include "bands.h"
#include "testutil.h"

#include <string.h>

int main(void)
{
    const pr_bandplan *bp = pr_bandplan_default();

    printf("Bandplan: %s\nQuelle : %s\n\n", bp->name, bp->source);

    printf("== Aufbau ==\n");
    CHECK_INT(bp->nch, 80);
    CHECK_INT(bp->bw_hz, 10000L);

    /* Kanalnummern 1..80, vollstaendig und eindeutig */
    {
        int seen[81];
        memset(seen, 0, sizeof seen);
        int ok = 1;
        for (size_t i = 0; i < bp->nch; i++) {
            int n = bp->ch[i].num;
            if (n < 1 || n > 80 || seen[n]) { ok = 0; break; }
            seen[n] = 1;
        }
        for (int n = 1; n <= 80; n++)
            if (!seen[n]) ok = 0;
        CHECK(ok);
    }

    /* Frequenzen eindeutig */
    {
        int dup = 0;
        for (size_t i = 0; i < bp->nch; i++)
            for (size_t j = i + 1; j < bp->nch; j++)
                if (bp->ch[i].freq_hz == bp->ch[j].freq_hz) dup++;
        CHECK_INT(dup, 0);
    }

    printf("\n== Lage der Bereiche ==\n");
    CHECK_INT(pr_bandplan_channel(bp, 1)->freq_hz,  26965000L);
    CHECK_INT(pr_bandplan_channel(bp, 40)->freq_hz, 27405000L);
    CHECK_INT(pr_bandplan_channel(bp, 41)->freq_hz, 26565000L);
    CHECK_INT(pr_bandplan_channel(bp, 80)->freq_hz, 26955000L);

    printf("\n== Kanaldreher bei 23 ==\n");
    CHECK_INT(pr_bandplan_channel(bp, 22)->freq_hz, 27225000L);
    CHECK_INT(pr_bandplan_channel(bp, 23)->freq_hz, 27255000L);
    CHECK_INT(pr_bandplan_channel(bp, 24)->freq_hz, 27235000L);
    CHECK_INT(pr_bandplan_channel(bp, 25)->freq_hz, 27245000L);
    CHECK_INT(pr_bandplan_channel(bp, 26)->freq_hz, 27265000L);

    printf("\n== Luecken im CEPT-Bereich ==\n");
    CHECK(pr_bandplan_at_freq(bp, 26995000L) == NULL);
    CHECK(pr_bandplan_at_freq(bp, 27045000L) == NULL);
    CHECK(pr_bandplan_at_freq(bp, 27095000L) == NULL);
    CHECK(pr_bandplan_at_freq(bp, 27145000L) == NULL);
    CHECK(pr_bandplan_at_freq(bp, 27195000L) == NULL);

    printf("\n== Betriebsarten ==\n");
    {
        int ok = 1;
        for (int n = 1; n <= 40; n++) {
            const pr_channel *c = pr_bandplan_channel(bp, n);
            if ((c->modes & (PR_BAND_FM | PR_BAND_AM | PR_BAND_SSB))
                    != (PR_BAND_FM | PR_BAND_AM | PR_BAND_SSB)) ok = 0;
        }
        CHECK(ok);          /* K1..40: FM + AM + SSB */
    }
    {
        int ok = 1;
        for (int n = 41; n <= 80; n++)
            if (pr_bandplan_channel(bp, n)->modes != PR_BAND_FM) ok = 0;
        CHECK(ok);          /* K41..80: nur FM */
    }

    printf("\n== Leistungsgrenzen ==\n");
    CHECK_INT(pr_bandplan_max_power_mw(bp, 1,  PR_BAND_FM),   4000L);
    CHECK_INT(pr_bandplan_max_power_mw(bp, 1,  PR_BAND_AM),   4000L);
    CHECK_INT(pr_bandplan_max_power_mw(bp, 1,  PR_BAND_SSB), 12000L);
    CHECK_INT(pr_bandplan_max_power_mw(bp, 80, PR_BAND_FM),   4000L);
    CHECK_INT(pr_bandplan_max_power_mw(bp, 80, PR_BAND_AM),      0L);
    CHECK_INT(pr_bandplan_max_power_mw(bp, 80, PR_BAND_SSB),     0L);

    printf("\n== TX-Gate ==\n");
    {
        char err[256];
        CHECK(pr_bandplan_tx_allowed(bp, 27125000L, PR_BAND_AM, 4000L, err, sizeof err));
        CHECK(!pr_bandplan_tx_allowed(bp, 26955000L, PR_BAND_AM, 4000L, err, sizeof err));
        CHECK(!pr_bandplan_tx_allowed(bp, 27125000L, PR_BAND_FM, 5000L, err, sizeof err));
        CHECK(!pr_bandplan_tx_allowed(bp, 27130000L, PR_BAND_FM, 4000L, err, sizeof err));
        CHECK(pr_bandplan_tx_allowed(bp, 26565000L, PR_BAND_FM, 4000L, err, sizeof err));
    }

    printf("\n== Merkmale ==\n");
    {
        static const int data_ch[] = { 6, 7, 24, 25, 52, 53, 76, 77 };
        static const int gw_ch[]   = { 11, 29, 34, 39, 40, 41, 61, 71, 80 };
        int ok = 1;
        for (size_t i = 0; i < sizeof data_ch / sizeof data_ch[0]; i++)
            if (!(pr_bandplan_channel(bp, data_ch[i])->flags & PR_CH_F_DATA)) ok = 0;
        CHECK(ok);
        ok = 1;
        for (size_t i = 0; i < sizeof gw_ch / sizeof gw_ch[0]; i++)
            if (!(pr_bandplan_channel(bp, gw_ch[i])->flags & PR_CH_F_GATEWAY)) ok = 0;
        CHECK(ok);
    }

    TEST_SUMMARY("bands");
}
