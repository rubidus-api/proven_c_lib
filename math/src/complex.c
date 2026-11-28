#include "proven/complex.h"
#include "proven/float_math.h"
#include "proven/float_ops.h"

/*
 * complex.h (RFC-0058 D-20): each function is a fixed formula of float_ops.h's correctly rounded
 * operations and float_math.h's correctly rounded functions, called by name - never C's
 * operators on doubles, which a compiler may contract into a fused multiply-add or, on x87,
 * evaluate in a wider format - so the bits are the same on every target. Comparisons are C's:
 * they are exact everywhere.
 *
 * The special values (zeros, infinities, NaNs) are decided first, on the first quadrant where a
 * function has the symmetries to allow it, as C23 Annex G G.6 lists them; the finite case then
 * follows a formula chosen to keep each part accurate where it is small: Kahan's sums of two
 * products for mul and div, log1p of x^2 + y^2 - 1 summed exactly near |z| = 1 for log, Kahan's
 * square root and tanh, and Hull, Fairgrieve and Tang's asin and acos (ACM TOMS 23(3), 1997).
 * sin, cos, tan, asin and atan are the hyperbolic ones turned by i (sin z = -i sinh(iz), ...),
 * as Annex G defines their special values.
 */

typedef proven_complex_t cx;

#define ADD proven_float_add
#define SUB proven_float_sub
#define MUL proven_float_mul
#define DIV proven_float_div
#define FMA proven_float_fma
#define SQRT proven_float_sqrt
#define LDEXP proven_float_ldexp

static const proven_u64 CX_SIGN = 0x8000000000000000u;

static inline proven_u64 cx_bits(double x) { return proven_float_to_bits(x); }
static inline double cx_dbl(proven_u64 u) { return proven_float_from_bits(u); }
static inline double cx_neg(double x) { return cx_dbl(cx_bits(x) ^ CX_SIGN); }
static inline double cx_abs(double x) { return cx_dbl(cx_bits(x) & ~CX_SIGN); }
static inline bool cx_sgn(double x) { return (cx_bits(x) & CX_SIGN) != 0; }
static inline double cx_cps(double x, double s) { return cx_dbl((cx_bits(x) & ~CX_SIGN) | (cx_bits(s) & CX_SIGN)); }
static inline bool cx_nan(double x) { return (cx_bits(x) & ~CX_SIGN) > 0x7ff0000000000000u; }
static inline bool cx_inf(double x) { return (cx_bits(x) & ~CX_SIGN) == 0x7ff0000000000000u; }
static inline bool cx_fin(double x) { return (cx_bits(x) & ~CX_SIGN) < 0x7ff0000000000000u; }
static inline cx cx_mk(double re, double im) { return (cx){ re, im }; }

#define CX_INF cx_dbl(0x7ff0000000000000u)
#define CX_NAN cx_dbl(0x7ff8000000000000u)
#define CX_PI cx_dbl(0x400921fb54442d18u)      /* pi, pi/2, pi/4, 3pi/4 rounded */
#define CX_PI_2 cx_dbl(0x3ff921fb54442d18u)
#define CX_PI_4 cx_dbl(0x3fe921fb54442d18u)
#define CX_PI34 cx_dbl(0x4002d97c7f3321d2u)
#define CX_LN2 cx_dbl(0x3fe62e42fefa39efu)      /* ln 2 rounded */
#define CX_LN2X2 cx_dbl(0x3ff62e42fefa39efu)    /* 2 ln 2 rounded */
#define CX_LN2_HI cx_dbl(0x3fe62e42fee00000u)   /* ln 2 to 32 bits, and the rest */
#define CX_LN2_LO cx_dbl(0x3dea39ef35793c76u)
#define CX_INV_LN2 cx_dbl(0x3ff71547652b82feu)
#define CX_600LN2 cx_dbl(0x4079fe3682cd3be4u)   /* 600 ln 2 rounded */
#define CX_BCROSS cx_dbl(0x3fe488ce703afb7fu)   /* 0.6417 as a double: no x87 literal of more precision */

/* Every NaN part made the default one. */
static inline cx cx_out(cx z) {
    if (cx_nan(z.re)) z.re = CX_NAN;
    if (cx_nan(z.im)) z.im = CX_NAN;
    return z;
}

/* a b + c d with Kahan's fused correction: within 2 units of the last place (Jeannerod, Louvet
 * and Muller, Math. Comp. 82, 2013). */
