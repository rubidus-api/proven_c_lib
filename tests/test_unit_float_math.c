#include "proven/float_math.h"
#include "proven/float_ops.h"
#include "proven/math_config.h"
#include "proven_test.h"
#include "../math/src/proven_internal_float_math.h"
#include "test_unit_float_math_vectors.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
 * float_math (RFC-0056, RFC-0058): the 35 functions of PROVEN_FLOAT_MATH_FNS, correctly rounded,
 * binary64 and binary32. The special values of C23 Annex F as bits; hard-to-round inputs and random ones
 * with MPFR's results (test_unit_float_math_vectors.h) through the public function and through
 * the integer path; and the two paths against each other on random inputs. The full lists
 * (about nine million inputs) and binary32's sampled exhaustively are a private check run
 * against MPFR, which this suite does not link.
 */

static uint64_t B(double x) { uint64_t u; memcpy(&u, &x, sizeof u); return u; }
static double D(uint64_t u) { double x; memcpy(&x, &u, sizeof x); return x; }
static uint32_t BF(float x) { uint32_t u; memcpy(&u, &x, sizeof u); return u; }
static float F(uint32_t u) { float x; memcpy(&x, &u, sizeof x); return x; }

#define CALL_1(f, x, y) f(x)
#define CALL_2(f, x, y) f(x, y)
#define CALL_3(f, x, y) f(x, (proven_i64)b)
static uint64_t pub(int fn, bool s, uint64_t a, uint64_t b) {
    if (s) {
        const float x = F((uint32_t)a), y = F((uint32_t)b);
        (void)y;
        switch (fn) {
#define CASE(ID, name, ar) case PROVEN_FLOAT_FN_##ID##_: return BF(CALL_##ar(proven_float32_##name, x, y));
        PROVEN_FLOAT_MATH_FNS(CASE)
#undef CASE
        default: return 0;
        }
    }
    const double x = D(a), y = D(b);
    (void)y;
    switch (fn) {
#define CASE(ID, name, ar) case PROVEN_FLOAT_FN_##ID##_: return B(CALL_##ar(proven_float_##name, x, y));
    PROVEN_FLOAT_MATH_FNS(CASE)
#undef CASE
    default: return 0;
    }
}

typedef struct { int fn; uint64_t a, b, want; } row_t;

/* A NaN's bits quieted; anything else unchanged (for the x87 note at SNAN). */
[[maybe_unused]] static uint64_t quiet64(uint64_t u) { return (u & 0x7fffffffffffffffu) > 0x7ff0000000000000u ? u | 0x0008000000000000u : u; }
#if defined(__i386__)
#define SNAN32_POW0 0x3f800000u      /* the NaN arrives quiet: pow(NaN, 0) = 1 */
#else
#define SNAN32_POW0 0x7fe00000u      /* pow(sNaN, 0): the NaN quieted */
#endif

#define PI64 0x400921fb54442d18u
#define PI2_64 0x3ff921fb54442d18u
#define PI4_64 0x3fe921fb54442d18u
#define PI34_64 0x4002d97c7f3321d2u
#define NZ 0x8000000000000000u
#define ONE 0x3ff0000000000000u
#define INF 0x7ff0000000000000u
#define NINF 0xfff0000000000000u
#define QNAN 0x7ff8000000000000u
/* 32-bit x86 returns a double or a float in the x87 register st(0), and loading a signalling
 * NaN there quiets it: on that target no signalling NaN survives a function's return (this file's
 * D and F included), whatever the function does. There the public functions are given the quiet NaN of the same
 * payload, and the rows expect what that gives (pow(NaN, 0) = 1); the integer path takes the
 * bits and keeps its rows. */
#define SNAN 0x7ff0000000000123u
#define SNANQ 0x7ff8000000000123u
#define TWO 0x4000000000000000u
#define HALF 0x3fe0000000000000u
#define M1 0xbff0000000000000u
#define M2 0xc000000000000000u
#define THREE 0x4008000000000000u

