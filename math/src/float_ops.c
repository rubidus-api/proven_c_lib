#include "proven/float_env.h"
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

/* The rounding direction and the exception flags of float_env.h (RFC-0058), carried through
 * the integer core. NULL means what every other function of this file does: nearest even, and
 * no flags kept. */
typedef struct { int mode; unsigned flags; } f_env;
enum { FR_NEAREST = 0, FR_ZERO = 1, FR_UP = 2, FR_DOWN = 3 };
enum { FF_INEXACT = 1u, FF_UNDERFLOW = 2u, FF_OVERFLOW = 4u, FF_DIVBYZERO = 8u, FF_INVALID = 16u };
static void f_raise(f_env *env, unsigned f) { if (env) env->flags |= f; }
static int f_mode(const f_env *env) { return env ? env->mode : FR_NEAREST; }
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
static proven_u64 f_soft_sqrt64_env(proven_u64 u, f_env *env);
double proven_float_sqrt_soft_(double x) { return proven_float_from_bits(f_soft_sqrt64_env(proven_float_to_bits(x), NULL)); }

static proven_u64 f_soft_sqrt64_env(proven_u64 u, f_env *env) {
    const double x = proven_float_from_bits(u);
    if (f_nan(u)) { if (!(u & F_QUIET)) f_raise(env, FF_INVALID); return u | F_QUIET; }
    if ((u & ~F_SIGN) == 0) return u;
    if (u & F_SIGN) { f_raise(env, FF_INVALID); return F_NAN; }
    if (u == F_EXP) return u;
    (void)x;
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
    /* r = floor(root); the root is past r + 1/2 exactly when the remainder passes r, and never
     * exactly on it. Directed rounding needs only whether there is a remainder. */
    const bool inexact = rem_hi != 0 || rem_lo != 0;
    if (inexact) f_raise(env, FF_INEXACT);
    const int mode = f_mode(env);
    if (mode == FR_NEAREST ? (rem_hi != 0 || rem_lo > r) : (mode == FR_UP && inexact)) r++;
    int re = (e - 52) / 2;
    if (r >> 53) { r >>= 1; re++; }
    return ((proven_u64)(re + 1075) << 52) | (r & F_MANT);     /* r * 2^re, r in [2^52, 2^53) */
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


/* Whether a rounding to the precision kept rounds the magnitude up: q the value kept, rem what
 * falls off and half its halfway point. */
static bool f_round_up(int mode, bool neg, proven_u64 q, proven_u64 rem, proven_u64 half) {
    switch (mode) {
    case FR_ZERO: return false;
    case FR_UP: return !neg && rem != 0;
    case FR_DOWN: return neg && rem != 0;
    default: return rem > half || (rem == half && (q & 1));
    }
}

/* m * 2^e (m nonzero, its bit 0 sticky) rounded in `env`'s direction (nearest even without one),
 * with the sign given: past the largest, infinity or the largest finite as the direction says;
 * through the subnormals to zero below them. Flags: inexact, overflow, and underflow when the
 * result is tiny after rounding (as x86 and RISC-V detect it) and inexact. */
static proven_u64 f_pack(bool neg, proven_i64 e, proven_u64 m, bool s);
static proven_u64 f_pack_env(bool neg, proven_i64 e, proven_u64 m, bool s, f_env *env) {
    if (!env) return f_pack(neg, e, m, s);
    const int mant = FMT_MANT(s), mode = f_mode(env);
    const proven_u64 sign = neg ? FMT_SIGN(s) : 0;
    const int z = f_clz64(m);
    m <<= z;
    e -= z;                                              /* m in [2^63, 2^64) */
    proven_i64 be = e + 63 + FMT_BIAS(s);
    if (be >= FMT_EMAX(s)) {
        f_raise(env, FF_OVERFLOW | FF_INEXACT);
        const bool to_inf = mode == FR_NEAREST || (mode == FR_UP && !neg) || (mode == FR_DOWN && neg);
        return sign | (to_inf ? FMT_INF(s) : FMT_INF(s) - 1);
    }
    bool tiny = false;
    proven_i64 shift = 63 - mant;
    if (be < 1) {
        /* Tiny after rounding: the value rounded to full precision, exponent unbounded, is below
         * the smallest normal. Only a value just below it can round up to it. */
        tiny = true;
        if (be == 0) {
            const proven_u64 q0 = m >> shift, rem0 = m & (((proven_u64)1 << shift) - 1);
            if (f_round_up(mode, neg, q0, rem0, (proven_u64)1 << (shift - 1)) && ((q0 + 1) >> (mant + 1))) tiny = false;
        }
        shift += 1 - be;
        be = 1;
    }
    proven_u64 q, rem, half;
    if (shift > 64) {
        q = 0; rem = 1; half = (proven_u64)1 << 63;      /* below half the smallest subnormal */
    } else if (shift == 64) {
        q = 0; rem = m; half = (proven_u64)1 << 63;
    } else {
        q = m >> shift; rem = m & (((proven_u64)1 << shift) - 1); half = (proven_u64)1 << (shift - 1);
    }
    if (rem) f_raise(env, tiny ? FF_INEXACT | FF_UNDERFLOW : FF_INEXACT);
    if (f_round_up(mode, neg, q, rem, half)) q++;
    /* A carry out of the significand moves into the exponent, and to infinity past the last. */
    const proven_u64 r = sign | (((proven_u64)(be - 1) << mant) + q);
    if ((r & ~sign) == FMT_INF(s)) f_raise(env, FF_OVERFLOW | FF_INEXACT);
    return r;
}

/* m * 2^e (m nonzero, its bit 0 sticky) rounded to nearest even, with the sign given:
 * infinity past the largest, through the subnormals to zero below them. The path of every
 * function but float_env.h's: kept apart from f_pack_env, which costs a fifth more. */
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

/* A signalling NaN among the operands raises invalid; a quiet one passes silently. */
static bool f_issnan(proven_u64 u, bool s) { return f_isnan(u, s) && !(u & FMT_QUIET(s)); }
static proven_u64 f_nan2_env(proven_u64 a, proven_u64 b, bool s, f_env *env) {
    if (f_issnan(a, s) || f_issnan(b, s)) f_raise(env, FF_INVALID);
    return f_nan2(a, b, s);
}
/* A NaN made by an invalid operation. */
static proven_u64 f_invalid(bool s, f_env *env) { f_raise(env, FF_INVALID); return FMT_NAN(s); }
/* The sign of an exact zero sum of operands with different signs: + but under rounding down. */
static proven_u64 f_zero_sum(bool s, f_env *env) { return f_mode(env) == FR_DOWN ? FMT_SIGN(s) : 0; }

static proven_u64 f_soft_add_env(proven_u64 a, proven_u64 b, bool s, f_env *env) {
    f_val x = f_dec(a, s), y = f_dec(b, s);
    if (x.cls == FC_NAN || y.cls == FC_NAN) return f_nan2_env(a, b, s, env);
    if (x.cls == FC_INF) return (y.cls == FC_INF && y.neg != x.neg) ? f_invalid(s, env) : a;
    if (y.cls == FC_INF) return b;
    if (x.cls == FC_ZERO) return y.cls == FC_ZERO ? (x.neg == y.neg ? a : f_zero_sum(s, env)) : b;
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
        if (m == 0) return f_zero_sum(s, env);           /* x - x is +0 (-0 rounding down) */
    }
    return f_pack_env(x.neg, (proven_i64)x.e - 9, m, s, env);
}
static proven_u64 f_soft_add(proven_u64 a, proven_u64 b, bool s) { return f_soft_add_env(a, b, s, NULL); }

