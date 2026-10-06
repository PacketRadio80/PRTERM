/*
 * PRTERM - CB & Amateur Radio Terminal
 * html.c - Layout, embedded assets, form building blocks.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "prterm_compat.h"

#include "html.h"
#include "asset.h"
#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

const char *html_css(void)
{
    return (const char *)prterm_css;
}

const char *html_js(void)
{
    return (const char *)prterm_js;
}

/* ======================================================================= */
/* Font file                                                               */
/* ======================================================================= */

bool html_font_mime(const char *path, char *mime, size_t mimelen)
{
    if (path == NULL || path[0] == '\0')
        return false;

    if (pr_ends_with(path, ".ttf")) {
        pr_strlcpy(mime, "font/ttf", mimelen);
        return true;
    }
    if (pr_ends_with(path, ".otf")) {
        pr_strlcpy(mime, "font/otf", mimelen);
        return true;
    }
    if (pr_ends_with(path, ".woff2")) {
        pr_strlcpy(mime, "font/woff2", mimelen);
        return true;
    }
    if (pr_ends_with(path, ".woff")) {
        pr_strlcpy(mime, "font/woff", mimelen);
        return true;
    }
    return false;
}

/* format() argument for @font-face per extension. */
static const char *font_format(const char *path)
{
    if (pr_ends_with(path, ".woff2")) return "woff2";
    if (pr_ends_with(path, ".woff"))  return "woff";
    if (pr_ends_with(path, ".otf"))   return "opentype";
    return "truetype";
}

/* ======================================================================= */
/* Document head                                                           */
/* ======================================================================= */

void html_open(pr_buf *out, const pr_config *cfg, const pr_session *sess,
               const char *title)
{
    (void)sess;

    pr_buf_add(out,
        "<!DOCTYPE html>\n"
        "<html lang=\"en\" data-theme=\"");
    pr_buf_add(out, pr_str_eq_ci(cfg->ui_theme, "dark") ? "dark" : "silver");
    pr_buf_add(out, "\" data-density=\"");
    pr_buf_add(out, pr_str_eq_ci(cfg->ui_density, "compact") ? "compact" : "normal");
    pr_buf_add(out, "\">\n<head>\n"
        "<meta charset=\"utf-8\">\n"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1,"
        "viewport-fit=cover\">\n"
        "<meta name=\"color-scheme\" content=\"light dark\">\n"
        "<meta name=\"referrer\" content=\"no-referrer\">\n"
        "<title>");

    pr_html_escape(out, title != NULL && title[0] != '\0' ? title : cfg->site_name);
    pr_buf_add(out, "</title>\n");

    /* User font - only if a file is configured and permitted.
     * Relative query link: resolves against the current URL so that
     * PRTERM can be mounted under any path. */
    char mime[32];
    if (html_font_mime(cfg->font_file, mime, sizeof mime)) {
        pr_buf_add(out, "<style>\n@font-face{font-family:\"PRTERM\";");
        pr_buf_add(out, "src:url(\"?action=font\") format(\"");
        pr_buf_add(out, font_format(cfg->font_file));
        pr_buf_add(out, "\");font-display:swap;}\n</style>\n");
    }

    /* Embedded stylesheet      */
    pr_buf_add(out, "<style>\n");
    pr_buf_add(out, html_css());
    pr_buf_add(out, "\n</style>\n");

    /* Dynamic variables from [ui]   */
    pr_buf_add(out, "<style>\n:root{");
    if (cfg->font_file[0] != '\0')
        pr_buf_add(out, "--pr-font:\"PRTERM\",");
    pr_buf_add(out, "ui-monospace,\"DejaVu Sans Mono\",\"Liberation Mono\","
                    "Consolas,monospace;");
    pr_buf_addf(out, "--pr-font-size:%dpx;", cfg->font_size);
    pr_buf_addf(out, "--pr-line-height:%.2f;", cfg->line_height_pct / 100.0);
    pr_buf_add(out, "}\n</style>\n</head>\n<body>\n");
}

