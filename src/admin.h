/*
 * PRTERM - CB & Amateur Radio Terminal
 * admin.h - Admin actions.
 *
 * The administration area has no own URL; this function evaluates the
 * actions of the same address (see docs/ROUTING.md).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PRTERM_ADMIN_H
#define PRTERM_ADMIN_H

#include "cgi.h"
#include "config.h"
#include "session.h"

/* Handles an admin action and sets the response to JSON.       */
int pr_admin_action(pr_request *req, pr_response *res,
                    pr_config *cfg, pr_session *sess);

/* --- MailboxD bridge actions (public, no admin session required) --- */

/* Probe whether the MailboxD bridge socket responds to HELLO. Returns
 * 1 if linked (OK MAILBOXD), 0 if not reachable.  Safe for polling. */
int pr_mbox_probe_linked(const pr_config *cfg);

/* POST action=mbox_login: silent /login over the bridge, sets
 * MBOX_U + MBOX_P cookies on success. */
void pr_mbox_login(pr_request *req, pr_response *res, pr_config *cfg);

/* POST action=mbox_run: silent re-login (if cookies), then RUN <cmd>.*/
void pr_mbox_run(pr_request *req, pr_response *res, pr_config *cfg);

/* POST action=mbox_logout: clears MBOX_U + MBOX_P cookies. */
void pr_mbox_logout(pr_request *req, pr_response *res);

#endif /* PRTERM_ADMIN_H */
