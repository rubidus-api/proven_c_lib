#include "proven.h"
#include "proven_test.h"
#include <stdalign.h>

static int panic_triggered = 0;

static void on_panic(const char *msg) {
    (void)msg;
    panic_triggered = 1;
}

static void check_pool_double_free_hardening(void) {
    PROVEN_TEST_SECTION(
        "pool double-free hardening",
        "Verify pool free detects a repeated free when debug validation or hardened validation is enabled.",
        "Inspect the pool free trait gating if a repeated free is accepted silently or if the panic hook is not reached."
    );

    proven_allocator_t heap = proven_heap_allocator();
    proven_pool_t pool = {0};
    proven_err_t init_err = proven_pool_init(&pool, heap, sizeof(int), alignof(int), 1);
    PROVEN_TEST_ASSERT(init_err == PROVEN_OK, "Pool should initialize for misuse testing", "Check the pool allocator setup if initialization fails.");

    proven_allocator_t alloc = proven_pool_as_allocator(&pool);
    proven_result_mem_mut_t block = alloc.alloc_fn(alloc.ctx, sizeof(int), alignof(int));
    PROVEN_TEST_ASSERT(block.err == PROVEN_OK && block.value.ptr != NULL, "Pool should hand out one fixed-size block", "Inspect pool allocation routing if the first allocation fails.");

    alloc.free_fn(alloc.ctx, block.value.ptr);

#if PROVEN_HARDENED || !defined(NDEBUG)
    panic_triggered = 0;
    alloc.free_fn(alloc.ctx, block.value.ptr);
    PROVEN_TEST_ASSERT(panic_triggered == 1, "Pool double free should trigger the panic hook under validation", "Inspect the pool double-free guard if the panic hook was not reached.");
#endif

    proven_pool_destroy(&pool);
    PROVEN_TEST_PASS("Pool double-free hardening checks passed.");
}

int main(void) {
    PROVEN_TEST_INFO("Running pool misuse hardening checks...");
    proven_set_panic_handler(on_panic);
    check_pool_double_free_hardening();
    return 0;
}