void html_close(pr_buf *out, const pr_config *cfg)
{
    (void)cfg;
    pr_buf_add(out, "\n<script>\n");
    pr_buf_add(out, html_js());
    pr_buf_add(out, "\n</script>\n</body>\n</html>\n");
}

/* ======================================================================= */
/* Form building blocks                                                    */
/* ======================================================================= */

static void field_label(pr_buf *out, const char *label, const char *hint)
{
    if (label == NULL)
        return;
    pr_buf_add(out, "<label>");
    pr_html_escape(out, label);
    pr_buf_add(out, "</label>");
    if (hint != NULL && hint[0] != '\0') {
        pr_buf_add(out, "<span class=\"hint\">");
        pr_html_escape(out, hint);
        pr_buf_add(out, "</span>");
    }
}

void html_input_text(pr_buf *out, const char *name, const char *value,
                     const char *placeholder, const char *label,
                     const char *hint)
{
    pr_buf_add(out, "<div class=\"field\">");
    field_label(out, label, hint);
    pr_buf_addf(out, "<input type=\"text\" name=\"%s\" value=\"", name);
    pr_attr_escape(out, value != NULL ? value : "");
    pr_buf_add(out, "\"");
    if (placeholder != NULL && placeholder[0] != '\0') {
        pr_buf_add(out, " placeholder=\"");
        pr_attr_escape(out, placeholder);
        pr_buf_add(out, "\"");
    }
    pr_buf_add(out, " autocomplete=\"off\" spellcheck=\"false\">");
    pr_buf_add(out, "</div>\n");
}

void html_input_number(pr_buf *out, const char *name, long value,
                       long lo, long hi, const char *label, const char *hint)
{
    pr_buf_add(out, "<div class=\"field\">");
    field_label(out, label, hint);
    pr_buf_addf(out,
        "<input type=\"number\" name=\"%s\" value=\"%ld\" min=\"%ld\" max=\"%ld\">",
        name, value, lo, hi);
    pr_buf_add(out, "</div>\n");
}

void html_input_hidden(pr_buf *out, const char *name, const char *value)
{
    pr_buf_addf(out, "<input type=\"hidden\" name=\"%s\" value=\"", name);
    pr_attr_escape(out, value != NULL ? value : "");
    pr_buf_add(out, "\">\n");
}

void html_select(pr_buf *out, const char *name,
                 const char *const *values, const char *const *labels,
                 size_t n, const char *selected,
                 const char *label, const char *hint)
{
    pr_buf_add(out, "<div class=\"field\">");
    field_label(out, label, hint);
    pr_buf_addf(out, "<select name=\"%s\">", name);
    for (size_t i = 0; i < n; i++) {
        bool sel = (selected != NULL) && pr_str_eq_ci(values[i], selected);
        pr_buf_addf(out, "<option value=\"%s\"%s>", values[i], sel ? " selected" : "");
        pr_html_escape(out, labels[i]);
        pr_buf_add(out, "</option>");
    }
    pr_buf_add(out, "</select></div>\n");
}

void html_checkbox(pr_buf *out, const char *name, bool checked,
                   const char *label, const char *hint)
{
    pr_buf_add(out, "<div class=\"field\"><label style=\"flex-direction:row;"
                    "align-items:center;gap:7px;text-transform:none\">");
    pr_buf_addf(out, "<input type=\"checkbox\" name=\"%s\" value=\"1\"%s>",
                name, checked ? " checked" : "");
    pr_html_escape(out, label != NULL ? label : name);
    pr_buf_add(out, "</label>");
    if (hint != NULL && hint[0] != '\0') {
        pr_buf_add(out, "<span class=\"hint\">");
        pr_html_escape(out, hint);
        pr_buf_add(out, "</span>");
    }
    pr_buf_add(out, "</div>\n");
}

void html_csrf(pr_buf *out, const pr_session *sess)
{
    if (sess != NULL && sess->valid)
        html_input_hidden(out, "csrf", sess->csrf);
}

void html_note(pr_buf *out, const char *kind, const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    pr_buf_addf(out, "<div class=\"note note-%s\">", kind != NULL ? kind : "info");
    pr_html_escape(out, msg);
    pr_buf_add(out, "</div>\n");
}
