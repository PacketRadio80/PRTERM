/*
 * PRTERM - CB & Amateur Radio Terminal
 * radio.c - Driver-Registry und Komfort-Dispatch.
 *
 * SPDX-License-Identifier: MIT
 */
#include "prterm_compat.h"

#include "radio.h"
#include "util.h"

#include <string.h>

static const pr_rig_vtbl *const drivers[] = {
#if PRTERM_RIG_SIM
    &pr_rig_sim,
#endif
#if PRTERM_RIG_TNC2
    &pr_rig_tnc2,
#endif
#if PRTERM_RIG_TMODEM
    &pr_rig_tmodem,
#endif
#if PRTERM_RIG_MAX25
    &pr_rig_max25,
#endif
};

#define DRIVER_COUNT (sizeof drivers / sizeof drivers[0])

size_t pr_rig_count(void)
{
    return DRIVER_COUNT;
}

const pr_rig_vtbl *pr_rig_at(size_t idx)
{
    return idx < DRIVER_COUNT ? drivers[idx] : NULL;
}

const pr_rig_vtbl *pr_rig_find(const char *name)
{
    if (name == NULL)
        return NULL;
    for (size_t i = 0; i < DRIVER_COUNT; i++) {
        if (pr_str_eq_ci(drivers[i]->name, name))
            return drivers[i];
    }
    return NULL;
}

int pr_rig_open(pr_rig *r, const pr_config *cfg, char *err, size_t errlen)
{
    memset(r, 0, sizeof *r);
    r->cfg = cfg;

    const pr_rig_vtbl *v = pr_rig_find(cfg->rig_driver);
    if (v == NULL) {
        snprintf(err, errlen,
                 "unbekannter Rig-Treiber \"%s\" - verfuegbar sind %d Treiber",
                 cfg->rig_driver, (int)DRIVER_COUNT);
        return -1;
    }
    r->vtbl = v;
    return v->open(r, err, errlen);
}

void pr_rig_close(pr_rig *r)
{
    if (r == NULL || r->vtbl == NULL || r->vtbl->close == NULL)
        return;
    r->vtbl->close(r);
    r->vtbl = NULL;
    r->impl = NULL;
}
