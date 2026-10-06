/*
 * PRTERM - CB & Amateur Radio Terminal
 * cgi.h - Request/response per RFC 3875 (Common Gateway Interface).
 *
 * PRTERM has exactly one URL. Everything distinguishable runs via query
 * parameters and form fields of the same address - see docs/ROUTING.md.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PRTERM_CGI_H
#define PRTERM_CGI_H

#include "util.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct pr_pair {
    char *key;
    char *value;
} pr_pair;

typedef struct pr_query {
    pr_pair *items;
    size_t   n;
    size_t   cap;
} pr_query;

typedef struct pr_request {
    char   method[8];
    char   script_name[256];
    char   path_info[256];
    char   query_string[4096];
    char   remote_addr[64];
    char   cookie[1024];
    char   content_type[128];
    size_t content_length;
    char  *body;

    pr_query query;      /* QUERY_STRING */
    pr_query form;       /* POST-Body, application/x-www-form-urlencoded */
} pr_request;

/* ---- Request ---------------------------------------------------------- */
int  pr_request_parse(pr_request *req, char *err, size_t errlen);
void pr_request_free(pr_request *req);

const char *pr_req_get(const pr_request *req, const char *key);
const char *pr_req_post(const pr_request *req, const char *key);
/* POST first, then GET - for actions that accept both.          */
const char *pr_req_param(const pr_request *req, const char *key);
bool pr_req_cookie(const pr_request *req, const char *name,
                   char *dst, size_t dstlen);

/* ---- Response --------------------------------------------------------- */
typedef struct pr_response {
    int    status;
    char   content_type[64];
    pr_buf headers;      /* extra headers, \n-separated */
    pr_buf body;
    bool   head_only;
    bool   emitted;
} pr_response;

void pr_response_init(pr_response *r);
void pr_response_free(pr_response *r);
void pr_response_header(pr_response *r, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;
void pr_response_set_cookie(pr_response *r, const char *name, const char *value,
                            int max_age_seconds, bool httponly);
void pr_response_clear_cookie(pr_response *r, const char *name);

/* Sets status + Content-Type  */
void pr_response_html(pr_response *r, int status);
void pr_response_json(pr_response *r, int status);
void pr_response_text(pr_response *r, int status);
void pr_response_binary(pr_response *r, int status, const char *mime);

void pr_response_emit(const pr_response *r);

/* ---- Environment ------------------------------------------------------------ */
bool pr_is_cgi(void);
const char *pr_cgi_env(const char *name, const char *dflt);

#endif /* PRTERM_CGI_H */
