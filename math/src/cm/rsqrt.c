/* Correctly-rounded reciprocal square root of binary64 value.

Copyright (c) 2022-2026 Alexei Sibidanov <sibid@uvic.ca>.

This file is part of the CORE-MATH project
(https://core-math.gitlabpages.inria.fr/).

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

/* proven_math (RFC-0056): ported from CORE-MATH 03948b7170fe, src/binary64/rsqrt/rsqrt.c. Changed: the
 * headers of fenv, errno, stdio, stdlib, math, inttypes and x86intrin are not included
 * (cm_shim.h: rounding to nearest, no flags); x86-64 MXCSR and intrinsic branches take the
 * portable one; unreachable exits return the integer path's result; the sqrt, nan, nanf
 * and ldexp builtins are float_ops.h's functions and round, fmax, fmin cm_shim.h's inline ones
 * (the same bits, no libm); no function is marked cold; nothing is written to signgam;
 * cr_NAME is
 * proven_cm_NAME_; FENV_ACCESS left out on wasm; non-ASCII letters in comments
 * transliterated. Nothing else. */

#include "cm_shim.h"
#if PROVEN_CM


#ifdef CORE_MATH_SUPPORT_ERRNO

#endif

#include <stdint.h>
#if 0 /* proven_math: the portable branch, not MXCSR or intrinsics */
#endif

// Warning: clang also defines __GNUC__
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wunknown-pragmas"
#endif

#if !defined(__wasm__) /* proven_math: no floating-point environment there */
#pragma STDC FENV_ACCESS ON
#endif
// This code emulates the _mm_getcsr SSE intrinsic by reading the FPCR register.
// fegetexceptflag accesses the FPSR register, which seems to be much slower
// than accessing FPCR, so it should be avoided if possible.
// Adapted from sse2neon: https://github.com/DLTcollab/sse2neon
#if 0 /* proven_math: the portable branch, not FPCR */
#if defined(_MSC_VER)
#include <arm64intr.h>
#endif

typedef struct
{
  uint16_t res0;
  uint8_t  res1  : 6;
  uint8_t  bit22 : 1;
  uint8_t  bit23 : 1;
  uint8_t  bit24 : 1;
  uint8_t  res2  : 7;
  uint32_t res3;
} fpcr_bitfield;

inline static unsigned int get_arm_rounding_mode(void)
{
  union
  {
    fpcr_bitfield field;
    uint64_t value;
  } r;

#if defined(_MSC_VER) && !defined(__clang__)
  r.value = _ReadStatusReg(ARM64_FPCR);
#else
  __asm__ __volatile__("mrs %0, FPCR" : "=r"(r.value));
#endif
  static const unsigned int lut[2][2] = {{FE_TONEAREST, FE_UPWARD}, {FE_DOWNWARD, FE_TOWARDZERO}};
  return lut[r.field.bit23][r.field.bit22];
}
#endif  // defined(__arm64__) || defined(_M_ARM64) || defined(__aarch64__)

static inline int get_rounding_mode (void)
{
#if 0 /* proven_math: the portable branch, not MXCSR or intrinsics */
  #if defined(__WIN32__) || defined(__WIN64__)
    // Windows 10 14393 swapped FE_UPWARD and FE_DOWNWARD.
    // Before: FE_UPWARD = 0x0100, FE_DOWNWARD = 0x0200
    // After:  FE_UPWARD = 0x0200, FE_DOWNWARD = 0x0100
    // The amount we need to shift changes depending on the value.
    #if FE_UPWARD == 0x0200
      return _MM_GET_ROUNDING_MODE()>>5;
    #elif FE_UPWARD == 0x0100
      // Lookup table used to eliminate branches.
      static const unsigned lut[4] = {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO};
      return lut[_MM_GET_ROUNDING_MODE()>>13];
    #else
      #warning The floating point rounding constants have an unknown value. A slower path will be taken.
      return fegetround();
    #endif
  #elif defined(__CYGWIN__)
    static const unsigned lut[4] = {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO};
    return lut[_MM_GET_ROUNDING_MODE()>>13];
  #else
    return _MM_GET_ROUNDING_MODE()>>3;
  #endif
#elif 0 /* proven_math: the portable branch, not FPCR */
  return get_arm_rounding_mode();
#else
  return fegetround ();
#endif
}

#if (defined(__clang__) && __clang_major__ >= 14) || (defined(__GNUC__) && __GNUC__ >= 14 && __BITINT_MAXWIDTH__ && __BITINT_MAXWIDTH__ >= 128)
typedef unsigned _BitInt(128) u128;
#else
typedef unsigned __int128 u128;
#endif

