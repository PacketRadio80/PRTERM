/*
 * PRTERM - Test: the trace module - levels, filtering, hexdumps.
 *
 * stderr is redirected into a file (dup/dup2, POSIX) so the output
 * can be checked byte for byte.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "prterm_compat.h"

#include "trace.h"
#include "testutil.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LOGFILE "test-trace.log"

static char logbuf[65536];

/* Point stderr at LOGFILE; returns the saved original fd. */
static int capture_begin(void)
{
    int saved = dup(STDERR_FILENO);
    FILE *f = fopen(LOGFILE, "w");
    if (f == NULL)
        return -1;
    (void)dup2(fileno(f), STDERR_FILENO);
    fclose(f);
    return saved;
}

static void capture_end(int saved)
{
    fflush(stderr);
    if (saved >= 0) {
        (void)dup2(saved, STDERR_FILENO);
        close(saved);
    }
}

static const char *capture_read(void)
{
    logbuf[0] = '\0';
    FILE *f = fopen(LOGFILE, "rb");
    if (f == NULL)
        return logbuf;
    size_t n = fread(logbuf, 1, sizeof logbuf - 1, f);
    logbuf[n] = '\0';
    fclose(f);
    return logbuf;
}

int main(void)
{
    printf("== trace levels ==\n");
    CHECK_INT(pr_trace_level_from_name("off", PR_TR_INFO), PR_TR_OFF);
    CHECK_INT(pr_trace_level_from_name("ERROR", PR_TR_INFO), PR_TR_ERROR);
    CHECK_INT(pr_trace_level_from_name("warn", PR_TR_INFO), PR_TR_WARN);
    CHECK_INT(pr_trace_level_from_name("Info", PR_TR_OFF), PR_TR_INFO);
    CHECK_INT(pr_trace_level_from_name("debug", PR_TR_OFF), PR_TR_DEBUG);
    CHECK_INT(pr_trace_level_from_name("trace", PR_TR_OFF), PR_TR_TRACE);
    CHECK_INT(pr_trace_level_from_name("nonsense", PR_TR_WARN), PR_TR_WARN);
    CHECK_INT(pr_trace_level_from_name(NULL, PR_TR_DEBUG), PR_TR_DEBUG);
    CHECK_INT(pr_trace_level_from_name("", PR_TR_DEBUG), PR_TR_DEBUG);

    CHECK_STR(pr_trace_level_name(PR_TR_OFF), "off");
    CHECK_STR(pr_trace_level_name(PR_TR_ERROR), "error");
    CHECK_STR(pr_trace_level_name(PR_TR_WARN), "warn");
    CHECK_STR(pr_trace_level_name(PR_TR_INFO), "info");
    CHECK_STR(pr_trace_level_name(PR_TR_DEBUG), "debug");
    CHECK_STR(pr_trace_level_name(PR_TR_TRACE), "trace");
    for (int lv = PR_TR_OFF; lv <= PR_TR_TRACE; lv++)
        CHECK_INT(pr_trace_level_from_name(pr_trace_level_name(lv),
                                           PR_TR_OFF), lv);

    printf("\n== filtering + format ==\n");
    {
        int saved = capture_begin();
        CHECK(saved >= 0);

        pr_trace_init(PR_TR_WARN, "test");
        CHECK_INT(pr_trace_get_level(), PR_TR_WARN);
        pr_trace(PR_TR_INFO, "invisible %d", 1);
        pr_trace(PR_TR_WARN, "visible-warn");
        pr_trace(PR_TR_ERROR, "visible-error %s", "x");

        capture_end(saved);
        const char *log = capture_read();
        CHECK(strstr(log, "invisible") == NULL);
        CHECK(strstr(log, "visible-warn") != NULL);
        CHECK(strstr(log, "visible-error x") != NULL);
        CHECK(strstr(log, "test warn: ") != NULL);
        CHECK(strstr(log, "test error: ") != NULL);
        CHECK(log[0] == '[');            /* timestamp bracket */
    }

    printf("\n== off is silent ==\n");
    {
        int saved = capture_begin();
        pr_trace_init(PR_TR_OFF, "test");
        pr_trace(PR_TR_ERROR, "must not appear");
        pr_trace_hex(PR_TR_TRACE, "lbl", (const unsigned char *)"xy", 2);
        capture_end(saved);
        CHECK(capture_read()[0] == '\0');
    }

    printf("\n== hexdump ==\n");
    {
        int saved = capture_begin();
        pr_trace_init(PR_TR_TRACE, "hex");

        unsigned char data[80];
        for (size_t i = 0; i < sizeof data; i++)
            data[i] = (unsigned char)(0xC0 + (i % 4));   /* C0 C1 C2 C3 ... */
        pr_trace_hex(PR_TR_TRACE, "SER>>", data, sizeof data);

        capture_end(saved);
        const char *log = capture_read();
        CHECK(strstr(log, "SER>> - 80 bytes") != NULL);
        CHECK(strstr(log, "0000  c0 c1 c2 c3") != NULL);
        CHECK(strstr(log, "0040  ") != NULL);           /* second-line offset */
        CHECK(strstr(log, "|................|") != NULL);  /* ASCII column */
        CHECK(strstr(log, "not shown") == NULL);        /* under the cap */
    }

    printf("\n== hexdump cap ==\n");
    {
        int saved = capture_begin();
        pr_trace_init(PR_TR_TRACE, "hex");

        static unsigned char big[600];
        memset(big, 0xAB, sizeof big);
        pr_trace_hex(PR_TR_TRACE, "big", big, sizeof big);

        capture_end(saved);
        const char *log = capture_read();
        CHECK(strstr(log, "big - 600 bytes") != NULL);
        CHECK(strstr(log, "(88 bytes not shown)") != NULL);
    }

    printf("\n== enabled threshold ==\n");
    {
        int saved = capture_begin();
        pr_trace_init(PR_TR_DEBUG, "th");
        pr_trace(PR_TR_TRACE, "trace-hidden");     /* above threshold */
        pr_trace_hex(PR_TR_TRACE, "hidden", (const unsigned char *)"\xc0", 1);
        pr_trace(PR_TR_DEBUG, "debug-shown");
        capture_end(saved);
        const char *log = capture_read();
        CHECK(strstr(log, "trace-hidden") == NULL);
        CHECK(strstr(log, "hidden") == NULL);
        CHECK(strstr(log, "debug-shown") != NULL);
    }

    (void)remove(LOGFILE);
    pr_trace_init(PR_TR_WARN, "prterm");
    TEST_SUMMARY("trace");
}
