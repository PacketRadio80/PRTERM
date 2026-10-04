/*
 * PRTERM - CB & Amateur Radio Terminal
 * cgi.c - Request/Response nach RFC 3875.
 *
 * SPDX-License-Identifier: MIT
 */
#include "prterm_compat.h"

#include "cgi.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ======================================================================= */
/* Umgebung                                                                */
/* ======================================================================= */

bool pr_is_cgi(void)
{
    return getenv("GATEWAY_INTERFACE") != NULL;
}

const char *pr_cgi_env(const char *name, const char *dflt)
{
    const char *v = getenv(name);
    return v != NULL ? v : dflt;
}

/* ======================================================================= */
/* Query-Parameter                                                         */
/* ======================================================================= */

static bool query_reserve(pr_query *q, size_t need)
{
    if (q->n + need <= q->cap)
        return true;
    size_t ncap = q->cap != 0 ? q->cap : 8;
    while (ncap < q->n + need)
        ncap *= 2;
    pr_pair *p = realloc(q->items, ncap * sizeof *p);
    if (p == NULL)
        return false;
    q->items = p;
    q->cap = ncap;
    return true;
}

static void query_free(pr_query *q)
{
    for (size_t i = 0; i < q->n; i++) {
        free(q->items[i].key);
        free(q->items[i].value);
    }
    free(q->items);
    q->items = NULL;
    q->n = q->cap = 0;
}

/* Zerlegt "a=1&b=2" - verwurstet den String. */
static void query_parse(pr_query *q, char *s)
{
    char *p = s;
    while (p != NULL && *p != '\0') {
        char *amp = strchr(p, '&');
        if (amp != NULL)
            *amp = '\0';

        char *eq = strchr(p, '=');
        char *k = p;
        char *v;
        if (eq != NULL) {
            *eq = '\0';
            v = eq + 1;
        } else {
            v = (char *)"";
        }

        pr_url_decode(k);
        pr_url_decode(v);
        pr_trim(k);

        if (*k != '\0' && query_reserve(q, 1)) {
            q->items[q->n].key   = pr_strdup(k);
            q->items[q->n].value = pr_strdup(v);
            if (q->items[q->n].key != NULL && q->items[q->n].value != NULL)
                q->n++;
            else {
                free(q->items[q->n].key);
                free(q->items[q->n].value);
            }
        }
        p = (amp != NULL) ? amp + 1 : NULL;
    }
}

static const char *query_get(const pr_query *q, const char *key)
{
    for (size_t i = 0; i < q->n; i++)
        if (pr_str_eq_ci(q->items[i].key, key))
            return q->items[i].value;
    return NULL;
}

const char *pr_req_get(const pr_request *req, const char *key)
{
    return query_get(&req->query, key);
}

const char *pr_req_post(const pr_request *req, const char *key)
{
    return query_get(&req->form, key);
}

const char *pr_req_param(const pr_request *req, const char *key)
{
    const char *v = query_get(&req->form, key);
    return v != NULL ? v : query_get(&req->query, key);
}

/* ======================================================================= */
/* Cookies                                                                 */
/* ======================================================================= */

bool pr_req_cookie(const pr_request *req, const char *name,
                   char *dst, size_t dstlen)
{
    if (dstlen == 0)
        return false;
    dst[0] = '\0';

    const char *p = req->cookie;
    size_t nlen = strlen(name);

    while (*p != '\0') {
        while (*p == ' ' || *p == ';')
            p++;
        if (strncmp(p, name, nlen) == 0 && p[nlen] == '=') {
            p += nlen + 1;
            size_t w = 0;
            while (*p != '\0' && *p != ';' && w + 1 < dstlen)
                dst[w++] = *p++;
            dst[w] = '\0';
            pr_trim(dst);
            return true;
        }
        while (*p != '\0' && *p != ';')
            p++;
    }
    return false;
}

/* ======================================================================= */
/* Request lesen                                                           */
/* ======================================================================= */

