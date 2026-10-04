/*
 * PRTERM - CB & Amateur Radio Terminal
 * admin.h - Admin-Aktionen.
 *
 * Der Administrationsbereich hat keine eigene URL; diese Funktion wertet
 * die Aktionen derselben Adresse aus (siehe docs/ROUTING.md).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PRTERM_ADMIN_H
#define PRTERM_ADMIN_H

#include "cgi.h"
#include "config.h"
#include "session.h"

/* Bearbeitet eine Admin-Aktion und setzt die Antwort auf JSON. */
int pr_admin_action(pr_request *req, pr_response *res,
                    pr_config *cfg, pr_session *sess);

#endif /* PRTERM_ADMIN_H */
