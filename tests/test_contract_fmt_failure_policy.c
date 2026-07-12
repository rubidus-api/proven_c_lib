#include "proven/fmt.h"
#include "proven_test.h"
#include "proven/heap.h"
#include <string.h>

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    PROVEN_TEST_INFO("Phase 22: Formatting Atomic Failure Verification");

    // 1. Atomic Growable formatting (append_fmt_grow)
    {
        proven_result_u8str_t res = proven_u8str_create(alloc, 2);
        proven_u8str_t str = res.value;

        PROVEN_TEST_ASSERT(PROVEN_FMT_IS_OK(proven_u8str_append_fmt_grow(alloc, &str, "Val: {}", PROVEN_ARG(12345))), "Growth format", "Check growth format");
        PROVEN_TEST_ASSERT(strcmp(proven_u8str_as_cstr(&str), "Val: 12345") == 0, "Growth integrity", "Check growth integrity");
        
        proven_u8str_destroy(alloc, &str);
    }

    // 2. Formatting Policies Verification
    {
        proven_result_u8str_t res = proven_u8str_create(alloc, 5); // capacity for 5 + null
        proven_u8str_t str = res.value;

        // Populate initially
        PROVEN_TEST_ASSERT(PROVEN_IS_OK(proven_u8str_append(&str, PROVEN_LIT("ab"))), "Populate string", "Check populate string");

        // A. Atomic Fixed-Capacity (append_fmt)
        // Try formatting "Number: 100" (needs 11 bytes). Starts with "ab" (2 bytes). Total 13 > 5.
        // Should fail ATOMICALLY.
        proven_fmt_result_t fmt_res_fail = proven_u8str_append_fmt(&str, "Number: {}", PROVEN_ARG(100));
        PROVEN_TEST_ASSERT(fmt_res_fail.err == PROVEN_ERR_OUT_OF_BOUNDS, "Expected OOB in fixed format", "Check format overflow");
        PROVEN_TEST_ASSERT(strcmp(proven_u8str_as_cstr(&str), "ab") == 0, "Atomic format should not modify string on failure", "Check atomic failure");

        // B. Best-Effort / Truncating (append_fmt_trunc)
        proven_fmt_result_t fmt_res = proven_u8str_append_fmt_trunc(&str, "Number: {}", PROVEN_ARG(100));
        PROVEN_TEST_ASSERT(fmt_res.err == PROVEN_ERR_OUT_OF_BOUNDS, "Expected OOB in trunc format", "Check trunc overflow");
        PROVEN_TEST_ASSERT(fmt_res.written == 3, "Written 3 chars ('Num')", "Check written count");
        PROVEN_TEST_ASSERT(fmt_res.required == 11, "Required 11 chars", "Check required count");
        
        // Should have "abNum"
        PROVEN_TEST_ASSERT(strcmp(proven_u8str_as_cstr(&str), "abNum") == 0, "Truncated format result", "Check content");

        proven_u8str_destroy(alloc, &str);
    }
    
    // 3. Prevent DoS via maliciously large format padding values
    {
        proven_result_u8str_t res = proven_u8str_create(alloc, 1024);
        proven_u8str_t str = res.value;
        
        // Use an insanely large width inside format bounds
        proven_fmt_result_t fmt_res_dos = proven_u8str_append_fmt(&str, "{:->9999999999}", PROVEN_ARG(100));
        PROVEN_TEST_ASSERT(fmt_res_dos.err == PROVEN_ERR_OUT_OF_BOUNDS, "Should handle extremely large padding specs without overflowing internal width safely", "Check extreme padding");
        // Ensure parsing didn't create a negative integer that bypasses bounds logic or leads to OOM crash.
        
        proven_u8str_destroy(alloc, &str);
    }

    PROVEN_TEST_INFO("Phase 22 Tests Passed!");
    return 0;
}
