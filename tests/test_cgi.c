/*
 * PRTERM - Test: CGI and escaping
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "prterm_compat.h"

#include "cgi.h"
#include "html.h"
#include "session.h"
#include "testutil.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

int main(void)
{
    printf("== URL-Kodierung ==\n");
    {
        pr_buf b;
        pr_buf_init(&b);
        pr_url_encode(&b, "DL1ABC-1 & Co");
        CHECK_STR(b.data, "DL1ABC-1+%26+Co");
        pr_buf_free(&b);
    }
    {
        char s[] = "DL1ABC-1+%26+Co";
        pr_url_decode(s);
        CHECK_STR(s, "DL1ABC-1 & Co");
    }
    {
        char s[] = "wert=%C3%A4";
        pr_url_decode(s);
        CHECK_STR(s, "wert=\xc3\xa4");
    }

    printf("\n== HTML-Escaping ==\n");
    {
        pr_buf b;
        pr_buf_init(&b);
        pr_html_escape(&b, "<script>alert(\"x\")</script>");
        CHECK_STR(b.data, "&lt;script&gt;alert(&quot;x&quot;)&lt;/script&gt;");
        pr_buf_free(&b);
    }
    {
        pr_buf b;
        pr_buf_init(&b);
        pr_attr_escape(&b, "a\"b'c`d\ne");
        CHECK(strchr(b.data, '"') == NULL);
        CHECK(strchr(b.data, '\'') == NULL);
        CHECK(strchr(b.data, '`') == NULL);
        CHECK(strchr(b.data, '\n') == NULL);
        pr_buf_free(&b);
    }

    printf("\n== JSON-Escaping ==\n");
    {
        pr_buf b;
        pr_buf_init(&b);
        pr_json_escape(&b, "zeile1\nzeile2\"mit\" \\slash\\");
        CHECK_STR(b.data, "zeile1\\nzeile2\\\"mit\\\" \\\\slash\\\\");
        pr_buf_free(&b);
    }
    {
        pr_buf b;
        pr_buf_init(&b);
        pr_json_escape(&b, "\x01");
        CHECK_STR(b.data, "\\u0001");
        pr_buf_free(&b);
    }

    printf("\n== Cookies ==\n");
    {
        pr_request req;
        memset(&req, 0, sizeof req);
        pr_strlcpy(req.cookie, "a=1; PRTERM_SID=abc123; b=2", sizeof req.cookie);

        char v[64];
        CHECK(pr_req_cookie(&req, "PRTERM_SID", v, sizeof v));
        CHECK_STR(v, "abc123");
        CHECK(pr_req_cookie(&req, "a", v, sizeof v));
        CHECK_STR(v, "1");
        CHECK(!pr_req_cookie(&req, "fehlt", v, sizeof v));
    }

    printf("\n== Schrift-MIME ==\n");
    {
        char mime[32];
        CHECK(html_font_mime("fonts/x.ttf", mime, sizeof mime));
        CHECK_STR(mime, "font/ttf");
        CHECK(html_font_mime("fonts/x.otf", mime, sizeof mime));
        CHECK_STR(mime, "font/otf");
        CHECK(html_font_mime("fonts/x.woff2", mime, sizeof mime));
        CHECK_STR(mime, "font/woff2");
        CHECK(!html_font_mime("fonts/x.exe", mime, sizeof mime));
        CHECK(!html_font_mime("", mime, sizeof mime));
        CHECK(!html_font_mime(NULL, mime, sizeof mime));
    }

    printf("\n== Wildcards ==\n");
    CHECK(pr_wildcard_match("*", "was auch immer"));
    CHECK(pr_wildcard_match("abc*", "abcdef"));
    CHECK(!pr_wildcard_match("abc*", "xabc"));
    CHECK(pr_wildcard_match("a?c", "abc"));
    CHECK(!pr_wildcard_match("a?c", "ac"));

    printf("\n== Puffer ==\n");
    {
        pr_buf b;
        pr_buf_init(&b);
        for (int i = 0; i < 1000; i++)
            pr_buf_addf(&b, "zeile %d\n", i);
        CHECK(b.len > 8000);
        CHECK(b.data != NULL && b.data[b.len] == '\0');
        pr_buf_reset(&b);
        CHECK_INT(b.len, 0);
        pr_buf_free(&b);
    }

    printf("\n== Passwort-Hash ==\n");
    {
        char hash[160];
        CHECK_INT(pr_hash_password("geheim123", hash, sizeof hash), 0);
        CHECK(pr_starts_with(hash, "sha256$"));
        CHECK(pr_verify_password("geheim123", hash));
        CHECK(!pr_verify_password("falsch", hash));
        CHECK(!pr_verify_password("", hash));
        CHECK(!pr_verify_password("geheim123", ""));
    }

    TEST_SUMMARY("cgi");
}
