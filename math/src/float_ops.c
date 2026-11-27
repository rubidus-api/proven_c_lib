#include "proven/float_ops.h"
#include "proven_internal_float_ops.h"
#include "proven/math_config.h"

/* binary64 on its bits (RFC-0054). Nothing here does floating-point arithmetic except the
 * hardware square root, so no compiler flag and no FPU mode changes an answer. */

#define F_SIGN ((proven_u64)1 << 63)
#define F_EXP ((proven_u64)0x7ff << 52)
#define F_MANT (((proven_u64)1 << 52) - 1)
#define F_HIDDEN ((proven_u64)1 << 52)
#define F_QUIET ((proven_u64)1 << 51)
#define F_NAN ((proven_u64)0x7ff8000000000000u)

/* The bits made opaque to the optimizer: clang otherwise turns a test of them, such as
 * (u & ~sign) == 0, back into a floating-point compare - a call into the soft-float runtime on
 * a target without an FPU. */
#if defined(__GNUC__) && !defined(__wasm__)
#define F_OPAQUE(u) __asm__("" : "+r"(u))
#else
#define F_OPAQUE(u) ((void)0)
#endif

/* By a union, which C defines for this, so that nothing below the base's public headers is
 * needed. */
proven_u64 proven_float_to_bits(double x) {
    union { double d; proven_u64 u; } v = { .d = x };
    proven_u64 u = v.u;
    F_OPAQUE(u);
    return u;
}

double proven_float_from_bits(proven_u64 bits) {
    union { proven_u64 u; double d; } v = { .u = bits };
    return v.d;
}

static bool f_nan(proven_u64 u) { return (u & ~F_SIGN) > F_EXP; }
static double f_quiet(proven_u64 u) { return proven_float_from_bits(u | F_QUIET); }
/* The unbiased exponent of a normal number (a subnormal's is -1023, a zero's too). */
static int f_exp(proven_u64 u) { return (int)((u >> 52) & 0x7ff) - 1023; }

bool proven_float_is_nan(double x) { return f_nan(proven_float_to_bits(x)); }
bool proven_float_is_inf(double x) { return (proven_float_to_bits(x) & ~F_SIGN) == F_EXP; }
bool proven_float_is_finite(double x) { return (proven_float_to_bits(x) & F_EXP) != F_EXP; }

double proven_float_fabs(double x) { return proven_float_from_bits(proven_float_to_bits(x) & ~F_SIGN); }

double proven_float_copysign(double x, double y) {
    return proven_float_from_bits((proven_float_to_bits(x) & ~F_SIGN) | (proven_float_to_bits(y) & F_SIGN));
}

// -----------------------------------------------------------------------------
// Integral values
// -----------------------------------------------------------------------------

double proven_float_trunc(double x) {
    proven_u64 u = proven_float_to_bits(x);
    const int e = f_exp(u);
    if (e >= 52) return f_nan(u) ? f_quiet(u) : x;     /* already an integer, or not a number */
    if (e < 0) return proven_float_from_bits(u & F_SIGN);
    return proven_float_from_bits(u & ~(F_MANT >> e));
}

double proven_float_floor(double x) {
    proven_u64 u = proven_float_to_bits(x);
    const int e = f_exp(u);
    if (e >= 52) return f_nan(u) ? f_quiet(u) : x;
    if (e < 0) {
        if ((u & ~F_SIGN) == 0 || !(u & F_SIGN)) return proven_float_from_bits(u & F_SIGN);
        return proven_float_from_bits(F_SIGN | ((proven_u64)1023 << 52));      /* -1 */
    }
    const proven_u64 mask = F_MANT >> e;
    if ((u & mask) == 0) return x;
    if (u & F_SIGN) u += mask;                          /* away from zero; a carry raises the exponent */
    return proven_float_from_bits(u & ~mask);
}

double proven_float_ceil(double x) {
    proven_u64 u = proven_float_to_bits(x);
    const int e = f_exp(u);
    if (e >= 52) return f_nan(u) ? f_quiet(u) : x;
    if (e < 0) {
        if ((u & ~F_SIGN) == 0 || (u & F_SIGN)) return proven_float_from_bits(u & F_SIGN);
        return proven_float_from_bits((proven_u64)1023 << 52);               /* 1 */
    }
    const proven_u64 mask = F_MANT >> e;
    if ((u & mask) == 0) return x;
    if (!(u & F_SIGN)) u += mask;
    return proven_float_from_bits(u & ~mask);
}

double proven_float_round(double x) {
    proven_u64 u = proven_float_to_bits(x);
    const int e = f_exp(u);
    if (e >= 52) return f_nan(u) ? f_quiet(u) : x;
    if (e < 0) {
        /* |x| in [0.5, 1) goes to 1, below that to 0. */
        if (e == -1) return proven_float_from_bits((u & F_SIGN) | ((proven_u64)1023 << 52));
        return proven_float_from_bits(u & F_SIGN);
    }
    const proven_u64 mask = F_MANT >> e;
    u += (mask + 1) >> 1;                               /* half a unit of the integer's last place */
    return proven_float_from_bits(u & ~mask);
}

double proven_float_rint(double x) {
    proven_u64 u = proven_float_to_bits(x);
    const int e = f_exp(u);
    if (e >= 52) return f_nan(u) ? f_quiet(u) : x;
    const proven_u64 sign = u & F_SIGN;
    if (e < 0) {
        /* Above a half goes to 1; a half itself to the even 0. */
        if (e == -1 && (u & F_MANT) != 0) return proven_float_from_bits(sign | ((proven_u64)1023 << 52));
        return proven_float_from_bits(sign);
    }
    const proven_u64 mask = F_MANT >> e, half = (mask + 1) >> 1, frac = u & mask;
    const proven_u64 base = u & ~mask;
    const bool odd = ((((u & F_MANT) | F_HIDDEN) >> (52 - e)) & 1) != 0;
    if (frac > half || (frac == half && odd)) return proven_float_from_bits(base + mask + 1);
    return proven_float_from_bits(base);
}

// -----------------------------------------------------------------------------
// Comparison
// -----------------------------------------------------------------------------

/* A key whose unsigned order is the numbers' order, -0 below +0. */
static proven_u64 f_key(proven_u64 u) { return (u & F_SIGN) ? ~u : u | F_SIGN; }

double proven_float_fmin(double x, double y) {
    const proven_u64 a = proven_float_to_bits(x), b = proven_float_to_bits(y);
    if (f_nan(a)) return f_nan(b) ? f_quiet(a) : y;
    if (f_nan(b)) return x;
    return f_key(a) <= f_key(b) ? x : y;
}

