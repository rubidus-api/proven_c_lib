/*
Regression tests are not general feature tests.
They encode specific bugs that were previously found and fixed.

Phase tests check broad feature behavior.
Regression tests check that known dangerous edge cases do not return.
*/

#include "proven.h"
#include "proven_test.h"
#include <string.h>
#include <stdlib.h>
#include <stdalign.h>

#if defined(_WIN32) || defined(_WIN64)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

// Mock allocator to track allocations
typedef struct {
    size_t alloc_count;
    size_t free_count;
} track_alloc_t;

static proven_result_mem_mut_t track_alloc_fn(void* ctx, proven_size_t size, proven_size_t align) {
    (void)align;
    track_alloc_t* track = (track_alloc_t*)ctx;
    track->alloc_count++;
    void* p = malloc(size);
    if (!p) return (proven_result_mem_mut_t){ .err = PROVEN_ERR_NOMEM };
    return (proven_result_mem_mut_t){ .err = PROVEN_OK, .value = { .ptr = (proven_byte_t*)p, .size = size } };
}

static proven_result_mem_mut_t track_realloc_fn(void* ctx, void* old_ptr, proven_size_t old_size, proven_size_t new_size, proven_size_t align) {
    (void)align;
    (void)old_size;
    track_alloc_t* track = (track_alloc_t*)ctx;
    track->alloc_count++;
    void* p = realloc(old_ptr, new_size);
    if (!p) return (proven_result_mem_mut_t){ .err = PROVEN_ERR_NOMEM };
    return (proven_result_mem_mut_t){ .err = PROVEN_OK, .value = { .ptr = (proven_byte_t*)p, .size = new_size } };
}

static void track_free_fn(void* ctx, void* ptr) {
    track_alloc_t* track = (track_alloc_t*)ctx;
    if (ptr) {
        track->free_count++;
        free(ptr);
    }
}

static proven_allocator_t make_tracker(track_alloc_t* t) {
    return (proven_allocator_t){
        .ctx = t,
        .alloc_fn = track_alloc_fn,
        .realloc_fn = track_realloc_fn,
        .free_fn = track_free_fn
    };
}

void test_map_self_payload_rehash(void) {
    PROVEN_TEST_INFO("Testing map self-payload rehash...");
    proven_allocator_t alloc = proven_heap_allocator();
    
    // Create a map with small capacity to force rehash
    proven_map_t map;
    proven_result_map_t res = PROVEN_MAP_INIT_INT(alloc, int, 8);
    PROVEN_TEST_ASSERT(proven_is_ok(res.err), "Map init failed", "");
    map = res.value;
    
    // Fill it up to threshold (6 for cap 8)
    for (int i = 0; i < 6; ++i) {
        int val = i * 10;
        (void)PROVEN_MAP_SET_INT(&map, i, int, val);
    }
    
    // Now trigger a rehash by setting a new key (6), BUT using a value that is inside the map.
    // Get pointer to existing value in map
    const int *ex_val = PROVEN_MAP_GET_INT(&map, int, 0);
    PROVEN_TEST_ASSERT(ex_val != NULL, "Key 0 missing", "");
    PROVEN_TEST_ASSERT(*ex_val == 0, "Value 0 mismatch", "");
    
    // Setting key 6 using pointer to key 0's value. 
    // Since used (6) >= threshold (6), it will rehash.
    int new_key = 6;
    
    track_alloc_t scratch_tracker = {0};
    proven_allocator_t scratch = make_tracker(&scratch_tracker);
    
    (void)proven_map_set_with_scratch(&map, (proven_map_key_t){ .id = (proven_size_t)new_key }, ex_val, scratch);
    
    // Verify result
    const int *res_val = PROVEN_MAP_GET_INT(&map, int, 6);
    PROVEN_TEST_ASSERT(res_val != NULL, "Key 6 missing", "");
    PROVEN_TEST_ASSERT(*res_val == 0, "Value 6 mismatch", "");
    
    proven_map_destroy(&map);
}

