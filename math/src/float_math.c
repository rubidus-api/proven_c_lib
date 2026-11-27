#include "proven/float_math.h"
#include "proven/float_ops.h"
#include "proven/math_config.h"
#include "proven_internal_float_ops.h"
#include "proven_internal_float_math.h"
#include "float_math_consts.h"
#include "cm/cm_config.h"

/*
 * exp, log, pow, sin, cos, tan and atan2, correctly rounded to nearest even (RFC-0056 D-11).
 *
 * This file is the integer path: Ziv's method on multiple-precision numbers of 32-bit limbs.
 * Each function is evaluated at a precision of n limbs with a bound on its error; if every
 * value within the bound rounds to the same bits, those are the answer, and otherwise the
 * evaluation is made again at twice the precision. The results of these functions at a
 * representable argument other than the few exact ones (exp(0), log(1), ...) are
 * transcendental, so they are never a rounding boundary and the loop ends; pow's exact and
 * halfway results are found first, by integers. Every function is evaluated one limb beyond
 * the precision tested, so the error of its own arithmetic is far below the bound of 4 units
 * the test allows (see each function).
 *
 * Integer arithmetic only, so a core without an FPU computes the same bits as one with:
 * 32 x 32 -> 64 multiplies, and divisions by numbers below 2^16 (bit by bit where the core
 * has no divide instruction). The numbers live on the stack: about 140 bytes each.
 */

typedef proven_u32 u32;
typedef proven_u64 u64;

#define FM_G 4                         /* the error allowed by the rounding test, in units of the last place */

// -----------------------------------------------------------------------------
// Integer helpers
// -----------------------------------------------------------------------------

#if (defined(__arm__) && !defined(__ARM_FEATURE_IDIV)) || (defined(__riscv) && !defined(__riscv_div))
static u32 fm_udiv(u32 n, u32 d) {
    u32 q = 0, r = 0;
    for (int i = 31; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1u);
        if (r >= d) { r -= d; q |= (u32)1 << i; }
    }
    return q;
}
#else
static u32 fm_udiv(u32 n, u32 d) { return n / d; }
#endif

static int fm_clz32(u32 x) {
    int n = 0;
    if (!(x >> 16)) { n += 16; x <<= 16; }
    if (!(x >> 24)) { n += 8; x <<= 8; }
    if (!(x >> 28)) { n += 4; x <<= 4; }
    if (!(x >> 30)) { n += 2; x <<= 2; }
    if (!(x >> 31)) n += 1;
    return n;
}

/* Bits [pos, pos + 32) of a little-endian limb array, zero outside it. */
static u32 fm_get32(const u32 *d, int n, proven_i64 pos) {
    if (pos >= ((proven_i64)32) * n || pos <= -32) return 0;
    if (pos < 0) return d[0] << (-pos);
    const int li = (int)(pos >> 5), sh = (int)(pos & 31);
    u32 v = d[li] >> sh;
    if (sh && li + 1 < n) v |= d[li + 1] << (32 - sh);
    return v;
}

static int fm_getbit(const u32 *d, int n, proven_i64 pos) {
    if (pos < 0 || pos >= ((proven_i64)32) * n) return 0;
    return (int)((d[pos >> 5] >> (pos & 31)) & 1u);
}

// -----------------------------------------------------------------------------
// Multiple-precision numbers
// -----------------------------------------------------------------------------

#define MP_CAP 40                      /* limbs: the last Ziv level (24) with the extra limbs its functions take */

typedef struct {
    int n;                             /* limbs in use; 0 is the value zero */
    bool neg;
    proven_i64 e;                            /* the weight of d[0]'s lowest bit is 2^e */
    u32 d[MP_CAP];
} mp_t;

static void mp_zero(mp_t *r) { r->n = 0; r->neg = false; r->e = 0; }

/* dst[0..W) = the bits of src[0..sn) shifted left by `shift` (right when negative), truncated
 * below bit 0; nothing reaches above W limbs where it is used. */
static void fm_place(u32 *dst, int W, const u32 *src, int sn, proven_i64 shift) {
    for (int i = 0; i < W; i++) dst[i] = 0;
    if (shift >= 0) {
        const proven_i64 li = shift >> 5;
        const int bi = (int)(shift & 31);
        for (int i = 0; i < sn; i++) {
            const proven_i64 k = li + i;
            if (k >= W) break;
            dst[k] |= src[i] << bi;
            if (bi && k + 1 < W) dst[k + 1] |= src[i] >> (32 - bi);
        }
    } else {
        const proven_i64 drop = -shift;
        const proven_i64 lo = drop >> 5;
        const int bi = (int)(drop & 31);
        for (int i = 0; i < W; i++) {
            const proven_i64 k = lo + i;
            if (k >= sn) break;
            u32 v = src[k] >> bi;
            if (bi && k + 1 < sn) v |= src[k + 1] << (32 - bi);
            dst[i] = v;
        }
    }
}

/* r = the top n limbs of the magnitude b[0..len) * 2^e, truncated, with the sign given. */
static void mp_set_buf(mp_t *r, const u32 *b, int len, proven_i64 e, bool neg, int n) {
    int top = len - 1;
    while (top >= 0 && b[top] == 0) top--;
    if (top < 0) { mp_zero(r); return; }
    const proven_i64 tb = ((proven_i64)32) * top + 31 - fm_clz32(b[top]);
    const proven_i64 lsb = tb - (((proven_i64)32) * n - 1);
    u32 tmp[MP_CAP];
    fm_place(tmp, n, b, top + 1, -lsb);
    for (int i = 0; i < n; i++) r->d[i] = tmp[i];
    r->n = n;
    r->e = e + lsb;
    r->neg = neg;
}

static void mp_set_u64(mp_t *r, bool neg, u64 m, proven_i64 e, int n) {
    const u32 b[2] = { (u32)m, (u32)(m >> 32) };
    mp_set_buf(r, b, 2, e, neg, n);
}

static void mp_copy(mp_t *r, const mp_t *a, int n) {
    if (!a->n) { mp_zero(r); return; }
    mp_set_buf(r, a->d, a->n, a->e, a->neg, n);
}

/* A constant's top n limbs (truncated: within one unit of the last place). */
static void mp_const(mp_t *r, const u32 *table, int tl, proven_i64 texp, int n) {
    if (n > tl) n = tl;
    for (int i = 0; i < n; i++) r->d[i] = table[tl - n + i];
    r->n = n;
    r->neg = false;
    r->e = texp + ((proven_i64)32) * (tl - n);
}

static proven_i64 mp_top(const mp_t *a) { return a->e + ((proven_i64)32) * a->n - 1; }

/* -1, 0 or 1 as |a| is below, equal to or above |b|. */
static int mp_cmp_abs(const mp_t *a, const mp_t *b) {
    if (!a->n || !b->n) return a->n ? 1 : b->n ? -1 : 0;
    const proven_i64 ta = mp_top(a), tb = mp_top(b);
    if (ta != tb) return ta > tb ? 1 : -1;
    for (proven_i64 pos = ta - 31; pos > ta - ((proven_i64)32) * MP_CAP - 64; pos -= 32) {
        const u32 x = fm_get32(a->d, a->n, pos - a->e), y = fm_get32(b->d, b->n, pos - b->e);
        if (x != y) return x > y ? 1 : -1;
        if (pos - a->e < -32 && pos - b->e < -32) break;
    }
    return 0;
}

static bool mp_cmp_abs_gt(const mp_t *a, const mp_t *b) { return mp_cmp_abs(a, b) > 0; }

/* r = a + b, or a - b when bsub. */
static void mp_addsub(mp_t *r, const mp_t *a, const mp_t *b, bool bsub, int n) {
    const bool bneg = b->neg != bsub;
    if (!a->n) {
        if (!b->n) { mp_zero(r); return; }
        mp_set_buf(r, b->d, b->n, b->e, bneg, n);
        return;
    }
    if (!b->n) { mp_copy(r, a, n); return; }
    const proven_i64 ta = mp_top(a), tb = mp_top(b);
    const proven_i64 T = (ta > tb ? ta : tb) + 1;
    const int W = n + 2;
    const proven_i64 base = T - ((proven_i64)32) * W;
    u32 A[MP_CAP + 3], B[MP_CAP + 3];
    fm_place(A, W, a->d, a->n, a->e - base);
    fm_place(B, W, b->d, b->n, b->e - base);
    A[W] = B[W] = 0;
    if (a->neg == bneg) {
        u64 c = 0;
        for (int i = 0; i <= W; i++) { c += (u64)A[i] + B[i]; A[i] = (u32)c; c >>= 32; }
        mp_set_buf(r, A, W + 1, base, a->neg, n);
        return;
    }
    int cmp = 0;
    for (int i = W - 1; i >= 0 && !cmp; i--) cmp = A[i] > B[i] ? 1 : A[i] < B[i] ? -1 : 0;
    if (cmp == 0) { mp_zero(r); return; }
    const u32 *X = cmp > 0 ? A : B, *Y = cmp > 0 ? B : A;
    u32 D[MP_CAP + 3];
    u64 br = 0;
    for (int i = 0; i < W; i++) {
        const u64 t = (u64)X[i] - Y[i] - br;
        D[i] = (u32)t;
        br = (t >> 63) & 1u;
    }
    mp_set_buf(r, D, W, base, cmp > 0 ? a->neg : bneg, n);
}