static const row_t SPECIAL64[] = {
    { PROVEN_FLOAT_FN_EXP_, 0, 0, ONE }, { PROVEN_FLOAT_FN_EXP_, NZ, 0, ONE }, { PROVEN_FLOAT_FN_EXP_, NINF, 0, 0 },
    { PROVEN_FLOAT_FN_EXP_, INF, 0, INF }, { PROVEN_FLOAT_FN_EXP_, SNAN, 0, SNANQ },
    { PROVEN_FLOAT_FN_EXP_, 0x408f400000000000u, 0, INF }, { PROVEN_FLOAT_FN_EXP_, 0xc08f400000000000u, 0, 0 },
    { PROVEN_FLOAT_FN_EXP_, 0x40862e42fefa39f0u, 0, INF },                 /* just past log(DBL_MAX) */
    { PROVEN_FLOAT_FN_EXP_, 0xc0874910d52d3052u, 0, 0 },                   /* -745.1332...: just under half the smallest subnormal */
    { PROVEN_FLOAT_FN_LOG_, ONE, 0, 0 }, { PROVEN_FLOAT_FN_LOG_, 0, 0, NINF }, { PROVEN_FLOAT_FN_LOG_, NZ, 0, NINF },
    { PROVEN_FLOAT_FN_LOG_, M1, 0, QNAN }, { PROVEN_FLOAT_FN_LOG_, NINF, 0, QNAN }, { PROVEN_FLOAT_FN_LOG_, INF, 0, INF },
    { PROVEN_FLOAT_FN_LOG_, 1, 0, 0xc0874385446d71c3u },                  /* log of the smallest subnormal */
    { PROVEN_FLOAT_FN_SIN_, NZ, 0, NZ }, { PROVEN_FLOAT_FN_SIN_, 0, 0, 0 }, { PROVEN_FLOAT_FN_SIN_, INF, 0, QNAN },
    { PROVEN_FLOAT_FN_SIN_, NINF, 0, QNAN }, { PROVEN_FLOAT_FN_SIN_, 1, 0, 1 },
    { PROVEN_FLOAT_FN_COS_, NZ, 0, ONE }, { PROVEN_FLOAT_FN_COS_, INF, 0, QNAN }, { PROVEN_FLOAT_FN_COS_, 1, 0, ONE },
    { PROVEN_FLOAT_FN_TAN_, NZ, 0, NZ }, { PROVEN_FLOAT_FN_TAN_, NINF, 0, QNAN }, { PROVEN_FLOAT_FN_TAN_, 1, 0, 1 },
    { PROVEN_FLOAT_FN_ATAN2_, 0, 0, 0 }, { PROVEN_FLOAT_FN_ATAN2_, 0, NZ, PI64 }, { PROVEN_FLOAT_FN_ATAN2_, NZ, NZ, PI64 | NZ },
    { PROVEN_FLOAT_FN_ATAN2_, NZ, ONE, NZ }, { PROVEN_FLOAT_FN_ATAN2_, ONE, 0, PI2_64 }, { PROVEN_FLOAT_FN_ATAN2_, M1, NZ, PI2_64 | NZ },
    { PROVEN_FLOAT_FN_ATAN2_, INF, INF, PI4_64 }, { PROVEN_FLOAT_FN_ATAN2_, INF, NINF, PI34_64 }, { PROVEN_FLOAT_FN_ATAN2_, NINF, ONE, PI2_64 | NZ },
    { PROVEN_FLOAT_FN_ATAN2_, ONE, NINF, PI64 }, { PROVEN_FLOAT_FN_ATAN2_, M1, INF, NZ }, { PROVEN_FLOAT_FN_ATAN2_, ONE, ONE, PI4_64 },
    { PROVEN_FLOAT_FN_ATAN2_, SNAN, QNAN, SNANQ }, { PROVEN_FLOAT_FN_ATAN2_, ONE, SNAN, SNANQ },
    { PROVEN_FLOAT_FN_POW_, QNAN, 0, ONE }, { PROVEN_FLOAT_FN_POW_, SNAN, NZ, SNANQ }, { PROVEN_FLOAT_FN_POW_, ONE, QNAN, ONE },
    { PROVEN_FLOAT_FN_POW_, ONE, SNAN, SNANQ },
    { PROVEN_FLOAT_FN_POW_, SNAN, ONE, SNANQ }, { PROVEN_FLOAT_FN_POW_, TWO, SNAN, SNANQ }, { PROVEN_FLOAT_FN_POW_, M1, INF, ONE },
    { PROVEN_FLOAT_FN_POW_, M1, NINF, ONE }, { PROVEN_FLOAT_FN_POW_, HALF, INF, 0 }, { PROVEN_FLOAT_FN_POW_, HALF, NINF, INF },
    { PROVEN_FLOAT_FN_POW_, TWO, INF, INF }, { PROVEN_FLOAT_FN_POW_, TWO, NINF, 0 }, { PROVEN_FLOAT_FN_POW_, NZ, M1, NINF },
    { PROVEN_FLOAT_FN_POW_, NZ, M2, INF }, { PROVEN_FLOAT_FN_POW_, NZ, THREE, NZ }, { PROVEN_FLOAT_FN_POW_, NZ, TWO, 0 },
    { PROVEN_FLOAT_FN_POW_, 0, M1, INF }, { PROVEN_FLOAT_FN_POW_, NINF, THREE, NINF }, { PROVEN_FLOAT_FN_POW_, NINF, M1, NZ },
    { PROVEN_FLOAT_FN_POW_, NINF, HALF, INF }, { PROVEN_FLOAT_FN_POW_, INF, M1, 0 }, { PROVEN_FLOAT_FN_POW_, M2, HALF, QNAN },
    { PROVEN_FLOAT_FN_POW_, M2, THREE, 0xc020000000000000u }, { PROVEN_FLOAT_FN_POW_, M2, TWO, 0x4010000000000000u },
    { PROVEN_FLOAT_FN_POW_, 0x4022000000000000u, HALF, THREE },             /* 9^0.5 = 3, exactly */
    { PROVEN_FLOAT_FN_POW_, TWO, 0xc090c80000000000u, 1 },                 /* 2^-1074: the smallest subnormal */
    { PROVEN_FLOAT_FN_POW_, TWO, 0xc090cc0000000000u, 0 },                 /* 2^-1075: half of it, to even: +0 */
    { PROVEN_FLOAT_FN_POW_, THREE, 0x4044000000000000u, 0x43e517168a4523fdu }, /* 3^40 rounded once */
    { PROVEN_FLOAT_FN_POW_, 0x4044000000000000u, THREE, 0x40ef400000000000u }, /* 40^3 = 64000, exactly */
    /* RFC-0058 */
    { PROVEN_FLOAT_FN_EXP2_, NZ, 0, ONE }, { PROVEN_FLOAT_FN_EXP2_, NINF, 0, 0 }, { PROVEN_FLOAT_FN_EXP2_, INF, 0, INF },
    { PROVEN_FLOAT_FN_EXP2_, SNAN, 0, SNANQ }, { PROVEN_FLOAT_FN_EXP2_, ONE, 0, TWO }, { PROVEN_FLOAT_FN_EXP2_, 0x4090000000000000u, 0, INF },
    { PROVEN_FLOAT_FN_EXP2_, 0xc090c80000000000u, 0, 1 },                  /* 2^-1074, exactly */
    { PROVEN_FLOAT_FN_EXP2_, 0xc090cc0000000000u, 0, 0 },                  /* 2^-1075: a tie, to even */
    { PROVEN_FLOAT_FN_EXP10_, 0, 0, ONE }, { PROVEN_FLOAT_FN_EXP10_, NINF, 0, 0 }, { PROVEN_FLOAT_FN_EXP10_, THREE, 0, 0x408f400000000000u },
    { PROVEN_FLOAT_FN_EXP10_, 0x4037000000000000u, 0, 0x44b52d02c7e14af6u }, /* 10^23: halfway between two doubles, to even */
    { PROVEN_FLOAT_FN_EXP10_, 0x4036000000000000u, 0, 0x4480f0cf064dd592u }, /* 10^22, exactly */
    { PROVEN_FLOAT_FN_EXPM1_, NZ, 0, NZ }, { PROVEN_FLOAT_FN_EXPM1_, NINF, 0, M1 }, { PROVEN_FLOAT_FN_EXPM1_, INF, 0, INF },
    { PROVEN_FLOAT_FN_EXPM1_, 0xc050000000000000u, 0, M1 }, { PROVEN_FLOAT_FN_EXPM1_, 1, 0, 1 },
    { PROVEN_FLOAT_FN_LOG2_, TWO, 0, ONE }, { PROVEN_FLOAT_FN_LOG2_, HALF, 0, M1 }, { PROVEN_FLOAT_FN_LOG2_, 1, 0, 0xc090c80000000000u },
    { PROVEN_FLOAT_FN_LOG2_, M1, 0, QNAN }, { PROVEN_FLOAT_FN_LOG2_, NZ, 0, NINF }, { PROVEN_FLOAT_FN_LOG2_, ONE, 0, 0 },
    { PROVEN_FLOAT_FN_LOG10_, 0x408f400000000000u, 0, THREE }, { PROVEN_FLOAT_FN_LOG10_, 0x4480f0cf064dd592u, 0, 0x4036000000000000u },
    { PROVEN_FLOAT_FN_LOG10_, 0, 0, NINF }, { PROVEN_FLOAT_FN_LOG10_, NINF, 0, QNAN },
    { PROVEN_FLOAT_FN_LOG1P_, M1, 0, NINF }, { PROVEN_FLOAT_FN_LOG1P_, M2, 0, QNAN }, { PROVEN_FLOAT_FN_LOG1P_, NZ, 0, NZ },
    { PROVEN_FLOAT_FN_LOG1P_, INF, 0, INF }, { PROVEN_FLOAT_FN_LOG1P_, NINF, 0, QNAN }, { PROVEN_FLOAT_FN_LOG1P_, 1, 0, 1 },
    { PROVEN_FLOAT_FN_CBRT_, 0xc03b000000000000u, 0, 0xc008000000000000u }, /* -27 */
    { PROVEN_FLOAT_FN_CBRT_, 0x4020000000000000u, 0, TWO }, { PROVEN_FLOAT_FN_CBRT_, NZ, 0, NZ }, { PROVEN_FLOAT_FN_CBRT_, NINF, 0, NINF },
    { PROVEN_FLOAT_FN_HYPOT_, THREE, 0x4010000000000000u, 0x4014000000000000u }, /* 3, 4, 5 */
    { PROVEN_FLOAT_FN_HYPOT_, INF, QNAN, INF }, { PROVEN_FLOAT_FN_HYPOT_, QNAN, NINF, INF }, { PROVEN_FLOAT_FN_HYPOT_, NZ, NZ, 0 },
    { PROVEN_FLOAT_FN_HYPOT_, 0x7fefffffffffffffu, 0x7fefffffffffffffu, INF }, { PROVEN_FLOAT_FN_HYPOT_, 1, 1, 1 },
    { PROVEN_FLOAT_FN_ASIN_, ONE, 0, PI2_64 }, { PROVEN_FLOAT_FN_ASIN_, M1, 0, PI2_64 | NZ }, { PROVEN_FLOAT_FN_ASIN_, TWO, 0, QNAN },
    { PROVEN_FLOAT_FN_ASIN_, NZ, 0, NZ }, { PROVEN_FLOAT_FN_ASIN_, INF, 0, QNAN },
    { PROVEN_FLOAT_FN_ACOS_, ONE, 0, 0 }, { PROVEN_FLOAT_FN_ACOS_, M1, 0, PI64 }, { PROVEN_FLOAT_FN_ACOS_, NZ, 0, PI2_64 },
    { PROVEN_FLOAT_FN_ACOS_, M2, 0, QNAN },
    { PROVEN_FLOAT_FN_ATAN_, INF, 0, PI2_64 }, { PROVEN_FLOAT_FN_ATAN_, NINF, 0, PI2_64 | NZ }, { PROVEN_FLOAT_FN_ATAN_, NZ, 0, NZ },
    { PROVEN_FLOAT_FN_ATAN_, ONE, 0, PI4_64 }, { PROVEN_FLOAT_FN_ATAN_, 1, 0, 1 },
    { PROVEN_FLOAT_FN_SINH_, NINF, 0, NINF }, { PROVEN_FLOAT_FN_SINH_, NZ, 0, NZ }, { PROVEN_FLOAT_FN_SINH_, 0x408f400000000000u, 0, INF },
    { PROVEN_FLOAT_FN_COSH_, NINF, 0, INF }, { PROVEN_FLOAT_FN_COSH_, NZ, 0, ONE }, { PROVEN_FLOAT_FN_COSH_, 1, 0, ONE },
    { PROVEN_FLOAT_FN_TANH_, INF, 0, ONE }, { PROVEN_FLOAT_FN_TANH_, NINF, 0, M1 }, { PROVEN_FLOAT_FN_TANH_, NZ, 0, NZ },
    { PROVEN_FLOAT_FN_TANH_, 0x4040000000000000u, 0, ONE },
    { PROVEN_FLOAT_FN_ASINH_, NINF, 0, NINF }, { PROVEN_FLOAT_FN_ASINH_, NZ, 0, NZ }, { PROVEN_FLOAT_FN_ASINH_, SNAN, 0, SNANQ },
    { PROVEN_FLOAT_FN_ACOSH_, ONE, 0, 0 }, { PROVEN_FLOAT_FN_ACOSH_, HALF, 0, QNAN }, { PROVEN_FLOAT_FN_ACOSH_, INF, 0, INF },
    { PROVEN_FLOAT_FN_ACOSH_, NINF, 0, QNAN },
    { PROVEN_FLOAT_FN_ATANH_, ONE, 0, INF }, { PROVEN_FLOAT_FN_ATANH_, M1, 0, NINF }, { PROVEN_FLOAT_FN_ATANH_, TWO, 0, QNAN },
    { PROVEN_FLOAT_FN_ATANH_, NZ, 0, NZ },
    { PROVEN_FLOAT_FN_ERF_, 0, 0, 0 }, { PROVEN_FLOAT_FN_ERF_, NZ, 0, NZ }, { PROVEN_FLOAT_FN_ERF_, INF, 0, ONE },
    { PROVEN_FLOAT_FN_ERF_, NINF, 0, M1 }, { PROVEN_FLOAT_FN_ERF_, 0x4020000000000000u, 0, ONE }, { PROVEN_FLOAT_FN_ERF_, 1, 0, 1 },
    { PROVEN_FLOAT_FN_ERF_, SNAN, 0, SNANQ },
    { PROVEN_FLOAT_FN_ERFC_, 0, 0, ONE }, { PROVEN_FLOAT_FN_ERFC_, NZ, 0, ONE }, { PROVEN_FLOAT_FN_ERFC_, INF, 0, 0 },
    { PROVEN_FLOAT_FN_ERFC_, NINF, 0, TWO }, { PROVEN_FLOAT_FN_ERFC_, 0xc020000000000000u, 0, TWO },
    { PROVEN_FLOAT_FN_ERFC_, 0x4020000000000000u, 0, 0x39ec74fc41217dfbu },               /* erfc(8) */
    { PROVEN_FLOAT_FN_ERFC_, 0x403b000000000000u, 0, 0x19e0fu },                          /* erfc(27): subnormal */
    { PROVEN_FLOAT_FN_ERFC_, 0x403b400000000000u, 0, 0 }, { PROVEN_FLOAT_FN_ERFC_, 1, 0, ONE },
    { PROVEN_FLOAT_FN_TGAMMA_, 0, 0, INF }, { PROVEN_FLOAT_FN_TGAMMA_, NZ, 0, NINF }, { PROVEN_FLOAT_FN_TGAMMA_, INF, 0, INF },
    { PROVEN_FLOAT_FN_TGAMMA_, NINF, 0, QNAN }, { PROVEN_FLOAT_FN_TGAMMA_, M1, 0, QNAN }, { PROVEN_FLOAT_FN_TGAMMA_, M2, 0, QNAN },
    { PROVEN_FLOAT_FN_TGAMMA_, ONE, 0, ONE }, { PROVEN_FLOAT_FN_TGAMMA_, THREE, 0, TWO }, { PROVEN_FLOAT_FN_TGAMMA_, SNAN, 0, SNANQ },
    { PROVEN_FLOAT_FN_TGAMMA_, 0x4065800000000000u, 0, INF },                              /* 172 */
    { PROVEN_FLOAT_FN_LGAMMA_, ONE, 0, 0 }, { PROVEN_FLOAT_FN_LGAMMA_, TWO, 0, 0 }, { PROVEN_FLOAT_FN_LGAMMA_, 0, 0, INF },
    { PROVEN_FLOAT_FN_LGAMMA_, NZ, 0, INF }, { PROVEN_FLOAT_FN_LGAMMA_, INF, 0, INF }, { PROVEN_FLOAT_FN_LGAMMA_, NINF, 0, INF },
    { PROVEN_FLOAT_FN_LGAMMA_, M1, 0, INF }, { PROVEN_FLOAT_FN_LGAMMA_, M2, 0, INF }, { PROVEN_FLOAT_FN_LGAMMA_, SNAN, 0, SNANQ },
    { PROVEN_FLOAT_FN_SINPI_, ONE, 0, 0 }, { PROVEN_FLOAT_FN_SINPI_, M1, 0, NZ }, { PROVEN_FLOAT_FN_SINPI_, HALF, 0, ONE },
    { PROVEN_FLOAT_FN_SINPI_, 0xbff8000000000000u, 0, ONE }, { PROVEN_FLOAT_FN_SINPI_, INF, 0, QNAN }, { PROVEN_FLOAT_FN_SINPI_, NZ, 0, NZ },
    { PROVEN_FLOAT_FN_COSPI_, HALF, 0, 0 }, { PROVEN_FLOAT_FN_COSPI_, 0xbff8000000000000u, 0, 0 }, { PROVEN_FLOAT_FN_COSPI_, ONE, 0, M1 },
    { PROVEN_FLOAT_FN_COSPI_, NZ, 0, ONE }, { PROVEN_FLOAT_FN_COSPI_, NINF, 0, QNAN },
    { PROVEN_FLOAT_FN_TANPI_, HALF, 0, INF }, { PROVEN_FLOAT_FN_TANPI_, 0xbfe0000000000000u, 0, NINF }, { PROVEN_FLOAT_FN_TANPI_, 0x3ff8000000000000u, 0, NINF },
    { PROVEN_FLOAT_FN_TANPI_, ONE, 0, NZ }, { PROVEN_FLOAT_FN_TANPI_, TWO, 0, 0 }, { PROVEN_FLOAT_FN_TANPI_, 0x3fd0000000000000u, 0, ONE },
    { PROVEN_FLOAT_FN_TANPI_, 0x3fe8000000000000u, 0, M1 }, { PROVEN_FLOAT_FN_TANPI_, NZ, 0, NZ },
    { PROVEN_FLOAT_FN_RSQRT_, 0, 0, INF }, { PROVEN_FLOAT_FN_RSQRT_, NZ, 0, NINF }, { PROVEN_FLOAT_FN_RSQRT_, INF, 0, 0 },
    { PROVEN_FLOAT_FN_RSQRT_, M1, 0, QNAN }, { PROVEN_FLOAT_FN_RSQRT_, 0x4010000000000000u, 0, HALF },
    { PROVEN_FLOAT_FN_POWN_, QNAN, 0, ONE }, { PROVEN_FLOAT_FN_POWN_, SNAN, 0, SNANQ }, { PROVEN_FLOAT_FN_POWN_, NZ, (uint64_t)-3, NINF },
    { PROVEN_FLOAT_FN_POWN_, NINF, 3, NINF }, { PROVEN_FLOAT_FN_POWN_, M1, (uint64_t)INT64_MIN, ONE }, { PROVEN_FLOAT_FN_POWN_, TWO, 10, 0x4090000000000000u },
    { PROVEN_FLOAT_FN_POWN_, 0x3ff0000000000001u, (uint64_t)1 << 62, 0x7ff0000000000000u },
    { PROVEN_FLOAT_FN_ROOTN_, 0xc020000000000000u, 3, M2 }, { PROVEN_FLOAT_FN_ROOTN_, TWO, 0, QNAN }, { PROVEN_FLOAT_FN_ROOTN_, M2, 2, QNAN },
    { PROVEN_FLOAT_FN_ROOTN_, NZ, (uint64_t)-3, NINF }, { PROVEN_FLOAT_FN_ROOTN_, NINF, 3, NINF }, { PROVEN_FLOAT_FN_ROOTN_, 0x4050000000000000u, 6, TWO },
    { PROVEN_FLOAT_FN_COMPOUNDN_, M1, (uint64_t)-1, INF }, { PROVEN_FLOAT_FN_COMPOUNDN_, M1, 2, 0 }, { PROVEN_FLOAT_FN_COMPOUNDN_, M2, 0, QNAN },
    { PROVEN_FLOAT_FN_COMPOUNDN_, QNAN, 0, ONE }, { PROVEN_FLOAT_FN_COMPOUNDN_, ONE, 10, 0x4090000000000000u }, { PROVEN_FLOAT_FN_COMPOUNDN_, NINF, 1, QNAN },
};

