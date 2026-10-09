#include "proven_internal_crypto.h"

/* Multi-precision modular arithmetic in Montgomery form, over 32-bit limbs so that every
 * product fits a 64-bit word on every target. Loops run over the modulus size only; selections
 * are made with masks. */

static proven_u32 mp_mask_from_bit(proven_u32 bit) { return (proven_u32)0 - (bit & 1u); }

bool proven_crypto_mp_load_be(proven_u32 *out, proven_size_t limbs, const proven_byte_t *be, proven_size_t len) {
    for (proven_size_t i = 0; i < limbs; ++i) out[i] = 0;
    for (proven_size_t i = 0; i < len; ++i) {
        proven_byte_t v = be[len - 1 - i];
        if (i / 4 >= limbs) {
            if (v != 0) return false;
            continue;
        }
        out[i / 4] |= (proven_u32)v << (8 * (i % 4));
    }
    return true;
}

void proven_crypto_mp_store_be(proven_byte_t *out, proven_size_t len, const proven_u32 *a, proven_size_t limbs) {
    for (proven_size_t i = 0; i < len; ++i) {
        proven_size_t limb = i / 4;
        out[len - 1 - i] = limb < limbs ? (proven_byte_t)(a[limb] >> (8 * (i % 4))) : 0;
    }
}

/* a - b into out, returning the borrow (0 or 1). */
static proven_u32 mp_sub_raw(proven_u32 *out, const proven_u32 *a, const proven_u32 *b, proven_size_t limbs) {
    proven_u64 borrow = 0;
    for (proven_size_t i = 0; i < limbs; ++i) {
        proven_u64 d = (proven_u64)a[i] - b[i] - borrow;
        out[i] = (proven_u32)d;
        borrow = (d >> 32) & 1u;
    }
    return (proven_u32)borrow;
}

static proven_u32 mp_add_raw(proven_u32 *out, const proven_u32 *a, const proven_u32 *b, proven_size_t limbs) {
    proven_u64 carry = 0;
    for (proven_size_t i = 0; i < limbs; ++i) {
        proven_u64 s = (proven_u64)a[i] + b[i] + carry;
        out[i] = (proven_u32)s;
        carry = s >> 32;
    }
    return (proven_u32)carry;
}

proven_u32 proven_crypto_mp_lt(const proven_u32 *a, const proven_u32 *b, proven_size_t limbs) {
    proven_u64 borrow = 0;
    for (proven_size_t i = 0; i < limbs; ++i) {
        proven_u64 d = (proven_u64)a[i] - b[i] - borrow;
        borrow = (d >> 32) & 1u;
    }
    return mp_mask_from_bit((proven_u32)borrow);
}

proven_u32 proven_crypto_mp_is_zero(const proven_u32 *a, proven_size_t limbs) {
    proven_u32 acc = 0;
    for (proven_size_t i = 0; i < limbs; ++i) acc |= a[i];
    /* acc | -acc has its top bit set exactly when acc is not zero. */
    return mp_mask_from_bit(((acc | ((proven_u32)0 - acc)) >> 31) ^ 1u);
}

proven_u32 proven_crypto_mp_eq(const proven_u32 *a, const proven_u32 *b, proven_size_t limbs) {
    proven_u32 acc = 0;
    for (proven_size_t i = 0; i < limbs; ++i) acc |= a[i] ^ b[i];
    return mp_mask_from_bit(((acc | ((proven_u32)0 - acc)) >> 31) ^ 1u);
}

void proven_crypto_mp_select(proven_u32 *out, const proven_u32 *a, const proven_u32 *b, proven_u32 mask, proven_size_t limbs) {
    for (proven_size_t i = 0; i < limbs; ++i) out[i] = (a[i] & mask) | (b[i] & ~mask);
}

