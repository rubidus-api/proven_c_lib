#include "proven.h"
#include "proven_test.h"
#include "proven/sysio.h"

#include <string.h>


int main() {
    PROVEN_TEST_INFO("Running Phase 7 U8String Mutation & Parsing Tests...");

    proven_allocator_t alloc = proven_heap_allocator();

    // 1. Array & View Searching
    PROVEN_TEST_INFO("Testing array and view searching...");
    proven_u8str_view_t haystack = PROVEN_LIT("apple.banana.apple.cherry");
    proven_u8str_view_t needle = PROVEN_LIT("apple");
    
    proven_size_t idx1 = proven_u8str_view_find(haystack, 0, needle);
    PROVEN_TEST_ASSERT(idx1 == 0, "Testing condition: idx1 == 0", "Review logic surrounding idx1 == 0");
    
    // Find next (using offset)
    proven_size_t idx2 = proven_u8str_view_find(haystack, idx1 + needle.size, needle);
    PROVEN_TEST_ASSERT(idx2 == 13, "Testing condition: idx2 == 13", "Review logic surrounding idx2 == 13");
    
    // Find non-existent
    proven_size_t idx3 = proven_u8str_view_find(haystack, idx2 + needle.size, needle);
    PROVEN_TEST_ASSERT(idx3 == PROVEN_INDEX_NOT_FOUND, "Testing condition: idx3 == PROVEN_INDEX_NOT_FOUND", "Review logic surrounding idx3 == PROVEN_INDEX_NOT_FOUND");

    // Starts / Ends with
    PROVEN_TEST_ASSERT(proven_u8str_view_starts_with(haystack, PROVEN_LIT("apple")) == 1, "Testing condition: proven_u8str_view_starts_with(haystack, PROVEN_LIT('apple')) == 1", "Review logic surrounding proven_u8str_view_starts_with(haystack, PROVEN_LIT('apple')) == 1");
    PROVEN_TEST_ASSERT(proven_u8str_view_starts_with(haystack, PROVEN_LIT("banana")) == 0, "Testing condition: proven_u8str_view_starts_with(haystack, PROVEN_LIT('banana')) == 0", "Review logic surrounding proven_u8str_view_starts_with(haystack, PROVEN_LIT('banana')) == 0");
    PROVEN_TEST_ASSERT(proven_u8str_view_ends_with(haystack, PROVEN_LIT("cherry")) == 1, "Testing condition: proven_u8str_view_ends_with(haystack, PROVEN_LIT('cherry')) == 1", "Review logic surrounding proven_u8str_view_ends_with(haystack, PROVEN_LIT('cherry')) == 1");
    
    // Slice clamping
    proven_u8str_view_t slice1 = proven_u8str_view_slice(haystack, 6, 6); // "banana"
    PROVEN_TEST_ASSERT(proven_u8str_view_eq(slice1, PROVEN_LIT("banana")) == 1, "Testing condition: proven_u8str_view_eq(slice1, PROVEN_LIT('banana')) == 1", "Review logic surrounding proven_u8str_view_eq(slice1, PROVEN_LIT('banana')) == 1");

    // 2. String Mutation (Replace At, Insert, Remove)
    PROVEN_TEST_INFO("Testing string mutation (replace/insert/remove)...");
    proven_result_u8str_t res_str = proven_u8str_create(alloc, 50); // Generous capacity
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(res_str.err), "Testing condition: PROVEN_IS_OK(res_str.err)", "Review logic surrounding PROVEN_IS_OK(res_str.err)");
    proven_u8str_t str = res_str.value;

    proven_err_t err = proven_u8str_append(&str, PROVEN_LIT("Hello World"));
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(err), "Testing condition: PROVEN_IS_OK(err)", "Review logic surrounding PROVEN_IS_OK(err)");

    // A. Replace exactly same length
    err = proven_u8str_replace_at(&str, 6, 5, PROVEN_LIT("Earth"));
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(err), "Testing condition: PROVEN_IS_OK(err)", "Review logic surrounding PROVEN_IS_OK(err)");
    PROVEN_TEST_ASSERT(strcmp(proven_u8str_as_cstr(&str), "Hello Earth") == 0, "Testing condition: strcmp(proven_u8str_as_cstr(&str), 'Hello Earth') == 0", "Review logic surrounding strcmp(proven_u8str_as_cstr(&str), 'Hello Earth') == 0");

    // B. Shrink replacement
    err = proven_u8str_replace_at(&str, 6, 5, PROVEN_LIT("C"));
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(err), "Testing condition: PROVEN_IS_OK(err)", "Review logic surrounding PROVEN_IS_OK(err)");
    PROVEN_TEST_ASSERT(strcmp(proven_u8str_as_cstr(&str), "Hello C") == 0, "Testing condition: strcmp(proven_u8str_as_cstr(&str), 'Hello C') == 0", "Review logic surrounding strcmp(proven_u8str_as_cstr(&str), 'Hello C') == 0");

    // C. Expand replacement
    err = proven_u8str_replace_at(&str, 6, 1, PROVEN_LIT("Universe!"));
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(err), "Testing condition: PROVEN_IS_OK(err)", "Review logic surrounding PROVEN_IS_OK(err)");
    PROVEN_TEST_ASSERT(strcmp(proven_u8str_as_cstr(&str), "Hello Universe!") == 0, "Testing condition: strcmp(proven_u8str_as_cstr(&str), 'Hello Universe!') == 0", "Review logic surrounding strcmp(proven_u8str_as_cstr(&str), 'Hello Universe!') == 0");

    // D. Insert (replace_at with old_len = 0)
    err = proven_u8str_insert(&str, 5, PROVEN_LIT(" Beautiful"));
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(err), "Testing condition: PROVEN_IS_OK(err)", "Review logic surrounding PROVEN_IS_OK(err)");
    PROVEN_TEST_ASSERT(strcmp(proven_u8str_as_cstr(&str), "Hello Beautiful Universe!") == 0, "Testing condition: strcmp(proven_u8str_as_cstr(&str), 'Hello Beautiful Universe!') == 0", "Review logic surrounding strcmp(proven_u8str_as_cstr(&str), 'Hello Beautiful Universe!') == 0");

    // E. Remove (replace_at with data.size = 0)
    err = proven_u8str_remove(&str, 5, 10);
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(err), "Testing condition: PROVEN_IS_OK(err)", "Review logic surrounding PROVEN_IS_OK(err)");
    PROVEN_TEST_ASSERT(strcmp(proven_u8str_as_cstr(&str), "Hello Universe!") == 0, "Testing condition: strcmp(proven_u8str_as_cstr(&str), 'Hello Universe!') == 0", "Review logic surrounding strcmp(proven_u8str_as_cstr(&str), 'Hello Universe!') == 0");

    // F. Replace First (Search + Replace via offset)
    err = proven_u8str_replace_first(&str, 0, PROVEN_LIT("Universe"), PROVEN_LIT("Moon"));
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(err), "Testing condition: PROVEN_IS_OK(err)", "Review logic surrounding PROVEN_IS_OK(err)");
    PROVEN_TEST_ASSERT(strcmp(proven_u8str_as_cstr(&str), "Hello Moon!") == 0, "Testing condition: strcmp(proven_u8str_as_cstr(&str), 'Hello Moon!') == 0", "Review logic surrounding strcmp(proven_u8str_as_cstr(&str), 'Hello Moon!') == 0");

    // 3. Out Of Bounds Defense Check
    PROVEN_TEST_INFO("Testing out-of-bounds defense...");
    // Test capacity defense during expansion (capacity is 50)
    proven_result_u8str_t small_res = proven_u8str_create(alloc, 10);
    proven_u8str_t small_str = small_res.value;
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(proven_u8str_append(&small_str, PROVEN_LIT("1234567890"))), "Testing condition: PROVEN_IS_OK(proven_u8str_append(&small_str, PROVEN_LIT('1234567890')))", "Review logic surrounding PROVEN_IS_OK(proven_u8str_append(&small_str, PROVEN_LIT('1234567890')))"); // Exactly 10
    
    // Try to append out of bound
    err = proven_u8str_append(&small_str, PROVEN_LIT("X"));
    PROVEN_TEST_ASSERT(err == PROVEN_ERR_OUT_OF_BOUNDS, "Testing condition: err == PROVEN_ERR_OUT_OF_BOUNDS", "Review logic surrounding err == PROVEN_ERR_OUT_OF_BOUNDS");
    
    PROVEN_TEST_ASSERT(strcmp(proven_u8str_as_cstr(&small_str), "1234567890") == 0, "String was preserved from append overflow", "Check if append handles bounds");
    PROVEN_TEST_ASSERT(small_str.internal.len == 10, "Length should be 10", "Check expected length");
    
    // Try to replace with expansion out of bounds on a populated string
    err = proven_u8str_replace_at(&small_str, 0, 5, PROVEN_LIT("123456")); // Needs 11 total
    PROVEN_TEST_ASSERT(err == PROVEN_ERR_OUT_OF_BOUNDS, "Testing condition: err == PROVEN_ERR_OUT_OF_BOUNDS", "Review logic surrounding err == PROVEN_ERR_OUT_OF_BOUNDS");
    
    // String content is PRESERVED on replace_at failure with Best Effort configuration
    PROVEN_TEST_ASSERT(strcmp(proven_u8str_as_cstr(&small_str), "1234567890") == 0, "String preserved on replace overflow", "Check replace overflow");
    PROVEN_TEST_ASSERT(small_str.internal.len == 10, "Length should be 10", "Check expected length");

    proven_u8str_destroy(alloc, &str); // Clean up manual explicit frees for heap based testing
    proven_u8str_destroy(alloc, &small_str);

    // 4. Append Policies Verification
    PROVEN_TEST_INFO("Verifying append policies (atomic vs partial vs growable)...");
    {
        // A. Atomic Fixed-Capacity (append)
        proven_result_u8str_t atomic_res = proven_u8str_create(alloc, 5);
        proven_u8str_t atomic_str = atomic_res.value;
        
        // Attempting to append 10 bytes into 5-byte capacity -> Should fail ATOMICALLY (no change)
        proven_err_t atomic_err = proven_u8str_append(&atomic_str, PROVEN_LIT("1234567890"));
        PROVEN_TEST_ASSERT(atomic_err == PROVEN_ERR_OUT_OF_BOUNDS, "Expected error on overflow", "Check overflow handling");
        PROVEN_TEST_ASSERT(atomic_str.internal.len == 0, "Atomic append should not modify string on failure", "Check atomic failure");
        PROVEN_TEST_ASSERT(strcmp(proven_u8str_as_cstr(&atomic_str), "") == 0, "String should remain empty", "Check content");

        // B. Best-Effort / Truncating (append_partial)
        proven_result_size_t partial_res = proven_u8str_append_partial(&atomic_str, PROVEN_LIT("1234567890"));
        PROVEN_TEST_ASSERT(partial_res.err == PROVEN_ERR_OUT_OF_BOUNDS, "Expected OOB on partial append", "Check partial error");
        PROVEN_TEST_ASSERT(partial_res.value == 5, "Should have written 5 bytes", "Check partial count");
        PROVEN_TEST_ASSERT(strcmp(proven_u8str_as_cstr(&atomic_str), "12345") == 0, "String should contain truncated data", "Check partial content");
        PROVEN_TEST_ASSERT(atomic_str.internal.len == 5, "Length should be 5", "Check length");

        // C. Atomic Growable (append_grow)
        proven_err_t grow_err = proven_u8str_append_grow(alloc, &atomic_str, PROVEN_LIT("67890"));
        PROVEN_TEST_ASSERT(PROVEN_IS_OK(grow_err), "Growable append should succeed", "Check grow success");
        PROVEN_TEST_ASSERT(strcmp(proven_u8str_as_cstr(&atomic_str), "1234567890") == 0, "String should be fully appended", "Check full content");
        
        proven_u8str_destroy(alloc, &atomic_str);
    }

    PROVEN_TEST_PASS("All Phase 7 Tests Passed Successfully!");
    return 0;
}
