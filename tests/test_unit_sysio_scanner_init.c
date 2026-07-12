#include "proven/sysio.h"
#include "proven/heap.h"
#include "proven_test.h"

static int partial_alloc_calls = 0;

static proven_result_mem_mut_t partial_alloc_fn(void *ctx, proven_size_t size, proven_size_t align) {
    (void)ctx;
    (void)size;
    (void)align;
    partial_alloc_calls++;
    return (proven_result_mem_mut_t){ .err = PROVEN_OK, .value = {0} };
}

int main(void) {
    PROVEN_TEST_SUITE(
        "test_unit_sysio_scanner_init",
        "Validate buffered scanner initialization rejects partial allocators and preserves the zero-safe failure path.",
        "If this fails, inspect proven_sysio_scanner_init and make sure it validates the full allocator trait before allocating the buffer."
    );

    PROVEN_TEST_SECTION(
        "partial allocator rejection",
        "A scanner initializer should reject an allocator that exposes only alloc_fn and leave the scanner untouched.",
        "Inspect the allocator validation gate in proven_sysio_scanner_init if a partial allocator is accepted or any allocation happens."
    );

    proven_allocator_t partial = {
        .ctx = NULL,
        .alloc_fn = partial_alloc_fn,
        .realloc_fn = NULL,
        .free_fn = NULL,
    };

    proven_sysio_scanner_t scanner = {0};
    proven_err_t init_err = proven_sysio_scanner_init(&scanner, proven_sysio_stdin(), partial, 16);
    PROVEN_TEST_ASSERT(init_err == PROVEN_ERR_INVALID_ARG,
                       "Partial allocator should be rejected by proven_sysio_scanner_init",
                       "Inspect allocator validity checks in proven_sysio_scanner_init.");
    PROVEN_TEST_ASSERT(partial_alloc_calls == 0,
                       "Partial allocator must not be called when it is rejected up front",
                       "Inspect the early exit path before the buffer allocation call.");
    PROVEN_TEST_ASSERT(scanner.buffer == NULL && scanner.capacity == 0 && scanner.cursor == 0 && scanner.length == 0 && !scanner.eof,
                       "Rejected initialization should leave the scanner zero-safe",
                       "Inspect the failure path in proven_sysio_scanner_init and keep the scanner cleared on error.");

#ifndef PROVEN_FREESTANDING
    PROVEN_TEST_SECTION(
        "normal allocator success",
        "A fully valid heap allocator should still initialize and deinitialize the scanner normally.",
        "If this fails, inspect the successful allocation path and scanner deinitialization.");

    proven_allocator_t heap = proven_heap_allocator();
    PROVEN_TEST_ASSERT(proven_alloc_is_valid(heap),
                       "Heap allocator should be valid in hosted builds",
                       "Inspect proven_heap_allocator if the hosted allocator trait is missing a callback.");

    proven_sysio_scanner_t ok_scanner = {0};
    proven_err_t ok_err = proven_sysio_scanner_init(&ok_scanner, proven_sysio_stdin(), heap, 16);
    PROVEN_TEST_ASSERT(ok_err == PROVEN_OK,
                       "Valid allocator should initialize the scanner",
                       "Inspect the success path in proven_sysio_scanner_init if a hosted heap allocator fails.");
    PROVEN_TEST_ASSERT(ok_scanner.buffer != NULL && ok_scanner.capacity == 16,
                       "Successful initialization should populate the scanner buffer and capacity",
                       "Inspect the successful allocation assignment path in proven_sysio_scanner_init.");
    proven_sysio_scanner_deinit(&ok_scanner);
    PROVEN_TEST_ASSERT(ok_scanner.buffer == NULL && ok_scanner.capacity == 0 && ok_scanner.cursor == 0 && ok_scanner.length == 0 && !ok_scanner.eof,
                       "Deinit should clear the scanner state",
                       "Inspect proven_sysio_scanner_deinit if stale scanner fields remain after cleanup.");
#endif

    PROVEN_TEST_PASS("--- Finished test_unit_sysio_scanner_init ---");
    return 0;
}
