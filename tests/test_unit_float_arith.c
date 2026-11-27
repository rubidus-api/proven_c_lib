#include "proven/float_ops.h"
#include "proven/math_config.h"
#include "proven_test.h"
#include "../math/src/proven_internal_float_ops.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
 * float_ops arithmetic and binary32 (RFC-0055). Tables of bits for the specials and the NaN
 * rule; then, on millions of inputs weighted toward the edges (cancellation, ties, the
 * subnormals, overflow), the integer implementation against this host's SSE instructions bit
 * for bit, and fma against the FMA instruction where the CPU has one and against an exact
 * product error made without fma. No libm: the build does not link it.
 */

static uint64_t B(double x) { uint64_t u; memcpy(&u, &x, sizeof u); return u; }
static double D(uint64_t u) { double x; memcpy(&x, &u, sizeof x); return x; }
static uint32_t BF(float x) { uint32_t u; memcpy(&u, &x, sizeof u); return u; }
static float F(uint32_t u) { float x; memcpy(&x, &u, sizeof x); return x; }

#define QNAN 0x7ff8000000000000u
#define PINF 0x7ff0000000000000u
#define NINF 0xfff0000000000000u
#define NZ 0x8000000000000000u
#define ONE 0x3ff0000000000000u
/* 32-bit x86 returns a double or a float in the x87 register st(0), and loading a signalling
 * NaN there quiets it: on that target no signalling NaN survives a function's return (this file's
 * D and F included), whatever the function does. There the signalling rows use the quiet NaN of
 * the same payload, which every function must keep as well. */
#if defined(__i386__)
#define SNAN 0x7ff8000000000123u
#else
#define SNAN 0x7ff0000000000123u
#endif
#define NQNAN 0xfff8000000000456u
#define QNAN32 0x7fc00000u

static uint64_t g_seed = 0x2545f4914f6cdd1du;
static uint64_t rnd(void) {
    g_seed ^= g_seed << 13; g_seed ^= g_seed >> 7; g_seed ^= g_seed << 17;
    return g_seed;
}

/* Bits of either format, weighted to the edges: a few significant bits (ties, exact results),
 * exponents near the subnormals and the overflow, specials now and then. */
static uint64_t rnd_bits(bool s, int expmode, int *base_exp) {
    const int mant = s ? 23 : 52, emax = s ? 255 : 2047;
    uint64_t frac = rnd() & (((uint64_t)1 << mant) - 1);
    const uint64_t pick = rnd() % 16;
    if (pick < 4) frac &= (((uint64_t)1 << mant) - 1) & ~((((uint64_t)1 << mant) - 1) >> (rnd() % 6));   /* the top 0-5 bits only */
    int be;
    switch (expmode) {
    case 0: be = (int)(rnd() % (unsigned)emax); break;                 /* anywhere */
    case 1: be = *base_exp + (int)(rnd() % 5) - 2; break;              /* near another */
    case 2: be = (int)(rnd() % 4); break;                               /* the subnormals */
    case 3: be = emax - 1 - (int)(rnd() % 3); break;                    /* the largest */
    default: be = (emax / 2) - 30 + (int)(rnd() % 60); break;           /* around 1 */
    }
    if (be < 0) be = 0;
    if (be >= emax) be = emax - 1;
    if (rnd() % 64 == 0) { be = emax; if (rnd() & 1) frac = 0; }      /* inf or NaN */
    if (rnd() % 64 == 0) { be = 0; frac = 0; }                          /* zero */
    *base_exp = be;
    const uint64_t sign = (rnd() & 1) ? ((uint64_t)1 << (s ? 31 : 63)) : 0;
    return sign | ((uint64_t)be << mant) | frac;
}

/* The NaN rule: the first NaN operand quieted, else the made NaN. */
static bool isnan_b(uint64_t u, bool s) { return s ? (u & 0x7fffffffu) > 0x7f800000u : (u & ~NZ) > PINF; }
static uint64_t nan_of(uint64_t a, uint64_t b, uint64_t c, int n, bool s) {
    const uint64_t q = s ? 0x00400000u : 0x0008000000000000u;
    if (isnan_b(a, s)) return a | q;
    if (n > 1 && isnan_b(b, s)) return b | q;
    if (n > 2 && isnan_b(c, s)) return c | q;
    return s ? QNAN32 : QNAN;
}