typedef uint64_t u64;
typedef int64_t i64;
typedef union {double f; uint64_t u;} b64u64_u;

static double __attribute__((noinline)) as_rsqrt_refine(double rf, double a){
  b64u64_u ir = {.f = rf}, ia = {.f = a};
  if(ia.u < 1ll<<52){
    i64 nz = __builtin_clzll(ia.u);
    ia.u <<= nz - 11;
    ia.u &= ~0ull>>12;
    i64 e = nz - 12;
    ia.u |= e<<52;
  }
  if(ia.u<<11 == 1ull<<63){
  } else {
    unsigned mode = get_rounding_mode ();
    int e = (ia.u>>52)&1;
    u64 rm, am;
    rm = (ir.u<<11|1ull<<63)>>11;
    am = ((ia.u&(~0ull>>12))|1ull<<52)<<(5-e);
    u128 rt = (u128)rm*am;
    u64 rth = rt>>64, rtl = rt;
    u128 rrt = (u128)rtl*rm;
    u64 t0 = rrt, t1 = (rrt>>64) + rth*rm;
    rrt = (u128)t1<<64|t0;
    i64 s = rrt>>127, dd = 1 - 2*s;
    u128 rts = ((rt<<1)^(-s)) + s;
    u128 prrt;
    u64 am2 = am<<1, am20 = -am;
    do {
      ir.u -= dd;
      prrt = rrt;
      am20 += am2;
      u128 tt = rts - am20;
      rrt -= tt;
    } while(__builtin_expect(!((prrt^rrt)>>127), 0));
    ir.u += (rrt>>127)?0:dd;
    rrt = (rrt>>127)?rrt:prrt;
    if(__builtin_expect(mode==FE_TONEAREST, 1)){
      rm = (ir.u<<11|1ull<<63)>>11;
      rt = (u128)rm*am;
      rrt += am>>2;
      rrt += rt;
      u64 inc = rrt>>127;
      ir.u += inc;
    } else {
      ir.u += mode==FE_UPWARD;
    }
    rf = ir.f;
  }
  return rf;
}

double proven_cm_rsqrt_(double x){
  b64u64_u ix = {.f = x};
  double r;
  int e = ix.u>>52;
  if(__builtin_expect(!e, 0)) {
    if (ix.u) // x <> +0
      r = proven_float_sqrt(x)/x;
    else {
#ifdef CORE_MATH_SUPPORT_ERRNO
      errno = ERANGE; // pole error
#endif
      return 1.0 / 0.0; // case x = +0
    }
  } else if(__builtin_expect(e<0x7fd, 1)){
    // common case: 2^-1022 <= x < 2^1022
    r = (1.0 / x) * proven_float_sqrt(x);
  } else if(__builtin_expect(e<0x7ff, 1)){
      r = (4.0 / x) * (0.25 * proven_float_sqrt(x));
  } else {
    u64 ax = ix.u<<1;
    if (!ax) {
#ifdef CORE_MATH_SUPPORT_ERRNO
      errno = ERANGE; // pole error
#endif
      return 1.0 / -0.0; // x=-0
    }
    if (ax > 0x7ffull<<53 ) return x + x; // NaN
    if (e >> 11){ // x < 0
#ifdef CORE_MATH_SUPPORT_ERRNO
      errno = EDOM;
#endif
      feraiseexcept (FE_INVALID);
      return proven_float_from_bits (0x7ff8000000000000u);
    }
    return 0.0; // +Inf
  }
  // use Newton iteration r' = r - r/2*(r^2*x-1)
  double rx = r*x, drx = proven_cm_fma (r, x, -rx);
  // rx + drx approximates r*x with about 106 bits
  double h = proven_cm_fma (r, rx, -1.0) + r * drx, dr = (r * 0.5) * h;
  // h approximates r^2*x-1
  // dr approximates the correction term r/2*(r^2*x-1)
  double rf = r - dr;
  b64u64_u idr = {.f = dr}, ir = {.f = r};
  int de = (ir.u>>52)-((idr.u>>52)&0x7ff);
  u64 d = (idr.u + 18)&~0ull>>13;
  if(__builtin_expect(d<=23||de>100, 0))
    rf = as_rsqrt_refine(rf, x);
  return rf;
}

#ifdef __INTEL_CLANG_COMPILER // rsqrt is called invsqrt with icx
extern double invsqrt (double);
double rsqrt(double x){
  return invsqrt(x);
}
#endif

#include "cm_shim_end.h"
#endif /* PROVEN_CM */