static double cx_dot2(double a, double b, double c, double d) {
    const double w = MUL(c, d);
    const double e = FMA(c, d, cx_neg(w));        /* c d - w, exactly */
    const double f = FMA(a, b, w);
    const double r = ADD(f, e);
    /* With an infinite product the correction is a NaN, and a zero sum takes the correction's +0
     * whatever its own sign: then the plain sum, which gives the infinity and IEEE's sign of an
     * exact zero (-0 for -0 + -0) - what Annex G's cuts and C's arithmetic need. */
    if (cx_nan(r) || r == 0.0) return ADD(MUL(a, b), w);
    return r;
}

/* a b + c d for finite operands without overflow or underflow on the way: each product as its
 * mantissas' product and its exponent, the smaller aligned to the larger, the exponent put back
 * once at the end (which rounds once if the result is out of range). */
static double cx_dot2_parts(double a, double b, double c, double d, int *E) {
    int ea = 0, eb = 0, ec = 0, ed = 0;
    const double ma = proven_float_frexp(a, &ea), mb = proven_float_frexp(b, &eb);
    const double mc = proven_float_frexp(c, &ec), md = proven_float_frexp(d, &ed);
    const bool z1 = ma == 0.0 || mb == 0.0, z2 = mc == 0.0 || md == 0.0;
    if (z1 && z2) { *E = 0; return ADD(MUL(a, b), MUL(c, d)); }      /* zeros, with their signs */
    const int e1 = ea + eb, e2 = ec + ed;
    *E = z1 ? e2 : z2 ? e1 : (e1 > e2 ? e1 : e2);
    return cx_dot2(ma, LDEXP(mb, e1 - *E), mc, LDEXP(md, e2 - *E));
}

static double cx_dot2_wide(double a, double b, double c, double d) {
    int E = 0;
    const double r = cx_dot2_parts(a, b, c, d, &E);
    return LDEXP(r, E);
}

/* x + y with its rounding error (Knuth's two-sum). */
static void cx_two_sum(double a, double b, double *s, double *e) {
    const double t = ADD(a, b), bb = SUB(t, a);
    *s = t;
    *e = ADD(SUB(a, SUB(t, bb)), SUB(b, bb));
}

/* The sum of n doubles as if computed in three times the precision, then rounded (Ogita, Rump
 * and Oishi's SumK, K = 3): its error is a unit and a half of the last place plus 2^-159 of
 * the largest term. */
static double cx_sum(double *v, int n) {
    for (int k = 0; k < 2; k++)
        for (int i = 1; i < n; i++) cx_two_sum(v[i], v[i - 1], &v[i], &v[i - 1]);
    double s = 0;
    for (int i = 0; i < n - 1; i++) s = ADD(s, v[i]);
    return ADD(s, v[n - 1]);
}

/* sign * (x^2 + y^2 - 1) for 0 <= y <= x, exactly but for the last rounding: x^2 and y^2
 * split into two doubles each by fma. */
static double cx_norm_m1(double x, double y, bool neg) {
    const double xh = MUL(x, x), xl = FMA(x, x, cx_neg(xh)), yh = MUL(y, y), yl = FMA(y, y, cx_neg(yh));
    double v[5] = { xl, yl, -1.0, xh, yh };
    if (neg) for (int i = 0; i < 5; i++) v[i] = cx_neg(v[i]);
    return cx_sum(v, 5);
}

/* e^x c 2^adj for finite nonzero c and |x| > 700, without overflow or underflow in between:
 * x = k ln2 + r with |r| <= ln2/2 (k ln2_hi exact, r by two fused steps), e^r times c's
 * mantissa, and the exponents added once at the end, which rounds once if the result is out of
 * range. About 1.5 units of the last place. */
static double cx_exp_mul(double x, double c, int adj) {
    if (x > 2300.0) return cx_cps(CX_INF, c);                    /* e^2300 2^-1074 > 2^1024 */
    if (x < -2300.0) return cx_cps(0.0, c);
    proven_i64 k = 0;
    (void)proven_float_rint_to_i64(MUL(x, CX_INV_LN2), &k);
    const double kd = proven_float_from_i64(k);
    const double r = FMA(cx_neg(kd), CX_LN2_LO, FMA(cx_neg(kd), CX_LN2_HI, x));
    int ec = 0;
    const double m = proven_float_frexp(c, &ec);
    return LDEXP(MUL(proven_float_exp(r), m), (int)k + ec + adj);
}

// -----------------------------------------------------------------------------
// Arithmetic
// -----------------------------------------------------------------------------

proven_complex_t proven_complex_add(proven_complex_t a, proven_complex_t b) { return cx_out(cx_mk(ADD(a.re, b.re), ADD(a.im, b.im))); }
proven_complex_t proven_complex_sub(proven_complex_t a, proven_complex_t b) { return cx_out(cx_mk(SUB(a.re, b.re), SUB(a.im, b.im))); }

