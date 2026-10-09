#include "proven_internal_crypto.h"
#include "proven/hmac.h"

/* P-256 and P-384: ECDH, ECDSA signing and verification.
 *
 * Points are projective (X : Y : Z) with coordinates in Montgomery form, and are added with the
 * complete formulas of Renes, Costello and Batina (2016, algorithm 1). "Complete" is the
 * property this unit is built on: one formula is correct for every pair of points - equal,
 * opposite, the point at infinity - so there is no special case to branch into, and a scalar
 * multiplication does the same sequence of operations for every scalar. */

#define EC_LIMBS 12                               /* P-384 */

typedef struct {
    proven_size_t limbs;
    proven_size_t bytes;
    proven_crypto_mp_mod_t p;                     /* the field */
    proven_crypto_mp_mod_t n;                     /* the group order */
    proven_u32 a[EC_LIMBS];                       /* -3, Montgomery form */
    proven_u32 b[EC_LIMBS];
    proven_u32 b3[EC_LIMBS];
    proven_u32 one[EC_LIMBS];
    proven_u32 gx[EC_LIMBS], gy[EC_LIMBS];
} ec_curve_t;

typedef struct { proven_u32 x[EC_LIMBS], y[EC_LIMBS], z[EC_LIMBS]; } ec_point_t;

static const proven_byte_t EC_P256_P[32] = {
    0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff };
static const proven_byte_t EC_P256_N[32] = {
    0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
    0xbc,0xe6,0xfa,0xad,0xa7,0x17,0x9e,0x84,0xf3,0xb9,0xca,0xc2,0xfc,0x63,0x25,0x51 };
static const proven_byte_t EC_P256_B[32] = {
    0x5a,0xc6,0x35,0xd8,0xaa,0x3a,0x93,0xe7,0xb3,0xeb,0xbd,0x55,0x76,0x98,0x86,0xbc,
    0x65,0x1d,0x06,0xb0,0xcc,0x53,0xb0,0xf6,0x3b,0xce,0x3c,0x3e,0x27,0xd2,0x60,0x4b };
static const proven_byte_t EC_P256_GX[32] = {
    0x6b,0x17,0xd1,0xf2,0xe1,0x2c,0x42,0x47,0xf8,0xbc,0xe6,0xe5,0x63,0xa4,0x40,0xf2,
    0x77,0x03,0x7d,0x81,0x2d,0xeb,0x33,0xa0,0xf4,0xa1,0x39,0x45,0xd8,0x98,0xc2,0x96 };
static const proven_byte_t EC_P256_GY[32] = {
    0x4f,0xe3,0x42,0xe2,0xfe,0x1a,0x7f,0x9b,0x8e,0xe7,0xeb,0x4a,0x7c,0x0f,0x9e,0x16,
    0x2b,0xce,0x33,0x57,0x6b,0x31,0x5e,0xce,0xcb,0xb6,0x40,0x68,0x37,0xbf,0x51,0xf5 };

static const proven_byte_t EC_P384_P[48] = {
    0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
    0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xfe,
    0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff };
static const proven_byte_t EC_P384_N[48] = {
    0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
    0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xc7,0x63,0x4d,0x81,0xf4,0x37,0x2d,0xdf,
    0x58,0x1a,0x0d,0xb2,0x48,0xb0,0xa7,0x7a,0xec,0xec,0x19,0x6a,0xcc,0xc5,0x29,0x73 };
static const proven_byte_t EC_P384_B[48] = {
    0xb3,0x31,0x2f,0xa7,0xe2,0x3e,0xe7,0xe4,0x98,0x8e,0x05,0x6b,0xe3,0xf8,0x2d,0x19,
    0x18,0x1d,0x9c,0x6e,0xfe,0x81,0x41,0x12,0x03,0x14,0x08,0x8f,0x50,0x13,0x87,0x5a,
    0xc6,0x56,0x39,0x8d,0x8a,0x2e,0xd1,0x9d,0x2a,0x85,0xc8,0xed,0xd3,0xec,0x2a,0xef };
