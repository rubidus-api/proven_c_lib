#ifndef PROVEN_TEST_UNIT_COMPLEX_SWEEP_H
#define PROVEN_TEST_UNIT_COMPLEX_SWEEP_H

/* For tests/test_unit_complex.c and the private wasm check (ops/checks/complex/wasm): a hash
 * of each complex.h function's result bits over 400 fixed inputs of every kind, both formats.
 * Pure computation - complex.h, float_ops.h and stdint.h only - so a freestanding module can
 * compute it too. Index: the 15 functions of CXS_FN, then mul, div, pow, abs, arg. */

#include "proven/complex.h"
#include "proven/float_ops.h"
#include <stdint.h>

static proven_complex_t (*const CXS_FN[15])(proven_complex_t) = {
    proven_complex_exp, proven_complex_log, proven_complex_sqrt, proven_complex_sin, proven_complex_cos,
    proven_complex_tan, proven_complex_asin, proven_complex_acos, proven_complex_atan, proven_complex_sinh,
    proven_complex_cosh, proven_complex_tanh, proven_complex_asinh, proven_complex_acosh, proven_complex_atanh };
static proven_complex32_t (*const CXS_FNF[15])(proven_complex32_t) = {
    proven_complex32_exp, proven_complex32_log, proven_complex32_sqrt, proven_complex32_sin, proven_complex32_cos,
    proven_complex32_tan, proven_complex32_asin, proven_complex32_acos, proven_complex32_atan, proven_complex32_sinh,
    proven_complex32_cosh, proven_complex32_tanh, proven_complex32_asinh, proven_complex32_acosh, proven_complex32_atanh };

static uint64_t cxs_seed;
static uint64_t cxs_rnd(void) { cxs_seed ^= cxs_seed << 13; cxs_seed ^= cxs_seed >> 7; cxs_seed ^= cxs_seed << 17; return cxs_seed; }
/* one part: of every kind - the specials, subnormals, the extremes, around 1, and moderate */
static uint64_t cxs_part(void) {
    static const uint64_t sp[] = { 0, 0x8000000000000000u, 0x7ff0000000000000u, 0xfff0000000000000u, 0x7ff8000000000000u,
                                   0x3ff0000000000000u, 0xbff0000000000000u, 1, 0x7fefffffffffffffu, 0x0010000000000000u,
                                   0x3ff8000000000000u };
    const uint64_t r = cxs_rnd();
    switch (r % 8) {
    case 0: return sp[(r >> 8) % 11];
    case 1: return (cxs_rnd() & 0x800fffffffffffffu) | ((1023 - 2 + cxs_rnd() % 4) << 52);
    case 2: return (cxs_rnd() & 0x800fffffffffffffu) | ((cxs_rnd() % 2047) << 52);
    default: return (cxs_rnd() & 0x800fffffffffffffu) | ((1023 - 30 + cxs_rnd() % 60) << 52);
    }
}
static uint64_t cxs_fnv(uint64_t h, uint64_t v) { for (int k = 0; k < 8; k++) { h ^= (v >> (8 * k)) & 0xff; h *= 0x100000001b3u; } return h; }

static uint64_t cxs_hash(int s, int f) {
    cxs_seed = 0x2545f4914f6cdd1du ^ (uint64_t)(f * 2 + s + 1);
    uint64_t h = 0xcbf29ce484222325u;
    for (int i = 0; i < 400; i++) {
        const uint64_t a = cxs_part(), b = cxs_part(), c = cxs_part(), d = cxs_part();
        const double ar = proven_float_from_bits(a), br = proven_float_from_bits(b), cr = proven_float_from_bits(c), dr = proven_float_from_bits(d);
        if (!s) {
            const proven_complex_t z = proven_complex(ar, br), w = proven_complex(cr, dr);
            proven_complex_t r = { 0, 0 };
            if (f < 15) r = CXS_FN[f](z);
            else if (f == 15) r = proven_complex_mul(z, w);
            else if (f == 16) r = proven_complex_div(z, w);
            else if (f == 17) r = proven_complex_pow(z, proven_complex(proven_float_from_i64((proven_i64)(c % 7) - 3), proven_float_ldexp(proven_float_from_i64((proven_i64)(d % 5)), -2)));
            else if (f == 18) r.re = proven_complex_abs(z);
            else r.re = proven_complex_arg(z);
            h = cxs_fnv(cxs_fnv(h, proven_float_to_bits(r.re)), proven_float_to_bits(r.im));
        } else {
            const proven_complex32_t z = proven_complex32(proven_float_to_float32(ar), proven_float_to_float32(br));
            const proven_complex32_t w = proven_complex32(proven_float_to_float32(cr), proven_float_to_float32(dr));
            proven_complex32_t r = { 0, 0 };
            if (f < 15) r = CXS_FNF[f](z);
            else if (f == 15) r = proven_complex32_mul(z, w);
            else if (f == 16) r = proven_complex32_div(z, w);
            else if (f == 17) r = proven_complex32_pow(z, proven_complex32(proven_float_to_float32(proven_float_from_i64((proven_i64)(c % 7) - 3)),
                                                                           proven_float_to_float32(proven_float_ldexp(proven_float_from_i64((proven_i64)(d % 5)), -2))));
            else if (f == 18) r.re = proven_complex32_abs(z);
            else r.re = proven_complex32_arg(z);
            h = cxs_fnv(cxs_fnv(h, proven_float32_to_bits(r.re)), proven_float32_to_bits(r.im));
        }
    }
    return h;
}

#endif