static double cx_unit_or_zero(double x) { return cx_cps(cx_inf(x) ? 1.0 : 0.0, x); }

proven_complex_t proven_complex_mul(proven_complex_t z, proven_complex_t w) {
    double a = z.re, b = z.im, c = w.re, d = w.im;
    double x = cx_dot2(a, c, cx_neg(b), d), y = cx_dot2(a, d, b, c);
    if ((!cx_fin(x) || !cx_fin(y)) && cx_fin(a) && cx_fin(b) && cx_fin(c) && cx_fin(d))
        return cx_mk(cx_dot2_wide(a, c, cx_neg(b), d), cx_dot2_wide(a, d, b, c));   /* a product overflowed */
    if (cx_nan(x) && cx_nan(y)) {
        /* Annex G G.5.2: an infinite operand gives an infinite result */
        bool again = false;
        if (cx_inf(a) || cx_inf(b)) {
            a = cx_unit_or_zero(a); b = cx_unit_or_zero(b);
            if (cx_nan(c)) c = cx_cps(0.0, c);
            if (cx_nan(d)) d = cx_cps(0.0, d);
            again = true;
        }
        if (cx_inf(c) || cx_inf(d)) {
            c = cx_unit_or_zero(c); d = cx_unit_or_zero(d);
            if (cx_nan(a)) a = cx_cps(0.0, a);
            if (cx_nan(b)) b = cx_cps(0.0, b);
            again = true;
        }
        if (!again && (cx_inf(MUL(a, c)) || cx_inf(MUL(b, d)) || cx_inf(MUL(a, d)) || cx_inf(MUL(b, c)))) {
            if (cx_nan(a)) a = cx_cps(0.0, a);
            if (cx_nan(b)) b = cx_cps(0.0, b);
            if (cx_nan(c)) c = cx_cps(0.0, c);
            if (cx_nan(d)) d = cx_cps(0.0, d);
            again = true;
        }
        if (again) {
            x = MUL(CX_INF, SUB(MUL(a, c), MUL(b, d)));
            y = MUL(CX_INF, ADD(MUL(a, d), MUL(b, c)));
        }
    }
    return cx_out(cx_mk(x, y));
}

/* a / b for finite operands, b not 0: the numerators a conj(b) and the denominator |b|^2 as
 * mantissas and exponents (cx_dot2_parts), so that no part overflows or underflows on the way
 * however far apart the four exponents are, the exponents put back once at the end. About 2.5
 * units of the last place: two for each sum of products, a half for the quotient. */
proven_complex_t proven_complex_div(proven_complex_t z, proven_complex_t w) {
    double a = z.re, b = z.im, c = w.re, d = w.im;
    if (cx_fin(a) && cx_fin(b) && cx_fin(c) && cx_fin(d) && (c != 0.0 || d != 0.0)) {
        int en1 = 0, en2 = 0, ed = 0;
        const double n1 = cx_dot2_parts(a, c, b, d, &en1), n2 = cx_dot2_parts(b, c, cx_neg(a), d, &en2);
        const double den = cx_dot2_parts(c, c, d, d, &ed);
        return cx_mk(LDEXP(DIV(n1, den), en1 - ed), LDEXP(DIV(n2, den), en2 - ed));
    }
    /* infinities, NaNs and a zero divisor: the plain formula, then Annex G G.5.2 */
    const double denom = ADD(MUL(c, c), MUL(d, d));
    double x = DIV(ADD(MUL(a, c), MUL(b, d)), denom), y = DIV(SUB(MUL(b, c), MUL(a, d)), denom);
    if (cx_nan(x) && cx_nan(y)) {
        if (denom == 0.0 && (!cx_nan(a) || !cx_nan(b))) {
            x = MUL(cx_cps(CX_INF, c), a);
            y = MUL(cx_cps(CX_INF, c), b);
        } else if ((cx_inf(a) || cx_inf(b)) && cx_fin(c) && cx_fin(d)) {
            a = cx_unit_or_zero(a); b = cx_unit_or_zero(b);
            x = MUL(CX_INF, ADD(MUL(a, c), MUL(b, d)));
            y = MUL(CX_INF, SUB(MUL(b, c), MUL(a, d)));
        } else if ((cx_inf(c) || cx_inf(d)) && cx_fin(a) && cx_fin(b)) {
            c = cx_unit_or_zero(c); d = cx_unit_or_zero(d);
            x = MUL(0.0, ADD(MUL(a, c), MUL(b, d)));
            y = MUL(0.0, SUB(MUL(b, c), MUL(a, d)));
        }
    }
    return cx_out(cx_mk(x, y));
}

