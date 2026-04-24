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
    proven_result_buf_t res_buf = proven_buf_create(alloc, 10);
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(res_buf.err), "Testing condition: PROVEN_IS_OK(res_buf.err)", "Review logic surrounding PROVEN_IS_OK(res_buf.err)");
    
    proven_buf_t buf = res_buf.value;
    
    // 2. Testing PROVEN_LIT (Macro resolving string length via literal concatenation check)
    proven_u8str_view_t lit1 = PROVEN_LIT("Hello");
    PROVEN_TEST_ASSERT(lit1.size == 5, "Testing condition: lit1.size == 5", "Review logic surrounding lit1.size == 5"); 
    
    proven_mem_view_t raw_lit1 = { .ptr = lit1.ptr, .size = lit1.size };
    proven_err_t err = proven_buf_append(&buf, raw_lit1);
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(err), "Testing condition: PROVEN_IS_OK(err)", "Review logic surrounding PROVEN_IS_OK(err)");

    // 3. Test Buffer Bounds Defense
    proven_u8str_view_t lit2 = PROVEN_LIT(" World!");
    proven_mem_view_t raw_lit2 = { .ptr = lit2.ptr, .size = lit2.size };
    err = proven_buf_append(&buf, raw_lit2);
    PROVEN_TEST_ASSERT(err == PROVEN_ERR_OUT_OF_BOUNDS, "Testing condition: err == PROVEN_ERR_OUT_OF_BOUNDS", "Review logic surrounding err == PROVEN_ERR_OUT_OF_BOUNDS");

    // ============================================
    // 4. U8Str Tests
    // ============================================
    proven_result_u8str_t res_str = proven_u8str_create(alloc, 12);
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(res_str.err), "Testing condition: PROVEN_IS_OK(res_str.err)", "Review logic surrounding PROVEN_IS_OK(res_str.err)");
    proven_u8str_t str = res_str.value;
    
    err = proven_u8str_append(&str, lit1); 
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(err), "Testing condition: PROVEN_IS_OK(err)", "Review logic surrounding PROVEN_IS_OK(err)");
    err = proven_u8str_append(&str, lit2); 
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(err), "Testing condition: PROVEN_IS_OK(err)", "Review logic surrounding PROVEN_IS_OK(err)");
    
    const char *cstr = proven_u8str_as_cstr(&str);
    PROVEN_TEST_ASSERT(strcmp(cstr, "Hello World!") == 0, "Testing condition: strcmp(cstr, 'Hello World!') == 0", "Review logic surrounding strcmp(cstr, 'Hello World!') == 0");

    // ============================================
    // 5. Slice View -> Heap allocated C-String Test
    // ============================================
    // Take a slice "Hello" from earlier and allocate explicitly into a safe c-string
    proven_result_cstr_t hello_slice = proven_u8str_view_to_cstr(lit1, alloc);
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(hello_slice.err), "Testing condition: PROVEN_IS_OK(hello_slice.err)", "Review logic surrounding PROVEN_IS_OK(hello_slice.err)");
    // The underlying data is safely terminated at position 5
    PROVEN_TEST_ASSERT(strcmp(hello_slice.value, "Hello") == 0, "Testing condition: strcmp(hello_slice.value, 'Hello') == 0", "Review logic surrounding strcmp(hello_slice.value, 'Hello') == 0");

    // ============================================
    // 6. C-String Boundaries & Equality Tests
    // ============================================
    const char *os_env = "EXTERNAL";
    proven_u8str_view_t from_os = proven_u8str_view_from_cstr(os_env);
    PROVEN_TEST_ASSERT(from_os.size == 8, "Testing condition: from_os.size == 8", "Review logic surrounding from_os.size == 8"); 
    
    proven_u8str_view_t eq_test1 = PROVEN_LIT("EXTERNAL");
    PROVEN_TEST_ASSERT(proven_u8str_view_eq(from_os, eq_test1) == 1, "Testing condition: proven_u8str_view_eq(from_os, eq_test1) == 1", "Review logic surrounding proven_u8str_view_eq(from_os, eq_test1) == 1"); 
    
    proven_u8str_view_t eq_test2 = PROVEN_LIT("EXTERN");
    PROVEN_TEST_ASSERT(proven_u8str_view_eq(from_os, eq_test2) == 0, "Testing condition: proven_u8str_view_eq(from_os, eq_test2) == 0", "Review logic surrounding proven_u8str_view_eq(from_os, eq_test2) == 0"); 

    PROVEN_TEST_INFO("All Phase 5 Tests Passed Successfully!");
    return 0;
}
