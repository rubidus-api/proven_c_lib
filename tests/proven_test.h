#ifndef PROVEN_TEST_H
#define PROVEN_TEST_H

static inline const char *proven_test_text_or_default(const char *text, const char *fallback) {
    return (text != 0 && text[0] != '\0') ? text : fallback;
}

#ifdef PROVEN_FREESTANDING
#include <stdio.h>
#include <stdlib.h>

#define PROVEN_TEST_SUITE(name, intent, hint) \
    do { \
        printf("[PROVEN][TEST][BEGIN] suite=%s\n", name); \
        printf("[PROVEN][TEST][INTENT] %s\n", proven_test_text_or_default(intent, "No suite intent supplied.")); \
        printf("[PROVEN][TEST][FAIL_HINT] %s\n", proven_test_text_or_default(hint, "Read the failing check and inspect the documented contract.")); \
    } while (0)

#define PROVEN_TEST_SECTION(name, intent, hint) \
    do { \
        printf("[PROVEN][SECTION][BEGIN] name=%s\n", name); \
        printf("[PROVEN][SECTION][INTENT] %s\n", proven_test_text_or_default(intent, "No section intent supplied.")); \
        printf("[PROVEN][SECTION][FAIL_HINT] %s\n", proven_test_text_or_default(hint, "Read the failing check and inspect the documented contract.")); \
    } while (0)

#define PROVEN_TEST_ASSERT(cond, msg, hint) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "\n[PROVEN][CHECK][FAIL] file=%s line=%d\n", __FILE__, __LINE__); \
            fprintf(stderr, "[PROVEN][CHECK][COND] %s\n", #cond); \
            fprintf(stderr, "[PROVEN][CHECK][INTENT] %s\n", proven_test_text_or_default(msg, "Assertion intent was not supplied.")); \
            fprintf(stderr, "[PROVEN][CHECK][FAIL_HINT] %s\n", proven_test_text_or_default(hint, "Inspect this check, its caller contract, and the implementation named by the test suite metadata.")); \
            exit(1); \
        } \
    } while(0)

#define PROVEN_TEST_INFO(fmt, ...) \
    do { printf("[PROVEN][TEST][INFO] " fmt "\n" __VA_OPT__(,) __VA_ARGS__); } while(0)

#define PROVEN_TEST_PASS(fmt, ...) \
    do { printf("[PROVEN][TEST][PASS] " fmt "\n" __VA_OPT__(,) __VA_ARGS__); } while(0)

#else

#include "proven.h"
#include <stdlib.h>

#define PROVEN_TEST_SUITE(name, intent, hint) \
    do { \
        proven_println("[PROVEN][TEST][BEGIN] suite={}", PROVEN_ARG((const char *)(name))); \
        proven_println("[PROVEN][TEST][INTENT] {}", PROVEN_ARG((const char *)proven_test_text_or_default(intent, "No suite intent supplied."))); \
        proven_println("[PROVEN][TEST][FAIL_HINT] {}", PROVEN_ARG((const char *)proven_test_text_or_default(hint, "Read the failing check and inspect the documented contract."))); \
    } while (0)

#define PROVEN_TEST_SECTION(name, intent, hint) \
    do { \
        proven_println("[PROVEN][SECTION][BEGIN] name={}", PROVEN_ARG((const char *)(name))); \
        proven_println("[PROVEN][SECTION][INTENT] {}", PROVEN_ARG((const char *)proven_test_text_or_default(intent, "No section intent supplied."))); \
        proven_println("[PROVEN][SECTION][FAIL_HINT] {}", PROVEN_ARG((const char *)proven_test_text_or_default(hint, "Read the failing check and inspect the documented contract."))); \
    } while (0)

#define PROVEN_TEST_ASSERT(cond, msg, hint) \
    do { \
        if (!(cond)) { \
            proven_eprintln("\n[PROVEN][CHECK][FAIL] file={} line={}", PROVEN_ARG((const char *)__FILE__), PROVEN_ARG((int)__LINE__)); \
            proven_eprintln("[PROVEN][CHECK][COND] {}", PROVEN_ARG((const char *)#cond)); \
            proven_eprintln("[PROVEN][CHECK][INTENT] {}", PROVEN_ARG((const char *)proven_test_text_or_default(msg, "Assertion intent was not supplied."))); \
            proven_eprintln("[PROVEN][CHECK][FAIL_HINT] {}", PROVEN_ARG((const char *)proven_test_text_or_default(hint, "Inspect this check, its caller contract, and the implementation named by the test suite metadata."))); \
            exit(1); \
        } \
    } while(0)

#define PROVEN_TEST_INFO(fmt, ...) \
    proven_println("[PROVEN][TEST][INFO] " fmt __VA_OPT__(,) __VA_ARGS__)

#define PROVEN_TEST_PASS(fmt, ...) \
    proven_println("[PROVEN][TEST][PASS] " fmt __VA_OPT__(,) __VA_ARGS__)

#endif

#endif // PROVEN_TEST_H