double proven_complex_abs(proven_complex_t z) {
    const double r = proven_float_hypot(z.re, z.im);
    return cx_nan(r) ? CX_NAN : r;
}

double proven_complex_arg(proven_complex_t z) {
    const double r = proven_float_atan2(z.im, z.re);
    return cx_nan(r) ? CX_NAN : r;
}

proven_complex_t proven_complex_conj(proven_complex_t z) { return cx_mk(z.re, cx_neg(z.im)); }

proven_complex_t proven_complex_proj(proven_complex_t z) {
    if (cx_inf(z.re) || cx_inf(z.im)) return cx_mk(CX_INF, cx_cps(0.0, z.im));
    return z;
}

// -----------------------------------------------------------------------------
// exp, log, pow, sqrt
// -----------------------------------------------------------------------------

proven_complex_t proven_complex_exp(proven_complex_t z) {
    const double x = z.re, y = z.im, ay = cx_abs(y);
    double re, im;
    if (!cx_fin(x) || !cx_fin(y)) {
        /* on the upper half-plane, then conj(exp z) = exp(conj z) */
        if (cx_nan(x)) { re = CX_NAN; im = ay == 0.0 ? 0.0 : CX_NAN; }
        else if (cx_fin(x)) { re = CX_NAN; im = CX_NAN; }                  /* y infinite or NaN */
        else if (!cx_sgn(x)) {                                            /* +inf */
            if (ay == 0.0) { re = CX_INF; im = 0.0; }
            else if (cx_fin(ay)) { re = cx_cps(CX_INF, proven_float_cos(ay)); im = cx_cps(CX_INF, proven_float_sin(ay)); }
            else { re = CX_INF; im = CX_NAN; }
        } else {                                                          /* -inf */
            if (cx_fin(ay)) { re = cx_cps(0.0, proven_float_cos(ay)); im = cx_cps(0.0, proven_float_sin(ay)); }
            else { re = 0.0; im = 0.0; }
        }
        return cx_out(cx_mk(re, cx_sgn(y) ? cx_neg(im) : im));
    }
    if (y == 0.0) return cx_mk(proven_float_exp(x), y);
    const double c = proven_float_cos(y), s = proven_float_sin(y);
    if (cx_abs(x) > 700.0) return cx_mk(cx_exp_mul(x, c, 0), cx_exp_mul(x, s, 0));
    const double e = proven_float_exp(x);
    return cx_mk(MUL(e, c), MUL(e, s));
}

/* log|z| for finite z, not 0. Near |z| = 1 as log1p(x^2 + y^2 - 1) / 2 with the argument summed
 * exactly (within 2 units in all); elsewhere log(hypot(x, y)), |log| >= 0.34 there, within 2.5
 * units; scaled by 2^-1 or 2^600 at the ends of the range. */
static double cx_log_abs(double x, double y) {
    double ax = cx_abs(x), ay = cx_abs(y);
    if (ax < ay) { const double t = ax; ax = ay; ay = t; }
    if (ax > 0x1p1000) return ADD(proven_float_log(proven_float_hypot(LDEXP(ax, -1), LDEXP(ay, -1))), CX_LN2);
    if (ax < 0x1p-1000) return SUB(proven_float_log(proven_float_hypot(LDEXP(ax, 600), LDEXP(ay, 600))), CX_600LN2);
    const double s = FMA(ax, ax, MUL(ay, ay));
    if (s >= 0.5 && s <= 2.0) return LDEXP(proven_float_log1p(cx_norm_m1(ax, ay, false)), -1);
    return proven_float_log(proven_float_hypot(ax, ay));
}

proven_complex_t proven_complex_log(proven_complex_t z) {
    const double x = z.re, y = z.im;
    const double im = proven_float_atan2(y, x);
    if (!cx_fin(x) || !cx_fin(y) || (x == 0.0 && y == 0.0))
        return cx_out(cx_mk(proven_float_log(proven_float_hypot(x, y)), im));   /* Annex G's cases, by hypot and atan2 */
    return cx_out(cx_mk(cx_log_abs(x, y), im));
}

proven_complex_t proven_complex_pow(proven_complex_t z, proven_complex_t w) {
    if (w.re == 0.0 && w.im == 0.0) return cx_mk(1.0, 0.0);
    if (z.re == 0.0 && z.im == 0.0 && w.im == 0.0 && w.re > 0.0) return cx_mk(0.0, 0.0);
    return proven_complex_exp(proven_complex_mul(w, proven_complex_log(z)));
}

