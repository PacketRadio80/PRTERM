/*
 * PRTERM - CB & Amateur Radio Terminal
 * inieditor.h - Full-window .ini editor.
 *
 * Two targets are supported: `target=prterm` reads/writes the
 * prterm.ini the running CGI was started with; `target=mailboxd`
 * reads/writes the MailboxD INI at `<mailboxd_dir>/mailboxd.ini`.
 *
 * Read flow (GET action=ini_editor&target=...):
 *   - load file into a textarea
 *   - "Save" / "Dismiss" / "Editor hint" buttons
 *   - the page is its own view, full-window, no other PRTERM chrome
 *
 * Write flow (POST action=ini_editor_save&target=...):
 *   - validate: file must still parse with the same parser
 *   - atomic write: temp file + rename
 *   - redirect back to the editor with a flash message
 *
 * The editor never edits the [admin] pass_hash or the [mailboxd]
 * user/group without intent - PRTERM keeps the running `cfg` in sync
 * with what was just written, so a subsequent re-render sees the
 * new values.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PRTERM_INIEDITOR_H
#define PRTERM_INIEDITOR_H

#include "config.h"
#include "cgi.h"
#include "session.h"

/* Render the editor page. Sends headers + HTML on @p res. */
void pr_inieditor_render(pr_response *res, const pr_config *cfg,
                          const pr_session *sess,
                          const char *target, const char *flash);

/* Apply a save. Returns 0 on success, -1 on failure. On success,
 * @p flash_out is filled with a short success message. The caller's
 * loaded `cfg` is NOT touched here - the page handler re-loads it
 * from disk so the in-memory model and the file agree. */
int  pr_inieditor_save(pr_request *req, pr_response *res,
                       pr_config *cfg, const char *target,
                       char *flash_out, size_t flash_len,
                       char *err, size_t errlen);

#endif /* PRTERM_INIEDITOR_H */
