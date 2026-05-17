#include "proven/memory.h"
#include "proven/arena.h"
#include "proven/array.h"
#include "proven/list.h"
#include "proven/ring.h"
#include "proven/map.h"
#include "proven/u8str.h"
#include "proven/fmt.h"
#include "proven/coro.h"
#include "proven/scan.h"
#include "proven/algorithm.h"
#include "proven/align.h"
#include "proven_test.h"

// libc results are bypassed for core logic, but output used for report.
// proven freestanding macros already defined on compiler command line:
// PROVEN_FREESTANDING, PROVEN_FMT_NO_FLOAT, PROVEN_NO_U16STR

static alignas(PROVEN_MAX_ALIGN) proven_byte_t g_arena_mem[64 * 1024];

typedef struct {
    proven_i32 state;
    proven_i32 counter;
} test_coro_state;

static proven_i32 test_coro(test_coro_state *ctx) {
    PROVEN_CORO_BEGIN(ctx);
    ctx->counter = 1;
    PROVEN_CORO_YIELD(ctx);
    ctx->counter = 2;
    PROVEN_CORO_YIELD(ctx);
    ctx->counter = 3;
    PROVEN_CORO_END(ctx);
}

static int test_compare(const void *a, const void *b) {
    int v1 = *(const int*)a; // Works well with proven_i32
    int v2 = *(const int*)b;
    return (v1 > v2) - (v1 < v2);
}

typedef struct my_node {
    proven_list_node_t list_node;
    proven_i32 val;
} my_node_t;

// Override the panic handler for freestanding deterministic traps
void proven_panic_handler(const char *msg) {
    (void)msg;
    __builtin_trap();
}

