#ifndef PROVEN_CM_SHIM_H
#define PROVEN_CM_SHIM_H

/*
 * What the CORE-MATH sources of math/src/cm take from the C library, for proven_math
 * (RFC-0056), and whether they are compiled at all.
 *
 * PROVEN_CM is 1 where the FPU path is taken: x86-64 and AArch64, little-endian, with a 128-bit
 * integer type, evaluating at each type's own precision, not under fast-math, and not with
 * PROVEN_FLOAT_SOFT or PROVEN_FLOAT_MATH_SMALL. On x86-64 the code is compiled for FMA and AVX2
 * (CORE-MATH's fast paths are written for fused multiply-add) and float_math.c calls it only
 * when the processor has them, or always when the build itself targets them; elsewhere the
 * integer path answers, with the same bits.
 *
 * The rounding mode is to nearest (RFC-0055 D-7), and no floating-point flag is raised or read
 * (no flags): CORE-MATH uses the flags only to report them, never to choose a result.
 * Contraction is off, as CORE-MATH's error analyses assume.
 */

#include "proven/math_config.h"
#include "proven/float_ops.h"
#include <stdint.h>

#include "cm_config.h"

#if PROVEN_CM
#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#if defined(__x86_64__) && !(defined(__FMA__) && defined(__AVX2__))
#pragma clang attribute push (__attribute__((target("fma,avx2"))), apply_to = function)
#define PROVEN_CM_PUSHED 1
#endif
#else
#pragma GCC optimize ("fp-contract=off")
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
#else
#define proven_cm_fma __builtin_fma
#define proven_cm_fmaf __builtin_fmaf
#endif

/* The integer path, for the branches CORE-MATH marks as unreachable. */
double proven_cm_pow_fallback_(double x, double y);
double proven_cm_atan2_fallback_(double y, double x);

#endif /* PROVEN_CM_SHIM_H */
