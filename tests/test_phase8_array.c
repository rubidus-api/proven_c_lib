#include "proven.h"
#include "proven_test.h"
#include "proven/sysio.h"

#include <stdalign.h>


typedef struct {
    int id;
    float score;
} test_player_t;

int main() {
    PROVEN_TEST_INFO("Running Phase 8 Dynamic Array Tests...");

    proven_allocator_t heap = proven_heap_allocator();

    // ============================================
    // 1. Array Creation & Push/Pop
    // ============================================
    proven_result_array_t res = PROVEN_ARRAY_INIT(heap, test_player_t, 2);
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(res.err), "Testing condition: PROVEN_IS_OK(res.err)", "Review logic surrounding PROVEN_IS_OK(res.err)");
    proven_array_t arr = res.value;

    PROVEN_TEST_ASSERT(arr.len == 0, "Testing condition: arr.len == 0", "Review logic surrounding arr.len == 0");
    PROVEN_TEST_ASSERT(arr.internal.size >= 2 * sizeof(test_player_t), "Testing condition: arr.internal.size >= 2 * sizeof(test_player_t)", "Review logic surrounding arr.internal.size >= 2 * sizeof(test_player_t)");
    PROVEN_TEST_ASSERT(arr.elem_size == sizeof(test_player_t), "Testing condition: arr.elem_size == sizeof(test_player_t)", "Review logic surrounding arr.elem_size == sizeof(test_player_t)");

    // Type-safe macro push
    test_player_t p1 = { .id = 1, .score = 10.5f };
    test_player_t p2 = { .id = 2, .score = 22.0f };
    
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&arr, test_player_t, p1)), "Testing condition: PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&arr, test_player_t, p1))", "Review logic surrounding PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&arr, test_player_t, p1))");
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&arr, test_player_t, p2)), "Testing condition: PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&arr, test_player_t, p2))", "Review logic surrounding PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&arr, test_player_t, p2))");
    PROVEN_TEST_ASSERT(arr.len == 2, "Testing condition: arr.len == 2", "Review logic surrounding arr.len == 2");

    // ============================================
    // 2. Growth Behavior (Re-allocation Strategy)
    // ============================================
    void* old_ptr = arr.internal.ptr;
    
    test_player_t p3 = { .id = 3, .score = 50.0f };
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&arr, test_player_t, p3)), "Testing condition: PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&arr, test_player_t, p3))", "Review logic surrounding PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&arr, test_player_t, p3))"); // Triggers capacity expansion!

    PROVEN_TEST_ASSERT(arr.len == 3, "Testing condition: arr.len == 3", "Review logic surrounding arr.len == 3");
    PROVEN_TEST_ASSERT(arr.internal.size >= 4 * sizeof(test_player_t), "Testing condition: arr.internal.size >= 4 * sizeof(test_player_t)", "Review logic surrounding arr.internal.size >= 4 * sizeof(test_player_t)"); // Expanded capacity
    PROVEN_TEST_ASSERT(arr.internal.ptr != old_ptr, "Testing condition: arr.internal.ptr != old_ptr", "Review logic surrounding arr.internal.ptr != old_ptr"); // Pointer MUST have changed via re-alloc

    // Verify pristine data state after migration
    test_player_t *migrated_p1 = PROVEN_ARRAY_GET(&arr, test_player_t, 0);
    PROVEN_TEST_ASSERT(migrated_p1->id == 1, "Testing condition: migrated_p1->id == 1", "Review logic surrounding migrated_p1->id == 1");
    test_player_t *migrated_p3 = PROVEN_ARRAY_GET(&arr, test_player_t, 2);
    PROVEN_TEST_ASSERT(migrated_p3->id == 3, "Testing condition: migrated_p3->id == 3", "Review logic surrounding migrated_p3->id == 3");
    PROVEN_TEST_ASSERT(migrated_p3->score == 50.0f, "Testing condition: migrated_p3->score == 50.0f", "Review logic surrounding migrated_p3->score == 50.0f");

    // ============================================
    // 3. Popping and Bound Guards
    // ============================================
    test_player_t pop_result;
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(PROVEN_ARRAY_POP(&arr, test_player_t, &pop_result)), "Testing condition: PROVEN_IS_OK(PROVEN_ARRAY_POP(&arr, test_player_t, &pop_result))", "Review logic surrounding PROVEN_IS_OK(PROVEN_ARRAY_POP(&arr, test_player_t, &pop_result))");
    PROVEN_TEST_ASSERT(pop_result.id == 3, "Testing condition: pop_result.id == 3", "Review logic surrounding pop_result.id == 3");
    PROVEN_TEST_ASSERT(arr.len == 2, "Testing condition: arr.len == 2", "Review logic surrounding arr.len == 2");
    
    // Bounds guard null tests
    PROVEN_TEST_ASSERT(PROVEN_ARRAY_GET(&arr, test_player_t, 5) == NULL, "Testing condition: PROVEN_ARRAY_GET(&arr, test_player_t, 5) == NULL", "Review logic surrounding PROVEN_ARRAY_GET(&arr, test_player_t, 5) == NULL");

    PROVEN_ARRAY_DESTROY(&arr);

    // ============================================
    // 4. Zero-Overhead Arena Array Integration
    // ============================================
    // Embodying Best Practices - Stack-based Arena logic integration
    alignas(max_align_t) proven_byte_t stack_mem[512]; 
    proven_mem_mut_t back = { .ptr = stack_mem, .size = sizeof(stack_mem) };
    proven_arena_t arena = proven_arena_create(back);
    proven_allocator_t arena_alloc = proven_arena_as_allocator(&arena);

    proven_result_array_t a_res = PROVEN_ARRAY_INIT(arena_alloc, int, 2);
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(a_res.err), "Testing condition: PROVEN_IS_OK(a_res.err)", "Review logic surrounding PROVEN_IS_OK(a_res.err)");
    proven_array_t int_arr = a_res.value;
    
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&int_arr, int, 100)), "Testing condition: PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&int_arr, int, 100))", "Review logic surrounding PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&int_arr, int, 100))");
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&int_arr, int, 200)), "Testing condition: PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&int_arr, int, 200))", "Review logic surrounding PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&int_arr, int, 200))");
    
    void* initial_arena_ptr = int_arr.internal.ptr;
    // Push 3rd triggering Arena Growth
    // With highly optimized zero-copy traits, this expansion occurs perfectly transparently without migrations!
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&int_arr, int, 300)), "Testing condition: PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&int_arr, int, 300))", "Review logic surrounding PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&int_arr, int, 300))");
    
    PROVEN_TEST_ASSERT(int_arr.internal.ptr == initial_arena_ptr, "Testing condition: int_arr.internal.ptr == initial_arena_ptr", "Review logic surrounding int_arr.internal.ptr == initial_arena_ptr"); // CONFIRM ZERO-COPY EXTENSION! The pointer did not move!
    PROVEN_TEST_ASSERT(*PROVEN_ARRAY_GET(&int_arr, int, 0) == 100, "Testing condition: *PROVEN_ARRAY_GET(&int_arr, int, 0) == 100", "Review logic surrounding *PROVEN_ARRAY_GET(&int_arr, int, 0) == 100");
    PROVEN_TEST_ASSERT(*PROVEN_ARRAY_GET(&int_arr, int, 1) == 200, "Testing condition: *PROVEN_ARRAY_GET(&int_arr, int, 1) == 200", "Review logic surrounding *PROVEN_ARRAY_GET(&int_arr, int, 1) == 200");
    PROVEN_TEST_ASSERT(*PROVEN_ARRAY_GET(&int_arr, int, 2) == 300, "Testing condition: *PROVEN_ARRAY_GET(&int_arr, int, 2) == 300", "Review logic surrounding *PROVEN_ARRAY_GET(&int_arr, int, 2) == 300");
    
    // Test what happens when another unconnected allocation interrupts the tail!
    (void)proven_arena_alloc_aligned(&arena, 16, 8); // Claim space!
    
    // Array pushes 4th. (Still has capacity, 4 <= 4)
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&int_arr, int, 400)), "Testing condition: PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&int_arr, int, 400))", "Review logic surrounding PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&int_arr, int, 400))"); 
    PROVEN_TEST_ASSERT(int_arr.internal.ptr == initial_arena_ptr, "Testing condition: int_arr.internal.ptr == initial_arena_ptr", "Review logic surrounding int_arr.internal.ptr == initial_arena_ptr"); 

    // Array pushes 5th. This TRIGGERS expansion (5 > 4).
    // Since we interrupted the tail at line 94, this MUST move now safely avoiding overlap!
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&int_arr, int, 500)), "Testing condition: PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&int_arr, int, 500))", "Review logic surrounding PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&int_arr, int, 500))");
    PROVEN_TEST_ASSERT(int_arr.internal.ptr != initial_arena_ptr, "Testing condition: int_arr.internal.ptr != initial_arena_ptr", "Review logic surrounding int_arr.internal.ptr != initial_arena_ptr"); 
    PROVEN_TEST_ASSERT(*PROVEN_ARRAY_GET(&int_arr, int, 4) == 500, "Testing condition: *PROVEN_ARRAY_GET(&int_arr, int, 4) == 500", "Review logic surrounding *PROVEN_ARRAY_GET(&int_arr, int, 4) == 500");

    // And here is the magic trait. Calling destroy invokes arena's No-Op free! 
    PROVEN_ARRAY_DESTROY(&int_arr); 
    
    PROVEN_TEST_INFO("All Phase 8 Dynamic Array Tests Passed Successfully!");
    return 0;
}
