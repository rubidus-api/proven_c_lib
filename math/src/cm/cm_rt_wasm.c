/*
 * The three 128-bit integer helpers clang calls for CORE-MATH's __int128 arithmetic on wasm
 * (RFC-0056 D-14): multiply, logical shift right, shift left. A wasm toolchain's compiler-rt has
 * them; a -nostdlib link does not. They are weak, so a toolchain's own definitions win where
 * they are linked. Only 64-bit operations and shifts by constants are used here, which clang
 * does not lower to these helpers again. Compiled only where the FPU path is, on wasm.
 */
#include "cm_config.h"

#if PROVEN_CM && defined(__wasm__)
typedef unsigned long long proven_cm_u64;
typedef unsigned __int128 proven_cm_u128;

static proven_cm_u64 cm_lo(proven_cm_u128 x) { return (proven_cm_u64)x; }
static proven_cm_u64 cm_hi(proven_cm_u128 x) { return (proven_cm_u64)(x >> 64); }
static proven_cm_u128 cm_make(proven_cm_u64 hi, proven_cm_u64 lo) { return ((proven_cm_u128)hi << 64) | lo; }

/* The full 128-bit product of two 64-bit numbers, by four 32 x 32 -> 64 multiplies. */
static proven_cm_u128 cm_mul64(proven_cm_u64 a, proven_cm_u64 b) {
    const proven_cm_u64 a0 = a & 0xffffffffu, a1 = a >> 32, b0 = b & 0xffffffffu, b1 = b >> 32;
    const proven_cm_u64 p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    const proven_cm_u64 mid = (p00 >> 32) + (p01 & 0xffffffffu) + (p10 & 0xffffffffu);
    return cm_make(p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32), (mid << 32) | (p00 & 0xffffffffu));
}

__attribute__((weak)) __int128 __multi3(__int128 a, __int128 b);
__attribute__((weak)) __int128 __multi3(__int128 a, __int128 b) {
    const proven_cm_u128 x = (proven_cm_u128)a, y = (proven_cm_u128)b;
    const proven_cm_u128 p = cm_mul64(cm_lo(x), cm_lo(y));
    return (__int128)cm_make(cm_hi(p) + cm_lo(x) * cm_hi(y) + cm_hi(x) * cm_lo(y), cm_lo(p));
}

__attribute__((weak)) proven_cm_u128 __lshrti3(proven_cm_u128 a, int s);
__attribute__((weak)) proven_cm_u128 __lshrti3(proven_cm_u128 a, int s) {
    if (s == 0) return a;
    if (s >= 64) return cm_make(0, cm_hi(a) >> (s - 64));
    return cm_make(cm_hi(a) >> s, (cm_lo(a) >> s) | (cm_hi(a) << (64 - s)));
}

__attribute__((weak)) proven_cm_u128 __ashlti3(proven_cm_u128 a, int s);
__attribute__((weak)) proven_cm_u128 __ashlti3(proven_cm_u128 a, int s) {
    if (s == 0) return a;
    if (s >= 64) return cm_make(cm_lo(a) << (s - 64), 0);
    return cm_make((cm_hi(a) << s) | (cm_lo(a) >> (64 - s)), cm_lo(a) << s);
}
#else
typedef int proven_cm_rt_wasm_unused; /* ISO C: a translation unit is not empty */
#endif