proven_complex_t proven_complex_sqrt(proven_complex_t z) {
    const double x = z.re, y = z.im;
    double ax = cx_abs(x), ay = cx_abs(y), re, im;
    if (cx_inf(y)) return cx_mk(CX_INF, y);                             /* even for a NaN x */
    if (!cx_fin(x) || !cx_fin(y)) {
        if (cx_nan(x) || cx_nan(y)) {
            if (cx_inf(x) && !cx_sgn(x)) { re = CX_INF; im = CX_NAN; }
            else if (cx_inf(x)) { re = CX_NAN; im = CX_INF; }
            else { re = CX_NAN; im = CX_NAN; }
        } else if (cx_sgn(x)) { re = 0.0; im = CX_INF; }                 /* -inf + iy */
        else { re = CX_INF; im = 0.0; }                                   /* +inf + iy */
        return cx_out(cx_mk(re, cx_sgn(y) ? cx_neg(im) : im));
    }
    if (ax == 0.0 && ay == 0.0) return cx_mk(0.0, y);
    /* Kahan: t = sqrt((|x| + |z|) / 2), then the other part |y| / 2t */
    int scale = 0;
    if (ax > 0x1p1020 || ay > 0x1p1020) { ax = LDEXP(ax, -2); ay = LDEXP(ay, -2); scale = 1; }
    else if (ax < 0x1p-1020 && ay < 0x1p-1020) { ax = LDEXP(ax, 54); ay = LDEXP(ay, 54); scale = -27; }
    const double t = SQRT(LDEXP(ADD(ax, proven_float_hypot(ax, ay)), -1));
    const double u = DIV(ay, LDEXP(t, 1));
    if (!cx_sgn(x)) { re = t; im = u; } else { re = u; im = t; }
    re = LDEXP(re, scale);
    im = LDEXP(im, scale);
    return cx_mk(re, cx_cps(im, y));
}

// -----------------------------------------------------------------------------
// The hyperbolic functions, and the trigonometric ones turned by i
// -----------------------------------------------------------------------------

/* sinh on the first quadrant (x, y >= 0, or NaN); sinh is odd and conj(sinh z) = sinh(conj z). */
static cx cx_sinh_q(double x, double y) {
    if (!cx_fin(x) || !cx_fin(y)) {
        if (cx_nan(x)) return cx_mk(CX_NAN, y == 0.0 ? 0.0 : CX_NAN);
        if (cx_fin(x)) return cx_mk(x == 0.0 ? 0.0 : CX_NAN, CX_NAN);          /* y inf or NaN */
        if (y == 0.0) return cx_mk(CX_INF, 0.0);
        if (cx_fin(y)) return cx_mk(cx_cps(CX_INF, proven_float_cos(y)), cx_cps(CX_INF, proven_float_sin(y)));
        return cx_mk(CX_INF, CX_NAN);
    }
    if (y == 0.0) return cx_mk(proven_float_sinh(x), 0.0);
    const double c = proven_float_cos(y), s = proven_float_sin(y);
    if (x > 700.0) return cx_mk(cx_exp_mul(x, c, -1), cx_exp_mul(x, s, -1));
    return cx_mk(MUL(proven_float_sinh(x), c), MUL(proven_float_cosh(x), s));
}

/* cosh on the first quadrant; cosh is even and conj(cosh z) = cosh(conj z). */
static cx cx_cosh_q(double x, double y) {
    if (!cx_fin(x) || !cx_fin(y)) {
        if (cx_nan(x)) return cx_mk(CX_NAN, y == 0.0 ? 0.0 : CX_NAN);
        if (cx_fin(x)) return cx_mk(CX_NAN, x == 0.0 ? 0.0 : CX_NAN);
        if (y == 0.0) return cx_mk(CX_INF, 0.0);
        if (cx_fin(y)) return cx_mk(cx_cps(CX_INF, proven_float_cos(y)), cx_cps(CX_INF, proven_float_sin(y)));
        return cx_mk(CX_INF, CX_NAN);
    }
    if (y == 0.0) return cx_mk(proven_float_cosh(x), 0.0);
    const double c = proven_float_cos(y), s = proven_float_sin(y);
    if (x > 700.0) return cx_mk(cx_exp_mul(x, c, -1), cx_exp_mul(x, s, -1));
    return cx_mk(MUL(proven_float_cosh(x), c), MUL(proven_float_sinh(x), s));
}

/* tanh on the first quadrant; odd, and conj. Kahan's form: t = tan y, b = 1 + t^2, s = sinh x,
 * tanh z = (b cosh(x) s + i t) / (1 + b s^2), without cancellation; past |x| = 22,
 * 1 + i 4 sin y cos y e^-2x. */
