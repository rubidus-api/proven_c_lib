#ifndef PROVEN_MANUAL_EXAMPLE_H
#define PROVEN_MANUAL_EXAMPLE_H

/*
 * Shared preamble for the manual's examples.
 *
 * The examples are real programs: the build driver compiles and runs every one
 * of them, so a manual example that stops being true stops the build. This
 * header only supplies the check macro and the includes - the examples
 * themselves are written the way a caller would write them, with explicit
 * allocators, explicit error handling, and a destroy for everything owned.
 */

#include "proven.h"
#include <stdio.h>

static int g_example_failures = 0;

#define EXAMPLE_REQUIRE(cond, msg)                                             \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("[PROVEN][EXAMPLE][FAIL] %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
            printf("[PROVEN][EXAMPLE][COND] %s\n", #cond);                     \
            ++g_example_failures;                                              \
        }                                                                      \
    } while (0)

/* Return this from main: non-zero if any check failed, so the build catches it. */
#define EXAMPLE_OK()                                                           \
    (g_example_failures == 0                                                   \
        ? (printf("[PROVEN][EXAMPLE][PASS] %s\n", __FILE__), 0)                \
        : (printf("[PROVEN][EXAMPLE][FAIL] %s: %d check(s) failed\n",          \
                  __FILE__, g_example_failures), 1))

#endif /* PROVEN_MANUAL_EXAMPLE_H */
