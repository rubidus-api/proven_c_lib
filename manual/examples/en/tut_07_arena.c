#include "example.h"

/*
 * Lesson 7 - memory that is freed all at once.
 *
 * Lesson 5 handed an arena to make_greeting and then reset it, without saying
 * what the reset was. This lesson is about that one call.
 *
 * An arena bumps a pointer through a block of memory you own. There is no
 * per-object free: destroy on an arena reclaims nothing. Everything comes back
 * at once, with proven_arena_reset. That sounds like a restriction, and it is
 * the reason to use one - a loop that builds scratch text for each piece of
 * work can drop all of it with one statement, however many strings it made.
 */

int main(void) {
    /* The arena does not own this array - we do. It only hands out pieces. */
    alignas(PROVEN_MAX_ALIGN) proven_byte_t backing[256];
    proven_arena_t arena = proven_arena_create((proven_mem_mut_t){
        .ptr = backing, .size = sizeof backing });
    proven_allocator_t scratch = proven_arena_as_allocator(&arena);

    static const proven_u8str_view_t names[] = {
        PROVEN_LIT("ada"), PROVEN_LIT("grace"), PROVEN_LIT("barbara"),
    };

    for (proven_size_t i = 0; i < sizeof names / sizeof names[0]; ++i) {
        /* Two strings per round, both out of the arena. */
        proven_result_u8str_t line = proven_u8str_create(scratch, 32);
        proven_result_u8str_t note = proven_u8str_create(scratch, 32);
        EXAMPLE_REQUIRE(proven_is_ok(line.err) && proven_is_ok(note.err),
                        "each round fits easily in 256 bytes");
        if (!proven_is_ok(line.err) || !proven_is_ok(note.err)) return 1;

        proven_err_t err = proven_u8str_append(&line.value, PROVEN_LIT("hello, "));
        if (proven_is_ok(err)) err = proven_u8str_append(&line.value, names[i]);
        if (proven_is_ok(err)) err = proven_u8str_append(&note.value, PROVEN_LIT("(scratch)"));
        EXAMPLE_REQUIRE(proven_is_ok(err), "the appends fit their capacity");

        /* The first allocation of every round lands at the start of `backing`:
         * the reset at the end of the previous round gave all of it back. */
        EXAMPLE_REQUIRE((const void *)proven_u8str_as_view(&line.value).ptr == (const void *)backing,
                        "each round starts again at the beginning of the block");

        proven_println("round {}: {} {} ({} bytes in use)",
                       PROVEN_ARG(i), PROVEN_ARG(proven_u8str_as_view(&line.value)),
                       PROVEN_ARG(proven_u8str_as_view(&note.value)),
                       PROVEN_ARG(arena.offset));

        /* One statement, and both strings are gone. No destroy loop, nothing to
         * forget. `line` and `note` point at reclaimed memory from here on. */
        proven_arena_reset(&arena);
        EXAMPLE_REQUIRE(arena.offset == 0, "reset reclaims everything the round allocated");
    }

    /* An arena cannot grow. Asking for more than the block holds is an error
     * value, the same kind lesson 3 showed - not a crash, and not a silent
     * fallback to malloc. */
    proven_result_u8str_t too_big = proven_u8str_create(scratch, 1024);
    EXAMPLE_REQUIRE(too_big.err == PROVEN_ERR_NOMEM, "a 256-byte arena refuses 1 KiB");

    proven_arena_destroy(&arena);
    return EXAMPLE_OK();
}