static void mp_add(mp_t *r, const mp_t *a, const mp_t *b, int n) { mp_addsub(r, a, b, false, n); }

static void mp_sub(mp_t *r, const mp_t *a, const mp_t *b, int n) { mp_addsub(r, a, b, true, n); }

static void mp_mul(mp_t *r, const mp_t *a, const mp_t *b, int n) {
    if (!a->n || !b->n) { mp_zero(r); return; }
    /* Only the top n + 1 limbs of each take part: what is below is far under the result's last place. */
    const int na = a->n > n + 1 ? n + 1 : a->n, nb = b->n > n + 1 ? n + 1 : b->n;
    const u32 *da = a->d + (a->n - na), *db = b->d + (b->n - nb);
    const proven_i64 e = a->e + ((proven_i64)32) * (a->n - na) + b->e + ((proven_i64)32) * (b->n - nb);
    u32 P[2 * MP_CAP + 2];
    for (int i = 0; i < na + nb; i++) P[i] = 0;
    for (int i = 0; i < na; i++) {
        u64 c = 0;
        for (int j = 0; j < nb; j++) {
            c += (u64)da[i] * db[j] + P[i + j];
            P[i + j] = (u32)c;
            c >>= 32;
        }
        P[i + nb] = (u32)c;
    }
    mp_set_buf(r, P, na + nb, e, a->neg != b->neg, n);
}

static void mp_mul_small(mp_t *r, const mp_t *a, u32 k, int n) {
    if (!a->n || !k) { mp_zero(r); return; }
    u32 P[MP_CAP + 1];
    u64 c = 0;
    for (int i = 0; i < a->n; i++) { c += (u64)a->d[i] * k; P[i] = (u32)c; c >>= 32; }
    P[a->n] = (u32)c;
    mp_set_buf(r, P, a->n + 1, a->e, a->neg, n);
}

/* r = a / k for 0 < k < 2^16, by 16-bit digits so that each division is 32 by 32 bits. */
static void mp_div_small(mp_t *r, const mp_t *a, u32 k, int n) {
    if (!a->n) { mp_zero(r); return; }
    const int L = n + 2, take = a->n < L ? a->n : L;
    u32 N[MP_CAP + 3];
    for (int i = 0; i < L; i++) N[i] = 0;
    for (int i = 0; i < take; i++) N[L - take + i] = a->d[a->n - take + i];
    const proven_i64 e = a->e + ((proven_i64)32) * (a->n - take) - ((proven_i64)32) * (L - take);
    u32 rem = 0;
    for (int i = L - 1; i >= 0; i--) {
        const u32 hi = (rem << 16) | (N[i] >> 16);
        const u32 qh = fm_udiv(hi, k);
        rem = hi - qh * k;
        const u32 lo = (rem << 16) | (N[i] & 0xffffu);
        const u32 ql = fm_udiv(lo, k);
        rem = lo - ql * k;
        N[i] = (qh << 16) | ql;
    }
    mp_set_buf(r, N, L, e, a->neg, n);
}

static void mp_one(mp_t *r, int n) { mp_set_u64(r, false, 1, 0, n); }

/* 1 / b by Newton's iteration x += x (1 - b x), from a 31-bit estimate, doubling the
 * precision each step, with one more step at the last. */
static void mp_recip(mp_t *r, const mp_t *b, int n) {
    const u32 t = b->d[b->n - 1];
    const u64 num = 0x7fffffffffffffffu;
    u64 q = 0, rem = 0;
    for (int i = 63; i >= 0; i--) {
        rem = (rem << 1) | ((num >> i) & 1u);
        if (rem >= t) { rem -= t; q |= (u64)1 << i; }
    }
    mp_t x, t1, t2, one;
    mp_set_u64(&x, b->neg, q, -63 - ((proven_i64)32) * (b->n - 1) - b->e, 2);
    int acc = 30, p = 2;
    const int target = n + 1;
    bool last = false;
    for (;;) {
        p = (2 * acc) / 32 + 2;
        if (p > target) p = target;
        mp_one(&one, p + 1);
        mp_mul(&t1, b, &x, p + 1);
        mp_sub(&t2, &one, &t1, p + 1);
        mp_mul(&t1, &x, &t2, p + 1);
        mp_add(&x, &x, &t1, p + 1);
        acc = 2 * acc;
        if (acc > 32 * p - 8) acc = 32 * p - 8;
        if (p == target) {
            if (last) break;
            last = acc >= 32 * target - 8;
        }
    }
    mp_copy(r, &x, n);
}

static void mp_div(mp_t *r, const mp_t *a, const mp_t *b, int n) {
    mp_t inv;
    mp_recip(&inv, b, n + 1);
    mp_mul(r, a, &inv, n);
}

/* The nearest integer to a, for |a| < 2^30. */
static proven_i64 mp_round_long(const mp_t *a) {
    if (!a->n || mp_top(a) < -1) return 0;
    const proven_i64 unit = -a->e;                                /* the mantissa bit of weight 1 */
    proven_i64 v = (proven_i64)(fm_get32(a->d, a->n, unit) & 0x7fffffffu);
    if (fm_getbit(a->d, a->n, unit - 1)) v++;
    return a->neg ? -v : v;
}

/* The magnitude b[0..len) * 2^e as its top 64 bits, the lowest one sticky, and their exponent. */
static void fm_to_m64(const u32 *b, int len, proven_i64 e, u64 *m, proven_i64 *me) {
    int top = len - 1;
    while (top >= 0 && b[top] == 0) top--;
    const proven_i64 tb = ((proven_i64)32) * top + 31 - fm_clz32(b[top]);
    const proven_i64 lsb = tb - 63;
    *m = ((u64)fm_get32(b, len, lsb + 32) << 32) | fm_get32(b, len, lsb);
    bool sticky = false;
    if (lsb > 0) {
        const proven_i64 full = lsb >> 5;                         /* limbs wholly below */
        for (proven_i64 j = 0; j < full && j < len && !sticky; j++) sticky = b[j] != 0;
        if (!sticky && (lsb & 31) && full < len) sticky = (b[full] & (((u32)1 << (lsb & 31)) - 1u)) != 0;
    }
    if (sticky) *m |= 1u;
    *me = e + lsb;
}

/* Whether every value within FM_G units of v's last place rounds to the same bits. */
static bool mp_round_test(const mp_t *v, bool b32, u64 *out) {
    u32 lo[MP_CAP + 1], hi[MP_CAP + 1];
    const int n = v->n;
    for (int i = 0; i < n; i++) lo[i] = hi[i] = v->d[i];
    lo[n] = hi[n] = 0;
    u64 c = FM_G;
    for (int i = 0; i <= n && c; i++) { c += hi[i]; hi[i] = (u32)c; c >>= 32; }
    u64 br = FM_G;
    for (int i = 0; i <= n && br; i++) {
        const u64 t = (u64)lo[i] - br;
        lo[i] = (u32)t;
        br = (t >> 63) & 1u;
    }
    u64 ml, mh;
    proven_i64 el, eh;
    fm_to_m64(lo, n + 1, v->e, &ml, &el);
    fm_to_m64(hi, n + 1, v->e, &mh, &eh);
    const u64 rl = proven_float_pack_(v->neg, el, ml, b32), rh = proven_float_pack_(v->neg, eh, mh, b32);
    if (rl != rh) return false;
    *out = rl;
    return true;
}

/* v rounded to nearest, without a test: the last resort after the largest precision. */
static u64 mp_round_nearest(const mp_t *v, bool b32) {
    u64 m;
    proven_i64 e;
    fm_to_m64(v->d, v->n, v->e, &m, &e);
    return proven_float_pack_(v->neg, e, m, b32);
}

// -----------------------------------------------------------------------------
// The functions on multiple-precision arguments
// -----------------------------------------------------------------------------

static void mp_exp_fast(mp_t *r, const mp_t *x, int m);
static void mp_log_fast(mp_t *r, const mp_t *x, int m);
static void mp_sincos_fast(mp_t *S, mp_t *C, const mp_t *r, int m);
static void mp_atan_fast(mp_t *out, const mp_t *a, const mp_t *b, int m);
#define FM_FAST(m) ((m) + 1 <= FM_TAB_LIMBS)    /* whether a precision of m limbs is the tables' */

