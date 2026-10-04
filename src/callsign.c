/*
 * PRTERM - CB & Amateur Radio Terminal
 * callsign.c - CALLID/CALLERID-Validierung und Ban-Matching.
 *
 * SPDX-License-Identifier: MIT
 */
#include "prterm_compat.h"

#include "callsign.h"
#include "util.h"

#include <stdio.h>
#include <string.h>

void pr_call_rules_default(pr_call_rules *r)
{
    r->callid_max_len     = 6;
    r->callerid_base_len  = 6;
    r->callerid_max_total = 8;
    r->allow_ssid         = true;
    r->ssid_digits        = 1;
}

/* ======================================================================= */
/* Zeichenklassen                                                          */
/* ======================================================================= */

static bool is_call_char(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

static bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

/* Prueft 1..maxlen Zeichen [A-Z0-9] in einem String ohne Suffix. */
static bool body_ok(const char *s, int maxlen)
{
    if (s == NULL || s[0] == '\0')
        return false;
    int n = 0;
    for (const char *p = s; *p != '\0'; p++, n++) {
        if (n >= maxlen)
            return false;
        if (!is_call_char(*p))
            return false;
    }
    return n >= 1;
}

/* ======================================================================= */
/* CALLID - ohne Suffix                                                    */
/* ======================================================================= */

bool callid_valid(const char *s, const pr_call_rules *r)
{
    pr_call_rules def;
    if (r == NULL) {
        pr_call_rules_default(&def);
        r = &def;
    }
    return body_ok(s, r->callid_max_len);
}

bool callid_normalize(char *dst, size_t dstlen, const char *src,
                      const pr_call_rules *r)
{
    if (dst == NULL || dstlen == 0)
        return false;
    dst[0] = '\0';
    if (src == NULL)
        return false;

    /* Blanks/Zeilenumbrueche raus, gross schreiben */
    size_t w = 0;
    for (const char *p = src; *p != '\0' && w + 1 < dstlen; p++) {
        char c = *p;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
            continue;
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
        dst[w++] = c;
    }
    dst[w] = '\0';

    return callid_valid(dst, r);
}

/* ======================================================================= */
/* CALLERID - mit optionaler SSID                                          */
/* ======================================================================= */

bool callerid_split(const char *s, char *base, size_t baselen, int *ssid)
{
    if (base != NULL && baselen > 0)
        base[0] = '\0';
    if (ssid != NULL)
        *ssid = -1;
    if (s == NULL)
        return false;

    const char *dash = strchr(s, '-');
    if (dash == NULL) {
        if (base != NULL)
            pr_strlcpy(base, s, baselen);
        return true;
    }

    size_t n = (size_t)(dash - s);
    if (base != NULL) {
        if (n >= baselen)
            n = baselen - 1;
        memcpy(base, s, n);
        base[n] = '\0';
    }
    if (ssid != NULL) {
        int v = 0;
        const char *p = dash + 1;
        if (*p == '\0')
            return false;               /* "-" ohne Ziffer ist ungueltig */
        for (; *p != '\0'; p++) {
            if (!is_digit(*p))
                return false;
            v = v * 10 + (*p - '0');
        }
        *ssid = v;
    }
    return true;
}

bool callerid_valid(const char *s, const pr_call_rules *r)
{
    pr_call_rules def;
    if (r == NULL) {
        pr_call_rules_default(&def);
        r = &def;
    }
    if (s == NULL || s[0] == '\0')
        return false;

    size_t total = strlen(s);
    if ((int)total > r->callerid_max_total)
        return false;

    char base[PR_CALLSIGN_MAX];
    int ssid = -1;
    if (!callerid_split(s, base, sizeof base, &ssid))
        return false;

    if (!body_ok(base, r->callerid_base_len))
        return false;

    if (ssid < 0)
        return true;                    /* ohne SSID immer ok            */

    if (!r->allow_ssid)
        return false;

    /* SSID-Digitzahl pruefen */
    int digits = 0;
    const char *dash = strchr(s, '-');
    for (const char *p = dash + 1; *p != '\0'; p++)
        digits++;
    return digits >= 1 && digits <= r->ssid_digits;
}

bool callerid_normalize(char *dst, size_t dstlen, const char *src,
                        const pr_call_rules *r)
{
    if (dst == NULL || dstlen == 0)
        return false;
    dst[0] = '\0';
    if (src == NULL)
        return false;

    size_t w = 0;
    for (const char *p = src; *p != '\0' && w + 1 < dstlen; p++) {
        char c = *p;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
            continue;
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
        dst[w++] = c;
    }
    dst[w] = '\0';

    return callerid_valid(dst, r);
}

bool anyid_valid(const char *s, const pr_call_rules *r)
{
    return callid_valid(s, r) || callerid_valid(s, r);
}

/* ======================================================================= */
/* Muster                                                                  */
/* ======================================================================= */

bool call_pattern_match(const char *pattern, const char *text)
{
    if (pattern == NULL || text == NULL)
        return false;

    /* Grossschreibungs-Vergleich: Rufzeichen sind case-insensitiv. */
    const char *p = pattern, *t = text;
    const char *star = NULL, *star_t = NULL;

    while (*t != '\0') {
        char pc = *p, tc = *t;
        if (pc >= 'a' && pc <= 'z') pc = (char)(pc - 'a' + 'A');
        if (tc >= 'a' && tc <= 'z') tc = (char)(tc - 'a' + 'A');

        if (pc == '?') {
            p++;
            t++;
        } else if (pc == '*') {
            star = p++;
            star_t = t;
        } else if (pc == tc) {
            p++;
            t++;
        } else if (star != NULL) {
            p = star + 1;
            t = ++star_t;
        } else {
            return false;
        }
    }
    while (*p == '*')
        p++;
    return *p == '\0';
}

const char *call_pattern_list_match(const char *const *patterns, size_t npatterns,
                                    const char *id)
{
    if (patterns == NULL || id == NULL)
        return NULL;
    for (size_t i = 0; i < npatterns; i++) {
        if (patterns[i] == NULL)
            continue;
        if (call_pattern_match(patterns[i], id))
            return patterns[i];
    }
    return NULL;
}

/* ======================================================================= */
/* AX.25-Wire-Encoding                                                     */
/* ======================================================================= */

/*
 * Adressformat: 6 Byte Call (blank-gefuellt), jedes Byte << 1,
 * danach 1 Byte SSID: ((ssid & 0x0F) << 1) | 0x60,
 * letztes Adressbyte bekommt zusaetzlich 0x01 (Endekennung).
 */
bool call_to_ax25(const char *id, unsigned char dst[7])
{
    if (id == NULL || dst == NULL)
        return false;

    char base[8];
    int ssid = 0;
    if (!callerid_split(id, base, sizeof base, &ssid))
        return false;
    if (ssid < 0)
        ssid = 0;
    if (ssid > 15)
        return false;

    if (!body_ok(base, 6))
        return false;

    for (int i = 0; i < 6; i++) {
        char c = (i < (int)strlen(base)) ? base[i] : ' ';
        dst[i] = (unsigned char)((unsigned char)c << 1);
    }
    dst[6] = (unsigned char)(((unsigned)ssid << 1) | 0x60u);
    return true;
}

bool call_from_ax25(const unsigned char src[7], char *dst, size_t dstlen)
{
    if (src == NULL || dst == NULL || dstlen == 0)
        return false;

    char body[8];
    for (int i = 0; i < 6; i++)
        body[i] = (char)(src[i] >> 1);
    body[6] = '\0';

    /* Blanks rechts entfernen */
    int n = 6;
    while (n > 0 && body[n - 1] == ' ')
        n--;
    body[n] = '\0';

    int ssid = (src[6] >> 1) & 0x0f;

    if (ssid != 0)
        snprintf(dst, dstlen, "%s-%d", body, ssid);
    else
        snprintf(dst, dstlen, "%s", body);
    return true;
}

/* ======================================================================= */
/* AX.25-UI-Rahmen                                                         */
/* ======================================================================= */

/*
 * Control 0x03 = UI, PID 0xF0 = ohne Unterprotokoll.
 * Das Endekennung-Bit (0x01) wird an der Quelladresse gesetzt.
 */
#define AX25_CTRL_UI 0x03u
#define AX25_PID_NONE 0xF0u

size_t ax25_ui_frame(unsigned char *out, size_t outcap,
                     const char *from, const char *to,
                     const unsigned char *info, size_t infolen)
{
    if (out == NULL)
        return 0;

    unsigned char addr[7];
    if (outcap < 7 + 7 + 2 + infolen)
        return 0;

    /* Zieladresse - ohne Endekennung */
    if (!call_to_ax25(to, addr))
        return 0;
    memcpy(out, addr, 7);

    /* Quelladresse - MIT Endekennung */
    if (!call_to_ax25(from, addr))
        return 0;
    addr[6] |= 0x01u;
    memcpy(out + 7, addr, 7);

    out[14] = AX25_CTRL_UI;
    out[15] = AX25_PID_NONE;

    if (infolen > 0 && info != NULL)
        memcpy(out + 16, info, infolen);

    return 16 + infolen;
}
