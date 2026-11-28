#include "proven/float_ops.h"
#include "proven_test.h"
#include "../math/src/proven_internal_float_ops.h"
#include <limits.h>
#include <stdint.h>
#include <string.h>

/*
 * float_ops (RFC-0054): each function against a table of values whose answers are written as
 * bits - normal, subnormal, both zeros, both infinities, quiet and signalling NaN, the rounding
 * boundaries - and then against references computed another way on random inputs: the hardware
 * root against the integer one, integral values against conversions and the 2^52 rounding
 * trick, fmod against integer remainders, ldexp against one multiplication by a power of two,
 * the int64 conversions against the compiler's. No libm: the build does not link it.
 */

static uint64_t B(double x) { uint64_t u; memcpy(&u, &x, sizeof u); return u; }
static double D(uint64_t u) { double x; memcpy(&x, &u, sizeof x); return x; }

#define PZ 0x0000000000000000u
#define NZ 0x8000000000000000u
#define PINF 0x7ff0000000000000u
#define NINF 0xfff0000000000000u
#define QNAN 0x7ff8000000000000u
/* 32-bit x86 returns a double or a float in the x87 register st(0), and loading a signalling
 * NaN there quiets it: on that target no signalling NaN survives a function's return (this file's
 * D and F included), whatever the function does. There the signalling rows use the quiet NaN of
 * the same payload, which every function must keep as well. */
#if defined(__i386__)
#define SNAN 0x7ff8000000000123u
#else
#define SNAN 0x7ff0000000000123u     /* signalling, payload 0x123 */
#endif
#define SNANQ 0x7ff8000000000123u    /* ... quieted */
#define NQNAN 0xfff8000000000456u    /* a negative quiet NaN with a payload */
#define MINSUB 0x0000000000000001u
#define MAXSUB 0x000fffffffffffffu
#define MINNORM 0x0010000000000000u
#define MAXF 0x7fefffffffffffffu
#define ONE 0x3ff0000000000000u
#define PINF_D (proven_float_from_bits(PINF))
#define NINF_D (proven_float_from_bits(NINF))

typedef double (*f1_t)(double);
typedef struct { uint64_t in, out; } row1_t;

static void table1(const char *name, f1_t f, const row1_t *rows, size_t n) {
    int bad = 0;
    for (size_t i = 0; i < n; i++) {
        const uint64_t got = B(f(D(rows[i].in)));
        if (got != rows[i].out) bad++;
    }
    PROVEN_TEST_ASSERT(bad == 0, name, "A row of the table gave other bits: run the table by hand and compare bit by bit.");
}

static uint64_t g_seed = 0x9e3779b97f4a7c15u;
static uint64_t rnd(void) {
    g_seed ^= g_seed << 13; g_seed ^= g_seed >> 7; g_seed ^= g_seed << 17;
    return g_seed;
}
/* Random bits, half of them with exponents near 2^0 .. 2^60 where the fraction matters. */
static double rnd_double(void) {
    uint64_t u = rnd();
    if (u & 1) u = (u & ~((uint64_t)0x7ff << 52)) | ((uint64_t)(1000 + (rnd() % 90)) << 52);
    return D(u);
}

/* References by other means (valid where |x| < 2^52, x not NaN). */
static double ref_trunc(double x) { return proven_float_copysign((double)(int64_t)x, x); }
static double ref_floor(double x) { double t = ref_trunc(x); return t > x ? t - 1.0 : t; }
static double ref_ceil(double x) { double t = ref_trunc(x); return t < x ? proven_float_copysign(t + 1.0, x) : t; }
static double ref_round(double x) {
    double t = ref_trunc(x), d = x - t;
    if (d >= 0.5) t += 1.0;
    if (d <= -0.5) t -= 1.0;
    return proven_float_copysign(t, x);
}
static double ref_rint(double x) {
    /* x - trunc(x) is exact; not x + 2^52 - 2^52, which x87's wider sum rounds twice */
    double t = ref_trunc(x);
    const double d = x - t;
    const bool odd = ((int64_t)t & 1) != 0;
    if (d > 0.5 || (d == 0.5 && odd)) t += 1.0;
    if (d < -0.5 || (d == -0.5 && odd)) t -= 1.0;
    return proven_float_copysign(t, x);
}