double proven_float_fmax(double x, double y) {
    const proven_u64 a = proven_float_to_bits(x), b = proven_float_to_bits(y);
    if (f_nan(a)) return f_nan(b) ? f_quiet(a) : y;
    if (f_nan(b)) return x;
    return f_key(a) >= f_key(b) ? x : y;
}

// -----------------------------------------------------------------------------
// fmod
// -----------------------------------------------------------------------------

/* The significand with its hidden bit, and the exponent of its last bit, of a nonzero finite
 * number: x = m * 2^e, m in [2^52, 2^53). */
static proven_u64 f_split(proven_u64 u, int *e) {
    proven_u64 m = u & F_MANT;
    int ex = (int)((u >> 52) & 0x7ff);
    if (ex == 0) {
        ex = 1;
        while (!(m & F_HIDDEN)) { m <<= 1; ex--; }
    } else {
        m |= F_HIDDEN;
    }
    *e = ex - 1075;
    return m;
}

/* m * 2^e with m in [2^52, 2^53) - or below, for a result among the subnormals that needs no
 * rounding (fmod's are exact) - with the sign given. */
static proven_u64 f_join_exact(proven_u64 sign, proven_u64 m, int e) {
    int biased = e + 1075;
    if (biased >= 1) return sign | ((proven_u64)biased << 52) | (m & F_MANT);
    return sign | (m >> (1 - biased));
}

double proven_float_fmod(double x, double y) {
    const proven_u64 a = proven_float_to_bits(x), b = proven_float_to_bits(y);
    if (f_nan(a)) return f_quiet(a);
    if (f_nan(b)) return f_quiet(b);
    if ((a & ~F_SIGN) == F_EXP || (b & ~F_SIGN) == 0) return proven_float_from_bits(F_NAN);
    if ((b & ~F_SIGN) == F_EXP || (a & ~F_SIGN) == 0) return x;
    const proven_u64 sign = a & F_SIGN;
    if ((a & ~F_SIGN) < (b & ~F_SIGN)) return x;        /* |x| < |y| */
    int ex, ey;
    proven_u64 mx = f_split(a, &ex);
    const proven_u64 my = f_split(b, &ey);
    /* Long division of the significands, a bit a step, keeping only the remainder. */
    for (; ex > ey; ex--) {
        if (mx >= my) mx -= my;
        mx <<= 1;
    }
    if (mx >= my) mx -= my;
    if (mx == 0) return proven_float_from_bits(sign);
    while (!(mx & F_HIDDEN)) { mx <<= 1; ex--; }
    return proven_float_from_bits(f_join_exact(sign, mx, ex));
}

// -----------------------------------------------------------------------------
// Exponents
// -----------------------------------------------------------------------------

double proven_float_frexp(double x, int *exp) {
    const proven_u64 u = proven_float_to_bits(x);
    int dummy;
    if (!exp) exp = &dummy;
    if ((u & ~F_SIGN) == 0 || (u & F_EXP) == F_EXP) {
        *exp = 0;
        return f_nan(u) ? f_quiet(u) : x;
    }
    int e;
    const proven_u64 m = f_split(u, &e);
    *exp = e + 53;
    return proven_float_from_bits((u & F_SIGN) | ((proven_u64)1022 << 52) | (m & F_MANT));
}

/* m * 2^e (m in [2^52, 2^53)) rounded once to nearest even, with the sign given. */
static proven_u64 f_join_round(proven_u64 sign, proven_u64 m, proven_i64 e) {
    const proven_i64 biased = e + 1075;
    if (biased >= 2047) return sign | F_EXP;
    if (biased >= 1) return sign | ((proven_u64)biased << 52) | (m & F_MANT);
    const proven_i64 shift = 1 - biased;
    if (shift > 54) return sign;                         /* below half the smallest subnormal */
    proven_u64 q = m >> shift;
    const proven_u64 rem = m & (((proven_u64)1 << shift) - 1), half = (proven_u64)1 << (shift - 1);
    if (rem > half || (rem == half && (q & 1))) q++;     /* a carry into bit 52 is the smallest normal */
    return sign | q;
}

double proven_float_ldexp(double x, int exp) {
    const proven_u64 u = proven_float_to_bits(x);
    if (f_nan(u)) return f_quiet(u);
    if ((u & ~F_SIGN) == 0 || (u & ~F_SIGN) == F_EXP) return x;
    int e;
    const proven_u64 m = f_split(u, &e);
    proven_i64 n = exp;
    if (n > 4000) n = 4000;
    if (n < -4000) n = -4000;
    return proven_float_from_bits(f_join_round(u & F_SIGN, m, (proven_i64)e + n));
}

// -----------------------------------------------------------------------------
// int64
// -----------------------------------------------------------------------------

double proven_float_from_i64(proven_i64 v) {
    if (v == 0) return 0.0;
    const proven_u64 sign = v < 0 ? F_SIGN : 0;
    proven_u64 a = v < 0 ? (proven_u64)0 - (proven_u64)v : (proven_u64)v;
    int p = 63;
    while (!(a >> p)) p--;
    if (p <= 52) return proven_float_from_bits(sign | ((proven_u64)(p + 1023) << 52) | ((a << (52 - p)) & F_MANT));
    const int shift = p - 52;
    proven_u64 q = a >> shift;
    const proven_u64 rem = a & (((proven_u64)1 << shift) - 1), half = (proven_u64)1 << (shift - 1);
    if (rem > half || (rem == half && (q & 1))) q++;
    if (q >> 53) { q >>= 1; p++; }
    return proven_float_from_bits(sign | ((proven_u64)(p + 1023) << 52) | (q & F_MANT));
}

proven_err_t proven_float_to_i64(double x, proven_i64 *out) {
    if (!out) return PROVEN_ERR_INVALID_ARG;
    const proven_u64 u = proven_float_to_bits(x);
    if (f_nan(u)) return PROVEN_ERR_OUT_OF_BOUNDS;
    const int e = f_exp(u);
    if (e < 0) { *out = 0; return PROVEN_OK; }
    if (e >= 63) {
        if (u == (F_SIGN | ((proven_u64)(63 + 1023) << 52))) { *out = -INT64_MAX - 1; return PROVEN_OK; }  /* -2^63 exactly */
        return PROVEN_ERR_OUT_OF_BOUNDS;
    }
    const proven_u64 m = (u & F_MANT) | F_HIDDEN;
    const proven_u64 mag = e >= 52 ? m << (e - 52) : m >> (52 - e);
    *out = (u & F_SIGN) ? (proven_i64)((proven_u64)0 - mag) : (proven_i64)mag;
    return PROVEN_OK;
}

