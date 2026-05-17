#if !defined(_WIN32) && !defined(_WIN64)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "proven.h"
#include "proven_test.h"
// Notice: NO <stdio.h> IS INCLUDED IN THIS TEST!
#include "proven/sysio.h"

#include <stdlib.h>

static int test_set_env(const char *name, const char *value) {
#if defined(_WIN32) || defined(_WIN64)
    return _putenv_s(name, value);
#else
    return setenv(name, value, 1);
#endif
}

static void test_unset_env(const char *name) {
#if defined(_WIN32) || defined(_WIN64)
    (void)_putenv_s(name, "");
#else
    (void)unsetenv(name);
#endif
}

int main() {
    // 1. Test Console Output using the proven structural formatter
    PROVEN_TEST_INFO("Testing console output and formatting...");
    PROVEN_TEST_INFO("Phase 18 [SysIO & Env]: Started standard stream extraction tests.");
    
    PROVEN_TEST_INFO("Testing structural printing: Hello {1}! I am {0} and this is number {:0>4}.", 
        PROVEN_ARG("Proven"), PROVEN_ARG("World"), PROVEN_ARG(42));
    
    (void)proven_eprintln("Warning: This message is correctly rendered via STDERR stream.");

    // 2. Test Environment Variables
    PROVEN_TEST_INFO("Testing environment variable access...");
    proven_byte_t backing[4096];
    proven_arena_t arena = proven_arena_create((proven_mem_mut_t){backing, sizeof(backing)});
    proven_allocator_t alloc = proven_arena_as_allocator(&arena);

    // Get an environment variable that is likely to exist (like PATH or USER/USERNAME)
    proven_result_u8str_t path_env = proven_env_get(alloc, PROVEN_LIT("PATH"));
    if (PROVEN_IS_OK(path_env.err)) {
        PROVEN_TEST_INFO("SUCCESS: Found PATH environment variable.");
        // We print a small substring to keep terminal output clean
        proven_size_t snippet_len = path_env.value.internal.len > 50 ? 50 : path_env.value.internal.len;
        proven_u8str_view_t snippet = { .ptr = path_env.value.internal.ptr, .size = snippet_len };
        PROVEN_TEST_INFO("PATH Head: {}...", PROVEN_ARG(snippet));
    } else {
        (void)proven_eprintln("Failed to read PATH environment block.");
    }

    // Try a fake env var to ensure error handling is stable
    proven_result_u8str_t fake_env = proven_env_get(alloc, PROVEN_LIT("PROVEN_FAKE_NON_EXISTENT"));
    if (!PROVEN_IS_OK(fake_env.err)) {
        PROVEN_TEST_INFO("SUCCESS: Correctly failed to find PROVEN_FAKE_NON_EXISTENT.");
    } else {
        (void)proven_eprintln("FAIL: Oh no! The fake env var somehow existed?");
    }

    // Long names must not be rejected by an internal fixed-size key buffer.
    char long_key[320];
    for (proven_size_t i = 0; i < sizeof(long_key) - 1u; ++i) {
        long_key[i] = (char)('A' + (i % 26u));
    }
    long_key[0] = 'P';
    long_key[1] = 'R';
    long_key[2] = 'O';
    long_key[3] = 'V';
    long_key[4] = 'E';
    long_key[5] = 'N';
    long_key[sizeof(long_key) - 1u] = '\0';

    const char *long_value = "long-env-value";
    PROVEN_TEST_ASSERT(test_set_env(long_key, long_value) == 0,
        "Long environment key can be installed for the regression test",
        "Check the host environment API");

    proven_u8str_view_t long_key_view = {
        .ptr = (const proven_byte_t*)long_key,
        .size = sizeof(long_key) - 1u
    };
    proven_result_u8str_t long_env = proven_env_get(alloc, long_key_view);
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(long_env.err),
        "proven_env_get accepts keys larger than the old 255-byte stack limit",
        "Use the allocator for oversized key conversion instead of returning OUT_OF_BOUNDS");
    PROVEN_TEST_ASSERT(long_env.value.internal.len == 14u,
        "Long-key environment value length matches",
        "Check returned value length");
    PROVEN_TEST_ASSERT(proven_u8str_view_eq(
            (proven_u8str_view_t){ .ptr = long_env.value.internal.ptr, .size = long_env.value.internal.len },
            PROVEN_LIT("long-env-value")),
        "Long-key environment value content matches",
        "Check temporary key buffer conversion");
    test_unset_env(long_key);

    proven_arena_destroy(&arena);
    
    PROVEN_TEST_INFO("Phase 18 [SysIO & Env] passed!");
    return 0;
}
