/* Must come before the first proven header: alloc_check.h reads it when first included. */
#define PROVEN_ALLOC_CHECK 1
#include "proven.h"
#include "proven_test.h"

/*
 * The other half of test_unit_alloc_check's "off unless asked for": in a translation unit that
 * defines PROVEN_ALLOC_CHECK before including proven, proven_alloc_checked() really wraps - the
 * switch a caller flips with -DPROVEN_ALLOC_CHECK. Written after the manual's example showed that
 * a #define placed after the include silently leaves the checks off.
 */

static int g_panics;
static void on_panic(const char *msg) { (void)msg; ++g_panics; }

int main(void) {
    PROVEN_TEST_SUITE("alloc_check switched on by its macro",
        "With PROVEN_ALLOC_CHECK defined first, proven_alloc_checked returns a checking allocator that refuses a foreign free.",
        "If this sees the inner allocator, the #ifdef in proven_alloc_checked is not reading the macro.");
    proven_set_panic_handler(on_panic);
    proven_allocator_t heap = proven_heap_allocator();
    proven_alloc_check_t st;
    proven_alloc_check_entry_t rec[4];
    proven_allocator_t a = proven_alloc_checked(&st, heap, rec, 4);
    PROVEN_TEST_ASSERT(a.ctx == &st && a.alloc_fn != heap.alloc_fn, "the macro turns the wrapper on", "");
    proven_result_mem_mut_t foreign = heap.alloc_fn(heap.ctx, 8, 8);
    a.free_fn(a.ctx, foreign.value.ptr);
    PROVEN_TEST_ASSERT(g_panics == 1 && st.faults == 1, "and it refuses a foreign free", "");
    heap.free_fn(heap.ctx, foreign.value.ptr);
    proven_set_panic_handler(NULL);
    PROVEN_TEST_PASS("alloc_check switched on by its macro");
    return 0;
}
