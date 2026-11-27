#ifndef PROVEN_CM_SHIM_H
#define PROVEN_CM_SHIM_H

/*
 * What the CORE-MATH sources of math/src/cm take from the C library, for proven_math
 * (RFC-0056), and whether they are compiled at all.
 *
 * PROVEN_CM is 1 where the FPU path is taken: x86-64, AArch64 and wasm, little-endian, with a
 * 128-bit integer type, evaluating at each type's own precision, not under fast-math, and not
 * with PROVEN_FLOAT_SOFT or PROVEN_FLOAT_MATH_SMALL. On x86-64 the code is compiled for FMA and
 * AVX2 (CORE-MATH's fast paths are written for fused multiply-add) and float_math.c calls it
 * only when the processor has them, or always when the build itself targets them; elsewhere the
 * integer path answers, with the same bits. wasm has no fused multiply-add: fma is computed
 * exactly without it (proven_cm_fma below), and the 128-bit helpers clang calls there come from
 * cm_rt_wasm.c unless the toolchain links its own.
 *
 * The rounding mode is to nearest (RFC-0055 D-7), and no floating-point flag is raised or read
 * (no flags): CORE-MATH uses the flags only to report them, never to choose a result.
 * Contraction is on within an expression (-ffp-contract=on), which CORE-MATH's code is written
 * to allow, never across statements (=fast, which it is not); RFC-0056 D-15. Each compiler then
 * makes a program of its own: the hard-to-round lists are run for each one used.
 */

#include "proven/math_config.h"
#include "proven/float_ops.h"
#include <stdint.h>

#include "cm_config.h"

#if PROVEN_CM
#if defined(__clang__)
#pragma STDC FP_CONTRACT ON
#if defined(__x86_64__) && !(defined(__FMA__) && defined(__AVX2__))
#pragma clang attribute push (__attribute__((target("fma,avx2"))), apply_to = function)
#define PROVEN_CM_PUSHED 1
#endif
#else
#pragma GCC optimize ("fp-contract=on")
#if defined(__x86_64__) && !(defined(__FMA__) && defined(__AVX2__))
#pragma GCC target ("fma,avx2")
#endif
#endif
#endif

#define FE_TONEAREST 0
#define FE_DOWNWARD 1
#define FE_UPWARD 2
#define FE_TOWARDZERO 3
#define FE_INEXACT 1
#define FE_UNDERFLOW 2
#define FE_OVERFLOW 4
#define FE_DIVBYZERO 8
#define FE_INVALID 16
typedef int fenv_t;
typedef int fexcept_t;
static inline int proven_cm_fegetround(void) { return FE_TONEAREST; }
static inline int proven_cm_feraise(int e) { (void)e; return 0; }
static inline int proven_cm_fetest(int e) { (void)e; return 0; }
static inline int proven_cm_feclear(int e) { (void)e; return 0; }
static inline int proven_cm_fehold(fenv_t *e) { *e = 0; return 0; }
static inline int proven_cm_feupdate(const fenv_t *e) { (void)e; return 0; }
static inline int proven_cm_fegetflag(fexcept_t *f, int e) { (void)e; *f = 0; return 0; }
static inline int proven_cm_fesetflag(const fexcept_t *f, int e) { (void)f; (void)e; return 0; }
#define fegetround proven_cm_fegetround
#define feraiseexcept proven_cm_feraise
#define fetestexcept proven_cm_fetest
#define feclearexcept proven_cm_feclear
#define feholdexcept proven_cm_fehold
#define feupdateenv proven_cm_feupdate
#define fegetexceptflag proven_cm_fegetflag
#define fesetexceptflag proven_cm_fesetflag
#define NAN __builtin_nan("")
#define INFINITY __builtin_inf()
#define PRIx64 "llx"
#define PRId64 "lld"
static inline int proven_cm_noprint(const char *f, ...) { (void)f; return 0; }
#define printf(...) proven_cm_noprint(__VA_ARGS__)