/*
 * exp(x) for |x| < 2^11, at n limbs.
 * x = k ln2 + t: k from x / ln2 to the nearest, t by ln2 at n + 3 limbs (|t| <= ln2 / 2, its
 * absolute error under 2^-(32n + 80), which is what exp's relative error is). Then exp(t) =
 * exp(t / 2^S)^(2^S): a Taylor sum of the small argument at n + 1 limbs, its terms below
 * 2^-(32(n + 1) + 2) left out (the rest of the series is smaller than the last term kept), then
 * S squarings, each doubling the relative error and adding two units. Below 2^(S + 10) units of
 * the (n + 1)-limb result in all, which is under 2^-16 of a unit at n limbs.
 */
static void mp_exp(mp_t *r, const mp_t *x, int n) {
    if (FM_FAST(n)) { mp_exp_fast(r, x, n); return; }
    if (!x->n) { mp_one(r, n); return; }
    mp_t c, t, kk, sum, term;
    mp_const(&c, FM_INV_LN2, 2, FM_INV_LN2_EXP, 2);
    mp_mul(&t, x, &c, 3);
    const proven_i64 k = mp_round_long(&t);
    mp_const(&c, FM_LN2, FM_CONST_LIMBS, FM_LN2_EXP, n + 3);
    mp_set_u64(&kk, k < 0, (u64)(k < 0 ? -k : k), 0, 2);
    mp_mul(&kk, &c, &kk, n + 3);
    mp_sub(&t, x, &kk, n + 3);
    const int S = n <= 4 ? 6 : 10;
    const int m = n + 1;
    if (t.n) t.e -= S;
    mp_one(&sum, m);
    mp_copy(&term, &t, m);
    mp_add(&sum, &sum, &term, m);
    for (u32 j = 2; term.n; j++) {
        mp_mul(&term, &term, &t, m);
        mp_div_small(&term, &term, j, m);
        if (!term.n || mp_top(&term) < -(((proven_i64)32) * m + 2)) break;
        mp_add(&sum, &sum, &term, m);
    }
    for (int i = 0; i < S; i++) mp_mul(&sum, &sum, &sum, m);
    mp_copy(r, &sum, n);
    r->e += k;
}

/*
 * log(x) for an exact positive x of at most two limbs (a binary64 or binary32 value), at n
 * limbs. x = m 2^E with m in [sqrt(1/2), sqrt(2)), log x = E ln2 + log m, and the two do not
 * cancel (|E ln2| >= 0.69 > 0.35 >= |log m|). log m by Newton's iteration on exp,
 * y += m exp(-y) - 1, from a 19-bit series, doubling the bits each step, the last one twice.
 * The correction m exp(-y) - 1 has an absolute error of a few units at the step's precision,
 * which is relative to 1, not to log m: when E is 0 and m is near 1 the precision is raised by
 * the bits of |m - 1| below 1, so that the error stays relative to the result.
 */
static void mp_log(mp_t *r, const mp_t *x, int n) {
    if (FM_FAST(n)) { mp_log_fast(r, x, n); return; }
    proven_i64 E = mp_top(x);
    mp_t m = *x;
    m.e -= E;                                               /* [1, 2) */
    if (m.d[m.n - 1] > 0xb504f333u) { m.e -= 1; E += 1; }    /* above sqrt(2): halve */
    mp_t one, t, two, d, u, u2, a, b, y, ny, ey, c;
    mp_one(&one, 4);
    mp_sub(&t, &m, &one, 4);                                /* exact */
    if (!t.n) {
        if (!E) { mp_zero(r); return; }
        mp_zero(&y);
    } else {
        int extra = 0;
        if (!E && mp_top(&t) < 0) extra = (int)(-mp_top(&t));
        const int xl = (extra + 31) / 32;
        const int n2 = n + 1 + xl;
        /* u = t / (2 + t), y0 = 2u (1 + u^2/3 + u^4/5): |u| <= 0.172, within 2u^7/7 < 2^-19 |y|. */
        const int p0 = 2 + xl;
        mp_set_u64(&two, false, 2, 0, 2);
        mp_add(&d, &two, &t, p0);
        mp_div(&u, &t, &d, p0);
        mp_mul(&u2, &u, &u, p0);
        mp_div_small(&a, &u2, 5, p0);
        mp_mul(&a, &a, &u2, p0);
        mp_div_small(&b, &u2, 3, p0);
        mp_one(&y, p0);
        mp_add(&y, &y, &a, p0);
        mp_add(&y, &y, &b, p0);
        mp_mul(&y, &y, &u, p0);
        y.e += 1;
        int acc = 19;
        bool last = false;
        for (;;) {
            int p = xl + (2 * acc) / 32 + 2;
            if (p > n2) p = n2;
            ny = y;
            ny.neg = !ny.neg;
            mp_exp(&ey, &ny, p);
            mp_mul(&ey, &ey, &m, p);
            mp_one(&one, p);
            mp_sub(&ey, &ey, &one, p);
            mp_add(&y, &y, &ey, p);
            acc = 2 * acc;
            if (acc > 32 * (p - xl) - 8) acc = 32 * (p - xl) - 8;
            if (p == n2) {
                if (last) break;
                last = acc >= 32 * (n2 - xl) - 8;
            }
        }
    }
    if (E) {
        mp_const(&c, FM_LN2, FM_CONST_LIMBS, FM_LN2_EXP, n + 2);
        mp_mul_small(&c, &c, (u32)(E < 0 ? -E : E), n + 2);
        c.neg = E < 0;
        mp_add(&y, &y, &c, n + 2);
    }
    mp_copy(r, &y, n);
}

/*
 * sin and cos of |r| <= pi/4 + a little, at n limbs, by their Taylor series: alternating,
 * terms decreasing from the second, stopped below 2^-(32n + 2) of the sum, so the series' rest
 * is under one unit. Each term is the last times r^2 / (k (k + 1)), three operations; the sums
 * do not cancel (cos r >= 0.7, sin r / r >= 0.9). A few units per term at n limbs in all.
 */
static void mp_sincos(mp_t *S, mp_t *C, const mp_t *r, int n) {
    mp_t r2, term;
    if (!r->n) {
        if (S) mp_zero(S);
        if (C) mp_one(C, n);
        return;
    }
    mp_mul(&r2, r, r, n);
    r2.neg = true;                                          /* -r^2 */
    if (S) {
        mp_copy(S, r, n);
        mp_copy(&term, r, n);
        const proven_i64 stop = mp_top(r) - (((proven_i64)32) * n + 2);
        for (u32 k = 2; ; k += 2) {
            mp_mul(&term, &term, &r2, n);
            mp_div_small(&term, &term, k * (k + 1), n);
            if (!term.n || mp_top(&term) < stop) break;
            mp_add(S, S, &term, n);
        }
    }
    if (C) {
        mp_one(C, n);
        mp_one(&term, n);
        const proven_i64 stop = -(((proven_i64)32) * n + 2);
        for (u32 k = 1; ; k += 2) {
            mp_mul(&term, &term, &r2, n);
            mp_div_small(&term, &term, k * (k + 1), n);
            if (!term.n || mp_top(&term) < stop) break;
            mp_add(C, C, &term, n);
        }
    }
}

/*
 * Reduction for sin, cos and tan: x = (q + f) pi/2 with q an integer (only q mod 4 kept) and
 * |f| <= 1/2, so r = f pi/2 is in [-pi/4, pi/4]. Payne and Hanek's way: only the bits of 2/pi
 * that reach the fraction or the two bits of q mod 4 are multiplied, from the table of 2,304
 * fraction bits (enough for any binary64 exponent and the largest precision). The window is
 * widened until the fraction, however much it cancels, keeps 32(n + 1) + 40 bits above the
 * window's error. Below pi/4 nothing is reduced.
 */
