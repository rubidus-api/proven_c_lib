#include "proven.h"
#include "proven_test.h"
#include <string.h>

/* 0.1 + 0.2 as a double adds them: 0x1.3333333333334p-2, "0.30000000000000004". Written as the
 * literal, not the sum - on i686 the sum is taken in x87's 80 bits and rounded twice, which can
 * give the double nearest 0.3 instead (seen 2026-11-12 on the Windows i686 run). */
#define SUM_01_02 0x1.3333333333334p-2

#include "test_unit_fmt_general_vectors.h"

/*
 * RFC-0046 (v0.114.0): `{:g}` is printf's %g, `{:r}` the shortest form that reads back (what
 * `{:g}` was), and the printf letters spell infinity and NaN as printf does. Every expected
 * string in the vector table is C17 7.21.6.1's, which glibc writes too; the rows below it are
 * the grammar's edges and the scanner's side.
 */

static proven_fmt_result_t r;

static const char *fmt1(proven_byte_t *buf, proven_size_t cap, const char *f, proven_arg_t a) {
    proven_u8str_t s = proven_u8str_borrow(buf, cap);
    proven_arg_t args[2] = { proven_arg_none(), a };
    r = proven_u8str_fmt_internal((proven_allocator_t){0}, &s, false, f, (proven_allocator_t){0}, args, 2);
    return proven_is_ok(r.err) ? proven_u8str_as_cstr(&s) : "<err>";
}

#define EQ(spec, arg, want) do { \
    const char *g_ = fmt1(buf, sizeof buf, (spec), (arg)); \
    PROVEN_TEST_ASSERT(strcmp(g_, (want)) == 0, "\"" spec "\" renders as \"" want "\"", "printf's spelling is the contract."); \
} while (0)

#define REFUSE(spec, arg) do { \
    (void)fmt1(buf, sizeof buf, (spec), (arg)); \
    PROVEN_TEST_ASSERT(r.err == PROVEN_ERR_INVALID_FORMAT, "\"" spec "\" is refused", "Check the type-letter checks in render_arg."); \
} while (0)

static proven_err_t scan1(const char *in, const char *f, proven_scan_arg_t a) {
    proven_scan_t sc = proven_scan_init(proven_u8str_view_from_cstr(in));
    return proven_scan_fmt_cursor(&sc, f, a);
}

