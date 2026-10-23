#include "proven_internal_h3.h"

#ifndef PROVEN_NO_HTTP3

#include "qpack_static.h"

/*
 * QPACK (RFC 9204) with a dynamic table of capacity 0 - what this side announces, so a peer
 * cannot use one (RFC-0017 B-4). A field section is then its two-part prefix, both zero in
 * effect, and field lines that name the static table or carry literals: no state from one
 * section to the next, no blocked streams, and nothing to keep in step. Anything that would
 * need a dynamic table is QPACK_DECOMPRESSION_FAILED, or an error of the stream it came on.
 *
 * Integers are HPACK's (an N-bit prefix and 7-bit continuations) and so is the Huffman code.
 */

#define QP_INT_MAX_CONTINUATION 9u         /* bytes after the prefix: enough for 2^62 */

static bool qp_eq(proven_u8str_view_t a, const char *b, proven_size_t n) {
    if (a.size != n) return false;
    for (proven_size_t i = 0; i < n; ++i) if (a.ptr[i] != (proven_byte_t)b[i]) return false;
    return true;
}

/* An integer with an N-bit prefix at in[*pos]. False when it runs past the input or past
 * 2^62 - 1. */
static bool qp_int(proven_mem_view_t in, proven_size_t *pos, unsigned prefix, proven_u64 *out) {
    if (*pos >= in.size) return false;
    const proven_u64 mask = ((proven_u64)1 << prefix) - 1u;
    proven_u64 v = in.ptr[(*pos)++] & mask;
    if (v == mask) {
        unsigned shift = 0;
        for (unsigned i = 0;; ++i) {
            if (i == QP_INT_MAX_CONTINUATION || *pos >= in.size) return false;
            const proven_byte_t b = in.ptr[(*pos)++];
            const proven_u64 add = (proven_u64)(b & 0x7f) << shift;
            if (shift > 56 || (add >> shift) != (proven_u64)(b & 0x7f) || add > PROVEN_QUIC_VARINT_MAX - v) return false;
            v += add;
            shift += 7;
            if ((b & 0x80) == 0) break;
        }
    }
    *out = v;
    return true;
}

static void qp_put_int(proven_byte_t *out, proven_size_t *at, unsigned prefix, proven_byte_t first_bits, proven_u64 v) {
    const proven_u64 mask = ((proven_u64)1 << prefix) - 1u;
    if (v < mask) { out[(*at)++] = (proven_byte_t)(first_bits | v); return; }
    out[(*at)++] = (proven_byte_t)(first_bits | mask);
    v -= mask;
    while (v >= 128) { out[(*at)++] = (proven_byte_t)(0x80 | (v & 0x7f)); v >>= 7; }
    out[(*at)++] = (proven_byte_t)v;
}

// -----------------------------------------------------------------------------
// Decoding a field section (section 4.5)
// -----------------------------------------------------------------------------

typedef struct {
    proven_mem_view_t in;
    proven_size_t pos;
    proven_mem_mut_t scratch;
    proven_size_t used;                /* of scratch */
    bool over;                         /* the list passed a limit: nothing more is given out, but the section is read on */
} proven_qp_run_t;

/* A string literal whose length has an N-bit prefix and whose H bit is `hbit` of the first
 * byte. PROVEN_ERR_INVALID_FORMAT, or PROVEN_ERR_OUT_OF_BOUNDS when it is Huffman-coded and
 * the scratch has no room for it. */
