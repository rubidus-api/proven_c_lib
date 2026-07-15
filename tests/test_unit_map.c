#include "proven.h"
#include "proven_test.h"
#include "proven/sysio.h"

#include <string.h>

// Tracker for allocation counting
static int scratch_alloc_count = 0;
static int scratch_free_count = 0;
static int map_alloc_count = 0;
static int map_free_count = 0;
static proven_allocator_t global_heap;

static proven_result_mem_mut_t map_alloc(void *ctx, proven_size_t size, proven_size_t align) {
    (void)ctx;
    map_alloc_count++;
    return global_heap.alloc_fn(global_heap.ctx, size, align);
}

static void map_free(void *ctx, void *ptr) {
    (void)ctx;
    map_free_count++;
    global_heap.free_fn(global_heap.ctx, ptr);
}

static proven_result_mem_mut_t map_realloc(void *ctx, void *old_ptr, proven_size_t old_size, proven_size_t new_size, proven_size_t align) {
    (void)ctx;
    return global_heap.realloc_fn(global_heap.ctx, old_ptr, old_size, new_size, align);
}

static proven_allocator_t get_map_tracker_allocator() {
    return (proven_allocator_t){ .ctx = NULL, .alloc_fn = map_alloc, .free_fn = map_free, .realloc_fn = map_realloc };
}

static proven_result_mem_mut_t scratch_alloc(void *ctx, proven_size_t size, proven_size_t align) {
    (void)ctx;
    scratch_alloc_count++;
    return global_heap.alloc_fn(global_heap.ctx, size, align);
}

static void scratch_free(void *ctx, void *ptr) {
    (void)ctx;
    scratch_free_count++;
    global_heap.free_fn(global_heap.ctx, ptr);
}

static proven_result_mem_mut_t scratch_realloc(void *ctx, void *old_ptr, proven_size_t old_size, proven_size_t new_size, proven_size_t align) {
    (void)ctx;
    return global_heap.realloc_fn(global_heap.ctx, old_ptr, old_size, new_size, align);
}

static proven_allocator_t get_scratch_allocator() {
    return (proven_allocator_t){ .ctx = NULL, .alloc_fn = scratch_alloc, .free_fn = scratch_free, .realloc_fn = scratch_realloc };
}

static int fail_mode = 0;
static proven_result_mem_mut_t failing_alloc(void *ctx, proven_size_t size, proven_size_t align) {
    (void)ctx;
    if (fail_mode) {
        return (proven_result_mem_mut_t){ .err = PROVEN_ERR_NOMEM };
    }
    return global_heap.alloc_fn(global_heap.ctx, size, align);
}

static proven_result_mem_mut_t failing_realloc(void *ctx, void *old_ptr, proven_size_t old_size, proven_size_t new_size, proven_size_t align) {
    (void)ctx;
    if (fail_mode) {
        return (proven_result_mem_mut_t){ .err = PROVEN_ERR_NOMEM };
    }
    return global_heap.realloc_fn(global_heap.ctx, old_ptr, old_size, new_size, align);
}

static proven_allocator_t get_failing_allocator() {
    return (proven_allocator_t){ .ctx = NULL, .alloc_fn = failing_alloc, .free_fn = scratch_free, .realloc_fn = failing_realloc };
}

typedef struct {
    int access_level;
    float budget;
} user_info_t;

typedef struct {
    proven_byte_t large_arr[300];
    int id;
} large_struct_t;