#if defined(__x86_64__) && defined(__GNUC__)
#define HAVE_FMA_REF 1
__attribute__((target("fma"))) static double ref_fma(double x, double y, double z) { return __builtin_fma(x, y, z); }
__attribute__((target("fma"))) static float ref_fmaf(float x, float y, float z) { return __builtin_fmaf(x, y, z); }
#else
#define HAVE_FMA_REF 0
#endif

/* Where the operators evaluate binary64 at more than its precision (x87, FLT_EVAL_METHOD 2), a
 * binary64 x op y is rounded twice, to 64 bits and then to 53, and is no reference: there the
 * binary64 comparisons with the operators are not made (the same integers are compared on every
 * other target). binary32's still are: 64 bits are more than 2 x 24 + 2, so the second rounding
 * gives the first's answer. */
#if defined(__FLT_EVAL_METHOD__) && __FLT_EVAL_METHOD__ != 0
#define REF64 0
#else
#define REF64 1
#endif

static uint64_t ref2(int op, uint64_t a, uint64_t b, bool s) {
    if (s) {
        const float x = F((uint32_t)a), y = F((uint32_t)b);
        float r = 0;
        switch (op) {
        case PROVEN_FLOAT_OP_ADD_: r = x + y; break;
        case PROVEN_FLOAT_OP_SUB_: r = x - y; break;
        case PROVEN_FLOAT_OP_MUL_: r = x * y; break;
        default: r = x / y; break;
        }
        const uint64_t u = BF(r);
        return isnan_b(u, true) ? nan_of(a, b, 0, 2, true) : u;
    }
    const double x = D(a), y = D(b);
    double r = 0;
    switch (op) {
    case PROVEN_FLOAT_OP_ADD_: r = x + y; break;
    case PROVEN_FLOAT_OP_SUB_: r = x - y; break;
    case PROVEN_FLOAT_OP_MUL_: r = x * y; break;
    default: r = x / y; break;
    }
    const uint64_t u = B(r);
    return isnan_b(u, false) ? nan_of(a, b, 0, 2, false) : u;
}

static uint64_t pub2(int op, uint64_t a, uint64_t b, bool s) {
    if (s) {
        const float x = F((uint32_t)a), y = F((uint32_t)b);
        switch (op) {
        case PROVEN_FLOAT_OP_ADD_: return BF(proven_float32_add(x, y));
        case PROVEN_FLOAT_OP_SUB_: return BF(proven_float32_sub(x, y));
        case PROVEN_FLOAT_OP_MUL_: return BF(proven_float32_mul(x, y));
        default: return BF(proven_float32_div(x, y));
        }
    }
    const double x = D(a), y = D(b);
    switch (op) {
    case PROVEN_FLOAT_OP_ADD_: return B(proven_float_add(x, y));
    case PROVEN_FLOAT_OP_SUB_: return B(proven_float_sub(x, y));
    case PROVEN_FLOAT_OP_MUL_: return B(proven_float_mul(x, y));
    default: return B(proven_float_div(x, y));
    }
}

/* Dekker's exact product error, without fma: valid when nothing overflows or underflows. */
static double two_prod_err(double a, double b, double p) {
    const double c = 134217729.0;                          /* 2^27 + 1 */
    double t = c * a; const double ah = t - (t - a), al = a - ah;
    t = c * b; const double bh = t - (t - b), bl = b - bh;
    return ((ah * bh - p) + ah * bl + al * bh) + al * bl;
}

static const char *OPNAME[] = { "add", "sub", "mul", "div" };

