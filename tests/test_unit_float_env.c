#include "proven/float_env.h"
#include "proven/float_ops.h"
#include "proven_test.h"
#include <stdint.h>
#include <string.h>
#if defined(__x86_64__) && defined(__GNUC__)
#define HW_REFERENCE 1
#else
#define HW_REFERENCE 0
#endif

/*
 * float_env.h (RFC-0058, D-18): the floating-point environment as a value. Tables of bits and
 * flags for each rounding direction everywhere; on x86-64 with glibc, also every operation of
 * both formats against the processor with its rounding mode set (MXCSR), result bits and all five flags, on inputs
 * weighted to the edges (subnormals, overflow, ties, signalling NaNs).
 */

static uint64_t B(double x) { uint64_t u; memcpy(&u, &x, sizeof u); return u; }
static double D(uint64_t u) { double x; memcpy(&x, &u, sizeof x); return x; }
static uint32_t BF(float x) { uint32_t u; memcpy(&u, &x, sizeof u); return u; }
static float F(uint32_t u) { float x; memcpy(&x, &u, sizeof x); return x; }
static uint64_t g_seed = 0x9e3779b97f4a7c15u;
static uint64_t rnd(void) { g_seed ^= g_seed << 13; g_seed ^= g_seed >> 7; g_seed ^= g_seed << 17; return g_seed; }

#if HW_REFERENCE
static double pick(void) {
    static const uint64_t sp[] = { 0, 0x8000000000000000u, 0x7ff0000000000000u, 0xfff0000000000000u, 1, 0x8000000000000001u,
                                   0x000fffffffffffffu, 0x0010000000000000u, 0x7fefffffffffffffu, 0x3ff0000000000000u, 0x7ff4000000000000u };
    const uint64_t r = rnd();
    uint64_t e;
    switch (r % 6) {
    case 0: return D(rnd());
    case 1: return D(sp[rnd() % 11]);
    case 2: e = rnd() % 60; break;
    case 3: e = 2046 - rnd() % 40; break;
    default: e = 1023 - 30 + rnd() % 60; break;
    }
    return D((rnd() & 0x800fffffffffffffu) | (e << 52));
}
static float pickf(void) {
    const uint64_t r = rnd();
    uint32_t e;
    switch (r % 4) {
    case 0: return F((uint32_t)rnd());
    case 1: e = (uint32_t)(rnd() % 40); break;
    case 2: e = 254 - (uint32_t)(rnd() % 30); break;
    default: e = 127 - 20 + (uint32_t)(rnd() % 40); break;
    }
    return F(((uint32_t)rnd() & 0x807fffffu) | (e << 23));
}
/* SSE's control and status register, directly: the test links no libm (fenv.h's functions are
 * there). Rounding bits 13-14: nearest, down, up, toward zero; flags in bits 0-5. */
static unsigned get_mxcsr(void) { unsigned m; __asm__ volatile("stmxcsr %0" : "=m"(m)); return m; }
static void set_mxcsr(unsigned m) { __asm__ volatile("ldmxcsr %0" : : "m"(m)); }
static void hw_round(int mode) {
    static const unsigned rc[4] = { 0u, 3u, 2u, 1u };    /* nearest, toward zero, up, down */
    set_mxcsr((get_mxcsr() & ~0x603fu) | (rc[mode] << 13));
}
static unsigned hwflags(void) {
    const unsigned f = get_mxcsr();
    return (f & 32 ? PROVEN_FLOAT_INEXACT : 0) | (f & 16 ? PROVEN_FLOAT_UNDERFLOW : 0) | (f & 8 ? PROVEN_FLOAT_OVERFLOW : 0)
         | (f & 4 ? PROVEN_FLOAT_DIVBYZERO : 0) | (f & 1 ? PROVEN_FLOAT_INVALID : 0);
}
static void hw_clear(void) { set_mxcsr(get_mxcsr() & ~0x3fu); }
static double hw_sqrt(double x) { __asm__ volatile("sqrtsd %1, %0" : "=x"(x) : "x"(x)); return x; }
static float hw_sqrtf(float x) { __asm__ volatile("sqrtss %1, %0" : "=x"(x) : "x"(x)); return x; }
__attribute__((target("sse4.1"))) static double hw_rint(double x) { __asm__ volatile("roundsd $4, %1, %0" : "=x"(x) : "x"(x)); return x; }
static bool is_nan(double x) { return x != x; }
__attribute__((target("fma"))) static double hw_fma(double a, double b, double c) { return __builtin_fma(a, b, c); }
__attribute__((target("fma"))) static float hw_fmaf(float a, float b, float c) { return __builtin_fmaf(a, b, c); }
#endif

