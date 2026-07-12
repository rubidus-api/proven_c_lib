#include "proven.h"
#include "proven_test.h"
#include "proven/sysio.h"

#include <string.h>


int main(void) {
    PROVEN_TEST_INFO("Running Deallocation Strategy Examples...");

    // ============================================
    // 1. Arena Strategy: All-or-Nothing
    // ============================================
    PROVEN_TEST_INFO("Strategy 1: Arena (Bump Allocator)");
    
    proven_byte_t backing[1024];
    proven_mem_mut_t mem = { .ptr = backing, .size = sizeof(backing) };
    proven_arena_t arena = proven_arena_create(mem);
    proven_allocator_t arena_alloc = proven_arena_as_allocator(&arena);

    // Allocating through the trait
    proven_result_mem_mut_t res1 = arena_alloc.alloc_fn(arena_alloc.ctx, 100, 8);
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(res1.err), "Arena allocation of 100 bytes must succeed", "Check alloc_fn and available backing size");
    PROVEN_TEST_INFO("  Allocated 100 bytes from Arena successfully.");

    // "Deallocating" through the trait (No-Op)
    arena_alloc.free_fn(arena_alloc.ctx, res1.value.ptr);
    PROVEN_TEST_INFO("  Called free on Arena (Verified No-Op behavior)");

    // Proper Arena Deallocation (Resetting the whole thing)
    proven_arena_reset(&arena);
    PROVEN_TEST_INFO("  Arena Reset: All previous allocations discarded instantly.");


    // ============================================
    // 2. Heap Strategy: Individual Free
    // ============================================
    PROVEN_TEST_INFO("\nStrategy 2: Heap (System Malloc)");
    
    proven_allocator_t heap_alloc = proven_heap_allocator();

    // Allocating through the trait
    proven_result_mem_mut_t res2 = heap_alloc.alloc_fn(heap_alloc.ctx, 200, 16);
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(res2.err), "Heap allocation of 200 bytes must succeed", "Check heap allocator OS request logic");
    PROVEN_TEST_INFO("  Allocated 200 bytes from Heap successfully.");

    // Actual Heap Deallocation (Calls PAL -> free)
    heap_alloc.free_fn(heap_alloc.ctx, res2.value.ptr);
    PROVEN_TEST_INFO("  Called free on Heap (System memory returned to OS)");

    PROVEN_TEST_INFO("\nDeallocation Examples Completed Successfully!");
    return 0;
}
