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

#endif /* PRTERM_ADMIN_H */
