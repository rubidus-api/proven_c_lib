#include "proven.h"
#include "proven_test.h"
#include "proven/sysio.h"

#include <stdalign.h>
#include <string.h>


typedef struct {
    int id;
    float score;
} test_player_t;

/* RFC-0009 X-002 model check: an allocator that can be told to refuse, so a failed grow can be
 * shown to leave the array exactly as it was. */
static bool g_refuse_grow;
static proven_result_mem_mut_t x2_alloc(void *ctx, proven_size_t size, proven_size_t align) {
    (void)ctx; proven_allocator_t h = proven_heap_allocator(); return h.alloc_fn(h.ctx, size, align);
}
static proven_result_mem_mut_t x2_realloc(void *ctx, void *p, proven_size_t o, proven_size_t n, proven_size_t align) {
    (void)ctx;
    if (g_refuse_grow && n > o) return (proven_result_mem_mut_t){ .err = PROVEN_ERR_NOMEM };
    proven_allocator_t h = proven_heap_allocator(); return h.realloc_fn(h.ctx, p, o, n, align);
}
static void x2_free(void *ctx, void *p) { (void)ctx; proven_allocator_t h = proven_heap_allocator(); h.free_fn(h.ctx, p); }

int main() {
    PROVEN_TEST_INFO("Running Phase 8 Dynamic Array Tests...");

    proven_allocator_t heap = proven_heap_allocator();

    // 1. Array Creation & Push/Pop
    PROVEN_TEST_INFO("Testing dynamic array creation and push/pop...");
    proven_result_array_t res = PROVEN_ARRAY_INIT(heap, test_player_t, 2);
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(res.err), "Testing condition: PROVEN_IS_OK(res.err)", "Review logic surrounding PROVEN_IS_OK(res.err)");
    proven_array_t arr = res.value;

    PROVEN_TEST_ASSERT(arr.len == 0, "Testing condition: arr.len == 0", "Review logic surrounding arr.len == 0");
    PROVEN_TEST_ASSERT(arr.cap >= 2, "Testing condition: arr.cap >= 2", "Review logic surrounding arr.cap >= 2");
    PROVEN_TEST_ASSERT(arr.elem_size == sizeof(test_player_t), "Testing condition: arr.elem_size == sizeof(test_player_t)", "Review logic surrounding arr.elem_size == sizeof(test_player_t)");
    
    // Validation Test
    PROVEN_TEST_ASSERT(proven_array_is_valid(&arr), "Array should be valid initially", "");
    arr.len = SIZE_MAX; // Direct manipulation is UB equivalent, simulated for check
    PROVEN_TEST_ASSERT(!proven_array_is_valid(&arr), "Array should detect invalid length bounds", "");
    arr.len = 0; // Restore
    
    // Type-safe macro push
    test_player_t p1 = { .id = 1, .score = 10.5f };
    test_player_t p2 = { .id = 2, .score = 22.0f };
    
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&arr, test_player_t, p1)), "Testing condition: PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&arr, test_player_t, p1))", "Review logic surrounding PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&arr, test_player_t, p1))");
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&arr, test_player_t, p2)), "Testing condition: PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&arr, test_player_t, p2))", "Review logic surrounding PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&arr, test_player_t, p2))");
    PROVEN_TEST_ASSERT(arr.len == 2, "Testing condition: arr.len == 2", "Review logic surrounding arr.len == 2");

    // 2. Growth Behavior (Re-allocation Strategy)
    PROVEN_TEST_INFO("Testing growth behavior and re-allocation strategies...");
    void* old_ptr = arr.data;
    
    test_player_t p3 = { .id = 3, .score = 50.0f };
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&arr, test_player_t, p3)), "Testing condition: PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&arr, test_player_t, p3))", "Review logic surrounding PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&arr, test_player_t, p3))"); // Triggers capacity expansion!

    PROVEN_TEST_ASSERT(arr.len == 3, "Testing condition: arr.len == 3", "Review logic surrounding arr.len == 3");
    PROVEN_TEST_ASSERT(arr.cap >= 4, "Testing condition: arr.cap >= 4", "Review logic surrounding arr.cap >= 4"); // Expanded capacity
    // Growing must preserve the elements. Whether the block moved is the
    // allocator's business: an in-place realloc is a correct, and cheaper,
    // outcome, so asserting the pointer changed would test the allocator's
    // implementation rather than the array's contract.
    (void)old_ptr;
    PROVEN_TEST_ASSERT(arr.data != NULL, "Testing condition: arr.data != NULL after growth", "Growth must leave the array with live storage.");

    // Verify pristine data state after migration
    const test_player_t *migrated_p1 = PROVEN_ARRAY_GET(&arr, test_player_t, 0);
    PROVEN_TEST_ASSERT(migrated_p1->id == 1, "Testing condition: migrated_p1->id == 1", "Review logic surrounding migrated_p1->id == 1");
    const test_player_t *migrated_p3 = PROVEN_ARRAY_GET(&arr, test_player_t, 2);
    PROVEN_TEST_ASSERT(migrated_p3->id == 3, "Testing condition: migrated_p3->id == 3", "Review logic surrounding migrated_p3->id == 3");
    PROVEN_TEST_ASSERT(migrated_p3->score == 50.0f, "Testing condition: migrated_p3->score == 50.0f", "Review logic surrounding migrated_p3->score == 50.0f");

    // 3. Popping and Bound Guards
    PROVEN_TEST_INFO("Testing popping and boundary guards...");
    test_player_t pop_result;
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(PROVEN_ARRAY_POP(&arr, test_player_t, &pop_result)), "Testing condition: PROVEN_IS_OK(PROVEN_ARRAY_POP(&arr, test_player_t, &pop_result))", "Review logic surrounding PROVEN_IS_OK(PROVEN_ARRAY_POP(&arr, test_player_t, &pop_result))");
    PROVEN_TEST_ASSERT(pop_result.id == 3, "Testing condition: pop_result.id == 3", "Review logic surrounding pop_result.id == 3");
    PROVEN_TEST_ASSERT(arr.len == 2, "Testing condition: arr.len == 2", "Review logic surrounding arr.len == 2");
    
    // Bounds guard null tests
    PROVEN_TEST_ASSERT(PROVEN_ARRAY_GET(&arr, test_player_t, 5) == NULL, "Testing condition: PROVEN_ARRAY_GET(&arr, test_player_t, 5) == NULL", "Review logic surrounding PROVEN_ARRAY_GET(&arr, test_player_t, 5) == NULL");

    PROVEN_ARRAY_DESTROY(&arr);

    // 4. Zero-Overhead Arena Array Integration
    PROVEN_TEST_INFO("Testing zero-overhead arena array integration...");
    // Embodying Best Practices - Stack-based Arena logic integration
    alignas(max_align_t) proven_byte_t stack_mem[512]; 
    proven_mem_mut_t back = { .ptr = stack_mem, .size = sizeof(stack_mem) };
    proven_arena_t arena = proven_arena_create(back);
    proven_allocator_t arena_alloc = proven_arena_as_allocator(&arena);

    proven_result_array_t a_res = PROVEN_ARRAY_INIT(arena_alloc, int, 2);
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(a_res.err), "Testing condition: PROVEN_IS_OK(a_res.err)", "Review logic surrounding PROVEN_IS_OK(a_res.err)");
    proven_array_t int_arr = a_res.value;
    
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&int_arr, int, 100)), "Testing condition: PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&int_arr, int, 100))", "Review logic surrounding PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&int_arr, int, 100))");
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&int_arr, int, 200)), "Testing condition: PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&int_arr, int, 200))", "Review logic surrounding PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&int_arr, int, 200))");
    
    void* initial_arena_ptr = int_arr.data;
    // Push 3rd triggering Arena Growth
    // With highly optimized zero-copy traits, this expansion occurs perfectly transparently without migrations!
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&int_arr, int, 300)), "Testing condition: PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&int_arr, int, 300))", "Review logic surrounding PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&int_arr, int, 300))");
    
    PROVEN_TEST_ASSERT(int_arr.data == initial_arena_ptr, "Testing condition: int_arr.data == initial_arena_ptr", "Review logic surrounding int_arr.data == initial_arena_ptr"); // CONFIRM ZERO-COPY EXTENSION! The pointer did not move!
    PROVEN_TEST_ASSERT(*PROVEN_ARRAY_GET(&int_arr, int, 0) == 100, "Testing condition: *PROVEN_ARRAY_GET(&int_arr, int, 0) == 100", "Review logic surrounding *PROVEN_ARRAY_GET(&int_arr, int, 0) == 100");
    PROVEN_TEST_ASSERT(*PROVEN_ARRAY_GET(&int_arr, int, 1) == 200, "Testing condition: *PROVEN_ARRAY_GET(&int_arr, int, 1) == 200", "Review logic surrounding *PROVEN_ARRAY_GET(&int_arr, int, 1) == 200");
    PROVEN_TEST_ASSERT(*PROVEN_ARRAY_GET(&int_arr, int, 2) == 300, "Testing condition: *PROVEN_ARRAY_GET(&int_arr, int, 2) == 300", "Review logic surrounding *PROVEN_ARRAY_GET(&int_arr, int, 2) == 300");
    
    // Test what happens when another unconnected allocation interrupts the tail!
    (void)proven_arena_alloc_aligned(&arena, 16, 8); // Claim space!
    
    // Array pushes 4th. (Still has capacity, 4 <= 4)
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&int_arr, int, 400)), "Testing condition: PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&int_arr, int, 400))", "Review logic surrounding PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&int_arr, int, 400))"); 
    PROVEN_TEST_ASSERT(int_arr.data == initial_arena_ptr, "Testing condition: int_arr.data == initial_arena_ptr", "Review logic surrounding int_arr.data == initial_arena_ptr"); 

    // Array pushes 5th. This TRIGGERS expansion (5 > 4).
    // Since we interrupted the tail at line 94, this MUST move now safely avoiding overlap!
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&int_arr, int, 500)), "Testing condition: PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&int_arr, int, 500))", "Review logic surrounding PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&int_arr, int, 500))");
    PROVEN_TEST_ASSERT(int_arr.data != initial_arena_ptr, "Testing condition: int_arr.data != initial_arena_ptr", "Review logic surrounding int_arr.data != initial_arena_ptr"); 
    PROVEN_TEST_ASSERT(*PROVEN_ARRAY_GET(&int_arr, int, 4) == 500, "Testing condition: *PROVEN_ARRAY_GET(&int_arr, int, 4) == 500", "Review logic surrounding *PROVEN_ARRAY_GET(&int_arr, int, 4) == 500");

    // And here is the magic trait. Calling destroy invokes arena's No-Op free! 
    PROVEN_ARRAY_DESTROY(&int_arr); 
    
    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("editing in place agrees with a plain array",
        "RFC-0009 X-002: clear, truncate, insert, remove_at, swap_remove and extend, against a hand-written model over 20,000 random operations - with elements taken from the array itself, and with growth refused part of the time.",
        "Inspect the new functions at the end of src/proven/array.c; a mismatch prints the operation number.");
    // ---------------------------------------------------------------
    {
        proven_allocator_t a = { .ctx = NULL, .alloc_fn = x2_alloc, .realloc_fn = x2_realloc, .free_fn = x2_free };
        proven_result_array_t ar = PROVEN_ARRAY_INIT(a, int, 0);
        PROVEN_TEST_ASSERT(proven_is_ok(ar.err), "created", "");
        proven_array_t *arr = &ar.value;
        static int model[4096];
        size_t n = 0;
        proven_u64 s = 0x9e3779b97f4a7c15ull;
        for (int op = 0; op < 20000; ++op) {
            s ^= s << 13; s ^= s >> 7; s ^= s << 17;
            unsigned kind = (unsigned)(s % 7u);
            size_t r = (size_t)(s >> 16);
            g_refuse_grow = ((s >> 40) % 10u) == 0;   /* one grow in ten is refused */
            proven_err_t e = PROVEN_OK;
            if (kind == 0 && n < 4000) {                       /* insert a fresh value */
                size_t at = r % (n + 1); int v = op;
                e = proven_array_insert(arr, at, &v);
                if (proven_is_ok(e)) { memmove(model + at + 1, model + at, (n - at) * sizeof(int)); model[at] = v; ++n; }
            } else if (kind == 1 && n > 0 && n < 4000) {       /* insert one of its own elements */
                size_t at = r % (n + 1), from = (r >> 12) % n;
                int v = model[from];
                e = proven_array_insert(arr, at, proven_array_get(arr, from));
                if (proven_is_ok(e)) { memmove(model + at + 1, model + at, (n - at) * sizeof(int)); model[at] = v; ++n; }
            } else if (kind == 2 && n > 0) {
                size_t at = r % n; int got = -1;
                e = proven_array_remove_at(arr, at, &got);
                PROVEN_TEST_ASSERT(proven_is_ok(e) && got == model[at], "remove_at hands back the element", "");
                memmove(model + at, model + at + 1, (n - at - 1) * sizeof(int)); --n;
            } else if (kind == 3 && n > 0) {
                size_t at = r % n; int got = -1;
                e = proven_array_swap_remove(arr, at, &got);
                PROVEN_TEST_ASSERT(proven_is_ok(e) && got == model[at], "swap_remove hands back the element", "");
                model[at] = model[n - 1]; --n;
            } else if (kind == 4 && n > 0 && 2 * n <= 4000) {  /* extend with a slice of itself */
                size_t from = r % n, cnt = 1 + (r >> 12) % (n - from);
                e = proven_array_extend(arr, proven_array_get(arr, from), cnt);
                if (proven_is_ok(e)) { memmove(model + n, model + from, cnt * sizeof(int)); n += cnt; }
            } else if (kind == 5) {
                size_t to = n ? r % (n + 1) : 0;
                e = proven_array_truncate(arr, to);
                PROVEN_TEST_ASSERT(proven_is_ok(e), "truncate to a shorter length", "");
                n = to;
            } else if (kind == 6 && (r % 50u) == 0) {
                proven_array_clear(arr); n = 0;
            }
            PROVEN_TEST_ASSERT(proven_is_ok(e) || e == PROVEN_ERR_NOMEM, "only a refused grow may fail", "");
            PROVEN_TEST_ASSERT(arr->len == n, "the length follows the model", "");
            bool same = n == 0 || memcmp(arr->data, model, n * sizeof(int)) == 0;
            if (!same) PROVEN_TEST_INFO("first mismatch after operation {}", PROVEN_ARG(op));
            PROVEN_TEST_ASSERT(same, "the contents follow the model, including after a refused grow", "");
        }
        g_refuse_grow = false;
        int v = 0;
        PROVEN_TEST_ASSERT(proven_array_truncate(arr, arr->len + 1) == PROVEN_ERR_OUT_OF_BOUNDS, "truncate cannot lengthen", "");
        PROVEN_TEST_ASSERT(proven_array_insert(arr, arr->len + 1, &v) == PROVEN_ERR_OUT_OF_BOUNDS, "insert past the end", "");
        PROVEN_TEST_ASSERT(proven_array_remove_at(arr, arr->len, NULL) == PROVEN_ERR_OUT_OF_BOUNDS, "remove_at past the end", "");
        PROVEN_TEST_ASSERT(proven_array_swap_remove(arr, arr->len, NULL) == PROVEN_ERR_OUT_OF_BOUNDS, "swap_remove past the end", "");
        PROVEN_TEST_ASSERT(proven_array_extend(arr, NULL, 1) == PROVEN_ERR_INVALID_ARG, "extend from NULL", "");
        PROVEN_TEST_ASSERT(proven_array_extend(arr, NULL, 0) == PROVEN_OK, "extend by nothing", "");
        if (arr->len >= 2) {
            /* a source that starts inside the array and runs past its storage */
            const int *last = proven_array_get(arr, arr->len - 1);
            size_t past = arr->cap - arr->len + 2;
            PROVEN_TEST_ASSERT(proven_array_extend(arr, last, past) == PROVEN_ERR_INVALID_ARG,
                "a source that only partly overlaps the array is refused", "");
        }
        proven_array_destroy(arr);
    }

    PROVEN_TEST_PASS("All Phase 8 Dynamic Array Tests Passed Successfully!");
    return 0;
}