// -----------------------------------------------------------------------------
// Square root
// -----------------------------------------------------------------------------

/* 32 x 32 -> 64: one instruction on a 32-bit target with a multiplier (no runtime helper). */
static proven_u64 f_mul32(proven_u32 a, proven_u32 b) { return (proven_u64)a * b; }

/* 64 x 64 -> 128 by 32-bit halves: every target has these. */
static void f_mul(proven_u64 a, proven_u64 b, proven_u64 *hi, proven_u64 *lo) {
    const proven_u32 a0 = (proven_u32)a, a1 = (proven_u32)(a >> 32), b0 = (proven_u32)b, b1 = (proven_u32)(b >> 32);
    const proven_u64 p00 = f_mul32(a0, b0), p01 = f_mul32(a0, b1), p10 = f_mul32(a1, b0), p11 = f_mul32(a1, b1);
    const proven_u64 mid = (p00 >> 32) + (p01 & 0xffffffffu) + (p10 & 0xffffffffu);
    *lo = (p00 & 0xffffffffu) | (mid << 32);
    *hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
}

/* The root of a positive finite number by integers: r = floor(sqrt(m * 2^52)) bit by bit, then
 * up by one when the remainder passes r (a root is never exactly halfway). */
double proven_float_sqrt_soft_(double x) {
    const proven_u64 u = proven_float_to_bits(x);
    if (f_nan(u)) return f_quiet(u);
    if ((u & ~F_SIGN) == 0) return x;
    if (u & F_SIGN) return proven_float_from_bits(F_NAN);
    if (u == F_EXP) return x;
    int e;
    proven_u64 m = f_split(u, &e);
    if (e & 1) { m <<= 1; e--; }                         /* an even exponent: m in [2^52, 2^54) */
    const proven_u64 shi = m >> 12, slo = m << 52;       /* S = m * 2^52 */
    proven_u64 r = 0;
    for (int bit = 52; bit >= 0; bit--) {
        const proven_u64 t = r | ((proven_u64)1 << bit);
        proven_u64 hi, lo;
        f_mul(t, t, &hi, &lo);
        if (hi < shi || (hi == shi && lo <= slo)) r = t;
    }
    proven_u64 hi, lo;
    f_mul(r, r, &hi, &lo);
    const proven_u64 rem_lo = slo - lo, rem_hi = shi - hi - (slo < lo);
    if (rem_hi != 0 || rem_lo > r) r++;
    int re = (e - 52) / 2;
    if (r >> 53) { r >>= 1; re++; }
    return proven_float_from_bits(((proven_u64)(re + 1075) << 52) | (r & F_MANT));     /* r * 2^re, r in [2^52, 2^53) */
}

// -----------------------------------------------------------------------------
// The instruction or the integers (RFC-0055 D-4)
// -----------------------------------------------------------------------------

/* Each instruction below is IEEE 754 by construction and does one operation, so no
 * contraction can fuse it. x86 and AArch64 take inline assembly, which no compiler flag
 * reaches; the other targets the plain operator, only where the build evaluates at the type's
 * own precision and has no fast-math. A NaN result is never taken from the instruction (they
 * disagree on its bits): it is made again from the operands, by the integers' rule. */

/* Not under fast-math even for the assembly: the flag that the compiler cannot apply to it
 * also links in startup code that turns on flush-to-zero (crtfastmath on x86). */
#if !PROVEN_FLOAT_SOFT && !defined(__FAST_MATH__) && defined(__GNUC__) && (defined(__x86_64__) || (defined(__i386__) && defined(__SSE2__)))
#define F_ASM_X86 1
#elif !PROVEN_FLOAT_SOFT && !defined(__FAST_MATH__) && defined(__GNUC__) && defined(__aarch64__)
#define F_ASM_A64 1
#endif

#if !PROVEN_FLOAT_SOFT && !defined(F_ASM_X86) && !defined(F_ASM_A64) && !defined(__FAST_MATH__) \
    && !(defined(__FINITE_MATH_ONLY__) && __FINITE_MATH_ONLY__) && defined(__FLT_EVAL_METHOD__) && __FLT_EVAL_METHOD__ == 0
#if defined(__wasm__) || (defined(__ARM_FP) && (__ARM_FP & 8)) || (defined(__riscv_flen) && __riscv_flen >= 64)
#define F_OP64 1
#endif
#if defined(__wasm__) || (defined(__ARM_FP) && (__ARM_FP & 4)) || (defined(__riscv_flen) && __riscv_flen >= 32)
#define F_OP32 1
#endif
#endif

#if defined(F_ASM_X86) || defined(F_ASM_A64) || defined(F_OP64)
#define F_HW64 1
#else
#define F_HW64 0
#endif
#if defined(F_ASM_X86) || defined(F_ASM_A64) || defined(F_OP32)
#define F_HW32 1
#else
#define F_HW32 0
#endif

/* fma: only where the target says it is fused and the compiler inlines the builtin. */
#if !PROVEN_FLOAT_SOFT && defined(__GNUC__) && !defined(__FAST_MATH__) && defined(__FP_FAST_FMA) \
    && (defined(F_ASM_X86) || defined(F_ASM_A64) || defined(F_OP64))
#define F_HW_FMA64 1
#else
#define F_HW_FMA64 0
#endif
#if !PROVEN_FLOAT_SOFT && defined(__GNUC__) && !defined(__FAST_MATH__) && defined(__FP_FAST_FMAF) \
    && (defined(F_ASM_X86) || defined(F_ASM_A64) || defined(F_OP32))
#define F_HW_FMA32 1
#else
#define F_HW_FMA32 0
#endif

/* The square root: the instruction on x86 (SSE2), AArch64 and wasm, as before RFC-0055, but
 * not under fast-math. */
#if defined(F_ASM_X86)
#define F_HW_SQRT 1
static double f_hw_sqrt(double x) { double r; __asm__("sqrtsd %1, %0" : "=x"(r) : "x"(x)); return r; }
static float f_hw_sqrt32(float x) { float r; __asm__("sqrtss %1, %0" : "=x"(r) : "x"(x)); return r; }
#elif defined(F_ASM_A64)
#define F_HW_SQRT 1
static double f_hw_sqrt(double x) { double r; __asm__("fsqrt %d0, %d1" : "=w"(r) : "w"(x)); return r; }
static float f_hw_sqrt32(float x) { float r; __asm__("fsqrt %s0, %s1" : "=w"(r) : "w"(x)); return r; }
#elif !PROVEN_FLOAT_SOFT && !defined(__FAST_MATH__) && defined(__wasm__)
#define F_HW_SQRT 1
static double f_hw_sqrt(double x) { return __builtin_sqrt(x); }      /* f64.sqrt: WebAssembly has no errno */
static float f_hw_sqrt32(float x) { return __builtin_sqrtf(x); }
#else
#define F_HW_SQRT 0
#endif