static proven_u64 f_soft_sub_env(proven_u64 a, proven_u64 b, bool s, f_env *env) {
    if (f_isnan(a, s) || f_isnan(b, s)) return f_nan2_env(a, b, s, env);
    return f_soft_add_env(a, b ^ FMT_SIGN(s), s, env);
}
static proven_u64 f_soft_sub(proven_u64 a, proven_u64 b, bool s) { return f_soft_sub_env(a, b, s, NULL); }

static proven_u64 f_soft_mul_env(proven_u64 a, proven_u64 b, bool s, f_env *env) {
    const f_val x = f_dec(a, s), y = f_dec(b, s);
    if (x.cls == FC_NAN || y.cls == FC_NAN) return f_nan2_env(a, b, s, env);
    const bool neg = x.neg != y.neg;
    const proven_u64 sign = neg ? FMT_SIGN(s) : 0;
    if (x.cls == FC_INF || y.cls == FC_INF) return (x.cls == FC_ZERO || y.cls == FC_ZERO) ? f_invalid(s, env) : sign | FMT_INF(s);
    if (x.cls == FC_ZERO || y.cls == FC_ZERO) return sign;
    proven_u64 hi, lo;
    f_mul(x.m, y.m, &hi, &lo);                           /* in [2^104, 2^106) */
    const proven_u64 m = (hi << 22) | (lo >> 42) | ((lo & (((proven_u64)1 << 42) - 1)) != 0);
    return f_pack_env(neg, (proven_i64)x.e + y.e + 42, m, s, env);
}
static proven_u64 f_soft_mul(proven_u64 a, proven_u64 b, bool s) { return f_soft_mul_env(a, b, s, NULL); }

