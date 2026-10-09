#include "proven_internal_crypto.h"
#include "proven/hash.h"

/* Curve25519: the field, X25519 and Ed25519.
 *
 * A field element is ten limbs of alternately 26 and 25 bits (radix 2^25.5), so that every
 * product of two limbs, and the sum of the ten such products in a column, fits a 64-bit word
 * on every target. All limbs are unsigned; subtraction adds a multiple of p first. Nothing
 * here branches on, or indexes by, a field element or a scalar. */

typedef proven_u32 fe_t[10];

static const fe_t FE_D = { 0x35978a3, 0x0d37284, 0x3156ebd, 0x06a0a0e, 0x001c029, 0x179e898, 0x3a03cbb, 0x1ce7198, 0x2e2b6ff, 0x1480db3 };
static const fe_t FE_D2 = { 0x2b2f159, 0x1a6e509, 0x22add7a, 0x0d4141d, 0x0038052, 0x0f3d130, 0x3407977, 0x19ce331, 0x1c56dff, 0x0901b67 };
static const fe_t FE_SQRTM1 = { 0x20ea0b0, 0x186c9d2, 0x08f189d, 0x035697f, 0x0bd0c60, 0x1fbd7a7, 0x2804c9e, 0x1e16569, 0x004fc1d, 0x0ae0c92 };
static const fe_t ED_BX = { 0x325d51a, 0x18b5823, 0x0f6592a, 0x104a92d, 0x1a4b31d, 0x1d6dc5c, 0x27118fe, 0x07fd814, 0x13cd6e5, 0x085a4db };
static const fe_t ED_BY = { 0x2666658, 0x1999999, 0x0cccccc, 0x1333333, 0x1999999, 0x0666666, 0x3333333, 0x0cccccc, 0x2666666, 0x1999999 };

/* The group order L = 2^252 + 27742317777372353535851937790883648493, big-endian. */
static const proven_byte_t ED_L[32] = {
    0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x14, 0xde, 0xf9, 0xde, 0xa2, 0xf7, 0x9c, 0xd6, 0x58, 0x12, 0x63, 0x1a, 0x5c, 0xf5, 0xd3, 0xed };

#define FE_WIDTH(i) (((i) & 1) ? 25 : 26)
#define FE_MASK(i) (((i) & 1) ? 0x1ffffffu : 0x3ffffffu)

static void fe_copy(fe_t h, const fe_t f) { for (int i = 0; i < 10; ++i) h[i] = f[i]; }
static void fe_zero(fe_t h) { for (int i = 0; i < 10; ++i) h[i] = 0; }
static void fe_one(fe_t h) { fe_zero(h); h[0] = 1; }

static void fe_carry(fe_t h) {
    proven_u32 c;
    for (int i = 0; i < 9; ++i) { c = h[i] >> FE_WIDTH(i); h[i] &= FE_MASK(i); h[i + 1] += c; }
    c = h[9] >> 25; h[9] &= 0x1ffffffu; h[0] += 19 * c;
    c = h[0] >> 26; h[0] &= 0x3ffffffu; h[1] += c;
}

static void fe_add(fe_t h, const fe_t f, const fe_t g) {
    for (int i = 0; i < 10; ++i) h[i] = f[i] + g[i];
    fe_carry(h);
}

static void fe_sub(fe_t h, const fe_t f, const fe_t g) {
    /* f + 2p - g: every limb of 2p is at least as large as a carried limb of g. */
    h[0] = f[0] + 0x7ffffdau - g[0];
    for (int i = 1; i < 10; ++i) h[i] = f[i] + ((i & 1) ? 0x3fffffeu : 0x7fffffeu) - g[i];
    fe_carry(h);
}

static void fe_reduce_wide(fe_t h, proven_u64 t[19]) {
    for (int k = 18; k >= 10; --k) t[k - 10] += 19 * t[k];
    proven_u64 c;
    for (int i = 0; i < 9; ++i) { c = t[i] >> FE_WIDTH(i); t[i] &= FE_MASK(i); t[i + 1] += c; }
    c = t[9] >> 25; t[9] &= 0x1ffffffu; t[0] += 19 * c;
    c = t[0] >> 26; t[0] &= 0x3ffffffu; t[1] += c;
    for (int i = 0; i < 10; ++i) h[i] = (proven_u32)t[i];
}