int main(void) {
    PROVEN_TEST_SUITE("float_arith: binary64 and binary32 arithmetic with the same bits everywhere",
        "RFC-0055's functions of float_ops.h: the specials and the NaN rule by tables, then the integer implementation against the instruction bit for bit.",
        "Inspect math/src/float_ops.c: f_pack for any rounding, then the operation the failing check names.");

    PROVEN_TEST_SECTION("which path", "On x86-64 the arithmetic takes SSE unless PROVEN_FLOAT_SOFT; fma the instruction only where the build has FMA.", "Check the F_ASM_X86 / F_HW_* conditions in math/src/float_ops.c.");
    {
#if defined(__x86_64__)
#if defined(__FAST_MATH__)
        const bool hw = false;                 /* fast-math: the integers, for its flush-to-zero */
#else
        const bool hw = !PROVEN_FLOAT_SOFT;
#endif
#if defined(__FP_FAST_FMA) && !defined(__FAST_MATH__)
        const bool hwfma = !PROVEN_FLOAT_SOFT;
#else
        const bool hwfma = false;
#endif
        PROVEN_TEST_ASSERT(proven_float_op_is_hardware_(PROVEN_FLOAT_OP_ADD_, false) == hw && proven_float_op_is_hardware_(PROVEN_FLOAT_OP_DIV_, true) == hw
            && proven_float_op_is_hardware_(PROVEN_FLOAT_OP_SQRT_, true) == hw && proven_float_sqrt_is_hardware_() == hw,
            hw ? "add .. div and sqrt take the instruction, both formats" : "PROVEN_FLOAT_SOFT: the integers everywhere", "");
        PROVEN_TEST_ASSERT(proven_float_op_is_hardware_(PROVEN_FLOAT_OP_FMA_, false) == hwfma && proven_float_op_is_hardware_(PROVEN_FLOAT_OP_FMA_, true) == hwfma,
            hwfma ? "fma takes the instruction (the build has FMA)" : "fma takes the integers (no FMA in this build)", "");
#else
        PROVEN_TEST_ASSERT(true, "not x86-64: the path is reported, not checked", "");
#endif
    }

    PROVEN_TEST_SECTION("specials", "Signed zeros, infinities, invalid operations, and NaN operands by the rule: the first NaN in argument order, quieted.", "Check the class branches of f_soft_add / mul / div / fma.");
    {
        PROVEN_TEST_ASSERT(B(proven_float_add(D(PINF), D(NINF))) == QNAN && B(proven_float_sub(D(PINF), D(PINF))) == QNAN && B(proven_float_mul(D(0), D(NINF))) == QNAN
            && B(proven_float_div(D(0), D(NZ))) == QNAN && B(proven_float_div(D(NINF), D(PINF))) == QNAN && B(proven_float_fma(D(PINF), D(0), D(ONE))) == QNAN
            && B(proven_float_fma(D(ONE), D(PINF), D(NINF))) == QNAN, "invalid operations make 0x7FF8000000000000, positive", "");
        PROVEN_TEST_ASSERT(B(proven_float_add(D(SNAN), D(NQNAN))) == (SNAN | 0x0008000000000000u) && B(proven_float_mul(D(ONE), D(NQNAN))) == NQNAN
            && B(proven_float_sub(D(NQNAN), D(SNAN))) == NQNAN && B(proven_float_sub(D(ONE), D(SNAN))) == (SNAN | 0x0008000000000000u)
            && B(proven_float_fma(D(PINF), D(0), D(NQNAN))) == NQNAN && B(proven_float_fma(D(ONE), D(SNAN), D(NQNAN))) == (SNAN | 0x0008000000000000u),
            "a NaN operand: the first, quieted, its sign and payload kept (sub does not flip it)", "");
        PROVEN_TEST_ASSERT(B(proven_float_add(D(NZ), D(NZ))) == NZ && B(proven_float_add(D(NZ), D(0))) == 0 && B(proven_float_sub(D(ONE), D(ONE))) == 0
            && B(proven_float_mul(D(NZ), D(ONE))) == NZ && B(proven_float_div(D(ONE), D(NZ))) == NINF && B(proven_float_div(D(NZ), D(PINF))) == NZ
            && B(proven_float_fma(D(NZ), D(ONE), D(NZ))) == NZ && B(proven_float_fma(D(NZ), D(ONE), D(0))) == 0,
            "the zeros' signs: -0 + -0 is -0, x - x is +0, 1 / -0 is -inf", "");
        PROVEN_TEST_ASSERT(B(proven_float_mul(D(0x7fefffffffffffffu), D(0x4000000000000000u))) == PINF && B(proven_float_mul(D(1), D(0x3fe0000000000000u))) == 0
            && B(proven_float_mul(D(3), D(0x3fe0000000000000u))) == 2 && B(proven_float_add(D(0x0010000000000000u), D(0x8000000000000001u))) == 0x000fffffffffffffu,
            "overflow to inf; the smallest subnormal halved is 0 and three halves 2 (ties to even); a subnormal difference", "");
        PROVEN_TEST_ASSERT(BF(proven_float32_add(F(0x7f800000u), F(0xff800000u))) == QNAN32 && BF(proven_float32_mul(F(0x7fa00001u), F(0x3f800000u))) == 0x7fe00001u
            && BF(proven_float32_div(F(0x3f800000u), F(0x80000000u))) == 0xff800000u && BF(proven_float32_fma(F(0), F(0x7f800000u), F(0x3f800000u))) == QNAN32,
            "binary32: the same rules, 0x7FC00000 for a NaN made", "");
    }

    PROVEN_TEST_SECTION("against the instruction", "add, sub, mul, div of both formats: the integers and the public function against SSE on inputs weighted to the edges.", "Check f_pack (the rounding, the subnormals, the carry into the exponent) and the operation the failing line names.");
    for (int s = 0; s <= 1; s++) {
        if (!s && !REF64) {
            PROVEN_TEST_ASSERT(true, "binary64: the operators round twice here (x87), no reference to compare with", "");
            continue;
        }
        for (int op = PROVEN_FLOAT_OP_ADD_; op <= PROVEN_FLOAT_OP_DIV_; op++) {
            int bad_soft = 0, bad_pub = 0;
            for (int i = 0; i < 400000; i++) {
                int be = 0;
                const uint64_t a = rnd_bits(s, (int)(rnd() % 5), &be);
                int mode = (int)(rnd() % 5);
                if (mode == 1 && op == PROVEN_FLOAT_OP_MUL_) mode = 2;   /* products into the subnormals */
                const uint64_t b = rnd_bits(s, mode, &be);
                const uint64_t want = ref2(op, a, b, s);
                if (proven_float_soft_(op, s, a, b, 0) != want) bad_soft++;
                if (pub2(op, a, b, s) != want) bad_pub++;
            }
            char msg[128];
            snprintf(msg, sizeof msg, "%s %s: 400,000 pairs, the integers as the instruction", s ? "binary32" : "binary64", OPNAME[op]);
            PROVEN_TEST_ASSERT(bad_soft == 0, msg, "");
            snprintf(msg, sizeof msg, "%s %s: the public function as the instruction", s ? "binary32" : "binary64", OPNAME[op]);
            PROVEN_TEST_ASSERT(bad_pub == 0, msg, "");
        }
    }

    PROVEN_TEST_SECTION("exact quotients and ties", "Products and quotients built to be exact or halfway, in the normals and the subnormals.", "Check f_soft_div's remainder and f_pack's tie.");
    {
        int bad = 0;
        for (int i = 0; i < 200000; i++) {
            /* q * b / b and (2k + 1) * 2^-n: exact or a tie at every scale. */
            const uint64_t q = (rnd() >> 40) | 1, k = (rnd() >> 50) | 1;
            const double bq = (double)q, bb = (double)k;
            const int sh = (int)(rnd() % 1150);
            const double prod = proven_float_ldexp(bq * bb, -sh);
            const uint64_t a = B(prod), b = B(proven_float_ldexp(bb, (int)(rnd() % 40) - 20));
            if (REF64 && proven_float_soft_(PROVEN_FLOAT_OP_DIV_, false, a, b, 0) != ref2(PROVEN_FLOAT_OP_DIV_, a, b, false)) bad++;
            if (REF64 && proven_float_soft_(PROVEN_FLOAT_OP_MUL_, false, a, b, 0) != ref2(PROVEN_FLOAT_OP_MUL_, a, b, false)) bad++;
            const uint64_t a32 = BF((float)proven_float_ldexp((double)(q & 0xffffff), -(int)(rnd() % 170))), b32 = BF((float)(k & 0xfff));
            if (proven_float_soft_(PROVEN_FLOAT_OP_DIV_, true, a32, b32, 0) != ref2(PROVEN_FLOAT_OP_DIV_, a32, b32, true)) bad++;
            if (proven_float_soft_(PROVEN_FLOAT_OP_MUL_, true, a32, b32, 0) != ref2(PROVEN_FLOAT_OP_MUL_, a32, b32, true)) bad++;
        }
        PROVEN_TEST_ASSERT(bad == 0, REF64 ? "200,000 built cases of each format: exact and halfway results as the instruction"
                                  : "200,000 built binary32 cases: exact and halfway results as the instruction (binary64: x87, no reference)", "");
    }

    PROVEN_TEST_SECTION("fma", "One rounding: against the FMA instruction where the CPU has one, and against Dekker's exact product error everywhere.", "Check f_soft_fma: the 128-bit alignment, the cancellation, and the specials.");
    {
        int bad_err = 0, bad_ref = 0, bad_ref32 = 0, n_err = 0;
#if HAVE_FMA_REF
        const bool have = __builtin_cpu_supports("fma");
#else
        const bool have = false;
#endif
        for (int i = 0; i < 400000; i++) {
            int be = 0;
            const uint64_t a = rnd_bits(false, (int)(rnd() % 5), &be);
            const uint64_t b = rnd_bits(false, (int)(rnd() % 5), &be);
            int be2 = be;
            const uint64_t c = rnd_bits(false, (int)(rnd() % 5), &be2);
            const uint64_t got = proven_float_soft_(PROVEN_FLOAT_OP_FMA_, false, a, b, c);
            if (B(proven_float_fma(D(a), D(b), D(c))) != got) bad_ref++;
#if HAVE_FMA_REF
            if (have) {
                uint64_t w = B(ref_fma(D(a), D(b), D(c)));
                if (isnan_b(w, false)) w = nan_of(a, b, c, 3, false);
                if (w != got) bad_ref++;
                const uint32_t a32 = (uint32_t)rnd_bits(true, (int)(rnd() % 5), &be), b32 = (uint32_t)rnd_bits(true, (int)(rnd() % 5), &be);
                be2 = be;
                const uint32_t c32 = (uint32_t)rnd_bits(true, (int)(rnd() % 5), &be2);
                uint64_t w32 = BF(ref_fmaf(F(a32), F(b32), F(c32)));
                if (isnan_b(w32, true)) w32 = nan_of(a32, b32, c32, 3, true);
                if (w32 != proven_float_soft_(PROVEN_FLOAT_OP_FMA_, true, a32, b32, c32) || w32 != BF(proven_float32_fma(F(a32), F(b32), F(c32)))) bad_ref32++;
            }
#endif
            /* fma(x, y, -x*y) is the product's exact error. */
            const double x = D((rnd() & 0x800fffffffffffffu) | ((uint64_t)(900 + rnd() % 240) << 52));
            const double y = D((rnd() & 0x800fffffffffffffu) | ((uint64_t)(900 + rnd() % 240) << 52));
            const double p = x * y;
            const double e1 = D(proven_float_soft_(PROVEN_FLOAT_OP_FMA_, false, B(x), B(y), B(p) ^ NZ));
            const double e2 = two_prod_err(x, y, p);
            n_err++;
            if (REF64 && B(e1) != B(e2) && !(e1 == 0 && e2 == 0)) bad_err++;
        }
#if HAVE_FMA_REF
        /* z tied to the product: -(x*y) scaled by 2^k and nudged by a few ulps, so that the sum
         * cancels deeply, or z sits just past the product's last bit. */
        int bad_near = 0;
        if (have) {
            for (int i = 0; i < 400000; i++) {
                int be = 0;
                for (int s = 0; s <= 1; s++) {
                    const uint64_t a = rnd_bits(s, (int)(rnd() % 5), &be), b = rnd_bits(s, (int)(rnd() % 5), &be);
                    const int k = (int)(rnd() % 161) - 80;
                    const int64_t nudge = (int64_t)(rnd() % 9) - 4;
                    uint64_t c, w, got;
                    if (s) {
                        const float p = proven_float32_ldexp(F((uint32_t)a) * F((uint32_t)b), k);
                        c = (uint64_t)(uint32_t)(BF(p) + (uint32_t)nudge) ^ 0x80000000u;
                        w = BF(ref_fmaf(F((uint32_t)a), F((uint32_t)b), F((uint32_t)c)));
                        if (isnan_b(w, true)) w = nan_of(a, b, c, 3, true);
                    } else {
                        const double p = proven_float_ldexp(D(a) * D(b), k);
                        c = (B(p) + (uint64_t)nudge) ^ NZ;
                        w = B(ref_fma(D(a), D(b), D(c)));
                        if (isnan_b(w, false)) w = nan_of(a, b, c, 3, false);
                    }
                    got = proven_float_soft_(PROVEN_FLOAT_OP_FMA_, s, a, b, c);
                    if (got != w) bad_near++;
                }
            }
        }
        PROVEN_TEST_ASSERT(bad_near == 0, have ? "400,000 triples of each format with z near -(x*y) * 2^k, |k| <= 80: as the FMA instruction"
                                                : "z near the product: no FMA on this CPU to compare with", "");
#endif
        PROVEN_TEST_ASSERT(bad_err == 0, REF64 ? "400,000 products: fma(x, y, -x*y) is Dekker's exact error" : "Dekker's product error needs binary64 operators: not on x87", "");
        PROVEN_TEST_ASSERT(bad_ref == 0, have ? "400,000 triples: the integers as the FMA instruction, and the public function as the integers"
                                               : "400,000 triples: the public function as the integers (no FMA on this CPU)", "");
        PROVEN_TEST_ASSERT(bad_ref32 == 0, have ? "binary32: 400,000 triples as the FMA instruction" : "binary32: no FMA on this CPU to compare with", "");
        (void)n_err;
    }

    PROVEN_TEST_SECTION("binary32 square root", "Every 997th binary32 value: the integer root against the instruction.", "Check f_soft_sqrt32.");
#if defined(__x86_64__) && defined(__GNUC__)
    {
        int bad = 0;
        for (uint64_t u = 0; u <= 0xffffffffu; u += 997) {
            float r = 0;
            const float x = F((uint32_t)u);
            __asm__("sqrtss %1, %0" : "=x"(r) : "x"(x));
            uint64_t w = BF(r);
            if (isnan_b(w, true)) w = isnan_b(u, true) ? (u | 0x00400000u) : QNAN32;
            if (proven_float_soft_(PROVEN_FLOAT_OP_SQRT_, true, u, 0, 0) != w || BF(proven_float32_sqrt(x)) != w) bad++;
        }
        PROVEN_TEST_ASSERT(bad == 0, "4.3 million roots agree bit for bit", "");
    }
#endif

    PROVEN_TEST_SECTION("comparison", "eq, lt, le, unordered and compare against the compiler's comparisons.", "Check f_cmp's key and its zero case.");
    {
        int bad = 0;
        for (int i = 0; i < 300000; i++) {
            int be = 0;
            const uint64_t a = rnd_bits(false, (int)(rnd() % 5), &be);
            const uint64_t b = (rnd() % 8 == 0) ? a ^ (rnd() % 2 ? NZ : 0) : rnd_bits(false, 1, &be);
            const double x = D(a), y = D(b);
            const bool un = x != x || y != y;
            const int want = un ? 2 : x < y ? -1 : x > y ? 1 : 0;
            if (proven_float_compare(x, y) != want || proven_float_eq(x, y) != (x == y) || proven_float_lt(x, y) != (x < y)
                || proven_float_le(x, y) != (x <= y) || proven_float_unordered(x, y) != un) bad++;
            const float fx = F((uint32_t)rnd_bits(true, (int)(rnd() % 5), &be)), fy = F((uint32_t)rnd_bits(true, 1, &be));
            const bool un32 = fx != fx || fy != fy;
            if (proven_float32_compare(fx, fy) != (un32 ? 2 : fx < fy ? -1 : fx > fy ? 1 : 0) || proven_float32_eq(fx, fy) != (fx == fy)
                || proven_float32_lt(fx, fy) != (fx < fy) || proven_float32_le(fx, fy) != (fx <= fy) || proven_float32_unordered(fx, fy) != un32) bad++;
        }
        PROVEN_TEST_ASSERT(bad == 0, "300,000 pairs of each format, zeros and NaNs among them", "");
        PROVEN_TEST_ASSERT(proven_float_eq(D(NZ), D(0)) && !proven_float_eq(D(QNAN), D(QNAN)) && proven_float_compare(D(QNAN), D(ONE)) == 2
            && proven_float_le(D(NINF), D(NZ)) && !proven_float_lt(D(NZ), D(0)), "-0 equals +0; a NaN equals nothing, itself included", "");
    }

    PROVEN_TEST_SECTION("conversions", "Between the formats and from and to the integers, against the compiler's conversions; NaN payloads; the ranges.", "Check f_narrow, f_widen, f_from_int and f_to_range.");
    {
        int bad = 0;
        for (int i = 0; i < 300000; i++) {
            int be = 0;
            const uint64_t a = rnd_bits(false, (int)(rnd() % 5), &be);
            const double x = D(a);
            uint64_t w = BF((float)x);
            if (isnan_b(a, false)) w = ((a >> 32) & 0x80000000u) | 0x7fc00000u | ((a & 0x000fffffffffffffu) >> 29);
            if (BF(proven_float_to_float32(x)) != w) bad++;
            /* Near binary32's range, where narrowing rounds into its subnormals or overflows. */
            const double xn = D((a & 0x800fffffffffffffu) | ((uint64_t)(1023 - 160 + rnd() % 300) << 52));
            if (BF(proven_float_to_float32(xn)) != BF((float)xn)) bad++;
            const uint32_t f = (uint32_t)rnd_bits(true, (int)(rnd() % 5), &be);
            uint64_t wd = B((double)F(f));
            if (isnan_b(f, true)) wd = ((uint64_t)(f & 0x80000000u) << 32) | QNAN | ((uint64_t)(f & 0x7fffffu) << 29);
            if (B(proven_float_from_float32(F(f))) != wd) bad++;
            const uint64_t r = rnd() >> (rnd() % 64);
            if (B(proven_float_from_u64(r)) != B((double)r) || BF(proven_float32_from_u64(r)) != BF((float)r)
                || BF(proven_float32_from_i64((int64_t)r)) != BF((float)(int64_t)r) || B(proven_float_from_i32((int32_t)r)) != B((double)(int32_t)r)
                || B(proven_float_from_u32((uint32_t)r)) != B((double)(uint32_t)r) || BF(proven_float32_from_i32((int32_t)r)) != BF((float)(int32_t)r)
                || BF(proven_float32_from_u32((uint32_t)r)) != BF((float)(uint32_t)r)) bad++;
            const double t = D((a & 0x800fffffffffffffu) | ((uint64_t)(1023 + rnd() % 66) << 52));
            proven_i32 i32; proven_u32 u32; proven_u64 u64; proven_i64 i64;
            const bool i32ok = t > -2147483649.0 && t < 2147483648.0, u32ok = t > -1.0 && t < 4294967296.0, u64ok = t > -1.0 && t < 18446744073709551616.0;
            if ((proven_float_to_i32(t, &i32) == PROVEN_OK) != i32ok || (i32ok && i32 != (int32_t)t)) bad++;
            if ((proven_float_to_u32(t, &u32) == PROVEN_OK) != u32ok || (u32ok && u32 != (uint32_t)t)) bad++;
            if ((proven_float_to_u64(t, &u64) == PROVEN_OK) != u64ok || (u64ok && u64 != (uint64_t)t)) bad++;
            const float tf = (float)t;
            const bool i64ok = tf >= -9223372036854775808.0f && tf < 9223372036854775808.0f;
            if ((proven_float32_to_i64(tf, &i64) == PROVEN_OK) != i64ok || (i64ok && i64 != (int64_t)tf)) bad++;
            const bool fu64ok = tf > -1.0f && tf < 18446744073709551616.0f, fi32ok = tf >= -2147483648.0f && tf < 2147483648.0f;
            if ((proven_float32_to_u64(tf, &u64) == PROVEN_OK) != fu64ok || (fu64ok && u64 != (uint64_t)tf)) bad++;
            if ((proven_float32_to_i32(tf, &i32) == PROVEN_OK) != fi32ok || (fi32ok && i32 != (int32_t)tf)) bad++;
            const bool fu32ok = tf > -1.0f && tf < 4294967296.0f;
            if ((proven_float32_to_u32(tf, &u32) == PROVEN_OK) != fu32ok || (fu32ok && u32 != (uint32_t)tf)) bad++;
        }
        PROVEN_TEST_ASSERT(bad == 0, "300,000 values each way: as the compiler converts, the ranges exact", "");
        proven_u32 u = 7;
        PROVEN_TEST_ASSERT(proven_float_to_u32(D(0xbfe0000000000000u), &u) == PROVEN_OK && u == 0 && proven_float_to_u32(D(0xbff0000000000000u), &u) == PROVEN_ERR_OUT_OF_BOUNDS
            && u == 0 && proven_float_to_u32(D(QNAN), (void *)0) == PROVEN_ERR_INVALID_ARG && BF(proven_float_to_float32(D(SNAN))) == 0x7fc00000u
            && BF(proven_float_to_float32(D(0x7ff0000020000000u))) == 0x7fc00001u, "-0.5 is 0 and -1 is refused for unsigned; a NaN narrowed keeps the top of its payload", "");
    }

    PROVEN_TEST_SECTION("binary32 of RFC-0054's functions", "Integral values, fmod, frexp, ldexp, fmin and fmax of binary32, against binary64's on the widened value narrowed by the compiler.", "Check F32_VIA64 and f_narrow in math/src/float_ops.c.");
    {
        int bad = 0;
        for (int i = 0; i < 300000; i++) {
            int be = 0;
            uint32_t a = (uint32_t)rnd_bits(true, (int)(rnd() % 5), &be), b = (uint32_t)rnd_bits(true, 1, &be);
#if defined(__i386__)
            if (isnan_b(a, true)) a |= 0x00400000u;          /* the x87 note at SNAN */
            if (isnan_b(b, true)) b |= 0x00400000u;
#endif
            const float x = F(a), y = F(b);
            const double xd = x, yd = y;
#define CHK1(fn) do { const uint64_t g = BF(proven_float32_##fn(x)); const uint64_t w = BF((float)proven_float_##fn(xd)); \
                      if (g != w && !(isnan_b(a, true) && g == (a | 0x00400000u))) bad++; } while (0)
            CHK1(floor); CHK1(ceil); CHK1(trunc); CHK1(round); CHK1(rint);
            const uint64_t gm = BF(proven_float32_fmod(x, y)), wm = BF((float)proven_float_fmod(xd, yd));
            if (gm != wm && !isnan_b(gm, true)) bad++;
            if (BF(proven_float32_fmin(x, y)) != BF((float)proven_float_fmin(xd, yd)) && !isnan_b(a, true)) bad++;
            if (BF(proven_float32_fmax(x, y)) != BF((float)proven_float_fmax(xd, yd)) && !isnan_b(a, true)) bad++;
            int e1 = 0, e2 = 0;
            if (!isnan_b(a, true) && (BF(proven_float32_frexp(x, &e1)) != BF((float)proven_float_frexp(xd, &e2)) || e1 != e2)) bad++;
            const int n = (int)(rnd() % 400) - 200;
            if (!isnan_b(a, true) && BF(proven_float32_ldexp(x, n)) != BF((float)proven_float_ldexp(xd, n))) bad++;
            if (proven_float32_is_nan(x) != (x != x) || proven_float32_is_inf(x) != (x == F(0x7f800000u) || x == F(0xff800000u))
                || proven_float32_is_finite(x) != (!proven_float32_is_nan(x) && !proven_float32_is_inf(x))) bad++;
            if (BF(proven_float32_fabs(x)) != (a & 0x7fffffffu) || BF(proven_float32_copysign(x, y)) != ((a & 0x7fffffffu) | (b & 0x80000000u))) bad++;
            if (proven_float32_to_bits(x) != a || BF(proven_float32_from_bits(a)) != a) bad++;
        }
        PROVEN_TEST_ASSERT(bad == 0, "300,000 values: each as binary64's function narrowed", "");
        PROVEN_TEST_ASSERT(BF(proven_float32_floor(F(0xbf000000u))) == 0xbf800000u && BF(proven_float32_rint(F(0x40200000u))) == 0x40000000u
            && BF(proven_float32_fmin(F(QNAN32), F(0x3f800000u))) == 0x3f800000u && BF(proven_float32_fmin(F(0x7f800001u), F(QNAN32))) == 0x7fc00001u
            && BF(proven_float32_ldexp(F(0x3f800000u), -149)) == 1u && BF(proven_float32_ldexp(F(0x3fc00000u), -150)) == 1u,
            "floor(-0.5) is -1, rint(2.5) is 2; a number wins over a NaN, two give the first quieted; ldexp into the subnormals", "");
    }

    PROVEN_TEST_PASS("float_arith checks passed.");
    return 0;
}