int main(void) {
    PROVEN_TEST_SUITE("float_math: math.h's transcendental functions correctly rounded",
        "RFC-0056, RFC-0058: exp, exp2, exp10, expm1, log, log2, log10, log1p, pow, cbrt, hypot, the trigonometric and hyperbolic functions and their inverses, erf, erfc, tgamma, lgamma, sinpi, cospi, tanpi, rsqrt, pown, rootn and compoundn; the special values of Annex F as bits, hard-to-round and random inputs with MPFR's results through both paths, and the two paths against each other.",
        "Inspect math/src/float_math.c: the special branches of fm_exp / fm_log / fm_trig / fm_atan2 / fm_pow / fm_x_special, mp_round_test, and the function named; for the FPU path math/src/cm and fm_fix.");

    PROVEN_TEST_SECTION("which path", "On x86-64 with FMA and AVX2 the public functions take the FPU path (CORE-MATH), unless PROVEN_FLOAT_SOFT or PROVEN_FLOAT_MATH_SMALL.", "Check cm_config.h and fm_cm_ready in math/src/float_math.c.");
    {
#if defined(__x86_64__) && defined(__GNUC__) && !PROVEN_FLOAT_SOFT && !PROVEN_FLOAT_MATH_SMALL && !defined(__FAST_MATH__)
        const bool want = __builtin_cpu_supports("fma") && __builtin_cpu_supports("avx2");
        PROVEN_TEST_ASSERT(proven_float_math_is_fpu_() == want, want ? "the FPU path is taken" : "no FMA and AVX2 here: the integer path", "");
#else
        PROVEN_TEST_ASSERT(true, "not x86-64, or the integer path forced: reported, not checked", "");
#endif
    }

    PROVEN_TEST_SECTION("special values", "Annex F's cases as bits: zeros, infinities, NaNs (the first operand, quieted; a NaN made is 0x7FF8000000000000), the exact powers, over- and underflow.", "Check the special branches of the function in the failing row.");
    {
        int bad = 0;
        for (size_t i = 0; i < sizeof SPECIAL64 / sizeof SPECIAL64[0]; i++) {
            const row_t *r = &SPECIAL64[i];
            const uint64_t gp = pub(r->fn, false, r->a, r->b), gi = proven_float_math_int_(r->fn, false, r->a, r->b);
            uint64_t want_pub = r->want;
#if defined(__i386__)
            want_pub = proven_float_math_int_(r->fn, false, quiet64(r->a), quiet64(r->b));
#endif
            if (gp != want_pub || gi != r->want) {
                bad++;
                PROVEN_TEST_INFO("row {}: public {} integer {}", PROVEN_ARG((unsigned long long)i), PROVEN_ARG((unsigned long long)gp), PROVEN_ARG((unsigned long long)gi));
            }
        }
        PROVEN_TEST_ASSERT(bad == 0, "every special row of binary64, both paths", "");
        /* binary32: the same cases where they differ in form */
        const uint32_t pi32 = 0x40490fdbu, one32 = 0x3f800000u;
        PROVEN_TEST_ASSERT(BF(proven_float32_atan2(F(0), F(0x80000000u))) == pi32 && BF(proven_float32_log(F(0xbf800000u))) == 0x7fc00000u
            && BF(proven_float32_exp(F(0xff800000u))) == 0 && BF(proven_float32_pow(F(0x7fc00001u), F(0))) == one32
            && BF(proven_float32_pow(F(0x7fa00000u), F(one32))) == 0x7fe00000u && BF(proven_float32_pow(F(0x7fa00000u), F(0))) == SNAN32_POW0 && BF(proven_float32_cos(F(0x80000000u))) == one32
            && BF(proven_float32_sin(F(0x7f800000u))) == 0x7fc00000u && BF(proven_float32_pow(F(0x40400000u), F(0x40000000u))) == 0x41100000u,
            "binary32: atan2(+0, -0) is pi, log(-1) and sin(inf) 0x7FC00000, pow(NaN, 0) = 1, 3^2 = 9", "");
        PROVEN_TEST_ASSERT(BF(proven_float32_exp10(F(0x41200000u))) == 0x501502f9u && BF(proven_float32_log2(F(0x00000001u))) == 0xc3150000u
            && BF(proven_float32_cbrt(F(0xc1d80000u))) == 0xc0400000u && BF(proven_float32_hypot(F(0x7f800000u), F(0x7fc00000u))) == 0x7f800000u
            && BF(proven_float32_hypot(F(0x40a00000u), F(0x41400000u))) == 0x41500000u && BF(proven_float32_asin(F(0x40000000u))) == 0x7fc00000u
            && BF(proven_float32_acos(F(0xbf800000u))) == pi32 && BF(proven_float32_atanh(F(0xbf800000u))) == 0xff800000u
            && BF(proven_float32_expm1(F(0xff800000u))) == 0xbf800000u && BF(proven_float32_cosh(F(0x80000000u))) == one32,
            "binary32: 10^10, log2 of the smallest subnormal (-149), cbrt(-27), hypot(inf, NaN), 5-12-13, asin(2), acos(-1), atanh(-1), expm1(-inf), cosh(-0)", "");
        PROVEN_TEST_ASSERT(BF(proven_float32_erf(F(0xc0800000u))) == 0xbf800000u && BF(proven_float32_erf(F(1))) == 1
            && BF(proven_float32_erfc(F(0x40800000u))) == 0x32846ee9u && BF(proven_float32_erfc(F(0xc0800000u))) == 0x40000000u
            && BF(proven_float32_erfc(F(0x41200000u))) == 1 && BF(proven_float32_erfc(F(0x41210000u))) == 0,
            "binary32: erf(-4) = -1, erf of the smallest subnormal, erfc(4), erfc(-4) = 2, erfc(10) the smallest subnormal, erfc(10.0625) = +0", "");
    }

    PROVEN_TEST_SECTION("MPFR's results", "Hard-to-round inputs from CORE-MATH's lists and random ones, each with MPFR's correctly rounded result, through the public function and the integer path.", "Run the failing input through the private MPFR check; then the path that differs.");
    {
        int bad_pub = 0, bad_int = 0;
        const size_t n = sizeof FM_VECTORS / sizeof FM_VECTORS[0];
        for (size_t i = 0; i < n; i++) {
            const fm_vector_t *v = &FM_VECTORS[i];
            uint64_t want_pub = v->want;
#if defined(__i386__)
            /* 32-bit x86 quiets a signalling NaN on its way through the x87 registers (float_ops.h):
             * the public function sees it quiet, as the integer path does given it quiet */
            const bool nth = v->fn == PROVEN_FLOAT_FN_POWN_ || v->fn == PROVEN_FLOAT_FN_ROOTN_ || v->fn == PROVEN_FLOAT_FN_COMPOUNDN_;
#define Q32(u) ((((u) & 0x7fffffffu) > 0x7f800000u) ? (u) | 0x00400000u : (u))
            const uint64_t qa = v->b32 ? Q32(v->a) : quiet64(v->a);
            const uint64_t qb = nth ? v->b : v->b32 ? Q32(v->b) : quiet64(v->b);   /* n is an integer, not a float */
#undef Q32
            want_pub = proven_float_math_int_(v->fn, v->b32 != 0, qa, qb);
#endif
            if (pub(v->fn, v->b32 != 0, v->a, v->b) != want_pub) bad_pub++;
            if (proven_float_math_int_(v->fn, v->b32 != 0, v->a, v->b) != v->want) bad_int++;
        }
        char msg[160];
        snprintf(msg, sizeof msg, "%zu inputs: the public functions (%s path) give MPFR's bits", n, proven_float_math_is_fpu_() ? "FPU" : "integer");
        PROVEN_TEST_ASSERT(bad_pub == 0, msg, "");
        snprintf(msg, sizeof msg, "%zu inputs: the integer path gives MPFR's bits", n);
        PROVEN_TEST_ASSERT(bad_int == 0, msg, "");
    }

    PROVEN_TEST_SECTION("the two paths agree", "Random inputs of each function and format: the public function and the integer path give the same bits (both are correctly rounded, so any difference is a defect in one).", "Run the input through the private MPFR check to see which path is wrong.");
    {
        uint64_t seed = 0x243f6a8885a308d3u;
        int bad = 0;
        for (int fn = PROVEN_FLOAT_FN_EXP_; fn < PROVEN_FLOAT_FN_COUNT_; fn++) {
            for (int s = 0; s <= 1; s++) {
                for (int i = 0; i < 300; i++) {
                    seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
                    const uint64_t r1 = seed;
                    seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
                    const uint64_t r2 = seed;
                    /* exponents near 1, where the functions are not trivial */
                    uint64_t a, b;
                    if (s) {
                        a = (r1 & 0x807fffffu) | ((uint64_t)(117 + (r1 >> 40) % 20) << 23);
                        b = (r2 & 0x807fffffu) | ((uint64_t)(117 + (r2 >> 40) % 20) << 23);
                    } else {
                        a = (r1 & 0x800fffffffffffffu) | ((uint64_t)(1013 + (r1 >> 52) % 20) << 52);
                        b = (r2 & 0x800fffffffffffffu) | ((uint64_t)(1013 + (r2 >> 52) % 20) << 52);
                    }
                    /* pown, rootn, compoundn: the second operand is the integer n, here -20 .. 20 */
                    if (fn == PROVEN_FLOAT_FN_POWN_ || fn == PROVEN_FLOAT_FN_ROOTN_ || fn == PROVEN_FLOAT_FN_COMPOUNDN_)
                        b = (uint64_t)((int64_t)(r2 % 41) - 20);
                    if (pub(fn, s, a, b) != proven_float_math_int_(fn, s, a, b)) bad++;
                }
            }
        }
        PROVEN_TEST_ASSERT(bad == 0, "300 inputs of each function and format: the same bits on both paths", "");
    }

    PROVEN_TEST_PASS("float_math checks passed.");
    return 0;
}
