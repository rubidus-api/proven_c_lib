#include "proven.h"
#include "proven_test.h"
#include "proven/sysio.h"



/**
 * @file test_phase4.c
 * @brief Unit tests for Phase 4 (Arena Allocator).
 */

int main() {
    PROVEN_TEST_INFO("Running Phase 4 Arena Allocator Tests...");

    /* 1. Setup a backing buffer simulating system memory (e.g. from stack or mmap) */
    proven_byte_t backing_buf[128];
    proven_mem_mut_t backing = { .ptr = backing_buf, .size = sizeof(backing_buf) };

    /* 2. Create Arena */
    proven_arena_t arena = proven_arena_create(backing);
    PROVEN_TEST_ASSERT(arena.offset == 0, "Testing condition: arena.offset == 0", "Review logic surrounding arena.offset == 0");
    PROVEN_TEST_ASSERT(arena.backing.size == 128, "Testing condition: arena.backing.size == 128", "Review logic surrounding arena.backing.size == 128");

    /* 3. Successful Allocations */
    proven_result_mem_mut_t res1 = proven_arena_alloc(&arena, 10);
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(res1.err), "Testing condition: PROVEN_IS_OK(res1.err)", "Review logic surrounding PROVEN_IS_OK(res1.err)");
    PROVEN_TEST_ASSERT(res1.value.size == 10, "Testing condition: res1.value.size == 10", "Review logic surrounding res1.value.size == 10");
    // Address must be explicitly 8-byte aligned (default)
    PROVEN_TEST_ASSERT((proven_size_t)res1.value.ptr % PROVEN_DEFAULT_ALIGNMENT == 0, "Testing condition: (proven_size_t)res1.value.ptr % PROVEN_DEFAULT_ALIGNMENT == 0", "Review logic surrounding (proven_size_t)res1.value.ptr % PROVEN_DEFAULT_ALIGNMENT == 0");

    /* 4. Force a specific alignment */
    proven_result_mem_mut_t res2 = proven_arena_alloc_aligned(&arena, 4, 32);
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(res2.err), "Testing condition: PROVEN_IS_OK(res2.err)", "Review logic surrounding PROVEN_IS_OK(res2.err)");
    PROVEN_TEST_ASSERT(res2.value.size == 4, "Testing condition: res2.value.size == 4", "Review logic surrounding res2.value.size == 4");
    PROVEN_TEST_ASSERT((proven_size_t)res2.value.ptr % 32 == 0, "Testing condition: (proven_size_t)res2.value.ptr % 32 == 0", "Review logic surrounding (proven_size_t)res2.value.ptr % 32 == 0");

    /* 5. Trigger OOM (Out Of Memory) */
    proven_result_mem_mut_t res_oom = proven_arena_alloc(&arena, 200); // Exceeds remaining of 128
    PROVEN_TEST_ASSERT(!PROVEN_IS_OK(res_oom.err), "Testing condition: !PROVEN_IS_OK(res_oom.err)", "Review logic surrounding !PROVEN_IS_OK(res_oom.err)");
    PROVEN_TEST_ASSERT(res_oom.err == PROVEN_ERR_NOMEM, "Testing condition: res_oom.err == PROVEN_ERR_NOMEM", "Review logic surrounding res_oom.err == PROVEN_ERR_NOMEM");
    PROVEN_TEST_ASSERT(res_oom.value.ptr == NULL, "Testing condition: res_oom.value.ptr == NULL", "Review logic surrounding res_oom.value.ptr == NULL");

    /* 6. Verify Reset behavior (No-Free rollback) */
    proven_arena_reset(&arena);
    PROVEN_TEST_ASSERT(arena.offset == 0, "Testing condition: arena.offset == 0", "Review logic surrounding arena.offset == 0");
    
    // Can allocate again up to full size
    proven_result_mem_mut_t res_remalloc = proven_arena_alloc(&arena, 120);
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(res_remalloc.err), "Testing condition: PROVEN_IS_OK(res_remalloc.err)", "Review logic surrounding PROVEN_IS_OK(res_remalloc.err)");

    PROVEN_TEST_INFO("All Phase 4 Tests Passed Successfully!");
    return 0;
}