static int mp_reduce(mp_t *r, u64 mant, int ex, bool neg, int n) {
    /* x = mant 2^ex, mant < 2^53 */
    {
        mp_t xa, q4;
        mp_set_u64(&xa, false, mant, ex, 2);
        mp_const(&q4, FM_PI_2, FM_CONST_LIMBS, FM_PI_2_EXP, 3);
        q4.e -= 1;                                          /* pi/4, truncated: an x at it is reduced, harmlessly */
        if (mp_cmp_abs(&xa, &q4) < 0) {
            mp_set_u64(r, neg, mant, ex, n);
            return 0;
        }
    }
    const proven_i64 i0 = ex - 1 > 1 ? ex - 1 : 1;               /* bits i < ex - 1 of 2/pi only add multiples of 4 */
    int wl = n + 7;                                         /* window limbs */
    for (;;) {
        if (wl > 40) wl = 40;
        proven_i64 avail = ((proven_i64)32) * FM_TWO_OVER_PI_LIMBS - (i0 - 1);
        if (((proven_i64)32) * wl > avail) wl = (int)(avail / 32);
        /* window bit k (from its LSB) is 2/pi's fraction bit i0 + 32 wl - 1 - k */
        u32 Wn[42], P[45];
        for (int j = 0; j < wl; j++) {
            /* limb j holds fraction bits [i0 + 32(wl - 1 - j), +32), the first of them its MSB */
            const proven_i64 first = i0 + ((proven_i64)32) * (wl - 1 - j);     /* 1-based fraction bit index */
            const proven_i64 off = first - 1;                     /* 0-based from the table's MSB */
            const proven_i64 li = off >> 5, bsh = off & 31;
            u32 v = li < FM_TWO_OVER_PI_LIMBS ? FM_TWO_OVER_PI[li] << bsh : 0;
            if (bsh && li + 1 < FM_TWO_OVER_PI_LIMBS) v |= FM_TWO_OVER_PI[li + 1] >> (32 - bsh);
            Wn[j] = v;
        }
        const u32 mm[2] = { (u32)mant, (u32)(mant >> 32) };
        for (int j = 0; j < wl + 2; j++) P[j] = 0;
        for (int a = 0; a < 2; a++) {
            u64 c = 0;
            for (int j = 0; j < wl; j++) { c += (u64)mm[a] * Wn[j] + P[a + j]; P[a + j] = (u32)c; c >>= 32; }
            P[a + wl] = (u32)c;
        }
        const int pl = wl + 2;
        /* P * 2^(ex - i0 - 32 wl + 1): the binary point is at bit sh of P */
        const proven_i64 sh = i0 + ((proven_i64)32) * wl - 1 - ex;
        int q = fm_getbit(P, pl, sh) | (fm_getbit(P, pl, sh + 1) << 1);
        const bool up = fm_getbit(P, pl, sh - 1) != 0;
        /* the fraction: bits below sh; if its top bit is set, f = frac - 1 (and q + 1) */
        u32 F[45];
        for (int j = 0; j < pl; j++) F[j] = 0;
        for (proven_i64 b = 0; b < sh; b += 32) {
            u32 v = fm_get32(P, pl, b);
            if (b + 32 > sh) v &= ((u32)1 << (sh - b)) - 1u;
            F[b >> 5] = v;
        }
        bool fneg = false;
        if (up) {
            /* 2^sh - frac */
            u64 br = 0;
            for (int j = 0; j < pl; j++) { const u64 tv = (u64)0 - F[j] - br; F[j] = (u32)tv; br = (tv >> 63) & 1u; }
            for (proven_i64 b = sh; b < ((proven_i64)32) * pl; b++) F[b >> 5] &= ~((u32)1 << (b & 31));
            fneg = true;
            q = (q + 1) & 3;
        }
        mp_t f;
        mp_set_buf(&f, F, pl, -sh, fneg, n + 2);
        /* the window's error is under 2^54 units of P's last bit (mant times the bits left out) */
        if (f.n && mp_top(&f) + sh >= 54 + ((proven_i64)32) * (n + 1) + 40) {
            mp_t c;
            mp_const(&c, FM_PI_2, FM_CONST_LIMBS, FM_PI_2_EXP, n + 2);
            mp_mul(r, &f, &c, n);
            if (neg) { r->neg = !r->neg; q = (4 - q) & 3; }
            return q;
        }
        if (wl == 40 || ((proven_i64)32) * wl >= avail) {
            /* cannot happen for binary64 arguments (the closest to a multiple of pi/2 cancels about 62 bits) */
            mp_t c;
            mp_const(&c, FM_PI_2, FM_CONST_LIMBS, FM_PI_2_EXP, n + 2);
            mp_mul(r, &f, &c, n);
            if (neg) { r->neg = !r->neg; q = (4 - q) & 3; }
            return q;
        }
        wl += 4;
    }
}

enum { FM_SIN, FM_COS, FM_TAN };

/* sin, cos or tan of x = mant 2^ex (with its sign), at n limbs. */
static void mp_trig(mp_t *out, int which, u64 mant, int ex, bool neg, int n) {
    mp_t r, S, C;
    const int m = n + 1;
    const int q = mp_reduce(&r, mant, ex, neg, m);
    if (which == FM_TAN) {
        mp_t t;
        if (FM_FAST(m)) mp_sincos_fast(&S, &C, &r, m); else mp_sincos(&S, &C, &r, m);
        if (q & 1) { mp_div(&t, &C, &S, m); t.neg = !t.neg; }
        else mp_div(&t, &S, &C, m);
        mp_copy(out, &t, n);
        return;
    }
    const bool use_cos = (which == FM_COS) != ((q & 1) != 0);
    if (FM_FAST(m)) mp_sincos_fast(use_cos ? (mp_t *)0 : &S, use_cos ? &C : (mp_t *)0, &r, m);
    else mp_sincos(use_cos ? (mp_t *)0 : &S, use_cos ? &C : (mp_t *)0, &r, m);
    mp_t *v = use_cos ? &C : &S;
    bool flip;
    if (which == FM_SIN) flip = q >= 2;
    else flip = q == 1 || q == 2;
    mp_copy(out, v, n);
    if (flip && out->n) out->neg = !out->neg;
}

/*
 * atan2(y, x) for finite nonzero x and y: a = |y|, b = |x|, swapped so that a <= b, then
 * t = atan(a / b) in [0, pi/4] by Newton's iteration on a cos t - b sin t = 0,
 * t += (a cos t - b sin t) / (b cos t + a sin t), from t0 = z / (1 + 0.28 z^2) (z = a / b,
 * within 0.005), doubling the bits each step, the last one twice. The numerator cancels, but
 * its error is relative to a and so to t: the iteration keeps t's relative error at a few
 * units. Then pi/2 - t if swapped, pi - that if x < 0 (neither cancels: t <= pi/4), and y's sign.
 */
static void mp_atan2(mp_t *out, const mp_t *yv, const mp_t *xv, int n) {
    mp_t a = *yv, b = *xv, z, t, num, den, S, C, s1, s2, c;
    a.neg = false;
    b.neg = false;
    const bool swap = mp_cmp_abs_gt(&a, &b);
    if (swap) { const mp_t tmp = a; a = b; b = tmp; }
    const int m = n + 2;
    if (FM_FAST(m)) {
        mp_atan_fast(&t, &a, &b, m);
        goto adjust;
    }
    mp_div(&z, &a, &b, 2);
    mp_mul(&t, &z, &z, 2);
    mp_mul_small(&t, &t, 9, 2);
    t.e -= 5;                                               /* 0.28125 z^2 */
    mp_one(&c, 2);
    mp_add(&t, &t, &c, 2);
    mp_div(&t, &z, &t, 2);
    int acc = 7;
    bool last = false;
    for (;;) {
        int p = (2 * acc) / 32 + 2;
        if (p > m) p = m;
        mp_sincos(&S, &C, &t, p);
        mp_mul(&s1, &a, &C, p);
        mp_mul(&s2, &b, &S, p);
        mp_sub(&num, &s1, &s2, p);
        mp_mul(&s1, &b, &C, p);
        mp_mul(&s2, &a, &S, p);
        mp_add(&den, &s1, &s2, p);
        mp_div(&num, &num, &den, p);
        mp_add(&t, &t, &num, p);
        acc = 2 * acc;
        if (acc > 32 * p - 8) acc = 32 * p - 8;
        if (p == m) {
            if (last) break;
            last = acc >= 32 * m - 8;
        }
    }
adjust:
    if (swap) {
        mp_const(&c, FM_PI_2, FM_CONST_LIMBS, FM_PI_2_EXP, m);
        mp_sub(&t, &c, &t, m);
    }
    if (xv->neg) {
        mp_const(&c, FM_PI_2, FM_CONST_LIMBS, FM_PI_2_EXP, m);
        c.e += 1;                                           /* pi */
        mp_sub(&t, &c, &t, m);
    }
    mp_copy(out, &t, n);
    if (yv->neg && out->n) out->neg = true;
}

/* |x|^y = exp(y log |x|) for exact finite nonzero x and y, |y log x| < 2^11, at n limbs. log at
 * n + 2 limbs makes y log x's absolute error under 2^-(32n + 49), which is exp's relative one. */
static void mp_pow_core(mp_t *out, const mp_t *x, const mp_t *y, int n) {
    mp_t L, z, ax = *x;
    ax.neg = false;
    mp_log(&L, &ax, n + 2);
    mp_mul(&z, &L, y, n + 2);
    mp_exp(out, &z, n);
}

// -----------------------------------------------------------------------------
// The first Ziv level: 192-bit tables and short series (at most FM_TAB_LIMBS limbs inside)
// -----------------------------------------------------------------------------

/*
 * At the first level each function reduces its argument by a table entry to below 2^-7 and
 * sums a short series by Horner's rule from tabulated coefficients: one multiplication and
 * one addition a term, a few units of error in all, inside FM_TAB_LIMBS limbs. Above it the
 * functions above take over, without tables, at any precision.
 */

