#include "proven/complex.h"
#include "proven/float_ops.h"
#include "proven_test.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
 * complex.h (RFC-0058 D-20): C23 Annex G's special values as bits, a few exact results, and a
 * hash of each function's bits over fixed inputs of every kind - the same on every target, which
 * is what this header promises (the hashes were taken on x86-64; a target that differs fails
 * here). The accuracy of each function against GNU MPC is a private check (ops/checks/complex),
 * which this suite does not link.
 */

static uint64_t B(double x) { uint64_t u; memcpy(&u, &x, sizeof u); return u; }
static double D(uint64_t u) { double x; memcpy(&x, &u, sizeof x); return x; }
static uint32_t BF(float x) { uint32_t u; memcpy(&u, &x, sizeof u); return u; }

typedef proven_complex_t cx;
typedef cx (*fn1_t)(cx);
typedef proven_complex32_t cf;
typedef cf (*fn1f_t)(cf);

enum { EXP, LOG, SQRT, SIN, COS, TAN, ASIN, ACOS, ATAN, SINH, COSH, TANH, ASINH, ACOSH, ATANH, N1 };
static const char *NAMES[N1] = { "exp", "log", "sqrt", "sin", "cos", "tan", "asin", "acos", "atan", "sinh", "cosh", "tanh", "asinh", "acosh", "atanh" };
static const fn1_t FN[N1] = { proven_complex_exp, proven_complex_log, proven_complex_sqrt, proven_complex_sin, proven_complex_cos,
                              proven_complex_tan, proven_complex_asin, proven_complex_acos, proven_complex_atan, proven_complex_sinh,
                              proven_complex_cosh, proven_complex_tanh, proven_complex_asinh, proven_complex_acosh, proven_complex_atanh };
static const fn1f_t FNF[N1] = { proven_complex32_exp, proven_complex32_log, proven_complex32_sqrt, proven_complex32_sin, proven_complex32_cos,
                                proven_complex32_tan, proven_complex32_asin, proven_complex32_acos, proven_complex32_atan, proven_complex32_sinh,
                                proven_complex32_cosh, proven_complex32_tanh, proven_complex32_asinh, proven_complex32_acosh, proven_complex32_atanh };

#define Z 0x0000000000000000u
#define NZ 0x8000000000000000u
#define ONE 0x3ff0000000000000u
#define M1 0xbff0000000000000u
#define X15 0x3ff8000000000000u                  /* 1.5 */
#define TWO 0x4000000000000000u
#define INF 0x7ff0000000000000u
#define NINF 0xfff0000000000000u
#define NAN_ 0x7ff8000000000000u
#define PI 0x400921fb54442d18u
#define PI2 0x3ff921fb54442d18u
#define PI4 0x3fe921fb54442d18u
#define PI34 0x4002d97c7f3321d2u

typedef struct { int fn; uint64_t x, y, re, im; } row_t;

