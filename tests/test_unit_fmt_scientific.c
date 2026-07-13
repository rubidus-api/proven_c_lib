#include "proven.h"
#include "proven_test.h"
#include <string.h>

/*
 * Written from printf's %e - the one float form the {} grammar did not offer - before the
 * always-scientific engine mode was wired up (docs/TESTING.md §5.1). Every expected string
 * below is exactly what C's printf produces for the same spec, because "{:e} matches %e" is
 * the whole contract: a caller reaching for scientific notation with a fixed precision wants
 * the spelling the rest of the world already agrees on (mantissa, six default digits, a
 * signed two-digit-minimum exponent, half-to-even rounding).
 */

static const char *fe(proven_byte_t *buf, proven_size_t cap, const char *f, double v) {
    proven_u8str_t s = proven_u8str_borrow(buf, cap);
    proven_arg_t args[2] = { proven_arg_none(), proven_arg_f64(v) };
    proven_fmt_result_t r = proven_u8str_fmt_internal((proven_allocator_t){0}, &s, false, f,
                                                      (proven_allocator_t){0}, args, 2);
    if (!proven_is_ok(r.err)) return "<err>";
    return proven_u8str_as_cstr(&s);
}

#define EQ(spec, val, want) do { \
    const char *g = fe(buf, sizeof buf, (spec), (val)); \
    PROVEN_TEST_ASSERT(strcmp(g, (want)) == 0, \
        "\"" spec "\" must render as \"" want "\"", \
        "proven's {:e} must match printf's %e, digit for digit."); \
} while (0)

int main(void) {
    PROVEN_TEST_SUITE("the {:e} scientific float form",
        "Always scientific, printf %e: mantissa, a fixed number of fractional digits, and a signed two-digit exponent, correctly rounded.",
        "Inspect the {:e} branch in src/proven/fmt.c and PROVEN_FLOAT_FORMAT_MODE_SCIENTIFIC in float_format.c.");

    proven_byte_t buf[128];

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("the default six digits, matching %e",
        "Six fractional digits and a signed exponent of at least two digits is what %e means by default.",
        "");
    // ---------------------------------------------------------------
    EQ("{:e}", 3.14159, "3.141590e+00");
    EQ("{:e}", 1e20,    "1.000000e+20");
    EQ("{:e}", 0.0,     "0.000000e+00");
    EQ("{:e}", -2.5,    "-2.500000e+00");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a chosen precision, and rounding half-to-even",
        "{:.Ne} gives N digits after the mantissa point; the rounding is the exact engine's, not the FPU's.",
        "");
    // ---------------------------------------------------------------
    EQ("{:.2e}",  3.14159,  "3.14e+00");
    EQ("{:.0e}",  9.6,      "1e+01");        /* rounds up, and the exponent follows */
    EQ("{:.3e}",  -1.5e-10, "-1.500e-10");
    EQ("{:.15e}", 1.0,      "1.000000000000000e+00");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("it forces scientific where {:f} and {:g} would not",
        "This is the reason {:e} exists: an ordinary-magnitude value that {:g} would spell plainly and {:f} would spell without an exponent.",
        "");
    // ---------------------------------------------------------------
    EQ("{:e}", 42.0,   "4.200000e+01");      /* {:g} would give "42", {:f} "42.000000" */
    EQ("{:.1e}", 0.5,  "5.0e-01");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("the smallest subnormal, at the edge of the exponent range",
        "The exact engine has no magnitude ceiling; %e of 5e-324 is a real answer, not an overflow.",
        "");
    // ---------------------------------------------------------------
    EQ("{:e}", 5e-324, "4.940656e-324");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("width and alignment apply to the scientific form",
        "It is a rendered string like any other, so a column of them lines up.",
        "");
    // ---------------------------------------------------------------
    EQ("{:>14.2e}", 3.14159, "      3.14e+00");
    EQ("{:e}", 1.0, "1.000000e+00");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("zero-fill goes between the sign and the mantissa, like %e and like an integer",
        "\"000-3.14e+00\" is not a number; the zeros belong AFTER the sign, exactly as printf and the integer path do it.",
        "The float render path lifts the sign out and places the zeros after it; render_with_spec would pad the whole string, sign included.");
    // ---------------------------------------------------------------
    EQ("{:010.2e}", -3.14, "-03.14e+00");
    EQ("{:010.2e}", 3.14,  "003.14e+00");
    EQ("{:+010.2e}", 3.14, "+03.14e+00");
    EQ("{:08.2f}", -3.14,  "-0003.14");     /* the same bug lived in {:f} */
    EQ("{:+08.2f}", 3.14,  "+0003.14");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("{:e} on a non-float is refused, like {:f} and {:g}",
        "The library cannot render an integer in scientific float notation without inventing something.",
        "");
    // ---------------------------------------------------------------
    {
        const char *g = fe(buf, sizeof buf, "{:e}", 0.0);
        (void)g;
        proven_u8str_t s = proven_u8str_borrow(buf, sizeof buf);
        proven_arg_t args[2] = { proven_arg_none(), PROVEN_ARG(42) };
        proven_fmt_result_t r = proven_u8str_fmt_internal((proven_allocator_t){0}, &s, false, "{:e}",
                                                          (proven_allocator_t){0}, args, 2);
        PROVEN_TEST_ASSERT(r.err == PROVEN_ERR_INVALID_FORMAT,
            "{:e} on an integer must be PROVEN_ERR_INVALID_FORMAT", "");
    }

    PROVEN_TEST_PASS("{:e} spells scientific notation exactly as printf does.");
    return 0;
}
