#include "proven.h"
#include "proven_test.h"
#include <string.h>

/*
 * The allocator is a TRAIT: code is written against it without knowing which allocator it
 * will be handed. That only works if every allocator answers the same question the same
 * way, and three questions were answered differently:
 *
 *   - alloc(0): the heap said PROVEN_ERR_NOMEM (a lie - nothing was out of memory) and the
 *     arena said PROVEN_OK with a live pointer to nothing.
 *   - realloc(ptr, 0): the trait says "frees the block and returns a null pointer with
 *     PROVEN_OK". The heap did. The arena returned a NON-null pointer, so trait-generic
 *     code testing `ptr == NULL` to learn the block was gone did the wrong thing on one of
 *     the two allocators.
 *   - a pure SHRINK of a non-tail arena block failed with PROVEN_ERR_NOMEM on a full
 *     arena, which is an absurd answer to "please use less memory".
 *
 * Every check below runs against BOTH allocators, from the same code, which is the point.
 */

static void check_trait(const char *who, proven_allocator_t a) {
    /* alloc(0) is a caller bug, and both must say so the same way. */
    proven_result_mem_mut_t z = a.alloc_fn(a.ctx, 0, 8);
    PROVEN_TEST_ASSERT(z.err == PROVEN_ERR_INVALID_ARG,
        "alloc(0) must be PROVEN_ERR_INVALID_ARG",
        "The heap used to call it NOMEM and the arena used to call it OK. A trait whose answer depends on the allocator is not a trait.");
    (void)who;

    /* A real allocation, then a realloc to zero: the block is gone and the pointer is NULL. */
    proven_result_mem_mut_t m = a.alloc_fn(a.ctx, 64, 8);
    PROVEN_TEST_ASSERT(proven_is_ok(m.err) && m.value.ptr != NULL, "a 64-byte allocation must succeed", "");
    memset(m.value.ptr, 0xAB, 64);

    proven_result_mem_mut_t gone = a.realloc_fn(a.ctx, m.value.ptr, 64, 0, 8);
    PROVEN_TEST_ASSERT(proven_is_ok(gone.err),
        "realloc(ptr, 0) must be PROVEN_OK", "");
    PROVEN_TEST_ASSERT(gone.value.ptr == NULL,
        "realloc(ptr, 0) must return a NULL pointer, as the trait documents",
        "The arena used to return a live pointer, so a caller that tests for NULL to learn the block is gone got a different answer per allocator.");

    /* Alignment: what is asked for is what is delivered, for every allocator. */
    const proven_size_t aligns[] = { 8, 16, 32, 64, 128, 256 };
    for (proven_size_t i = 0; i < sizeof aligns / sizeof aligns[0]; ++i) {
        proven_result_mem_mut_t p = a.alloc_fn(a.ctx, 100, aligns[i]);
        PROVEN_TEST_ASSERT(proven_is_ok(p.err), "an over-aligned allocation must succeed", "");
        PROVEN_TEST_ASSERT(((proven_uintptr_t)p.value.ptr % aligns[i]) == 0,
            "and the pointer must actually have the alignment it was asked for", "");
        a.free_fn(a.ctx, p.value.ptr);
    }
}

int main(void) {
    PROVEN_TEST_SUITE("the allocator trait means the same thing for every allocator",
        "alloc(0), realloc(ptr, 0) and alignment must not depend on which allocator the caller was handed.",
        "Inspect src/proven/heap.c, src/proven/arena.c, and the contract in include/proven/allocator.h.");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("the heap allocator",
        "The reference implementation of the trait.",
        "");
    // ---------------------------------------------------------------
    check_trait("heap", proven_heap_allocator());

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("the arena allocator answers identically",
        "Same code, same expectations, different allocator. That is what a trait is for.",
        "");
    // ---------------------------------------------------------------
    {
        static proven_byte_t backing[64 * 1024];
        proven_arena_t arena = proven_arena_create(
            (proven_mem_mut_t){ .ptr = backing, .size = sizeof backing });
        check_trait("arena", proven_arena_as_allocator(&arena));
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("shrinking never fails for want of memory",
        "\"Please use less\" cannot sensibly be answered with PROVEN_ERR_NOMEM.",
        "A non-tail arena shrink used to route through a fresh tail allocation, so it failed once the arena was full.");
    // ---------------------------------------------------------------
    {
        static proven_byte_t backing[256];
        proven_arena_t arena = proven_arena_create(
            (proven_mem_mut_t){ .ptr = backing, .size = sizeof backing });
        proven_allocator_t a = proven_arena_as_allocator(&arena);

        proven_result_mem_mut_t first = a.alloc_fn(a.ctx, 64, 8);
        PROVEN_TEST_ASSERT(proven_is_ok(first.err), "setup: a block", "");
        memset(first.value.ptr, 'x', 64);

        /* Fill the arena so no fresh allocation can succeed - and pin `first` as non-tail. */
        for (;;) {
            proven_result_mem_mut_t f = a.alloc_fn(a.ctx, 16, 8);
            if (!proven_is_ok(f.err)) break;
        }

        proven_result_mem_mut_t smaller = a.realloc_fn(a.ctx, first.value.ptr, 64, 16, 8);
        PROVEN_TEST_ASSERT(proven_is_ok(smaller.err),
            "shrinking a non-tail block in a FULL arena must still succeed",
            "It used to be PROVEN_ERR_NOMEM: the shrink was implemented as a fresh allocation plus a copy, and there was nothing left to allocate.");
        PROVEN_TEST_ASSERT(smaller.value.size == 16, "and the block must be the size that was asked for", "");
        PROVEN_TEST_ASSERT(((const unsigned char *)smaller.value.ptr)[0] == 'x',
            "and it must still hold the data it held", "");
    }

    PROVEN_TEST_PASS("both allocators keep the same contract.");
    return 0;
}
