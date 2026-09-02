#include "example.h"

/*
 * Three things a program that owns its own memory eventually has to do, and the
 * calls that do them:
 *
 *   - Allocate during start-up, where running out of memory is not a condition
 *     the program can carry on from. That is what the `_or_panic` calls are
 *     for, and installing a panic handler is how you decide what "cannot carry
 *     on" means for your program - a log line and an exit, rather than a trap.
 *
 *   - Grow the block you allocated last, without copying it. An arena can do
 *     that in place, because the block that was allocated last is the one
 *     sitting at the end of the used region.
 *
 *   - Instrument the allocator - count allocations, or fail the tenth one on
 *     purpose in a test - without changing the code being measured. An
 *     allocator here is three function pointers and a context pointer, so
 *     wrapping one is writing three forwarding functions. The arena's own three
 *     are public for exactly this reason: your wrapper forwards to them.
 */

/* --- what a panic handler is for ----------------------------------------- */

static int g_panics = 0;
static char g_last_panic[128];

/* A panic handler receives the message and decides the program's fate. The
 * default one traps immediately, which is right in production and useless in a
 * test - so this one records the message and returns. Returning is allowed ONLY
 * when you are deliberately testing the panic path, and the memory block the
 * panicking call returns must then not be used. */
static void record_panic(const char *msg) {
    ++g_panics;
    snprintf(g_last_panic, sizeof g_last_panic, "%s", msg);
}

/* --- an allocator that counts what passes through it ---------------------- */

typedef struct {
    proven_arena_t *arena;
    proven_size_t   live_bytes;
    proven_size_t   alloc_calls;
    proven_size_t   free_calls;
} counting_ctx_t;

/* Each of the three matches one field of proven_allocator_t. Each does its own
 * bookkeeping and then forwards to the arena's public trait function, so the
 * behaviour being measured is exactly the arena's behaviour and not a
 * re-implementation of it. */
static proven_result_mem_mut_t counting_alloc(void *ctx, proven_size_t size, proven_size_t align) {
    counting_ctx_t *c = (counting_ctx_t *)ctx;
    proven_result_mem_mut_t r = proven_arena_alloc_trait(c->arena, size, align);
    if (proven_is_ok(r.err)) {
        c->live_bytes += size;
        ++c->alloc_calls;
    }
    return r;
}

static proven_result_mem_mut_t counting_realloc(void *ctx, void *old_ptr, proven_size_t old_size,
                                               proven_size_t new_size, proven_size_t align) {
    counting_ctx_t *c = (counting_ctx_t *)ctx;
    proven_result_mem_mut_t r = proven_arena_realloc_trait(c->arena, old_ptr, old_size, new_size, align);
    if (proven_is_ok(r.err)) {
        c->live_bytes = c->live_bytes - old_size + new_size;
    }
    return r;
}

static void counting_free(void *ctx, void *ptr) {
    counting_ctx_t *c = (counting_ctx_t *)ctx;
    ++c->free_calls;
    proven_arena_free_trait(c->arena, ptr);   /* an arena free is a no-op; the count is the point */
}