static void mp_tab(mp_t *r, const fm_tab_t *t, int m) {
    if (t->d[FM_TAB_LIMBS - 1] == 0) { mp_zero(r); return; }
    mp_const(r, t->d, FM_TAB_LIMBS, t->e, m);
    r->neg = t->neg != 0;
}

/* The floor of a nonnegative a < 2^30. */
static proven_i64 mp_floor_long(const mp_t *a) {
    if (!a->n || mp_top(a) < 0) return 0;
    return (proven_i64)(fm_get32(a->d, a->n, -a->e) & 0x7fffffffu);
}

/* The terms a series on t needs at m limbs: the first left out, |t|^count |c_count|, under
 * 2^-(32m + 4) (c_count <= 1, the coefficients falling). */
static int fm_terms(const mp_t *t, int m) {
    if (!t->n) return 1;
    const proven_i64 tb = -(mp_top(t) + 1);                       /* |t| < 2^-tb */
    if (tb <= 0) return 64;
    int c = 1;                                              /* by addition: no division helper on small cores */
    for (proven_i64 bits = tb; bits < ((proven_i64)32) * m + 4 && c < 64; bits += tb) c++;
    return c + 1;
}

/* Fixed point for the series: a sign and a magnitude of W limbs, value = M / 2^(32W - 8).
 * The series' values are near 1 and their variables small, so an absolute error at that scale
 * is a relative one, and nothing is normalized or aligned. */
typedef struct { bool neg; u32 d[FM_TAB_LIMBS]; } fx_t;

static proven_i64 fx_frac(int W) { return ((proven_i64)32) * W - 8; }

static void fx_mul(fx_t *r, const fx_t *a, const fx_t *b, int W) {
    u32 P[2 * FM_TAB_LIMBS] = { 0 };
    for (int i = 0; i < W; i++) {
        u64 c = 0;
        for (int j = 0; j < W; j++) { c += (u64)a->d[i] * b->d[j] + P[i + j]; P[i + j] = (u32)c; c >>= 32; }
        P[i + W] = (u32)c;
    }
    fm_place(r->d, W, P, 2 * W, -fx_frac(W));
    r->neg = a->neg != b->neg;
}

static void fx_add(fx_t *r, const fx_t *a, const fx_t *b, int W) {
    if (a->neg == b->neg) {
        u64 c = 0;
        for (int i = 0; i < W; i++) { c += (u64)a->d[i] + b->d[i]; r->d[i] = (u32)c; c >>= 32; }
        r->neg = a->neg;
        return;
    }
    int cmp = 0;
    for (int i = W - 1; i >= 0 && !cmp; i--) cmp = a->d[i] > b->d[i] ? 1 : a->d[i] < b->d[i] ? -1 : 0;
    const fx_t *x = cmp >= 0 ? a : b, *y = cmp >= 0 ? b : a;
    u64 br = 0;
    for (int i = 0; i < W; i++) { const u64 t = (u64)x->d[i] - y->d[i] - br; r->d[i] = (u32)t; br = (t >> 63) & 1u; }
    r->neg = x->neg;
}

/* acc = sum over q < count of (+-) cf[k0 + step q] t^q, by Horner's rule, the coefficients
 * from a fixed-point table (FM_TAB_LIMBS limbs, of which the top W are taken), at W limbs. */
static void mp_horner(mp_t *acc, const mp_t *t, const u32 (*cf)[FM_TAB_LIMBS], int k0, int step, int count, bool alt, int m) {
    const int W = m;
    fx_t x, a, c;
    x.neg = t->neg;
    if (t->n) fm_place(x.d, W, t->d, t->n, t->e + fx_frac(W)); else for (int i = 0; i < W; i++) x.d[i] = 0;
    int q = count - 1;
    for (int i = 0; i < W; i++) a.d[i] = cf[k0 + step * q][FM_TAB_LIMBS - W + i];
    a.neg = alt && (q & 1);
    for (q = count - 2; q >= 0; q--) {
        fx_mul(&a, &a, &x, W);
        for (int i = 0; i < W; i++) c.d[i] = cf[k0 + step * q][FM_TAB_LIMBS - W + i];
        c.neg = alt && (q & 1);
        fx_add(&a, &a, &c, W);
    }
    mp_set_buf(acc, a.d, W, -fx_frac(W), a.neg, m);
}

/* exp(x) for |x| < 2^11 at m <= FM_TAB_LIMBS - 1 limbs: x = (64k + j) ln2/64 + t, |t| <= ln2/128,
 * exp(x) = 2^k 2^(j/64) exp(t), exp(t) by its Taylor series. */
static void mp_exp_fast(mp_t *r, const mp_t *x, int m) {
    mp_t c, t, kk, p;
    if (!x->n) { mp_one(r, m); return; }
    mp_const(&c, FM_INV_LN2, 2, FM_INV_LN2_EXP, 2);
    mp_mul(&t, x, &c, 3);
    t.e += 6;
    const proven_i64 k64 = mp_round_long(&t);
    mp_const(&c, FM_LN2, FM_CONST_LIMBS, FM_LN2_EXP, m + 3);
    mp_set_u64(&kk, k64 < 0, (u64)(k64 < 0 ? -k64 : k64), -6, 2);
    mp_mul(&kk, &c, &kk, m + 3);
    mp_sub(&t, x, &kk, m + 3);
    const int mi = m + 1;
    int cnt = fm_terms(&t, mi);
    if (cnt > 31) cnt = 31;
    mp_horner(&p, &t, FM_INV_FACT_FX, 0, 1, cnt, false, mi);
    const int j = (int)(k64 & 63);
    const proven_i64 k = (k64 - j) / 64;
    mp_tab(&c, &FM_EXP2_64[j], mi);
    mp_mul(&p, &p, &c, mi);
    mp_copy(r, &p, m);
    r->e += k;
}

/* log(x) for an exact positive x of at most two limbs, at m <= FM_TAB_LIMBS - 1 limbs:
 * x = mm 2^E with mm in [0.75, 1.5); near 1 log1p(mm - 1) directly, else
 * log1p(mm R_j / 2^16 - 1) - log(R_j / 2^16), the argument below 2^-8 either way. The products
 * and differences before the series are exact. */
static void mp_log_fast(mp_t *r, const mp_t *x, int m) {
    proven_i64 E = mp_top(x);
    mp_t mm = *x, one, t, L0, c, p;
    mm.e -= E;                                              /* [1, 2) */
    if (mm.d[mm.n - 1] >= 0xc0000000u) { mm.e -= 1; E += 1; }  /* [0.75, 1.5) */
    const int mi = m + 1;
    mp_one(&one, 4);
    mp_sub(&t, &mm, &one, 4);
    bool tab = false;
    if (t.n && mp_top(&t) >= -8) {
        mp_t q;
        mp_set_u64(&q, false, 3, -2, 2);                    /* 0.75 */
        mp_sub(&q, &mm, &q, 4);
        q.e += 8;
        const proven_i64 j = mp_floor_long(&q);                  /* (mm - 0.75) 256, 0 .. 191 */
        mp_mul_small(&t, &mm, FM_LOG_R[j], 4);
        t.e -= 16;
        mp_sub(&t, &t, &one, 4);
        mp_tab(&L0, &FM_LOG_RL[j], mi);
        tab = true;
    }
    if (t.n) {
        int cnt = fm_terms(&t, mi) + 1;
        if (cnt > 46) cnt = 46;
        mp_horner(&p, &t, FM_INV_K_FX, 1, 1, cnt, true, mi);  /* 1 - t/2 + t^2/3 - ... */
        mp_mul(&p, &p, &t, mi);
    } else {
        mp_zero(&p);
    }
    if (tab) mp_add(&p, &p, &L0, mi);
    if (E) {
        mp_const(&c, FM_LN2, FM_CONST_LIMBS, FM_LN2_EXP, mi + 1);
        mp_mul_small(&c, &c, (u32)(E < 0 ? -E : E), mi + 1);
        c.neg = E < 0;
        mp_add(&p, &p, &c, mi);
    }
    mp_copy(r, &p, m);
}

/* sin and cos of |r| <= pi/4 + a little at m <= FM_TAB_LIMBS - 1 limbs: r = j/64 + d, |d| <= 1/128,
 * from sin(j/64), cos(j/64) and the series of sin d and cos d in d^2. */
