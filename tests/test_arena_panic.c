#include "proven/arena.h"
#include "proven_test.h"
#include <stdlib.h>
#include <string.h>

static int panic_triggered = 0;

// Override the weakly linked panic handler
void proven_panic_handler(const char *msg) {
    (void)msg;
    panic_triggered = 1;
}

int main(void) {
    PROVEN_TEST_INFO("--- Running test_arena_panic ---");

    proven_u8 buffer[64];
    proven_mem_mut_t backing = { .ptr = buffer, .size = sizeof(buffer) };
    proven_arena_t arena = proven_arena_create(backing);

    // Test non-panic path
    proven_mem_mut_t alloc1 = proven_arena_alloc_or_panic(&arena, 32);
    PROVEN_TEST_ASSERT(alloc1.ptr != NULL, "alloc_or_panic allocates successfully when space is available", "");
    PROVEN_TEST_ASSERT(panic_triggered == 0, "Panic is not triggered on successful allocation", "");

    // Test panic path
    // Allocate more than available (32 bytes remaining + some alignment overhead vs 64)
    proven_mem_mut_t alloc2 = proven_arena_alloc_or_panic(&arena, 64);
    (void)alloc2;
    PROVEN_TEST_ASSERT(panic_triggered == 1, "Panic is triggered on OOM when using alloc_or_panic", "");

    PROVEN_TEST_PASS("--- Finished test_arena_panic ---");
    return 0;
}
