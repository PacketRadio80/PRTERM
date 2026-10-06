/*
 * PRTERM - CB & Amateur Radio Terminal
 * pages.h - Page composition and request handling.
 *
 * PRTERM has exactly one URL (see docs/ROUTING.md). pr_handle() decides
 * via the action parameter whether to render, deliver JSON or change a
 * configuration.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PRTERM_PAGES_H
#define PRTERM_PAGES_H

#include "cgi.h"
#include "config.h"
#include "radio.h"
#include "session.h"

/* Handles a request completely.         */
int pr_handle(pr_request *req, pr_response *res, pr_config *cfg);

/* Pages (individually callable for tests) */
/* Emits the CSRF token as a <meta> tag so the JavaScript can send
 * it along. */
void page_csrf_meta(pr_buf *out, const pr_session *sess);

void page_render(pr_buf *out, const pr_config *cfg, const pr_session *sess,
                 const pr_rig_state *st, const pr_msg *msgs, size_t nmsg,
                 const char *flash_kind, const char *flash_msg);

#endif /* PRTERM_PAGES_H */