static void mp_sincos_fast(mp_t *S, mp_t *C, const mp_t *r, int m) {
    const int mi = m + 1;
    if (!r->n) { if (S) mp_zero(S); if (C) mp_one(C, m); return; }
    mp_t q = *r, jj, d, d2, sd, cd, Sj, Cj, t1, t2;
    q.e += 6;
    const proven_i64 j = mp_round_long(&q);
    mp_set_u64(&jj, j < 0, (u64)(j < 0 ? -j : j), -6, 1);
    mp_sub(&d, r, &jj, mi);
    mp_mul(&d2, &d, &d, mi);
    int cnt = fm_terms(&d2, mi);
    if (cnt > 15) cnt = 15;
    mp_horner(&sd, &d2, FM_INV_FACT_FX, 1, 2, cnt, true, mi);
    mp_mul(&sd, &sd, &d, mi);
    mp_horner(&cd, &d2, FM_INV_FACT_FX, 0, 2, cnt, true, mi);
    if (j == 0) {
        if (S) mp_copy(S, &sd, m);
        if (C) mp_copy(C, &cd, m);
        return;
    }
    const proven_i64 aj = j < 0 ? -j : j;
    mp_tab(&Sj, &FM_SIN_64[aj], mi);
    if (j < 0) Sj.neg = true;
    mp_tab(&Cj, &FM_COS_64[aj], mi);
    if (S) {
        mp_mul(&t1, &Sj, &cd, mi);
        mp_mul(&t2, &Cj, &sd, mi);
        mp_add(&t1, &t1, &t2, mi);
        mp_copy(S, &t1, m);
    }
    if (C) {
        mp_mul(&t1, &Cj, &cd, mi);
        mp_mul(&t2, &Sj, &sd, mi);
        mp_sub(&t1, &t1, &t2, mi);
        mp_copy(C, &t1, m);
    }
}

/* atan(a / b) for 0 < a <= b at m <= FM_TAB_LIMBS - 1 limbs: z = a / b, j = round(64 z),
 * atan z = atan(j/64) + atan w, w = (z - j/64) / (1 + z j/64), |w| <= 2^-7. */
static void mp_atan_fast(mp_t *out, const mp_t *a, const mp_t *b, int m) {
    const int mi = m + 1;
    mp_t z, q, c, w, num, den, w2, p, A, one;
    mp_div(&z, a, b, mi);
    q = z;
    q.e += 6;
    const proven_i64 j = mp_round_long(&q);
    if (j == 0) {
        w = z;
    } else {
        mp_set_u64(&c, false, (u64)j, -6, 1);
        mp_sub(&num, &z, &c, mi);
        mp_mul(&den, &z, &c, mi);
        mp_one(&one, 1);
        mp_add(&den, &den, &one, mi);
        mp_div(&w, &num, &den, mi);
    }
    if (w.n) {
        mp_mul(&w2, &w, &w, mi);
        int cnt = fm_terms(&w2, mi) + 1;
        if (cnt > 23) cnt = 23;
        mp_horner(&p, &w2, FM_INV_K_FX, 1, 2, cnt, true, mi);   /* 1 - w^2/3 + w^4/5 - ... */
        mp_mul(&p, &p, &w, mi);
    } else {
        mp_zero(&p);
    }
    if (j) {
        mp_tab(&A, &FM_ATAN_64[j], mi);
        mp_add(&p, &p, &A, mi);
    }
    mp_copy(out, &p, m);
}

// -----------------------------------------------------------------------------
// Values of the formats
// -----------------------------------------------------------------------------

#define FMT_MANT(s) ((s) ? 23 : 52)
#define FMT_SIGNB(s) ((s) ? (u64)0x80000000u : (u64)0x8000000000000000u)
#define FMT_INFB(s) ((s) ? (u64)0x7f800000u : (u64)0x7ff0000000000000u)
#define FMT_QUIETB(s) ((s) ? (u64)0x00400000u : (u64)0x0008000000000000u)
#define FMT_NANB(s) ((s) ? (u64)0x7fc00000u : (u64)0x7ff8000000000000u)
#define FMT_ONEB(s) ((s) ? (u64)0x3f800000u : (u64)0x3ff0000000000000u)

enum { FC_ZERO, FC_FIN, FC_INF, FC_NAN };

typedef struct {
    int cls;
    bool neg;
    u64 mant;                          /* with the hidden bit; value = mant * 2^ex */
    int ex;
} fm_val;

static fm_val fm_dec(u64 u, bool s) {
    const int mb = FMT_MANT(s), emax = s ? 255 : 2047, bias = s ? 127 : 1023;
    fm_val v = { FC_FIN, (u & FMT_SIGNB(s)) != 0, 0, 0 };
    const u64 frac = u & (((u64)1 << mb) - 1);
    int be = (int)((u >> mb) & (u64)emax);
    if (be == emax) { v.cls = frac ? FC_NAN : FC_INF; return v; }
    if (be == 0) {
        if (!frac) { v.cls = FC_ZERO; return v; }
        be = 1;
        v.mant = frac;
    } else {
        v.mant = frac | ((u64)1 << mb);
    }
    v.ex = be - bias - mb;
    return v;
}

static void fm_mp_of(mp_t *r, const fm_val *v, int n) { mp_set_u64(r, v->neg, v->mant, v->ex, n); }

/* The Ziv levels: limbs tried in turn. */
static int fm_level(bool s, int i) {
    static const int b64[4] = { 3, 6, 12, 24 }, b32[4] = { 2, 4, 8, 12 };
    return s ? b32[i] : b64[i];
}
#define FM_LEVELS 4

// -----------------------------------------------------------------------------
// The functions on the bits of either format
// -----------------------------------------------------------------------------

static u64 fm_exp(u64 u, bool s) {
    const fm_val v = fm_dec(u, s);
    if (v.cls == FC_NAN) return u | FMT_QUIETB(s);
    if (v.cls == FC_INF) return v.neg ? 0 : u;
    if (v.cls == FC_ZERO) return FMT_ONEB(s);
    /* beyond 2^11 the result is surely infinite or below half the smallest subnormal */
    {
        mp_t t;
        fm_mp_of(&t, &v, 2);
        if (mp_top(&t) >= 11) return v.neg ? 0 : FMT_INFB(s);
    }
    mp_t x, r;
    u64 out = 0;
    for (int i = 0; i < FM_LEVELS; i++) {
        const int n = fm_level(s, i);
        fm_mp_of(&x, &v, 2);
        mp_exp(&r, &x, n);
        if (mp_round_test(&r, s, &out)) return out;
    }
    return mp_round_nearest(&r, s);
}

static u64 fm_log(u64 u, bool s) {
    const fm_val v = fm_dec(u, s);
    if (v.cls == FC_NAN) return u | FMT_QUIETB(s);
    if (v.cls == FC_ZERO) return FMT_SIGNB(s) | FMT_INFB(s);
    if (v.neg) return FMT_NANB(s);
    if (v.cls == FC_INF) return u;
    if (u == FMT_ONEB(s)) return 0;
    mp_t x, r;
    u64 out = 0;
    for (int i = 0; i < FM_LEVELS; i++) {
        const int n = fm_level(s, i);
        fm_mp_of(&x, &v, 2);
        mp_log(&r, &x, n);
        if (mp_round_test(&r, s, &out)) return out;
    }
    return mp_round_nearest(&r, s);
}

static u64 fm_trig(u64 u, bool s, int which) {
    const fm_val v = fm_dec(u, s);
    if (v.cls == FC_NAN) return u | FMT_QUIETB(s);
    if (v.cls == FC_INF) return FMT_NANB(s);
    if (v.cls == FC_ZERO) return which == FM_COS ? FMT_ONEB(s) : u;
    mp_t r;
    u64 out = 0;
    for (int i = 0; i < FM_LEVELS; i++) {
        const int n = fm_level(s, i);
        mp_trig(&r, which, v.mant, v.ex, v.neg, n);
        if (mp_round_test(&r, s, &out)) return out;
    }
    return mp_round_nearest(&r, s);
}

/* k * pi/4 (k = 1, 2, 3, 4) rounded, with a sign. */
static u64 fm_pi_mult(int k, bool neg, bool s) {
    mp_t c, r;
    mp_const(&c, FM_PI_2, FM_CONST_LIMBS, FM_PI_2_EXP, 8);
    mp_mul_small(&r, &c, (u32)k, 8);
    r.e -= 1;
    r.neg = neg;
    u64 out = 0;
    if (mp_round_test(&r, s, &out)) return out;
    return mp_round_nearest(&r, s);
}

static u64 fm_atan2(u64 yb, u64 xb, bool s) {
    const fm_val y = fm_dec(yb, s), x = fm_dec(xb, s);
    if (y.cls == FC_NAN) return yb | FMT_QUIETB(s);
    if (x.cls == FC_NAN) return xb | FMT_QUIETB(s);
    if (y.cls == FC_ZERO) {
        if (!x.neg) return yb;                              /* +-0 for x = +0 or x > 0 */
        return fm_pi_mult(4, y.neg, s);                     /* +-pi for x = -0 or x < 0 */
    }
    if (y.cls == FC_INF) {
        if (x.cls == FC_INF) return fm_pi_mult(x.neg ? 3 : 1, y.neg, s);
        return fm_pi_mult(2, y.neg, s);
    }
    if (x.cls == FC_ZERO) return fm_pi_mult(2, y.neg, s);
    if (x.cls == FC_INF) return x.neg ? fm_pi_mult(4, y.neg, s) : (y.neg ? FMT_SIGNB(s) : 0);
    mp_t ym, xm, r;
    u64 out = 0;
    for (int i = 0; i < FM_LEVELS; i++) {
        const int n = fm_level(s, i);
        fm_mp_of(&ym, &y, 2);
        fm_mp_of(&xm, &x, 2);
        mp_atan2(&r, &ym, &xm, n);
        if (mp_round_test(&r, s, &out)) return out;
    }
    return mp_round_nearest(&r, s);
}