double proven_float_sqrt(double x) {
#if F_HW_SQRT
    const proven_u64 u = proven_float_to_bits(x);
    /* What the instruction would answer in its own way - its default NaN is negative on x86 - is
     * answered here; it takes only positive finite numbers. */
    if (f_nan(u) || (u & ~F_SIGN) == 0 || (u & F_SIGN) || u == F_EXP) return proven_float_sqrt_soft_(x);
    return f_hw_sqrt(x);
#else
    return proven_float_sqrt_soft_(x);
#endif
}

bool proven_float_sqrt_is_hardware_(void) { return F_HW_SQRT != 0; }

// -----------------------------------------------------------------------------
// The integer core (RFC-0055): both formats, rounded once
// -----------------------------------------------------------------------------

/* A value of either format is carried as its bits in a proven_u64 (binary32's in the low 32)
 * and, unpacked, as m * 2^e with m in [2^52, 2^53) whatever the format: binary32 is the same
 * arithmetic with its rounding at bit 23 instead of 52. Every result is rounded once, by
 * f_pack, from a significand whose lowest bit is sticky (set when a bit was dropped). */

enum { FC_ZERO, FC_FIN, FC_INF, FC_NAN };

typedef struct {
    int cls;
    bool neg;
    int e;
    proven_u64 m;
} f_val;

#define FMT_MANT(s) ((s) ? 23 : 52)
#define FMT_EMAX(s) ((s) ? 255 : 2047)
#define FMT_BIAS(s) ((s) ? 127 : 1023)
#define FMT_SIGN(s) ((s) ? (proven_u64)0x80000000u : F_SIGN)
#define FMT_INF(s) ((s) ? (proven_u64)0x7f800000u : F_EXP)
#define FMT_QUIET(s) ((s) ? (proven_u64)0x00400000u : F_QUIET)
#define FMT_NAN(s) ((s) ? (proven_u64)0x7fc00000u : F_NAN)

/* Leading zeros of a nonzero value, by halves (no builtin: on some targets it is a call). */
static int f_clz64(proven_u64 x) {
    int n = 0;
    if (!(x >> 32)) { n += 32; x <<= 32; }
    if (!(x >> 48)) { n += 16; x <<= 16; }
    if (!(x >> 56)) { n += 8; x <<= 8; }
    if (!(x >> 60)) { n += 4; x <<= 4; }
    if (!(x >> 62)) { n += 2; x <<= 2; }
    if (!(x >> 63)) n += 1;
    return n;
}

static f_val f_dec(proven_u64 u, bool s) {
    const int mant = FMT_MANT(s), emax = FMT_EMAX(s);
    f_val v = { FC_FIN, (u & FMT_SIGN(s)) != 0, 0, 0 };
    const proven_u64 frac = u & (((proven_u64)1 << mant) - 1);
    int be = (int)((u >> mant) & (proven_u64)emax);
    if (be == emax) { v.cls = frac ? FC_NAN : FC_INF; return v; }
    if (be == 0) {
        if (frac == 0) { v.cls = FC_ZERO; return v; }
        be = 1;
        v.m = frac;
    } else {
        v.m = frac | ((proven_u64)1 << mant);
    }
    v.e = be - FMT_BIAS(s) - mant;
    const int z = f_clz64(v.m) - 11;                     /* to [2^52, 2^53) */
    v.m <<= z;
    v.e -= z;
    return v;
}

/* m * 2^e (m nonzero, its bit 0 sticky) rounded to nearest even, with the sign given:
 * infinity past the largest, through the subnormals to zero below them. */
static proven_u64 f_pack(bool neg, proven_i64 e, proven_u64 m, bool s) {
    const int mant = FMT_MANT(s);
    const proven_u64 sign = neg ? FMT_SIGN(s) : 0;
    const int z = f_clz64(m);
    m <<= z;
    e -= z;                                              /* m in [2^63, 2^64) */
    proven_i64 be = e + 63 + FMT_BIAS(s);
    if (be >= FMT_EMAX(s)) return sign | FMT_INF(s);
    proven_i64 shift = 63 - mant;
    if (be < 1) {
        shift += 1 - be;
        be = 1;
    }
    if (shift > 64) return sign;                         /* below half the smallest subnormal */
    proven_u64 q, rem, half;
    if (shift == 64) {
        q = 0; rem = m; half = (proven_u64)1 << 63;
    } else {
        q = m >> shift; rem = m & (((proven_u64)1 << shift) - 1); half = (proven_u64)1 << (shift - 1);
    }
    if (rem > half || (rem == half && (q & 1))) q++;
    /* A carry out of the significand moves into the exponent, and to infinity past the last. */
    return sign | (((proven_u64)(be - 1) << mant) + q);
}

/* `m` shifted right by `n`, its lowest bit set if a one fell off. */
static proven_u64 f_shr_jam(proven_u64 m, int n) {
    if (n <= 0) return m;
    if (n >= 64) return m != 0;
    return (m >> n) | ((m & (((proven_u64)1 << n) - 1)) != 0);
}

/* The NaN result of an operation whose operands include a NaN: the first, quieted. */
static proven_u64 f_nan2(proven_u64 a, proven_u64 b, bool s) {
    const f_val x = f_dec(a, s);
    return (x.cls == FC_NAN ? a : b) | FMT_QUIET(s);
}

static bool f_isnan(proven_u64 u, bool s) {
    return (u & ~FMT_SIGN(s)) > FMT_INF(s);
}

static proven_u64 f_soft_add(proven_u64 a, proven_u64 b, bool s) {
    f_val x = f_dec(a, s), y = f_dec(b, s);
    if (x.cls == FC_NAN || y.cls == FC_NAN) return f_nan2(a, b, s);
    if (x.cls == FC_INF) return (y.cls == FC_INF && y.neg != x.neg) ? FMT_NAN(s) : a;
    if (y.cls == FC_INF) return b;
    if (x.cls == FC_ZERO) return y.cls == FC_ZERO ? ((x.neg && y.neg) ? FMT_SIGN(s) : 0) : b;
    if (y.cls == FC_ZERO) return a;
    /* Nine bits of room below the significand and two above it. */
    proven_u64 ma = x.m << 9, mb = y.m << 9;
    if (x.e < y.e || (x.e == y.e && ma < mb)) {
        const f_val t = x; x = y; y = t;
        const proven_u64 tm = ma; ma = mb; mb = tm;
    }
    mb = f_shr_jam(mb, x.e - y.e);
    proven_u64 m;
    if (x.neg == y.neg) {
        m = ma + mb;
    } else {
        m = ma - mb;
        if (m == 0) return 0;                            /* x - x is +0 */
    }
    return f_pack(x.neg, (proven_i64)x.e - 9, m, s);
}

