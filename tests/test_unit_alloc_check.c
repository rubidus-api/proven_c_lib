#include "proven.h"
#include "proven_test.h"
#include <string.h>

/*
 * alloc_check.h (B-040), from its contract. A panic handler that RETURNS is
 * installed, so each refusal is observed as a message and a `faults` count instead of a trap -
 * and the test then checks that the refused pointer never reached the inner allocator: under
 * ASan a forwarded foreign or double free is a report, not a silent pass.
 *
 * This translation unit does NOT define PROVEN_ALLOC_CHECK, so it also pins that
 * proven_alloc_checked() is the identity here; the manual's example defines it and shows the
 * other side.
 */

static int g_panics;
static char g_last[160];
static void on_panic(const char *msg) {
    ++g_panics;
    size_t n = strlen(msg);
    if (n >= sizeof g_last) n = sizeof g_last - 1;
    memcpy(g_last, msg, n);
    g_last[n] = 0;
}

static bool last_says(const char *word) { return strstr(g_last, word) != NULL; }

int main(void) {
    PROVEN_TEST_SUITE("an allocator that knows its own blocks",
        "The alloc_check wrapper passes correct use straight through, and refuses - with a panic, at the call - a foreign free, a double free, a realloc of a foreign block or with the wrong old size or alignment, and an allocation past its record; nothing refused reaches the inner allocator.",
        "Inspect src/proven/alloc_check.c. A refusal that is not reported is a missing find_entry check; an ASan report after a refusal means the bad pointer was forwarded.");
    proven_set_panic_handler(on_panic);
    proven_allocator_t heap = proven_heap_allocator();

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("correct use passes through and is counted", "", "");
    // ---------------------------------------------------------------
    {
        proven_alloc_check_t st;
        proven_alloc_check_entry_t rec[8];
        proven_allocator_t a = proven_alloc_check_wrap(&st, heap, rec, 8);
        PROVEN_TEST_ASSERT(proven_alloc_is_valid(a), "wrap gives a valid allocator", "");
        proven_result_mem_mut_t m1 = a.alloc_fn(a.ctx, 32, 8);
        proven_result_mem_mut_t m2 = a.alloc_fn(a.ctx, 64, 16);
        PROVEN_TEST_ASSERT(proven_is_ok(m1.err) && proven_is_ok(m2.err), "allocations succeed", "");
        PROVEN_TEST_ASSERT(proven_alloc_check_live(&st) == 2 && st.live_bytes == 96, "two live blocks, 96 bytes", "");
        PROVEN_TEST_ASSERT(proven_alloc_check_owns(&st, m1.value.ptr) && !proven_alloc_check_owns(&st, &st), "owns what it gave", "");
        proven_result_mem_mut_t g = a.realloc_fn(a.ctx, m1.value.ptr, 32, 128, 8);
        PROVEN_TEST_ASSERT(proven_is_ok(g.err) && st.live_bytes == 192 && proven_alloc_check_owns(&st, g.value.ptr),
            "a correct realloc moves the record with the block", "");
        proven_result_mem_mut_t z = a.realloc_fn(a.ctx, g.value.ptr, 128, 0, 8);
        PROVEN_TEST_ASSERT(proven_is_ok(z.err) && proven_alloc_check_live(&st) == 1, "realloc to 0 frees and forgets", "");
        a.free_fn(a.ctx, m2.value.ptr);
        a.free_fn(a.ctx, NULL);
        PROVEN_TEST_ASSERT(proven_alloc_check_live(&st) == 0 && st.live_bytes == 0 && st.peak_live == 2,
            "all freed: nothing live, peak remembered", "");
        PROVEN_TEST_ASSERT(g_panics == 0 && st.faults == 0, "correct use never panics", "");
        PROVEN_TEST_ASSERT(!proven_alloc_is_valid(proven_alloc_check_wrap(NULL, heap, rec, 8)) &&
                           !proven_alloc_is_valid(proven_alloc_check_wrap(&st, (proven_allocator_t){0}, rec, 8)) &&
                           !proven_alloc_is_valid(proven_alloc_check_wrap(&st, heap, NULL, 8)),
            "bad arguments give an invalid allocator", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("misuse is refused at the call, and never forwarded", "", "");
    // ---------------------------------------------------------------
    {
        proven_alloc_check_t st;
        proven_alloc_check_entry_t rec[2];
        proven_allocator_t a = proven_alloc_check_wrap(&st, heap, rec, 2);

        proven_result_mem_mut_t foreign = heap.alloc_fn(heap.ctx, 16, 8);   /* not this wrapper's */
        a.free_fn(a.ctx, foreign.value.ptr);
        PROVEN_TEST_ASSERT(g_panics == 1 && last_says("does not own"), "a foreign free panics", "");
        heap.free_fn(heap.ctx, foreign.value.ptr);   /* still valid: the wrapper did not free it */

        proven_result_mem_mut_t m = a.alloc_fn(a.ctx, 16, 8);
        a.free_fn(a.ctx, m.value.ptr);
        a.free_fn(a.ctx, m.value.ptr);
        PROVEN_TEST_ASSERT(g_panics == 2 && last_says("double free"), "a double free panics, and is not forwarded", "");

        m = a.alloc_fn(a.ctx, 16, 8);
        proven_result_mem_mut_t r = a.realloc_fn(a.ctx, m.value.ptr, 32, 64, 8);
        PROVEN_TEST_ASSERT(g_panics == 3 && r.err == PROVEN_ERR_INVALID_ARG && last_says("old size"),
            "a realloc with the wrong old size panics", "");
        r = a.realloc_fn(a.ctx, m.value.ptr, 16, 64, 16);
        PROVEN_TEST_ASSERT(g_panics == 4 && r.err == PROVEN_ERR_INVALID_ARG, "and with the wrong alignment", "");
        PROVEN_TEST_ASSERT(proven_alloc_check_owns(&st, m.value.ptr), "the block is untouched and still owned", "");

        int local = 0;
        r = a.realloc_fn(a.ctx, &local, 4, 8, 8);
        PROVEN_TEST_ASSERT(g_panics == 5 && r.err == PROVEN_ERR_INVALID_ARG, "a realloc of a foreign pointer panics", "");

        proven_result_mem_mut_t m2 = a.alloc_fn(a.ctx, 8, 8);
        proven_result_mem_mut_t m3 = a.alloc_fn(a.ctx, 8, 8);   /* record of 2 is full */
        PROVEN_TEST_ASSERT(proven_is_ok(m2.err) && g_panics == 6 && m3.err == PROVEN_ERR_NOMEM && last_says("record is full"),
            "an allocation past the record panics and fails, without leaking the block", "");
        PROVEN_TEST_ASSERT(st.faults == 6, "every refusal is counted", "");
        a.free_fn(a.ctx, m.value.ptr);
        a.free_fn(a.ctx, m2.value.ptr);
        PROVEN_TEST_ASSERT(proven_alloc_check_live(&st) == 0, "and the correct frees still work", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("the B-023 mistake is caught where it is made",
        "A string made through a checked arena and destroyed through a checked heap: the heap refuses it at the destroy, instead of corrupting itself for a crash somewhere later.",
        "");
    // ---------------------------------------------------------------
    {
        static proven_byte_t arena_mem[1024];
        proven_arena_t arena = proven_arena_create((proven_mem_mut_t){ arena_mem, sizeof arena_mem });
        proven_alloc_check_t sa, sh;
        proven_alloc_check_entry_t ra[4], rh[4];
        proven_allocator_t a_arena = proven_alloc_check_wrap(&sa, proven_arena_as_allocator(&arena), ra, 4);
        proven_allocator_t a_heap = proven_alloc_check_wrap(&sh, heap, rh, 4);

        proven_result_u8str_t r = proven_u8str_create(a_arena, 32);
        PROVEN_TEST_ASSERT(proven_is_ok(r.err), "made in the arena", "");
        proven_u8str_t s = r.value;
        int before = g_panics;
        proven_u8str_destroy(a_heap, &s);
        PROVEN_TEST_ASSERT(g_panics == before + 1 && sh.faults == 1, "destroying it through the heap is refused", "");
        PROVEN_TEST_ASSERT(proven_alloc_check_live(&sa) == 1, "the arena still owns it", "");

        s = r.value;
        proven_u8str_destroy(a_arena, &s);
        PROVEN_TEST_ASSERT(g_panics == before + 1 && proven_alloc_check_live(&sa) == 0, "the right allocator frees it", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("off unless asked for",
        "Without PROVEN_ALLOC_CHECK in the translation unit, proven_alloc_checked returns the inner allocator itself.",
        "");
    // ---------------------------------------------------------------
    {
        proven_alloc_check_t st;
        proven_alloc_check_entry_t rec[1];
        proven_allocator_t a = proven_alloc_checked(&st, heap, rec, 1);
        PROVEN_TEST_ASSERT(a.alloc_fn == heap.alloc_fn && a.free_fn == heap.free_fn && a.ctx == heap.ctx,
            "the identity: zero cost when the macro is off", "");
    }

    proven_set_panic_handler(NULL);
    PROVEN_TEST_PASS("an allocator that knows its own blocks");
    return 0;
}