int main(void) {
    PROVEN_TEST_INFO("--- Starting Freestanding Environment Tests ---");
    proven_mem_mut_t backing = { .ptr = g_arena_mem, .size = sizeof(g_arena_mem) };
    proven_arena_t arena = proven_arena_create(backing);
    proven_allocator_t alloc = proven_arena_as_allocator(&arena);

    // 1. Array & Algorithm
    PROVEN_TEST_INFO("Testing Array & Sort Algorithm...");
    proven_result_array_t arr_res = PROVEN_ARRAY_INIT(alloc, proven_i32, 4);
    PROVEN_TEST_ASSERT(arr_res.err == PROVEN_OK, "Array initialization", "Ensure allocators work in freestanding mode");
    
    proven_array_t arr = arr_res.value;
    PROVEN_TEST_ASSERT(PROVEN_ARRAY_PUSH(&arr, proven_i32, 42) == PROVEN_OK, "Push 42", "Array should grow or use initial capacity");
    PROVEN_TEST_ASSERT(PROVEN_ARRAY_PUSH(&arr, proven_i32, 12) == PROVEN_OK, "Push 12", "Array should grow or use initial capacity");
    PROVEN_TEST_ASSERT(PROVEN_ARRAY_PUSH(&arr, proven_i32, 5) == PROVEN_OK, "Push 5", "Array should grow or use initial capacity");
    
    proven_array_sort(&arr, test_compare);
    PROVEN_TEST_ASSERT(*PROVEN_ARRAY_GET(&arr, proven_i32, 0) == 5, "Sorted index 0", "Algorithm proven_array_sort should produce ascending order");
    PROVEN_TEST_ASSERT(*PROVEN_ARRAY_GET(&arr, proven_i32, 1) == 12, "Sorted index 1", "Algorithm proven_array_sort should produce ascending order");
    PROVEN_TEST_ASSERT(*PROVEN_ARRAY_GET(&arr, proven_i32, 2) == 42, "Sorted index 2", "Algorithm proven_array_sort should produce ascending order");

    proven_i32 key = 12;
    void *found = proven_array_binary_search(&arr, &key, test_compare);
    PROVEN_TEST_ASSERT(found != (void*)0, "Binary search found key", "Algorithm binary_search should find existing item");
    PROVEN_TEST_ASSERT(*(proven_i32*)found == 12, "Binary search correct value", "Returned pointer should point to the searched key");

    // 2. List
    PROVEN_TEST_INFO("Testing Intrusive List...");
    my_node_t node1 = {0};
    node1.val = 1;
    my_node_t node2 = {0};
    node2.val = 2;
    proven_list_t list;
    proven_list_init(&list);
    proven_list_push_back(&list, &node1.list_node);
    proven_list_push_back(&list, &node2.list_node);
    PROVEN_TEST_ASSERT(proven_list_is_empty(&list) == 0, "List is not empty", "Pushing items should update list state");
    
    proven_list_node_t *iter;
    proven_i32 sum = 0;
    PROVEN_LIST_FOR_EACH(iter, &list) {
        my_node_t *n = PROVEN_CONTAINER_OF(iter, my_node_t, list_node);
        sum += n->val;
    }
    PROVEN_TEST_ASSERT(sum == 3, "List traversal sum", "Iteration should visit all elements exactly once");

    // 3. Ring
    PROVEN_TEST_INFO("Testing Ring Buffer...");
    proven_result_ring_t ring_res = PROVEN_RING_INIT(alloc, proven_i32, 4);
    PROVEN_TEST_ASSERT(ring_res.err == PROVEN_OK, "Ring initialization", "Ring buffer allocation should succeed");
    
    proven_ring_t ring = ring_res.value;
    PROVEN_TEST_ASSERT(PROVEN_RING_PUSH(&ring, proven_i32, 100) == PROVEN_OK, "Ring push", "Ring buffer should accept data");
    proven_i32 pop_val = 0;
    PROVEN_TEST_ASSERT(PROVEN_RING_POP(&ring, proven_i32, &pop_val) == PROVEN_OK, "Ring pop", "Ring buffer should return pushed data");
    PROVEN_TEST_ASSERT(pop_val == 100, "Ring pop value", "Data integrity check for ring buffer");

    // 4. Map
    PROVEN_TEST_INFO("Testing Hash Map...");
    proven_result_map_t map_res = PROVEN_MAP_INIT_INT(alloc, proven_i32, 16);
    PROVEN_TEST_ASSERT(map_res.err == PROVEN_OK, "Map initialization", "Hash map allocation should succeed");
    
    proven_map_t map = map_res.value;
    PROVEN_TEST_ASSERT(PROVEN_MAP_SET_INT(&map, 1234, proven_i32, 5678) == PROVEN_OK, "Map set", "Map should store key-value pair");
    const proven_i32 *map_val = PROVEN_MAP_GET_INT(&map, proven_i32, 1234);
    PROVEN_TEST_ASSERT(map_val != (void*)0, "Map get existing key", "Key lookup should succeed");
    PROVEN_TEST_ASSERT(*map_val == 5678, "Map get value", "Value retrieved should match value stored");

    // 5. u8str & fmt
    PROVEN_TEST_INFO("Testing u8str & String Formatting...");
    proven_result_u8str_t str_res = proven_u8str_create(alloc, 64);
    PROVEN_TEST_ASSERT(str_res.err == PROVEN_OK, "u8str creation", "String allocation should succeed");
    
    proven_u8str_t s = str_res.value;
    proven_i32 fnum = 777;
    proven_fmt_result_t fmt_res = proven_u8str_append_fmt_grow(alloc, &s, "Result: {}", PROVEN_ARG(fnum));
    PROVEN_TEST_ASSERT(fmt_res.err == PROVEN_OK, "String formatting", "Formatting should handle integers correctly");

    // 6. scan
    PROVEN_TEST_INFO("Testing Scan Parser...");
    proven_u8str_view_t view = proven_u8str_as_view(&s);
    proven_scan_t sc = proven_scan_init(view);
    proven_scan_skip_until_number(&sc);
    proven_result_i64_t sc_res = proven_scan_i64(&sc);
    PROVEN_TEST_ASSERT(sc_res.err == PROVEN_OK, "Scan integer", "Scanner should find the formatted number");
    PROVEN_TEST_ASSERT(sc_res.val == 777, "Scanner value", "Scanner should parse 777 correctly");

    // 6.1 scan_fmt (modern structural scan)
    PROVEN_TEST_INFO("Testing scan_fmt...");
    proven_i32 scanned_val = 0;
    proven_err_t sc_err = proven_scan_fmt(view, "Result: {}", PROVEN_SCAN_ARG(&scanned_val));
    PROVEN_TEST_ASSERT(sc_err == PROVEN_OK, "scan_fmt call", "Structural scan should match formatted string");
    PROVEN_TEST_ASSERT(scanned_val == 777, "scan_fmt value", "Structural scan should extract 777 correcty");

    // 7. coro
    PROVEN_TEST_INFO("Testing Coroutines...");
    test_coro_state coro = {0};
    PROVEN_CORO_INIT(&coro); 
    PROVEN_TEST_ASSERT(test_coro(&coro) == 0, "Coro 1st yield", "Coro should yield 0 when active");
    PROVEN_TEST_ASSERT(coro.counter == 1, "Coro counter 1", "Check side effect after 1st yield");
    PROVEN_TEST_ASSERT(test_coro(&coro) == 0, "Coro 2nd yield", "Coro should yield 0 when active");
    PROVEN_TEST_ASSERT(coro.counter == 2, "Coro counter 2", "Check side effect after 2nd yield");
    PROVEN_TEST_ASSERT(test_coro(&coro) == 1, "Coro 3rd yield (end)", "Coro should yield 1 when finished");
    PROVEN_TEST_ASSERT(coro.counter == 3, "Coro counter 3", "Check side effect after finishing");

    PROVEN_TEST_PASS("test_freestanding passed");

    return 0;
}