int pr_request_parse(pr_request *req, char *err, size_t errlen)
{
    memset(req, 0, sizeof *req);

    pr_strlcpy(req->method,       pr_cgi_env("REQUEST_METHOD", "GET"), sizeof req->method);
    pr_strlcpy(req->script_name,  pr_cgi_env("SCRIPT_NAME", ""),       sizeof req->script_name);
    pr_strlcpy(req->path_info,    pr_cgi_env("PATH_INFO", ""),         sizeof req->path_info);
    pr_strlcpy(req->query_string, pr_cgi_env("QUERY_STRING", ""),      sizeof req->query_string);
    pr_strlcpy(req->remote_addr,  pr_cgi_env("REMOTE_ADDR", ""),       sizeof req->remote_addr);
    pr_strlcpy(req->cookie,       pr_cgi_env("HTTP_COOKIE", ""),       sizeof req->cookie);
    pr_strlcpy(req->content_type, pr_cgi_env("CONTENT_TYPE", ""),      sizeof req->content_type);

    const char *cl = getenv("CONTENT_LENGTH");
    req->content_length = (cl != NULL) ? (size_t)atoll(cl) : 0;

    /* QUERY_STRING */
    if (req->query_string[0] != '\0') {
        char *qs = pr_strdup(req->query_string);
        if (qs == NULL) {
            snprintf(err, errlen, "Speicher erschoepft");
            return -1;
        }
        query_parse(&req->query, qs);
        free(qs);
    }

    /* POST-Body */
    if (req->content_length > 0) {
        if (req->content_length > 1024u * 1024u) {
            snprintf(err, errlen, "Anfrage zu gross");
            return -1;
        }
        req->body = malloc(req->content_length + 1);
        if (req->body == NULL) {
            snprintf(err, errlen, "Speicher erschoepft");
            return -1;
        }
        size_t got = fread(req->body, 1, req->content_length, stdin);
        req->body[got] = '\0';
        req->content_length = got;

        /* Nur urlencoded parsen - multipart wird nicht unterstuetzt. */
        if (pr_starts_with(req->content_type, "application/x-www-form-urlencoded") ||
            req->content_type[0] == '\0') {
            query_parse(&req->form, req->body);
        }
    }

    return 0;
}

void pr_request_free(pr_request *req)
{
    if (req == NULL)
        return;
    query_free(&req->query);
    query_free(&req->form);
    free(req->body);
    req->body = NULL;
}

/* ======================================================================= */
/* Response                                                                */
/* ======================================================================= */

void pr_response_init(pr_response *r)
{
    memset(r, 0, sizeof *r);
    r->status = 200;
    pr_strlcpy(r->content_type, "text/html; charset=utf-8", sizeof r->content_type);
    pr_buf_init(&r->headers);
    pr_buf_init(&r->body);
}

void pr_response_free(pr_response *r)
{
    pr_buf_free(&r->headers);
    pr_buf_free(&r->body);
}

void pr_response_header(pr_response *r, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char tmp[1024];
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    pr_buf_add(&r->headers, tmp);
    pr_buf_addc(&r->headers, '\n');
}

void pr_response_set_cookie(pr_response *r, const char *name, const char *value,
                            int max_age_seconds, bool httponly)
{
    pr_response_header(r, "Set-Cookie: %s=%s; Path=/; Max-Age=%d; SameSite=Strict%s",
                       name, value, max_age_seconds, httponly ? "; HttpOnly" : "");
}

void pr_response_clear_cookie(pr_response *r, const char *name)
{
    pr_response_header(r, "Set-Cookie: %s=; Path=/; Max-Age=0; SameSite=Strict; HttpOnly",
                       name);
}

void pr_response_html(pr_response *r, int status)
{
    r->status = status;
    pr_strlcpy(r->content_type, "text/html; charset=utf-8", sizeof r->content_type);
}

void pr_response_json(pr_response *r, int status)
{
    r->status = status;
    pr_strlcpy(r->content_type, "application/json; charset=utf-8", sizeof r->content_type);
}

void pr_response_text(pr_response *r, int status)
{
    r->status = status;
    pr_strlcpy(r->content_type, "text/plain; charset=utf-8", sizeof r->content_type);
}

void pr_response_binary(pr_response *r, int status, const char *mime)
{
    r->status = status;
    pr_strlcpy(r->content_type, mime, sizeof r->content_type);
}

static const char *status_text(int s)
{
    switch (s) {
    case 200: return "OK";
    case 204: return "No Content";
    case 302: return "Found";
    case 303: return "See Other";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Payload Too Large";
    case 500: return "Internal Server Error";
    default:  return "Status";
    }
}

void pr_response_emit(const pr_response *r)
{
    printf("Status: %d %s\r\n", r->status, status_text(r->status));
    printf("Content-Type: %s\r\n", r->content_type);
    printf("Content-Length: %lu\r\n", (unsigned long)r->body.len);
    printf("X-Content-Type-Options: nosniff\r\n");
    printf("Referrer-Policy: no-referrer\r\n");
    printf("Cache-Control: no-store\r\n");
    printf("Expires: 0\r\n");

    if (r->headers.len > 0) {
        /* Mehrere Header mit \n getrennt */
        const char *p = r->headers.data;
        while (p != NULL && *p != '\0') {
            const char *nl = strchr(p, '\n');
            size_t n = (nl != NULL) ? (size_t)(nl - p) : strlen(p);
            if (n > 0) {
                printf("%.*s\r\n", (int)n, p);
            }
            p = (nl != NULL) ? nl + 1 : NULL;
        }
    }

    printf("\r\n");
    if (!r->head_only && r->body.len > 0)
        fwrite(r->body.data, 1, r->body.len, stdout);
    fflush(stdout);
}
