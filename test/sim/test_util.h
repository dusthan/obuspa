#ifndef TEST_UTIL_H
#define TEST_UTIL_H

#include <stdio.h>

// Fallback for platforms whose <sys/wait.h> lacks W_EXITCODE
#ifndef W_EXITCODE
#define W_EXITCODE(code, sig) (((code) << 8) | (sig))
#endif

static int g_test_failures = 0;

#define CHECK(cond, ...) \
    do { \
        if (!(cond)) { \
            g_test_failures++; \
            fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
            fprintf(stderr, __VA_ARGS__); \
            fprintf(stderr, "\n"); \
        } \
    } while (0)

#endif