static proven_u64 f_soft_div_env(proven_u64 a, proven_u64 b, bool s, f_env *env) {
    const f_val x = f_dec(a, s), y = f_dec(b, s);
    if (x.cls == FC_NAN || y.cls == FC_NAN) return f_nan2_env(a, b, s, env);
    const bool neg = x.neg != y.neg;
    const proven_u64 sign = neg ? FMT_SIGN(s) : 0;
    if (x.cls == FC_INF) return y.cls == FC_INF ? f_invalid(s, env) : sign | FMT_INF(s);
    if (y.cls == FC_INF) return sign;
    if (y.cls == FC_ZERO) {
        if (x.cls == FC_ZERO) return f_invalid(s, env);
        f_raise(env, FF_DIVBYZERO);
        return sign | FMT_INF(s);
    }
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
    return f_pack_env(neg, e - 56, (q << 1) | (r != 0), s, env);
}
static proven_u64 f_soft_div(proven_u64 a, proven_u64 b, bool s) { return f_soft_div_env(a, b, s, NULL); }

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

static proven_u64 f_soft_fma_env(proven_u64 a, proven_u64 b, proven_u64 c, bool s, f_env *env) {
    const f_val x = f_dec(a, s), y = f_dec(b, s), z = f_dec(c, s);
    if (x.cls == FC_NAN || y.cls == FC_NAN || z.cls == FC_NAN) {
        if (f_issnan(a, s) || f_issnan(b, s) || f_issnan(c, s)) f_raise(env, FF_INVALID);
        /* inf * 0 with a quiet NaN to add: IEEE 754 leaves invalid to the implementation; not
         * signalled, as x86 does not */
        if (x.cls == FC_NAN) return a | FMT_QUIET(s);
        if (y.cls == FC_NAN) return b | FMT_QUIET(s);
        return c | FMT_QUIET(s);
    }
    const bool pneg = x.neg != y.neg;
    const bool pzero = x.cls == FC_ZERO || y.cls == FC_ZERO;
    if (x.cls == FC_INF || y.cls == FC_INF) {
        if (pzero) return f_invalid(s, env);
        if (z.cls == FC_INF && z.neg != pneg) return f_invalid(s, env);
        return (pneg ? FMT_SIGN(s) : 0) | FMT_INF(s);
    }
    if (z.cls == FC_INF) return c;
    if (pzero) {
        if (z.cls == FC_ZERO) return pneg == z.neg ? c : f_zero_sum(s, env);
        return c;
    }
    proven_u64 hi, lo;
    f_mul(x.m, y.m, &hi, &lo);                           /* in [2^104, 2^106) */
    if (z.cls == FC_ZERO) {
        const proven_u64 m = (hi << 22) | (lo >> 42) | ((lo & (((proven_u64)1 << 42) - 1)) != 0);
        return f_pack_env(pneg, (proven_i64)x.e + y.e + 42, m, s, env);
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
        if (r.hi == 0 && r.lo == 0) return f_zero_sum(s, env);   /* an exact cancellation is +0 (-0 rounding down) */
    }
    /* To 64 bits with a sticky bit; a deep cancellation was exact, so it loses nothing. */
    int lz = r.hi ? f_clz64(r.hi) : 64 + f_clz64(r.lo);
    r = f_shl128(r, lz);
    return f_pack_env(pn, ep - lz + 64, r.hi | (r.lo != 0), s, env);
}
static proven_u64 f_soft_fma(proven_u64 a, proven_u64 b, proven_u64 c, bool s) { return f_soft_fma_env(a, b, c, s, NULL); }

