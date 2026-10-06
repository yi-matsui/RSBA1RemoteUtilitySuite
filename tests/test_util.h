/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 *
 * 外部依存なしの最小テストマクロ。
 */
#ifndef TEST_UTIL_H
#define TEST_UTIL_H

#include <stdio.h>

static int g_test_failures;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            g_test_failures++;                                                   \
        }                                                                        \
    } while (0)

#define CHECK_RC(expr, expected)                                                 \
    do {                                                                         \
        int rc_ = (expr);                                                        \
        if (rc_ != (expected)) {                                                 \
            fprintf(stderr, "%s:%d: %s returned %d, expected %d\n",              \
                    __FILE__, __LINE__, #expr, rc_, (int)(expected));            \
            g_test_failures++;                                                   \
        }                                                                        \
    } while (0)

#define TEST_RESULT()                                                            \
    (g_test_failures == 0 ? (printf("OK\n"), 0)                                  \
                          : (printf("%d failure(s)\n", g_test_failures), 1))

#endif /* TEST_UTIL_H */
