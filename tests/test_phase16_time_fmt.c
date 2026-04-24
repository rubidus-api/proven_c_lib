#include "proven/sysio.h"

#include "proven.h"
#include "proven_test.h"
#include "proven/time.h"
#include "proven/fmt.h"

int main() {
    PROVEN_TEST_INFO("Running Phase 16 Time & Format Tests...");

    // 1. Time Test
    {
        proven_time_t start = proven_time_now();
        proven_time_sleep(100);
        proven_time_t end = proven_time_now();
        
        proven_i64 diff_ns = end - start;
        PROVEN_TEST_INFO("Slept for ~100ms. Measured diff: {} ns", PROVEN_ARG((proven_i32)diff_ns));
        PROVEN_TEST_ASSERT(diff_ns >= 90000000LL, "Testing condition: diff_ns >= 90000000LL", "Review logic surrounding diff_ns >= 90000000LL"); // Should be at least 90ms in ns
    }

    // 2. Modern Format Test
    {
        proven_byte_t backing[4096];
        proven_arena_t arena = proven_arena_create((proven_mem_mut_t){backing, sizeof(backing)});
        proven_allocator_t alloc = proven_arena_as_allocator(&arena);
        
        proven_u8str_t str = {0};
        const char *name = "Proven";
        int version = 16;
        unsigned int hex = 0xDEADBEEF;

        // Modern Syntax: {} for auto-index, {N} for explicit index, :spec for formatting
        PROVEN_TEST_ASSERT(PROVEN_IS_OK(proven_u8str_append_fmt(alloc, &str, 
            "Hello {} v{}! Hex: {:x}, Again: {0}", 
            PROVEN_ARG(name), PROVEN_ARG(version), PROVEN_ARG(hex))), "Format string parsing", "Check proven_u8str_append_fmt");
        
        PROVEN_TEST_INFO("Modern Format Result: {}", PROVEN_ARG((const char*)str.internal.ptr));
        
        // Simple verification
        PROVEN_TEST_ASSERT(str.internal.len > 0, "Testing condition: str.internal.len > 0", "Review logic surrounding str.internal.len > 0");
        
        proven_arena_destroy(&arena);
    }

    // 3. Datetime Test (Modern)
    {
        proven_byte_t backing[4096];
        proven_arena_t arena = proven_arena_create((proven_mem_mut_t){backing, sizeof(backing)});
        proven_allocator_t alloc = proven_arena_as_allocator(&arena);
        
        proven_datetime_t dt = proven_time_now_datetime();
        proven_u8str_t str = {0};
        
        // Smart Rendering: Pass an object directly!
        PROVEN_TEST_ASSERT(PROVEN_IS_OK(proven_u8str_append_fmt(alloc, &str, "Current Time: {}", PROVEN_ARG(dt))), "Testing condition: PROVEN_IS_OK(proven_u8str_append_fmt(alloc, &str, 'Current Time: {}', PROVEN_ARG(dt)))", "Review logic surrounding PROVEN_IS_OK(proven_u8str_append_fmt(alloc, &str, 'Current Time: {}', PROVEN_ARG(dt)))");
        
        PROVEN_TEST_INFO("Formatted Datetime (Smart): {}", PROVEN_ARG((const char*)str.internal.ptr));
        
        proven_arena_destroy(&arena);
    }

    // 4. Escape Braces Test
    {
        proven_byte_t backing[4096];
        proven_arena_t arena = proven_arena_create((proven_mem_mut_t){backing, sizeof(backing)});
        proven_allocator_t alloc = proven_arena_as_allocator(&arena);
        
        proven_u8str_t str = {0};
        
        // Print literal { and }
        PROVEN_TEST_ASSERT(PROVEN_IS_OK(proven_u8str_append_fmt(alloc, &str, "Braces: {{ and }} and {}", PROVEN_ARG("value"))), "Testing condition: PROVEN_IS_OK(proven_u8str_append_fmt(alloc, &str, 'Braces: {{ and }} and {}', PROVEN_ARG('value')))", "Review logic surrounding PROVEN_IS_OK(proven_u8str_append_fmt(alloc, &str, 'Braces: {{ and }} and {}', PROVEN_ARG('value')))");
        
        PROVEN_TEST_INFO("Escape Result: {}", PROVEN_ARG((const char*)str.internal.ptr));
        // Expected: Braces: { and } and value
        
        proven_arena_destroy(&arena);
    }

    // 5. Zig-style Alignment Test
    {
        proven_byte_t backing[4096];
        proven_arena_t arena = proven_arena_create((proven_mem_mut_t){backing, sizeof(backing)});
        proven_allocator_t alloc = proven_arena_as_allocator(&arena);
        
        proven_u8str_t str = {0};
        
        // 1. Right align with zero: {:0>5}
        PROVEN_TEST_ASSERT(PROVEN_IS_OK(proven_u8str_append_fmt(alloc, &str, "Zero: {:0>5}\n", PROVEN_ARG(42))), "Testing condition: PROVEN_IS_OK(proven_u8str_append_fmt(alloc, &str, 'Zero: {:0>5}\n', PROVEN_ARG(42)))", "Review logic surrounding PROVEN_IS_OK(proven_u8str_append_fmt(alloc, &str, 'Zero: {:0>5}\n', PROVEN_ARG(42)))");
        
        // 2. Center align with star: {:*^10}
        PROVEN_TEST_ASSERT(PROVEN_IS_OK(proven_u8str_append_fmt(alloc, &str, "Stars: {:*^10}\n", PROVEN_ARG("Proven"))), "Testing condition: PROVEN_IS_OK(proven_u8str_append_fmt(alloc, &str, 'Stars: {:*^10}\n', PROVEN_ARG('Proven')))", "Review logic surrounding PROVEN_IS_OK(proven_u8str_append_fmt(alloc, &str, 'Stars: {:*^10}\n', PROVEN_ARG('Proven')))");
        
        // 3. Left align with dots: {:.<10}
        PROVEN_TEST_ASSERT(PROVEN_IS_OK(proven_u8str_append_fmt(alloc, &str, "Dots: {:.<10}\n", PROVEN_ARG("End"))), "Testing condition: PROVEN_IS_OK(proven_u8str_append_fmt(alloc, &str, 'Dots: {:.<10}\n', PROVEN_ARG('End')))", "Review logic surrounding PROVEN_IS_OK(proven_u8str_append_fmt(alloc, &str, 'Dots: {:.<10}\n', PROVEN_ARG('End')))");

        (void)proven_print("Zig-style Results:\n{}", PROVEN_ARG((const char*)str.internal.ptr));
        
        proven_arena_destroy(&arena);
    }

    // 6. Unsigned Char Support Test
    {
        proven_byte_t backing[4096];
        proven_arena_t arena = proven_arena_create((proven_mem_mut_t){backing, sizeof(backing)});
        proven_allocator_t alloc = proven_arena_as_allocator(&arena);
        
        proven_u8str_t str = {0};
        unsigned char *ubuf = (unsigned char*)"Unsigned Buffer";
        
        PROVEN_TEST_ASSERT(PROVEN_IS_OK(proven_u8str_append_fmt(alloc, &str, "Data: {}", PROVEN_ARG(ubuf))), "Testing condition: PROVEN_IS_OK(proven_u8str_append_fmt(alloc, &str, 'Data: {}', PROVEN_ARG(ubuf)))", "Review logic surrounding PROVEN_IS_OK(proven_u8str_append_fmt(alloc, &str, 'Data: {}', PROVEN_ARG(ubuf)))");
        
        PROVEN_TEST_INFO("Unsigned Char Result: {}", PROVEN_ARG((const char*)str.internal.ptr));
        
        proven_arena_destroy(&arena);
    }

    // 7. Pointer & Function Pointer Test
    {
        proven_byte_t backing[4096];
        proven_arena_t arena = proven_arena_create((proven_mem_mut_t){backing, sizeof(backing)});
        proven_allocator_t alloc = proven_arena_as_allocator(&arena);
        
        proven_u8str_t str = {0};
        void *p = (void*)0xDEADBEEF;
        
        // Normal pointer
        PROVEN_TEST_ASSERT(PROVEN_IS_OK(proven_u8str_append_fmt(alloc, &str, "Ptr: {}\n", PROVEN_ARG(p))), "Testing condition: PROVEN_IS_OK(proven_u8str_append_fmt(alloc, &str, 'Ptr: {}\n', PROVEN_ARG(p)))", "Review logic surrounding PROVEN_IS_OK(proven_u8str_append_fmt(alloc, &str, 'Ptr: {}\n', PROVEN_ARG(p)))");
        
        // Function pointer using the safety macro
        PROVEN_TEST_ASSERT(PROVEN_IS_OK(proven_u8str_append_fmt(alloc, &str, "Func: {}\n", PROVEN_ARG_FN(main))), "Testing condition: PROVEN_IS_OK(proven_u8str_append_fmt(alloc, &str, 'Func: {}\n', PROVEN_ARG_FN(main)))", "Review logic surrounding PROVEN_IS_OK(proven_u8str_append_fmt(alloc, &str, 'Func: {}\n', PROVEN_ARG_FN(main)))");
        
        (void)proven_print("Pointer Results:\n{}", PROVEN_ARG((const char*)str.internal.ptr));
        
        proven_arena_destroy(&arena);
    }

    PROVEN_TEST_INFO("All Phase 16 Time & Format Tests Passed Successfully!");
    return 0;
}