static const proven_byte_t EC_P384_GX[48] = {
    0xaa,0x87,0xca,0x22,0xbe,0x8b,0x05,0x37,0x8e,0xb1,0xc7,0x1e,0xf3,0x20,0xad,0x74,
    0x6e,0x1d,0x3b,0x62,0x8b,0xa7,0x9b,0x98,0x59,0xf7,0x41,0xe0,0x82,0x54,0x2a,0x38,
    0x55,0x02,0xf2,0x5d,0xbf,0x55,0x29,0x6c,0x3a,0x54,0x5e,0x38,0x72,0x76,0x0a,0xb7 };
static const proven_byte_t EC_P384_GY[48] = {
    0x36,0x17,0xde,0x4a,0x96,0x26,0x2c,0x6f,0x5d,0x9e,0x98,0xbf,0x92,0x92,0xdc,0x29,
    0xf8,0xf4,0x1d,0xbd,0x28,0x9a,0x14,0x7c,0xe9,0xda,0x31,0x13,0xb5,0xf0,0xb8,0xc0,
    0x0a,0x60,0xb1,0xce,0x1d,0x7e,0x81,0x9d,0x7a,0x43,0x1d,0x7c,0x90,0xea,0x0e,0x5f };

proven_size_t proven_crypto_ec_size(proven_crypto_ec_curve_t curve) {
    return curve == PROVEN_CRYPTO_EC_P256 ? 32 : curve == PROVEN_CRYPTO_EC_P384 ? 48 : 0;
}

static bool ec_curve_init(ec_curve_t *c, proven_crypto_ec_curve_t id) {
    const proven_byte_t *p, *n, *b, *gx, *gy;
    if (id == PROVEN_CRYPTO_EC_P256) { c->bytes = 32; p = EC_P256_P; n = EC_P256_N; b = EC_P256_B; gx = EC_P256_GX; gy = EC_P256_GY; }
    else if (id == PROVEN_CRYPTO_EC_P384) { c->bytes = 48; p = EC_P384_P; n = EC_P384_N; b = EC_P384_B; gx = EC_P384_GX; gy = EC_P384_GY; }
    else return false;
    c->limbs = c->bytes / 4;
    if (!proven_crypto_mp_mod_init(&c->p, p, c->bytes) || !proven_crypto_mp_mod_init(&c->n, n, c->bytes)) return false;
    proven_u32 t[EC_LIMBS], three[EC_LIMBS], zero[EC_LIMBS];
    for (proven_size_t i = 0; i < c->limbs; ++i) { t[i] = 0; three[i] = 0; zero[i] = 0; }
    t[0] = 1; three[0] = 3;
    proven_crypto_mp_to_mont(&c->p, c->one, t);
    proven_crypto_mp_to_mont(&c->p, three, three);
    proven_crypto_mp_sub(&c->p, c->a, zero, three);
    if (!proven_crypto_mp_load_be(t, c->limbs, b, c->bytes)) return false;
    proven_crypto_mp_to_mont(&c->p, c->b, t);
    proven_crypto_mp_add(&c->p, c->b3, c->b, c->b);
    proven_crypto_mp_add(&c->p, c->b3, c->b3, c->b);
    if (!proven_crypto_mp_load_be(t, c->limbs, gx, c->bytes)) return false;
    proven_crypto_mp_to_mont(&c->p, c->gx, t);
    if (!proven_crypto_mp_load_be(t, c->limbs, gy, c->bytes)) return false;
    proven_crypto_mp_to_mont(&c->p, c->gy, t);
    return true;
}

static void ec_copy(proven_u32 *out, const proven_u32 *a, proven_size_t limbs) {
    for (proven_size_t i = 0; i < limbs; ++i) out[i] = a[i];
}

static void ec_set_infinity(const ec_curve_t *c, ec_point_t *p) {
    for (proven_size_t i = 0; i < EC_LIMBS; ++i) { p->x[i] = 0; p->y[i] = 0; p->z[i] = 0; }
    ec_copy(p->y, c->one, c->limbs);
}

