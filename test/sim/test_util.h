#ifndef TEST_UTIL_H
#define TEST_UTIL_H

#include <stdio.h>

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
