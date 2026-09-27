#include "proven/utf.h"

/*
 * UTF-8 <-> UTF-16, strict (RFC 3629, Unicode 15 chapter 3 table 3-7). Pure computation.
 *
 * Both decoders answer in three ways, and the difference between the last two is the whole
 * reason this file exists rather than a call to MultiByteToWideChar: that function fails the
 * whole buffer for a character cut in half, so text read in pieces cannot be converted with it.
 *
 *   > 0  a whole character of that many units was decoded;
 *     0  the input ends inside a character that could still be valid - NEED_MORE;
 *    -1  the input is malformed, and more of it would not help.
 *
 * The UTF-8 decoder checks every byte it can see before it asks for more, so "E0 80" is
 * malformed at once (E0 must be followed by A0..BF) rather than reported as incomplete.
 */

static int utf8_decode(const proven_byte_t *s, proven_size_t n, proven_u32 *cp_out) {
    proven_byte_t b0 = s[0];
    if (b0 < 0x80) {
        *cp_out = b0;
        return 1;
    }

    int len;
    proven_byte_t lo = 0x80, hi = 0xBF;
    proven_u32 cp;
    if (b0 < 0xC2) {
        return -1;                       /* a continuation byte, or an overlong C0/C1 lead */
    } else if (b0 < 0xE0) {
        len = 2; cp = b0 & 0x1Fu;
    } else if (b0 < 0xF0) {
        len = 3; cp = b0 & 0x0Fu;
        if (b0 == 0xE0) lo = 0xA0;       /* overlong below U+0800 */
        if (b0 == 0xED) hi = 0x9F;       /* U+D800..U+DFFF are not characters */
    } else if (b0 < 0xF5) {
        len = 4; cp = b0 & 0x07u;
        if (b0 == 0xF0) lo = 0x90;       /* overlong below U+10000 */
        if (b0 == 0xF4) hi = 0x8F;       /* above U+10FFFF */
    } else {
        return -1;
    }

    for (int i = 1; i < len; ++i) {
        if ((proven_size_t)i >= n) return 0;
        proven_byte_t b = s[i];
        proven_byte_t min = (i == 1) ? lo : 0x80;
        proven_byte_t max = (i == 1) ? hi : 0xBF;
        if (b < min || b > max) return -1;
        cp = (cp << 6) | (b & 0x3Fu);
    }
    *cp_out = cp;
    return len;
}

static int utf16_decode(const proven_u16 *s, proven_size_t n, proven_u32 *cp_out) {
    proven_u32 u = s[0];
    if (u < 0xD800u || u > 0xDFFFu) {
        *cp_out = u;
        return 1;
    }
    if (u >= 0xDC00u) return -1;         /* a low surrogate with no high one before it */
    if (n < 2) return 0;
    proven_u32 v = s[1];
    if (v < 0xDC00u || v > 0xDFFFu) return -1;
    *cp_out = 0x10000u + ((u - 0xD800u) << 10) + (v - 0xDC00u);
    return 2;
}

static int utf8_width(proven_u32 cp) {
    if (cp < 0x80u) return 1;
    if (cp < 0x800u) return 2;
    if (cp < 0x10000u) return 3;
    return 4;
}

static void utf8_put(proven_byte_t *out, proven_u32 cp, int width) {
    switch (width) {
    case 1:
        out[0] = (proven_byte_t)cp;
        break;
    case 2:
        out[0] = (proven_byte_t)(0xC0u | (cp >> 6));
        out[1] = (proven_byte_t)(0x80u | (cp & 0x3Fu));
        break;
    case 3:
        out[0] = (proven_byte_t)(0xE0u | (cp >> 12));
        out[1] = (proven_byte_t)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (proven_byte_t)(0x80u | (cp & 0x3Fu));
        break;
    default:
        out[0] = (proven_byte_t)(0xF0u | (cp >> 18));
        out[1] = (proven_byte_t)(0x80u | ((cp >> 12) & 0x3Fu));
        out[2] = (proven_byte_t)(0x80u | ((cp >> 6) & 0x3Fu));
        out[3] = (proven_byte_t)(0x80u | (cp & 0x3Fu));
        break;
    }
}

// -------------------------------------------------------------
// Partial
// -------------------------------------------------------------