bool proven_crypto_mp_mod_init(proven_crypto_mp_mod_t *mod, const proven_byte_t *be, proven_size_t len) {
    while (len > 0 && be[0] == 0) { be++; len--; }
    if (len == 0 || len > PROVEN_CRYPTO_MP_MAX * 4) return false;
    proven_size_t limbs = (len + 3) / 4;
    if (!proven_crypto_mp_load_be(mod->n, limbs, be, len)) return false;
    if ((mod->n[0] & 1u) == 0) return false;
    if (limbs == 1 && mod->n[0] < 3) return false;
    mod->limbs = limbs;
    mod->bits = len * 8;
    for (proven_byte_t top = be[0]; (top & 0x80u) == 0; top = (proven_byte_t)(top << 1)) mod->bits--;
    /* Newton's iteration doubles the correct low bits each round: 5 rounds from 3 bits. */
    proven_u32 inv = mod->n[0];
    for (int i = 0; i < 5; ++i) inv *= 2u - mod->n[0] * inv;
    mod->n0inv = (proven_u32)0 - inv;
    /* R^2 mod n by doubling 1 that many times, subtracting n whenever the value reaches it. */
    proven_u32 *r = mod->rr;
    proven_u32 t[PROVEN_CRYPTO_MP_MAX];
    for (proven_size_t i = 0; i < limbs; ++i) r[i] = 0;
    r[0] = 1;
    for (proven_size_t k = 0; k < 2 * 32 * limbs; ++k) {
        proven_u32 carry = mp_add_raw(r, r, r, limbs);
        proven_u32 borrow = mp_sub_raw(t, r, mod->n, limbs);
        /* Keep the difference when the doubling overflowed or did not borrow. */
        proven_crypto_mp_select(r, t, r, mp_mask_from_bit(carry | (borrow ^ 1u)), limbs);
    }
    return true;
}

void proven_crypto_mp_montmul(const proven_crypto_mp_mod_t *mod, proven_u32 *out, const proven_u32 *a, const proven_u32 *b) {
    const proven_size_t s = mod->limbs;
    const proven_u32 *n = mod->n;
    proven_u32 t[PROVEN_CRYPTO_MP_MAX + 2];
    for (proven_size_t i = 0; i < s + 2; ++i) t[i] = 0;
    for (proven_size_t i = 0; i < s; ++i) {
        proven_u64 c = 0;
        const proven_u64 bi = b[i];
        for (proven_size_t j = 0; j < s; ++j) {
            proven_u64 v = (proven_u64)a[j] * bi + t[j] + c;
            t[j] = (proven_u32)v;
            c = v >> 32;
        }
        proven_u64 v = (proven_u64)t[s] + c;
        t[s] = (proven_u32)v;
        t[s + 1] = (proven_u32)(v >> 32);
        const proven_u64 m = (proven_u32)(t[0] * mod->n0inv);
        v = m * n[0] + t[0];
        c = v >> 32;
        for (proven_size_t j = 1; j < s; ++j) {
            v = m * n[j] + t[j] + c;
            t[j - 1] = (proven_u32)v;
            c = v >> 32;
        }
        v = (proven_u64)t[s] + c;
        t[s - 1] = (proven_u32)v;
        t[s] = t[s + 1] + (proven_u32)(v >> 32);
    }
    /* The result is below 2n: one subtraction, chosen by mask. */
    proven_u32 d[PROVEN_CRYPTO_MP_MAX];
    proven_u32 borrow = mp_sub_raw(d, t, n, s);
    proven_crypto_mp_select(out, d, t, mp_mask_from_bit(t[s] | (borrow ^ 1u)), s);
}

void proven_crypto_mp_to_mont(const proven_crypto_mp_mod_t *mod, proven_u32 *out, const proven_u32 *a) {
    proven_crypto_mp_montmul(mod, out, a, mod->rr);
}

void proven_crypto_mp_from_mont(const proven_crypto_mp_mod_t *mod, proven_u32 *out, const proven_u32 *a) {
    proven_u32 one[PROVEN_CRYPTO_MP_MAX];
    for (proven_size_t i = 0; i < mod->limbs; ++i) one[i] = 0;
    one[0] = 1;
    proven_crypto_mp_montmul(mod, out, a, one);
}

