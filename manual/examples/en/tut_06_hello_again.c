#include "example.h"

/*
 * Lesson 6 - the program from Chapter 0, read line by line.
 *
 * Nothing new is introduced here. This is the same greeting program the manual
 * opens with, and the point of the lesson is that you can now name every part
 * of it: the allocator you pass in (lesson 5), the result you must check
 * (lesson 4), the error a refusal returns (lesson 3), the view that carries its
 * own length (lesson 2), and the printing that checks its arguments (lesson 1).
 *
 * If that reads as ordinary now, the tutorial has done its job and the
 * reference chapters are open to you.
 */

int main(void) {
    /* lesson 5 - the caller decides where memory comes from */
    proven_allocator_t alloc = proven_heap_allocator();

    /* lesson 4 - the string and the error that guards it, together */
    proven_result_u8str_t greeting = proven_u8str_create(alloc, 64);
    if (!proven_is_ok(greeting.err)) return 1;

    /* lesson 2 - borrowed text that knows its own size */
    proven_u8str_view_t name = PROVEN_LIT("world");

    /* lesson 3 - each append either fits or refuses; none of them truncates */
    proven_err_t err = proven_u8str_append(&greeting.value, PROVEN_LIT("hello, "));
    if (proven_is_ok(err)) err = proven_u8str_append(&greeting.value, name);
    if (proven_is_ok(err)) err = proven_u8str_append(&greeting.value, PROVEN_LIT("!"));

    if (!proven_is_ok(err)) {
        proven_u8str_destroy(alloc, &greeting.value);
        return 1;
    }

    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_as_view(&greeting.value),
                                         PROVEN_LIT("hello, world!")),
                    "the three appends should have built the whole greeting");

    /* lesson 1 - the format string and the argument cannot disagree */
    proven_println("{}", PROVEN_ARG(proven_u8str_as_view(&greeting.value)));

    /* lesson 5 again - destroyed with the allocator that created it */
    proven_u8str_destroy(alloc, &greeting.value);
    return EXAMPLE_OK();
}