proven_utf_step_t proven_utf8_to_utf16_partial(proven_u8str_view_t src, proven_u16 *out, proven_size_t out_cap) {
    proven_utf_step_t st = { PROVEN_OK, 0, 0 };
    if ((src.size > 0 && !src.ptr) || (out_cap > 0 && !out)) {
        st.err = PROVEN_ERR_INVALID_ARG;
        return st;
    }

    while (st.consumed < src.size) {
        proven_u32 cp;
        int len = utf8_decode(src.ptr + st.consumed, src.size - st.consumed, &cp);
        if (len < 0) { st.err = PROVEN_ERR_INVALID_ENCODING; return st; }
        if (len == 0) { st.err = PROVEN_ERR_NEED_MORE; return st; }

        proven_size_t units = (cp >= 0x10000u) ? 2 : 1;
        if (out_cap - st.written < units) { st.err = PROVEN_ERR_OUT_OF_BOUNDS; return st; }

        if (units == 1) {
            out[st.written] = (proven_u16)cp;
        } else {
            proven_u32 v = cp - 0x10000u;
            out[st.written]     = (proven_u16)(0xD800u + (v >> 10));
            out[st.written + 1] = (proven_u16)(0xDC00u + (v & 0x3FFu));
        }
        st.written += units;
        st.consumed += (proven_size_t)len;
    }
    return st;
}

proven_utf_step_t proven_utf16_to_utf8_partial(const proven_u16 *src, proven_size_t n,
                                               proven_byte_t *out, proven_size_t out_cap) {
    proven_utf_step_t st = { PROVEN_OK, 0, 0 };
    if ((n > 0 && !src) || (out_cap > 0 && !out)) {
        st.err = PROVEN_ERR_INVALID_ARG;
        return st;
    }

    while (st.consumed < n) {
        proven_u32 cp;
        int len = utf16_decode(src + st.consumed, n - st.consumed, &cp);
        if (len < 0) { st.err = PROVEN_ERR_INVALID_ENCODING; return st; }
        if (len == 0) { st.err = PROVEN_ERR_NEED_MORE; return st; }

        int width = utf8_width(cp);
        if (out_cap - st.written < (proven_size_t)width) { st.err = PROVEN_ERR_OUT_OF_BOUNDS; return st; }
        utf8_put(out + st.written, cp, width);
        st.written += (proven_size_t)width;
        st.consumed += (proven_size_t)len;
    }
    return st;
}

// -------------------------------------------------------------
// Measuring
// -------------------------------------------------------------

proven_result_size_t proven_utf8_to_utf16_size(proven_u8str_view_t src) {
    proven_result_size_t res = { PROVEN_OK, 0 };
    if (src.size > 0 && !src.ptr) { res.err = PROVEN_ERR_INVALID_ARG; return res; }

    /* No overflow check is needed: every UTF-16 unit comes from at least one input byte. */
    proven_size_t i = 0;
    while (i < src.size) {
        proven_u32 cp;
        int len = utf8_decode(src.ptr + i, src.size - i, &cp);
        if (len <= 0) { res.err = PROVEN_ERR_INVALID_ENCODING; res.value = 0; return res; }
        res.value += (cp >= 0x10000u) ? 2 : 1;
        i += (proven_size_t)len;
    }
    return res;
}

proven_result_size_t proven_utf16_to_utf8_size(const proven_u16 *src, proven_size_t n) {
    proven_result_size_t res = { PROVEN_OK, 0 };
    if (n > 0 && !src) { res.err = PROVEN_ERR_INVALID_ARG; return res; }

    proven_size_t i = 0;
    while (i < n) {
        proven_u32 cp;
        int len = utf16_decode(src + i, n - i, &cp);
        if (len <= 0) { res.err = PROVEN_ERR_INVALID_ENCODING; res.value = 0; return res; }
        /* Three bytes per unit at most, so this can wrap only for a unit count above
         * SIZE_MAX / 3 - which no real array reaches, and which is checked anyway. */
        if (PROVEN_CKD_ADD(&res.value, res.value, (proven_size_t)utf8_width(cp))) {
            res.err = PROVEN_ERR_OVERFLOW;
            res.value = 0;
            return res;
        }
        i += (proven_size_t)len;
    }
    return res;
}

// -------------------------------------------------------------
// Fixed capacity, atomic
// -------------------------------------------------------------

proven_err_t proven_utf8_to_utf16(proven_u8str_view_t src, proven_u16 *out, proven_size_t out_cap,
                                  proven_size_t *written) {
    if (written) *written = 0;
    if (out_cap > 0 && !out) return PROVEN_ERR_INVALID_ARG;

    proven_result_size_t need = proven_utf8_to_utf16_size(src);
    if (!proven_is_ok(need.err)) return need.err;
    if (need.value > out_cap) return PROVEN_ERR_OUT_OF_BOUNDS;

    proven_utf_step_t st = proven_utf8_to_utf16_partial(src, out, out_cap);
    if (!proven_is_ok(st.err)) return st.err;   /* unreachable after the measure; kept honest */
    if (written) *written = st.written;
    return PROVEN_OK;
}