static proven_err_t qp_string(proven_qp_run_t *r, unsigned prefix, proven_byte_t hbit, proven_u8str_view_t *out) {
    if (r->pos >= r->in.size) return PROVEN_ERR_INVALID_FORMAT;
    const bool huffman = (r->in.ptr[r->pos] & hbit) != 0;
    proven_u64 n = 0;
    if (!qp_int(r->in, &r->pos, prefix, &n) || n > r->in.size - r->pos) return PROVEN_ERR_INVALID_FORMAT;
    const proven_mem_view_t raw = { .ptr = r->in.ptr + r->pos, .size = (proven_size_t)n };
    r->pos += (proven_size_t)n;
    if (!huffman) {
        *out = (proven_u8str_view_t){ .ptr = raw.ptr, .size = raw.size };
        return PROVEN_OK;
    }
    proven_size_t at = r->used;
    const proven_err_t e = proven_hpack_huffman_decode(raw, r->scratch, &at);
    if (e != PROVEN_OK) return e;
    *out = (proven_u8str_view_t){ .ptr = r->scratch.ptr + r->used, .size = at - r->used };
    r->used = at;
    return PROVEN_OK;
}

static proven_u8str_view_t qp_static_name(proven_size_t i) {
    return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)qpack_static[i].name, .size = qpack_static[i].name_len };
}
static proven_u8str_view_t qp_static_value(proven_size_t i) {
    return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)qpack_static[i].value, .size = qpack_static[i].value_len };
}

proven_err_t proven_qpack_decode(proven_mem_view_t section, proven_http_header_t *headers, bool *never_indexed, proven_size_t header_cap,
                                 proven_mem_mut_t scratch, proven_size_t max_list_bytes, proven_size_t *count) {
    if (count) *count = 0;
    if (!count || (section.size > 0 && !section.ptr) || (header_cap > 0 && !headers) || (scratch.size > 0 && !scratch.ptr)) return PROVEN_ERR_INVALID_ARG;
    proven_qp_run_t r = { .in = section, .scratch = scratch };

    /* The prefix: a Required Insert Count of 0 is the only one a table of capacity 0 allows
     * (section 4.5.1.1: any other could not come from a conformant encoder), and a sign bit
     * of 1 with it would make the Base negative (section 4.5.1.2). The Delta Base is
     * otherwise unused: nothing refers to the table. */
    proven_u64 ric = 0, delta = 0;
    if (!qp_int(section, &r.pos, 8, &ric) || ric != 0) return PROVEN_ERR_INVALID_FORMAT;
    if (r.pos >= section.size) return PROVEN_ERR_INVALID_FORMAT;
    const bool sign = (section.ptr[r.pos] & 0x80) != 0;
    if (!qp_int(section, &r.pos, 7, &delta) || sign) return PROVEN_ERR_INVALID_FORMAT;

    proven_size_t n = 0;
    proven_u64 list = 0;
    while (r.pos < section.size) {
        const proven_size_t field_at = r.pos;
        const proven_byte_t b = section.ptr[r.pos];
        proven_u8str_view_t name = { 0 }, value = { 0 };
        bool never = false;
        if (r.over) r.used = 0;
        proven_u64 index = 0;
        proven_err_t e = PROVEN_OK;
        if ((b & 0x80) != 0) {
            /* An indexed field line (1T): the static table only. */
            if ((b & 0x40) == 0 || !qp_int(section, &r.pos, 6, &index) || index >= QPACK_STATIC_COUNT) return PROVEN_ERR_INVALID_FORMAT;
            name = qp_static_name((proven_size_t)index);
            value = qp_static_value((proven_size_t)index);
        } else if ((b & 0x40) != 0) {
            /* A literal with a name reference (01NT): the static table only. */
            never = (b & 0x20) != 0;
            if ((b & 0x10) == 0 || !qp_int(section, &r.pos, 4, &index) || index >= QPACK_STATIC_COUNT) return PROVEN_ERR_INVALID_FORMAT;
            name = qp_static_name((proven_size_t)index);
            e = qp_string(&r, 7, 0x80, &value);
        } else if ((b & 0x20) != 0) {
            /* A literal with a literal name (001NH). */
            never = (b & 0x10) != 0;
            e = qp_string(&r, 3, 0x08, &name);
            if (e == PROVEN_OK) e = qp_string(&r, 7, 0x80, &value);
        } else {
            /* 0001: indexed after the Base; 0000: a literal named after the Base. Both refer
             * to the dynamic table. */
            return PROVEN_ERR_INVALID_FORMAT;
        }
        if (e == PROVEN_ERR_OUT_OF_BOUNDS) {
            /* The scratch is full: the list is too large to give out. Nothing is kept from
             * one section to the next, so nothing has to be kept in step, but the rest is
             * still read - this field again with the whole scratch to work in - so that a
             * malformed section is told apart from a large one. A field that does not fit
             * even then is larger than any list the scratch was sized for. */
            if (r.over) return PROVEN_ERR_OUT_OF_BOUNDS;
            r.over = true;
            r.pos = field_at;
            continue;
        }
        if (e != PROVEN_OK) return PROVEN_ERR_INVALID_FORMAT;
        list += (proven_u64)name.size + value.size + PROVEN_QPACK_ENTRY_OVERHEAD;
        if ((max_list_bytes != 0 && list > max_list_bytes) || n == header_cap) r.over = true;
        if (!r.over) {
            if (never_indexed) never_indexed[n] = never;
            headers[n++] = (proven_http_header_t){ .name = name, .value = value };
        }
    }
    if (r.over) return PROVEN_ERR_OUT_OF_BOUNDS;
    *count = n;
    return PROVEN_OK;
}