#if defined(__aarch64__)
static inline double proven_cm_fma(double a, double b, double c) {
    double r;
    __asm__("fmadd %d0, %d1, %d2, %d3" : "=w"(r) : "w"(a), "w"(b), "w"(c));
    return r;
}
static inline float proven_cm_fmaf(float a, float b, float c) {
    float r;
    __asm__("fmadd %s0, %s1, %s2, %s3" : "=w"(r) : "w"(a), "w"(b), "w"(c));
    return r;
}
#elif defined(__wasm__)
/* No fused multiply-add on wasm. CORE-MATH's usual fma is fma(a, b, -a*b), the exact error of a
 * product: Dekker's product with Veltkamp's split computes it exactly when nothing overflows or
 * underflows, which the bounds below make sure of (|a*b| in [2^-900, 2^1000], |a| and |b| at
 * most 2^995: the split's 2^27 + 1 times a does not overflow, and no partial product falls
 * below the normals). For binary32 the product of two floats is exact in a double, so
 * a*b - p is exact there and fits a float. Every other fma: float_ops.h's, by integers. Both
 * give the fused result's bits (ops/checks/float-math/fmaemu.c compares them). */
static inline double proven_cm_fma(double a, double b, double c) {
    const double p = a * b, ap = __builtin_fabs(p);
    if (c == -p && ap >= 0x1p-900 && ap <= 0x1p1000 && __builtin_fabs(a) <= 0x1p995 && __builtin_fabs(b) <= 0x1p995) {
        const double k = 0x1p27 + 1.0;
        const double ta = k * a, ah = ta - (ta - a), al = a - ah;
        const double tb = k * b, bh = tb - (tb - b), bl = b - bh;
        return ((ah * bh - p) + ah * bl + al * bh) + al * bl;
    }
    return proven_float_fma(a, b, c);
}
static inline float proven_cm_fmaf(float a, float b, float c) {
    const float p = a * b;
    if (c == -p && __builtin_fabsf(p) >= 0x1p-100f && __builtin_fabsf(p) <= 0x1p100f)
        return (float)((double)a * (double)b - (double)p);
    return proven_float32_fma(a, b, c);
}
#else
#define proven_cm_fma __builtin_fma
#define proven_cm_fmaf __builtin_fmaf
#endif

/* round, fmax and fmin, inline: they sit on the fast paths, where the compilers leave them as
 * calls to the C library on x86-64 and float_ops.c's are calls too, which cost as much as the
 * work. The results are float_ops.h's: round halves away from zero; fmax and fmin give the other
 * operand for one NaN and the first quieted for two, and put -0 below +0. trunc is one
 * instruction here (SSE4.1, which AVX2 brings; FRINTZ on AArch64). */
static inline double proven_cm_round(double x) {
    const double t = __builtin_trunc(x);                 /* x - t is exact */
    return __builtin_fabs(x - t) >= 0.5 ? t + __builtin_copysign(1.0, x) : t;
}
static inline float proven_cm_roundf(float x) {
    const float t = __builtin_truncf(x);
    return __builtin_fabsf(x - t) >= 0.5f ? t + __builtin_copysignf(1.0f, x) : t;
}
static inline double proven_cm_quiet(double x) {
    union { double d; uint64_t u; } v = { .d = x };
    v.u |= (uint64_t)1 << 51;
    return v.d;
}
static inline float proven_cm_quietf(float x) {
    union { float f; uint32_t u; } v = { .f = x };
    v.u |= (uint32_t)1 << 22;
    return v.f;
}
static inline double proven_cm_fmax(double x, double y) {
    if (x != x) return y != y ? proven_cm_quiet(x) : y;
    if (y != y || x > y) return x;
    if (y > x) return y;
    return __builtin_signbit(x) ? y : x;                 /* equal: +0 above -0 */
}
static inline double proven_cm_fmin(double x, double y) {
    if (x != x) return y != y ? proven_cm_quiet(x) : y;
    if (y != y || x < y) return x;
    if (y < x) return y;
    return __builtin_signbit(x) ? x : y;                 /* equal: -0 below +0 */
}
static inline float proven_cm_fmaxf(float x, float y) {
    if (x != x) return y != y ? proven_cm_quietf(x) : y;
    if (y != y || x > y) return x;
    if (y > x) return y;
    return __builtin_signbit(x) ? y : x;
}
static inline float proven_cm_fminf(float x, float y) {
    if (x != x) return y != y ? proven_cm_quietf(x) : y;
    if (y != y || x < y) return x;
    if (y < x) return y;
    return __builtin_signbit(x) ? x : y;
}

/* The integer path, for the branches CORE-MATH marks as unreachable. */
double proven_cm_pow_fallback_(double x, double y);
double proven_cm_atan2_fallback_(double y, double x);

#endif /* PROVEN_CM_SHIM_H */