static void fe_mul(fe_t h, const fe_t f, const fe_t g) {
    proven_u64 t[19];
    for (int k = 0; k < 19; ++k) t[k] = 0;
    for (int i = 0; i < 10; ++i) {
        for (int j = 0; j < 10; ++j) {
            /* Limb i sits at bit ceil(25.5 i): two odd limbs meet one bit above their column. */
            proven_u64 m = (proven_u64)f[i] * g[j];
            t[i + j] += m << (i & j & 1);
        }
    }
    fe_reduce_wide(h, t);
}

static void fe_sq(fe_t h, const fe_t f) { fe_mul(h, f, f); }

static void fe_sqn(fe_t h, const fe_t f, int n) {
    fe_sq(h, f);
    for (int i = 1; i < n; ++i) fe_sq(h, h);
}

static void fe_mul_small(fe_t h, const fe_t f, proven_u32 k) {
    proven_u64 t[19];
    for (int i = 0; i < 10; ++i) t[i] = (proven_u64)f[i] * k;
    for (int i = 10; i < 19; ++i) t[i] = 0;
    fe_reduce_wide(h, t);
}

/* z^(2^250 - 1) into t250, z^11 into t11: the shared front of the two exponentiations. */
static void fe_pow_front(fe_t t250, fe_t t11, const fe_t z) {
    fe_t t0, t1, t2, t3;
    fe_sq(t0, z);                         /* 2 */
    fe_sqn(t1, t0, 2);                    /* 8 */
    fe_mul(t1, z, t1);                    /* 9 */
    fe_mul(t0, t0, t1);                   /* 11 */
    fe_copy(t11, t0);
    fe_sq(t2, t0);                        /* 22 */
    fe_mul(t1, t1, t2);                   /* 2^5 - 1 */
    fe_sqn(t2, t1, 5); fe_mul(t1, t2, t1);    /* 2^10 - 1 */
    fe_sqn(t2, t1, 10); fe_mul(t2, t2, t1);   /* 2^20 - 1 */
    fe_sqn(t3, t2, 20); fe_mul(t2, t3, t2);   /* 2^40 - 1 */
    fe_sqn(t2, t2, 10); fe_mul(t1, t2, t1);   /* 2^50 - 1 */
    fe_sqn(t2, t1, 50); fe_mul(t2, t2, t1);   /* 2^100 - 1 */
    fe_sqn(t3, t2, 100); fe_mul(t2, t3, t2);  /* 2^200 - 1 */
    fe_sqn(t2, t2, 50); fe_mul(t250, t2, t1); /* 2^250 - 1 */
}

static void fe_invert(fe_t out, const fe_t z) {
    fe_t t, t11;
    fe_pow_front(t, t11, z);
    fe_sqn(t, t, 5);                      /* 2^255 - 32 */
    fe_mul(out, t, t11);                  /* 2^255 - 21 = p - 2 */
}

static void fe_pow22523(fe_t out, const fe_t z) {
    fe_t t, t11;
    fe_pow_front(t, t11, z);
    fe_sqn(t, t, 2);                      /* 2^252 - 4 */
    fe_mul(out, t, z);                    /* 2^252 - 3 = (p - 5) / 8 */
}

static void fe_frombytes(fe_t h, const proven_byte_t s[32]) {
    int off = 0;
    for (int i = 0; i < 10; ++i) {
        proven_u64 v = 0;
        for (int b = 0; b < 5; ++b) {
            int at = off / 8 + b;
            if (at < 32) v |= (proven_u64)s[at] << (8 * b);
        }
        h[i] = (proven_u32)(v >> (off % 8)) & FE_MASK(i);
        off += FE_WIDTH(i);
    }
}