/* Algorithm 1 of Renes-Costello-Batina: complete addition for y^2 = x^3 + ax + b. */
static void ec_add(const ec_curve_t *c, ec_point_t *out, const ec_point_t *p, const ec_point_t *q) {
    const proven_crypto_mp_mod_t *f = &c->p;
    proven_u32 t0[EC_LIMBS], t1[EC_LIMBS], t2[EC_LIMBS], t3[EC_LIMBS], t4[EC_LIMBS], t5[EC_LIMBS];
    proven_u32 x3[EC_LIMBS], y3[EC_LIMBS], z3[EC_LIMBS];
#define M(o, a, b) proven_crypto_mp_montmul(f, o, a, b)
#define A(o, a, b) proven_crypto_mp_add(f, o, a, b)
#define S(o, a, b) proven_crypto_mp_sub(f, o, a, b)
    M(t0, p->x, q->x); M(t1, p->y, q->y); M(t2, p->z, q->z);
    A(t3, p->x, p->y); A(t4, q->x, q->y); M(t3, t3, t4);
    A(t4, t0, t1); S(t3, t3, t4); A(t4, p->x, p->z);
    A(t5, q->x, q->z); M(t4, t4, t5); A(t5, t0, t2);
    S(t4, t4, t5); A(t5, p->y, p->z); A(x3, q->y, q->z);
    M(t5, t5, x3); A(x3, t1, t2); S(t5, t5, x3);
    M(z3, c->a, t4); M(x3, c->b3, t2); A(z3, x3, z3);
    S(x3, t1, z3); A(z3, t1, z3); M(y3, x3, z3);
    A(t1, t0, t0); A(t1, t1, t0); M(t2, c->a, t2);
    M(t4, c->b3, t4); A(t1, t1, t2); S(t2, t0, t2);
    M(t2, c->a, t2); A(t4, t4, t2); M(t0, t1, t4);
    A(y3, y3, t0); M(t0, t5, t4); M(x3, t3, x3);
    S(x3, x3, t0); M(t0, t3, t1); M(z3, t5, z3);
    A(z3, z3, t0);
#undef M
#undef A
#undef S
    ec_copy(out->x, x3, c->limbs); ec_copy(out->y, y3, c->limbs); ec_copy(out->z, z3, c->limbs);
}

/* k * p, k a reduced scalar in plain limbs (SECRET). A fixed window of four bits: sixteen
 * multiples in a table, every entry read on every lookup and one kept by mask. */
static void ec_mul(const ec_curve_t *c, ec_point_t *out, const proven_u32 *k, const ec_point_t *p) {
    ec_point_t table[16];
    ec_set_infinity(c, &table[0]);
    table[1] = *p;
    for (int i = 2; i < 16; ++i) ec_add(c, &table[i], &table[i - 1], p);
    ec_point_t acc, pick;
    ec_set_infinity(c, &acc);
    for (proven_size_t i = c->limbs * 8; i-- > 0;) {
        for (int d = 0; d < 4; ++d) ec_add(c, &acc, &acc, &acc);
        proven_u32 nib = (k[i / 8] >> (4 * (i % 8))) & 15u;
        for (proven_size_t j = 0; j < EC_LIMBS; ++j) { pick.x[j] = 0; pick.y[j] = 0; pick.z[j] = 0; }
        for (proven_u32 t = 0; t < 16; ++t) {
            proven_u32 x = t ^ nib;
            proven_u32 mask = ((x | ((proven_u32)0 - x)) >> 31) - 1u;      /* all ones when t == nib */
            for (proven_size_t j = 0; j < c->limbs; ++j) {
                pick.x[j] |= table[t].x[j] & mask;
                pick.y[j] |= table[t].y[j] & mask;
                pick.z[j] |= table[t].z[j] & mask;
            }
        }
        ec_add(c, &acc, &acc, &pick);
    }
    *out = acc;
    proven_mem_wipe((proven_mem_mut_t){ .ptr = (proven_byte_t *)table, .size = sizeof table });
    proven_mem_wipe((proven_mem_mut_t){ .ptr = (proven_byte_t *)&pick, .size = sizeof pick });
    proven_mem_wipe((proven_mem_mut_t){ .ptr = (proven_byte_t *)&acc, .size = sizeof acc });
}

/* Affine coordinates in plain limbs. Returns all-ones when the point is at infinity (the
 * outputs are then zero). */
