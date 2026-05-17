#include "proven/u16str.h"
#include "proven_test.h"
#include "proven/heap.h"
#include <string.h>

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    PROVEN_TEST_INFO("Phase 17: U16 String System Verification");

    // 1. Basic Creation & Destruction
    PROVEN_TEST_INFO("Testing u16 string creation and destruction...");
    {
        proven_result_u16str_t res = proven_u16str_create(alloc, 16);
        PROVEN_TEST_ASSERT(PROVEN_IS_OK(res.err), "U16 string creation", "Check creation logic");
        PROVEN_TEST_ASSERT(proven_u16str_len(&res.value) == 0, "Initial length should be 0", "Check initial string length");
        
        proven_u16str_destroy(alloc, &res.value);
    }

    // 2. Append Policies Verification
    PROVEN_TEST_INFO("Verifying u16 string append policies...");
    {
        proven_result_u16str_t res = proven_u16str_create(alloc, 5); // space for 5 + null
        proven_u16str_t str = res.value;

        // A. Atomic Fixed-Capacity (append)
        PROVEN_TEST_ASSERT(PROVEN_IS_OK(proven_u16str_append(&str, PROVEN_U16_LIT("ABCD"))), "Append ABCD", "Check append result");
        PROVEN_TEST_ASSERT(proven_u16str_len(&str) == 4, "Length should be 4", "Check correct length");

        // Attempt "EFG" (3 units) into remaining 1 unit space -> Should fail ATOMICALLY
        proven_err_t err = proven_u16str_append(&str, PROVEN_U16_LIT("EFG"));
        PROVEN_TEST_ASSERT(err == PROVEN_ERR_OUT_OF_BOUNDS, "Expected overflow", "Verify overflow result");
        PROVEN_TEST_ASSERT(proven_u16str_len(&str) == 4, "Length should remain 4 on atomic failure", "Verify length atomic");

        // B. Best-Effort / Truncating (append_partial)
        proven_result_size_t partial_res = proven_u16str_append_partial(&str, PROVEN_U16_LIT("EFG"));
        PROVEN_TEST_ASSERT(partial_res.err == PROVEN_ERR_OUT_OF_BOUNDS, "Expected OOB on partial append", "Check partial error");
        PROVEN_TEST_ASSERT(partial_res.value == 1, "Should have written 1 unit", "Check partial count");
        PROVEN_TEST_ASSERT(proven_u16str_len(&str) == 5, "Length should be 5 on partial write", "Verify length partial");
        
        const proven_u16 *ptr = proven_u16str_as_ptr(&str);
        PROVEN_TEST_ASSERT(ptr[0] == 'A' && ptr[4] == 'E' && ptr[5] == 0, "Data integrity after partial append", "Check data");

        proven_u16str_destroy(alloc, &str);
    }

    // 3. Allocator Growth (append_grow)
    PROVEN_TEST_INFO("Testing u16 string allocator growth...");
    {
        proven_result_u16str_t res = proven_u16str_create(alloc, 2);
        proven_u16str_t str = res.value;

        PROVEN_TEST_ASSERT(PROVEN_IS_OK(proven_u16str_append_grow(alloc, &str, PROVEN_U16_LIT("Hello World"))), "Growth append", "Check view append");
        PROVEN_TEST_ASSERT(proven_u16str_len(&str) == 11, "Length after growth", "Check new length");
        
        proven_u16str_destroy(alloc, &str);
    }

    PROVEN_TEST_INFO("Phase 17 Tests Passed!");
    return 0;
}