static void fe_tobytes(proven_byte_t s[32], const fe_t f) {
    fe_t h;
    fe_copy(h, f);
    fe_carry(h);
    /* q = floor((h + 19) / 2^255): 1 exactly when h >= p. Then h + 19q - q 2^255 is h mod p. */
    proven_u32 q = 19;
    for (int i = 0; i < 10; ++i) q = (h[i] + q) >> FE_WIDTH(i);
    h[0] += 19 * q;
    for (int i = 0; i < 9; ++i) { proven_u32 c = h[i] >> FE_WIDTH(i); h[i] &= FE_MASK(i); h[i + 1] += c; }
    h[9] &= 0x1ffffffu;
    proven_u64 acc = 0;
    int bits = 0, at = 0;
    for (int i = 0; i < 10; ++i) {
        acc |= (proven_u64)h[i] << bits;
        bits += FE_WIDTH(i);
        while (bits >= 8) { s[at++] = (proven_byte_t)acc; acc >>= 8; bits -= 8; }
    }
    s[at] = (proven_byte_t)acc;           /* the last 7 bits */
}

/* Swap f and g when bit is 1, by mask. */
static void fe_cswap(fe_t f, fe_t g, proven_u32 bit) {
    proven_u32 mask = (proven_u32)0 - bit;
    for (int i = 0; i < 10; ++i) { proven_u32 x = (f[i] ^ g[i]) & mask; f[i] ^= x; g[i] ^= x; }
}

static bool fe_equal(const fe_t f, const fe_t g) {
    proven_byte_t a[32], b[32];
    fe_tobytes(a, f); fe_tobytes(b, g);
    return proven_mem_equal_ct((proven_mem_view_t){ .ptr = a, .size = 32 }, (proven_mem_view_t){ .ptr = b, .size = 32 });
}

static void wipe(void *p, proven_size_t n) { proven_mem_wipe((proven_mem_mut_t){ .ptr = p, .size = n }); }

/* ---- X25519 ---- */

bool proven_crypto_x25519(proven_byte_t out[32], const proven_byte_t scalar[32], const proven_byte_t u[32]) {
    proven_byte_t k[32];
    for (int i = 0; i < 32; ++i) k[i] = scalar[i];
    k[0] &= 248; k[31] &= 127; k[31] |= 64;
    fe_t x1, x2, z2, x3, z3, a, aa, b, bb, e, c, d, da, cb, t;
    fe_frombytes(x1, u);
    fe_one(x2); fe_zero(z2); fe_copy(x3, x1); fe_one(z3);
    proven_u32 swap = 0;
    for (int pos = 254; pos >= 0; --pos) {
        proven_u32 bit = (proven_u32)(k[pos / 8] >> (pos % 8)) & 1u;
        swap ^= bit;
        fe_cswap(x2, x3, swap); fe_cswap(z2, z3, swap);
        swap = bit;
        fe_add(a, x2, z2); fe_sq(aa, a);
        fe_sub(b, x2, z2); fe_sq(bb, b);
        fe_sub(e, aa, bb);
        fe_add(c, x3, z3); fe_sub(d, x3, z3);
        fe_mul(da, d, a); fe_mul(cb, c, b);
        fe_add(t, da, cb); fe_sq(x3, t);
        fe_sub(t, da, cb); fe_sq(t, t); fe_mul(z3, x1, t);
        fe_mul(x2, aa, bb);
        fe_mul_small(t, e, 121665); fe_add(t, aa, t); fe_mul(z2, e, t);
    }
    fe_cswap(x2, x3, swap); fe_cswap(z2, z3, swap);
    fe_invert(z2, z2);
    fe_mul(x2, x2, z2);
    fe_tobytes(out, x2);
    proven_byte_t acc = 0;
    for (int i = 0; i < 32; ++i) acc |= out[i];
    wipe(k, sizeof k); wipe(x2, sizeof(fe_t)); wipe(z2, sizeof(fe_t)); wipe(x3, sizeof(fe_t)); wipe(z3, sizeof(fe_t));
    wipe(a, sizeof(fe_t)); wipe(b, sizeof(fe_t)); wipe(aa, sizeof(fe_t)); wipe(bb, sizeof(fe_t)); wipe(e, sizeof(fe_t));
    wipe(c, sizeof(fe_t)); wipe(d, sizeof(fe_t)); wipe(da, sizeof(fe_t)); wipe(cb, sizeof(fe_t)); wipe(t, sizeof(fe_t));
    /* Whether the shared value is zero is revealed, as it must be: the exchange is refused. */
    bool nonzero = acc != 0;
    PROVEN_CT_PUBLIC(&nonzero, sizeof nonzero);
    return nonzero;
}

