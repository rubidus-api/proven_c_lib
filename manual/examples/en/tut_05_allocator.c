#include "example.h"

/*
 * Lesson 5 - who gives out the memory is an argument, not a global.
 *
 * malloc is a global decision made for you: one heap, one strategy, invisible
 * at the call site. Here, anything that needs memory takes a
 * proven_allocator_t and uses only that. Two consequences follow, and both are
 * the point.
 *
 *   - You can always answer "who allocated this?" by looking at the call.
 *   - You can hand a different allocator to the same code without changing it.
 */

/* This function does not know or care where the memory comes from. */
static proven_result_u8str_t make_greeting(proven_allocator_t alloc,
                                           proven_u8str_view_t name) {
    proven_result_u8str_t out = proven_u8str_create(alloc, 64);
    if (!proven_is_ok(out.err)) return out;

    proven_err_t err = proven_u8str_append(&out.value, PROVEN_LIT("hello, "));
    if (proven_is_ok(err)) err = proven_u8str_append(&out.value, name);
    if (!proven_is_ok(err)) {
        proven_u8str_destroy(alloc, &out.value);   /* undo what we made */
        out.err = err;
        out.value = (proven_u8str_t){0};
    }
    return out;
}

int main(void) {
    /* (a) the ordinary heap - malloc and free underneath */
    proven_allocator_t heap = proven_heap_allocator();

    proven_result_u8str_t a = make_greeting(heap, PROVEN_LIT("world"));
    EXAMPLE_REQUIRE(proven_is_ok(a.err), "the heap should be able to give 64 bytes");
    proven_println("from the heap : {}", PROVEN_ARG(proven_u8str_as_view(&a.value)));
    /* Destroyed with the SAME allocator that created it. That pairing is the
     * whole ownership rule of this library. */
    proven_u8str_destroy(heap, &a.value);

    /* (b) an arena - one block of memory handed out in order, freed all at once.
     *     Note that make_greeting above did not change at all. */
    alignas(PROVEN_MAX_ALIGN) proven_byte_t backing[512];
    proven_arena_t arena = proven_arena_create((proven_mem_mut_t){
        .ptr = backing, .size = sizeof backing });
    proven_allocator_t from_arena = proven_arena_as_allocator(&arena);

    proven_result_u8str_t b = make_greeting(from_arena, PROVEN_LIT("arena"));
    EXAMPLE_REQUIRE(proven_is_ok(b.err), "the arena has room for this too");
    proven_println("from an arena : {}", PROVEN_ARG(proven_u8str_as_view(&b.value)));

    /* No destroy loop here: an arena frees everything by being reset. That is
     * lesson 7's subject; the point now is only that the caller chose. */
    proven_arena_reset(&arena);

    return EXAMPLE_OK();
}