static proven_u64 f_soft_sub(proven_u64 a, proven_u64 b, bool s) {
    if (f_isnan(a, s) || f_isnan(b, s)) return f_nan2(a, b, s);
    return f_soft_add(a, b ^ FMT_SIGN(s), s);
}

static proven_u64 f_soft_mul(proven_u64 a, proven_u64 b, bool s) {
    const f_val x = f_dec(a, s), y = f_dec(b, s);
    if (x.cls == FC_NAN || y.cls == FC_NAN) return f_nan2(a, b, s);
    const bool neg = x.neg != y.neg;
    const proven_u64 sign = neg ? FMT_SIGN(s) : 0;
    if (x.cls == FC_INF || y.cls == FC_INF) return (x.cls == FC_ZERO || y.cls == FC_ZERO) ? FMT_NAN(s) : sign | FMT_INF(s);
    if (x.cls == FC_ZERO || y.cls == FC_ZERO) return sign;
    proven_u64 hi, lo;
    f_mul(x.m, y.m, &hi, &lo);                           /* in [2^104, 2^106) */
    const proven_u64 m = (hi << 22) | (lo >> 42) | ((lo & (((proven_u64)1 << 42) - 1)) != 0);
    return f_pack(neg, (proven_i64)x.e + y.e + 42, m, s);
}

static proven_u64 f_soft_div(proven_u64 a, proven_u64 b, bool s) {
    const f_val x = f_dec(a, s), y = f_dec(b, s);
    if (x.cls == FC_NAN || y.cls == FC_NAN) return f_nan2(a, b, s);
    const bool neg = x.neg != y.neg;
    const proven_u64 sign = neg ? FMT_SIGN(s) : 0;
    if (x.cls == FC_INF) return y.cls == FC_INF ? FMT_NAN(s) : sign | FMT_INF(s);
    if (y.cls == FC_INF) return sign;
    if (y.cls == FC_ZERO) return x.cls == FC_ZERO ? FMT_NAN(s) : sign | FMT_INF(s);
    if (x.cls == FC_ZERO) return sign;
    /* Restoring division, a bit a step: 56 quotient bits, then the remainder as the sticky bit. */
    proven_u64 r = x.m, q = 0;
    proven_i64 e = (proven_i64)x.e - y.e;
    if (r < y.m) { r <<= 1; e--; }
    for (int i = 0; i < 56; i++) {
        q <<= 1;
        if (r >= y.m) { r -= y.m; q |= 1; }
        r <<= 1;
    }
    return f_pack(neg, e - 56, (q << 1) | (r != 0), s);
}

/* 128-bit values for fma's exact sum. */
typedef struct { proven_u64 hi, lo; } f_u128;

static f_u128 f_shl128(f_u128 x, int n) {
    if (n <= 0) return x;
    if (n >= 64) return (f_u128){ x.lo << (n - 64), 0 };
    return (f_u128){ (x.hi << n) | (x.lo >> (64 - n)), x.lo << n };
}

static f_u128 f_shr_jam128(f_u128 x, int n) {
    if (n <= 0) return x;
    if (n >= 128) return (f_u128){ 0, (x.hi | x.lo) != 0 };
    f_u128 r;
    proven_u64 lost;
    if (n >= 64) {
        const int k = n - 64;
        lost = x.lo | (k ? x.hi << (64 - k) : 0);
        r.hi = 0;
        r.lo = k ? x.hi >> k : x.hi;
    } else {
        lost = x.lo << (64 - n);
        r.lo = (x.lo >> n) | (x.hi << (64 - n));
        r.hi = x.hi >> n;
    }
    r.lo |= lost != 0;
    return r;
}

static proven_u64 f_soft_fma(proven_u64 a, proven_u64 b, proven_u64 c, bool s) {
    const f_val x = f_dec(a, s), y = f_dec(b, s), z = f_dec(c, s);
    if (x.cls == FC_NAN) return a | FMT_QUIET(s);
    if (y.cls == FC_NAN) return b | FMT_QUIET(s);
    if (z.cls == FC_NAN) return c | FMT_QUIET(s);
    const bool pneg = x.neg != y.neg;
    const bool pzero = x.cls == FC_ZERO || y.cls == FC_ZERO;
    if (x.cls == FC_INF || y.cls == FC_INF) {
        if (pzero) return FMT_NAN(s);
        if (z.cls == FC_INF && z.neg != pneg) return FMT_NAN(s);
        return (pneg ? FMT_SIGN(s) : 0) | FMT_INF(s);
    }
    if (z.cls == FC_INF) return c;
    if (pzero) {
        if (z.cls == FC_ZERO) return (pneg && z.neg) ? FMT_SIGN(s) : 0;
        return c;
    }
    proven_u64 hi, lo;
    f_mul(x.m, y.m, &hi, &lo);                           /* in [2^104, 2^106) */
    if (z.cls == FC_ZERO) {
        const proven_u64 m = (hi << 22) | (lo >> 42) | ((lo & (((proven_u64)1 << 42) - 1)) != 0);
        return f_pack(pneg, (proven_i64)x.e + y.e + 42, m, s);
    }
    /* Both with their leading bit at 125, the smaller shifted down with a sticky bit. */
    f_u128 p = { hi, lo }, q = { 0, z.m };
    const int pz = (hi >> 41) ? 20 : 21;
    p = f_shl128(p, pz);
    q = f_shl128(q, 73);
    proven_i64 ep = (proven_i64)x.e + y.e - pz, eq = (proven_i64)z.e - 73;
    bool pn = pneg, qn = z.neg;
    if (ep < eq || (ep == eq && (p.hi < q.hi || (p.hi == q.hi && p.lo < q.lo)))) {
        const f_u128 t = p; p = q; q = t;
        const proven_i64 te = ep; ep = eq; eq = te;
        const bool tn = pn; pn = qn; qn = tn;
    }
    const proven_i64 d = ep - eq;
    q = f_shr_jam128(q, d > 200 ? 200 : (int)d);
    f_u128 r;
    if (pn == qn) {
        r.lo = p.lo + q.lo;
        r.hi = p.hi + q.hi + (r.lo < p.lo);
    } else {
        r.lo = p.lo - q.lo;
        r.hi = p.hi - q.hi - (p.lo < q.lo);
        if (r.hi == 0 && r.lo == 0) return 0;            /* an exact cancellation is +0 */
    }
    /* To 64 bits with a sticky bit; a deep cancellation was exact, so it loses nothing. */
    int lz = r.hi ? f_clz64(r.hi) : 64 + f_clz64(r.lo);
    r = f_shl128(r, lz);
    return f_pack(pn, ep - lz + 64, r.hi | (r.lo != 0), s);
}