void proven_crypto_x25519_public(proven_byte_t out[32], const proven_byte_t scalar[32]) {
    proven_byte_t base[32] = { 9 };
    (void)proven_crypto_x25519(out, scalar, base);     /* never zero for the base point */
}

/* ---- Ed25519 ---- */

/* Extended coordinates on -x^2 + y^2 = 1 + d x^2 y^2: x = X/Z, y = Y/Z, T = XY/Z. The addition
 * below is complete on this curve: it is right for every pair of points. */
typedef struct { fe_t x, y, z, t; } ge_t;

static void ge_identity(ge_t *p) { fe_zero(p->x); fe_one(p->y); fe_one(p->z); fe_zero(p->t); }

static void ge_add(ge_t *r, const ge_t *p, const ge_t *q) {
    fe_t a, b, c, d, e, f, g, h, t;
    fe_sub(a, p->y, p->x); fe_sub(t, q->y, q->x); fe_mul(a, a, t);
    fe_add(b, p->y, p->x); fe_add(t, q->y, q->x); fe_mul(b, b, t);
    fe_mul(c, p->t, q->t); fe_mul(c, c, FE_D2);
    fe_mul(d, p->z, q->z); fe_add(d, d, d);
    fe_sub(e, b, a); fe_sub(f, d, c); fe_add(g, d, c); fe_add(h, b, a);
    fe_mul(r->x, e, f); fe_mul(r->y, g, h); fe_mul(r->t, e, h); fe_mul(r->z, f, g);
}

static void ge_double(ge_t *r, const ge_t *p) {
    fe_t a, b, c, e, f, g, h, t;
    fe_sq(a, p->x); fe_sq(b, p->y);
    fe_sq(c, p->z); fe_add(c, c, c);
    fe_add(t, p->x, p->y); fe_sq(e, t); fe_sub(e, e, a); fe_sub(e, e, b);
    fe_sub(g, b, a);
    fe_sub(f, g, c);
    fe_zero(t); fe_sub(h, t, a); fe_sub(h, h, b);
    fe_mul(r->x, e, f); fe_mul(r->y, g, h); fe_mul(r->t, e, h); fe_mul(r->z, f, g);
}

/* r = k * p for a 256-bit little-endian scalar (SECRET): a fixed window of four bits, every
 * table entry read on every lookup. */
static void ge_mul(ge_t *r, const proven_byte_t k[32], const ge_t *p) {
    ge_t table[16], acc, pick;
    ge_identity(&table[0]);
    table[1] = *p;
    for (int i = 2; i < 16; ++i) ge_add(&table[i], &table[i - 1], p);
    ge_identity(&acc);
    for (int i = 63; i >= 0; --i) {
        for (int d = 0; d < 4; ++d) ge_double(&acc, &acc);
        proven_u32 nib = (proven_u32)(k[i / 2] >> (4 * (i % 2))) & 15u;
        proven_u32 *dst = (proven_u32 *)&pick;
        for (proven_size_t j = 0; j < 40; ++j) dst[j] = 0;
        for (proven_u32 t = 0; t < 16; ++t) {
            proven_u32 x = t ^ nib;
            proven_u32 mask = ((x | ((proven_u32)0 - x)) >> 31) - 1u;
            const proven_u32 *src = (const proven_u32 *)&table[t];
            for (proven_size_t j = 0; j < 40; ++j) dst[j] |= src[j] & mask;
        }
        ge_add(&acc, &acc, &pick);
    }
    *r = acc;
    wipe(table, sizeof table); wipe(&acc, sizeof acc); wipe(&pick, sizeof pick);
}