/* The root of m * 2^e by integers, for binary32 (binary64's is proven_float_sqrt_soft_). */
static proven_u64 f_soft_sqrt32_env(proven_u64 a, f_env *env) {
    const f_val x = f_dec(a, true);
    if (x.cls == FC_NAN) { if (f_issnan(a, true)) f_raise(env, FF_INVALID); return a | FMT_QUIET(true); }
    if (x.cls == FC_ZERO) return a;
    if (x.neg) return f_invalid(true, env);
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
    return f_pack_env(false, (proven_i64)(e - 30) / 2 - 1, m2, true, env);
}
static proven_u64 f_soft_sqrt32(proven_u64 a) { return f_soft_sqrt32_env(a, NULL); }

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

// -----------------------------------------------------------------------------
// The rest of math.h's exact functions (RFC-0058): remainder, remquo, nextafter, modf, ilogb,
// logb, fdim, and rounding to an integer value that is returned as one
// -----------------------------------------------------------------------------

/* IEEE 754 remainder: x - n*y with n the integer nearest x/y, ties to even; exact. `quo` gets
 * the sign of x/y and the low 31 bits of |n| (C asks for at least three). fmod's long division, which also keeps the
 * quotient's low bits. */
static proven_u64 f_remquo(proven_u64 a, proven_u64 b, int *quo) {
    *quo = 0;
    if (f_nan(a)) return a | F_QUIET;
    if (f_nan(b)) return b | F_QUIET;
    if ((a & ~F_SIGN) == F_EXP || (b & ~F_SIGN) == 0) return F_NAN;
    if ((b & ~F_SIGN) == F_EXP || (a & ~F_SIGN) == 0) return a;
    const proven_u64 sign = a & F_SIGN;
    const int qsign = ((a ^ b) & F_SIGN) ? -1 : 1;
    int ex, ey;
    proven_u64 mx = f_split(a, &ex);
    const proven_u64 my = f_split(b, &ey);
    proven_u64 q = 0;
    if (ex >= ey) {
        for (; ex > ey; ex--) {
            if (mx >= my) { mx -= my; q |= 1; }
            mx <<= 1;
            q <<= 1;
        }
        if (mx >= my) { mx -= my; q |= 1; }
        /* now |x| = q*|y| + mx*2^ey, 0 <= mx < my */
    } else {
        /* |x| < |y|: the quotient is 0, and 2|x| reaches |y| only when the exponents differ by one */
        if (ey - ex >= 2) { *quo = 0; return a; }
        /* 2|x| against |y|, on |x|'s grid: 2*mx against 2*my */
        if (mx > my) {                                   /* 2|x| > |y|: x - sign*|y| */
            *quo = qsign;
            proven_u64 m = 2 * my - mx;                  /* |y| - |x|, on |x|'s grid */
            while (!(m & F_HIDDEN)) { m <<= 1; ex--; }
            return f_join_exact(sign ^ F_SIGN, m, ex);
        }
        return a;                                        /* below, or a tie with n = 0 (even) */
    }
    bool flip = false;
    if (2 * mx > my || (2 * mx == my && (q & 1))) { mx = my - mx; q++; flip = true; }
    *quo = qsign * (int)(q & 0x7fffffff);           /* C asks for at least the low three bits */
    if (mx == 0) return sign;
    while (!(mx & F_HIDDEN)) { mx <<= 1; ex--; }
    return f_join_exact(flip ? sign ^ F_SIGN : sign, mx, ex);
}