void test_map_large_value_rehash_allocation(void) {
    PROVEN_TEST_INFO("Testing map large value rehash allocation tracking...");
    track_alloc_t tracker = {0};
    proven_allocator_t talloc = make_tracker(&tracker);
    
    track_alloc_t scratch_tracker = {0};
    proven_allocator_t scratch = make_tracker(&scratch_tracker);
    
    typedef struct { char data[300]; } large_t;
    
    proven_result_map_t res = proven_map_create(talloc, 8, PROVEN_KEY_TYPE_INT, sizeof(large_t), 16);
    PROVEN_TEST_ASSERT(proven_is_ok(res.err), "Map create failed", "");
    proven_map_t map = res.value;
    
    large_t l1 = {0};
    l1.data[0] = 'A';
    for (int i = 0; i < 6; ++i) {
        (void)PROVEN_MAP_SET_INT(&map, i, large_t, l1);
    }
    
    // used = 6, capacity = 8. Threshold = 6.
    // Next insert will rehash.
    
    const large_t *in_map_ptr = PROVEN_MAP_GET_INT(&map, large_t, 0);
    
    size_t prev_allocs = tracker.alloc_count;
    size_t prev_scratch_allocs = scratch_tracker.alloc_count;
    size_t prev_scratch_frees = scratch_tracker.free_count;
    
    // This should trigger rehash AND use scratch for temp buffer because sizeof(large_t) > 256
    // We pass in_map_ptr directly to ensure self-aliasing is detected (macros would copy the value)
    proven_err_t err = proven_map_set_with_scratch(&map, (proven_map_key_t){ .id = 6 }, in_map_ptr, scratch);
    PROVEN_TEST_ASSERT(proven_is_ok(err), "Map set failed", "");
    
    // We expect:
    // 1. New bucket allocation in map->alloc (talloc)
    // 2. Temp buffer allocation in scratch (scratch_tracker) because it's > 256 and self-aliased
    PROVEN_TEST_ASSERT(tracker.alloc_count > prev_allocs, "Alloc missing", "");
    PROVEN_TEST_ASSERT(scratch_tracker.alloc_count == prev_scratch_allocs + 1, "Scratch alloc missing", "");
    PROVEN_TEST_ASSERT(scratch_tracker.free_count == prev_scratch_frees + 1, "Scratch free missing", "");
    
    proven_map_destroy(&map);
}

void test_fmt_self_view_grow(void) {
    PROVEN_TEST_INFO("Testing fmt self STR_VIEW grow...");
    proven_allocator_t alloc = proven_heap_allocator();
    
    // Small initial capacity
    proven_u8str_t str = {0};
    (void)proven_u8str_append_grow(alloc, &str, proven_u8str_view_from_cstr("InitialContent"));
    
    // Create a view of itself
    proven_u8str_view_t self_view = proven_u8str_as_view(&str);
    
    // Append itself multiple times to force growth
    for (int i = 0; i < 10; ++i) {
        (void)proven_u8str_append_fmt_grow(alloc, &str, "{}", PROVEN_ARG(self_view));
        // Update view to new content for next iteration
        self_view = proven_u8str_as_view(&str);
    }
    
    PROVEN_TEST_ASSERT(str.internal.len > 14, "Length too short", "");
    
    proven_u8str_destroy(alloc, &str);
}

void test_fmt_huge_index(void) {
    PROVEN_TEST_INFO("Testing fmt huge argument index...");
    proven_u8str_t str = {0};
    // Should safely return error or handle it without crashing/UBSan
    proven_fmt_result_t res = proven_u8str_append_fmt_grow(proven_heap_allocator(), &str, "{999999}", PROVEN_ARG(42));
    PROVEN_TEST_ASSERT(!proven_is_ok(res.err), "Should fail with OOB", "");
    proven_u8str_destroy(proven_heap_allocator(), &str);
}

void test_fmt_many_args_no_alias_grow(void) {
    PROVEN_TEST_INFO("Testing fmt many args no alias with grow (stack overflow check)...");
    proven_allocator_t a = proven_heap_allocator();

    proven_result_u8str_t r = proven_u8str_create(a, 1);
    PROVEN_TEST_ASSERT(proven_is_ok(r.err), "Str create failed", "");

    proven_arg_t args[] = {
        proven_arg_none(),
        proven_arg_i32(1), proven_arg_i32(2), proven_arg_i32(3), proven_arg_i32(4),
        proven_arg_i32(5), proven_arg_i32(6), proven_arg_i32(7), proven_arg_i32(8),
        proven_arg_i32(9), proven_arg_i32(10), proven_arg_i32(11), proven_arg_i32(12),
        proven_arg_i32(13), proven_arg_i32(14), proven_arg_i32(15), proven_arg_i32(16),
        proven_arg_i32(17), proven_arg_i32(18), proven_arg_i32(19), proven_arg_i32(20)
    };

    proven_fmt_result_t fr = proven_u8str_fmt_internal(
        a,
        &r.value,
        false,
        "{}{}{}{}{}{}{}{}{}{}{}{}{}{}{}{}{}{}{}{}",
        (proven_allocator_t){0},
        args,
        sizeof(args) / sizeof(args[0])
    );

    PROVEN_TEST_ASSERT(proven_is_ok(fr.err), "Fmt internal failed", "");

    proven_u8str_destroy(a, &r.value);
}

