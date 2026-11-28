#include "proven/numfmt.h"
#include "proven/float_format.h"
#include "proven/float_parse.h"

/**
 * @file numfmt.c
 * @brief numfmt.h (RFC-0058): the ASCII digits made by the locale-independent writers, then the
 *        conventions put in; reading takes them out and hands ASCII to the parsers.
 */

#define V(s) { (const proven_byte_t *)(s), sizeof(s) - 1 }

const proven_numfmt_t proven_numfmt_c = { V("."), V(""), { 0 }, V("-") };
const proven_numfmt_t proven_numfmt_en = { V("."), V(","), { 3 }, V("-") };
const proven_numfmt_t proven_numfmt_de = { V(","), V("."), { 3 }, V("-") };
const proven_numfmt_t proven_numfmt_fr = { V(","), V("\xe2\x80\xaf"), { 3 }, V("-") };
const proven_numfmt_t proven_numfmt_ch = { V("."), V("\xe2\x80\x99"), { 3 }, V("-") };
const proven_numfmt_t proven_numfmt_in = { V("."), V(","), { 3, 2 }, V("-") };

typedef struct { proven_byte_t *p; proven_size_t cap, n; bool over; } out_t;

static void put(out_t *o, const proven_byte_t *s, proven_size_t n) {
    if (o->over || n > o->cap - o->n) { o->over = true; return; }
    for (proven_size_t i = 0; i < n; i++) o->p[o->n + i] = s[i];
    o->n += n;
}
static void put_view(out_t *o, proven_u8str_view_t v) { if (v.size) put(o, v.ptr, v.size); }

/* Whether a separator goes before digit `i` (from the left) of an integer part of `n` digits. */
static bool group_before(const proven_numfmt_t *f, proven_size_t n, proven_size_t i) {
    if (!f->group.size || !f->grouping[0] || i == 0) return false;
    proven_size_t from_right = n - i, edge = 0;
    for (int k = 0;; ) {
        const proven_u8 g = f->grouping[k];
        edge += g;
        if (edge == from_right) return true;
        if (edge > from_right) return false;
        if (k < 3 && f->grouping[k + 1]) k++;            /* else the last size repeats */
    }
}

/* ASCII "[-]ddd[.ddd]" (or inf/nan) written with f's conventions. */
static proven_err_t localize(const proven_numfmt_t *f, const char *a, proven_size_t n, proven_mem_mut_t out, proven_size_t *written) {
    if (!f || !written || (!out.ptr && out.size)) return PROVEN_ERR_INVALID_ARG;
    out_t o = { out.ptr, out.size, 0, false };
    proven_size_t i = 0;
    if (i < n && a[i] == '-') { put_view(&o, f->minus); i++; }
    proven_size_t int_end = i;
    while (int_end < n && a[int_end] >= '0' && a[int_end] <= '9') int_end++;
    const proven_size_t digits = int_end - i;
    if (digits == 0) put(&o, (const proven_byte_t *)a + i, n - i);   /* inf, nan */
    else {
        for (proven_size_t k = 0; k < digits; k++) {
            if (group_before(f, digits, k)) put_view(&o, f->group);
            put(&o, (const proven_byte_t *)a + i + k, 1);
        }
        if (int_end < n && a[int_end] == '.') {
            put_view(&o, f->decimal);
            put(&o, (const proven_byte_t *)a + int_end + 1, n - int_end - 1);
        }
    }
    if (o.over) return PROVEN_ERR_OUT_OF_BOUNDS;
    *written = o.n;
    return PROVEN_OK;
}

