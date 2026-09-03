#include "example.h"

/*
 * Lesson 3 - a call that can fail says so, in its return value.
 *
 * The C you know reports failure in three different ways: a magic return value
 * (-1), a null pointer, or a global called errno that the next call overwrites.
 * All three are easy to not look at, and nothing complains when you don't.
 *
 * Here a fallible call returns proven_err_t. It is an ordinary value: you can
 * store it, compare it, and pass it upward. And [[nodiscard]] on these
 * functions means ignoring it is a compiler warning, not a habit.
 */

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    /* Room for exactly 8 bytes. (Chapter 2 is about where that room comes
     * from; for now, notice only that we said how much.) */
    proven_result_u8str_t s = proven_u8str_create(alloc, 8);
    EXAMPLE_REQUIRE(proven_is_ok(s.err), "8 bytes should be available");

    /* This fits. */
    proven_err_t err = proven_u8str_append(&s.value, PROVEN_LIT("12345678"));
    EXAMPLE_REQUIRE(proven_is_ok(err), "eight bytes into eight bytes fits exactly");

    /* This does not - and the library REFUSES it. It does not append the part
     * that would fit, because half a word is not a shorter word, it is a
     * different one. */
    proven_err_t too_much = proven_u8str_append(&s.value, PROVEN_LIT("9"));
    EXAMPLE_REQUIRE(!proven_is_ok(too_much), "one byte more than capacity must fail");
    EXAMPLE_REQUIRE(too_much == PROVEN_ERR_OUT_OF_BOUNDS, "and it says why: out of bounds");

    /* The refusal changed nothing. The string is exactly as it was, which is
     * what "failure-atomic" means: a failed call leaves no half-done state. */
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_as_view(&s.value), PROVEN_LIT("12345678")),
                    "the refused append must not have written anything");

    proven_println("after the refusal, the string is still: {}",
                   PROVEN_ARG(proven_u8str_as_view(&s.value)));

    proven_u8str_destroy(alloc, &s.value);
    return EXAMPLE_OK();
}
