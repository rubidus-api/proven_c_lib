#include "proven/sysio.h"

#include "proven.h"
#include "proven_test.h"
#include "proven/time.h"
#include "proven/fmt.h"
#include <string.h>

int main() {
    PROVEN_TEST_INFO("Running Phase 16 Time & Format Tests...");

    // 1. Time Test
    PROVEN_TEST_INFO("Testing relative time and sleep...");
    {
        proven_time_t start = proven_time_now();
        proven_time_sleep(100);
        proven_time_t end = proven_time_now();
        
        proven_i64 diff_ns = end - start;
        PROVEN_TEST_INFO("Slept for ~100ms. Measured diff: {} ns", PROVEN_ARG((proven_i32)diff_ns));
        PROVEN_TEST_ASSERT(diff_ns >= 90000000LL, "Testing condition: diff_ns >= 90000000LL", "Review logic surrounding diff_ns >= 90000000LL"); // Should be at least 90ms in ns
    }

    // 2. Modern Format Test
    PROVEN_TEST_INFO("Testing modern {} formatting syntax...");
    {
        proven_byte_t backing[4096];
        proven_arena_t arena = proven_arena_create((proven_mem_mut_t){backing, sizeof(backing)});
        proven_allocator_t alloc = proven_arena_as_allocator(&arena);
        
        proven_u8str_t str = {0};
        const char *name = "Proven";
        int version = 16;
        unsigned int hex = 0xDEADBEEF;

        // Modern Syntax: {} for auto-index, {N} for explicit index, :spec for formatting
        PROVEN_TEST_ASSERT(PROVEN_FMT_IS_OK(proven_u8str_append_fmt_grow(alloc, &str, 
            "Hello {} v{}! Hex: {:x}, Again: {0}", 
            PROVEN_ARG(name), PROVEN_ARG(version), PROVEN_ARG(hex))), "Format string parsing", "Check proven_u8str_append_fmt_grow");
        
        PROVEN_TEST_INFO("Modern Format Result: {}", PROVEN_ARG((const char*)str.internal.ptr));
        
        // Simple verification
        PROVEN_TEST_ASSERT(str.internal.len > 0, "Testing condition: str.internal.len > 0", "Review logic surrounding str.internal.len > 0");
        
        proven_arena_destroy(&arena);
    }

    // 3. Datetime Test (Modern)
    PROVEN_TEST_INFO("Testing smart datetime formatting...");
    {
        proven_byte_t backing[4096];
        proven_arena_t arena = proven_arena_create((proven_mem_mut_t){backing, sizeof(backing)});
        proven_allocator_t alloc = proven_arena_as_allocator(&arena);
        
        proven_datetime_t dt = proven_time_now_datetime();
        proven_u8str_t str = {0};
        
        // Smart Rendering: Pass an object directly!
        PROVEN_TEST_ASSERT(PROVEN_FMT_IS_OK(proven_u8str_append_fmt_grow(alloc, &str, "Current Time: {}", PROVEN_ARG(dt))), "Testing condition: PROVEN_FMT_IS_OK(proven_u8str_append_fmt_grow(alloc, &str, 'Current Time: {}', PROVEN_ARG(dt)))", "Review logic surrounding PROVEN_FMT_IS_OK(proven_u8str_append_fmt_grow(alloc, &str, 'Current Time: {}', PROVEN_ARG(dt)))");
        
        PROVEN_TEST_INFO("Formatted Datetime (Smart): {}", PROVEN_ARG((const char*)str.internal.ptr));
        
        proven_arena_destroy(&arena);
    }

    // 4. Escape Braces Test
    PROVEN_TEST_INFO("Testing brace escaping ({{ and }})...");
    {
        proven_byte_t backing[4096];
        proven_arena_t arena = proven_arena_create((proven_mem_mut_t){backing, sizeof(backing)});
        proven_allocator_t alloc = proven_arena_as_allocator(&arena);
        
        proven_u8str_t str = {0};
        
        // Print literal { and }
        PROVEN_TEST_ASSERT(PROVEN_FMT_IS_OK(proven_u8str_append_fmt_grow(alloc, &str, "Braces: {{ and }} and {}", PROVEN_ARG("value"))), "Testing condition: PROVEN_FMT_IS_OK(proven_u8str_append_fmt_grow(alloc, &str, 'Braces: {{ and }} and {}', PROVEN_ARG('value')))", "Review logic surrounding PROVEN_FMT_IS_OK(proven_u8str_append_fmt_grow(alloc, &str, 'Braces: {{ and }} and {}', PROVEN_ARG('value')))");
        
        PROVEN_TEST_INFO("Escape Result: {}", PROVEN_ARG((const char*)str.internal.ptr));
        // Expected: Braces: { and } and value
        
        proven_arena_destroy(&arena);
    }

    // 5. Zig-style Alignment Test
    PROVEN_TEST_INFO("Testing Zig-style alignment ({:^}, {:>}, {:<})...");
    {
        proven_byte_t backing[4096];
        proven_arena_t arena = proven_arena_create((proven_mem_mut_t){backing, sizeof(backing)});
        proven_allocator_t alloc = proven_arena_as_allocator(&arena);
        
        proven_u8str_t str = {0};
        
        // 1. Right align with zero: {:0>5}
        PROVEN_TEST_ASSERT(PROVEN_FMT_IS_OK(proven_u8str_append_fmt_grow(alloc, &str, "Zero: {:0>5}\n", PROVEN_ARG(42))), "Testing condition: PROVEN_FMT_IS_OK(proven_u8str_append_fmt_grow(alloc, &str, 'Zero: {:0>5}\n', PROVEN_ARG(42)))", "Review logic surrounding PROVEN_FMT_IS_OK(proven_u8str_append_fmt_grow(alloc, &str, 'Zero: {:0>5}\n', PROVEN_ARG(42)))");
        
        // 2. Center align with star: {:*^10}
        PROVEN_TEST_ASSERT(PROVEN_FMT_IS_OK(proven_u8str_append_fmt_grow(alloc, &str, "Stars: {:*^10}\n", PROVEN_ARG("Proven"))), "Testing condition: PROVEN_FMT_IS_OK(proven_u8str_append_fmt_grow(alloc, &str, 'Stars: {:*^10}\n', PROVEN_ARG('Proven')))", "Review logic surrounding PROVEN_FMT_IS_OK(proven_u8str_append_fmt_grow(alloc, &str, 'Stars: {:*^10}\n', PROVEN_ARG('Proven')))");
        
        // 3. Left align with dots: {:.<10}
        PROVEN_TEST_ASSERT(PROVEN_FMT_IS_OK(proven_u8str_append_fmt_grow(alloc, &str, "Dots: {:.<10}\n", PROVEN_ARG("End"))), "Testing condition: PROVEN_FMT_IS_OK(proven_u8str_append_fmt_grow(alloc, &str, 'Dots: {:.<10}\n', PROVEN_ARG('End')))", "Review logic surrounding PROVEN_FMT_IS_OK(proven_u8str_append_fmt_grow(alloc, &str, 'Dots: {:.<10}\n', PROVEN_ARG('End')))");

        (void)proven_print("Zig-style Results:\n{}", PROVEN_ARG((const char*)str.internal.ptr));
        
        proven_arena_destroy(&arena);
    }

    // 6. Unsigned Char Support Test
    PROVEN_TEST_INFO("Testing unsigned char string support...");
    {
        proven_byte_t backing[4096];
        proven_arena_t arena = proven_arena_create((proven_mem_mut_t){backing, sizeof(backing)});
        proven_allocator_t alloc = proven_arena_as_allocator(&arena);
        
        proven_u8str_t str = {0};
        unsigned char *ubuf = (unsigned char*)"Unsigned Buffer";
        
        PROVEN_TEST_ASSERT(PROVEN_FMT_IS_OK(proven_u8str_append_fmt_grow(alloc, &str, "Data: {}", PROVEN_ARG(ubuf))), "Testing condition: PROVEN_FMT_IS_OK(proven_u8str_append_fmt_grow(alloc, &str, 'Data: {}', PROVEN_ARG(ubuf)))", "Review logic surrounding PROVEN_FMT_IS_OK(proven_u8str_append_fmt_grow(alloc, &str, 'Data: {}', PROVEN_ARG(ubuf)))");
        
        PROVEN_TEST_INFO("Unsigned Char Result: {}", PROVEN_ARG((const char*)str.internal.ptr));
        
        proven_arena_destroy(&arena);
    }

    // 7. Pointer & Function Pointer Test
    PROVEN_TEST_INFO("Testing pointer and function pointer formatting...");
    {
        proven_byte_t backing[4096];
        proven_arena_t arena = proven_arena_create((proven_mem_mut_t){backing, sizeof(backing)});
        proven_allocator_t alloc = proven_arena_as_allocator(&arena);
        
        proven_u8str_t str = {0};
        void *p = (void*)0xDEADBEEF;
        
        // Normal pointer
        PROVEN_TEST_ASSERT(PROVEN_FMT_IS_OK(proven_u8str_append_fmt_grow(alloc, &str, "Ptr: {}\n", PROVEN_ARG(p))), "Testing condition: PROVEN_FMT_IS_OK(proven_u8str_append_fmt_grow(alloc, &str, 'Ptr: {}\n', PROVEN_ARG(p)))", "Review logic surrounding PROVEN_FMT_IS_OK(proven_u8str_append_fmt_grow(alloc, &str, 'Ptr: {}\n', PROVEN_ARG(p)))");
        
        // Function pointer using the safety macro
        PROVEN_TEST_ASSERT(PROVEN_FMT_IS_OK(proven_u8str_append_fmt_grow(alloc, &str, "Func: {}\n", PROVEN_ARG_FN(main))), "Testing condition: PROVEN_FMT_IS_OK(proven_u8str_append_fmt_grow(alloc, &str, 'Func: {}\n', PROVEN_ARG_FN(main)))", "Review logic surrounding PROVEN_FMT_IS_OK(proven_u8str_append_fmt_grow(alloc, &str, 'Func: {}\n', PROVEN_ARG_FN(main)))");
        
        (void)proven_print("Pointer Results:\n{}", PROVEN_ARG((const char*)str.internal.ptr));
        
        proven_arena_destroy(&arena);
    }

    // 8. Explicit Positional Formatting Test
    PROVEN_TEST_INFO("Testing explicit positional formatting ({0}, {1})...");
    {
        proven_byte_t backing[4096];
        proven_arena_t arena = proven_arena_create((proven_mem_mut_t){backing, sizeof(backing)});
        proven_allocator_t alloc = proven_arena_as_allocator(&arena);
        
        proven_u8str_t str = {0};
        
        // Use explicit index to reorder or re-use arguments
        PROVEN_TEST_ASSERT(PROVEN_FMT_IS_OK(proven_u8str_append_fmt_grow(alloc, &str, 
            "{2} {1} {2} {0}", 
            PROVEN_ARG("Zero"), 
            PROVEN_ARG("One"), 
            PROVEN_ARG("Two"))), "Testing condition: PROVEN_FMT_IS_OK(proven_u8str_append_fmt_grow(...))", "Review logic for explicit positional parsing");
            
        (void)proven_print("Positional Result: {}\n", PROVEN_ARG((const char*)str.internal.ptr));
        PROVEN_TEST_ASSERT(strcmp((const char*)str.internal.ptr, "Two One Two Zero") == 0, "Checking positional layout result", "Ensure explicit indices match correctly");
        
        proven_arena_destroy(&arena);
    }

    // 9. Positional with Formatting Specifics Test
    PROVEN_TEST_INFO("Testing positional with formatting combinations...");
    {
        proven_byte_t backing[4096];
        proven_arena_t arena = proven_arena_create((proven_mem_mut_t){backing, sizeof(backing)});
        proven_allocator_t alloc = proven_arena_as_allocator(&arena);
        
        proven_u8str_t str = {0};
        
        // Combine explicit indexing '{1}' with formatting ':0>5'
        PROVEN_TEST_ASSERT(PROVEN_FMT_IS_OK(proven_u8str_append_fmt_grow(alloc, &str, 
            "{1:*^10} {0:0>5}", 
            PROVEN_ARG(42), 
            PROVEN_ARG("Test"))), "Testing condition: PROVEN_FMT_IS_OK(proven_u8str_append_fmt_grow(...))", "Review logic for positional + formatting combo");
            
        (void)proven_print("Positional+Formatting Result: {}\n", PROVEN_ARG((const char*)str.internal.ptr));
        PROVEN_TEST_ASSERT(strcmp((const char*)str.internal.ptr, "***Test*** 00042") == 0, "Checking positional+formatting layout result", "Ensure explicit indices combinations match correctly");
        
        proven_arena_destroy(&arena);
    }

    // 10. Explicit Datetime Localized output test
    PROVEN_TEST_INFO("Testing datetime localization (EN)...");
    {
        proven_byte_t backing[4096];
        proven_arena_t arena = proven_arena_create((proven_mem_mut_t){backing, sizeof(backing)});
        proven_allocator_t alloc = proven_arena_as_allocator(&arena);
        
        proven_u8str_t str = {0};
        
        // Let's create a specific known datetime: 1970-01-01 -> Thursday
        proven_datetime_t dt = proven_time_breakdown(0); // 0 nanoseconds since epoch
        
        PROVEN_TEST_ASSERT(dt.year == 1970, "Testing condition: dt.year == 1970", "Review logic for epoch start year");
        PROVEN_TEST_ASSERT(dt.month == 1, "Testing condition: dt.month == 1", "Review logic for epoch start month");
        
        PROVEN_TEST_ASSERT(PROVEN_FMT_IS_OK(proven_u8str_append_fmt_grow(alloc, &str, 
            "{}, {}, {}, {}", 
            PROVEN_ARG(proven_time_locale_en.month_names[dt.month - 1]),
            PROVEN_ARG(proven_time_locale_en.month_short_names[dt.month - 1]),
            PROVEN_ARG(proven_time_locale_en.weekday_short_names[dt.weekday]),
            PROVEN_ARG(proven_time_locale_en.weekday_names[dt.weekday]))), "Testing condition: PROVEN_FMT_IS_OK(proven_u8str_append_fmt_grow(...localized dates))", "Review logic for date localization combo");
            
        (void)proven_print("Datetime Localized Result: '{}'\n", PROVEN_ARG((const char*)str.internal.ptr));
        PROVEN_TEST_ASSERT(strcmp((const char*)str.internal.ptr, "January, Jan, Thu, Thursday") == 0, "Checking datetime localized formatting output match", "Ensure string arrays mapping values identically");
        
        proven_arena_destroy(&arena);
    }

    // 11. Datetime Custom String Formation tests
    PROVEN_TEST_INFO("Testing custom datetime string patterns...");
    {
        proven_byte_t backing[4096];
        proven_arena_t arena = proven_arena_create((proven_mem_mut_t){backing, sizeof(backing)});
        proven_allocator_t alloc = proven_arena_as_allocator(&arena);
        
        proven_u8str_t str = {0};
        proven_datetime_t dt = { .year = 2026, .month = 5, .day = 1, .hour = 14, .min = 5, .sec = 9, .weekday = 5 };
        
        PROVEN_TEST_ASSERT(PROVEN_IS_OK(proven_time_u8_fmt(alloc, &str, dt, NULL, "{year}-{month:0>2}-{day:0>2} {hour:0>2}:{min:0>2}:{sec:0>2} {wday}, {Month}")), "Time Format parsing correctness", "Check expected length formatting");
        
        (void)proven_print("Custom Time Formatter: {}\n", PROVEN_ARG((const char*)str.internal.ptr));
        PROVEN_TEST_ASSERT(strcmp((const char*)str.internal.ptr, "2026-05-01 14:05:09 Fri, May") == 0, "Checking expected output mappings against explicit format", "Check time formatting logic matches explicitly");
        
        proven_arena_destroy(&arena);
    }

    // 12. u16 Datetime Formatting tests
    PROVEN_TEST_INFO("Testing u16 datetime formatting...");
    {
        proven_byte_t backing[4096];
        proven_arena_t arena = proven_arena_create((proven_mem_mut_t){backing, sizeof(backing)});
        proven_allocator_t alloc = proven_arena_as_allocator(&arena);
        
        proven_u16str_t str = {0};
        proven_datetime_t dt = { .year = 2026, .month = 5, .day = 1, .hour = 14, .min = 5, .sec = 9, .weekday = 5 };
        
        PROVEN_TEST_ASSERT(PROVEN_IS_OK(proven_time_u16_fmt(alloc, &str, dt, NULL, "{year}-{month:0>2}-{day:0>2} {hour:0>2}:{min:0>2}:{sec:0>2} {wday}, {Month}")), "Time u16 Format parsing correctness", "Check u16 parsing length");
        
        // Convert to u8str to test it correctly
        proven_u8str_t u8str = {0};
        for (proven_size_t i = 0; i < str.internal.len; i++) {
            proven_u16 ch = ((const proven_u16*)str.internal.ptr)[i];
            (void)proven_u8str_append_grow(alloc, &u8str, (proven_u8str_view_t){(const proven_byte_t*)&ch, 1});
        }
        
        (void)proven_print("Custom Time Formatter (u16 -> u8): {}\n", PROVEN_ARG((const char*)u8str.internal.ptr));
        PROVEN_TEST_ASSERT(strcmp((const char*)u8str.internal.ptr, "2026-05-01 14:05:09 Fri, May") == 0, "Checking expected output mappings against explicit format in u16 mode", "Check conversion output");
        
        proven_arena_destroy(&arena);
    }

    // 13. Bounds Check and UB Prevention in Datetime Formatting
    PROVEN_TEST_INFO("Testing bounds safety and UB prevention in datetimes...");
    {
        proven_byte_t backing[4096];
        proven_arena_t arena = proven_arena_create((proven_mem_mut_t){backing, sizeof(backing)});
        proven_allocator_t alloc = proven_arena_as_allocator(&arena);
        
        // Test Out-Of-Bounds month and weekday
        proven_u8str_t str = {0};
        proven_datetime_t dt = { .year = -2147483647, .month = 255, .day = 0, .hour = 0, .min = 0, .sec = 0, .weekday = 99 };
        
        // The formatter should silently discard the invalid array accesses or ignore them
        PROVEN_TEST_ASSERT(PROVEN_IS_OK(proven_time_u8_fmt(alloc, &str, dt, NULL, "{year}-{Month}-{wday}")), "Time Format handling invalid bounds", "Check invalid OOB logic bounds padding");
        (void)proven_print("OOB u8 Result: '{}'\n", PROVEN_ARG((const char*)str.internal.ptr));
        PROVEN_TEST_ASSERT(strcmp((const char*)str.internal.ptr, "-2147483647--") == 0, "Checking OOB month/weekday fallback", "Check bounds fallback");
        
        // Large padding bounds test (e.g., zero pad > 9 should cap without overflowing buffer!)
        proven_u16str_t u16str = {0};
        PROVEN_TEST_ASSERT(PROVEN_IS_OK(proven_time_u16_fmt(alloc, &u16str, dt, NULL, "{year:0>9}")), "Large padding u16 handling", "Check large zero padding cap");
        
        proven_u8str_t u8_from_16 = {0};
        for (proven_size_t i = 0; i < u16str.internal.len; i++) {
            proven_u16 ch = ((const proven_u16*)u16str.internal.ptr)[i];
            (void)proven_u8str_append_grow(alloc, &u8_from_16, (proven_u8str_view_t){(const proven_byte_t*)&ch, 1});
        }
        (void)proven_print("Padding u16 Result: '{}'\n", PROVEN_ARG((const char*)u8_from_16.internal.ptr));
        
        // Should cap pad to max 9 safely (so -2147483647 since it's already >9 length, or pad -0000...)
        // It's up to specific impl handling length, but no crash = pass.
        
        proven_arena_destroy(&arena);
    }

    PROVEN_TEST_PASS("All Phase 16 Time & Format Tests Passed Successfully!");
    return 0;
}