static proven_u32 ec_affine(const ec_curve_t *c, proven_u32 *x, proven_u32 *y, const ec_point_t *p) {
    proven_u32 zi[EC_LIMBS];
    proven_crypto_mp_inv_prime(&c->p, zi, p->z);
    proven_crypto_mp_montmul(&c->p, x, p->x, zi);
    proven_crypto_mp_montmul(&c->p, y, p->y, zi);
    proven_crypto_mp_from_mont(&c->p, x, x);
    proven_crypto_mp_from_mont(&c->p, y, y);
    proven_u32 inf = proven_crypto_mp_is_zero(p->z, c->limbs);
    PROVEN_CT_PUBLIC(&inf, sizeof inf);          /* a result at infinity is refused, which is seen */
    proven_mem_wipe((proven_mem_mut_t){ .ptr = (proven_byte_t *)zi, .size = sizeof zi });
    return inf;
}

/* An uncompressed point from the wire. Everything about it is public. */
static bool ec_decode(const ec_curve_t *c, ec_point_t *out, proven_mem_view_t pub) {
    if (!pub.ptr || pub.size != 1 + 2 * c->bytes || pub.ptr[0] != 0x04) return false;
    proven_u32 x[EC_LIMBS], y[EC_LIMBS], lhs[EC_LIMBS], rhs[EC_LIMBS];
    if (!proven_crypto_mp_load_be(x, c->limbs, pub.ptr + 1, c->bytes)) return false;
    if (!proven_crypto_mp_load_be(y, c->limbs, pub.ptr + 1 + c->bytes, c->bytes)) return false;
    if (!proven_crypto_mp_lt(x, c->p.n, c->limbs) || !proven_crypto_mp_lt(y, c->p.n, c->limbs)) return false;
    proven_crypto_mp_to_mont(&c->p, x, x);
    proven_crypto_mp_to_mont(&c->p, y, y);
    /* y^2 == x^3 + ax + b */
    proven_crypto_mp_montmul(&c->p, lhs, y, y);
    proven_crypto_mp_montmul(&c->p, rhs, x, x);
    proven_crypto_mp_add(&c->p, rhs, rhs, c->a);
    proven_crypto_mp_montmul(&c->p, rhs, rhs, x);
    proven_crypto_mp_add(&c->p, rhs, rhs, c->b);
    if (!proven_crypto_mp_eq(lhs, rhs, c->limbs)) return false;
    for (proven_size_t i = 0; i < EC_LIMBS; ++i) { out->x[i] = 0; out->y[i] = 0; out->z[i] = 0; }
    ec_copy(out->x, x, c->limbs); ec_copy(out->y, y, c->limbs); ec_copy(out->z, c->one, c->limbs);
    return true;
}

/* A private scalar from bytes: in [1, n-1], or refused. The comparison's result is public (a
 * key out of range is not a key), the value is not. */
static bool ec_load_scalar(const ec_curve_t *c, proven_u32 *k, const proven_byte_t *priv) {
    if (!proven_crypto_mp_load_be(k, c->limbs, priv, c->bytes)) return false;
    proven_u32 ok = proven_crypto_mp_lt(k, c->n.n, c->limbs) & ~proven_crypto_mp_is_zero(k, c->limbs);
    PROVEN_CT_PUBLIC(&ok, sizeof ok);
    return ok != 0;
}

static void ec_base(const ec_curve_t *c, ec_point_t *g) {
    for (proven_size_t i = 0; i < EC_LIMBS; ++i) { g->x[i] = 0; g->y[i] = 0; g->z[i] = 0; }
    ec_copy(g->x, c->gx, c->limbs); ec_copy(g->y, c->gy, c->limbs); ec_copy(g->z, c->one, c->limbs);
}