int main() {
    PROVEN_TEST_INFO("Running Phase 11 HashMap Key/Value Tests...");

    proven_allocator_t heap = proven_heap_allocator();
    global_heap = heap;

    // 1. Integer Key Setup & Core Traversal
    PROVEN_TEST_INFO("Testing Integer Key Setup & Core Traversal...");
    proven_result_map_t res_int = PROVEN_MAP_INIT_INT(heap, user_info_t, 2);
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(res_int.err), "Testing condition: PROVEN_IS_OK(res_int.err)", "Review logic surrounding PROVEN_IS_OK(res_int.err)");
    proven_map_t map_int = res_int.value;

    // Verify power-of-2 alignment (passing 2 initially falls back to floor threshold cap limit of 8!)
    PROVEN_TEST_ASSERT(map_int.cap == 8, "Testing condition: map_int.cap == 8", "Review logic surrounding map_int.cap == 8");
    
    user_info_t u1 = { .access_level = 99, .budget = 1000.5f };
    user_info_t u2 = { .access_level = 10, .budget = 0.0f };
    
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(PROVEN_MAP_SET_INT(&map_int, 404, user_info_t, u1)), "Testing condition: PROVEN_IS_OK(PROVEN_MAP_SET_INT(&map_int, 404, user_info_t, u1))", "Review logic surrounding PROVEN_IS_OK(PROVEN_MAP_SET_INT(&map_int, 404, user_info_t, u1))");
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(PROVEN_MAP_SET_INT(&map_int, 200, user_info_t, u2)), "Testing condition: PROVEN_IS_OK(PROVEN_MAP_SET_INT(&map_int, 200, user_info_t, u2))", "Review logic surrounding PROVEN_IS_OK(PROVEN_MAP_SET_INT(&map_int, 200, user_info_t, u2))");
    PROVEN_TEST_ASSERT(map_int.len == 2, "Testing condition: map_int.len == 2", "Review logic surrounding map_int.len == 2");

    // Get checks ensuring retrieval
    const user_info_t *get_u1 = PROVEN_MAP_GET_INT(&map_int, user_info_t, 404);
    PROVEN_TEST_ASSERT(get_u1 != NULL && get_u1->access_level == 99, "Testing condition: get_u1 != NULL && get_u1->access_level == 99", "Review logic surrounding get_u1 != NULL && get_u1->access_level == 99");

    // Overwrite test (Updates existing value exactly at index, no length growth!)
    u1.budget = 9999.0f;
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(PROVEN_MAP_SET_INT(&map_int, 404, user_info_t, u1)), "Testing condition: PROVEN_IS_OK(PROVEN_MAP_SET_INT(&map_int, 404, user_info_t, u1))", "Review logic surrounding PROVEN_IS_OK(PROVEN_MAP_SET_INT(&map_int, 404, user_info_t, u1))");
    PROVEN_TEST_ASSERT(map_int.len == 2, "Testing condition: map_int.len == 2", "Review logic surrounding map_int.len == 2");
    get_u1 = PROVEN_MAP_GET_INT(&map_int, user_info_t, 404);
    PROVEN_TEST_ASSERT(get_u1->budget == 9999.0f, "Testing condition: get_u1->budget == 9999.0f", "Review logic surrounding get_u1->budget == 9999.0f");

    // Deletion and Tombstones logical check validation
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(PROVEN_MAP_REMOVE_INT(&map_int, 200)), "Testing condition: PROVEN_IS_OK(PROVEN_MAP_REMOVE_INT(&map_int, 200))", "Review logic surrounding PROVEN_IS_OK(PROVEN_MAP_REMOVE_INT(&map_int, 200))");
    PROVEN_TEST_ASSERT(map_int.len == 1, "Testing condition: map_int.len == 1", "Review logic surrounding map_int.len == 1");
    PROVEN_TEST_ASSERT(PROVEN_MAP_GET_INT(&map_int, user_info_t, 200) == NULL, "Testing condition: PROVEN_MAP_GET_INT(&map_int, user_info_t, 200) == NULL", "Review logic surrounding PROVEN_MAP_GET_INT(&map_int, user_info_t, 200) == NULL");

    PROVEN_MAP_DESTROY(&map_int);

    // 2. U8 String Key Architecture & Expansion
    PROVEN_TEST_INFO("Testing U8 String Key Architecture & Expansion...");
    proven_result_map_t res_str = PROVEN_MAP_INIT_U8_BORROWED(heap, int, 4); // Caches into 8 capacity minimum initially
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(res_str.err), "Testing condition: PROVEN_IS_OK(res_str.err)", "Review logic surrounding PROVEN_IS_OK(res_str.err)");
    proven_map_t map_str = res_str.value;

    // Simulate aggressive loads injecting > 6 strings forcing dynamic rehashing boundary growths!
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(PROVEN_MAP_SET_U8_BORROWED(&map_str, PROVEN_LIT("alpha"), int, 1)), "Testing condition: PROVEN_IS_OK(PROVEN_MAP_SET_U8_BORROWED(&map_str, PROVEN_LIT('alpha'), int, 1))", "Review logic surrounding PROVEN_IS_OK(PROVEN_MAP_SET_U8_BORROWED(&map_str, PROVEN_LIT('alpha'), int, 1))");
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(PROVEN_MAP_SET_U8_BORROWED(&map_str, PROVEN_LIT("beta"), int, 2)), "Testing condition: PROVEN_IS_OK(PROVEN_MAP_SET_U8_BORROWED(&map_str, PROVEN_LIT('beta'), int, 2))", "Review logic surrounding PROVEN_IS_OK(PROVEN_MAP_SET_U8_BORROWED(&map_str, PROVEN_LIT('beta'), int, 2))");
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(PROVEN_MAP_SET_U8_BORROWED(&map_str, PROVEN_LIT("gamma"), int, 3)), "Testing condition: PROVEN_IS_OK(PROVEN_MAP_SET_U8_BORROWED(&map_str, PROVEN_LIT('gamma'), int, 3))", "Review logic surrounding PROVEN_IS_OK(PROVEN_MAP_SET_U8_BORROWED(&map_str, PROVEN_LIT('gamma'), int, 3))");
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(PROVEN_MAP_SET_U8_BORROWED(&map_str, PROVEN_LIT("delta"), int, 4)), "Testing condition: PROVEN_IS_OK(PROVEN_MAP_SET_U8_BORROWED(&map_str, PROVEN_LIT('delta'), int, 4))", "Review logic surrounding PROVEN_IS_OK(PROVEN_MAP_SET_U8_BORROWED(&map_str, PROVEN_LIT('delta'), int, 4))");
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(PROVEN_MAP_SET_U8_BORROWED(&map_str, PROVEN_LIT("epsilon"), int, 5)), "Testing condition: PROVEN_IS_OK(PROVEN_MAP_SET_U8_BORROWED(&map_str, PROVEN_LIT('epsilon'), int, 5))", "Review logic surrounding PROVEN_IS_OK(PROVEN_MAP_SET_U8_BORROWED(&map_str, PROVEN_LIT('epsilon'), int, 5))");
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(PROVEN_MAP_SET_U8_BORROWED(&map_str, PROVEN_LIT("zeta"), int, 6)), "Testing condition: PROVEN_IS_OK(PROVEN_MAP_SET_U8_BORROWED(&map_str, PROVEN_LIT('zeta'), int, 6))", "Review logic surrounding PROVEN_IS_OK(PROVEN_MAP_SET_U8_BORROWED(&map_str, PROVEN_LIT('zeta'), int, 6))");
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(PROVEN_MAP_SET_U8_BORROWED(&map_str, PROVEN_LIT("eta"), int, 7)), "Testing condition: PROVEN_IS_OK(PROVEN_MAP_SET_U8_BORROWED(&map_str, PROVEN_LIT('eta'), int, 7))", "Review logic surrounding PROVEN_IS_OK(PROVEN_MAP_SET_U8_BORROWED(&map_str, PROVEN_LIT('eta'), int, 7))"); // Triggers rehash! (7 > 8 * 0.75)

    PROVEN_TEST_ASSERT(map_str.cap == 16, "Testing condition: map_str.cap == 16", "Review logic surrounding map_str.cap == 16"); // Re-allocation proven! 8 -> 16 bounds triggered safely scaling values
    PROVEN_TEST_ASSERT(map_str.len == 7, "Testing condition: map_str.len == 7", "Review logic surrounding map_str.len == 7");
    
    // Verify values navigated safely during migration preserving Open Address indexing locations newly hashed!
    const int *val_gamma = PROVEN_MAP_GET_U8_BORROWED(&map_str, int, PROVEN_LIT("gamma"));
    PROVEN_TEST_ASSERT(val_gamma && *val_gamma == 3, "Testing condition: val_gamma && *val_gamma == 3", "Review logic surrounding val_gamma && *val_gamma == 3");
    
    const int *val_missing = PROVEN_MAP_GET_U8_BORROWED(&map_str, int, PROVEN_LIT("omega"));
    PROVEN_TEST_ASSERT(val_missing == NULL, "Testing condition: val_missing == NULL", "Review logic surrounding val_missing == NULL");

    PROVEN_MAP_DESTROY(&map_str);

    // 3. Scratch Allocator and Safe Rehash Tests
    PROVEN_TEST_INFO("Testing Scratch Allocator and Safe Rehash...");
    {
        map_alloc_count = 0;
        map_free_count = 0;
        
        // Use failing allocator to verify that updates to existing keys do not allocate
        proven_result_map_t res_fail = PROVEN_MAP_INIT_INT(get_failing_allocator(), large_struct_t, 2);
        PROVEN_TEST_ASSERT(PROVEN_IS_OK(res_fail.err), "Failing allocator map creation", "");
        proven_map_t map_fail = res_fail.value;

        large_struct_t big_val = { .id = 1 };
        PROVEN_TEST_ASSERT(PROVEN_IS_OK(PROVEN_MAP_SET_INT(&map_fail, 100, large_struct_t, big_val)), "Initial insert", "");

        // Turn on failing mode
        fail_mode = 1;
        
        // Existing key update MUST succeed and not invoke allocator (O(1) in-place update)
        large_struct_t big_val_new = { .id = 99 };
        PROVEN_TEST_ASSERT(PROVEN_IS_OK(PROVEN_MAP_SET_INT(&map_fail, 100, large_struct_t, big_val_new)), "Existing update under failing alloc", "");
        const large_struct_t *v_f = PROVEN_MAP_GET_INT(&map_fail, large_struct_t, 100);
        PROVEN_TEST_ASSERT(v_f && v_f->id == 99, "Existing key updated under failing allocator", "");

        // Set failure off
        fail_mode = 0;
        PROVEN_MAP_DESTROY(&map_fail);

        // Core tracker validation for rehash logic
        map_alloc_count = 0;
        map_free_count = 0;
        proven_result_map_t res_large = PROVEN_MAP_INIT_INT(get_map_tracker_allocator(), large_struct_t, 2);
        PROVEN_TEST_ASSERT(PROVEN_IS_OK(res_large.err), "Large map creation", "");
        proven_map_t map_large = res_large.value;
        PROVEN_TEST_ASSERT(map_alloc_count == 1, "Initial bucket allocation", "");

        big_val = (large_struct_t){ .id = 1 };
        PROVEN_TEST_ASSERT(PROVEN_IS_OK(PROVEN_MAP_SET_INT(&map_large, 100, large_struct_t, big_val)), "Initial insert", "");

        // Turn on failing mode simulating threshold limit
        // We know load threshold logic might reject new allocs
        fail_mode = 1;
        
        // Existing key update MUST succeed and not invoke allocator
        big_val_new = (large_struct_t){ .id = 99 };
        PROVEN_TEST_ASSERT(PROVEN_IS_OK(PROVEN_MAP_SET_INT(&map_large, 100, large_struct_t, big_val_new)), "Existing update under failing alloc", "");
        const large_struct_t *v = PROVEN_MAP_GET_INT(&map_large, large_struct_t, 100);
        PROVEN_TEST_ASSERT(v && v->id == 99, "Existing key updated", "");

        // Set failure off for next tests
        fail_mode = 0;

        // Force a large self-payload rehash by using the scratch wrapper directly
        // Currently map has 1 element, cap is 8. Insert until threshold (6).
        for (int i = 2; i <= 6; i++) {
            large_struct_t val = { .id = i };
            proven_err_t res = PROVEN_MAP_SET_INT(&map_large, 100 + i, large_struct_t, val);
            PROVEN_TEST_ASSERT(PROVEN_IS_OK(res), "Insert for rehash prep", "");
        }
        
        // Now map has 6 elements, threshold is 6. Next insert triggers rehash.
        // We will do a self-aliased insert using `scratch_allocator`.
        large_struct_t *aliased_ptr = PROVEN_MAP_GET_MUT_INT(&map_large, large_struct_t, 100);
        aliased_ptr->id = 777; // Mutate value in-place
        
        scratch_alloc_count = 0;
        scratch_free_count = 0;
        int prev_map_allocs = map_alloc_count;

        proven_err_t scratch_err = proven_map_set_with_scratch(&map_large, (proven_map_key_t){.id = 999}, aliased_ptr, get_scratch_allocator());
        PROVEN_TEST_ASSERT(PROVEN_IS_OK(scratch_err), "Scratch rehash array", "");
        
        // Assert the scratch allocator was invoked due to size > 256 and aliasing
        PROVEN_TEST_ASSERT(scratch_alloc_count == 1, "Scratch allocator used once for temp buffer", "");
        PROVEN_TEST_ASSERT(scratch_free_count == 1, "Scratch buffer freed correctly", "");
        
        // Map allocator must have rehashed
        PROVEN_TEST_ASSERT(map_alloc_count > prev_map_allocs, "Map allocator rehashed", "");
        
        const large_struct_t *v2 = PROVEN_MAP_GET_INT(&map_large, large_struct_t, 999);
        PROVEN_TEST_ASSERT(v2 && v2->id == 777, "Self-aliased large pointer rehashed safely", "");

        PROVEN_MAP_DESTROY(&map_large);
        PROVEN_TEST_ASSERT(map_free_count > 0, "Map buckets freed", "");
    }

    PROVEN_TEST_PASS("All Phase 11 HashMap Collection Tests Passed Successfully!");
    return 0;
}
