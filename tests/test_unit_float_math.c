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
 * float_math (RFC-0056): exp, log, pow, sin, cos, tan and atan2, correctly rounded, binary64
 * and binary32. The special values of C23 Annex F as bits; hard-to-round inputs and random ones
 * with MPFR's results (test_unit_float_math_vectors.h) through the public function and through
 * the integer path; and the two paths against each other on random inputs. The full lists
 * (about nine million inputs) and binary32's sampled exhaustively are a private check run
 * against MPFR, which this suite does not link.
 */

static uint64_t B(double x) { uint64_t u; memcpy(&u, &x, sizeof u); return u; }
static double D(uint64_t u) { double x; memcpy(&x, &u, sizeof x); return x; }
static uint32_t BF(float x) { uint32_t u; memcpy(&u, &x, sizeof u); return u; }
static float F(uint32_t u) { float x; memcpy(&x, &u, sizeof x); return x; }

static uint64_t pub(int fn, bool s, uint64_t a, uint64_t b) {
    if (s) {
        const float x = F((uint32_t)a), y = F((uint32_t)b);
        switch (fn) {
        case PROVEN_FLOAT_FN_EXP_: return BF(proven_float32_exp(x));
        case PROVEN_FLOAT_FN_LOG_: return BF(proven_float32_log(x));
        case PROVEN_FLOAT_FN_SIN_: return BF(proven_float32_sin(x));
        case PROVEN_FLOAT_FN_COS_: return BF(proven_float32_cos(x));
        case PROVEN_FLOAT_FN_TAN_: return BF(proven_float32_tan(x));
        case PROVEN_FLOAT_FN_ATAN2_: return BF(proven_float32_atan2(x, y));
        default: return BF(proven_float32_pow(x, y));
        }
    }
    const double x = D(a), y = D(b);
    switch (fn) {
    case PROVEN_FLOAT_FN_EXP_: return B(proven_float_exp(x));
    case PROVEN_FLOAT_FN_LOG_: return B(proven_float_log(x));
    case PROVEN_FLOAT_FN_SIN_: return B(proven_float_sin(x));
    case PROVEN_FLOAT_FN_COS_: return B(proven_float_cos(x));
    case PROVEN_FLOAT_FN_TAN_: return B(proven_float_tan(x));
    case PROVEN_FLOAT_FN_ATAN2_: return B(proven_float_atan2(x, y));
    default: return B(proven_float_pow(x, y));
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
};

int main(void) {
    PROVEN_TEST_SUITE("float_math: exp, log, pow, sin, cos, tan, atan2 correctly rounded",
        "RFC-0056: the special values of Annex F as bits, hard-to-round and random inputs with MPFR's results through both paths, and the two paths against each other.",
        "Inspect math/src/float_math.c: the special branches of fm_exp / fm_log / fm_trig / fm_atan2 / fm_pow, mp_round_test, and the function named; for the FPU path math/src/cm and fm_fix.");

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
    }

    PROVEN_TEST_SECTION("MPFR's results", "Hard-to-round inputs from CORE-MATH's lists and random ones, each with MPFR's correctly rounded result, through the public function and the integer path.", "Run the failing input through the private MPFR check; then the path that differs.");
    {
        int bad_pub = 0, bad_int = 0;
        const size_t n = sizeof FM_VECTORS / sizeof FM_VECTORS[0];
        for (size_t i = 0; i < n; i++) {
            const fm_vector_t *v = &FM_VECTORS[i];
            if (pub(v->fn, v->b32 != 0, v->a, v->b) != v->want) bad_pub++;
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
        for (int fn = PROVEN_FLOAT_FN_EXP_; fn <= PROVEN_FLOAT_FN_POW_; fn++) {
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
                    if (pub(fn, s, a, b) != proven_float_math_int_(fn, s, a, b)) bad++;
                }
            }
        }
        PROVEN_TEST_ASSERT(bad == 0, "300 inputs of each function and format: the same bits on both paths", "");
    }

    PROVEN_TEST_PASS("float_math checks passed.");
    return 0;
}
