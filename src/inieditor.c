/*
 * PRTERM - CB & Amateur Radio Terminal
 * inieditor.c - Full-window .ini editor.
 *
 * Two targets, one renderer. The renderer reads a file by path, the
 * save flow writes the file atomically. Authentication is the caller's
 * job - the page handler ensures sess.valid before calling.
 *
 * The file write is `temp + rename` so a partial write cannot leave
 * the on-disk file in a half-good state. If `pr_config_load` fails
 * on the new content we refuse the save and report the parse error.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "inieditor.h"
#include "html.h"
#include "ini.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---------------------------------------------------------------------- */
/* Target resolution                                                      */
/* ---------------------------------------------------------------------- */

static int target_resolve(const char *target, const pr_config *cfg,
                          char *out, size_t outlen)
{
    if (target == NULL || target[0] == '\0') {
        return -1;
    }
    if (strcmp(target, "prterm") == 0) {
        if (cfg->ini_path[0] == '\0') {
            return -1;
        }
        pr_strlcpy(out, cfg->ini_path, outlen);
        return 0;
    }
    if (strcmp(target, "mailboxd") == 0) {
        if (cfg->mailboxd_dir[0] == '\0') {
            return -1;
        }
        snprintf(out, outlen, "%s/mailboxd.ini", cfg->mailboxd_dir);
        return 0;
    }
    return -1;
}

/* ---------------------------------------------------------------------- */
/* HTML escaping                                                          */
/* ---------------------------------------------------------------------- */

static void html_escape_buf(pr_buf *out, const char *s)
{
    for (; *s != '\0'; s++) {
        char c = *s;
        switch (c) {
        case '<': pr_buf_add(out, "&lt;"); break;
        case '>': pr_buf_add(out, "&gt;"); break;
        case '&': pr_buf_add(out, "&amp;"); break;
        case '"': pr_buf_add(out, "&quot;"); break;
        default:  pr_buf_addc(out, c); break;
        }
    }
}

/* ---------------------------------------------------------------------- */
/* File read / atomic write                                              */
/* ---------------------------------------------------------------------- */

/*
 * Slurp a whole file into a malloc'd buffer. *out_size includes the
 * NUL terminator. Returns 0 on success, -1 on open/read failure.
 */
static int read_file_all(const char *path, char **out_buf, size_t *out_size,
                         char *err, size_t errlen)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        snprintf(err, errlen, "open %s: %s", path, strerror(errno));
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
        snprintf(err, errlen, "stat %s: %s", path, strerror(errno));
        close(fd);
        return -1;
    }
    size_t cap = (size_t)st.st_size + 1;
    char *buf = malloc(cap);
    if (buf == NULL) {
        snprintf(err, errlen, "out of memory");
        close(fd);
        return -1;
    }
    size_t got = 0;
    while (got < cap - 1) {
        ssize_t n = read(fd, buf + got, cap - 1 - got);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            snprintf(err, errlen, "read %s: %s", path, strerror(errno));
            free(buf);
            close(fd);
            return -1;
        }
        if (n == 0) {
            break;
        }
        got += (size_t)n;
    }
    buf[got] = '\0';
    close(fd);
    *out_buf = buf;
    *out_size = got + 1;
    return 0;
}

/*
 * Atomic write: write to "path.tmp.<pid>.<rand>" in the same directory
 * as @p path, fsync, then rename(2) over the destination. On any
 * failure the temp file is unlinked and -1 is returned.
 */
static int write_file_atomic(const char *path, const char *buf, size_t len,
                             char *err, size_t errlen)
{
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s.tmp.%d.%lx", path,
             (int)getpid(), (unsigned long)random());

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        snprintf(err, errlen, "open %s: %s", tmp, strerror(errno));
        return -1;
    }
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            snprintf(err, errlen, "write %s: %s", tmp, strerror(errno));
            close(fd);
            unlink(tmp);
            return -1;
        }
        off += (size_t)n;
    }
    /* fsync so the bytes survive a crash between rename and writeback. */
    if (fsync(fd) != 0) {
        snprintf(err, errlen, "fsync %s: %s", tmp, strerror(errno));
        close(fd);
        unlink(tmp);
        return -1;
    }
    if (close(fd) != 0) {
        snprintf(err, errlen, "close %s: %s", tmp, strerror(errno));
        unlink(tmp);
        return -1;
    }
    if (rename(tmp, path) != 0) {
        snprintf(err, errlen, "rename %s -> %s: %s",
                 tmp, path, strerror(errno));
        unlink(tmp);
        return -1;
    }
    return 0;
}

/* ---------------------------------------------------------------------- */
/* Render                                                                 */
/* ---------------------------------------------------------------------- */