proven_err_t proven_numfmt_u64(const proven_numfmt_t *f, proven_u64 v, proven_mem_mut_t out, proven_size_t *written) {
    char buf[24];
    proven_size_t n = 0;
    char rev[24];
    do { rev[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    for (proven_size_t i = 0; i < n; i++) buf[i] = rev[n - 1 - i];
    return localize(f, buf, n, out, written);
}

proven_err_t proven_numfmt_i64(const proven_numfmt_t *f, proven_i64 v, proven_mem_mut_t out, proven_size_t *written) {
    char buf[24];
    proven_u64 m = v < 0 ? (proven_u64)0 - (proven_u64)v : (proven_u64)v;
    char rev[24];
    proven_size_t n = 0, k = 0;
    do { rev[n++] = (char)('0' + m % 10); m /= 10; } while (m);
    if (v < 0) buf[k++] = '-';
    while (n) buf[k++] = rev[--n];
    return localize(f, buf, k, out, written);
}

proven_err_t proven_numfmt_f64(const proven_numfmt_t *f, double v, int decimals, proven_mem_mut_t out, proven_size_t *written) {
    if (decimals < 0 || decimals > 340) return PROVEN_ERR_INVALID_ARG;
    char buf[720];                                       /* 309 integer digits, the mark, 340 decimals */
    proven_float_format_options_t opt = proven_float_format_options_fixed_default();
    opt.precision = decimals;
    opt.never_scientific = true;
    opt.printf_specials = true;
    proven_size_t n = 0;
    const proven_err_t e = proven_float_format_f64_policy(buf, sizeof buf, v, PROVEN_FLOAT_FORMAT_POLICY_DEFAULT, opt, &n);
    if (e != PROVEN_OK) return e;
    return localize(f, buf, n, out, written);
}

static bool at(proven_u8str_view_t s, proven_size_t i, proven_u8str_view_t w) {
    if (!w.size || w.size > s.size - i) return false;
    for (proven_size_t k = 0; k < w.size; k++) if (s.ptr[i + k] != w.ptr[k]) return false;
    return true;
}

/* The number at the front of s with f's conventions, as ASCII in buf: its length, and the bytes
 * of s taken. 0 when there is none or it is longer than the buffer. */
static proven_size_t delocalize(const proven_numfmt_t *f, proven_u8str_view_t s, bool frac_ok, char *buf, proven_size_t cap, proven_size_t *taken) {
    if (!s.ptr) s.size = 0;
    proven_size_t i = 0, n = 0;
    if (i < s.size && s.ptr[i] == '+') i++;
    else if (at(s, i, f->minus)) { buf[n++] = '-'; i += f->minus.size; }
    else if (i < s.size && s.ptr[i] == '-') { buf[n++] = '-'; i++; }
    const proven_size_t first = n;
    for (;;) {
        if (i < s.size && s.ptr[i] >= '0' && s.ptr[i] <= '9') {
            if (n + 1 >= cap) return 0;
            buf[n++] = (char)s.ptr[i++];
        } else if (n > first && at(s, i, f->group) && i + f->group.size < s.size
                   && s.ptr[i + f->group.size] >= '0' && s.ptr[i + f->group.size] <= '9') {
            i += f->group.size;                           /* a separator between two digits */
        } else break;
    }
    if (n == first) return 0;
    if (frac_ok && at(s, i, f->decimal) && i + f->decimal.size < s.size
        && s.ptr[i + f->decimal.size] >= '0' && s.ptr[i + f->decimal.size] <= '9') {
        i += f->decimal.size;
        buf[n++] = '.';
        while (i < s.size && s.ptr[i] >= '0' && s.ptr[i] <= '9') {
            if (n + 1 >= cap) return 0;
            buf[n++] = (char)s.ptr[i++];
        }
    }
    *taken = i;
    return n;
}

proven_err_t proven_numfmt_parse_i64(const proven_numfmt_t *f, proven_u8str_view_t s, proven_size_t *used, proven_i64 *out) {
    if (!f || !out) return PROVEN_ERR_INVALID_ARG;
    char buf[64];
    proven_size_t taken = 0;
    const proven_size_t n = delocalize(f, s, false, buf, sizeof buf, &taken);
    if (!n) return PROVEN_ERR_INVALID_ARG;
    if (!used && taken != s.size) return PROVEN_ERR_INVALID_ARG;
    proven_i64 v;
    const proven_err_t e = proven_u8str_view_parse_i64((proven_u8str_view_t){ (const proven_byte_t *)buf, n }, 10, NULL, &v);
    if (e != PROVEN_OK) return e;
    if (used) *used = taken;
    *out = v;
    return PROVEN_OK;
}

proven_err_t proven_numfmt_parse_f64(const proven_numfmt_t *f, proven_u8str_view_t s, proven_size_t *used, double *out) {
    if (!f || !out) return PROVEN_ERR_INVALID_ARG;
    char buf[1001];
    proven_size_t taken = 0;
    const proven_size_t n = delocalize(f, s, true, buf, sizeof buf, &taken);
    if (!n) return PROVEN_ERR_INVALID_ARG;
    if (!used && taken != s.size) return PROVEN_ERR_INVALID_ARG;
    const proven_parse_double_result_t r = proven_parse_double_ascii((proven_u8str_view_t){ (const proven_byte_t *)buf, n });
    if (r.err != PROVEN_OK && r.err != PROVEN_ERR_OVERFLOW) return r.err;
    if (used) *used = taken;
    *out = r.val;
    return r.err;
}