double proven_float_remainder(double x, double y) {
    int q;
    return proven_float_from_bits(f_remquo(proven_float_to_bits(x), proven_float_to_bits(y), &q));
}

double proven_float_remquo(double x, double y, int *quo) {
    int q;
    const double r = proven_float_from_bits(f_remquo(proven_float_to_bits(x), proven_float_to_bits(y), &q));
    if (quo) *quo = q;
    return r;
}

double proven_float_nextafter(double x, double y) {
    const proven_u64 a = proven_float_to_bits(x), b = proven_float_to_bits(y);
    if (f_nan(a)) return f_quiet(a);
    if (f_nan(b)) return f_quiet(b);
    const int c = proven_float_compare(x, y);
    if (c == 0) return y;                                /* equal: y, so nextafter(-0, +0) is +0 */
    if ((a & ~F_SIGN) == 0) return proven_float_from_bits((b & F_SIGN) | 1);   /* the smallest subnormal toward y */
    /* away from zero when x and the direction agree in sign, else toward it */
    const bool up = c < 0;
    const bool neg = (a & F_SIGN) != 0;
    return proven_float_from_bits(up != neg ? a + 1 : a - 1);
}

double proven_float_modf(double x, double *int_part) {
    const proven_u64 u = proven_float_to_bits(x);
    double dummy;
    if (!int_part) int_part = &dummy;
    if (f_nan(u)) { *int_part = f_quiet(u); return *int_part; }
    const double t = proven_float_trunc(x);
    *int_part = t;
    if ((u & ~F_SIGN) == F_EXP || proven_float_to_bits(t) == u) return proven_float_from_bits(u & F_SIGN);
    return proven_float_sub(x, t);                     /* exact: x and t share the grid */
}

int proven_float_ilogb(double x) {
    const proven_u64 u = proven_float_to_bits(x);
    if (f_nan(u)) return PROVEN_FLOAT_ILOGB_NAN;
    if ((u & ~F_SIGN) == 0) return PROVEN_FLOAT_ILOGB_ZERO;
    if ((u & ~F_SIGN) == F_EXP) return INT_MAX;
    int e;
    (void)f_split(u, &e);
    return e + 52;
}

double proven_float_logb(double x) {
    const proven_u64 u = proven_float_to_bits(x);
    if (f_nan(u)) return f_quiet(u);
    if ((u & ~F_SIGN) == 0) return proven_float_from_bits(F_SIGN | F_EXP);     /* -inf */
    if ((u & ~F_SIGN) == F_EXP) return proven_float_from_bits(F_EXP);          /* +inf */
    return proven_float_from_i32(proven_float_ilogb(x));
}

double proven_float_fdim(double x, double y) {
    const proven_u64 a = proven_float_to_bits(x), b = proven_float_to_bits(y);
    if (f_nan(a)) return f_quiet(a);
    if (f_nan(b)) return f_quiet(b);
    return proven_float_lt(y, x) ? proven_float_sub(x, y) : proven_float_from_bits(0);
}

proven_err_t proven_float_round_to_i64(double x, proven_i64 *out) {
    return proven_float_to_i64(proven_float_round(x), out);
}

proven_err_t proven_float_rint_to_i64(double x, proven_i64 *out) {
    return proven_float_to_i64(proven_float_rint(x), out);
}

proven_err_t proven_float_round_to_i32(double x, proven_i32 *out) {
    return proven_float_to_i32(proven_float_round(x), out);
}

