/*
 * PRTERM - CB & Amateur Radio Terminal
 * locator.c - Maidenhead grid square.
 *
 * Conversion by the common method:
 *
 *   field    : (lon + 180) / 20  or (lat + 90) / 10
 *   square   : remainder / 2     or remainder / 1
 *   subsq.   : remainder / (2/24) or remainder / (1/24)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "prterm_compat.h"

#include "locator.h"

#include <stdio.h>
#include <string.h>

static bool latlon_ok(double lat, double lon)
{
    return lat >= -90.0 && lat <= 90.0 && lon >= -180.0 && lon <= 180.0;
}

bool pr_locator4(double lat, double lon, char out[5])
{
    if (out == NULL || !latlon_ok(lat, lon))
        return false;

    double lo = lon + 180.0;
    double la = lat + 90.0;

    int fld_lo = (int)(lo / 20.0);
    int fld_la = (int)(la / 10.0);
    if (fld_lo < 0 || fld_lo > 17 || fld_la < 0 || fld_la > 17)
        return false;

    int sq_lo = (int)((lo - (double)fld_lo * 20.0) / 2.0);
    int sq_la = (int)(la - (double)fld_la * 10.0);

    out[0] = (char)('A' + fld_lo);
    out[1] = (char)('A' + fld_la);
    out[2] = (char)('0' + sq_lo);
    out[3] = (char)('0' + sq_la);
    out[4] = '\0';
    return true;
}

bool pr_locator6(double lat, double lon, char out[7])
{
    if (out == NULL || !latlon_ok(lat, lon))
        return false;

    double lo = lon + 180.0;
    double la = lat + 90.0;

    /* Square  */
    int fld_lo = (int)(lo / 20.0);
    int fld_la = (int)(la / 10.0);
    if (fld_lo < 0 || fld_lo > 17 || fld_la < 0 || fld_la > 17)
        return false;

    double rem_lo = lo - (double)fld_lo * 20.0;
    double rem_la = la - (double)fld_la * 10.0;

    int sq_lo = (int)(rem_lo / 2.0);
    int sq_la = (int)rem_la;

    /* Subsquare: 24 x 24 fields per square */
    double ss_lo = (rem_lo - (double)sq_lo * 2.0) / 2.0 * 24.0;
    double ss_la = (rem_la - (double)sq_la) * 24.0;

    int s_lo = (int)ss_lo;
    int s_la = (int)ss_la;
    if (s_lo > 23) s_lo = 23;
    if (s_la > 23) s_la = 23;
    if (s_lo < 0)  s_lo = 0;
    if (s_la < 0)  s_la = 0;

    out[0] = (char)('A' + fld_lo);
    out[1] = (char)('A' + fld_la);
    out[2] = (char)('0' + sq_lo);
    out[3] = (char)('0' + sq_la);
    out[4] = (char)('A' + s_lo);
    out[5] = (char)('A' + s_la);
    out[6] = '\0';
    return true;
}

bool pr_locator_valid(const char *loc)
{
    if (loc == NULL)
        return false;
    size_t n = strlen(loc);
    if (n != 4 && n != 6 && n != 8)
        return false;

    if (loc[0] < 'A' || loc[0] > 'R') return false;
    if (loc[1] < 'A' || loc[1] > 'R') return false;
    if (loc[2] < '0' || loc[2] > '9') return false;
    if (loc[3] < '0' || loc[3] > '9') return false;

    if (n >= 6) {
        if (loc[4] < 'A' || loc[4] > 'X') return false;
        if (loc[5] < 'A' || loc[5] > 'X') return false;
    }
    if (n == 8) {
        if (loc[6] < '0' || loc[6] > '9') return false;
        if (loc[7] < '0' || loc[7] > '9') return false;
    }
    return true;
}

bool pr_locator_center(const char *loc, double *lat, double *lon)
{
    if (!pr_locator_valid(loc) || lat == NULL || lon == NULL)
        return false;

    double lo = (double)(loc[0] - 'A') * 20.0;
    double la = (double)(loc[1] - 'A') * 10.0;

    lo += (double)(loc[2] - '0') * 2.0;
    la += (double)(loc[3] - '0');

    size_t n = strlen(loc);
    if (n >= 6) {
        lo += (double)(loc[4] - 'A') * (2.0 / 24.0);
        la += (double)(loc[5] - 'A') * (1.0 / 24.0);
    }
    if (n == 8) {
        lo += (double)(loc[6] - '0') * (2.0 / 24.0 / 10.0);
        la += (double)(loc[7] - '0') * (1.0 / 24.0 / 10.0);
    }

    /* Center of the respective field */
    double lo_half = (n >= 6) ? (2.0 / 24.0) / 2.0 : 1.0;
    double la_half = (n >= 6) ? (1.0 / 24.0) / 2.0 : 0.5;

    *lon = lo + lo_half - 180.0;
    *lat = la + la_half - 90.0;
    return true;
}