/* The root of m * 2^e by integers, for binary32 (binary64's is proven_float_sqrt_soft_). */
static proven_u64 f_soft_sqrt32(proven_u64 a) {
    const f_val x = f_dec(a, true);
    if (x.cls == FC_NAN) return a | FMT_QUIET(true);
    if (x.cls == FC_ZERO) return a;
    if (x.neg) return FMT_NAN(true);
    if (x.cls == FC_INF) return a;
    proven_u64 m = x.m >> 29;                            /* the 24-bit significand */
    int e = x.e + 29;
    if (e & 1) { m <<= 1; e--; }
    const proven_u64 sq = m << 30;                       /* below 2^55 */
    proven_u32 r = 0;
    for (int bit = 27; bit >= 0; bit--) {
        const proven_u32 t = r | ((proven_u32)1 << bit);
        if (f_mul32(t, t) <= sq) r = t;
    }
    const proven_u64 m2 = ((proven_u64)r << 1) | (f_mul32(r, r) != sq);
    return f_pack(false, (proven_i64)(e - 30) / 2 - 1, m2, true);
}

// -----------------------------------------------------------------------------
// binary64 arithmetic
// -----------------------------------------------------------------------------

#if defined(F_ASM_X86)
static double f_hw_add(double x, double y) { __asm__("addsd %1, %0" : "+x"(x) : "x"(y)); return x; }
static double f_hw_sub(double x, double y) { __asm__("subsd %1, %0" : "+x"(x) : "x"(y)); return x; }
static double f_hw_mul(double x, double y) { __asm__("mulsd %1, %0" : "+x"(x) : "x"(y)); return x; }
static double f_hw_div(double x, double y) { __asm__("divsd %1, %0" : "+x"(x) : "x"(y)); return x; }
static float f_hw_add32(float x, float y) { __asm__("addss %1, %0" : "+x"(x) : "x"(y)); return x; }
static float f_hw_sub32(float x, float y) { __asm__("subss %1, %0" : "+x"(x) : "x"(y)); return x; }
static float f_hw_mul32(float x, float y) { __asm__("mulss %1, %0" : "+x"(x) : "x"(y)); return x; }
static float f_hw_div32(float x, float y) { __asm__("divss %1, %0" : "+x"(x) : "x"(y)); return x; }
#elif defined(F_ASM_A64)
static double f_hw_add(double x, double y) { double r; __asm__("fadd %d0, %d1, %d2" : "=w"(r) : "w"(x), "w"(y)); return r; }
static double f_hw_sub(double x, double y) { double r; __asm__("fsub %d0, %d1, %d2" : "=w"(r) : "w"(x), "w"(y)); return r; }
static double f_hw_mul(double x, double y) { double r; __asm__("fmul %d0, %d1, %d2" : "=w"(r) : "w"(x), "w"(y)); return r; }
static double f_hw_div(double x, double y) { double r; __asm__("fdiv %d0, %d1, %d2" : "=w"(r) : "w"(x), "w"(y)); return r; }
static float f_hw_add32(float x, float y) { float r; __asm__("fadd %s0, %s1, %s2" : "=w"(r) : "w"(x), "w"(y)); return r; }
static float f_hw_sub32(float x, float y) { float r; __asm__("fsub %s0, %s1, %s2" : "=w"(r) : "w"(x), "w"(y)); return r; }
static float f_hw_mul32(float x, float y) { float r; __asm__("fmul %s0, %s1, %s2" : "=w"(r) : "w"(x), "w"(y)); return r; }
static float f_hw_div32(float x, float y) { float r; __asm__("fdiv %s0, %s1, %s2" : "=w"(r) : "w"(x), "w"(y)); return r; }
#else
#if defined(F_OP64)
static double f_hw_add(double x, double y) { return x + y; }
static double f_hw_sub(double x, double y) { return x - y; }
static double f_hw_mul(double x, double y) { return x * y; }
static double f_hw_div(double x, double y) { return x / y; }
#endif
#if defined(F_OP32)
static float f_hw_add32(float x, float y) { return x + y; }
static float f_hw_sub32(float x, float y) { return x - y; }
static float f_hw_mul32(float x, float y) { return x * y; }
static float f_hw_div32(float x, float y) { return x / y; }
#endif
#endif

#define F_BITS proven_float_to_bits
#define F_DBL proven_float_from_bits

#if F_HW64
#define F_OP2(name, hw, soft)                                                       \
    double proven_float_##name(double x, double y) {                              \
        const double r = hw(x, y);                                                 \
        if (f_nan(F_BITS(r))) return F_DBL(soft(F_BITS(x), F_BITS(y), false));      \
        return r;                                                                  \
    }
#else
#define F_OP2(name, hw, soft)                                                       \
    double proven_float_##name(double x, double y) { return F_DBL(soft(F_BITS(x), F_BITS(y), false)); }
#endif

F_OP2(add, f_hw_add, f_soft_add)
F_OP2(sub, f_hw_sub, f_soft_sub)
F_OP2(mul, f_hw_mul, f_soft_mul)
F_OP2(div, f_hw_div, f_soft_div)

#if F_HW_FMA64 && defined(F_ASM_A64)
static double f_hw_fma(double x, double y, double z) { double r; __asm__("fmadd %d0, %d1, %d2, %d3" : "=w"(r) : "w"(x), "w"(y), "w"(z)); return r; }
static float f_hw_fma32(float x, float y, float z) { float r; __asm__("fmadd %s0, %s1, %s2, %s3" : "=w"(r) : "w"(x), "w"(y), "w"(z)); return r; }
#else
#define f_hw_fma __builtin_fma
#define f_hw_fma32 __builtin_fmaf
#endif

double proven_float_fma(double x, double y, double z) {
#if F_HW_FMA64
    const double r = f_hw_fma(x, y, z);
    if (!f_nan(F_BITS(r))) return r;
#endif
    return F_DBL(f_soft_fma(F_BITS(x), F_BITS(y), F_BITS(z), false));
}

// -----------------------------------------------------------------------------
// Comparison (on the bits everywhere: nothing to round)
// -----------------------------------------------------------------------------

