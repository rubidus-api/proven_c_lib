#include "proven.h"
#include "proven_test.h"
#include <string.h>

/*
 * The format spec grammar.
 *
 * It used to support exactly four things: fill, align, width, and lowercase 'x'.
 * Everything else printf users reach for was either an error or - worse, in two
 * cases - silently wrong. The consequences were not cosmetic:
 *
 *   - Every float came out with exactly six decimals, FOREVER. So a float column
 *     could not be aligned: 12.5 rendered nine characters wide and 100.0 rendered
 *     ten, and the column simply broke.
 *   - There was no way to emit a single character. PROVEN_ARG('Z') printed 90.
 *   - A conventional uppercase hex dump was impossible.
 */

static const char *fmt1(proven_byte_t *buf, proven_size_t cap, proven_fmt_result_t *out,
                        const char *f, proven_arg_t a) {
    proven_u8str_t s = proven_u8str_borrow(buf, cap);
    proven_arg_t args[2] = { proven_arg_none(), a };
    *out = proven_u8str_fmt_internal((proven_allocator_t){0}, &s, false, f,
                                     (proven_allocator_t){0}, args, 2);
    return proven_u8str_as_cstr(&s);
}

#define EXPECT(spec, arg, want)                                                    \
    do {                                                                           \
        const char *got = fmt1(buf, sizeof buf, &r, (spec), (arg));                \
        PROVEN_TEST_ASSERT(proven_is_ok(r.err) && strcmp(got, (want)) == 0,        \
            "\"" spec "\" must render as \"" want "\"",                            \
            "The spec parser or the renderer disagrees with the documented grammar."); \
    } while (0)

#define REFUSE(spec, arg)                                                          \
    do {                                                                           \
        (void)fmt1(buf, sizeof buf, &r, (spec), (arg));                            \
        PROVEN_TEST_ASSERT(r.err == PROVEN_ERR_INVALID_FORMAT,                     \
            "\"" spec "\" must be refused",                                        \
            "A spec the argument cannot honour must be an error, never ignored.");  \
    } while (0)