int main(void) {
    alignas(max_align_t) static proven_byte_t storage[1024];
    proven_arena_t arena = proven_arena_create((proven_mem_mut_t){ .ptr = storage, .size = sizeof storage });

    /* --- 1. start-up allocation that must not fail ------------------------ */

    proven_set_panic_handler(record_panic);

    /* No result to unwrap: these return the block directly, because there is no
     * error the caller could act on. That is the entire difference. */
    proven_mem_mut_t table = proven_arena_alloc_or_panic(&arena, 256);
    EXAMPLE_REQUIRE(table.ptr != NULL, "a 256-byte start-up allocation must succeed");
    EXAMPLE_REQUIRE(g_panics == 0, "a successful allocation must not panic");

    /* The aligned form, for a type that needs more than the default boundary -
     * a 64-byte cache line here, the usual reason. */
    proven_mem_mut_t cache_line = proven_arena_alloc_aligned_or_panic(&arena, 64, 64);
    EXAMPLE_REQUIRE(((proven_uintptr_t)cache_line.ptr % 64) == 0,
                    "the block must start on the boundary that was asked for");
    EXAMPLE_REQUIRE(g_panics == 0, "an over-aligned allocation that fits must not panic either");

    /* Now the failing case, on purpose: more than the arena could ever hold.
     * With the recording handler installed we can observe it; with the default
     * handler the program would stop here, which is what it is for. */
    proven_mem_mut_t impossible = proven_arena_alloc_or_panic(&arena, sizeof storage * 2);
    (void)impossible;   /* after a handler returns, this block means nothing */
    EXAMPLE_REQUIRE(g_panics == 1, "exhausting the arena through _or_panic must panic");
    EXAMPLE_REQUIRE(g_last_panic[0] != '\0', "the handler receives a message naming the call");
    printf("panic handler saw: %s\n", g_last_panic);

    /* Passing NULL puts the default trapping handler back. Leaving a test
     * handler installed turns a real failure into silent corruption. */
    proven_set_panic_handler(NULL);

    /* --- 2. growing the most recent block in place ------------------------ */

    /* A parser that reads a header, then discovers the body is longer than it
     * guessed, wants to extend the buffer it just took - not copy it. */
    proven_result_mem_mut_t buf = proven_arena_alloc_aligned(&arena, 32, alignof(proven_u32));
    EXAMPLE_REQUIRE(proven_is_ok(buf.err), "the initial 32-byte buffer must fit");

    proven_size_t before = arena.offset;
    proven_result_mem_mut_t grown = proven_arena_realloc_aligned(&arena, buf.value.ptr, 32, 96, alignof(proven_u32));
    EXAMPLE_REQUIRE(proven_is_ok(grown.err), "growing the most recent block must succeed");
    EXAMPLE_REQUIRE(grown.value.ptr == buf.value.ptr,
                    "the most recent block grows in place: same address, no copy");
    EXAMPLE_REQUIRE(arena.offset == before + 64, "only the extra 64 bytes were taken");

    /* The in-place path is available only for the block allocated LAST. Take
     * another block, and the earlier one can no longer be extended where it
     * stands - the arena copies it to the end instead, and the old bytes are
     * dead until the next reset. Correct either way; just not free. */
    proven_result_mem_mut_t other = proven_arena_alloc(&arena, 16);
    EXAMPLE_REQUIRE(proven_is_ok(other.err), "a second block must fit");
    proven_result_mem_mut_t moved = proven_arena_realloc_aligned(&arena, grown.value.ptr, 96, 128, alignof(proven_u32));
    EXAMPLE_REQUIRE(proven_is_ok(moved.err), "growing an older block must still succeed");
    EXAMPLE_REQUIRE(moved.value.ptr != grown.value.ptr, "but it is relocated, not extended");

    /* --- 3. the arena behind a counting wrapper --------------------------- */

    proven_arena_reset(&arena);

    counting_ctx_t counted = { .arena = &arena };
    proven_allocator_t alloc = {
        .ctx        = &counted,
        .alloc_fn   = counting_alloc,
        .realloc_fn = counting_realloc,
        .free_fn    = counting_free,
    };
    EXAMPLE_REQUIRE(proven_alloc_is_valid(alloc), "all three function pointers must be present");

    /* Any part of the library that takes an allocator now runs through the
     * wrapper, unchanged and unaware. */
    proven_result_u8str_t s = proven_u8str_create(alloc, 16);
    EXAMPLE_REQUIRE(proven_is_ok(s.err), "creating a string through the wrapper must succeed");

    proven_err_t err = proven_u8str_append_grow(alloc, &s.value, PROVEN_LIT("a line long enough to need more room"));
    EXAMPLE_REQUIRE(proven_is_ok(err), "appending past the initial capacity must succeed");

    proven_u8str_destroy(alloc, &s.value);

    EXAMPLE_REQUIRE(counted.alloc_calls >= 1, "the wrapper saw the string being created");
    EXAMPLE_REQUIRE(counted.free_calls >= 1, "and saw it being destroyed");
    printf("counting allocator: %zu alloc call(s), %zu free call(s), %zu bytes handed out\n",
           (size_t)counted.alloc_calls, (size_t)counted.free_calls, (size_t)counted.live_bytes);

    proven_arena_destroy(&arena);
    return EXAMPLE_OK();
}