/* mant made odd: value = m * 2^e. */
static void fm_odd(u64 mant, int ex, u64 *m, int *e) {
    while (!(mant & 1u)) { mant >>= 1; ex++; }
    *m = mant;
    *e = ex;
}

/* m^k * 2^e exactly (m < 2^53, k <= 64), rounded once into the format. */
static u64 fm_exact_power(bool neg, u64 m, unsigned k, proven_i64 e, bool s) {
    u32 acc[120];
    int len = 1;
    acc[0] = 1;
    const u32 m0 = (u32)m, m1 = (u32)(m >> 32);
    for (unsigned i = 0; i < k; i++) {
        u32 nxt[121];
        for (int j = 0; j <= len + 1; j++) nxt[j] = 0;
        for (int j = 0; j < len; j++) {
            u64 c = (u64)acc[j] * m0 + nxt[j];
            nxt[j] = (u32)c;
            u64 c2 = (u64)acc[j] * m1 + nxt[j + 1] + (c >> 32);
            nxt[j + 1] = (u32)c2;
            u64 c3 = nxt[j + 2] + (c2 >> 32);
            nxt[j + 2] = (u32)c3;
        }
        len += 2;
        while (len > 1 && nxt[len - 1] == 0) len--;
        for (int j = 0; j < len; j++) acc[j] = nxt[j];
    }
    u64 mm;
    proven_i64 me;
    fm_to_m64(acc, len, e, &mm, &me);
    return proven_float_pack_(neg, me, mm, s);
}

/* The integer 2^k-th root of v if v is a perfect 2^k-th power, else 0. */
static u64 fm_root2k(u64 v, int k) {
    for (int i = 0; i < k; i++) {
        u64 lo = 0, hi = (u64)1 << 32;
        while (hi - lo > 1) {
            const u64 mid = (lo + hi) >> 1;
            if (mid * mid <= v) lo = mid; else hi = mid;
        }
        if (lo * lo != v) return 0;
        v = lo;
    }
    return v;
}

static u64 fm_pow(u64 xb, u64 yb, bool s) {
    const fm_val x = fm_dec(xb, s), y = fm_dec(yb, s);
    const u64 one = FMT_ONEB(s), sb = FMT_SIGNB(s), inf = FMT_INFB(s);
    /* pow(x, +-0) = 1 and pow(+1, y) = 1 even for a quiet NaN; a signalling NaN gives itself,
     * quieted (IEEE 754-2019 9.2.1), as the FPU path does */
    const bool xsig = x.cls == FC_NAN && !(xb & FMT_QUIETB(s)), ysig = y.cls == FC_NAN && !(yb & FMT_QUIETB(s));
    if (y.cls == FC_ZERO && !xsig) return one;
    if (xb == one && !ysig) return one;
    if (x.cls == FC_NAN) return xb | FMT_QUIETB(s);
    if (y.cls == FC_NAN) return yb | FMT_QUIETB(s);
    /* y's integer class: y = ym 2^ye with ym odd */
    bool yint = false, yodd = false;
    u64 ym = 0;
    int ye = 0;
    if (y.cls == FC_FIN) {
        fm_odd(y.mant, y.ex, &ym, &ye);
        yint = ye >= 0;
        yodd = ye == 0;
    }
    if (y.cls == FC_INF) {
        if (xb == (one | sb)) return one;
        const bool below1 = x.cls == FC_ZERO || (x.cls == FC_FIN && (xb & ~sb) < one);
        if (below1) return y.neg ? inf : 0;
        return y.neg ? 0 : inf;
    }
    if (x.cls == FC_ZERO) {
        if (yodd) return y.neg ? (xb & sb) | inf : xb;
        return y.neg ? inf : 0;
    }
    if (x.cls == FC_INF) {
        const u64 sign = (x.neg && yodd) ? sb : 0;
        return sign | (y.neg ? 0 : inf);
    }
    if (x.neg && !yint) return FMT_NANB(s);
    const bool rneg = x.neg && yodd;
    /* Exact and halfway results, found by integers. */
    u64 xm;
    int xe;
    fm_odd(x.mant, x.ex, &xm, &xe);
    if (xm == 1) {
        /* x = 2^xe: 2^(xe y), exact when xe y is an integer */
        const proven_i64 double_lim = 4000;
        if (ye >= 0) {
            if (ye > 20 || (u64)(xe < 0 ? -xe : xe) * ym > (u64)double_lim) {
                if (xe == 0) return rneg ? sb | one : one;
                const bool big = (xe > 0) != y.neg;
                return (rneg ? sb : 0) | (big ? inf : 0);
            }
            const proven_i64 N = (proven_i64)xe * (proven_i64)ym * (((proven_i64)1) << ye) * (y.neg ? -1 : 1);
            if (N > double_lim || N < -double_lim) return (rneg ? sb : 0) | (N > 0 ? inf : 0);
            return proven_float_pack_(rneg, N, 1, s);
        }
        const int k = -ye;
        /* divisibility and quotient by masks and shifts: no division helper on small cores */
        if (k <= 20 && ym <= 1000000 && ((proven_i64)xe & ((((proven_i64)1) << k) - 1)) == 0) {
            const proven_i64 N = ((proven_i64)xe >> k) * (proven_i64)ym * (y.neg ? -1 : 1);   /* |N| < 2^31 */
            if (N <= double_lim && N >= -double_lim) return proven_float_pack_(rneg, N, 1, s);
        }
    } else if (!y.neg && ym <= 64) {
        if (ye >= 0 && ye <= 6 && (ym << ye) <= 64)
            return fm_exact_power(rneg, xm, (unsigned)(ym << ye), (proven_i64)xe * (proven_i64)(ym << ye), s);
        if (ye < 0 && ye >= -5 && ((proven_i64)xe & ((((proven_i64)1) << -ye) - 1)) == 0) {
            const u64 w = fm_root2k(xm, -ye);
            if (w) return fm_exact_power(rneg, w, (unsigned)ym, ((proven_i64)xe >> -ye) * (proven_i64)ym, s);
        }
    }
    mp_t xv, yv, L, z, r;
    /* the size of y log x, to two limbs: beyond 2^11 the result is surely infinite or zero */
    fm_mp_of(&xv, &x, 2);
    fm_mp_of(&yv, &y, 2);
    xv.neg = false;
    mp_log(&L, &xv, 2);
    mp_mul(&z, &L, &yv, 2);
    if (z.n && mp_top(&z) >= 11) return (rneg ? sb : 0) | (z.neg ? 0 : inf);
    u64 out = 0;
    for (int i = 0; i < FM_LEVELS; i++) {
        const int n = fm_level(s, i);
        mp_pow_core(&r, &xv, &yv, n);
        r.neg = rneg;
        if (mp_round_test(&r, s, &out)) return out;
    }
    return mp_round_nearest(&r, s);
}

/* The bits of a value and back, inline: these sit on the FPU path's every call, and a call to
 * float_ops.c's functions there cost more than CORE-MATH's own work in log. */
static inline u64 fm_d2b(double x) { union { double d; u64 u; } v = { .d = x }; return v.u; }
static inline double fm_b2d(u64 u) { union { u64 u; double d; } v = { .u = u }; return v.d; }
static inline u64 fm_f2b(float x) { union { float f; proven_u32 u; } v = { .f = x }; return v.u; }
static inline float fm_b2f(u64 u) { union { proven_u32 u; float f; } v = { .u = (proven_u32)u }; return v.f; }
#define F64B(x) fm_d2b(x)
#define F64D(u) fm_b2d(u)
#define F32B(x) fm_f2b(x)
#define F32D(u) fm_b2f(u)

// -----------------------------------------------------------------------------
// The FPU path (math/src/cm, CORE-MATH): where it is compiled and the processor has what it needs
// -----------------------------------------------------------------------------

#if PROVEN_CM
double proven_cm_exp_(double x);
double proven_cm_log_(double x);
double proven_cm_sin_(double x);
double proven_cm_cos_(double x);
double proven_cm_tan_(double x);
double proven_cm_atan2_(double y, double x);
double proven_cm_pow_(double x, double y);
float proven_cm_expf_(float x);
float proven_cm_logf_(float x);
float proven_cm_sinf_(float x);
float proven_cm_cosf_(float x);
float proven_cm_tanf_(float x);
float proven_cm_atan2f_(float y, float x);
float proven_cm_powf_(float x, float y);