// -----------------------------------------------------------------------------
// Encoding a field section
// -----------------------------------------------------------------------------

static proven_size_t qp_int_size(void) { return 1 + QP_INT_MAX_CONTINUATION; }

proven_size_t proven_qpack_encode_bound(const proven_http_header_t *headers, proven_size_t count) {
    proven_size_t n = 2;                               /* the prefix */
    for (proven_size_t i = 0; i < count && headers; ++i) n += 2 * qp_int_size() + headers[i].name.size + headers[i].value.size;
    return n;
}

/* A string with an N-bit length prefix after `first_bits`, its H bit `hbit`: Huffman-coded
 * when that is shorter. */
static void qp_put_string(proven_byte_t *out, proven_size_t *at, unsigned prefix, proven_byte_t first_bits, proven_byte_t hbit, proven_u8str_view_t s) {
    const proven_size_t coded = proven_hpack_huffman_size(s);
    if (coded < s.size) {
        qp_put_int(out, at, prefix, (proven_byte_t)(first_bits | hbit), coded);
        proven_size_t len = *at;
        (void)proven_hpack_huffman_encode(s, (proven_mem_mut_t){ .ptr = out, .size = len + coded }, &len);
        *at = len;
        return;
    }
    qp_put_int(out, at, prefix, first_bits, s.size);
    for (proven_size_t i = 0; i < s.size; ++i) out[(*at)++] = s.ptr[i];
}