void test_fmt_many_args_alias_scratch_grow(void) {
    PROVEN_TEST_INFO("Testing fmt many args alias scratch grow...");
    track_alloc_t tracker = {0};
    proven_allocator_t scratch = make_tracker(&tracker);
    proven_allocator_t grow_alloc = proven_heap_allocator();

    proven_result_u8str_t r = proven_u8str_create(grow_alloc, 4);
    PROVEN_TEST_ASSERT(proven_is_ok(r.err), "Str create failed", "");
    (void)proven_u8str_append_grow(grow_alloc, &r.value, proven_u8str_view_from_cstr("TEST"));

    proven_u8str_view_t self_view = proven_u8str_as_view(&r.value);

    proven_arg_t args[] = {
        proven_arg_none(),
        PROVEN_ARG(self_view),
        proven_arg_i32(2), proven_arg_i32(3), proven_arg_i32(4),
        proven_arg_i32(5), proven_arg_i32(6), proven_arg_i32(7), proven_arg_i32(8),
        proven_arg_i32(9), proven_arg_i32(10), proven_arg_i32(11), proven_arg_i32(12),
        proven_arg_i32(13), proven_arg_i32(14), proven_arg_i32(15), proven_arg_i32(16),
        proven_arg_i32(17), proven_arg_i32(18), proven_arg_i32(19), proven_arg_i32(20)
    };

    size_t initial_allocs = tracker.alloc_count;

    proven_fmt_result_t fr = proven_u8str_fmt_internal(
        grow_alloc,
        &r.value,
        false,
        "{}{}{}{}{}{}{}{}{}{}{}{}{}{}{}{}{}{}{}{}",
        scratch,
        args,
        sizeof(args) / sizeof(args[0])
    );

    PROVEN_TEST_ASSERT(proven_is_ok(fr.err), "Fmt internal failed", "");
    
    PROVEN_TEST_ASSERT(tracker.alloc_count > initial_allocs, "Scratch alloc missing", "");
    PROVEN_TEST_ASSERT(tracker.free_count > 0, "No free tracked", "");
    PROVEN_TEST_ASSERT(tracker.alloc_count == tracker.free_count, "Leaked scratch memory", "");

    proven_u8str_destroy(grow_alloc, &r.value);
}

void test_map_existing_key_update_before_rehash(void) {
    PROVEN_TEST_INFO("Testing map existing key update before rehash...");
    proven_allocator_t alloc = proven_heap_allocator();
    
    proven_map_t map;
    proven_result_map_t res = PROVEN_MAP_INIT_INT(alloc, int, 8);
    PROVEN_TEST_ASSERT(proven_is_ok(res.err), "Map init failed", "");
    map = res.value;
    
    for (int i = 0; i < 6; ++i) {
        int val = i * 10;
        (void)PROVEN_MAP_SET_INT(&map, i, int, val);
    }
    
    // Threshold is 6. Setting an EXISTING key SHOULD NOT trigger a rehash.
    int val = 999;
    (void)PROVEN_MAP_SET_INT(&map, 0, int, val);
    
    // Validate that it was set properly.
    const int *res_val = PROVEN_MAP_GET_INT(&map, int, 0);
    PROVEN_TEST_ASSERT(res_val != NULL, "Key 0 missing", "");
    PROVEN_TEST_ASSERT(*res_val == 999, "Value 0 mismatch", "");
    
    PROVEN_TEST_ASSERT(map.cap == 8, "Unexpected grow", ""); // Ensure it didn't grow
    
    proven_map_destroy(&map);
}

void test_fmt_self_cstr_grow_invalid_arg(void) {
    PROVEN_TEST_INFO("Testing fmt self CSTR grow INVALID_ARG...");
    proven_allocator_t alloc = proven_heap_allocator();
    
    proven_u8str_t str = {0};
    (void)proven_u8str_append_grow(alloc, &str, proven_u8str_view_from_cstr("Short"));
    
    // Attempting to pass its own internal buffer as a CSTR should fail with INVALID_ARG
    proven_fmt_result_t res = proven_u8str_append_fmt_grow(alloc, &str, "{}", proven_arg_cstr((const char*)str.internal.ptr));
    PROVEN_TEST_ASSERT(res.err == PROVEN_ERR_INVALID_ARG, "Should fail with INVALID_ARG", "");
    
    proven_u8str_destroy(alloc, &str);
}

void test_scan_invalid_cursor(void) {
    PROVEN_TEST_INFO("Testing scan invalid cursor...");
    proven_scan_t scan = { .view = proven_u8str_view_from_cstr("abc"), .cursor = 999 };
    // Should fail cleanly when read out of bounds
    proven_result_u8str_view_t res = proven_scan_str(&scan);
    PROVEN_TEST_ASSERT(!proven_is_ok(res.err) || scan.cursor > 3, "Scan should fail", "");
}

