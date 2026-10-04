/*
 * PRTERM - CB & Amateur Radio Terminal
 * state.h - Persistenz des Rig-Zustands und des Nachrichtenlogs.
 *
 * CGI-Prozesse leben pro Request. Alles Dauerhafte liegt deshalb als Datei
 * unter [paths] runtime_dir. Mutationen laufen unter einem Advisory-Lock,
 * damit nebeneinander laufende Requests sich nicht ueberschreiben.
 *
 *   prterm.runtime/
 *     state.ini     Rig-Zustand
 *     log           Nachrichten-Log (ringbegrenzt ueber [radio] max_log)
 *     sessions/     Admin-Sessions
 *     lock          Advisory-Lock
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PRTERM_STATE_H
#define PRTERM_STATE_H

#include "config.h"
#include "radio.h"

#include <stdbool.h>
#include <stddef.h>

int  pr_runtime_init(const pr_config *cfg, char *err, size_t errlen);

int  pr_state_load(const pr_config *cfg, pr_rig_state *st,
                   char *err, size_t errlen);
int  pr_state_save(const pr_config *cfg, const pr_rig_state *st,
                   char *err, size_t errlen);

int  pr_log_append(const pr_config *cfg, const pr_msg *m,
                   char *err, size_t errlen);
/* Neueste zuerst? Nein - chronologisch, aelteste zuerst. */
int  pr_log_tail(const pr_config *cfg, pr_msg *out, size_t cap, size_t *n,
                 char *err, size_t errlen);
long pr_log_count(const pr_config *cfg);

/* Advisory-Lock fuer Zustandsmutationen. fd an pr_state_unlock uebergeben. */
int  pr_state_lock(const pr_config *cfg, char *err, size_t errlen);
void pr_state_unlock(int fd);

/* Pfade */
void pr_state_path(const pr_config *cfg, char *dst, size_t dstlen);
void pr_log_path(const pr_config *cfg, char *dst, size_t dstlen);
void pr_session_dir(const pr_config *cfg, char *dst, size_t dstlen);
void pr_lock_path(const pr_config *cfg, char *dst, size_t dstlen);

#endif /* PRTERM_STATE_H */
