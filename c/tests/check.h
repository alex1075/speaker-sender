/* tiny test helpers */
#ifndef AP_CHECK_H
#define AP_CHECK_H

#include <stdio.h>
#include <string.h>

static int g_checks = 0, g_fails = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        g_checks++;                                                            \
        if (!(cond)) {                                                         \
            g_fails++;                                                         \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
        }                                                                      \
    } while (0)

#define CHECK_MEM(a, b, n) CHECK(memcmp((a), (b), (n)) == 0)

#define RUN(fn)                                                                \
    do {                                                                       \
        int before = g_fails;                                                  \
        fn();                                                                  \
        printf("%-34s %s\n", #fn, g_fails == before ? "ok" : "FAILED");        \
    } while (0)

#define FINISH()                                                               \
    do {                                                                       \
        printf("%d checks, %d failed\n", g_checks, g_fails);                   \
        return g_fails ? 1 : 0;                                                \
    } while (0)

#endif