int main(void) {
    PROVEN_TEST_SUITE("printf's %g in the formatter, and the float letters in the scanner",
        "{:g}, {:.Ng}, {:G}, {:E} and # write what printf writes, digit for digit; {:r} is the shortest form that reads back; under a printf letter infinity and NaN are inf and nan; the scanner reads a double under {:f}, {:e}, {:g} as under {}.",
        "A vector row names its spec and value: compare with printf(\"%.*g\") on the same double. Inspect proven_float_format_g_exact and the GENERAL mode in src/proven/float_format.c, the type letters in src/proven/fmt.c, placeholder_len in src/proven/scan.c.");
    proven_byte_t buf[256];

    PROVEN_TEST_SECTION("the vectors", "Boundaries of the fixed and scientific forms (1e-4, 1e-5, P digits), rounding that carries (999999.5, 9.9999995), zeros, subnormals, DBL_MAX, precisions 0 to 20, G, # and E.",
        "The row printed is the first that differs.");
    {
        unsigned bad = 0;
        for (unsigned i = 0; i < sizeof GEN_ROWS / sizeof GEN_ROWS[0]; ++i) {
            const char *g = fmt1(buf, sizeof buf, GEN_ROWS[i].spec, proven_arg_f64(GEN_ROWS[i].value));
            if (strcmp(g, GEN_ROWS[i].want) != 0) {
                if (bad == 0) PROVEN_TEST_INFO("row {}: {} gives {}, printf {}", PROVEN_ARG(i), PROVEN_ARG(GEN_ROWS[i].spec), PROVEN_ARG(g), PROVEN_ARG(GEN_ROWS[i].want));
                ++bad;
            }
        }
        PROVEN_TEST_ASSERT(bad == 0, "every vector row renders as printf writes it", "");
    }

    PROVEN_TEST_SECTION("the cases a reader checks first", "0.1 + 0.2, the six-digit default, and {:r} beside {:g}.", "");
    {
        EQ("{:g}", proven_arg_f64(SUM_01_02), "0.3");
        EQ("{:r}", proven_arg_f64(SUM_01_02), "0.30000000000000004");
        EQ("{:g}", proven_arg_f64(3.14159265358979), "3.14159");
        EQ("{:.17g}", proven_arg_f64(0.1), "0.10000000000000001");
        EQ("{:g}", proven_arg_f64(999999.5), "1e+06");       /* the exponent of the ROUNDED value */
        EQ("{:g}", proven_arg_f64(0.0001), "0.0001");
        EQ("{:g}", proven_arg_f64(0.00001), "1e-05");
        EQ("{:G}", proven_arg_f64(1e-10), "1E-10");
        EQ("{:E}", proven_arg_f64(12.5), "1.250000E+01");
        EQ("{:#g}", proven_arg_f64(2.0), "2.00000");
        EQ("{:#.0f}", proven_arg_f64(2.0), "2.");
        EQ("{:#.0e}", proven_arg_f64(2.0), "2.e+00");
        /* glibc writes "1.e+06" here; C17 and this library keep the zeros # asks for. */
        EQ("{:#g}", proven_arg_f64(999999.5), "1.00000e+06");
    }

    PROVEN_TEST_SECTION("width, sign and fill", "They work on the result as for every other form.", "");
    {
        EQ("{:>10.4g}", proven_arg_f64(3.14159265), "     3.142");
        EQ("{:<8g}|", proven_arg_f64(1.5), "1.5     |");
        EQ("{:+g}", proven_arg_f64(2.0), "+2");
        EQ("{:08g}", proven_arg_f64(-1.5), "-00001.5");
        EQ("{:011.3G}", proven_arg_f64(1234.5), "0001.23E+03");
    }

    PROVEN_TEST_SECTION("infinity and NaN", "printf's spellings under f, e, g; upper case under E, G; spaces, not zeros, under a 0 flag; {} and {:r} keep Inf and NaN.",
        "Check proven_float_format_printf_special and printf_specials.");
    {
        const double inf = 1e308 * 10.0, nan = inf - inf;     /* inf - inf: a NaN, its sign the processor's */
        const double pnan = __builtin_copysign(nan, 1.0), mnan = __builtin_copysign(nan, -1.0);
        EQ("{:g}", proven_arg_f64(inf), "inf");
        EQ("{:g}", proven_arg_f64(-inf), "-inf");
        EQ("{:e}", proven_arg_f64(inf), "inf");
        EQ("{:.2f}", proven_arg_f64(-inf), "-inf");
        EQ("{:g}", proven_arg_f64(pnan), "nan");
        EQ("{:g}", proven_arg_f64(mnan), "-nan");
        EQ("{:G}", proven_arg_f64(-inf), "-INF");
        EQ("{:E}", proven_arg_f64(pnan), "NAN");
        EQ("{:+g}", proven_arg_f64(pnan), "+nan");
        EQ("{:08g}", proven_arg_f64(-inf), "    -inf");
        EQ("{:#g}", proven_arg_f64(inf), "inf");
        EQ("{}", proven_arg_f64(-inf), "-Inf");
        EQ("{}", proven_arg_f64(pnan), "NaN");
        EQ("{:r}", proven_arg_f64(inf), "Inf");
    }

    PROVEN_TEST_SECTION("what is refused", "A precision or # on {:r}, # on {}, a float letter on a non-float.", "");
    {
        REFUSE("{:.3r}", proven_arg_f64(1.0));
        REFUSE("{:#r}", proven_arg_f64(1.0));
        REFUSE("{:#}", proven_arg_f64(1.0));
        REFUSE("{:g}", proven_arg_i32(5));
        REFUSE("{:G}", proven_arg_cstr("x"));
        REFUSE("{:E}", proven_arg_u64(1));
        REFUSE("{:r}", proven_arg_bool(true));
    }

    PROVEN_TEST_SECTION("the GENERAL mode of proven_float_format", "The same rule without the format grammar: options_general, precision, alternate, uppercase, printf_specials.",
        "Check proven_float_format_options_general and the dispatch in float_format.c.");
    {
        char out[64];
        proven_size_t n = 0;
        proven_float_format_options_t o = proven_float_format_options_general();
        PROVEN_TEST_ASSERT(proven_is_ok(proven_float_format_f64_policy(out, sizeof out, SUM_01_02, PROVEN_FLOAT_FORMAT_POLICY_DEFAULT, o, &n)) &&
                           n == 3 && strcmp(out, "0.3") == 0, "six significant digits by default", "");
        o.precision = 0;
        PROVEN_TEST_ASSERT(proven_is_ok(proven_float_format_f64_policy(out, sizeof out, 25.0, PROVEN_FLOAT_FORMAT_POLICY_DEFAULT, o, &n)) &&
                           strcmp(out, "2e+01") == 0, "precision 0 means one digit", "");
        o.precision = 3; o.alternate = true; o.uppercase = true;
        PROVEN_TEST_ASSERT(proven_is_ok(proven_float_format_f64_policy(out, sizeof out, 1e-7, PROVEN_FLOAT_FORMAT_POLICY_DEFAULT, o, &n)) &&
                           strcmp(out, "1.00E-07") == 0, "alternate keeps the zeros, uppercase the letter", "");
        o.printf_specials = false;
        PROVEN_TEST_ASSERT(proven_is_ok(proven_float_format_f64_policy(out, sizeof out, -1e308 * 10.0, PROVEN_FLOAT_FORMAT_POLICY_DEFAULT, o, &n)) &&
                           strcmp(out, "-Inf") == 0, "without printf_specials, the library's spelling", "");
        o = proven_float_format_options_general();
        PROVEN_TEST_ASSERT(proven_float_format_f64_policy(out, 4, 123456.0, PROVEN_FLOAT_FORMAT_POLICY_DEFAULT, o, &n) == PROVEN_ERR_OUT_OF_BOUNDS,
                           "a buffer too small is refused, not cut", "");
        o.precision = -1;
        PROVEN_TEST_ASSERT(proven_float_format_f64_policy(out, sizeof out, 1.0, PROVEN_FLOAT_FORMAT_POLICY_DEFAULT, o, &n) == PROVEN_ERR_INVALID_ARG,
                           "a negative precision is refused", "");
        PROVEN_TEST_ASSERT(proven_float_format_f64_policy(out, sizeof out, 1e308 * 10.0, PROVEN_FLOAT_FORMAT_POLICY_DEFAULT, o, &n) == PROVEN_ERR_INVALID_ARG,
                           "and refused for infinity too: the options are checked before the value", "");
    }

    PROVEN_TEST_SECTION("the scanner's float letters", "{:f}, {:e}, {:E}, {:g}, {:G} read a double as {} does - every spelling the formatter writes, inf and nan included; on any other destination they are refused.",
        "Check placeholder_len in src/proven/scan.c.");
    {
        double d = 0;
        PROVEN_TEST_ASSERT(scan1("x=1e+06", "x={:g}", PROVEN_SCAN_ARG(&d)) == PROVEN_OK && d == 1e6, "{:g} reads 1e+06", "");
        PROVEN_TEST_ASSERT(scan1("2.5E+00", "{:E}", PROVEN_SCAN_ARG(&d)) == PROVEN_OK && d == 2.5, "{:E} reads 2.5E+00", "");
        PROVEN_TEST_ASSERT(scan1("7", "{:f}", PROVEN_SCAN_ARG(&d)) == PROVEN_OK && d == 7.0, "{:f} reads 7", "");
        PROVEN_TEST_ASSERT(scan1("-inf", "{:e}", PROVEN_SCAN_ARG(&d)) == PROVEN_OK && d < 0 && d * 0.5 == d, "{:e} reads -inf", "");
        PROVEN_TEST_ASSERT(scan1("nan", "{:G}", PROVEN_SCAN_ARG(&d)) == PROVEN_OK && d != d, "{:G} reads nan", "");
        int i = 0;
        PROVEN_TEST_ASSERT(scan1("12", "{:g}", PROVEN_SCAN_ARG(&i)) == PROVEN_ERR_INVALID_FORMAT, "{:g} into an int is refused", "");
        PROVEN_TEST_ASSERT(scan1("1.5", "{:q}", PROVEN_SCAN_ARG(&d)) == PROVEN_ERR_INVALID_ARG, "an unknown letter is still refused", "");

        /* Written with {:r}, read with {:g}: the same double, for every value of a sweep. */
        unsigned long long x = 0x9e3779b97f4a7c15ull;
        unsigned bad = 0;
        for (int k = 0; k < 2000; ++k) {
            x ^= x << 13; x ^= x >> 7; x ^= x << 17;
            double v;
            memcpy(&v, &x, sizeof v);
            if (v != v || v - v != 0.0) continue;
            const char *t = fmt1(buf, sizeof buf, "{:r}", proven_arg_f64(v));
            double back = 0;
            if (scan1(t, "{:g}", PROVEN_SCAN_ARG(&back)) != PROVEN_OK || memcmp(&back, &v, sizeof v) != 0) ++bad;
        }
        PROVEN_TEST_ASSERT(bad == 0, "{:r} written, {:g} read back: bit for bit", "");
    }

    PROVEN_TEST_PASS("the g letter is printf's general form, r the shortest one, and the scanner reads every spelling the formatter writes.");
    return 0;
}
