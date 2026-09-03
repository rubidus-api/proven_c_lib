#include "example.h"

/*
 * Lesson 4 - when the call has a value to give back, the value and the error
 * arrive together.
 *
 * proven_err_t alone is enough when there is nothing to return. When there IS
 * something, you get a small struct with two fields: `err` and `value`. The
 * rule is one sentence: `value` means nothing until you have looked at `err`.
 *
 * This is the same discipline as checking malloc for NULL, except the checking
 * place is part of the type instead of a convention you have to remember.
 */

/* A function of your own can return one too. Nothing in the library is magic. */
static proven_result_size_t safe_div(proven_size_t a, proven_size_t b) {
    proven_result_size_t res = {0};
    if (b == 0) {
        res.err = PROVEN_ERR_INVALID_ARG;    /* value stays 0 - and means nothing */
        return res;
    }
    res.err = PROVEN_OK;
    res.value = a / b;
    return res;
}

int main(void) {
    proven_result_size_t ok = safe_div(10, 2);
    EXAMPLE_REQUIRE(proven_is_ok(ok.err), "dividing by 2 is fine");
    EXAMPLE_REQUIRE(ok.value == 5, "and only now may we read the value");

    proven_result_size_t bad = safe_div(10, 0);
    EXAMPLE_REQUIRE(!proven_is_ok(bad.err), "dividing by zero must fail");
    EXAMPLE_REQUIRE(bad.err == PROVEN_ERR_INVALID_ARG, "and say which rule was broken");
    /* bad.value is 0, but that is not an answer. It is the absence of one. */

    /* The library's own calls have the same shape. proven_u8str_create hands
     * back the string it made together with the error that guards it. */
    proven_allocator_t alloc = proven_heap_allocator();
    proven_result_u8str_t s = proven_u8str_create(alloc, 16);
    if (!proven_is_ok(s.err)) return 1;      /* nothing was created; nothing to destroy */

    proven_println("10 / 2 = {}", PROVEN_ARG(ok.value));

    proven_u8str_destroy(alloc, &s.value);
    return EXAMPLE_OK();
}