void proven_crypto_mp_add(const proven_crypto_mp_mod_t *mod, proven_u32 *out, const proven_u32 *a, const proven_u32 *b) {
    proven_u32 s[PROVEN_CRYPTO_MP_MAX], d[PROVEN_CRYPTO_MP_MAX];
    proven_u32 carry = mp_add_raw(s, a, b, mod->limbs);
    proven_u32 borrow = mp_sub_raw(d, s, mod->n, mod->limbs);
    proven_crypto_mp_select(out, d, s, mp_mask_from_bit(carry | (borrow ^ 1u)), mod->limbs);
}

void proven_crypto_mp_sub(const proven_crypto_mp_mod_t *mod, proven_u32 *out, const proven_u32 *a, const proven_u32 *b) {
    proven_u32 d[PROVEN_CRYPTO_MP_MAX], s[PROVEN_CRYPTO_MP_MAX];
    proven_u32 borrow = mp_sub_raw(d, a, b, mod->limbs);
    (void)mp_add_raw(s, d, mod->n, mod->limbs);
    proven_crypto_mp_select(out, s, d, mp_mask_from_bit(borrow), mod->limbs);
}

void proven_crypto_mp_reduce_be(const proven_crypto_mp_mod_t *mod, proven_u32 *out, const proven_byte_t *be, proven_size_t len) {
    const proven_size_t s = mod->limbs;
    proven_u32 r[PROVEN_CRYPTO_MP_MAX], t[PROVEN_CRYPTO_MP_MAX];
    for (proven_size_t i = 0; i < s; ++i) r[i] = 0;
    for (proven_size_t i = 0; i < len; ++i) {
        for (int bit = 7; bit >= 0; --bit) {
            /* r = 2r + bit, then one subtraction when it reached n. r < n, so 2r + 1 < 2n. */
            proven_u32 carry = 0;
            proven_u32 in = (proven_u32)(be[i] >> bit) & 1u;
            for (proven_size_t j = 0; j < s; ++j) {
                proven_u32 v = r[j];
                r[j] = (v << 1) | in;
                in = v >> 31;
            }
            carry = in;
            proven_u32 borrow = mp_sub_raw(t, r, mod->n, s);
            proven_crypto_mp_select(r, t, r, mp_mask_from_bit(carry | (borrow ^ 1u)), s);
        }
    }
    for (proven_size_t i = 0; i < s; ++i) out[i] = r[i];
}

void proven_crypto_mp_pow(const proven_crypto_mp_mod_t *mod, proven_u32 *out, const proven_u32 *base,
                          const proven_u32 *exp, proven_size_t exp_limbs) {
    const proven_size_t s = mod->limbs;
    proven_u32 acc[PROVEN_CRYPTO_MP_MAX], b[PROVEN_CRYPTO_MP_MAX], one[PROVEN_CRYPTO_MP_MAX];
    for (proven_size_t i = 0; i < s; ++i) { one[i] = 0; b[i] = base[i]; }
    one[0] = 1;
    proven_crypto_mp_to_mont(mod, acc, one);
    for (proven_size_t i = exp_limbs; i-- > 0;) {
        for (int bit = 31; bit >= 0; --bit) {
            proven_crypto_mp_montmul(mod, acc, acc, acc);
            if ((exp[i] >> bit) & 1u) proven_crypto_mp_montmul(mod, acc, acc, b);
        }
    }
    for (proven_size_t i = 0; i < s; ++i) out[i] = acc[i];
}

void proven_crypto_mp_inv_prime(const proven_crypto_mp_mod_t *mod, proven_u32 *out, const proven_u32 *a) {
    /* a^(n-2). The exponent is the public modulus; the base may be secret. */
    proven_u32 e[PROVEN_CRYPTO_MP_MAX], two[PROVEN_CRYPTO_MP_MAX];
    for (proven_size_t i = 0; i < mod->limbs; ++i) two[i] = 0;
    two[0] = 2;
    (void)mp_sub_raw(e, mod->n, two, mod->limbs);
    proven_crypto_mp_pow(mod, out, a, e, mod->limbs);
}
