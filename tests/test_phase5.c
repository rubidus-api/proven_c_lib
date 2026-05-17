#include "proven.h"
#include "proven_test.h"
#include "proven/sysio.h"

#include <string.h>


int main() {
    PROVEN_TEST_INFO("Running Phase 5 Buffer & U8String Tests...");

    proven_byte_t backing_buf[256];
    proven_mem_mut_t back = { .ptr = backing_buf, .size = sizeof(backing_buf) };
    proven_arena_t arena = proven_arena_create(back);

    // Get the generic allocator interface bound to our Arena
    proven_allocator_t alloc = proven_arena_as_allocator(&arena);

    // 1. Array/Buffer Test
    PROVEN_TEST_INFO("Testing Array/Buffer... ");
    proven_result_buf_t res_buf = proven_buf_create(alloc, 10);
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(res_buf.err), "Testing condition: PROVEN_IS_OK(res_buf.err)", "Review logic surrounding PROVEN_IS_OK(res_buf.err)");
    
    proven_buf_t buf = res_buf.value;
    
    // 2. Testing PROVEN_LIT (Macro resolving string length via literal concatenation check)
    PROVEN_TEST_INFO("Testing PROVEN_LIT macro...");
    proven_u8str_view_t lit1 = PROVEN_LIT("Hello");
    PROVEN_TEST_ASSERT(lit1.size == 5, "Testing condition: lit1.size == 5", "Review logic surrounding lit1.size == 5"); 
    
    proven_mem_view_t raw_lit1 = { .ptr = lit1.ptr, .size = lit1.size };
    proven_err_t err = proven_buf_append(&buf, raw_lit1);
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(err), "Testing condition: PROVEN_IS_OK(err)", "Review logic surrounding PROVEN_IS_OK(err)");

    // 3. Test Buffer Bounds Defense
    PROVEN_TEST_INFO("Testing buffer bounds defense...");
    proven_u8str_view_t lit2 = PROVEN_LIT(" World!");
    proven_mem_view_t raw_lit2 = { .ptr = lit2.ptr, .size = lit2.size };
    err = proven_buf_append(&buf, raw_lit2);
    PROVEN_TEST_ASSERT(err == PROVEN_ERR_OUT_OF_BOUNDS, "Testing condition: err == PROVEN_ERR_OUT_OF_BOUNDS", "Review logic surrounding err == PROVEN_ERR_OUT_OF_BOUNDS");

    // 4. U8Str Tests
    PROVEN_TEST_INFO("Testing U8Str creation and appending...");
    proven_result_u8str_t res_str = proven_u8str_create(alloc, 12);
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(res_str.err), "Testing condition: PROVEN_IS_OK(res_str.err)", "Review logic surrounding PROVEN_IS_OK(res_str.err)");
    proven_u8str_t str = res_str.value;
    
    err = proven_u8str_append(&str, lit1); 
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(err), "Testing condition: PROVEN_IS_OK(err)", "Review logic surrounding PROVEN_IS_OK(err)");
    err = proven_u8str_append(&str, lit2); 
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(err), "Testing condition: PROVEN_IS_OK(err)", "Review logic surrounding PROVEN_IS_OK(err)");
    
    const char *cstr = proven_u8str_as_cstr(&str);
    PROVEN_TEST_ASSERT(strcmp(cstr, "Hello World!") == 0, "Testing condition: strcmp(cstr, 'Hello World!') == 0", "Review logic surrounding strcmp(cstr, 'Hello World!') == 0");

    // 5. Slice View -> Heap allocated C-String Test
    PROVEN_TEST_INFO("Testing slice view to heap-allocated C-string...");
    // Take a slice "Hello" from earlier and allocate explicitly into a safe c-string
    proven_result_cstr_t hello_slice = proven_u8str_view_to_cstr(lit1, alloc);
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(hello_slice.err), "Testing condition: PROVEN_IS_OK(hello_slice.err)", "Review logic surrounding PROVEN_IS_OK(hello_slice.err)");
    // The underlying data is safely terminated at position 5
    PROVEN_TEST_ASSERT(strcmp(hello_slice.value, "Hello") == 0, "Testing condition: strcmp(hello_slice.value, 'Hello') == 0", "Review logic surrounding strcmp(hello_slice.value, 'Hello') == 0");

    // 6. C-String Boundaries & Equality Tests
    PROVEN_TEST_INFO("Testing C-string boundaries and equality...");
    const char *os_env = "EXTERNAL";
    proven_u8str_view_t from_os = proven_u8str_view_from_cstr(os_env);
    PROVEN_TEST_ASSERT(from_os.size == 8, "Testing condition: from_os.size == 8", "Review logic surrounding from_os.size == 8"); 
    
    proven_u8str_view_t eq_test1 = PROVEN_LIT("EXTERNAL");
    PROVEN_TEST_ASSERT(proven_u8str_view_eq(from_os, eq_test1) == 1, "Testing condition: proven_u8str_view_eq(from_os, eq_test1) == 1", "Review logic surrounding proven_u8str_view_eq(from_os, eq_test1) == 1"); 
    
    proven_u8str_view_t eq_test2 = PROVEN_LIT("EXTERN");
    PROVEN_TEST_ASSERT(proven_u8str_view_eq(from_os, eq_test2) == 0, "Testing condition: proven_u8str_view_eq(from_os, eq_test2) == 0", "Review logic surrounding proven_u8str_view_eq(from_os, eq_test2) == 0"); 

    // 7. Buffer Integer Overflow Tests (Parameter Range Evaluation)
    PROVEN_TEST_INFO("Testing buffer integer overflow...");
    proven_buf_t overflow_buf = {0};
    overflow_buf.cap = 100;
    overflow_buf.len = 10;
    
    proven_mem_view_t giant_view;
    giant_view.ptr = (const proven_byte_t*)"fake";
    giant_view.size = (proven_size_t)-5; // Triggers unsigned overflow wrap-around in addition
    
    proven_err_t over_err = proven_buf_append(&overflow_buf, giant_view);
    PROVEN_TEST_ASSERT(over_err == PROVEN_ERR_OUT_OF_BOUNDS, "Testing condition: over_err == PROVEN_ERR_OUT_OF_BOUNDS", "Integer overflow (wrap-around) in append parameters should be correctly detected");

    // 8. U8 String Slice Integer Overflow Tests
    PROVEN_TEST_INFO("Testing U8 string slice integer overflow...");
    proven_u8str_view_t safe_view = PROVEN_LIT("safe_string");
    proven_u8str_view_t overflow_slice = proven_u8str_view_slice(safe_view, 5, (proven_size_t)-10); // Causes index + len wrap-around
    PROVEN_TEST_ASSERT(overflow_slice.size == 6, "Testing condition: overflow_slice.size == 6", "Slice must clamp correctly even when index + len overflows");
    
    // 9. U8 String Replace Integer Overflow Tests
    PROVEN_TEST_INFO("Testing U8 string replace integer overflow...");
    proven_result_u8str_t replace_res = proven_u8str_create_from_view(alloc, safe_view);
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(replace_res.err), "setup replace", "setup replace");
    
    proven_err_t over_replace = proven_u8str_replace_at(&replace_res.value, 5, (proven_size_t)-10, PROVEN_LIT("abc"));
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(over_replace), "Testing condition: PROVEN_IS_OK(over_replace)", "Replace should handle overflowing chunk bounds safely and cap them at the end of the string");
    PROVEN_TEST_ASSERT(strcmp(proven_u8str_as_cstr(&replace_res.value), "safe_abc") == 0, "Testing condition: strcmp(replaced, 'safe_abc') == 0", "Overflowing old_len is clamped, meaning everything from index 5 to end is replaced");
    
    proven_u8str_destroy(alloc, &replace_res.value);

    PROVEN_TEST_PASS("All Phase 5 Tests Passed Successfully!");
    return 0;
}