bool proven_crypto_ec_public(proven_crypto_ec_curve_t curve, const proven_byte_t *priv, proven_byte_t *pub_out) {
    ec_curve_t c;
    proven_u32 k[EC_LIMBS], x[EC_LIMBS], y[EC_LIMBS];
    ec_point_t g, r;
    if (!ec_curve_init(&c, curve) || !ec_load_scalar(&c, k, priv)) return false;
    ec_base(&c, &g);
    ec_mul(&c, &r, k, &g);
    proven_u32 inf = ec_affine(&c, x, y, &r);
    proven_mem_wipe((proven_mem_mut_t){ .ptr = (proven_byte_t *)k, .size = sizeof k });
    if (inf) return false;
    pub_out[0] = 0x04;
    proven_crypto_mp_store_be(pub_out + 1, c.bytes, x, c.limbs);
    proven_crypto_mp_store_be(pub_out + 1 + c.bytes, c.bytes, y, c.limbs);
    PROVEN_CT_PUBLIC(pub_out, 1 + 2 * c.bytes);
    return true;
}

bool proven_crypto_ecdh(proven_crypto_ec_curve_t curve, const proven_byte_t *priv, proven_mem_view_t peer, proven_byte_t *shared_out) {
    ec_curve_t c;
    proven_u32 k[EC_LIMBS], x[EC_LIMBS], y[EC_LIMBS];
    ec_point_t q, r;
    if (!ec_curve_init(&c, curve) || !ec_decode(&c, &q, peer) || !ec_load_scalar(&c, k, priv)) return false;
    ec_mul(&c, &r, k, &q);
    /* The curves have prime order, so a valid point times a scalar in [1, n-1] is never the
     * point at infinity; the check costs nothing and is kept. */
    proven_u32 inf = ec_affine(&c, x, y, &r);
    proven_crypto_mp_store_be(shared_out, c.bytes, x, c.limbs);
    proven_mem_wipe((proven_mem_mut_t){ .ptr = (proven_byte_t *)k, .size = sizeof k });
    proven_mem_wipe((proven_mem_mut_t){ .ptr = (proven_byte_t *)x, .size = sizeof x });
    proven_mem_wipe((proven_mem_mut_t){ .ptr = (proven_byte_t *)y, .size = sizeof y });
    proven_mem_wipe((proven_mem_mut_t){ .ptr = (proven_byte_t *)&r, .size = sizeof r });
    return inf == 0;
}

/* The digest as an integer mod n: its leftmost bytes, as many as the order has. */
static void ec_digest_scalar(const ec_curve_t *c, proven_u32 *z, proven_mem_view_t digest) {
    proven_size_t n = digest.size < c->bytes ? digest.size : c->bytes;
    proven_crypto_mp_reduce_be(&c->n, z, digest.ptr, n);
}

bool proven_crypto_ecdsa_verify(proven_crypto_ec_curve_t curve, proven_mem_view_t pub, proven_mem_view_t digest,
                                proven_mem_view_t r_in, proven_mem_view_t s_in) {
    ec_curve_t c;
    ec_point_t q, g, p1, p2;
    proven_u32 r[EC_LIMBS], s[EC_LIMBS], z[EC_LIMBS], w[EC_LIMBS], u1[EC_LIMBS], u2[EC_LIMBS], x[EC_LIMBS], y[EC_LIMBS];
    if (!ec_curve_init(&c, curve) || !ec_decode(&c, &q, pub)) return false;
    if (!digest.ptr || digest.size == 0 || !r_in.ptr || !s_in.ptr) return false;
    if (!proven_crypto_mp_load_be(r, c.limbs, r_in.ptr, r_in.size) || !proven_crypto_mp_load_be(s, c.limbs, s_in.ptr, s_in.size)) return false;
    if (proven_crypto_mp_is_zero(r, c.limbs) || proven_crypto_mp_is_zero(s, c.limbs)) return false;
    if (!proven_crypto_mp_lt(r, c.n.n, c.limbs) || !proven_crypto_mp_lt(s, c.n.n, c.limbs)) return false;
    ec_digest_scalar(&c, z, digest);
    /* w = s^-1; u1 = z w; u2 = r w (all mod n). */
    proven_crypto_mp_to_mont(&c.n, w, s);
    proven_crypto_mp_inv_prime(&c.n, w, w);
    proven_crypto_mp_montmul(&c.n, u1, z, w);
    proven_crypto_mp_montmul(&c.n, u2, r, w);
    ec_base(&c, &g);
    ec_mul(&c, &p1, u1, &g);
    ec_mul(&c, &p2, u2, &q);
    ec_add(&c, &p1, &p1, &p2);
    if (ec_affine(&c, x, y, &p1)) return false;
    /* x mod n: x < p < 2n for both curves, so one subtraction at most. */
    proven_u32 xr[EC_LIMBS];
    proven_byte_t xb[PROVEN_CRYPTO_EC_MAX_BYTES];
    proven_crypto_mp_store_be(xb, c.bytes, x, c.limbs);
    proven_crypto_mp_reduce_be(&c.n, xr, xb, c.bytes);
    return proven_crypto_mp_eq(xr, r, c.limbs) != 0;
}