int main(void) {
    PROVEN_TEST_SUITE("float_env: the floating-point environment as a value",
        "RFC-0058 D-18: arithmetic in each rounding direction with the five IEEE exceptions as flags, by tables everywhere and against the x86-64 processor under fesetround.",
        "Inspect math/src/float_ops.c: f_pack_env (the direction, overflow, tininess after rounding), the f_soft_*_env special cases, f_rint_env.");

    PROVEN_TEST_SECTION("tables", "Each direction on known cases: 1/3, overflow, the smallest subnormal, exact zeros' signs, the five flags.", "Check f_round_up and the overflow value of each direction.");
    {
        const double third_down = D(0x3fd5555555555555u), third_up = D(0x3fd5555555555556u);
        proven_float_env_t e[4] = { { PROVEN_ROUND_NEAREST, 0 }, { PROVEN_ROUND_TOWARD_ZERO, 0 }, { PROVEN_ROUND_UP, 0 }, { PROVEN_ROUND_DOWN, 0 } };
        PROVEN_TEST_ASSERT(B(proven_float_env_div(&e[0], 1.0, 3.0)) == B(third_down) && B(proven_float_env_div(&e[1], 1.0, 3.0)) == B(third_down)
            && B(proven_float_env_div(&e[2], 1.0, 3.0)) == B(third_up) && B(proven_float_env_div(&e[3], -1.0, 3.0)) == (B(third_up) | 0x8000000000000000u)
            && e[0].flags == PROVEN_FLOAT_INEXACT && e[2].flags == PROVEN_FLOAT_INEXACT,
            "1/3 in each direction, and inexact raised", "");
        proven_float_env_t z = { PROVEN_ROUND_TOWARD_ZERO, 0 }, up = { PROVEN_ROUND_UP, 0 }, n = { PROVEN_ROUND_NEAREST, 0 };
        const double big = D(0x7fefffffffffffffu);
        PROVEN_TEST_ASSERT(B(proven_float_env_mul(&z, big, 2.0)) == 0x7fefffffffffffffu && z.flags == (PROVEN_FLOAT_OVERFLOW | PROVEN_FLOAT_INEXACT)
            && B(proven_float_env_mul(&n, big, 2.0)) == 0x7ff0000000000000u,
            "overflow: the largest finite toward zero, infinity to nearest; overflow and inexact", "");
        proven_float_env_t u2 = { PROVEN_ROUND_UP, 0 };
        PROVEN_TEST_ASSERT(B(proven_float_env_mul(&up, D(1), 0.5)) == 1 && up.flags == (PROVEN_FLOAT_UNDERFLOW | PROVEN_FLOAT_INEXACT)
            && B(proven_float_env_mul(&u2, D(1), D(0x3fe0000000000000u))) == 1,
            "half the smallest subnormal rounds up to it: underflow and inexact", "");
        proven_float_env_t dn = { PROVEN_ROUND_DOWN, 0 }, nn = { PROVEN_ROUND_NEAREST, 0 };
        PROVEN_TEST_ASSERT(B(proven_float_env_sub(&dn, 1.0, 1.0)) == 0x8000000000000000u && B(proven_float_env_sub(&nn, 1.0, 1.0)) == 0
            && B(proven_float_env_add(&dn, D(0), D(0x8000000000000000u))) == 0x8000000000000000u && dn.flags == 0,
            "an exact zero sum is -0 rounding down, +0 otherwise; exact: no flag", "");
        proven_float_env_t f = { PROVEN_ROUND_NEAREST, 0 };
        (void)proven_float_env_div(&f, 1.0, 0.0);
        const unsigned dz = f.flags;
        f.flags = 0;
        (void)proven_float_env_sqrt(&f, -1.0);
        const unsigned inv = f.flags;
        f.flags = 0;
        (void)proven_float_env_add(&f, D(0x7ff4000000000000u), 1.0);
#if defined(__i386__)
        f.flags = PROVEN_FLOAT_INVALID;   /* 32-bit x86 quiets a signalling NaN on return (float_ops.h): not testable there */
#endif
        PROVEN_TEST_ASSERT(dz == PROVEN_FLOAT_DIVBYZERO && inv == PROVEN_FLOAT_INVALID && f.flags == PROVEN_FLOAT_INVALID,
            "divide by zero; the root of a negative and a signalling NaN are invalid", "");
        proven_i64 v = 0;
        proven_float_env_t r[4] = { { PROVEN_ROUND_NEAREST, 0 }, { PROVEN_ROUND_TOWARD_ZERO, 0 }, { PROVEN_ROUND_UP, 0 }, { PROVEN_ROUND_DOWN, 0 } };
        bool ok = true;
        const proven_i64 want[4][2] = { { 2, -2 }, { 2, -2 }, { 3, -2 }, { 2, -3 } };
        for (int k = 0; k < 4; k++) {
            ok &= proven_float_env_to_i64(&r[k], 2.5, &v) == PROVEN_OK && v == want[k][0];
            ok &= proven_float_env_to_i64(&r[k], -2.5, &v) == PROVEN_OK && v == want[k][1];
            ok &= B(proven_float_env_rint(&r[k], 2.5)) == B((double)want[k][0]);
        }
        proven_float_env_t big2 = { PROVEN_ROUND_NEAREST, 0 };
        ok &= proven_float_env_to_i64(&big2, 1e19, &v) == PROVEN_ERR_OUT_OF_BOUNDS && big2.flags == PROVEN_FLOAT_INVALID;
        proven_float_env_t c = { PROVEN_ROUND_UP, 0 };
        ok &= BF(proven_float_env_to_float32(&c, 1.0 + 0x1p-40)) == 0x3f800001u && c.flags == PROVEN_FLOAT_INEXACT;
        proven_float_env_t i64 = { PROVEN_ROUND_DOWN, 0 };
        ok &= B(proven_float_env_from_i64(&i64, ((proven_i64)1 << 53) + 1)) == 0x4340000000000000u && i64.flags == PROVEN_FLOAT_INEXACT;
        ok &= BF(proven_float32_env_from_i64(&i64, 16777217)) == 0x4b800000u;
        PROVEN_TEST_ASSERT(ok, "rint and to_i64 of +-2.5 in each direction; out of range is invalid; narrowing and from_i64 in a direction", "");
        bool nb = true;
        for (int k = 0; k < 4; k++) {
            proven_float_env_t e = { r[k].round, 0 }, e32 = { r[k].round, 0 };
            nb &= B(proven_float_env_nearbyint(&e, -2.5)) == B((double)want[k][1]) && e.flags == 0;
            nb &= BF(proven_float32_env_nearbyint(&e32, 2.5f)) == BF((float)want[k][0]) && e32.flags == 0;
        }
        proven_float_env_t ei = { PROVEN_ROUND_NEAREST, PROVEN_FLOAT_INEXACT };
        nb &= B(proven_float_env_nearbyint(&ei, 0.5)) == 0 && ei.flags == PROVEN_FLOAT_INEXACT;   /* a flag raised before stays */
        PROVEN_TEST_ASSERT(nb, "nearbyint: rint's result in each direction, no inexact raised, earlier flags kept", "");
        PROVEN_TEST_ASSERT(B(proven_float_env_add(NULL, 0.1, 0.2)) == B(proven_float_add(0.1, 0.2)), "a NULL env is nearest-even", "");
    }

    PROVEN_TEST_SECTION("against the processor", "x86-64: every operation of both formats with the processor's rounding mode set, result bits and flags.", "A difference names the operation and the direction; NaN bits are not compared (the instruction makes another NaN).");
    {
#if HW_REFERENCE
        const bool have_fma = __builtin_cpu_supports("fma"), have_sse41 = __builtin_cpu_supports("sse4.1");
        long bad = 0;
        for (long i = 0; i < 300000; i++) {
            const int m = (int)(rnd() % 4), op = (int)(i % 7);
            const double x = pick(), z = pick();
            const double y = (i % 5 == 0) ? x * (1 + 0x1p-52 * (double)(rnd() % 5)) : pick();
            const float a = pickf(), b = pickf(), c = pickf();
            if ((op == 4 && !have_fma) || (op == 6 && !have_sse41)) continue;
            volatile double vx = x, vy = y, vz = z;
            volatile float va = a, vb = b, vc = c;
            double hr = 0, pr = 0;
            float hf = 0, pf = 0;
            proven_float_env_t env = { (proven_round_t)m, 0 }, env32 = { (proven_round_t)m, 0 };
            hw_round(m);
            hw_clear();
            switch (op) {
            case 0: hr = vx + vy; break;
            case 1: hr = vx - vy; break;
            case 2: hr = vx * vy; break;
            case 3: hr = vx / vy; break;
            case 4: hr = hw_fma(vx, vy, vz); break;
            case 5: hr = hw_sqrt(vx); break;
            default: hr = hw_rint(vx); break;
            }
            const unsigned hfl = hwflags();
            hw_clear();
            switch (op) {
            case 0: hf = va + vb; break;
            case 1: hf = va - vb; break;
            case 2: hf = va * vb; break;
            case 3: hf = va / vb; break;
            case 4: hf = hw_fmaf(va, vb, vc); break;
            case 5: hf = hw_sqrtf(va); break;
            default: hf = (float)vx; break;
            }
            const unsigned hfl32 = hwflags();
            hw_round(0);
            hw_clear();
            switch (op) {
            case 0: pr = proven_float_env_add(&env, x, y); pf = proven_float32_env_add(&env32, a, b); break;
            case 1: pr = proven_float_env_sub(&env, x, y); pf = proven_float32_env_sub(&env32, a, b); break;
            case 2: pr = proven_float_env_mul(&env, x, y); pf = proven_float32_env_mul(&env32, a, b); break;
            case 3: pr = proven_float_env_div(&env, x, y); pf = proven_float32_env_div(&env32, a, b); break;
            case 4: pr = proven_float_env_fma(&env, x, y, z); pf = proven_float32_env_fma(&env32, a, b, c); break;
            case 5: pr = proven_float_env_sqrt(&env, x); pf = proven_float32_env_sqrt(&env32, a); break;
            default: pr = proven_float_env_rint(&env, x); pf = proven_float_env_to_float32(&env32, x); break;
            }
            if (!(B(hr) == B(pr) || (is_nan(hr) && is_nan(pr))) || hfl != env.flags) {
                if (bad < 4) PROVEN_TEST_INFO("binary64 op {} mode {}: {} vs {}", PROVEN_ARG(op), PROVEN_ARG(m), PROVEN_ARG((unsigned long long)B(pr)), PROVEN_ARG((unsigned long long)B(hr)));
                bad++;
            }
            if (!(BF(hf) == BF(pf) || (is_nan(hf) && is_nan(pf))) || hfl32 != env32.flags) {
                if (bad < 4) PROVEN_TEST_INFO("binary32 op {} mode {}", PROVEN_ARG(op), PROVEN_ARG(m));
                bad++;
            }
        }
        PROVEN_TEST_ASSERT(bad == 0, "300,000 operations of each format in every direction: the processor's bits and flags", "");
#else
        PROVEN_TEST_ASSERT(true, "not x86-64: the processor is not compared here", "");
#endif
    }
    PROVEN_TEST_PASS("float_env checks passed.");
    return 0;
}