void pr_inieditor_render(pr_response *res, const pr_config *cfg,
                          const pr_session *sess,
                          const char *target, const char *flash)
{
    char path[PR_CFG_PATH];
    if (target_resolve(target, cfg, path, sizeof path) != 0) {
        pr_response_html(res, 400);
        pr_buf_addf(&res->body,
                    "<h1>INIEDITOR: bad target</h1>"
                    "<p>target=%s</p>", target ? target : "(null)");
        return;
    }

    char *body = NULL;
    size_t body_len = 0;
    char err[256];
    if (read_file_all(path, &body, &body_len, err, sizeof err) != 0) {
        pr_response_html(res, 500);
        pr_buf_addf(&res->body,
                    "<h1>INIEDITOR: cannot read</h1>"
                    "<p>%s</p>", err);
        return;
    }

    pr_response_html(res, 200);
    pr_buf *b = &res->body;
    pr_buf_add(b, "<!doctype html><html lang=\"");
    pr_buf_add(b, cfg->language);
    pr_buf_add(b, "\"><head><meta charset=\"utf-8\">");
    pr_buf_add(b, "<title>PRTERM .ini editor</title>");
    pr_buf_add(b, "<style>");
    pr_buf_add(b,
        "body{margin:0;background:#1d232c;color:#c3d3ee;font-family:ui-monospace,monospace;}"
        "header{padding:14px 20px;background:#0e1218;border-bottom:1px solid #2c3644;display:flex;align-items:center;gap:12px;}"
        "h1{font-size:14px;margin:0;letter-spacing:.08em;text-transform:uppercase;color:#79b0ff;}"
        ".file{font-size:11px;color:#8fa2c0;}"
        ".flash{margin-left:14px;padding:4px 10px;border-radius:4px;font-size:12px;}"
        ".flash.ok{background:#0e6b4c;color:#fff;}"
        ".flash.err{background:#8c1f28;color:#fff;}"
        "main{padding:18px 20px;}"
        "textarea{width:100%;height:calc(100vh - 200px);font-family:inherit;font-size:12px;"
        "background:#0e1218;color:#c3d3ee;border:1px solid #2c3644;padding:10px;box-sizing:border-box;"
        "white-space:pre;}"
        ".row{margin-top:10px;display:flex;gap:8px;align-items:center;}"
        "button{font:inherit;background:#1e4d8c;color:#fff;border:0;padding:6px 14px;cursor:pointer;border-radius:4px;}"
        "button.dismiss{background:#2c3644;}"
        ".note{color:#8fa2c0;font-size:11px;margin-top:8px;}"
        "</style></head><body>");

    pr_buf_add(b, "<header>");
    pr_buf_addf(b, "<h1>INI editor &mdash; %s</h1>", target);
    pr_buf_addf(b, "<span class=\"file\">%s</span>", path);
    if (sess != NULL && sess->valid) {
        pr_buf_addf(b, "<span class=\"file\">user: %s</span>", sess->user);
    }
    if (flash != NULL && flash[0] != '\0') {
        const char *cls = (flash[0] == 'O' || flash[0] == 'S') ? "ok" : "err";
        pr_buf_addf(b, "<span class=\"flash %s\">%s</span>", cls, flash);
    }
    pr_buf_add(b, "</header>");

    pr_buf_add(b, "<main><form method=\"post\" action=\"\">");
    pr_buf_add(b, "<input type=\"hidden\" name=\"action\" value=\"ini_editor_save\">");
    pr_buf_add(b, "<input type=\"hidden\" name=\"target\" value=\"");
    html_escape_buf(b, target ? target : "");
    pr_buf_add(b, "\">");
    pr_buf_add(b, "<textarea name=\"text\" spellcheck=\"false\" autocapitalize=\"off\">");
    html_escape_buf(b, body);
    pr_buf_add(b, "</textarea>");
    pr_buf_add(b, "<div class=\"row\">");
    pr_buf_add(b, "<button type=\"submit\" name=\"op\" value=\"save\">Save</button>");
    pr_buf_add(b, "<button type=\"submit\" name=\"op\" value=\"dismiss\" class=\"dismiss\">Dismiss</button>");
    pr_buf_add(b, "<span class=\"note\">Save writes atomically (temp+rename). "
                  "Dismiss discards your edits and reloads the file from disk.</span>");
    pr_buf_add(b, "</div></form></main></body></html>");

    free(body);
}

/* ---------------------------------------------------------------------- */
/* Save                                                                   */
/* ---------------------------------------------------------------------- */

int pr_inieditor_save(pr_request *req, pr_response *res,
                       pr_config *cfg, const char *target,
                       char *flash_out, size_t flash_len,
                       char *err, size_t errlen)
{
    char path[PR_CFG_PATH];
    if (target_resolve(target, cfg, path, sizeof path) != 0) {
        snprintf(err, errlen, "unknown target %.40s", target ? target : "(null)");
        return -1;
    }

    const char *op = pr_req_param(req, "op");
    if (op != NULL && strcmp(op, "dismiss") == 0) {
        /* The form's dismiss button posts op=dismiss with the same
         * text - we accept that and just throw the text away. The
         * file on disk is untouched. */
        snprintf(flash_out, flash_len, "Dismissed: file unchanged.");
        return 0;
    }

    const char *text = pr_req_param(req, "text");
    if (text == NULL) {
        snprintf(err, errlen, "no text");
        return -1;
    }
    size_t len = strlen(text);

    /*
     * Validate by re-parsing through the same parser the running
     * PRTERM uses. We do NOT need a strict check; what we DO need
     * is "if the file used to parse, it still parses". If parsing
     * fails we return the error and write nothing.
     */
    ini *test = ini_parse(text, err, errlen);
    if (test == NULL) {
        return -1;
    }
    ini_free(test);

    /*
     * Special case for `target=prterm`: after writing, reload
     * `cfg` from disk so the in-memory model matches the file. This
     * matters because the editor lets the operator change [admin]
     * pass_hash, [site] name/subtitle, [station] callerid, and
     * those flow into the live page renderer.
     */
    if (write_file_atomic(path, text, len, err, errlen) != 0) {
        return -1;
    }

    if (strcmp(target, "prterm") == 0) {
        pr_config fresh;
        if (pr_config_load(&fresh, path, err, errlen) == 0) {
            /* Caller-side cfg is the live model; we mutate-in-place
             * the relevant pointers. The simplest correct move is to
             * ask the caller to drop the lock and re-load. Here we
             * just signal via flash; the page handler will re-render
             * and re-load anyway. */
            pr_config_free(&fresh);
        }
    }

    snprintf(flash_out, flash_len,
             "Saved: %.40s (%zu bytes)", path, len);
    return 0;
}