/* One DER INTEGER of fewer than 128 content bytes: non-negative and minimal. */
static bool ec_der_int(const proven_byte_t **p, const proven_byte_t *end, proven_mem_view_t *out) {
    if (end - *p < 2 || (*p)[0] != 0x02) return false;
    proven_size_t len = (*p)[1];
    if (len == 0 || len > 127 || (proven_size_t)(end - *p - 2) < len) return false;
    const proven_byte_t *v = *p + 2;
    if (v[0] & 0x80) return false;
    if (len > 1 && v[0] == 0 && (v[1] & 0x80) == 0) return false;
    out->ptr = v; out->size = len;
    *p = v + len;
    return true;
}

bool proven_crypto_ecdsa_verify_der(proven_crypto_ec_curve_t curve, proven_mem_view_t pub, proven_mem_view_t digest,
                                    proven_mem_view_t sig) {
    if (!sig.ptr || sig.size < 8 || sig.ptr[0] != 0x30) return false;
    const proven_byte_t *p = sig.ptr + 2, *end = sig.ptr + sig.size;
    proven_size_t len = sig.ptr[1];
    if (len == 0x81) {
        /* The long form is right only for 128 bytes and more. */
        if (sig.size < 3 || sig.ptr[2] < 0x80) return false;
        len = sig.ptr[2];
        p = sig.ptr + 3;
    } else if (len >= 0x80) {
        return false;
    }
    if ((proven_size_t)(end - p) != len) return false;
    proven_mem_view_t r, s;
    if (!ec_der_int(&p, end, &r) || !ec_der_int(&p, end, &s) || p != end) return false;
    return proven_crypto_ecdsa_verify(curve, pub, digest, r, s);
}

/* RFC 6979 section 3.2: the nonce from the key and the digest. */
typedef struct {
    proven_hmac_hash_t hash;
    proven_size_t hlen;
    proven_byte_t k[PROVEN_HMAC_MAX_SIZE];
    proven_byte_t v[PROVEN_HMAC_MAX_SIZE];
} ec_drbg_t;

static bool ec_drbg_step(ec_drbg_t *d, int sep, const proven_byte_t *x, const proven_byte_t *h, proven_size_t n) {
    proven_hmac_t m;
    if (proven_hmac_init(&m, d->hash, (proven_mem_view_t){ .ptr = d->k, .size = d->hlen }) != PROVEN_OK) return false;
    proven_hmac_update(&m, (proven_mem_view_t){ .ptr = d->v, .size = d->hlen });
    if (sep >= 0) {
        proven_byte_t b = (proven_byte_t)sep;
        proven_hmac_update(&m, (proven_mem_view_t){ .ptr = &b, .size = 1 });
        if (x) {
            proven_hmac_update(&m, (proven_mem_view_t){ .ptr = x, .size = n });
            proven_hmac_update(&m, (proven_mem_view_t){ .ptr = h, .size = n });
        }
        proven_hmac_final(&m, d->k);
        if (proven_hmac(d->hash, (proven_mem_view_t){ .ptr = d->k, .size = d->hlen }, (proven_mem_view_t){ .ptr = d->v, .size = d->hlen }, d->v) != PROVEN_OK) return false;
    } else {
        proven_hmac_final(&m, d->v);
    }
    return true;
}