/* Annex G G.6, as bits; where it leaves a sign unspecified, the one this library gives. */
static const row_t ROWS[] = {
    /* cexp G.6.3.1 */
    { EXP, Z, Z, ONE, Z }, { EXP, NZ, Z, ONE, Z }, { EXP, X15, INF, NAN_, NAN_ }, { EXP, X15, NAN_, NAN_, NAN_ },
    { EXP, INF, Z, INF, Z }, { EXP, NINF, ONE, Z, Z }, { EXP, NINF, TWO, NZ, Z }, { EXP, INF, ONE, INF, INF },
    { EXP, INF, TWO, NINF, INF }, { EXP, NINF, INF, Z, Z }, { EXP, INF, INF, INF, NAN_ }, { EXP, NINF, NAN_, Z, Z },
    { EXP, INF, NAN_, INF, NAN_ }, { EXP, NAN_, Z, NAN_, Z }, { EXP, NAN_, NZ, NAN_, NZ }, { EXP, NAN_, ONE, NAN_, NAN_ },
    { EXP, NAN_, NAN_, NAN_, NAN_ }, { EXP, INF, M1, INF, NINF },
    /* clog G.6.3.2 */
    { LOG, NZ, Z, NINF, PI }, { LOG, Z, Z, NINF, Z }, { LOG, X15, INF, INF, PI2 }, { LOG, X15, NAN_, NAN_, NAN_ },
    { LOG, NINF, ONE, INF, PI }, { LOG, INF, ONE, INF, Z }, { LOG, NINF, INF, INF, PI34 }, { LOG, INF, INF, INF, PI4 },
    { LOG, INF, NAN_, INF, NAN_ }, { LOG, NINF, NAN_, INF, NAN_ }, { LOG, NAN_, ONE, NAN_, NAN_ }, { LOG, NAN_, INF, INF, NAN_ },
    { LOG, NAN_, NAN_, NAN_, NAN_ }, { LOG, Z, NZ, NINF, NZ }, { LOG, M1, NZ, Z, PI | NZ }, { LOG, ONE, Z, Z, Z },
    /* csqrt G.6.4.2 */
    { SQRT, Z, Z, Z, Z }, { SQRT, NZ, Z, Z, Z }, { SQRT, X15, INF, INF, INF }, { SQRT, NAN_, INF, INF, INF },
    { SQRT, X15, NAN_, NAN_, NAN_ }, { SQRT, NINF, ONE, Z, INF }, { SQRT, INF, ONE, INF, Z }, { SQRT, NINF, NAN_, NAN_, INF },
    { SQRT, INF, NAN_, INF, NAN_ }, { SQRT, NAN_, ONE, NAN_, NAN_ }, { SQRT, NAN_, NAN_, NAN_, NAN_ },
    { SQRT, NINF, M1, Z, NINF }, { SQRT, 0xc010000000000000u, Z, Z, TWO }, { SQRT, 0xc010000000000000u, NZ, Z, TWO | NZ },
    { SQRT, X15, NINF, INF, NINF },
    /* csinh G.6.2.5 */
    { SINH, Z, Z, Z, Z }, { SINH, Z, INF, Z, NAN_ }, { SINH, Z, NAN_, Z, NAN_ }, { SINH, X15, INF, NAN_, NAN_ },
    { SINH, X15, NAN_, NAN_, NAN_ }, { SINH, INF, Z, INF, Z }, { SINH, INF, ONE, INF, INF }, { SINH, INF, TWO, NINF, INF },
    { SINH, INF, INF, INF, NAN_ }, { SINH, INF, NAN_, INF, NAN_ }, { SINH, NAN_, Z, NAN_, Z }, { SINH, NAN_, ONE, NAN_, NAN_ },
    { SINH, NAN_, NAN_, NAN_, NAN_ }, { SINH, NZ, NZ, NZ, NZ }, { SINH, NINF, Z, NINF, Z },
    /* ccosh G.6.2.4 */
    { COSH, Z, Z, ONE, Z }, { COSH, Z, INF, NAN_, Z }, { COSH, Z, NAN_, NAN_, Z }, { COSH, X15, INF, NAN_, NAN_ },
    { COSH, X15, NAN_, NAN_, NAN_ }, { COSH, INF, Z, INF, Z }, { COSH, INF, ONE, INF, INF }, { COSH, INF, INF, INF, NAN_ },
    { COSH, INF, NAN_, INF, NAN_ }, { COSH, NAN_, Z, NAN_, Z }, { COSH, NAN_, ONE, NAN_, NAN_ }, { COSH, NAN_, NAN_, NAN_, NAN_ },
    { COSH, NZ, Z, ONE, NZ },
    /* ctanh G.6.2.6 (C23) */
    { TANH, Z, Z, Z, Z }, { TANH, Z, INF, Z, NAN_ }, { TANH, Z, NAN_, Z, NAN_ }, { TANH, X15, INF, NAN_, NAN_ },
    { TANH, X15, NAN_, NAN_, NAN_ }, { TANH, INF, ONE, ONE, Z }, { TANH, INF, TWO, ONE, NZ }, { TANH, INF, INF, ONE, Z },
    { TANH, INF, NAN_, ONE, Z }, { TANH, NAN_, Z, NAN_, Z }, { TANH, NAN_, ONE, NAN_, NAN_ }, { TANH, NAN_, NAN_, NAN_, NAN_ },
    { TANH, NINF, NZ, M1, NZ },
    /* casinh G.6.2.2 */
    { ASINH, Z, Z, Z, Z }, { ASINH, X15, INF, INF, PI2 }, { ASINH, X15, NAN_, NAN_, NAN_ }, { ASINH, INF, ONE, INF, Z },
    { ASINH, INF, INF, INF, PI4 }, { ASINH, INF, NAN_, INF, NAN_ }, { ASINH, NAN_, Z, NAN_, Z }, { ASINH, NAN_, ONE, NAN_, NAN_ },
    { ASINH, NAN_, INF, INF, NAN_ }, { ASINH, NAN_, NAN_, NAN_, NAN_ }, { ASINH, NZ, NZ, NZ, NZ },
    /* cacos G.6.1.1 */
    { ACOS, Z, Z, PI2, NZ }, { ACOS, NZ, Z, PI2, NZ }, { ACOS, Z, NAN_, PI2, NAN_ }, { ACOS, X15, INF, PI2, NINF },
    { ACOS, X15, NAN_, NAN_, NAN_ }, { ACOS, NINF, ONE, PI, NINF }, { ACOS, INF, ONE, Z, NINF }, { ACOS, NINF, INF, PI34, NINF },
    { ACOS, INF, INF, PI4, NINF }, { ACOS, INF, NAN_, NAN_, NINF }, { ACOS, NAN_, ONE, NAN_, NAN_ }, { ACOS, NAN_, INF, NAN_, NINF },
    { ACOS, NAN_, NAN_, NAN_, NAN_ }, { ACOS, X15, NINF, PI2, INF }, { ACOS, ONE, Z, Z, NZ }, { ACOS, M1, Z, PI, NZ },
    /* cacosh G.6.2.1 */
    { ACOSH, Z, Z, Z, PI2 }, { ACOSH, X15, INF, INF, PI2 }, { ACOSH, X15, NAN_, NAN_, NAN_ }, { ACOSH, NINF, ONE, INF, PI },
    { ACOSH, INF, ONE, INF, Z }, { ACOSH, NINF, INF, INF, PI34 }, { ACOSH, INF, INF, INF, PI4 }, { ACOSH, INF, NAN_, INF, NAN_ },
    { ACOSH, NAN_, ONE, NAN_, NAN_ }, { ACOSH, NAN_, INF, INF, NAN_ }, { ACOSH, NAN_, NAN_, NAN_, NAN_ }, { ACOSH, ONE, Z, Z, Z },
    /* catanh G.6.2.3 */
    { ATANH, Z, Z, Z, Z }, { ATANH, Z, NAN_, Z, NAN_ }, { ATANH, ONE, Z, INF, Z }, { ATANH, X15, INF, Z, PI2 },
    { ATANH, X15, NAN_, NAN_, NAN_ }, { ATANH, INF, ONE, Z, PI2 }, { ATANH, INF, INF, Z, PI2 }, { ATANH, INF, NAN_, Z, NAN_ },
    { ATANH, NAN_, ONE, NAN_, NAN_ }, { ATANH, NAN_, INF, Z, PI2 }, { ATANH, NAN_, NAN_, NAN_, NAN_ }, { ATANH, M1, NZ, NINF, NZ },
    /* sin, cos, tan, asin, atan: the hyperbolic ones turned by i */
    { SIN, Z, Z, Z, Z }, { SIN, NZ, NZ, NZ, NZ }, { COS, Z, Z, ONE, NZ }, { TAN, Z, Z, Z, Z }, { TAN, Z, INF, Z, ONE },
    { ASIN, Z, Z, Z, Z }, { ASIN, ONE, Z, PI2, Z }, { ASIN, Z, INF, Z, INF }, { ATAN, Z, Z, Z, Z }, { ATAN, Z, ONE, Z, INF },
    { ATAN, INF, ONE, PI2, Z }, { SIN, X15, NAN_, NAN_, NAN_ }, { COS, NAN_, Z, NAN_, NZ },
};