static void ge_encode(proven_byte_t s[32], const ge_t *p) {
    fe_t zi, x, y;
    proven_byte_t xb[32];
    fe_invert(zi, p->z);
    fe_mul(x, p->x, zi); fe_mul(y, p->y, zi);
    fe_tobytes(s, y);
    fe_tobytes(xb, x);
    s[31] ^= (proven_byte_t)((xb[0] & 1) << 7);
    wipe(zi, sizeof zi); wipe(x, sizeof x); wipe(y, sizeof y); wipe(xb, sizeof xb);
}

/* A point from its encoding. Public data only. Refuses a y that is not below p, a y with no
 * x on the curve, and x = 0 with the sign bit set. */
static bool ge_decode(ge_t *p, const proven_byte_t s[32]) {
    proven_byte_t yb[32], back[32];
    for (int i = 0; i < 32; ++i) yb[i] = s[i];
    proven_u32 sign = yb[31] >> 7;
    yb[31] &= 127;
    fe_frombytes(p->y, yb);
    fe_tobytes(back, p->y);
    for (int i = 0; i < 32; ++i) if (back[i] != yb[i]) return false;
    fe_t u, v, v3, x, vxx, one, t;
    fe_one(one); fe_one(p->z);
    fe_sq(u, p->y); fe_mul(v, u, FE_D);
    fe_sub(u, u, one);                     /* y^2 - 1 */
    fe_add(v, v, one);                     /* d y^2 + 1 */
    /* x = u v^3 (u v^7)^((p-5)/8) */
    fe_sq(v3, v); fe_mul(v3, v3, v);
    fe_sq(x, v3); fe_mul(x, x, v); fe_mul(x, x, u);
    fe_pow22523(x, x);
    fe_mul(x, x, v3); fe_mul(x, x, u);
    fe_sq(vxx, x); fe_mul(vxx, vxx, v);
    if (!fe_equal(vxx, u)) {
        fe_zero(t); fe_sub(t, t, u);
        if (!fe_equal(vxx, t)) return false;
        fe_mul(x, x, FE_SQRTM1);
    }
    proven_byte_t xb[32];
    fe_tobytes(xb, x);
    proven_byte_t any = 0;
    for (int i = 0; i < 32; ++i) any |= xb[i];
    if (any == 0 && sign) return false;
    if ((proven_u32)(xb[0] & 1) != sign) { fe_zero(t); fe_sub(x, t, x); }
    fe_copy(p->x, x);
    fe_mul(p->t, p->x, p->y);
    return true;
}

static void ed_base(ge_t *b) {
    fe_copy(b->x, ED_BX); fe_copy(b->y, ED_BY); fe_one(b->z); fe_mul(b->t, ED_BX, ED_BY);
}

/* A 64-byte little-endian hash, or a 32-byte scalar, reduced mod L into 8 limbs. */
static void sc_reduce(const proven_crypto_mp_mod_t *l, proven_u32 out[8], const proven_byte_t *le, proven_size_t n) {
    proven_byte_t be[64];
    for (proven_size_t i = 0; i < n; ++i) be[i] = le[n - 1 - i];
    proven_crypto_mp_reduce_be(l, out, be, n);
    wipe(be, sizeof be);
}

static void sc_tobytes(proven_byte_t out[32], const proven_u32 a[8]) {
    for (int i = 0; i < 32; ++i) out[i] = (proven_byte_t)(a[i / 4] >> (8 * (i % 4)));
}

/* The secret scalar and the prefix from a seed (RFC 8032 section 5.1.5). */
static void ed_expand(proven_byte_t h[64], const proven_byte_t seed[32]) {
    proven_sha512((proven_mem_view_t){ .ptr = seed, .size = 32 }, h);
    h[0] &= 248; h[31] &= 127; h[31] |= 64;
}

void proven_crypto_ed25519_public(proven_byte_t pub[32], const proven_byte_t seed[32]) {
    proven_byte_t h[64];
    ge_t b, a;
    ed_expand(h, seed);
    ed_base(&b);
    ge_mul(&a, h, &b);
    ge_encode(pub, &a);
    PROVEN_CT_PUBLIC(pub, 32);
    wipe(h, sizeof h); wipe(&a, sizeof a);
}