proven_err_t proven_float_rint_to_i32(double x, proven_i32 *out) {
    return proven_float_to_i32(proven_float_rint(x), out);
}

/* binary32: through binary64, where each of these is exact, and back (also exact); nextafter
 * on binary32's own bits. */
float proven_float32_remainder(float x, float y) {
    return proven_float_to_float32(proven_float_remainder(proven_float_from_float32(x), proven_float_from_float32(y)));
}

float proven_float32_remquo(float x, float y, int *quo) {
    return proven_float_to_float32(proven_float_remquo(proven_float_from_float32(x), proven_float_from_float32(y), quo));
}

float proven_float32_nextafter(float x, float y) {
    const proven_u32 a = proven_float32_to_bits(x), b = proven_float32_to_bits(y);
    if ((a & 0x7fffffffu) > 0x7f800000u) return proven_float32_from_bits(a | 0x00400000u);
    if ((b & 0x7fffffffu) > 0x7f800000u) return proven_float32_from_bits(b | 0x00400000u);
    const int c = proven_float32_compare(x, y);
    if (c == 0) return y;
    if ((a & 0x7fffffffu) == 0) return proven_float32_from_bits((b & 0x80000000u) | 1u);
    const bool up = c < 0, neg = (a & 0x80000000u) != 0;
    return proven_float32_from_bits(up != neg ? a + 1u : a - 1u);
}

float proven_float32_modf(float x, float *int_part) {
    double i;
    const float f = proven_float_to_float32(proven_float_modf(proven_float_from_float32(x), &i));
    if (int_part) *int_part = proven_float_to_float32(i);
    return f;
}

int proven_float32_ilogb(float x) { return proven_float_ilogb(proven_float_from_float32(x)); }

float proven_float32_logb(float x) { return proven_float_to_float32(proven_float_logb(proven_float_from_float32(x))); }

float proven_float32_fdim(float x, float y) {
    const proven_u32 a = proven_float32_to_bits(x), b = proven_float32_to_bits(y);
    if ((a & 0x7fffffffu) > 0x7f800000u) return proven_float32_from_bits(a | 0x00400000u);
    if ((b & 0x7fffffffu) > 0x7f800000u) return proven_float32_from_bits(b | 0x00400000u);
    return proven_float32_lt(y, x) ? proven_float32_sub(x, y) : proven_float32_from_bits(0);
}

proven_err_t proven_float32_round_to_i64(float x, proven_i64 *out) { return proven_float_round_to_i64(proven_float_from_float32(x), out); }

proven_err_t proven_float32_rint_to_i64(float x, proven_i64 *out) { return proven_float_rint_to_i64(proven_float_from_float32(x), out); }

proven_err_t proven_float32_round_to_i32(float x, proven_i32 *out) { return proven_float_round_to_i32(proven_float_from_float32(x), out); }

proven_err_t proven_float32_rint_to_i32(float x, proven_i32 *out) { return proven_float_rint_to_i32(proven_float_from_float32(x), out); }

/* x compared with y in binary64, where x is exact: one step on x's own bits, as nextafter. */
float proven_float32_nexttoward(float x, double y) {
    const proven_u32 a = proven_float32_to_bits(x);
    if ((a & 0x7fffffffu) > 0x7f800000u) return proven_float32_from_bits(a | 0x00400000u);
    if (proven_float_is_nan(y)) return proven_float_to_float32(y);    /* quieted, the payload's top */
    const int c = proven_float_compare(proven_float_from_float32(x), y);
    if (c == 0) return proven_float_to_float32(y);                     /* exact: y is x's value */
    if ((a & 0x7fffffffu) == 0) return proven_float32_from_bits((proven_u32)((proven_float_to_bits(y) >> 32) & 0x80000000u) | 1u);
    const bool up = c < 0, neg = (a & 0x80000000u) != 0;
    return proven_float32_from_bits(up != neg ? a + 1u : a - 1u);
}

