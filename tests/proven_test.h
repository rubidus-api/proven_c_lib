#ifndef PROVEN_TEST_H
#define PROVEN_TEST_H

#ifdef PROVEN_FREESTANDING
#include <stdio.h>
#include <stdlib.h>

#define PROVEN_TEST_ASSERT(cond, msg, hint) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "\n[TEST FAILED] %s:%d\n", __FILE__, __LINE__); \
            fprintf(stderr, "  Condition : %s\n", #cond); \
            fprintf(stderr, "  Intent    : %s\n", msg); \
            fprintf(stderr, "  Fix Hint  : %s\n", hint); \
            exit(1); \
        } \
    } while(0)

#define PROVEN_TEST_INFO(fmt, ...) \
    do { printf("[INFO] " fmt "\n" __VA_OPT__(,) __VA_ARGS__); } while(0)

#define PROVEN_TEST_PASS(fmt, ...) \
    do { printf("[PASS] " fmt "\n" __VA_OPT__(,) __VA_ARGS__); } while(0)

#else

#include "proven.h"
#include <stdlib.h>

#define PROVEN_TEST_ASSERT(cond, msg, hint) \
    do { \
        if (!(cond)) { \
            proven_eprintln("\n[TEST FAILED] {}:{}", PROVEN_ARG(PROVEN_LIT(__FILE__)), PROVEN_ARG(__LINE__)); \
            proven_eprintln("  Condition : {}", PROVEN_ARG(PROVEN_LIT(#cond))); \
            proven_eprintln("  Intent    : {}", PROVEN_ARG(PROVEN_LIT(msg))); \
            proven_eprintln("  Fix Hint  : {}", PROVEN_ARG(PROVEN_LIT(hint))); \
            exit(1); \
        } \
    } while(0)

#define PROVEN_TEST_INFO(fmt, ...) \
    proven_println("[INFO] " fmt __VA_OPT__(,) __VA_ARGS__)

#define PROVEN_TEST_PASS(fmt, ...) \
    proven_println("[PASS] " fmt __VA_OPT__(,) __VA_ARGS__)

#endif

#endif // PROVEN_TEST_H