/* -1, 0, 1, or 2 when either is a NaN; -0 equals +0. */
static int f_cmp(proven_u64 a, proven_u64 b, bool s) {
    if (f_isnan(a, s) || f_isnan(b, s)) return 2;
    const proven_u64 sb = FMT_SIGN(s);
    if (((a | b) & ~sb) == 0) return 0;                   /* both zeros */
    const proven_u64 ka = (a & sb) ? ~a : a | sb, kb = (b & sb) ? ~b : b | sb;
    if (s) {
        const proven_u32 a32 = (proven_u32)ka, b32 = (proven_u32)kb;
        return a32 < b32 ? -1 : a32 > b32;
    }
    return ka < kb ? -1 : ka > kb;
}

int proven_float_compare(double x, double y) { return f_cmp(F_BITS(x), F_BITS(y), false); }
bool proven_float_eq(double x, double y) { return f_cmp(F_BITS(x), F_BITS(y), false) == 0; }
bool proven_float_lt(double x, double y) { return f_cmp(F_BITS(x), F_BITS(y), false) == -1; }
bool proven_float_le(double x, double y) { const int c = f_cmp(F_BITS(x), F_BITS(y), false); return c == -1 || c == 0; }
bool proven_float_unordered(double x, double y) { return f_cmp(F_BITS(x), F_BITS(y), false) == 2; }

// -----------------------------------------------------------------------------
// Conversions
// -----------------------------------------------------------------------------

proven_u32 proven_float32_to_bits(float x) {
    union { float f; proven_u32 u; } v = { .f = x };
    proven_u32 u = v.u;
    F_OPAQUE(u);
    return u;
}

float proven_float32_from_bits(proven_u32 bits) {
    union { proven_u32 u; float f; } v = { .u = bits };
    return v.f;
}

#define F32_BITS(x) ((proven_u64)proven_float32_to_bits(x))
#define F_FLT(u) proven_float32_from_bits((proven_u32)(u))

/* Between the formats: a NaN keeps its sign and the top of its payload, quieted. */
static proven_u64 f_narrow(proven_u64 u) {
    const f_val v = f_dec(u, false);
    const proven_u64 sign = v.neg ? FMT_SIGN(true) : 0;
    if (v.cls == FC_NAN) return sign | FMT_INF(true) | FMT_QUIET(true) | ((u & F_MANT) >> 29);
    if (v.cls == FC_INF) return sign | FMT_INF(true);
    if (v.cls == FC_ZERO) return sign;
    return f_pack(v.neg, v.e, v.m, true);
}

static proven_u64 f_widen(proven_u64 u) {
    const f_val v = f_dec(u, true);
    const proven_u64 sign = v.neg ? F_SIGN : 0;
    if (v.cls == FC_NAN) return sign | F_EXP | F_QUIET | ((u & 0x7fffffu) << 29);
    if (v.cls == FC_INF) return sign | F_EXP;
    if (v.cls == FC_ZERO) return sign;
    return f_pack(v.neg, v.e, v.m, false);
}

float proven_float_to_float32(double x) { return F_FLT(f_narrow(F_BITS(x))); }
double proven_float_from_float32(float x) { return F_DBL(f_widen(F32_BITS(x))); }

static proven_u64 f_from_int(bool neg, proven_u64 mag, bool s) { return mag ? f_pack(neg, 0, mag, s) : 0; }

static proven_u64 f_abs_i64(proven_i64 v) { return v < 0 ? (proven_u64)0 - (proven_u64)v : (proven_u64)v; }

double proven_float_from_i32(proven_i32 v) { return F_DBL(f_from_int(v < 0, f_abs_i64(v), false)); }
double proven_float_from_u32(proven_u32 v) { return F_DBL(f_from_int(false, v, false)); }
double proven_float_from_u64(proven_u64 v) { return F_DBL(f_from_int(false, v, false)); }

/* `u` (binary64) truncated toward zero: its magnitude and sign, or false for a NaN, an
 * infinity, or a magnitude of 2^64 or more. */
static bool f_to_int(proven_u64 u, proven_u64 *mag, bool *neg) {
    const f_val v = f_dec(u, false);
    *neg = v.neg;
    if (v.cls == FC_NAN || v.cls == FC_INF) return false;
    if (v.cls == FC_ZERO) { *mag = 0; return true; }
    if (v.e >= 0) {
        if (v.e > 11) return false;
        *mag = v.m << v.e;
    } else {
        *mag = v.e <= -64 ? 0 : v.m >> -v.e;
    }
    return true;
}

static proven_err_t f_to_range(proven_u64 u, proven_u64 pos_max, proven_u64 neg_max, proven_u64 *mag, bool *neg) {
    if (!f_to_int(u, mag, neg)) return PROVEN_ERR_OUT_OF_BOUNDS;
    if (*mag > (*neg ? neg_max : pos_max)) return PROVEN_ERR_OUT_OF_BOUNDS;
    return PROVEN_OK;
}

proven_err_t proven_float_to_i32(double x, proven_i32 *out) {
    if (!out) return PROVEN_ERR_INVALID_ARG;
    proven_u64 mag; bool neg;
    const proven_err_t err = f_to_range(F_BITS(x), 0x7fffffffu, 0x80000000u, &mag, &neg);
    if (err == PROVEN_OK) *out = neg ? (proven_i32)(0u - (proven_u32)mag) : (proven_i32)mag;
    return err;
}

proven_err_t proven_float_to_u32(double x, proven_u32 *out) {
    if (!out) return PROVEN_ERR_INVALID_ARG;
    proven_u64 mag; bool neg;
    const proven_err_t err = f_to_range(F_BITS(x), 0xffffffffu, 0, &mag, &neg);
    if (err == PROVEN_OK) *out = (proven_u32)mag;
    return err;
}

proven_err_t proven_float_to_u64(double x, proven_u64 *out) {
    if (!out) return PROVEN_ERR_INVALID_ARG;
    proven_u64 mag; bool neg;
    const proven_err_t err = f_to_range(F_BITS(x), ~(proven_u64)0, 0, &mag, &neg);
    if (err == PROVEN_OK) *out = mag;
    return err;
}

// -----------------------------------------------------------------------------
// binary32
// -----------------------------------------------------------------------------

/* What is exact - the integral values, fmod, frexp, the sign, the smaller and the larger - is
 * binary64's function on the widened value, narrowed again without a rounding. ldexp too: the
 * binary64 product is exact wherever the binary32 one is not zero. What rounds is done in
 * binary32 itself, once. */

