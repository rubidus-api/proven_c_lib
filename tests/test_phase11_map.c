#include "proven.h"
#include "proven_test.h"
#include "proven/sysio.h"

#include <string.h>


typedef struct {
    int access_level;
    float budget;
} user_info_t;

int main() {
    PROVEN_TEST_INFO("Running Phase 11 HashMap Key/Value Tests...");

    proven_allocator_t heap = proven_heap_allocator();

    // ============================================
    // 1. Integer Key Setup & Core Traversal
    // ============================================
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
    user_info_t *get_u1 = PROVEN_MAP_GET_INT(&map_int, user_info_t, 404);
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

    // ============================================
    // 2. U8 String Key Architecture & Expansion
    // ============================================
    proven_result_map_t res_str = PROVEN_MAP_INIT_U8(heap, int, 4); // Caches into 8 capacity minimum initially
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(res_str.err), "Testing condition: PROVEN_IS_OK(res_str.err)", "Review logic surrounding PROVEN_IS_OK(res_str.err)");
    proven_map_t map_str = res_str.value;

    // Simulate aggressive loads injecting > 6 strings forcing dynamic rehashing boundary growths!
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(PROVEN_MAP_SET_U8(&map_str, PROVEN_LIT("alpha"), int, 1)), "Testing condition: PROVEN_IS_OK(PROVEN_MAP_SET_U8(&map_str, PROVEN_LIT('alpha'), int, 1))", "Review logic surrounding PROVEN_IS_OK(PROVEN_MAP_SET_U8(&map_str, PROVEN_LIT('alpha'), int, 1))");
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(PROVEN_MAP_SET_U8(&map_str, PROVEN_LIT("beta"), int, 2)), "Testing condition: PROVEN_IS_OK(PROVEN_MAP_SET_U8(&map_str, PROVEN_LIT('beta'), int, 2))", "Review logic surrounding PROVEN_IS_OK(PROVEN_MAP_SET_U8(&map_str, PROVEN_LIT('beta'), int, 2))");
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(PROVEN_MAP_SET_U8(&map_str, PROVEN_LIT("gamma"), int, 3)), "Testing condition: PROVEN_IS_OK(PROVEN_MAP_SET_U8(&map_str, PROVEN_LIT('gamma'), int, 3))", "Review logic surrounding PROVEN_IS_OK(PROVEN_MAP_SET_U8(&map_str, PROVEN_LIT('gamma'), int, 3))");
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(PROVEN_MAP_SET_U8(&map_str, PROVEN_LIT("delta"), int, 4)), "Testing condition: PROVEN_IS_OK(PROVEN_MAP_SET_U8(&map_str, PROVEN_LIT('delta'), int, 4))", "Review logic surrounding PROVEN_IS_OK(PROVEN_MAP_SET_U8(&map_str, PROVEN_LIT('delta'), int, 4))");
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(PROVEN_MAP_SET_U8(&map_str, PROVEN_LIT("epsilon"), int, 5)), "Testing condition: PROVEN_IS_OK(PROVEN_MAP_SET_U8(&map_str, PROVEN_LIT('epsilon'), int, 5))", "Review logic surrounding PROVEN_IS_OK(PROVEN_MAP_SET_U8(&map_str, PROVEN_LIT('epsilon'), int, 5))");
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(PROVEN_MAP_SET_U8(&map_str, PROVEN_LIT("zeta"), int, 6)), "Testing condition: PROVEN_IS_OK(PROVEN_MAP_SET_U8(&map_str, PROVEN_LIT('zeta'), int, 6))", "Review logic surrounding PROVEN_IS_OK(PROVEN_MAP_SET_U8(&map_str, PROVEN_LIT('zeta'), int, 6))");
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(PROVEN_MAP_SET_U8(&map_str, PROVEN_LIT("eta"), int, 7)), "Testing condition: PROVEN_IS_OK(PROVEN_MAP_SET_U8(&map_str, PROVEN_LIT('eta'), int, 7))", "Review logic surrounding PROVEN_IS_OK(PROVEN_MAP_SET_U8(&map_str, PROVEN_LIT('eta'), int, 7))"); // Triggers rehash! (7 > 8 * 0.75)

    PROVEN_TEST_ASSERT(map_str.cap == 16, "Testing condition: map_str.cap == 16", "Review logic surrounding map_str.cap == 16"); // Re-allocation proven! 8 -> 16 bounds triggered safely scaling values
    PROVEN_TEST_ASSERT(map_str.len == 7, "Testing condition: map_str.len == 7", "Review logic surrounding map_str.len == 7");
    
    // Verify values navigated safely during migration preserving Open Address indexing locations newly hashed!
    int *val_gamma = PROVEN_MAP_GET_U8(&map_str, int, PROVEN_LIT("gamma"));
    PROVEN_TEST_ASSERT(val_gamma && *val_gamma == 3, "Testing condition: val_gamma && *val_gamma == 3", "Review logic surrounding val_gamma && *val_gamma == 3");
    
    int *val_missing = PROVEN_MAP_GET_U8(&map_str, int, PROVEN_LIT("omega"));
    PROVEN_TEST_ASSERT(val_missing == NULL, "Testing condition: val_missing == NULL", "Review logic surrounding val_missing == NULL");

    PROVEN_MAP_DESTROY(&map_str);

    PROVEN_TEST_INFO("All Phase 11 HashMap Collection Tests Passed Successfully!");
    return 0;
}