static cx cx_tanh_q(double x, double y) {
    if (!cx_fin(x) || !cx_fin(y)) {
        if (cx_nan(x)) return cx_mk(CX_NAN, y == 0.0 ? 0.0 : CX_NAN);
        if (cx_fin(x)) return cx_mk(x == 0.0 ? 0.0 : CX_NAN, CX_NAN);
        if (cx_fin(y)) return cx_mk(1.0, cx_cps(0.0, MUL(proven_float_sin(y), proven_float_cos(y))));
        return cx_mk(1.0, 0.0);
    }
    if (x > 22.0) {
        const double sc = MUL(proven_float_sin(y), proven_float_cos(y));
        return cx_mk(1.0, MUL(LDEXP(sc, 2), proven_float_exp(cx_neg(LDEXP(x, 1)))));
    }
    if (y == 0.0) return cx_mk(proven_float_tanh(x), y);
    const double t = proven_float_tan(y), b = FMA(t, t, 1.0), s = proven_float_sinh(x);
    const double den = FMA(b, MUL(s, s), 1.0);
    return cx_mk(DIV(MUL(MUL(b, proven_float_cosh(x)), s), den), DIV(t, den));
}

/* An odd function f with conj(f z) = f(conj z), from its first quadrant. */
static cx cx_odd(cx (*q)(double, double), double x, double y) {
    const cx r = q(cx_abs(x), cx_abs(y));
    return cx_out(cx_mk(cx_sgn(x) ? cx_neg(r.re) : r.re, cx_sgn(y) ? cx_neg(r.im) : r.im));
}

proven_complex_t proven_complex_sinh(proven_complex_t z) { return cx_odd(cx_sinh_q, z.re, z.im); }
proven_complex_t proven_complex_tanh(proven_complex_t z) { return cx_odd(cx_tanh_q, z.re, z.im); }
proven_complex_t proven_complex_cosh(proven_complex_t z) {
    const cx r = cx_cosh_q(cx_abs(z.re), cx_abs(z.im));
    return cx_out(cx_mk(r.re, cx_sgn(z.re) != cx_sgn(z.im) ? cx_neg(r.im) : r.im));
}

/* sin z = -i sinh(iz), cos z = cosh(iz), tan z = -i tanh(iz); iz = -y + ix. */
proven_complex_t proven_complex_sin(proven_complex_t z) {
    const cx r = proven_complex_sinh(cx_mk(cx_neg(z.im), z.re));
    return cx_out(cx_mk(r.im, cx_neg(r.re)));
}
proven_complex_t proven_complex_cos(proven_complex_t z) { return proven_complex_cosh(cx_mk(cx_neg(z.im), z.re)); }
proven_complex_t proven_complex_tan(proven_complex_t z) {
    const cx r = proven_complex_tanh(cx_mk(cx_neg(z.im), z.re));
    return cx_out(cx_mk(r.im, cx_neg(r.re)));
}

// -----------------------------------------------------------------------------
// The inverse functions
// -----------------------------------------------------------------------------

/*
 * asin and acos of x + iy for finite x, y >= 0 (Hull, Fairgrieve and Tang 1997): R = |z + 1|,
 * S = |z - 1|, A = (R + S) / 2 >= 1, B = x / A <= 1. The real part of asin is asin B and of acos
 * acos B while B <= 0.6417; past it, where those lose accuracy, an atan of a ratio with no
 * cancellation in it. The imaginary part (asin's; acos's is its negative) is
 * log(A + sqrt(A^2 - 1)), as log1p(Am1 + sqrt(Am1 (A + 1))) with A - 1 formed without
 * cancellation while A <= 1.5. Beyond 2^500 asin z = pi/2 - arg z - i log 2|z| to far below a
 * unit; below 2^-500 asin z = z. y^2 / D is formed as y (y / D), which does not underflow; for
 * y below 2^-500 and x < 1 the imaginary part is y / sqrt(1 - x^2).
 */