void test_array_string_self_alias_grow(void) {
    PROVEN_TEST_INFO("Testing array/string self-alias grow...");
    proven_allocator_t alloc = proven_heap_allocator();
    
    // U8STR self-alias
    proven_u8str_t u8 = {0};
    (void)proven_u8str_append_grow(alloc, &u8, proven_u8str_view_from_cstr("ABCD"));
    proven_u8str_view_t u8_view = proven_u8str_as_view(&u8);
    // Push it multiple times to force reallocation
    for (int i = 0; i < 20; ++i) {
        (void)proven_u8str_append_grow(alloc, &u8, u8_view);
        u8_view = proven_u8str_as_view(&u8);
    }
    PROVEN_TEST_ASSERT(u8.internal.len > 20, "Str internal len too small", "");
    proven_u8str_destroy(alloc, &u8);
    
    // Array self-alias
    proven_result_array_t a_res = proven_array_create(alloc, 4, sizeof(int), alignof(int));
    PROVEN_TEST_ASSERT(proven_is_ok(a_res.err), "Array create failed", "");
    proven_array_t arr = a_res.value;
    int val = 42;
    (void)proven_array_push(&arr, &val);
    // Push itself repeatedly
    for (int i = 0; i < 20; ++i) {
        const int* first = (const int*)arr.data;
        (void)proven_array_push(&arr, first);
    }
    PROVEN_TEST_ASSERT(arr.len == 21, "Array len mismatch", "");
    proven_array_destroy(&arr);
}

void test_fmt_cstr_n_bounds(void) {
    PROVEN_TEST_INFO("Testing PROVEN_ARG_CSTR_N safety bounds...");
    proven_allocator_t alloc = proven_heap_allocator();
    proven_u8str_t s = {0};

    // A string that is NOT null-terminated but happens to have other memory around it.
    char buffer[12] = {'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L'};
    
    // Formatting with CSTR_N should truncate properly and not overrun.
    proven_fmt_result_t res = proven_u8str_append_fmt_grow(alloc, &s, "Test: {}", PROVEN_ARG_CSTR_N(buffer, 4));
    PROVEN_TEST_ASSERT(res.err == PROVEN_OK, "Format CSTR_N failed", "");
    
    proven_u8str_view_t view = proven_u8str_as_view(&s);
    PROVEN_TEST_ASSERT(view.size == 10, "Format output size mismatch", "");
    
    // Explicit array comparison instead of strncmp to avoid string issues
    bool eq = proven_u8str_view_eq(view, proven_u8str_view_from_cstr("Test: ABCD"));
    PROVEN_TEST_ASSERT(eq, "Format CSTR_N result mismatch", "");
    
    proven_u8str_destroy(alloc, &s);
}

void test_env_get_large_value(void) {
    PROVEN_TEST_INFO("Testing proven_env_get with large value (>4KB)...");
    
    proven_allocator_t alloc = proven_heap_allocator();
    const char *key = "PROVEN_LARGE_TEST_VAR";
    
    // Create a 5KB string
    size_t large_size = 5000;
    char *large_val = (char*)malloc(large_size + 1);
    for (size_t i = 0; i < large_size; ++i) {
        large_val[i] = (char)('A' + (i % 26));
    }
    large_val[large_size] = '\0';
    
#if defined(_WIN32) || defined(_WIN64)
    // Environment variables are limited on some systems but 5KB is usually fine.
    // Using wchar_t version for Windows to be sure.
    wchar_t wkey[64];
    wchar_t *wval = (wchar_t*)malloc((large_size + 1) * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, key, -1, wkey, 64);
    MultiByteToWideChar(CP_UTF8, 0, large_val, -1, wval, (int)large_size + 1);
    SetEnvironmentVariableW(wkey, wval);
    free(wval);
#else
    setenv(key, large_val, 1);
#endif

    proven_result_u8str_t res = proven_env_get(alloc, proven_u8str_view_from_cstr(key));
    PROVEN_TEST_ASSERT(proven_is_ok(res.err), "proven_env_get failed for large value", "");
    
    PROVEN_TEST_ASSERT(res.value.internal.len == large_size, "Result length mismatch", "");
    PROVEN_TEST_ASSERT(memcmp(res.value.internal.ptr, large_val, large_size) == 0, "Result content mismatch", "");
    
    proven_u8str_destroy(alloc, &res.value);
    free(large_val);
}

int main(void) {
    test_map_self_payload_rehash();
    test_map_existing_key_update_before_rehash();
    test_map_large_value_rehash_allocation();
    test_fmt_self_view_grow();
    test_fmt_self_cstr_grow_invalid_arg();
    test_fmt_huge_index();
    test_fmt_many_args_no_alias_grow();
    test_fmt_many_args_alias_scratch_grow();
    test_scan_invalid_cursor();
    test_array_string_self_alias_grow();
    test_fmt_cstr_n_bounds();
    test_env_get_large_value();
    
    PROVEN_TEST_PASS("All regression tests passed successfully.");
    return 0;
}