bool proven_crypto_ecdsa_sign(proven_crypto_ec_curve_t curve, int hmac_hash, const proven_byte_t *priv,
                              proven_mem_view_t digest, proven_byte_t *sig_out) {
    ec_curve_t c;
    ec_drbg_t d;
    ec_point_t g, rp;
    proven_u32 key[EC_LIMBS], z[EC_LIMBS], k[EC_LIMBS], r[EC_LIMBS], s[EC_LIMBS], t[EC_LIMBS], x[EC_LIMBS], y[EC_LIMBS];
    proven_byte_t zb[PROVEN_CRYPTO_EC_MAX_BYTES], kb[PROVEN_CRYPTO_EC_MAX_BYTES + PROVEN_HMAC_MAX_SIZE], xb[PROVEN_CRYPTO_EC_MAX_BYTES];
    bool ok = false;
    d.hash = (proven_hmac_hash_t)hmac_hash;
    d.hlen = proven_hmac_size(d.hash);
    if (d.hlen == 0 || !digest.ptr || digest.size == 0) return false;
    if (!ec_curve_init(&c, curve) || !ec_load_scalar(&c, key, priv)) return false;
    ec_digest_scalar(&c, z, digest);
    proven_crypto_mp_store_be(zb, c.bytes, z, c.limbs);
    for (proven_size_t i = 0; i < d.hlen; ++i) { d.v[i] = 0x01; d.k[i] = 0x00; }
    if (!ec_drbg_step(&d, 0x00, priv, zb, c.bytes) || !ec_drbg_step(&d, 0x01, priv, zb, c.bytes)) goto done;
    ec_base(&c, &g);
    for (int tries = 0; tries < 64 && !ok; ++tries) {
        proven_size_t have = 0;
        while (have < c.bytes) {
            if (!ec_drbg_step(&d, -1, NULL, NULL, 0)) goto done;
            for (proven_size_t i = 0; i < d.hlen; ++i) kb[have + i] = d.v[i];
            have += d.hlen;
        }
        /* Whether a candidate is in range is not a secret: a refused one is thrown away. */
        if (ec_load_scalar(&c, k, kb)) {
            ec_mul(&c, &rp, k, &g);
            if (!ec_affine(&c, x, y, &rp)) {
                proven_crypto_mp_store_be(xb, c.bytes, x, c.limbs);
                proven_crypto_mp_reduce_be(&c.n, r, xb, c.bytes);
                PROVEN_CT_PUBLIC(r, sizeof r);    /* r is half of the signature */
                /* s = k^-1 (z + r d) mod n */
                proven_crypto_mp_to_mont(&c.n, t, key);
                proven_crypto_mp_montmul(&c.n, t, t, r);                 /* r d */
                proven_crypto_mp_add(&c.n, t, t, z);
                proven_crypto_mp_to_mont(&c.n, k, k);
                proven_crypto_mp_inv_prime(&c.n, k, k);                   /* k^-1, Montgomery form */
                proven_crypto_mp_montmul(&c.n, s, t, k);
                PROVEN_CT_PUBLIC(s, sizeof s);
                if (!proven_crypto_mp_is_zero(r, c.limbs) && !proven_crypto_mp_is_zero(s, c.limbs)) {
                    proven_crypto_mp_store_be(sig_out, c.bytes, r, c.limbs);
                    proven_crypto_mp_store_be(sig_out + c.bytes, c.bytes, s, c.limbs);
                    ok = true;
                }
            }
        }
        if (!ok && !ec_drbg_step(&d, 0x00, NULL, NULL, 0)) goto done;
    }
done:
    proven_mem_wipe((proven_mem_mut_t){ .ptr = (proven_byte_t *)&d, .size = sizeof d });
    proven_mem_wipe((proven_mem_mut_t){ .ptr = (proven_byte_t *)key, .size = sizeof key });
    proven_mem_wipe((proven_mem_mut_t){ .ptr = (proven_byte_t *)k, .size = sizeof k });
    proven_mem_wipe((proven_mem_mut_t){ .ptr = (proven_byte_t *)t, .size = sizeof t });
    proven_mem_wipe((proven_mem_mut_t){ .ptr = kb, .size = sizeof kb });
    proven_mem_wipe((proven_mem_mut_t){ .ptr = (proven_byte_t *)&rp, .size = sizeof rp });
    return ok;
}
