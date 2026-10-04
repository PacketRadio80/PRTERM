/*
 * PRTERM - CB & Amateur Radio Terminal
 * pages.h - Seitenkomposition und Request-Handling.
 *
 * PRTERM hat genau eine URL (siehe docs/ROUTING.md). pr_handle() entscheidet
 * ueber den action-Parameter, ob gerendert, JSON geliefert oder eine
 * Konfiguration geaendert wird.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PRTERM_PAGES_H
#define PRTERM_PAGES_H

#include "cgi.h"
#include "config.h"
#include "radio.h"
#include "session.h"

/* Bearbeitet eine Anfrage vollstaendig. */
int pr_handle(pr_request *req, pr_response *res, pr_config *cfg);

/* Seiten (fuer Tests einzeln aufrufbar) */
void page_render(pr_buf *out, const pr_config *cfg, const pr_session *sess,
                 const pr_rig_state *st, const pr_msg *msgs, size_t nmsg,
                 const char *flash_kind, const char *flash_msg);

#endif /* PRTERM_PAGES_H */
