#include "proven.h"
#include "proven_test.h"

// A dummy result creation function for testing the Result pattern
static proven_result_mem_mut_t fake_alloc(bool fail) {
    if (fail) {
        return (proven_result_mem_mut_t){ .err = PROVEN_ERR_NOMEM, .value = {0} };
    }
    return (proven_result_mem_mut_t){ .err = PROVEN_OK, .value = { .ptr = (proven_byte_t*)0xABCD, .size = 100 } };
}

int main(void) {
    PROVEN_TEST_INFO("Running Foundation Tests...");

    PROVEN_TEST_INFO("[CASE] Error Code Macros");
    PROVEN_TEST_INFO("Verifying basic error code mapping macros...");
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(PROVEN_OK), "PROVEN_IS_OK returns true for PROVEN_OK", "");
    PROVEN_TEST_ASSERT(!PROVEN_IS_OK(PROVEN_ERR_NOMEM), "PROVEN_IS_OK returns false for PROVEN_ERR_NOMEM", "");
    PROVEN_TEST_ASSERT(!proven_is_ok(PROVEN_ERR_INVALID_ARG), "proven_is_ok returns false for PROVEN_ERR_INVALID_ARG", "");

    PROVEN_TEST_INFO("[CASE] Checked Math Macros");
    PROVEN_TEST_INFO("Verifying C23-style checked arithmetic (stdckdint.h mappings)...");
    proven_u32 a_u32 = 0xFFFFFFFF;
    proven_u32 b_u32 = 1;
    proven_u32 res_u32;
    bool overflow;
    
    overflow = PROVEN_CKD_ADD(&res_u32, a_u32, b_u32);
    PROVEN_TEST_ASSERT(overflow, "PROVEN_CKD_ADD detects overflow on addition", "");
    PROVEN_TEST_ASSERT(res_u32 == 0, "PROVEN_CKD_ADD wraps around properly on overflow", "");

    overflow = PROVEN_CKD_ADD(&res_u32, 10, 20);
    PROVEN_TEST_ASSERT(!overflow, "PROVEN_CKD_ADD does not overflow on safe add", "");
    PROVEN_TEST_ASSERT(res_u32 == 30, "PROVEN_CKD_ADD calculates correct sum", "");

    overflow = PROVEN_CKD_SUB(&res_u32, 0, 1);
    PROVEN_TEST_ASSERT(overflow, "PROVEN_CKD_SUB detects underflow on subtraction", "");
    
    overflow = PROVEN_CKD_MUL(&res_u32, 0xFFFF, 0xFFFF);
    PROVEN_TEST_ASSERT(!overflow, "PROVEN_CKD_MUL safe bounds check", "");
    
    overflow = PROVEN_CKD_MUL(&res_u32, 0xFFFFFFFF, 2);
    PROVEN_TEST_ASSERT(overflow, "PROVEN_CKD_MUL overflow bounds check", "");

    PROVEN_TEST_INFO("[CASE] Result Type Handling");
    PROVEN_TEST_INFO("Verifying Value-Return (Result) pattern semantics...");
    proven_result_mem_mut_t res1 = fake_alloc(true);
    PROVEN_TEST_ASSERT(res1.err == PROVEN_ERR_NOMEM, "Result holds correct error code", "");
    PROVEN_TEST_ASSERT(!PROVEN_IS_OK(res1.err), "Result is correctly identified as a failure", "");

    proven_result_mem_mut_t res2 = fake_alloc(false);
    PROVEN_TEST_ASSERT(res2.err == PROVEN_OK, "Result holds correct success code", "");
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(res2.err), "Result is correctly identified as ok", "");
    PROVEN_TEST_ASSERT(res2.value.size == 100, "Result value is intact", "");

    PROVEN_TEST_PASS("All Foundation Tests Passed Successfully!");
    return 0;
}
