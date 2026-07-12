#include "proven.h"
#include "proven_test.h"
#include "proven/sysio.h"



/**
 * @file test_unit_arena.c
 * @brief Unit tests for Phase 4 (Arena Allocator).
 */

int main() {
    PROVEN_TEST_INFO("Running Phase 4 Arena Allocator Tests...");

    // 1. Setup a backing buffer simulating system memory (e.g. from stack or mmap)
    PROVEN_TEST_INFO("Setting up backing buffer...");
    proven_byte_t backing_buf[128];
    proven_mem_mut_t backing = { .ptr = backing_buf, .size = sizeof(backing_buf) };

    // 2. Create Arena
    PROVEN_TEST_INFO("Creating arena allocator...");
    proven_arena_t arena = proven_arena_create(backing);
    PROVEN_TEST_ASSERT(arena.offset == 0, "Testing condition: arena.offset == 0", "Review logic surrounding arena.offset == 0");
    PROVEN_TEST_ASSERT(arena.backing.size == 128, "Testing condition: arena.backing.size == 128", "Review logic surrounding arena.backing.size == 128");

    // 3. Successful Allocations
    PROVEN_TEST_INFO("Testing successful arena allocations...");
    proven_result_mem_mut_t res1 = proven_arena_alloc(&arena, 10);
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(res1.err), "Testing condition: PROVEN_IS_OK(res1.err)", "Review logic surrounding PROVEN_IS_OK(res1.err)");
    PROVEN_TEST_ASSERT(res1.value.size == 10, "Testing condition: res1.value.size == 10", "Review logic surrounding res1.value.size == 10");
    // Address must be explicitly 8-byte aligned (default)
    PROVEN_TEST_ASSERT((proven_size_t)res1.value.ptr % PROVEN_DEFAULT_ALIGNMENT == 0, "Testing condition: (proven_size_t)res1.value.ptr % PROVEN_DEFAULT_ALIGNMENT == 0", "Review logic surrounding (proven_size_t)res1.value.ptr % PROVEN_DEFAULT_ALIGNMENT == 0");

    // 4. Force a specific alignment
    PROVEN_TEST_INFO("Testing strict alignment (32-byte boundary)...");
    proven_result_mem_mut_t res2 = proven_arena_alloc_aligned(&arena, 4, 32);
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(res2.err), "Testing condition: PROVEN_IS_OK(res2.err)", "Review logic surrounding PROVEN_IS_OK(res2.err)");
    PROVEN_TEST_ASSERT(res2.value.size == 4, "Testing condition: res2.value.size == 4", "Review logic surrounding res2.value.size == 4");
    PROVEN_TEST_ASSERT((proven_size_t)res2.value.ptr % 32 == 0, "Testing condition: (proven_size_t)res2.value.ptr % 32 == 0", "Review logic surrounding (proven_size_t)res2.value.ptr % 32 == 0");

    // 5. Trigger OOM (Out Of Memory)
    PROVEN_TEST_INFO("Testing out-of-memory defense...");
    proven_result_mem_mut_t res_oom = proven_arena_alloc(&arena, 200); // Exceeds remaining of 128
    PROVEN_TEST_ASSERT(!PROVEN_IS_OK(res_oom.err), "Testing condition: !PROVEN_IS_OK(res_oom.err)", "Review logic surrounding !PROVEN_IS_OK(res_oom.err)");
    PROVEN_TEST_ASSERT(res_oom.err == PROVEN_ERR_NOMEM, "Testing condition: res_oom.err == PROVEN_ERR_NOMEM", "Review logic surrounding res_oom.err == PROVEN_ERR_NOMEM");
    PROVEN_TEST_ASSERT(res_oom.value.ptr == NULL, "Testing condition: res_oom.value.ptr == NULL", "Review logic surrounding res_oom.value.ptr == NULL");

    // 6. Verify Reset behavior (No-Free rollback)
    PROVEN_TEST_INFO("Testing arena reset behavior...");
    proven_arena_reset(&arena);
    PROVEN_TEST_ASSERT(arena.offset == 0, "Testing condition: arena.offset == 0", "Review logic surrounding arena.offset == 0");
    
    // Can allocate again up to full size
    proven_result_mem_mut_t res_remalloc = proven_arena_alloc(&arena, 120);
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(res_remalloc.err), "Testing condition: PROVEN_IS_OK(res_remalloc.err)", "Review logic surrounding PROVEN_IS_OK(res_remalloc.err)");

    // 7. Verify Reallocation Safety & Zero-Copy
    PROVEN_TEST_INFO("Testing reallocation and zero-copy semantics...");
    proven_arena_reset(&arena);
    proven_result_mem_mut_t res_tail = proven_arena_alloc(&arena, 10);
    proven_byte_t *tail_ptr = res_tail.value.ptr;
    
    // In-place grow (should maintain pointer)
    proven_result_mem_mut_t res_realloc = proven_arena_realloc_aligned(&arena, tail_ptr, 10, 20, PROVEN_DEFAULT_ALIGNMENT);
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(res_realloc.err), "Testing condition: PROVEN_IS_OK(res_realloc.err)", "Review logic for in-place realloc");
    PROVEN_TEST_ASSERT(res_realloc.value.ptr == tail_ptr, "Testing condition: res_realloc.value.ptr == tail_ptr", "Pointer should be unchanged for tail allocation");

    // Non-tail reallocation (should allocate new block)
    proven_result_mem_mut_t res_new_tail = proven_arena_alloc(&arena, 15);
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(res_new_tail.err), "Testing condition: PROVEN_IS_OK(res_new_tail.err)", "Allocation must succeed");
    proven_result_mem_mut_t res_non_tail_realloc = proven_arena_realloc_aligned(&arena, tail_ptr, 20, 30, PROVEN_DEFAULT_ALIGNMENT);
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(res_non_tail_realloc.err), "Testing condition: PROVEN_IS_OK(res_non_tail_realloc.err)", "Review logic for non-tail realloc");
    PROVEN_TEST_ASSERT(res_non_tail_realloc.value.ptr != tail_ptr, "Testing condition: res_non_tail_realloc.value.ptr != tail_ptr", "Pointer must change for non-tail allocation");

    // Invalid Reallocation: Pointer outside arena (Stack)
    proven_byte_t external_buf[10];
    proven_result_mem_mut_t res_invalid = proven_arena_realloc_aligned(&arena, external_buf, 5, 15, PROVEN_DEFAULT_ALIGNMENT);
    PROVEN_TEST_ASSERT(res_invalid.err == PROVEN_ERR_INVALID_ARG, "Testing condition: res_invalid.err == PROVEN_ERR_INVALID_ARG", "External pointer must be rejected safely");

    // Invalid Reallocation: Pointer outside arena (Heap)
    proven_allocator_t heap = proven_heap_allocator();
    proven_result_mem_mut_t ext_heap = heap.alloc_fn(heap.ctx, 16, 8);
    if (PROVEN_IS_OK(ext_heap.err)) {
        proven_result_mem_mut_t res_heap_invalid = proven_arena_realloc_aligned(&arena, ext_heap.value.ptr, 16, 32, 8);
        PROVEN_TEST_ASSERT(res_heap_invalid.err == PROVEN_ERR_INVALID_ARG, "Testing condition: res_heap_invalid.err == PROVEN_ERR_INVALID_ARG", "External heap pointer must be rejected");
        heap.free_fn(heap.ctx, ext_heap.value.ptr);
    }

    PROVEN_TEST_PASS("All Phase 4 Tests Passed Successfully!");
    return 0;
}
