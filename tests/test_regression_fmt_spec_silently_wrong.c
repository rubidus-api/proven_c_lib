#include "proven.h"
#include "proven_test.h"
#include <string.h>

/*
 * Two formatter defects that failed the worst way available: silently.
 *
 * 1. `{:08}` - the spelling every C, Python and Rust programmer reaches for -
 *    ate the '0' as the first digit of the width. `{:08}` on 42 produced
 *    "      42": space-padded, eight wide, no error. `{:08x}` on 42 produced
 *    "      2a". The output was wrong and the return code said OK.
 *
 * 2. `{:x}` on a double printed "3.500000"; on a string it printed the string.
 *    The hex request was parsed and then dropped on the floor. The caller asked
 *    for something, got something else, and was told it had worked.
 *
 * A spelling that is accepted and quietly does the wrong thing is worse than one
 * that is rejected. Both now do the right thing or say they cannot.
 */

static const char *fmt_once(proven_byte_t *buf, proven_size_t cap,
                            proven_fmt_result_t *out, const char *f, proven_arg_t a) {
    proven_u8str_t s = proven_u8str_borrow(buf, cap);
    proven_arg_t args[2] = { proven_arg_none(), a };
    *out = proven_u8str_fmt_internal((proven_allocator_t){0}, &s, false, f,
                                     (proven_allocator_t){0}, args, 2);
    return proven_u8str_as_cstr(&s);
}

int main(void) {
    PROVEN_TEST_SUITE("formatter specs that used to be silently wrong",
        "A format spec must do what it says or refuse. It must never do something else and report success.",
        "Inspect the spec parser and the spec-applicability guard in src/proven/fmt.c.");

    proven_byte_t buf[64];
    proven_fmt_result_t r;

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a leading zero in a spec means zero-padding",
        "{:08} is the universal spelling. It must pad with zeros, not eat the 0 as a width digit.",
        "In the spec parser, a leading '0' with no explicit fill/align must set fill='0' before the width digits are read.");
    // ---------------------------------------------------------------
    const char *out = fmt_once(buf, sizeof buf, &r, "{:08}", PROVEN_ARG(42));
    PROVEN_TEST_ASSERT(proven_is_ok(r.err), "{:08} must be accepted", "");
    PROVEN_TEST_ASSERT(strcmp(out, "00000042") == 0,
        "{:08} on 42 must produce 00000042",
        "It used to produce '      42' - space-padded, with no error.");

    out = fmt_once(buf, sizeof buf, &r, "{:08x}", PROVEN_ARG(42));
    PROVEN_TEST_ASSERT(proven_is_ok(r.err) && strcmp(out, "0000002a") == 0,
        "{:08x} on 42 must produce 0000002a",
        "It used to produce '      2a'.");

    /* An explicit fill still wins: the zero rule only applies when none was given. */
    out = fmt_once(buf, sizeof buf, &r, "{:*>8}", PROVEN_ARG(42));
    PROVEN_TEST_ASSERT(proven_is_ok(r.err) && strcmp(out, "******42") == 0,
        "an explicit fill is not overridden by the zero rule", "");

    out = fmt_once(buf, sizeof buf, &r, "{:0>8}", PROVEN_ARG(42));
    PROVEN_TEST_ASSERT(proven_is_ok(r.err) && strcmp(out, "00000042") == 0,
        "the explicit spelling still works", "");

    /* Width without a leading zero still pads with spaces. */
    out = fmt_once(buf, sizeof buf, &r, "{:8}", PROVEN_ARG(42));
    PROVEN_TEST_ASSERT(proven_is_ok(r.err) && strcmp(out, "      42") == 0,
        "a plain width still pads with spaces", "");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a spec the argument cannot honour is an error",
        "Asking a double for hex is asking for the impossible. Say so; do not print decimal and return OK.",
        "The applicability guard at the top of render_arg must reject spec.hex for any non-integer argument.");
    // ---------------------------------------------------------------
    (void)fmt_once(buf, sizeof buf, &r, "{:x}", PROVEN_ARG(3.5));
    PROVEN_TEST_ASSERT(r.err == PROVEN_ERR_INVALID_FORMAT,
        "{:x} on a double must be rejected",
        "It used to print 3.500000 and report success.");

    (void)fmt_once(buf, sizeof buf, &r, "{:x}", PROVEN_ARG("hi"));
    PROVEN_TEST_ASSERT(r.err == PROVEN_ERR_INVALID_FORMAT,
        "{:x} on a string must be rejected",
        "It used to print the string and report success.");

    /* And hex on the types that CAN honour it still works. */
    out = fmt_once(buf, sizeof buf, &r, "{:x}", PROVEN_ARG(255));
    PROVEN_TEST_ASSERT(proven_is_ok(r.err) && strcmp(out, "ff") == 0,
        "hex on an integer still works", "");

    PROVEN_TEST_PASS("the formatter no longer accepts a spec and then ignores it.");
    return 0;
}