int main(void) {
    PROVEN_TEST_SUITE("float_ops: binary64 operations with the same bits everywhere",
        "Each function of float_ops.h against a table of bits and against a reference computed another way on random inputs (RFC-0054).",
        "Inspect math/src/float_ops.c: the function the failing check names, and f_split / f_join_round for the exponent arithmetic.");

    PROVEN_TEST_SECTION("classifying", "is_nan, is_inf, is_finite by the bits.", "Check f_nan and the masks in math/src/float_ops.c.");
    {
        static const struct { uint64_t u; bool nan, inf, fin; } c[] = {
            { PZ, 0, 0, 1 }, { NZ, 0, 0, 1 }, { MINSUB, 0, 0, 1 }, { MAXF, 0, 0, 1 }, { PINF, 0, 1, 0 }, { NINF, 0, 1, 0 },
            { QNAN, 1, 0, 0 }, { SNAN, 1, 0, 0 }, { NQNAN, 1, 0, 0 }, { 0x7ff0000000000001u, 1, 0, 0 }, { 0xffffffffffffffffu, 1, 0, 0 },
        };
        int bad = 0;
        for (size_t i = 0; i < sizeof c / sizeof c[0]; i++)
            if (proven_float_is_nan(D(c[i].u)) != c[i].nan || proven_float_is_inf(D(c[i].u)) != c[i].inf || proven_float_is_finite(D(c[i].u)) != c[i].fin) bad++;
        PROVEN_TEST_ASSERT(bad == 0, "every class is told by its bits", "");
        PROVEN_TEST_ASSERT(proven_float_to_bits(D(SNAN)) == SNAN && B(proven_float_from_bits(NQNAN)) == NQNAN, "the bits go both ways unchanged", "");
    }

    PROVEN_TEST_SECTION("sign", "fabs and copysign touch only the sign bit, a signalling NaN included.", "Check proven_float_fabs and proven_float_copysign.");
    {
        PROVEN_TEST_ASSERT(B(proven_float_fabs(D(NZ))) == PZ && B(proven_float_fabs(D(NINF))) == PINF && B(proven_float_fabs(D(SNAN | NZ))) == SNAN,
            "fabs: -0 is +0, -inf +inf, a negative signalling NaN stays signalling", "");
        PROVEN_TEST_ASSERT(B(proven_float_copysign(D(ONE), D(NZ))) == (ONE | NZ) && B(proven_float_copysign(D(NQNAN), D(PZ))) == (NQNAN & ~NZ)
            && B(proven_float_copysign(D(MINSUB), D(NQNAN))) == (MINSUB | NZ), "copysign takes the sign of a zero and of a NaN, and gives it to a NaN", "");
    }

    PROVEN_TEST_SECTION("square root", "Exact cases, the specials, and on random inputs the hardware root (where there is one) against the integer root bit for bit.", "Check proven_float_sqrt_soft_ - the bit-by-bit root and its rounding - and the hardware branch's specials.");
    {
        static const row1_t t[] = {
            { PZ, PZ }, { NZ, NZ }, { PINF, PINF }, { NINF, QNAN }, { 0xbff0000000000000u, QNAN }, { MINSUB | NZ, QNAN },
            { QNAN, QNAN }, { SNAN, SNANQ }, { NQNAN, NQNAN },
            { ONE, ONE }, { 0x4010000000000000u, 0x4000000000000000u },             /* 4 -> 2 */
            { 0x4000000000000000u, 0x3ff6a09e667f3bcdu },                           /* 2 -> sqrt 2 */
            { MINSUB, 0x1e60000000000000u },                                        /* 2^-1074 -> 2^-537 */
            { MAXSUB, 0x1fffffffffffffffu }, { MINNORM, 0x2000000000000000u },
            { MAXF, 0x5fefffffffffffffu }, { 0x3fefffffffffffffu, 0x3fefffffffffffffu },
        };
        table1("sqrt: the table", proven_float_sqrt, t, sizeof t / sizeof t[0]);
        table1("the integer root: the table", proven_float_sqrt_soft_, t, sizeof t / sizeof t[0]);
        int bad = 0;
        for (int i = 0; i < 300000; i++) {
            double x = proven_float_fabs(rnd_double());
            if (B(proven_float_sqrt(x)) != B(proven_float_sqrt_soft_(x))) bad++;
            /* And the root squared brackets x: r^2 <= x < (r + ulp)^2 is what "nearest" needs on one side. */
        }
        PROVEN_TEST_ASSERT(bad == 0, proven_float_sqrt_is_hardware_() ? "300,000 random roots: the instruction and the integer root agree bit for bit" : "300,000 random roots (no instruction on this target: the integer root against itself)", "");
        /* Squares come back whole: the root of k*k is k. */
        int sq = 0;
        for (uint64_t k = 1; k < 200000; k += 7) if (B(proven_float_sqrt_soft_((double)(k * k))) != B((double)k)) sq++;
        PROVEN_TEST_ASSERT(sq == 0, "the integer root of every k*k tried is k", "");
    }

    PROVEN_TEST_SECTION("integral values", "trunc, floor, ceil, round, rint: tables at the halves, the signed zeros, the large and the specials; then references by conversion on random inputs.", "Check the masks and the carry in proven_float_floor / ceil / round / rint.");
    {
        static const row1_t tt[] = { { 0xbfe0000000000000u, NZ }, { 0x3ff8000000000000u, ONE }, { 0xc004000000000000u, 0xc000000000000000u },
                                     { MAXF, MAXF }, { PINF, PINF }, { SNAN, SNANQ }, { MINSUB | NZ, NZ } };
        table1("trunc: the table", proven_float_trunc, tt, sizeof tt / sizeof tt[0]);
        static const row1_t tf[] = { { 0xbfe0000000000000u, 0xbff0000000000000u }, { 0x3fe0000000000000u, PZ }, { NZ, NZ }, { MINSUB | NZ, 0xbff0000000000000u },
                                     { 0xbff8000000000000u, 0xc000000000000000u }, { NINF, NINF }, { NQNAN, NQNAN } };
        table1("floor: the table", proven_float_floor, tf, sizeof tf / sizeof tf[0]);
        static const row1_t tc[] = { { 0xbfe0000000000000u, NZ }, { 0x3fe0000000000000u, ONE }, { MINSUB, ONE }, { PZ, PZ },
                                     { 0x3ff8000000000000u, 0x4000000000000000u }, { 0x433fffffffffffffu, 0x433fffffffffffffu }, { QNAN, QNAN } };
        table1("ceil: the table", proven_float_ceil, tc, sizeof tc / sizeof tc[0]);
        static const row1_t tr[] = { { 0x4004000000000000u, 0x4008000000000000u },            /* 2.5 -> 3 */
                                     { 0xbfe0000000000000u, 0xbff0000000000000u },            /* -0.5 -> -1 */
                                     { 0x3fdfffffffffffffu, PZ }, { 0x3ff8000000000000u, 0x4000000000000000u }, { NZ, NZ },
                                     { 0x432fffffffffffffu, 0x4330000000000000u },            /* 2^52 - 0.5 -> 2^52 */
                                     { SNAN, SNANQ } };
        table1("round: the table, halves away from zero", proven_float_round, tr, sizeof tr / sizeof tr[0]);
        static const row1_t ti[] = { { 0x4004000000000000u, 0x4000000000000000u },            /* 2.5 -> 2 */
                                     { 0x400c000000000000u, 0x4010000000000000u },            /* 3.5 -> 4 */
                                     { 0xbfe0000000000000u, NZ },                             /* -0.5 -> -0 */
                                     { 0x3fe0000000000001u, ONE }, { 0x3ff8000000000000u, 0x4000000000000000u },
                                     { 0x432fffffffffffffu, 0x4330000000000000u }, { PINF, PINF }, { NQNAN, NQNAN } };
        table1("rint: the table, halves to even", proven_float_rint, ti, sizeof ti / sizeof ti[0]);
        int bad[5] = { 0 };
        for (int i = 0; i < 300000; i++) {
            const double x = rnd_double();
            if (proven_float_is_nan(x) || !(proven_float_fabs(x) < 4503599627370496.0)) continue;
            if (B(proven_float_trunc(x)) != B(ref_trunc(x))) bad[0]++;
            if (B(proven_float_floor(x)) != B(ref_floor(x))) bad[1]++;
            if (B(proven_float_ceil(x)) != B(ref_ceil(x))) bad[2]++;
            if (B(proven_float_round(x)) != B(ref_round(x))) bad[3]++;
            if (B(proven_float_rint(x)) != B(ref_rint(x))) bad[4]++;
        }
        PROVEN_TEST_ASSERT(bad[0] == 0 && bad[1] == 0 && bad[2] == 0 && bad[3] == 0 && bad[4] == 0, "random values below 2^52: all five agree with their references", "");
    }

    PROVEN_TEST_SECTION("smaller and larger", "A number wins over a NaN, -0 is below +0, and two NaNs give the first, quieted.", "Check f_key and the NaN branches of proven_float_fmin / fmax.");
    {
        PROVEN_TEST_ASSERT(B(proven_float_fmin(D(QNAN), D(ONE))) == ONE && B(proven_float_fmin(D(ONE), D(SNAN))) == ONE && B(proven_float_fmax(D(NQNAN), D(NINF))) == NINF,
            "a number wins over a NaN, quiet or signalling", "");
        PROVEN_TEST_ASSERT(B(proven_float_fmin(D(SNAN), D(QNAN))) == SNANQ && B(proven_float_fmax(D(NQNAN), D(SNAN))) == NQNAN, "two NaNs: the first, quieted", "");
        PROVEN_TEST_ASSERT(B(proven_float_fmin(D(PZ), D(NZ))) == NZ && B(proven_float_fmin(D(NZ), D(PZ))) == NZ && B(proven_float_fmax(D(NZ), D(PZ))) == PZ && B(proven_float_fmax(D(PZ), D(NZ))) == PZ,
            "-0 is below +0 whichever comes first", "");
        int bad = 0;
        for (int i = 0; i < 200000; i++) {
            const double x = rnd_double(), y = rnd_double();
            if (proven_float_is_nan(x) || proven_float_is_nan(y) || x == y) continue;
            if (B(proven_float_fmin(x, y)) != B(x < y ? x : y) || B(proven_float_fmax(x, y)) != B(x > y ? x : y)) bad++;
        }
        PROVEN_TEST_ASSERT(bad == 0, "random pairs: as the comparison says", "");
    }

    PROVEN_TEST_SECTION("fmod", "The specials, signs, subnormal results, and on random integers times a power of two the integer remainder.", "Check the long division in proven_float_fmod and f_join_exact.");
    {
        PROVEN_TEST_ASSERT(B(proven_float_fmod(D(PINF), D(ONE))) == QNAN && B(proven_float_fmod(D(ONE), D(NZ))) == QNAN && B(proven_float_fmod(D(SNAN), D(ONE))) == SNANQ
            && B(proven_float_fmod(D(ONE), D(NQNAN))) == NQNAN, "an infinite x or a zero y makes the NaN; a NaN given comes back quieted", "");
        PROVEN_TEST_ASSERT(B(proven_float_fmod(D(0xc014000000000000u), D(0x4008000000000000u))) == 0xc000000000000000u
            && B(proven_float_fmod(D(0x4014000000000000u), D(0xc008000000000000u))) == 0x4000000000000000u, "fmod(-5, 3) is -2 and fmod(5, -3) is 2: x's sign", "");
        PROVEN_TEST_ASSERT(B(proven_float_fmod(D(0xc008000000000000u), D(0x4008000000000000u))) == NZ && B(proven_float_fmod(D(NZ), D(ONE))) == NZ
            && B(proven_float_fmod(D(ONE), D(PINF))) == ONE, "an exact multiple gives a zero of x's sign; an infinite y gives x", "");
        PROVEN_TEST_ASSERT(B(proven_float_fmod(D(MAXSUB), D(0x0000000000000003u))) == (MAXSUB % 3) && B(proven_float_fmod(D(MAXF), D(MINSUB))) == PZ
            && B(proven_float_fmod(D(MAXF), D(0x0000000000000003u))) != QNAN, "subnormal operands and results", "");
        int bad = 0;
        for (int i = 0; i < 200000; i++) {
            const int64_t a = (int64_t)(rnd() >> 12) - ((int64_t)1 << 51), b = (int64_t)(rnd() >> (12 + rnd() % 40)) + 1;
            const int k = (int)(rnd() % 1200) - 600;
            const double x = proven_float_ldexp((double)a, k), y = proven_float_ldexp((double)b, k);
            const double want = proven_float_copysign(proven_float_ldexp((double)(a % b), k), x);
            if (B(proven_float_fmod(x, y)) != B(want)) bad++;
        }
        PROVEN_TEST_ASSERT(bad == 0, "random integers below 2^52 times one power of two: the integer remainder", "");
    }

    PROVEN_TEST_SECTION("exponents", "frexp and ldexp: the specials, subnormals both ways, the rounding into the subnormals, overflow; on random inputs ldexp against one multiplication and frexp undone by ldexp.", "Check f_split and f_join_round.");
    {
        int e = 7;
        PROVEN_TEST_ASSERT(B(proven_float_frexp(D(PZ), &e)) == PZ && e == 0 && B(proven_float_frexp(D(NINF), &e)) == NINF && e == 0
            && B(proven_float_frexp(D(SNAN), &e)) == SNANQ && e == 0, "frexp of a zero, an infinity, a NaN: itself and 0", "");
        PROVEN_TEST_ASSERT(B(proven_float_frexp(D(ONE), &e)) == 0x3fe0000000000000u && e == 1 && B(proven_float_frexp(D(MINSUB | NZ), &e)) == 0xbfe0000000000000u && e == -1073,
            "frexp(1) is 0.5 * 2^1; of -2^-1074, -0.5 * 2^-1073", "");
        PROVEN_TEST_ASSERT(B(proven_float_ldexp(D(ONE), -1074)) == MINSUB && B(proven_float_ldexp(D(ONE), -1075)) == PZ && B(proven_float_ldexp(D(0x3ff8000000000000u), -1075)) == MINSUB * 1
            && B(proven_float_ldexp(D(0x3ff8000000000000u), -1074)) == 0x0000000000000002u, "into the subnormals once, halves to even: 1.5 * 2^-1075 is the smallest, 1.5 * 2^-1074 is two of it", "");
        PROVEN_TEST_ASSERT(B(proven_float_ldexp(D(MAXF), 1)) == PINF && B(proven_float_ldexp(D(NZ | ONE), 5000)) == NINF && B(proven_float_ldexp(D(MINSUB), 2000)) == 0x79d0000000000000u
            && B(proven_float_ldexp(D(MINSUB), INT32_MIN)) == PZ && B(proven_float_ldexp(D(ONE), INT32_MAX)) == PINF, "overflow to infinity, a subnormal raised, exponents of any size", "");
        int bad = 0, back = 0;
        for (int i = 0; i < 200000; i++) {
            const double x = rnd_double();
            if (!proven_float_is_finite(x)) continue;
            const int n = (int)(rnd() % 2045) - 1022;
            const double p = D((uint64_t)(n + 1023) << 52);
            if (B(proven_float_ldexp(x, n)) != B(x * p)) bad++;
            int ex;
            const double m = proven_float_frexp(x, &ex);
            if (x != 0 && (!(proven_float_fabs(m) >= 0.5 && proven_float_fabs(m) < 1.0) || B(proven_float_ldexp(m, ex)) != B(x))) back++;
        }
        PROVEN_TEST_ASSERT(bad == 0, "random ldexp: as one multiplication by the power of two", "");
        PROVEN_TEST_ASSERT(back == 0, "random frexp: the fraction in [0.5, 1), undone by ldexp", "");
    }

    PROVEN_TEST_SECTION("int64", "from_i64 rounds to nearest even; to_i64 truncates and refuses NaN and what is outside [-2^63, 2^63).", "Check proven_float_from_i64's rounding and proven_float_to_i64's range.");
    {
        PROVEN_TEST_ASSERT(B(proven_float_from_i64(0)) == PZ && B(proven_float_from_i64(INT64_MIN)) == 0xc3e0000000000000u && B(proven_float_from_i64(INT64_MAX)) == 0x43e0000000000000u
            && B(proven_float_from_i64(((int64_t)1 << 53) + 1)) == 0x4340000000000000u && B(proven_float_from_i64(((int64_t)1 << 53) + 3)) == 0x4340000000000002u,
            "0 is +0; 2^63 - 1 rounds to 2^63; 2^53 + 1 to even below, 2^53 + 3 to even above", "");
        proven_i64 v = 5;
        PROVEN_TEST_ASSERT(proven_float_to_i64(D(QNAN), &v) == PROVEN_ERR_OUT_OF_BOUNDS && v == 5 && proven_float_to_i64(D(0x43e0000000000000u), &v) == PROVEN_ERR_OUT_OF_BOUNDS
            && proven_float_to_i64(D(NINF), &v) == PROVEN_ERR_OUT_OF_BOUNDS && v == 5, "NaN, 2^63 and -inf are refused, the out-parameter untouched", "");
        PROVEN_TEST_ASSERT(proven_float_to_i64(D(0xc3e0000000000000u), &v) == PROVEN_OK && v == INT64_MIN && proven_float_to_i64(D(0x43dfffffffffffffu), &v) == PROVEN_OK && v == (int64_t)0x7ffffffffffffc00,
            "-2^63 is taken, and the largest double below 2^63", "");
        PROVEN_TEST_ASSERT(proven_float_to_i64(D(0xbfefffffffffffffu), &v) == PROVEN_OK && v == 0 && proven_float_to_i64(D(0xc004000000000000u), &v) == PROVEN_OK && v == -2
            && proven_float_to_i64(D(ONE), (void *)0) == PROVEN_ERR_INVALID_ARG, "toward zero; a null out-parameter is refused", "");
        int bad = 0;
        for (int i = 0; i < 300000; i++) {
            const int64_t a = (int64_t)rnd() >> (rnd() % 64);
            if (B(proven_float_from_i64(a)) != B((double)a)) bad++;
            const double x = rnd_double();
            proven_i64 got;
            if (proven_float_is_finite(x) && proven_float_fabs(x) < 9.2e18) {
                if (proven_float_to_i64(x, &got) != PROVEN_OK || got != (int64_t)x) bad++;
            }
        }
        PROVEN_TEST_ASSERT(bad == 0, "random values both ways agree with the compiler's conversions", "");
    }

    PROVEN_TEST_SECTION("RFC-0058's exact functions", "remainder, remquo, nextafter, modf, ilogb, logb, fdim, round_to_i64 and rint_to_i64 by tables of bits; remainder also as x - n*y checked exactly on random inputs.", "Check f_remquo's quotient bits and its |x| < |y| branch, and nextafter's direction.");
    {
        int q = 0;
        PROVEN_TEST_ASSERT(B(proven_float_remainder(D(0x4014000000000000u), D(0x4000000000000000u))) == ONE              /* 5 rem 2 = 1 (n = 2, even) */
            && B(proven_float_remainder(D(0x401c000000000000u), D(0x4000000000000000u))) == 0xbff0000000000000u          /* 7 rem 2 = -1 (n = 4) */
            && B(proven_float_remainder(D(0x3ff8000000000000u), D(ONE))) == 0xbfe0000000000000u                          /* 1.5 rem 1 = -0.5 (n = 2) */
            && B(proven_float_remainder(D(0x3fe0000000000000u), D(ONE))) == 0x3fe0000000000000u                          /* 0.5 rem 1 = 0.5 (n = 0) */
            && B(proven_float_remainder(D(0xc010000000000000u), D(0x4000000000000000u))) == NZ                           /* -4 rem 2 = -0: x's sign */
            && B(proven_float_remainder(D(ONE), D(PZ))) == QNAN && B(proven_float_remainder(D(PINF), D(ONE))) == QNAN
            && B(proven_float_remainder(D(ONE), D(PINF))) == ONE && B(proven_float_remainder(D(SNAN), D(ONE))) == SNANQ,
            "remainder: ties to even n, a zero with x's sign, the invalid cases, NaNs", "");
        PROVEN_TEST_ASSERT(B(proven_float_remquo(D(0x401c000000000000u), D(0xc000000000000000u), &q)) == 0xbff0000000000000u && q == -4
            && B(proven_float_remquo(D(0x3ff8000000000000u), D(ONE), &q)) == 0xbfe0000000000000u && q == 2,
            "remquo: the quotient's sign and low bits", "");
        PROVEN_TEST_ASSERT(B(proven_float_nextafter(D(ONE), D(PINF))) == 0x3ff0000000000001u && B(proven_float_nextafter(D(ONE), D(PZ))) == 0x3fefffffffffffffu
            && B(proven_float_nextafter(D(PZ), D(NZ))) == NZ && B(proven_float_nextafter(D(PZ), D(ONE))) == MINSUB && B(proven_float_nextafter(D(NZ), D(0xbff0000000000000u))) == (MINSUB | NZ)
            && B(proven_float_nextafter(D(MAXF), D(PINF))) == PINF && B(proven_float_nextafter(D(PINF), D(ONE))) == MAXF && B(proven_float_nextafter(D(ONE), D(QNAN))) == QNAN,
            "nextafter: both directions, through zero, to and from infinity", "");
        double ip = 0;
        PROVEN_TEST_ASSERT(B(proven_float_modf(D(0xc004000000000000u), &ip)) == 0xbfe0000000000000u && B(ip) == 0xc000000000000000u
            && B(proven_float_modf(D(0x4000000000000000u), &ip)) == PZ && B(proven_float_modf(D(NINF), &ip)) == NZ && B(ip) == NINF,
            "modf: -2.5 is -2 and -0.5; an integer's fraction is a signed zero; an infinity's too", "");
        PROVEN_TEST_ASSERT(proven_float_ilogb(D(ONE)) == 0 && proven_float_ilogb(D(MINSUB)) == -1074 && proven_float_ilogb(D(MAXF)) == 1023
            && proven_float_ilogb(D(PZ)) == PROVEN_FLOAT_ILOGB_ZERO && proven_float_ilogb(D(QNAN)) == PROVEN_FLOAT_ILOGB_NAN && proven_float_ilogb(D(PINF)) == INT_MAX
            && B(proven_float_logb(D(PZ))) == NINF && B(proven_float_logb(D(0x4024000000000000u))) == 0x4008000000000000u,
            "ilogb and logb: subnormals counted, the special results", "");
        proven_i64 v = 0;
        PROVEN_TEST_ASSERT(B(proven_float_fdim(D(ONE), D(0x4000000000000000u))) == PZ && B(proven_float_fdim(D(0x4000000000000000u), D(ONE))) == ONE
            && proven_float_round_to_i64(D(0x4004000000000000u), &v) == PROVEN_OK && v == 3 && proven_float_rint_to_i64(D(0x4004000000000000u), &v) == PROVEN_OK && v == 2
            && proven_float_round_to_i64(D(QNAN), &v) == PROVEN_ERR_OUT_OF_BOUNDS,
            "fdim; 2.5 rounds to 3 away from zero and to 2 to even", "");
        int bad = 0;
        for (int i = 0; i < 300000; i++) {
            const double x = rnd_double(), y = rnd_double();
            if (!proven_float_is_finite(x) || !proven_float_is_finite(y) || proven_float_to_bits(y) << 1 == 0) continue;
            int qq;
            const double r = proven_float_remquo(x, y, &qq);
            /* |r| <= |y|/2, r has x's sign when zero, and x - r is an integer multiple of y whose
             * low bits are qq: checked with fmod (exact) on the difference */
            const double ay = proven_float_fabs(y), ar = proven_float_fabs(r);
            if (proven_float_lt(proven_float_mul(ay, 0.5), ar)) bad++;
            /* fmod is exact and truncates; the remainder is it, or it minus |y| with fmod's sign -
             * a difference Sterbenz makes exact, since then |fmod| > |y|/2 */
            const double f = proven_float_fmod(x, y), g = proven_float_sub(f, proven_float_copysign(ay, f));
            if (proven_float_to_bits(r) != proven_float_to_bits(f) && proven_float_to_bits(r) != proven_float_to_bits(g)) bad++;
            if (proven_float_to_bits(proven_float_nextafter(proven_float_nextafter(x, PINF_D), NINF_D)) != proven_float_to_bits(x) && proven_float_fabs(x) < 1e300) bad++;
        }
        PROVEN_TEST_ASSERT(bad == 0, "300,000 random pairs: |remainder| <= |y|/2 and it is fmod or fmod minus |y|; nextafter up then down returns", "");
    }

    PROVEN_TEST_PASS("float_ops checks passed.");
    return 0;
}
