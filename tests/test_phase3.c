#include "proven.h"
#include "proven_test.h"
#include "proven/sysio.h"



/**
 * @file test_phase3.c
 * @brief Unit tests for Phase 3 (Error Handling Primitives).
 */

int main() {
    PROVEN_TEST_INFO("Running Phase 3 Error Handling Tests...");

    /* 1. Test basic error mappings */
    proven_err_t err_ok = PROVEN_OK;
    proven_err_t err_nomem = PROVEN_ERR_NOMEM;

    PROVEN_TEST_ASSERT(PROVEN_IS_OK(err_ok) == 1, "Testing condition: PROVEN_IS_OK(err_ok) == 1", "Review logic surrounding PROVEN_IS_OK(err_ok) == 1");
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(err_nomem) == 0, "Testing condition: PROVEN_IS_OK(err_nomem) == 0", "Review logic surrounding PROVEN_IS_OK(err_nomem) == 0");

    /* 2. Test Result Structure */
    proven_byte_t dummy_data[10] = {0};
    proven_mem_mut_t dummy_mut = { .ptr = dummy_data, .size = 10 };

    proven_result_mem_mut_t res;
    
    // Simulate a successful operation returning a result
    res.err = PROVEN_OK;
    res.value = dummy_mut;

    PROVEN_TEST_ASSERT(PROVEN_IS_OK(res.err) == 1, "Testing condition: PROVEN_IS_OK(res.err) == 1", "Review logic surrounding PROVEN_IS_OK(res.err) == 1");
    PROVEN_TEST_ASSERT(res.value.size == 10, "Testing condition: res.value.size == 10", "Review logic surrounding res.value.size == 10");
    PROVEN_TEST_ASSERT(res.value.ptr == dummy_data, "Testing condition: res.value.ptr == dummy_data", "Review logic surrounding res.value.ptr == dummy_data");

    // Simulate a failed operation
    proven_result_mem_mut_t res_fail;
    res_fail.err = PROVEN_ERR_OUT_OF_BOUNDS;
    res_fail.value = (proven_mem_mut_t){ .ptr = NULL, .size = 0 };

    PROVEN_TEST_ASSERT(PROVEN_IS_OK(res_fail.err) == 0, "Testing condition: PROVEN_IS_OK(res_fail.err) == 0", "Review logic surrounding PROVEN_IS_OK(res_fail.err) == 0");
    PROVEN_TEST_ASSERT(res_fail.value.ptr == NULL, "Testing condition: res_fail.value.ptr == NULL", "Review logic surrounding res_fail.value.ptr == NULL");

    PROVEN_TEST_INFO("All Phase 3 Tests Passed Successfully!");
    return 0;
}
