#ifndef PROVEN_TEST_FW_H
#define PROVEN_TEST_FW_H

#include "proven.h"
#include <stdlib.h>

/**
 * @file proven_test_fw.h
 * @brief Structured unit testing framework for proven library
 */

typedef struct {
    int passed;
    int failed;
    const char *current_test;
} proven_test_fw_runner_t;

extern proven_test_fw_runner_t g_proven_test_runner;

#define PROVEN_TEST_FW_INIT() \
    proven_test_fw_runner_t g_proven_test_runner = {0, 0, ""};

static inline void proven_test_fw_start(void) {
    proven_println("[TEST FRAMEWORK] Starting tests...");
}

#define PROVEN_TEST_CASE(name) \
    do { \
        g_proven_test_runner.current_test = name; \
        proven_println("\n[TEST CASE] {}", PROVEN_ARG((const char*)name)); \
    } while (0)

#define PROVEN_ASSERT(cond, msg) \
    do { \
        if (cond) { \
            g_proven_test_runner.passed++; \
        } else { \
            g_proven_test_runner.failed++; \
            proven_eprintln("  [ASSERT FAILED] {}:{} in test case '{}'", \
                PROVEN_ARG((const char*)__FILE__), \
                PROVEN_ARG((int)__LINE__), \
                PROVEN_ARG((const char*)g_proven_test_runner.current_test)); \
            proven_eprintln("    Intent: {}", PROVEN_ARG((const char*)msg)); \
        } \
    } while (0)
 
 #define PROVEN_TEST_INFO(fmt, ...) \
    proven_println("[INFO] " fmt __VA_OPT__(,) __VA_ARGS__)

static inline int proven_test_fw_report(void) {
    proven_println("\n[TEST SUMMARY] Passed: {}, Failed: {}", \
                   PROVEN_ARG(g_proven_test_runner.passed), \
                   PROVEN_ARG(g_proven_test_runner.failed)); \
    return g_proven_test_runner.failed > 0 ? 1 : 0;
}

#endif // PROVEN_TEST_FW_H