// -----------------------------------------------------------------------------
// The floating-point environment as a value (float_env.h, RFC-0058)
// -----------------------------------------------------------------------------


/* Always the integers: the same bits and the same flags on every target, whatever the hardware's
 * own mode (which these functions neither read nor change). */
static f_env f_env_in(const proven_float_env_t *e) {
    return (f_env){ e ? (int)e->round : FR_NEAREST, 0 };
}
static void f_env_out(proven_float_env_t *e, const f_env *f) { if (e) e->flags |= f->flags; }

#define F_ENV_OP2(name, core)                                                                  \
    double proven_float_env_##name(proven_float_env_t *env, double x, double y) {             \
        f_env f = f_env_in(env);                                                               \
        const proven_u64 r = core(F_BITS(x), F_BITS(y), false, &f);                            \
        f_env_out(env, &f);                                                                    \
        return F_DBL(r);                                                                       \
    }                                                                                          \
    float proven_float32_env_##name(proven_float_env_t *env, float x, float y) {              \
        f_env f = f_env_in(env);                                                               \
        const proven_u64 r = core(F32_BITS(x), F32_BITS(y), true, &f);                         \
        f_env_out(env, &f);                                                                    \
        return F_FLT(r);                                                                       \
    }
F_ENV_OP2(add, f_soft_add_env)
F_ENV_OP2(sub, f_soft_sub_env)
F_ENV_OP2(mul, f_soft_mul_env)
F_ENV_OP2(div, f_soft_div_env)

double proven_float_env_fma(proven_float_env_t *env, double x, double y, double z) {
    f_env f = f_env_in(env);
    const proven_u64 r = f_soft_fma_env(F_BITS(x), F_BITS(y), F_BITS(z), false, &f);
    f_env_out(env, &f);
    return F_DBL(r);
}
float proven_float32_env_fma(proven_float_env_t *env, float x, float y, float z) {
    f_env f = f_env_in(env);
    const proven_u64 r = f_soft_fma_env(F32_BITS(x), F32_BITS(y), F32_BITS(z), true, &f);
    f_env_out(env, &f);
    return F_FLT(r);
}

double proven_float_env_sqrt(proven_float_env_t *env, double x) {
    f_env f = f_env_in(env);
    const proven_u64 r = f_soft_sqrt64_env(F_BITS(x), &f);
    f_env_out(env, &f);
    return F_DBL(r);
}
float proven_float32_env_sqrt(proven_float_env_t *env, float x) {
    f_env f = f_env_in(env);
    const proven_u64 r = f_soft_sqrt32_env(F32_BITS(x), &f);
    f_env_out(env, &f);
    return F_FLT(r);
}

/* To an integral value in the direction given (C's rint under fesetround): inexact when it
 * changes the value; a signalling NaN is invalid. */
static proven_u64 f_rint_env(proven_u64 u, f_env *env) {
    if (f_nan(u)) { if (!(u & F_QUIET)) f_raise(env, FF_INVALID); return u | F_QUIET; }
    const proven_u64 sign = u & F_SIGN, mag = u & ~F_SIGN;
    const int be = (int)(mag >> 52);
    if (be >= 1075 || mag == 0) return u;                /* already integral, or infinite, or zero */
    const bool neg = sign != 0;
    if (be < 1023) {                                     /* |x| < 1: 0 or 1 */
        f_raise(env, FF_INEXACT);
        bool up;
        switch (f_mode(env)) {
        case FR_ZERO: up = false; break;
        case FR_UP: up = !neg; break;
        case FR_DOWN: up = neg; break;
        default: up = be == 1022 && (mag & F_MANT) != 0;     /* above a half (a half itself goes to 0) */
        }
        return sign | (up ? (proven_u64)1023 << 52 : 0);
    }
    const int frac = 1075 - be;                          /* fraction bits, 1..52 */
    const proven_u64 m = (mag & F_MANT) | F_HIDDEN;
    const proven_u64 q = m >> frac, rem = m & (((proven_u64)1 << frac) - 1);
    if (!rem) return u;
    f_raise(env, FF_INEXACT);
    const proven_u64 r = (q + (f_round_up(f_mode(env), neg, q, rem, (proven_u64)1 << (frac - 1)) ? 1 : 0)) << frac;
    /* r * 2^(be - 1075): put it back as a double (a carry may add a bit) */
    return f_pack(neg, (proven_i64)be - 1075, r, false);
}