proven_err_t proven_utf16_to_utf8(const proven_u16 *src, proven_size_t n, proven_byte_t *out,
                                  proven_size_t out_cap, proven_size_t *written) {
    if (written) *written = 0;
    if (out_cap > 0 && !out) return PROVEN_ERR_INVALID_ARG;

    proven_result_size_t need = proven_utf16_to_utf8_size(src, n);
    if (!proven_is_ok(need.err)) return need.err;
    if (need.value > out_cap) return PROVEN_ERR_OUT_OF_BOUNDS;

    proven_utf_step_t st = proven_utf16_to_utf8_partial(src, n, out, out_cap);
    if (!proven_is_ok(st.err)) return st.err;
    if (written) *written = st.written;
    return PROVEN_OK;
}

// -------------------------------------------------------------
// Growable, atomic
// -------------------------------------------------------------

/* Whether [p, p + bytes) touches [base, base + cap). Compared as integers: the two ranges may
 * belong to different objects, and relational operators on unrelated pointers are undefined. */
static bool ranges_touch(const void *p, proven_size_t bytes, const void *base, proven_size_t cap) {
    if (!p || !base || bytes == 0 || cap == 0) return false;
    uintptr_t a = (uintptr_t)p, b = (uintptr_t)base;
    return a < b + cap && b < a + bytes;
}

/* Chunked through a stack buffer, so a long text costs no scratch allocation. The input is
 * validated first; after that the only failure left is the allocator, and that one is undone
 * by restoring the old length - the terminator is rewritten because a chunk overwrote it. */
#define UTF_CHUNK 256

proven_err_t proven_utf16_append_to_u8str(proven_allocator_t alloc, proven_u8str_t *dst,
                                          const proven_u16 *src, proven_size_t n) {
    if (!dst) return PROVEN_ERR_INVALID_ARG;
    if (n > 0 && !src) return PROVEN_ERR_INVALID_ARG;
    proven_size_t src_bytes;
    if (PROVEN_CKD_MUL(&src_bytes, n, sizeof(proven_u16))) return PROVEN_ERR_OVERFLOW;
    if (ranges_touch(src, src_bytes, dst->internal.ptr, dst->internal.cap)) {
        return PROVEN_ERR_INVALID_ARG;
    }

    proven_result_size_t need = proven_utf16_to_utf8_size(src, n);
    if (!proven_is_ok(need.err)) return need.err;
    if (need.value == 0) return proven_u8str_append_grow(alloc, dst, (proven_u8str_view_t){ 0 });

    proven_size_t old_len = dst->internal.len;
    proven_byte_t chunk[UTF_CHUNK];
    proven_size_t done = 0;
    while (done < n) {
        proven_utf_step_t st = proven_utf16_to_utf8_partial(src + done, n - done, chunk, sizeof chunk);
        /* OUT_OF_BOUNDS only means the chunk is full; anything else after validation is a bug. */
        proven_err_t e = st.err;
        if (e == PROVEN_OK || e == PROVEN_ERR_OUT_OF_BOUNDS) {
            e = proven_u8str_append_grow(alloc, dst, (proven_u8str_view_t){ chunk, st.written });
        }
        if (!proven_is_ok(e)) {
            dst->internal.len = old_len;
            if (dst->internal.ptr) dst->internal.ptr[old_len] = 0;
            return e;
        }
        done += st.consumed;
    }
    return PROVEN_OK;
}

#ifndef PROVEN_NO_U16STR
proven_err_t proven_utf8_append_to_u16str(proven_allocator_t alloc, proven_u16str_t *dst,
                                          proven_u8str_view_t src) {
    if (!dst) return PROVEN_ERR_INVALID_ARG;
    if (src.size > 0 && !src.ptr) return PROVEN_ERR_INVALID_ARG;
    if (ranges_touch(src.ptr, src.size, dst->internal.ptr, dst->internal.cap)) {
        return PROVEN_ERR_INVALID_ARG;
    }

    proven_result_size_t need = proven_utf8_to_utf16_size(src);
    if (!proven_is_ok(need.err)) return need.err;
    if (need.value == 0) return proven_u16str_append_grow(alloc, dst, (proven_u16str_view_t){ 0 });

    proven_size_t old_len = dst->internal.len;
    proven_u16 chunk[UTF_CHUNK];
    proven_size_t done = 0;
    while (done < src.size) {
        proven_utf_step_t st = proven_utf8_to_utf16_partial(
            (proven_u8str_view_t){ src.ptr + done, src.size - done }, chunk, UTF_CHUNK);
        proven_err_t e = st.err;
        if (e == PROVEN_OK || e == PROVEN_ERR_OUT_OF_BOUNDS) {
            e = proven_u16str_append_grow(alloc, dst, (proven_u16str_view_t){ chunk, st.written });
        }
        if (!proven_is_ok(e)) {
            dst->internal.len = old_len;
            if (dst->internal.ptr) ((proven_u16 *)(void *)dst->internal.ptr)[old_len / sizeof(proven_u16)] = 0;
            return e;
        }
        done += st.consumed;
    }
    return PROVEN_OK;
}
#endif
