#include "example.h"

/*
 * The first program. It is deliberately small, and every line of it is one of
 * the five contracts you will meet on every page of this manual.
 *
 * Compare it with the C you already know:
 *
 *     char buf[64];
 *     strcpy(buf, name);          <- how big is name? strcpy does not ask.
 *     strcat(buf, ", welcome!");  <- and now? strcat does not ask either.
 *     printf("%s\n", buf);
 *
 * That program is correct until the day `name` is longer than you assumed, and
 * then it is a security advisory. The version below cannot do that: every write
 * knows the size of its destination, and every operation that could fail hands
 * you back an error you are not allowed to ignore silently.
 */

int main(void) {
    /* (1) You pass the allocator in. The library never reaches for a global
     *     malloc behind your back, so you always know who allocated what. */
    proven_allocator_t alloc = proven_heap_allocator();

    /* (2) Anything that can fail returns its error WITH its value. There is no
     *     errno to remember to check, and `greeting.value` means nothing until
     *     you have looked at `greeting.err`. */
    proven_result_u8str_t greeting = proven_u8str_create(alloc, 64);
    if (!proven_is_ok(greeting.err)) return 1;

    /* (3) A view is borrowed text that knows its own length. PROVEN_LIT builds
     *     one from a literal at compile time - no strlen scan happens here. */
    proven_u8str_view_t name = PROVEN_LIT("world");

    /* (4) The append refuses rather than truncates. If "hello, " and the name
     *     did not fit in the 64 bytes asked for above, this returns
     *     PROVEN_ERR_OUT_OF_BOUNDS and writes nothing - it never quietly stores
     *     half a word and lets you carry on. */
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

    proven_println("{}", PROVEN_ARG(proven_u8str_as_view(&greeting.value)));

    /* (5) You created it with `alloc`, so you destroy it with the SAME `alloc`.
     *     Owning things are destroyed exactly once; borrowed things - like
     *     `name` above - are never destroyed at all. */
    proven_u8str_destroy(alloc, &greeting.value);

    return EXAMPLE_OK();
}