double proven_float_env_rint(proven_float_env_t *env, double x) {
    f_env f = f_env_in(env);
    const proven_u64 r = f_rint_env(F_BITS(x), &f);
    f_env_out(env, &f);
    return F_DBL(r);
}
float proven_float32_env_rint(proven_float_env_t *env, float x) {
    /* through binary64, where it is exact; the result is integral, so narrowing it is too */
    f_env f = f_env_in(env);
    const proven_u64 r = f_rint_env(f_widen(F32_BITS(x)), &f);
    f_env_out(env, &f);
    return F_FLT(f_narrow(r));
}

/* nearbyint: rint in the direction, its inexact not raised (f starts with no flags) */
double proven_float_env_nearbyint(proven_float_env_t *env, double x) {
    f_env f = f_env_in(env);
    const proven_u64 r = f_rint_env(F_BITS(x), &f);
    f.flags &= ~(unsigned)FF_INEXACT;
    f_env_out(env, &f);
    return F_DBL(r);
}
float proven_float32_env_nearbyint(proven_float_env_t *env, float x) {
    f_env f = f_env_in(env);
    const proven_u64 r = f_rint_env(f_widen(F32_BITS(x)), &f);
    f.flags &= ~(unsigned)FF_INEXACT;
    f_env_out(env, &f);
    return F_FLT(f_narrow(r));
}

proven_err_t proven_float_env_to_i64(proven_float_env_t *env, double x, proven_i64 *out) {
    f_env f = f_env_in(env);
    const proven_u64 r = f_rint_env(F_BITS(x), &f);
    const proven_err_t e = proven_float_to_i64(F_DBL(r), out);
    if (e != PROVEN_OK) f.flags = (f.flags & ~(unsigned)FF_INEXACT) | FF_INVALID;
    f_env_out(env, &f);
    return e;
}
proven_err_t proven_float32_env_to_i64(proven_float_env_t *env, float x, proven_i64 *out) {
    return proven_float_env_to_i64(env, F_DBL(f_widen(F32_BITS(x))), out);
}

float proven_float_env_to_float32(proven_float_env_t *env, double x) {
    f_env f = f_env_in(env);
    const proven_u64 u = F_BITS(x);
    const f_val v = f_dec(u, false);
    proven_u64 r;
    if (v.cls == FC_NAN) { if (!(u & F_QUIET)) f_raise(&f, FF_INVALID); r = f_narrow(u); }
    else if (v.cls != FC_FIN) r = f_narrow(u);
    else r = f_pack_env(v.neg, v.e, v.m, true, &f);
    f_env_out(env, &f);
    return F_FLT(r);
}

double proven_float_env_from_i64(proven_float_env_t *env, proven_i64 v) {
    f_env f = f_env_in(env);
    const bool neg = v < 0;
    const proven_u64 mag = neg ? (proven_u64)0 - (proven_u64)v : (proven_u64)v;
    const proven_u64 r = mag ? f_pack_env(neg, 0, mag, false, &f) : 0;
    f_env_out(env, &f);
    return F_DBL(r);
}
float proven_float32_env_from_i64(proven_float_env_t *env, proven_i64 v) {
    f_env f = f_env_in(env);
    const bool neg = v < 0;
    const proven_u64 mag = neg ? (proven_u64)0 - (proven_u64)v : (proven_u64)v;
    const proven_u64 r = mag ? f_pack_env(neg, 0, mag, true, &f) : 0;
    f_env_out(env, &f);
    return F_FLT(r);
}
