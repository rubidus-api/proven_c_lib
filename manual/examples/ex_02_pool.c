#include "example.h"

/*
 * A pool is a churn optimizer, not a region. It is for one type: allocate and
 * free the same fixed-size block over and over - list nodes, events, particles -
 * without paying malloc every time.
 *
 * It keeps a small stack of freed blocks (the "bin"). Freeing pushes a block
 * onto the bin instead of returning it to the base allocator; allocating pops
 * one back off. Both are O(1) and neither touches the heap. That recycling is
 * the entire point, and the check below proves it happens.
 *
 * Ownership: the pool caches freed blocks, but it does NOT track the blocks it
 * has handed out. Every block you take, you must give back before destroy - the
 * pool cannot free what it does not know about.
 */

typedef struct {
    int id;
    int score;
} node_t;

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();
    EXAMPLE_REQUIRE(proven_alloc_is_valid(heap), "hosted builds have a heap allocator");

    /* The pool takes a base allocator for the blocks it cannot serve from the
     * bin, plus the exact size and alignment of the one type it manages. The
     * last argument caps how many freed blocks are parked for reuse. */
    proven_pool_t pool = {0};
    proven_err_t err = proven_pool_init(&pool, heap, sizeof(node_t), alignof(node_t), 4);
    EXAMPLE_REQUIRE(proven_is_ok(err), "initializing a pool of node_t must succeed");
    if (!proven_is_ok(err)) {
        return 1;
    }

    proven_allocator_t nodes = proven_pool_as_allocator(&pool);

    /* --- first block: nothing in the bin, so it comes from the heap --------- */
    proven_result_mem_mut_t first = nodes.alloc_fn(nodes.ctx, sizeof(node_t), alignof(node_t));
    EXAMPLE_REQUIRE(proven_is_ok(first.err), "the pool must be able to serve its own item type");
    if (!proven_is_ok(first.err)) {
        proven_pool_destroy(&pool);
        return 1;
    }

    node_t *n = (node_t *)first.value.ptr;
    *n = (node_t){ .id = 1, .score = 100 };
    void *first_addr = n;

    /* --- hand it back: it lands in the bin, not back on the heap ------------ */
    nodes.free_fn(nodes.ctx, n);
    EXAMPLE_REQUIRE(pool.bin_len == 1, "a freed block is cached for reuse, not returned to the heap");
    /* `n` is dangling from here on. The pool owns those bytes again. */

    /* --- second block: the freed one is handed straight back ---------------- */
    proven_result_mem_mut_t second = nodes.alloc_fn(nodes.ctx, sizeof(node_t), alignof(node_t));
    EXAMPLE_REQUIRE(proven_is_ok(second.err), "allocating from a non-empty bin must succeed");
    EXAMPLE_REQUIRE(second.value.ptr == first_addr, "the recycled block is the one that was freed");
    EXAMPLE_REQUIRE(pool.bin_len == 0, "taking it back out empties the bin");

    /* Recycled memory is NOT zeroed for you - it is whatever the pool left there.
     * Initialize every field, exactly as you would for a fresh malloc. */
    node_t *m = (node_t *)second.value.ptr;
    *m = (node_t){ .id = 2, .score = 50 };
    EXAMPLE_REQUIRE(m->id == 2, "the recycled block is ours to overwrite");

    /* --- one pool serves one size and one alignment ------------------------- */
    /* A request for anything else is refused: this is not a general allocator, and it will
     * not silently hand you a block of the wrong size. The code is PROVEN_ERR_UNSUPPORTED -
     * "not my job" - and not INVALID_ARG, which would read as "you passed me garbage" and
     * send you hunting for a bug in your own code. */
    proven_result_mem_mut_t wrong = nodes.alloc_fn(nodes.ctx, sizeof(node_t) * 2, alignof(node_t));
    EXAMPLE_REQUIRE(wrong.err == PROVEN_ERR_UNSUPPORTED, "the pool only serves its configured item size");

    /* --- return every live block before destroying -------------------------- */
    /* proven_pool_destroy frees what is in the bin and the bin itself. `m` is
     * still handed out, so if we skipped this free it would leak. */
    nodes.free_fn(nodes.ctx, m);

    printf("pool: %zu block(s) cached for reuse at teardown\n", (size_t)pool.bin_len);

    proven_pool_destroy(&pool);
    return EXAMPLE_OK();
}