static void cx_asin_core(double x, double y, double *sre, double *cre, double *im) {
    if (x > 0x1p500 || y > 0x1p500) {
        *sre = proven_float_atan2(x, y);
        *cre = proven_float_atan2(y, x);
        *im = ADD(proven_float_log(proven_float_hypot(LDEXP(x, -1), LDEXP(y, -1))), CX_LN2X2);
        return;
    }
    if (x < 0x1p-500 && y < 0x1p-500) {
        *sre = x;
        *cre = CX_PI_2;
        *im = y;
        return;
    }
    const double xp1 = ADD(x, 1.0), xm1 = SUB(x, 1.0), omx = SUB(1.0, x);
    const double R = proven_float_hypot(xp1, y), S = proven_float_hypot(xm1, y);
    const double A = LDEXP(ADD(R, S), -1), B = DIV(x, A);
    const double yR = MUL(y, DIV(y, ADD(R, xp1)));          /* y^2 / (R + x + 1) */
    if (B <= CX_BCROSS) {
        *sre = proven_float_asin(B);
        *cre = proven_float_acos(B);
    } else if (x <= 1.0) {
        const double D = MUL(LDEXP(ADD(A, x), -1), ADD(yR, ADD(S, omx)));
        const double q = SQRT(D);
        *sre = proven_float_atan(DIV(x, q));
        *cre = proven_float_atan(DIV(q, x));
    } else {
        const double apx = ADD(A, x);
        const double D = LDEXP(ADD(DIV(apx, ADD(R, xp1)), DIV(apx, ADD(S, xm1))), -1);
        const double t = MUL(y, SQRT(D));
        *sre = proven_float_atan(DIV(x, t));
        *cre = proven_float_atan(DIV(t, x));
    }
    if (A <= 1.5) {
        if (x < 1.0 && y < 0x1p-500) {
            *im = DIV(y, SQRT(MUL(omx, xp1)));
            return;
        }
        double am1;
        if (x < 1.0) am1 = LDEXP(ADD(yR, MUL(y, DIV(y, ADD(S, omx)))), -1);
        else am1 = LDEXP(ADD(yR, ADD(S, xm1)), -1);
        *im = proven_float_log1p(ADD(am1, SQRT(MUL(am1, ADD(A, 1.0)))));
    } else if (A < 0x1p28) {
        *im = proven_float_log(ADD(A, SQRT(FMA(A, A, -1.0))));
    } else {
        *im = ADD(proven_float_log(A), CX_LN2);
    }
}

/* asinh on the first quadrant (Annex G G.6.2.2); odd and conj. asinh(x + iy) is
 * i asin(y - ix): the core taken at (y, x). */
static cx cx_asinh_q(double x, double y) {
    if (!cx_fin(x) || !cx_fin(y)) {
        if (cx_nan(x)) {
            if (y == 0.0) return cx_mk(CX_NAN, 0.0);
            return cx_mk(cx_inf(y) ? CX_INF : CX_NAN, CX_NAN);
        }
        if (cx_nan(y)) return cx_mk(cx_inf(x) ? CX_INF : CX_NAN, CX_NAN);
        if (cx_inf(x) && cx_inf(y)) return cx_mk(CX_INF, CX_PI_4);
        if (cx_inf(y)) return cx_mk(CX_INF, CX_PI_2);
        return cx_mk(CX_INF, 0.0);                                         /* x inf, y finite */
    }
    double sre, cre, im;
    cx_asin_core(y, x, &sre, &cre, &im);
    return cx_mk(im, sre);
}

proven_complex_t proven_complex_asinh(proven_complex_t z) { return cx_odd(cx_asinh_q, z.re, z.im); }

/* asin z = -i asinh(iz) */
proven_complex_t proven_complex_asin(proven_complex_t z) {
    const cx r = proven_complex_asinh(cx_mk(cx_neg(z.im), z.re));
    return cx_out(cx_mk(r.im, cx_neg(r.re)));
}

/* acos (Annex G G.6.1.1): conj(acos z) = acos(conj z), acos(-z) = pi - acos z. */
proven_complex_t proven_complex_acos(proven_complex_t z) {
    const double x = z.re, y = z.im, ax = cx_abs(x), ay = cx_abs(y);
    double re, im;                                                         /* im: of the upper half-plane, <= 0 */
    if (!cx_fin(x) || !cx_fin(y)) {
        if (cx_nan(x)) { re = CX_NAN; im = cx_inf(y) ? cx_neg(CX_INF) : CX_NAN; }
        else if (cx_nan(y)) {
            if (cx_inf(x)) { re = CX_NAN; im = cx_neg(CX_INF); }
            else if (x == 0.0) { re = CX_PI_2; im = CX_NAN; }
            else { re = CX_NAN; im = CX_NAN; }
        } else if (cx_inf(x) && cx_inf(y)) { re = cx_sgn(x) ? CX_PI34 : CX_PI_4; im = cx_neg(CX_INF); }
        else if (cx_inf(y)) { re = CX_PI_2; im = cx_neg(CX_INF); }
        else { re = cx_sgn(x) ? CX_PI : 0.0; im = cx_neg(CX_INF); }      /* x inf, y finite */
        return cx_out(cx_mk(re, cx_sgn(y) ? cx_neg(im) : im));
    }
    double sre, cre, v;
    cx_asin_core(ax, ay, &sre, &cre, &v);
    re = cx_sgn(x) ? SUB(CX_PI, cre) : cre;
    return cx_mk(re, cx_sgn(y) ? v : cx_neg(v));
}