#define F32_VIA64(name)                                                     \
    float proven_float32_##name(float x) {                                 \
        return F_FLT(f_narrow(F_BITS(proven_float_##name(F_DBL(f_widen(F32_BITS(x))))))); \
    }
#define F32_VIA64_2(name)                                                   \
    float proven_float32_##name(float x, float y) {                        \
        return F_FLT(f_narrow(F_BITS(proven_float_##name(F_DBL(f_widen(F32_BITS(x))), F_DBL(f_widen(F32_BITS(y))))))); \
    }

F32_VIA64(floor)
F32_VIA64(ceil)
F32_VIA64(trunc)
F32_VIA64(round)
F32_VIA64(rint)
F32_VIA64_2(fmin)
F32_VIA64_2(fmax)
F32_VIA64_2(fmod)

bool proven_float32_is_nan(float x) { return f_isnan(F32_BITS(x), true); }
bool proven_float32_is_inf(float x) { return (F32_BITS(x) & 0x7fffffffu) == 0x7f800000u; }
bool proven_float32_is_finite(float x) { return (F32_BITS(x) & 0x7f800000u) != 0x7f800000u; }

float proven_float32_fabs(float x) { return F_FLT(F32_BITS(x) & 0x7fffffffu); }
float proven_float32_copysign(float x, float y) { return F_FLT((F32_BITS(x) & 0x7fffffffu) | (F32_BITS(y) & 0x80000000u)); }

float proven_float32_frexp(float x, int *exp) {
    return F_FLT(f_narrow(F_BITS(proven_float_frexp(F_DBL(f_widen(F32_BITS(x))), exp))));
}

float proven_float32_ldexp(float x, int exp) {
    return F_FLT(f_narrow(F_BITS(proven_float_ldexp(F_DBL(f_widen(F32_BITS(x))), exp))));
}


float proven_float32_sqrt(float x) {
#if F_HW_SQRT
    const proven_u64 u = F32_BITS(x);
    if (f_isnan(u, true) || (u & 0x7fffffffu) == 0 || (u & 0x80000000u) || u == 0x7f800000u) return F_FLT(f_soft_sqrt32(u));
    return f_hw_sqrt32(x);
#else
    return F_FLT(f_soft_sqrt32(F32_BITS(x)));
#endif
}

#if F_HW32
#define F32_OP2(name, hw, soft)                                                     \
    float proven_float32_##name(float x, float y) {                                \
        const float r = hw(x, y);                                                  \
        if (f_isnan(F32_BITS(r), true)) return F_FLT(soft(F32_BITS(x), F32_BITS(y), true)); \
        return r;                                                                  \
    }
#else
#define F32_OP2(name, hw, soft)                                                     \
    float proven_float32_##name(float x, float y) { return F_FLT(soft(F32_BITS(x), F32_BITS(y), true)); }
#endif

F32_OP2(add, f_hw_add32, f_soft_add)
F32_OP2(sub, f_hw_sub32, f_soft_sub)
F32_OP2(mul, f_hw_mul32, f_soft_mul)
F32_OP2(div, f_hw_div32, f_soft_div)

float proven_float32_fma(float x, float y, float z) {
#if F_HW_FMA32
    const float r = f_hw_fma32(x, y, z);
    if (!f_isnan(F32_BITS(r), true)) return r;
#endif
    return F_FLT(f_soft_fma(F32_BITS(x), F32_BITS(y), F32_BITS(z), true));
}

int proven_float32_compare(float x, float y) { return f_cmp(F32_BITS(x), F32_BITS(y), true); }
bool proven_float32_eq(float x, float y) { return f_cmp(F32_BITS(x), F32_BITS(y), true) == 0; }
bool proven_float32_lt(float x, float y) { return f_cmp(F32_BITS(x), F32_BITS(y), true) == -1; }
bool proven_float32_le(float x, float y) { const int c = f_cmp(F32_BITS(x), F32_BITS(y), true); return c == -1 || c == 0; }
bool proven_float32_unordered(float x, float y) { return f_cmp(F32_BITS(x), F32_BITS(y), true) == 2; }

float proven_float32_from_i32(proven_i32 v) { return F_FLT(f_from_int(v < 0, f_abs_i64(v), true)); }
float proven_float32_from_u32(proven_u32 v) { return F_FLT(f_from_int(false, v, true)); }
float proven_float32_from_i64(proven_i64 v) { return F_FLT(f_from_int(v < 0, f_abs_i64(v), true)); }
float proven_float32_from_u64(proven_u64 v) { return F_FLT(f_from_int(false, v, true)); }

proven_err_t proven_float32_to_i32(float x, proven_i32 *out) { return proven_float_to_i32(F_DBL(f_widen(F32_BITS(x))), out); }
proven_err_t proven_float32_to_u32(float x, proven_u32 *out) { return proven_float_to_u32(F_DBL(f_widen(F32_BITS(x))), out); }
proven_err_t proven_float32_to_i64(float x, proven_i64 *out) { return proven_float_to_i64(F_DBL(f_widen(F32_BITS(x))), out); }
proven_err_t proven_float32_to_u64(float x, proven_u64 *out) { return proven_float_to_u64(F_DBL(f_widen(F32_BITS(x))), out); }

// -----------------------------------------------------------------------------
// For float_math.c and tests
// -----------------------------------------------------------------------------

proven_u64 proven_float_pack_(bool neg, proven_i64 e, proven_u64 m, bool b32) { return f_pack(neg, e, m, b32); }

proven_u64 proven_float_soft_(int op, bool b32, proven_u64 a, proven_u64 b, proven_u64 c) {
    switch (op) {
    case PROVEN_FLOAT_OP_ADD_: return f_soft_add(a, b, b32);
    case PROVEN_FLOAT_OP_SUB_: return f_soft_sub(a, b, b32);
    case PROVEN_FLOAT_OP_MUL_: return f_soft_mul(a, b, b32);
    case PROVEN_FLOAT_OP_DIV_: return f_soft_div(a, b, b32);
    case PROVEN_FLOAT_OP_FMA_: return f_soft_fma(a, b, c, b32);
    case PROVEN_FLOAT_OP_SQRT_: return b32 ? f_soft_sqrt32(a) : F_BITS(proven_float_sqrt_soft_(F_DBL(a)));
    default: return 0;
    }
}

bool proven_float_op_is_hardware_(int op, bool b32) {
    switch (op) {
    case PROVEN_FLOAT_OP_FMA_: return b32 ? F_HW_FMA32 != 0 : F_HW_FMA64 != 0;
    case PROVEN_FLOAT_OP_SQRT_: return F_HW_SQRT != 0;
    default: return b32 ? F_HW32 != 0 : F_HW64 != 0;
    }
}
