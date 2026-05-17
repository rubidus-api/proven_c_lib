#ifndef PROVEN_TEST_FW_H
#define PROVEN_TEST_FW_H

#include "proven.h"
#include <stdlib.h>

/**
 * @file proven_test_fw.h
 * @brief Structured unit testing helpers for proven library tests.
 */

typedef struct {
    int passed;
    int failed;
    const char *current_test;
    const char *current_intent;
    const char *current_hint;
} proven_test_fw_runner_t;

extern proven_test_fw_runner_t g_proven_test_runner;

#define PROVEN_TEST_FW_INIT() \
    proven_test_fw_runner_t g_proven_test_runner = {0, 0, "", "", ""};

static inline const char *proven_test_fw_text_or_default(const char *text, const char *fallback) {
    return (text != 0 && text[0] != '\0') ? text : fallback;
}

static inline void proven_test_fw_start(void) {
    proven_println("[PROVEN][TEST][INFO] framework=start");
}

#define PROVEN_TEST_CASE_EX(name, intent, hint) \
    do { \
        g_proven_test_runner.current_test = name; \
        g_proven_test_runner.current_intent = intent; \
        g_proven_test_runner.current_hint = hint; \
        proven_println("[PROVEN][SECTION][BEGIN] name={}", PROVEN_ARG((const char *)name)); \
        proven_println("[PROVEN][SECTION][INTENT] {}", PROVEN_ARG((const char *)proven_test_fw_text_or_default(intent, "No section intent supplied."))); \
        proven_println("[PROVEN][SECTION][FAIL_HINT] {}", PROVEN_ARG((const char *)proven_test_fw_text_or_default(hint, "Inspect this check and its documented contract."))); \
    } while (0)

#define PROVEN_TEST_CASE(name) \
    PROVEN_TEST_CASE_EX(name, "Legacy test case without a dedicated intent string.", "Inspect the failed assertion and convert this case to PROVEN_TEST_CASE_EX when editing it.")

#define PROVEN_ASSERT_EX(cond, msg, hint) \
    do { \
        if (cond) { \
            g_proven_test_runner.passed++; \
        } else { \
            g_proven_test_runner.failed++; \
            proven_eprintln("[PROVEN][CHECK][FAIL] file={} line={} case={}", \
                PROVEN_ARG((const char*)__FILE__), \
                PROVEN_ARG((int)__LINE__), \
                PROVEN_ARG((const char*)g_proven_test_runner.current_test)); \
            proven_eprintln("[PROVEN][CHECK][COND] {}", PROVEN_ARG((const char*)#cond)); \
            proven_eprintln("[PROVEN][CHECK][INTENT] {}", PROVEN_ARG((const char*)proven_test_fw_text_or_default(msg, g_proven_test_runner.current_intent))); \
            proven_eprintln("[PROVEN][CHECK][FAIL_HINT] {}", PROVEN_ARG((const char*)proven_test_fw_text_or_default(hint, g_proven_test_runner.current_hint))); \
        } \
    } while (0)

#define PROVEN_ASSERT(cond, msg) \
    PROVEN_ASSERT_EX(cond, msg, "Inspect the failing condition and the current test case contract.")
 
#define PROVEN_TEST_INFO(fmt, ...) \
    proven_println("[PROVEN][TEST][INFO] " fmt __VA_OPT__(,) __VA_ARGS__)

static inline int proven_test_fw_report(void) {
    proven_println("[PROVEN][RUN][SUMMARY] checks_passed={} checks_failed={}",
                   PROVEN_ARG(g_proven_test_runner.passed),
                   PROVEN_ARG(g_proven_test_runner.failed));
    return g_proven_test_runner.failed > 0 ? 1 : 0;
}

#endif // PROVEN_TEST_FW_H