proven_err_t proven_qpack_encode(const proven_http_header_t *headers, proven_size_t count, const bool *sensitive,
                                 proven_mem_mut_t out, proven_size_t *len) {
    if (!len || (count > 0 && !headers) || (out.size > 0 && !out.ptr) || *len > out.size) return PROVEN_ERR_INVALID_ARG;
    for (proven_size_t i = 0; i < count; ++i) {
        if ((headers[i].name.size > 0 && !headers[i].name.ptr) || (headers[i].value.size > 0 && !headers[i].value.ptr)) return PROVEN_ERR_INVALID_ARG;
    }
    if (out.size - *len < proven_qpack_encode_bound(headers, count)) return PROVEN_ERR_OUT_OF_BOUNDS;
    proven_size_t at = *len;
    out.ptr[at++] = 0;                                 /* Required Insert Count 0 */
    out.ptr[at++] = 0;                                 /* Sign 0, Delta Base 0 */
    for (proven_size_t i = 0; i < count; ++i) {
        const proven_u8str_view_t name = headers[i].name, value = headers[i].value;
        /* Never indexed: what the caller marks, and the credentials always, as HPACK's
         * encoder does - the mark tells every later hop too. */
        const bool never = (sensitive && sensitive[i]) || qp_eq(name, "authorization", 13) || qp_eq(name, "proxy-authorization", 19);
        proven_size_t whole = QPACK_STATIC_COUNT, by_name = QPACK_STATIC_COUNT;
        for (proven_size_t k = 0; k < QPACK_STATIC_COUNT; ++k) {
            if (qpack_static[k].name_len != name.size || !qp_eq(name, qpack_static[k].name, name.size)) continue;
            if (by_name == QPACK_STATIC_COUNT) by_name = k;
            if (qpack_static[k].value_len == value.size && qp_eq(value, qpack_static[k].value, value.size)) { whole = k; break; }
        }
        if (whole != QPACK_STATIC_COUNT && !never) {
            qp_put_int(out.ptr, &at, 6, 0xc0, whole);  /* indexed, static */
            continue;
        }
        if (by_name != QPACK_STATIC_COUNT) {
            qp_put_int(out.ptr, &at, 4, (proven_byte_t)(0x50 | (never ? 0x20 : 0)), by_name);
        } else {
            qp_put_string(out.ptr, &at, 3, (proven_byte_t)(0x20 | (never ? 0x10 : 0)), 0x08, name);
        }
        qp_put_string(out.ptr, &at, 7, 0x00, 0x80, value);
    }
    *len = at;
    return PROVEN_OK;
}

// -----------------------------------------------------------------------------
// The peer's encoder and decoder streams (sections 4.3 and 4.4)
// -----------------------------------------------------------------------------

/* An instruction may arrive in pieces: up to its first byte and nine continuation bytes are
 * held. Only an integer is ever read - no instruction that carries a string is accepted. */
static proven_u64 qp_stream_feed(proven_qpack_stream_t *s, proven_mem_view_t in, bool encoder) {
    for (proven_size_t i = 0; i < in.size; ++i) {
        const proven_byte_t b = in.ptr[i];
        if (s->held == 0) {
            if (encoder) {
                /* 001: Set Dynamic Table Capacity. 1T, 01H, 000: inserts and duplicates,
                 * which a table of capacity 0 cannot take. */
                if ((b & 0xe0) != 0x20) return PROVEN_QPACK_ENCODER_STREAM_ERROR;
            } else {
                /* 01: Stream Cancellation. 1: a Section Acknowledgement - nothing this side
                 * sent refers to a table, so none is owed. 00: an Insert Count Increment -
                 * no insert has been made, so any is too many. */
                if ((b & 0xc0) != 0x40) return PROVEN_QPACK_DECODER_STREAM_ERROR;
            }
        }
        s->buf[s->held++] = b;
        const unsigned prefix = encoder ? 5u : 6u;
        proven_size_t pos = 0;
        proven_u64 v = 0;
        if (qp_int((proven_mem_view_t){ .ptr = s->buf, .size = s->held }, &pos, prefix, &v)) {
            /* Complete. A capacity above 0 exceeds what this side allows (section 4.3.1). */
            if (encoder && v != 0) return PROVEN_QPACK_ENCODER_STREAM_ERROR;
            s->held = 0;
            continue;
        }
        /* Incomplete, or too long. */
        if (s->held == sizeof s->buf) return encoder ? PROVEN_QPACK_ENCODER_STREAM_ERROR : PROVEN_QPACK_DECODER_STREAM_ERROR;
    }
    return 0;
}

proven_u64 proven_qpack_encoder_stream_feed(proven_qpack_stream_t *s, proven_mem_view_t in) { return qp_stream_feed(s, in, true); }
proven_u64 proven_qpack_decoder_stream_feed(proven_qpack_stream_t *s, proven_mem_view_t in) { return qp_stream_feed(s, in, false); }

#endif /* PROVEN_NO_HTTP3 */
