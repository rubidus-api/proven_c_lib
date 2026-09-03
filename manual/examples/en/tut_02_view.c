#include "example.h"

/*
 * Lesson 2 - text that knows how long it is.
 *
 * In the C you already know, a string is a pointer and the length is wherever
 * the first zero byte happens to be. Every function has to walk the bytes to
 * find out how much there is, and if the zero is missing it walks off the end.
 *
 * A view is the pair that C leaves implicit: a pointer AND a size, travelling
 * together. It borrows - it does not own the bytes and never frees them.
 */

int main(void) {
    /* PROVEN_LIT makes a view from a literal. The size is computed at compile
     * time, so no scan happens here - unlike strlen. */
    proven_u8str_view_t hello = PROVEN_LIT("hello");

    EXAMPLE_REQUIRE(hello.size == 5, "the view already knows its own length");
    EXAMPLE_REQUIRE(hello.ptr != NULL, "and it points at the literal's bytes");

    /* Because the length travels with the pointer, comparing is a size check
     * plus a memcmp. No walking, and no way to run off the end. */
    EXAMPLE_REQUIRE(proven_u8str_view_eq(hello, PROVEN_LIT("hello")), "same text");
    EXAMPLE_REQUIRE(!proven_u8str_view_eq(hello, PROVEN_LIT("hell")), "shorter text differs");

    /* A view can name PART of something without copying it. Here is the middle
     * of the literal - still borrowed, still no allocation. */
    proven_u8str_view_t ell = { .ptr = hello.ptr + 1, .size = 3 };
    EXAMPLE_REQUIRE(proven_u8str_view_eq(ell, PROVEN_LIT("ell")), "a window onto the same bytes");

    proven_println("whole: {} (size {})", PROVEN_ARG(hello), PROVEN_ARG(hello.size));
    proven_println("part : {} (size {})", PROVEN_ARG(ell), PROVEN_ARG(ell.size));

    return EXAMPLE_OK();
}