#include "test_unit_complex_sweep.h"

static void hashes(uint64_t out[2][20]) {
    for (int s = 0; s <= 1; s++) for (int f = 0; f < 20; f++) out[s][f] = cxs_hash(s, f);
}

/* taken on x86-64 (both float_math paths give these: the functions underneath are correctly
 * rounded), and so on every target */
static const uint64_t WANT[2][20] = {
#include "test_unit_complex_hashes.h"
};

int main(void) {
    PROVEN_TEST_SUITE("complex: C's complex.h as a pair of values",
        "RFC-0058 D-20: Annex G's special values as bits, exact results, and the same bits on every target (hashes of each function over fixed inputs).",
        "Inspect math/src/complex.c: the first-quadrant special branches of the function named, cx_out, and the formula of its finite case.");

    PROVEN_TEST_SECTION("Annex G's special values", "Each row of G.6 that names a result, as bits (a NaN is 0x7FF8000000000000), the unspecified signs as the manual lists them; binary32 through the same rows.", "The row printed: compare it with Annex G and the function's special branch.");
    {
        int bad = 0, badf = 0;
        for (size_t i = 0; i < sizeof ROWS / sizeof ROWS[0]; i++) {
            const row_t *r = &ROWS[i];
            const cx g = FN[r->fn](proven_complex(D(r->x), D(r->y)));
            if (B(g.re) != r->re || B(g.im) != r->im) {
                bad++;
                PROVEN_TEST_INFO("row {} ({}): got {} {}", PROVEN_ARG((unsigned long long)i), PROVEN_ARG(NAMES[r->fn]),
                                 PROVEN_ARG((unsigned long long)B(g.re)), PROVEN_ARG((unsigned long long)B(g.im)));
            }
            /* binary32: the same row narrowed (the exact pi multiples rounded to binary32) */
            const cf gf = FNF[r->fn](proven_complex32(proven_float_to_float32(D(r->x)), proven_float_to_float32(D(r->y))));
            if (BF(gf.re) != BF(proven_float_to_float32(D(r->re))) || BF(gf.im) != BF(proven_float_to_float32(D(r->im)))) badf++;
        }
        PROVEN_TEST_ASSERT(bad == 0, "every Annex G row, binary64", "");
        PROVEN_TEST_ASSERT(badf == 0, "every Annex G row, binary32", "");
    }

    PROVEN_TEST_SECTION("arithmetic and exact results", "mul and div with Annex G's infinities, the conjugate and projection, and results that are exact.", "Check the function named in the failing assertion.");
    {
        const cx a = proven_complex(3.0, 4.0), b = proven_complex(1.0, -2.0);
        const cx m = proven_complex_mul(a, b), q = proven_complex_div(m, b);
        PROVEN_TEST_ASSERT(m.re == 11.0 && m.im == -2.0 && q.re == 3.0 && q.im == 4.0, "(3 + 4i)(1 - 2i) = 11 - 2i, and back", "");
        PROVEN_TEST_ASSERT(proven_complex_abs(a) == 5.0 && proven_complex_abs(proven_complex(D(INF), D(NAN_))) == D(INF), "|3 + 4i| = 5, |inf + i NaN| = inf", "");
        const cx s = proven_complex_sqrt(proven_complex(-4.0, 0.0)), s2 = proven_complex_sqrt(proven_complex(-3.0, 4.0));
        PROVEN_TEST_ASSERT(B(s.re) == Z && s.im == 2.0 && s2.re == 1.0 && s2.im == 2.0, "sqrt(-4) = 2i and sqrt(-3 + 4i) = 1 + 2i, exactly", "");
        const cx mi = proven_complex_mul(proven_complex(D(INF), D(NAN_)), proven_complex(1.0, 1.0));
        PROVEN_TEST_ASSERT(B(mi.re) == INF || B(mi.im) == INF || B(mi.re) == NINF || B(mi.im) == NINF, "(inf + i NaN)(1 + i) is infinite (Annex G G.5.2)", "");
        const cx dz = proven_complex_div(proven_complex(1.0, 1.0), proven_complex(0.0, 0.0));
        PROVEN_TEST_ASSERT(B(dz.re) == INF && B(dz.im) == INF, "(1 + i) / 0 = inf + i inf", "");
        const cx di = proven_complex_div(proven_complex(1.0, 1.0), proven_complex(D(INF), 0.0));
        PROVEN_TEST_ASSERT(di.re == 0.0 && di.im == 0.0, "(1 + i) / inf = 0", "");
        const cx mz = proven_complex_mul(proven_complex(-0.0, 1.0), proven_complex(1.0, 0.0));
        PROVEN_TEST_ASSERT(B(mz.re) == NZ && mz.im == 1.0, "(-0 + i)(1 + 0i) = -0 + i: an exact zero keeps IEEE's sign, so the point stays on its side of a cut", "");
        const cx big = proven_complex_mul(proven_complex(0x1p600, 0x1p-900), proven_complex(0x1p500, 0.0));
        PROVEN_TEST_ASSERT(B(big.re) == INF && big.im == 0x1p-400, "a product past the range overflows its own part only: 2^1100 + i 2^-400", "");
        const cx c = proven_complex_conj(proven_complex(1.0, D(NAN_ | 5))), p = proven_complex_proj(proven_complex(D(NINF), -2.0));
        PROVEN_TEST_ASSERT(B(c.im) == (NAN_ | 5 | NZ) && B(p.re) == INF && B(p.im) == NZ, "conj flips the sign of a NaN and keeps it; proj(-inf - 2i) = inf - i0", "");
        const cx e = proven_complex_exp(proven_complex(0.0, 3.141592653589793));
        PROVEN_TEST_ASSERT(e.re == -1.0 && B(e.im) == 0x3ca1a62633145c07u, "exp(i pi) = -1 + i 1.2246467991473532e-16 (sin of pi rounded)", "");
        const cx l = proven_complex_log(proven_complex(1.0, 0x1p-30));
        PROVEN_TEST_ASSERT(B(l.re) == 0x3c20000000000000u, "log|1 + 2^-30 i| = 2^-61 rounded, not 0: x^2 + y^2 - 1 is summed exactly near |z| = 1", "");
        const cx pw = proven_complex_pow(proven_complex(D(NAN_), 1.0), proven_complex(0.0, 0.0));
        PROVEN_TEST_ASSERT(pw.re == 1.0 && pw.im == 0.0, "z^0 = 1, even for a NaN z", "");
        const cf f = proven_complex32_mul(proven_complex32(3.0f, 4.0f), proven_complex32(1.0f, -2.0f));
        PROVEN_TEST_ASSERT(f.re == 11.0f && f.im == -2.0f && proven_complex32_abs(proven_complex32(5.0f, 12.0f)) == 13.0f, "binary32: the product and |5 + 12i| = 13", "");
    }

    PROVEN_TEST_SECTION("to and from C's _Complex", "The conversions move both parts bit for bit, a NaN payload and a signed zero included, both ways and both formats.", "Check proven_complex_from_c and _to_c in complex.h: the union must hold the real part first.");
    {
#if PROVEN_HAVE_C_COMPLEX
        const cx z = proven_complex(D(NAN_ | 7 | NZ), -0.0);
        const cx back = proven_complex_from_c(proven_complex_to_c(z));
        PROVEN_TEST_ASSERT(B(back.re) == B(z.re) && B(back.im) == NZ, "binary64: a NaN with a payload and -0 come back as they went", "");
        double _Complex c = proven_complex_to_c(proven_complex(1.0, 2.0));
        double parts[2];
        memcpy(parts, &c, sizeof parts);
        PROVEN_TEST_ASSERT(parts[0] == 1.0 && parts[1] == 2.0, "binary64: the real part first, as C lays a complex number out", "");
        const cx m = proven_complex_from_c(proven_complex_to_c(proven_complex_mul(proven_complex(1.0, 2.0), proven_complex(3.0, -4.0))));
        PROVEN_TEST_ASSERT(m.re == 11.0 && m.im == 2.0, "binary64: a result through _Complex and back", "");
        const cf zf = proven_complex32(-0.0f, 3.5f);
        const cf bf = proven_complex32_from_c(proven_complex32_to_c(zf));
        PROVEN_TEST_ASSERT(BF(bf.re) == 0x80000000u && bf.im == 3.5f, "binary32: -0 and 3.5 come back as they went", "");
        float _Complex g = proven_complex32_to_c(proven_complex32(5.0f, -6.0f));
        float gp[2];
        memcpy(gp, &g, sizeof gp);
        PROVEN_TEST_ASSERT(gp[0] == 5.0f && gp[1] == -6.0f, "binary32: the real part first", "");
#else
        PROVEN_TEST_INFO("{}", PROVEN_ARG("this compiler has no _Complex: the conversions are not declared"));
#endif
    }

    PROVEN_TEST_SECTION("the same bits everywhere", "A hash of each function's result bits over 400 fixed inputs of every kind, both formats, against the hashes taken on x86-64.", "A target whose hash differs computes some function differently: rerun with the inputs printed and compare part by part.");
    {
        uint64_t got[2][20];
        hashes(got);
        int bad = 0;
        for (int s = 0; s <= 1; s++) for (int f = 0; f < 20; f++) {
            if (got[s][f] != WANT[s][f]) {
                bad++;
                PROVEN_TEST_INFO("function {} format {}: hash {}", PROVEN_ARG(f), PROVEN_ARG(s), PROVEN_ARG((unsigned long long)got[s][f]));
            }
        }
#ifdef CX_PRINT_HASHES
        for (int s = 0; s <= 1; s++) { printf("    {"); for (int f = 0; f < 20; f++) printf(" 0x%016llxu,", (unsigned long long)got[s][f]); printf(" },\n"); }
#endif
        PROVEN_TEST_ASSERT(bad == 0, "every function's bits, both formats, as on x86-64", "");
    }

    PROVEN_TEST_PASS("complex checks passed.");
    return 0;
}