double proven_cm_pow_fallback_(double x, double y) { return proven_float_from_bits(fm_pow(proven_float_to_bits(x), proven_float_to_bits(y), false)); }
double proven_cm_atan2_fallback_(double y, double x) { return proven_float_from_bits(fm_atan2(proven_float_to_bits(y), proven_float_to_bits(x), false)); }

#if defined(__x86_64__) && !(defined(__FMA__) && defined(__AVX2__))
static bool fm_cm_ready(void) { return __builtin_cpu_supports("fma") && __builtin_cpu_supports("avx2"); }
#else
static bool fm_cm_ready(void) { return true; }
#endif

static bool fm_isnan(u64 u, bool s) { return (u & ~FMT_SIGNB(s)) > FMT_INFB(s); }

/* RFC-0055 D-6 on the FPU path's result: a NaN operand comes back quieted, the first; a NaN
 * made is the format's own. */
static u64 fm_fix(u64 r, u64 a, u64 b, int args, bool s) {
    if (!fm_isnan(r, s)) return r;
    if (fm_isnan(a, s)) return a | FMT_QUIETB(s);
    if (args > 1 && fm_isnan(b, s)) return b | FMT_QUIETB(s);
    return FMT_NANB(s);
}
#define FM_CM1(fn, x) if (fm_cm_ready()) return F64D(fm_fix(F64B(proven_cm_##fn##_(x)), F64B(x), 0, 1, false))
#define FM_CM2(fn, x, y) if (fm_cm_ready()) return F64D(fm_fix(F64B(proven_cm_##fn##_(x, y)), F64B(x), F64B(y), 2, false))
#define FM_CM1F(fn, x) if (fm_cm_ready()) return F32D(fm_fix(F32B(proven_cm_##fn##f_(x)), F32B(x), 0, 1, true))
#define FM_CM2F(fn, x, y) if (fm_cm_ready()) return F32D(fm_fix(F32B(proven_cm_##fn##f_(x, y)), F32B(x), F32B(y), 2, true))
#else
#define FM_CM1(fn, x) ((void)0)
#define FM_CM2(fn, x, y) ((void)0)
#define FM_CM1F(fn, x) ((void)0)
#define FM_CM2F(fn, x, y) ((void)0)
static bool fm_cm_ready(void) { return false; }
#endif

// -----------------------------------------------------------------------------
// Public functions
// -----------------------------------------------------------------------------

double proven_float_exp(double x) { FM_CM1(exp, x); return F64D(fm_exp(F64B(x), false)); }
double proven_float_log(double x) { FM_CM1(log, x); return F64D(fm_log(F64B(x), false)); }
double proven_float_sin(double x) { FM_CM1(sin, x); return F64D(fm_trig(F64B(x), false, FM_SIN)); }
double proven_float_cos(double x) { FM_CM1(cos, x); return F64D(fm_trig(F64B(x), false, FM_COS)); }
double proven_float_tan(double x) { FM_CM1(tan, x); return F64D(fm_trig(F64B(x), false, FM_TAN)); }
double proven_float_atan2(double y, double x) { FM_CM2(atan2, y, x); return F64D(fm_atan2(F64B(y), F64B(x), false)); }
double proven_float_pow(double x, double y) { FM_CM2(pow, x, y); return F64D(fm_pow(F64B(x), F64B(y), false)); }

float proven_float32_exp(float x) { FM_CM1F(exp, x); return F32D(fm_exp(F32B(x), true)); }
float proven_float32_log(float x) { FM_CM1F(log, x); return F32D(fm_log(F32B(x), true)); }
float proven_float32_sin(float x) { FM_CM1F(sin, x); return F32D(fm_trig(F32B(x), true, FM_SIN)); }
float proven_float32_cos(float x) { FM_CM1F(cos, x); return F32D(fm_trig(F32B(x), true, FM_COS)); }
float proven_float32_tan(float x) { FM_CM1F(tan, x); return F32D(fm_trig(F32B(x), true, FM_TAN)); }
float proven_float32_atan2(float y, float x) { FM_CM2F(atan2, y, x); return F32D(fm_atan2(F32B(y), F32B(x), true)); }
float proven_float32_pow(float x, float y) { FM_CM2F(pow, x, y); return F32D(fm_pow(F32B(x), F32B(y), true)); }

bool proven_float_math_is_fpu_(void) { return fm_cm_ready(); }

// -----------------------------------------------------------------------------
// For tests: the integer path on the bits, whatever path the public functions take
// -----------------------------------------------------------------------------

proven_u64 proven_float_math_int_(int fn, bool b32, proven_u64 a, proven_u64 b) {
    switch (fn) {
    case PROVEN_FLOAT_FN_EXP_: return fm_exp(a, b32);
    case PROVEN_FLOAT_FN_LOG_: return fm_log(a, b32);
    case PROVEN_FLOAT_FN_SIN_: return fm_trig(a, b32, FM_SIN);
    case PROVEN_FLOAT_FN_COS_: return fm_trig(a, b32, FM_COS);
    case PROVEN_FLOAT_FN_TAN_: return fm_trig(a, b32, FM_TAN);
    case PROVEN_FLOAT_FN_ATAN2_: return fm_atan2(a, b, b32);
    case PROVEN_FLOAT_FN_POW_: return fm_pow(a, b, b32);
    default: return 0;
    }
}

/* For tests: the integer path's value at Ziv level k (0 .. FM_LEVELS - 1) against a more
 * precise evaluation (the next level, or 4 limbs more past the last), in 1/1024 units of level
 * k's last place, for an input the general code takes (finite, not an exact case); -1 when the
 * input is not one. This measures the bound the rounding test assumes (FM_G units). */
static bool fm_eval(mp_t *r, int fn, bool s, u64 a, u64 b, int n) {
    const fm_val x = fm_dec(a, s), y = fm_dec(b, s);
    mp_t xv, yv;
    switch (fn) {
    case PROVEN_FLOAT_FN_EXP_:
        if (x.cls != FC_FIN) return false;
        fm_mp_of(&xv, &x, 2);
        if (mp_top(&xv) >= 11) return false;
        mp_exp(r, &xv, n);
        return true;
    case PROVEN_FLOAT_FN_LOG_:
        if (x.cls != FC_FIN || x.neg || a == FMT_ONEB(s)) return false;
        fm_mp_of(&xv, &x, 2);
        mp_log(r, &xv, n);
        return true;
    case PROVEN_FLOAT_FN_SIN_: case PROVEN_FLOAT_FN_COS_: case PROVEN_FLOAT_FN_TAN_:
        if (x.cls != FC_FIN) return false;
        mp_trig(r, fn == PROVEN_FLOAT_FN_SIN_ ? FM_SIN : fn == PROVEN_FLOAT_FN_COS_ ? FM_COS : FM_TAN, x.mant, x.ex, x.neg, n);
        return true;
    case PROVEN_FLOAT_FN_ATAN2_:
        if (x.cls != FC_FIN || y.cls != FC_FIN) return false;
        fm_mp_of(&xv, &x, 2);
        fm_mp_of(&yv, &y, 2);
        mp_atan2(r, &xv, &yv, n);
        return true;
    default: {
        if (x.cls != FC_FIN || y.cls != FC_FIN || x.neg || a == FMT_ONEB(s)) return false;
        mp_t L, z;
        fm_mp_of(&xv, &x, 2);
        fm_mp_of(&yv, &y, 2);
        mp_log(&L, &xv, 2);
        mp_mul(&z, &L, &yv, 2);
        if (z.n && mp_top(&z) >= 11) return false;
        mp_pow_core(r, &xv, &yv, n);
        return true;
    }
    }
}

proven_i64 proven_float_math_level_gap_(int fn, bool b32, proven_u64 a, proven_u64 b, int k) {
    if (k < 0 || k >= FM_LEVELS) return -1;
    const int n1 = fm_level(b32, k), n2 = k + 1 < FM_LEVELS ? fm_level(b32, k + 1) : n1 + 4;
    mp_t r1, r2, d;
    if (!fm_eval(&r1, fn, b32, a, b, n1) || !fm_eval(&r2, fn, b32, a, b, n2)) return -1;
    if (!r1.n) return r2.n ? ((proven_i64)1 << 40) : 0;
    mp_sub(&d, &r2, &r1, n2);
    if (!d.n) return 0;
    const proven_i64 t = mp_top(&d) - r1.e;                 /* |d| in [2^t, 2^(t+1)) units */
    if (t >= 30) return (proven_i64)1 << 40;
    if (t < -12) return 0;
    /* the top 32 bits of d as a number of 1/1024 units */
    const u32 top = d.d[d.n - 1];
    const proven_i64 sh = t - 31 + 10;
    if (sh <= -32) return 0;
    return sh >= 0 ? (proven_i64)top << sh : (proven_i64)(top >> (-sh));
}
