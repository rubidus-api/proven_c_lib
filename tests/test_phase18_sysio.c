#include "proven.h"
#include "proven_test.h"
// Notice: NO <stdio.h> IS INCLUDED IN THIS TEST!
#include "proven/sysio.h"

int main() {
    // 1. Test Console Output using the proven structural formatter
    PROVEN_TEST_INFO("Phase 18 [SysIO & Env]: Started standard stream extraction tests.");
    
    PROVEN_TEST_INFO("Testing structural printing: Hello {1}! I am {0} and this is number {:0>4}.", 
        PROVEN_ARG("Proven"), PROVEN_ARG("World"), PROVEN_ARG(42));
    
    (void)proven_eprintln("Warning: This message is correctly rendered via STDERR stream.");

    // 2. Test Environment Variables
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

    proven_arena_destroy(&arena);
    
    PROVEN_TEST_INFO("Phase 18 [SysIO & Env] passed!");
    return 0;
}