void proven_crypto_ed25519_sign(proven_byte_t sig[64], const proven_byte_t seed[32], const proven_byte_t pub[32],
                                proven_mem_view_t msg) {
    proven_crypto_mp_mod_t l;
    proven_byte_t h[64], digest[64], rb[32];
    proven_u32 r[8], k[8], a[8], s[8];
    proven_sha512_t ctx;
    ge_t b, rp;
    (void)proven_crypto_mp_mod_init(&l, ED_L, sizeof ED_L);      /* a fixed odd modulus: cannot fail */
    ed_expand(h, seed);
    /* r = H(prefix | M) mod L */
    proven_sha512_init(&ctx);
    proven_sha512_update(&ctx, (proven_mem_view_t){ .ptr = h + 32, .size = 32 });
    proven_sha512_update(&ctx, msg);
    proven_sha512_final(&ctx, digest);
    sc_reduce(&l, r, digest, 64);
    sc_tobytes(rb, r);
    ed_base(&b);
    ge_mul(&rp, rb, &b);
    ge_encode(sig, &rp);
    PROVEN_CT_PUBLIC(sig, 32);                   /* R is half of the signature */
    /* k = H(R | A | M) mod L; S = r + k a mod L */
    proven_sha512_init(&ctx);
    proven_sha512_update(&ctx, (proven_mem_view_t){ .ptr = sig, .size = 32 });
    proven_sha512_update(&ctx, (proven_mem_view_t){ .ptr = pub, .size = 32 });
    proven_sha512_update(&ctx, msg);
    proven_sha512_final(&ctx, digest);
    sc_reduce(&l, k, digest, 64);
    sc_reduce(&l, a, h, 32);
    proven_crypto_mp_to_mont(&l, k, k);
    proven_crypto_mp_montmul(&l, s, k, a);
    proven_crypto_mp_add(&l, s, s, r);
    sc_tobytes(sig + 32, s);
    PROVEN_CT_PUBLIC(sig + 32, 32);
    wipe(h, sizeof h); wipe(digest, sizeof digest); wipe(rb, sizeof rb); wipe(r, sizeof r); wipe(a, sizeof a);
    wipe(s, sizeof s); wipe(&ctx, sizeof ctx); wipe(&rp, sizeof rp);
}

bool proven_crypto_ed25519_verify(const proven_byte_t pub[32], proven_mem_view_t msg, const proven_byte_t sig[64]) {
    proven_crypto_mp_mod_t l;
    proven_byte_t digest[64], sbe[32], kb[32], check[32];
    proven_u32 s[8], k[8];
    proven_sha512_t ctx;
    ge_t a, b, p1, p2;
    fe_t zero;
    if (!proven_crypto_mp_mod_init(&l, ED_L, sizeof ED_L)) return false;
    for (int i = 0; i < 32; ++i) sbe[i] = sig[63 - i];
    if (!proven_crypto_mp_load_be(s, 8, sbe, 32) || !proven_crypto_mp_lt(s, l.n, 8)) return false;
    if (!ge_decode(&a, pub)) return false;
    /* -A */
    fe_zero(zero);
    fe_sub(a.x, zero, a.x); fe_sub(a.t, zero, a.t);
    proven_sha512_init(&ctx);
    proven_sha512_update(&ctx, (proven_mem_view_t){ .ptr = sig, .size = 32 });
    proven_sha512_update(&ctx, (proven_mem_view_t){ .ptr = pub, .size = 32 });
    proven_sha512_update(&ctx, msg);
    proven_sha512_final(&ctx, digest);
    sc_reduce(&l, k, digest, 64);
    sc_tobytes(kb, k);
    ed_base(&b);
    ge_mul(&p1, sig + 32, &b);
    ge_mul(&p2, kb, &a);
    ge_add(&p1, &p1, &p2);
    /* [S]B - [k]A must encode to exactly the R that was sent: a non-canonical R cannot match. */
    ge_encode(check, &p1);
    proven_byte_t diff = 0;
    for (int i = 0; i < 32; ++i) diff |= (proven_byte_t)(check[i] ^ sig[i]);
    return diff == 0;
}
