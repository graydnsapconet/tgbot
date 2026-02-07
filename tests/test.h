#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_test_pass = 0;
static int g_test_fail = 0;
static const char *g_test_current = NULL;

#define TEST(name)                                                                                 \
    static void test_##name(void);                                                                 \
    __attribute__((constructor)) static void register_##name(void)                                 \
    {                                                                                              \
        g_test_current = #name;                                                                    \
        printf("  [RUN ] %s\n", #name);                                                            \
        test_##name();                                                                             \
        printf("  [PASS] %s\n", #name);                                                            \
    }                                                                                              \
    static void test_##name(void)

#define ASSERT(cond)                                                                               \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "  [FAIL] %s:%d: ASSERT(%s) in %s\n", __FILE__, __LINE__, #cond,       \
                    g_test_current ? g_test_current : "?");                                        \
            g_test_fail++;                                                                         \
            return;                                                                                \
        }                                                                                          \
        g_test_pass++;                                                                             \
    } while (0)

#define ASSERT_EQ(a, b)                                                                            \
    do {                                                                                           \
        if ((a) != (b)) {                                                                          \
            fprintf(stderr, "  [FAIL] %s:%d: ASSERT_EQ(%s, %s) => %lld != %lld in %s\n",           \
                    __FILE__, __LINE__, #a, #b, (long long)(a), (long long)(b),                    \
                    g_test_current ? g_test_current : "?");                                        \
            g_test_fail++;                                                                         \
            return;                                                                                \
        }                                                                                          \
        g_test_pass++;                                                                             \
    } while (0)

#define ASSERT_STR_EQ(a, b)                                                                        \
    do {                                                                                           \
        if (strcmp((a), (b)) != 0) {                                                               \
            fprintf(stderr, "  [FAIL] %s:%d: ASSERT_STR_EQ(\"%s\", \"%s\") in %s\n", __FILE__,     \
                    __LINE__, (a), (b), g_test_current ? g_test_current : "?");                    \
            g_test_fail++;                                                                         \
            return;                                                                                \
        }                                                                                          \
        g_test_pass++;                                                                             \
    } while (0)

#define ASSERT_NOT_NULL(ptr)                                                                       \
    do {                                                                                           \
        if ((ptr) == NULL) {                                                                       \
            fprintf(stderr, "  [FAIL] %s:%d: ASSERT_NOT_NULL(%s) in %s\n", __FILE__, __LINE__,     \
                    #ptr, g_test_current ? g_test_current : "?");                                  \
            g_test_fail++;                                                                         \
            return;                                                                                \
        }                                                                                          \
        g_test_pass++;                                                                             \
    } while (0)

#define ASSERT_NULL(ptr)                                                                           \
    do {                                                                                           \
        if ((ptr) != NULL) {                                                                       \
            fprintf(stderr, "  [FAIL] %s:%d: ASSERT_NULL(%s) in %s\n", __FILE__, __LINE__, #ptr,   \
                    g_test_current ? g_test_current : "?");                                        \
            g_test_fail++;                                                                         \
            return;                                                                                \
        }                                                                                          \
        g_test_pass++;                                                                             \
    } while (0)

static inline int test_summarise(void)
{
    printf("\n  %d assertions passed, %d failed\n", g_test_pass, g_test_fail);
    return g_test_fail > 0 ? 1 : 0;
}