/* acosh z = +-i acos z (Annex G G.6.2.1): the real part |Im acos z|, the imaginary part
 * Re acos z with the sign of y. */
proven_complex_t proven_complex_acosh(proven_complex_t z) {
    const cx r = proven_complex_acos(z);
    return cx_out(cx_mk(cx_abs(r.im), cx_cps(r.re, z.im)));
}

/*
 * atanh of x + iy for finite x, y >= 0: Re = log1p(4x / |1 - z|^2) / 4, Im = atan2(2y,
 * 1 - x^2 - y^2) / 2 with the denominator summed exactly. Re near the pole (|1 - z| below
 * 2^-500) as (ln 2 + log(x) / 2 - log|1 - z|) / 2; beyond 2^500, Re = x / |z|^2 and Im = pi/2.
 */
static cx cx_atanh_q(double x, double y) {
    if (!cx_fin(x) || !cx_fin(y)) {
        if (cx_nan(x)) return cx_mk(cx_inf(y) ? 0.0 : CX_NAN, cx_inf(y) ? CX_PI_2 : CX_NAN);
        if (cx_nan(y)) return cx_mk(cx_inf(x) || x == 0.0 ? 0.0 : CX_NAN, CX_NAN);
        return cx_mk(0.0, CX_PI_2);                                       /* x or y infinite */
    }
    if (x == 1.0 && y == 0.0) return cx_mk(CX_INF, 0.0);
    if (x > 0x1p500 || y > 0x1p500) {
        const double h = proven_float_hypot(x, y);
        return cx_mk(DIV(DIV(x, h), h), CX_PI_2);
    }
    const double d = proven_float_hypot(SUB(1.0, x), y);
    double re;
    if (d < 0x1p-500) re = LDEXP(SUB(ADD(CX_LN2, LDEXP(proven_float_log(x), -1)), proven_float_log(d)), -1);
    else re = LDEXP(proven_float_log1p(DIV(DIV(LDEXP(x, 2), d), d)), -2);
    const double D = cx_norm_m1(x, y, true);                              /* 1 - x^2 - y^2 */
    return cx_mk(re, LDEXP(proven_float_atan2(LDEXP(y, 1), D), -1));
}

proven_complex_t proven_complex_atanh(proven_complex_t z) { return cx_odd(cx_atanh_q, z.re, z.im); }

/* atan z = -i atanh(iz) */
proven_complex_t proven_complex_atan(proven_complex_t z) {
    const cx r = proven_complex_atanh(cx_mk(cx_neg(z.im), z.re));
    return cx_out(cx_mk(r.im, cx_neg(r.re)));
}

// -----------------------------------------------------------------------------
// binary32: in binary64, each part rounded once
// -----------------------------------------------------------------------------

typedef proven_complex32_t cf;
static inline cx cx_of(cf z) { return cx_mk(proven_float_from_float32(z.re), proven_float_from_float32(z.im)); }
static inline cf cf_of(cx z) { return (cf){ proven_float_to_float32(z.re), proven_float_to_float32(z.im) }; }

#define CF1(name) proven_complex32_t proven_complex32_##name(proven_complex32_t z) { return cf_of(proven_complex_##name(cx_of(z))); }
#define CF2(name) proven_complex32_t proven_complex32_##name(proven_complex32_t a, proven_complex32_t b) { return cf_of(proven_complex_##name(cx_of(a), cx_of(b))); }
CF2(add) CF2(sub) CF2(mul) CF2(div) CF2(pow)
CF1(exp) CF1(log) CF1(sqrt) CF1(sin) CF1(cos) CF1(tan) CF1(asin) CF1(acos) CF1(atan)
CF1(sinh) CF1(cosh) CF1(tanh) CF1(asinh) CF1(acosh) CF1(atanh)
#undef CF1
#undef CF2
/* conj and proj only move signs and parts, here too: on the binary32 bits. */
proven_complex32_t proven_complex32_conj(proven_complex32_t z) {
    return (cf){ z.re, proven_float32_from_bits(proven_float32_to_bits(z.im) ^ 0x80000000u) };
}
proven_complex32_t proven_complex32_proj(proven_complex32_t z) {
    if (proven_float32_is_inf(z.re) || proven_float32_is_inf(z.im))
        return (cf){ proven_float32_from_bits(0x7f800000u), proven_float32_from_bits(proven_float32_to_bits(z.im) & 0x80000000u) };
    return z;
}
float proven_complex32_abs(proven_complex32_t z) { return proven_float_to_float32(proven_complex_abs(cx_of(z))); }
float proven_complex32_arg(proven_complex32_t z) { return proven_float_to_float32(proven_complex_arg(cx_of(z))); }
