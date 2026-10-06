/*
 * PRTERM - test scaffolding
 * testutil.h - minimal test helpers.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PRTERM_TESTUTIL_H
#define PRTERM_TESTUTIL_H

#include <stdio.h>
#include <string.h>

static int pr_test_fails = 0;
static int pr_test_checks = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        pr_test_checks++;                                                  \
        if (cond) {                                                        \
            printf("  ok   %s (%s:%d)\n", #cond, __FILE__, __LINE__);      \
        } else {                                                           \
            printf("  FAIL %s (%s:%d)\n", #cond, __FILE__, __LINE__);    \
            pr_test_fails++;                                               \
        }                                                                  \
    } while (0)

#define CHECK_STR(a, b)                                                    \
    do {                                                                   \
        pr_test_checks++;                                                  \
        const char *_a = (a), *_b = (b);                                   \
        if (_a != NULL && _b != NULL && strcmp(_a, _b) == 0) {             \
            printf("  ok   %s == %s (%s:%d)\n", #a, #b, __FILE__, __LINE__); \
        } else {                                                           \
            printf("  FAIL %s != %s\n     got:  \"%s\"\n     want: \"%s\"\n", \
                   #a, #b, _a ? _a : "(null)", _b ? _b : "(null)");        \
            pr_test_fails++;                                               \
        }                                                                  \
    } while (0)

#define CHECK_INT(a, b)                                                    \
    do {                                                                   \
        pr_test_checks++;                                                  \
        long long _a = (long long)(a), _b = (long long)(b);                \
        if (_a == _b) {                                                    \
            printf("  ok   %s == %s (%s:%d)\n", #a, #b, __FILE__, __LINE__); \
        } else {                                                           \
            printf("  FAIL %s != %s  (%lld != %lld) (%s:%d)\n",          \
                   #a, #b, _a, _b, __FILE__, __LINE__);                    \
            pr_test_fails++;                                               \
        }                                                                  \
    } while (0)

#define TEST_SUMMARY(name)                                                 \
    do {                                                                   \
        printf("\n%s: %d checks, %d failures\n",                         \
               (name), pr_test_checks, pr_test_fails);                     \
        return pr_test_fails == 0 ? 0 : 1;                                 \
    } while (0)

#endif /* PRTERM_TESTUTIL_H */