int main(void) {
    PROVEN_TEST_SUITE("format spec grammar",
        "Precision, bases, case, alternate form, sign, char and bool - and a refusal for anything an argument cannot honour.",
        "Inspect the spec parser and render_integer / the float case in src/proven/fmt.c.");

    proven_byte_t buf[128];
    proven_fmt_result_t r;

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("float precision - the gap that broke every float column",
        "Six decimals, forever, meant 12.5 was nine characters and 100.0 was ten.",
        "The exact engine has always done precision; the {} grammar simply could not reach it.");
    // ---------------------------------------------------------------
    EXPECT("{:.3}",  PROVEN_ARG(3.14159), "3.142");
    EXPECT("{:.3f}", PROVEN_ARG(12.3456), "12.346");

    /* Precision 0 means no decimals. It used to be silently rewritten to 6 - the
     * engine treated it as "unset" - so a caller asking for an integer-looking float
     * got six decimals and was told it worked. */
    EXPECT("{:.0}",  PROVEN_ARG(3.7), "4");

    /* Shortest round-trip. */
    EXPECT("{:g}",   PROVEN_ARG(0.1), "0.1");

    /* And now a column actually lines up. This is the whole point. */
    EXPECT("{:>9.2}", PROVEN_ARG(12.5),   "    12.50");
    EXPECT("{:>9.2}", PROVEN_ARG(100.0),  "   100.00");
    EXPECT("{:>9.2}", PROVEN_ARG(-3.125), "    -3.12");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("bases, case, and the alternate form",
        "Only lowercase 'x' existed. An uppercase hex dump was impossible.",
        "Inspect spec_base and spec_prefix.");
    // ---------------------------------------------------------------
    EXPECT("{:x}",     PROVEN_ARG(255), "ff");
    EXPECT("{:X}",     PROVEN_ARG(255), "FF");
    EXPECT("{:#x}",    PROVEN_ARG(255), "0xff");
    EXPECT("{:#X}",    PROVEN_ARG(255), "0XFF");
    EXPECT("{:o}",     PROVEN_ARG(8),   "10");
    EXPECT("{:b}",     PROVEN_ARG(5),   "101");
    EXPECT("{:#b}",    PROVEN_ARG(5),   "0b101");
    EXPECT("{:08x}",   PROVEN_ARG(255), "000000ff");
    EXPECT("{:#010x}", PROVEN_ARG(255), "0x000000ff");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("sign, and where the zero-padding goes",
        "Zero-padding belongs between the sign and the digits. \"0000+42\" is not a number.",
        "Inspect the zero-fill branch of render_integer: the sign and any 0x prefix are emitted before the zeros.");
    // ---------------------------------------------------------------
    EXPECT("{:+}",   PROVEN_ARG(42),  "+42");
    EXPECT("{:+}",   PROVEN_ARG(-42), "-42");
    EXPECT("{: }",   PROVEN_ARG(42),  " 42");
    EXPECT("{:+08}", PROVEN_ARG(42),  "+0000042");
    EXPECT("{:+.2}", PROVEN_ARG(1.5), "+1.50");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("char and bool",
        "PROVEN_ARG('Z') printed 90. There was no way to emit a character at all.",
        "char and _Bool map to their own argument types in the _Generic list now, not to the integer one.");
    // ---------------------------------------------------------------
    {
        char z = 'Z';
        bool yes = true, no = false;
        EXPECT("{}",    PROVEN_ARG(z),   "Z");
        EXPECT("{}",    PROVEN_ARG(yes), "true");
        EXPECT("{}",    PROVEN_ARG(no),  "false");
        EXPECT("{:>5}", PROVEN_ARG(yes), " true");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a spec the argument cannot honour is refused",
        "The caller asked for something. Giving them something else and reporting success is the worst available outcome.",
        "Inspect the applicability guard at the top of render_arg.");
    // ---------------------------------------------------------------
    REFUSE("{:x}",  PROVEN_ARG(3.5));      /* hex on a double */
    REFUSE("{:x}",  PROVEN_ARG("hi"));     /* hex on a string */
    REFUSE("{:.2}", PROVEN_ARG(42));       /* precision on an integer */
    REFUSE("{:f}",  PROVEN_ARG(42));       /* a float form on an integer */
    REFUSE("{:d}",  PROVEN_ARG(1.5));      /* an integer form on a float */
    REFUSE("{:#}",  PROVEN_ARG("hi"));     /* alternate form on a string */
    REFUSE("{:q}",  PROVEN_ARG(1));        /* a letter that does not exist */
    REFUSE("{:.}",  PROVEN_ARG(1.5));      /* a precision with no digits */

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("the old spellings still mean what they meant",
        "Adding grammar must not change what already-written format strings do.",
        "If one of these moved, the change was not backward compatible.");
    // ---------------------------------------------------------------
    EXPECT("{}",     PROVEN_ARG(42),      "42");
    EXPECT("{}",     PROVEN_ARG(3.14159), "3.141590");
    EXPECT("{:8}",   PROVEN_ARG(42),      "      42");
    EXPECT("{:<8}",  PROVEN_ARG(42),      "42      ");
    EXPECT("{:^8}",  PROVEN_ARG(42),      "   42   ");
    EXPECT("{:*>8}", PROVEN_ARG(42),      "******42");
    EXPECT("{:0>8}", PROVEN_ARG(42),      "00000042");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a wide zero-fill is honoured, not silently truncated",
        "The parser allows a width up to 10000. Every one of those characters must actually appear.",
        "render_integer must EMIT its padding through append_padding, not assemble the padded number in a fixed buffer. The first version did the latter and produced 127 characters for a width of 200 - while returning PROVEN_OK.");
    // ---------------------------------------------------------------
    {
        /* Big enough for the widest thing the parser will accept. */
        static proven_byte_t wide[16384];
        proven_u8str_t s = proven_u8str_borrow(wide, sizeof wide);
        proven_arg_t args[2] = { proven_arg_none(), PROVEN_ARG(255) };
        proven_fmt_result_t w = proven_u8str_fmt_internal((proven_allocator_t){0}, &s, false, "{:#0200x}",
                                                          (proven_allocator_t){0}, args, 2);
        PROVEN_TEST_ASSERT(proven_is_ok(w.err), "a 200-wide zero-filled hex value must format", "");
        PROVEN_TEST_ASSERT(proven_u8str_as_view(&s).size == 200,
            "a width of 200 must produce exactly 200 characters",
            "It used to produce 127 and report success: the padding was being assembled in a 128-byte buffer.");

        proven_u8str_t s2 = proven_u8str_borrow(wide, sizeof wide);
        proven_arg_t args2[2] = { proven_arg_none(), PROVEN_ARG(~0ull) };
        w = proven_u8str_fmt_internal((proven_allocator_t){0}, &s2, false, "{:#0300b}",
                                      (proven_allocator_t){0}, args2, 2);
        PROVEN_TEST_ASSERT(proven_is_ok(w.err) && proven_u8str_as_view(&s2).size == 300,
            "UINT64_MAX in binary, zero-filled to 300, must be exactly 300 characters",
            "64 binary digits plus a 0b prefix plus 234 zeros.");
    }

    PROVEN_TEST_PASS("the format spec grammar behaves.");
    return 0;
}
