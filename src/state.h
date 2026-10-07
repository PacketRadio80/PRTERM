/*
 * PRTERM - CB & Amateur Radio Terminal
 * state.h - Persistence of the rig state and the message log.
 *
 * CGI processes live per request. Everything persistent is therefore a
 * file under [paths] runtime_dir. Mutations run under an advisory lock
 * so that parallel requests do not overwrite each other.
 *
 *   prterm.runtime/
 *     state.ini     rig state
 *     log           message log (ring-limited via [radio] max_log)
 *     sessions/     admin sessions
 *     lock          advisory lock
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
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
/* Newest first? No - chronological, oldest first.        */
int  pr_log_tail(const pr_config *cfg, pr_msg *out, size_t cap, size_t *n,
                 char *err, size_t errlen);
long pr_log_count(const pr_config *cfg);

/* Advisory lock for state mutations. Pass fd to pr_state_unlock.
 * pr_state_save() and pr_log_append() take this lock THEMSELVES - do
 * not hold it around those calls. */
int  pr_state_lock(const pr_config *cfg, char *err, size_t errlen);
void pr_state_unlock(int fd);

/* Paths */
void pr_state_path(const pr_config *cfg, char *dst, size_t dstlen);
void pr_log_path(const pr_config *cfg, char *dst, size_t dstlen);
void pr_session_dir(const pr_config *cfg, char *dst, size_t dstlen);
void pr_lock_path(const pr_config *cfg, char *dst, size_t dstlen);

#endif /* PRTERM_STATE_H */
