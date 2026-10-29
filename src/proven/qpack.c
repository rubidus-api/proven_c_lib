#include "proven_internal_h3.h"

#ifndef PROVEN_NO_HTTP3

#include "qpack_static.h"
#include "../../platform/proven_sys_mem.h"

/*
 * QPACK (RFC 9204). This side's encoder uses no dynamic table: a field section it writes is its
 * two-part prefix, both zero, and field lines that name the static table or carry literals. As a
 * decoder it has a table when it announces one (RFC-0032, proven_qpack_dec_t): the peer's encoder
 * stream fills it, sections refer to it, and a section that needs entries not inserted yet waits
 * (a blocked stream, which h3_conn.c holds). Without one (capacity 0, RFC-0017 B-4) anything that
 * would need a table is QPACK_DECOMPRESSION_FAILED, or an error of the stream it came on.
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

/* The same, telling a value that has not all arrived (1) from one that is malformed (2): 0 read. */
static int qp_int_more(proven_mem_view_t in, proven_size_t *pos, unsigned prefix, proven_u64 *out) {
    if (*pos >= in.size) return 1;
    proven_size_t end = *pos + 1;
    const proven_u64 mask = ((proven_u64)1 << prefix) - 1u;
    if ((in.ptr[*pos] & mask) == mask) {
        for (unsigned i = 0;; ++i) {
            if (i == QP_INT_MAX_CONTINUATION) return 2;
            if (end >= in.size) return 1;
            if ((in.ptr[end++] & 0x80) == 0) break;
        }
    }
    return qp_int(in, pos, prefix, out) ? 0 : 2;
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

// -----------------------------------------------------------------------------
// The dynamic table, as a decoder (RFC 9204 sections 3.2, 4.3 and 4.5; RFC-0032)
// -----------------------------------------------------------------------------

/* An entry: its name and then its value, `at` bytes into the byte ring. Entries are evicted
 * oldest first, so their bytes stay in insertion order around the ring. */
typedef struct { proven_u32 at, nlen, vlen; } qp_entry_t;

struct proven_qpack_dec {
    proven_allocator_t alloc;
    proven_u32 max_cap;                /* what this side announced */
    proven_u32 cap;                    /* what the peer set, at most max_cap */
    proven_u32 size;                   /* the entries' sizes, name + value + 32 each */
    proven_u32 bytes;                  /* their names and values: what the ring holds */
    proven_byte_t *ring;               /* max_cap bytes */
    proven_u32 head;                   /* where the next entry's bytes go */
    qp_entry_t *ent;                   /* a ring of max_cap / 32 + 1 entries */
    proven_u32 slots, first, count;
    proven_u64 inserts;                /* entries inserted: the next one's absolute index */
    proven_byte_t *ib;                 /* an encoder-stream instruction not yet whole */
    proven_size_t ib_n, ib_cap;
    proven_byte_t *tmp;                /* a name and value decoded or copied, max_cap bytes */
};

#define QP_IB_MAX(d) ((proven_size_t)(d)->max_cap * 4u + 64u)     /* Huffman may take up to 30 bits a byte */

static void *qp_alloc(proven_allocator_t a, proven_size_t n) {
    proven_result_mem_mut_t m = a.alloc_fn(a.ctx, n ? n : 1, 8);
    return m.err == PROVEN_OK ? m.value.ptr : (void *)0;
}

proven_err_t proven_qpack_dec_create(proven_allocator_t alloc, proven_u32 max_capacity, proven_qpack_dec_t **out) {
    if (out) *out = (void *)0;
    if (!out || !proven_alloc_is_valid(alloc) || max_capacity < 32 || max_capacity > ((proven_u32)1 << 20)) return PROVEN_ERR_INVALID_ARG;
    proven_qpack_dec_t *d = qp_alloc(alloc, sizeof *d);
    if (!d) return PROVEN_ERR_NOMEM;
    *d = (proven_qpack_dec_t){ .alloc = alloc, .max_cap = max_capacity, .slots = max_capacity / 32 + 1 };
    *out = d;
    return PROVEN_OK;
}

/* The table's memory, made when the peer first gives it a capacity: a peer that never uses the
 * table costs a connection nothing more. */
static bool qp_make_table(proven_qpack_dec_t *d) {
    if (d->ring) return true;
    d->ring = qp_alloc(d->alloc, d->max_cap);
    d->ent = qp_alloc(d->alloc, (proven_size_t)d->slots * sizeof(qp_entry_t));
    d->tmp = qp_alloc(d->alloc, d->max_cap);
    if (d->ring && d->ent && d->tmp) return true;
    if (d->ring) d->alloc.free_fn(d->alloc.ctx, d->ring);
    if (d->ent) d->alloc.free_fn(d->alloc.ctx, d->ent);
    if (d->tmp) d->alloc.free_fn(d->alloc.ctx, d->tmp);
    d->ring = (void *)0; d->ent = (void *)0; d->tmp = (void *)0;
    return false;
}

void proven_qpack_dec_destroy(proven_qpack_dec_t *d) {
    if (!d) return;
    const proven_allocator_t a = d->alloc;
    if (d->ring) a.free_fn(a.ctx, d->ring);
    if (d->ent) a.free_fn(a.ctx, d->ent);
    if (d->tmp) a.free_fn(a.ctx, d->tmp);
    if (d->ib) a.free_fn(a.ctx, d->ib);
    a.free_fn(a.ctx, d);
}

proven_u64 proven_qpack_dec_inserts(const proven_qpack_dec_t *d) { return d ? d->inserts : 0; }

/* The entry of an absolute index, or null when it is not in the table (evicted, or not yet). */
static const qp_entry_t *qp_entry(const proven_qpack_dec_t *d, proven_u64 abs) {
    const proven_u64 oldest = d->inserts - d->count;
    if (abs < oldest || abs >= d->inserts) return (void *)0;
    return &d->ent[(d->first + (proven_u32)(abs - oldest)) % d->slots];
}

/* Bytes of the ring from `at`, wrapping. */
static void qp_ring_copy(const proven_qpack_dec_t *d, proven_u32 at, proven_u32 n, proven_byte_t *dst) {
    const proven_u32 first = d->max_cap - at < n ? d->max_cap - at : n;
    proven_sys_mem_copy(dst, d->ring + at, first);
    if (n > first) proven_sys_mem_copy(dst + first, d->ring, n - first);
}

static void qp_evict_to(proven_qpack_dec_t *d, proven_u32 limit) {
    while (d->size > limit && d->count > 0) {
        const qp_entry_t *e = &d->ent[d->first];
        d->size -= e->nlen + e->vlen + 32u;
        d->bytes -= e->nlen + e->vlen;
        d->first = (d->first + 1) % d->slots;
        d->count--;
    }
}

/* Insert a name and value that are not in the ring (the caller copied them out first). */
static bool qp_insert(proven_qpack_dec_t *d, const proven_byte_t *name, proven_u32 nlen, const proven_byte_t *value, proven_u32 vlen) {
    const proven_u64 esize = (proven_u64)nlen + vlen + 32u;
    if (esize > d->cap) return false;                  /* larger than the table: section 3.2.2 */
    qp_evict_to(d, d->cap - (proven_u32)esize);
    const proven_u32 at = d->head;
    for (proven_u32 i = 0; i < nlen; ++i) d->ring[(at + i) % d->max_cap] = name[i];
    for (proven_u32 i = 0; i < vlen; ++i) d->ring[(at + nlen + i) % d->max_cap] = value[i];
    d->head = (at + nlen + vlen) % d->max_cap;
    d->ent[(d->first + d->count) % d->slots] = (qp_entry_t){ .at = at, .nlen = nlen, .vlen = vlen };
    d->count++;
    d->size += (proven_u32)esize;
    d->bytes += nlen + vlen;
    d->inserts++;
    return true;
}

/* A string of an encoder instruction at in[*pos]: 1 not all here, 2 malformed or too large, 0
 * read - into tmp at *tmp_at when Huffman-coded (or always, `copy`), else left in place. */
static int qp_enc_string(proven_qpack_dec_t *d, proven_mem_view_t in, proven_size_t *pos, unsigned prefix, proven_byte_t hbit, bool copy,
                         proven_size_t *tmp_at, const proven_byte_t **str, proven_u32 *len) {
    if (*pos >= in.size) return 1;
    const bool huffman = (in.ptr[*pos] & hbit) != 0;
    proven_u64 n = 0;
    const int r = qp_int_more(in, pos, prefix, &n);
    if (r != 0) return r;
    if (n > QP_IB_MAX(d)) return 2;
    if (n > in.size - *pos) return 1;
    const proven_mem_view_t raw = { .ptr = in.ptr + *pos, .size = (proven_size_t)n };
    *pos += (proven_size_t)n;
    if (huffman) {
        proven_size_t at = *tmp_at;
        if (proven_hpack_huffman_decode(raw, (proven_mem_mut_t){ .ptr = d->tmp, .size = d->max_cap }, &at) != PROVEN_OK) return 2;
        *str = d->tmp + *tmp_at; *len = (proven_u32)(at - *tmp_at); *tmp_at = at;
        return 0;
    }
    if (copy) {
        if (raw.size > d->max_cap - *tmp_at) return 2;
        proven_sys_mem_copy(d->tmp + *tmp_at, raw.ptr, raw.size);
        *str = d->tmp + *tmp_at; *len = (proven_u32)raw.size; *tmp_at += raw.size;
        return 0;
    }
    if (raw.size > d->max_cap) return 2;
    *str = raw.ptr; *len = (proven_u32)raw.size;
    return 0;
}

/* One encoder instruction at the start of `in`: 1 not all here, 2 an error, 0 done (`*used`). */
static int qp_enc_instr(proven_qpack_dec_t *d, proven_mem_view_t in, proven_size_t *used) {
    proven_size_t pos = 0, tmp_at = 0;
    const proven_byte_t b = in.ptr[0];
    proven_u64 v = 0;
    int r;
    if ((b & 0x80) != 0) {
        /* Insert With Name Reference (1T): the name from the static table or the dynamic one. */
        if (!d->ring) return 2;
        const bool is_static = (b & 0x40) != 0;
        if ((r = qp_int_more(in, &pos, 6, &v)) != 0) return r;
        const proven_byte_t *name; proven_u32 nlen;
        if (is_static) {
            if (v >= QPACK_STATIC_COUNT) return 2;
            name = (const proven_byte_t *)qpack_static[v].name; nlen = (proven_u32)qpack_static[v].name_len;
        } else {
            if (v >= d->inserts) return 2;
            const qp_entry_t *e = qp_entry(d, d->inserts - 1 - v);
            if (!e) return 2;
            qp_ring_copy(d, e->at, e->nlen, d->tmp);    /* copied out: the insert may evict it */
            name = d->tmp; nlen = e->nlen; tmp_at = e->nlen;
        }
        const proven_byte_t *value; proven_u32 vlen;
        if ((r = qp_enc_string(d, in, &pos, 7, 0x80, false, &tmp_at, &value, &vlen)) != 0) return r;
        if (!qp_insert(d, name, nlen, value, vlen)) return 2;
    } else if ((b & 0x40) != 0) {
        /* Insert With Literal Name (01H): none fits a table of capacity 0. */
        if (!d->ring) return 2;
        const proven_byte_t *name, *value; proven_u32 nlen, vlen;
        if ((r = qp_enc_string(d, in, &pos, 5, 0x20, true, &tmp_at, &name, &nlen)) != 0) return r;
        if ((r = qp_enc_string(d, in, &pos, 7, 0x80, false, &tmp_at, &value, &vlen)) != 0) return r;
        if (!qp_insert(d, name, nlen, value, vlen)) return 2;
    } else if ((b & 0x20) != 0) {
        /* Set Dynamic Table Capacity (001): at most what this side allows (section 4.3.1). */
        if ((r = qp_int_more(in, &pos, 5, &v)) != 0) return r;
        if (v > d->max_cap || (v > 0 && !qp_make_table(d))) return 2;
        d->cap = (proven_u32)v;
        qp_evict_to(d, d->cap);
    } else {
        /* Duplicate (000). */
        if ((r = qp_int_more(in, &pos, 5, &v)) != 0) return r;
        if (v >= d->inserts) return 2;
        const qp_entry_t *e = qp_entry(d, d->inserts - 1 - v);
        if (!e) return 2;
        qp_ring_copy(d, e->at, e->nlen + e->vlen, d->tmp);
        if (!qp_insert(d, d->tmp, e->nlen, d->tmp + e->nlen, e->vlen)) return 2;
    }
    *used = pos;
    return 0;
}

proven_u64 proven_qpack_dec_feed(proven_qpack_dec_t *d, proven_mem_view_t in) {
    if (!d || (in.size > 0 && !in.ptr)) return PROVEN_QPACK_ENCODER_STREAM_ERROR;
    for (proven_size_t off = 0; off < in.size;) {
        /* Whole instructions straight from the input while nothing is held; the rest gathered. */
        proven_mem_view_t at;
        proven_size_t take = 0;
        if (d->ib_n == 0) {
            at = (proven_mem_view_t){ .ptr = in.ptr + off, .size = in.size - off };
        } else {
            take = in.size - off;
            if (take > QP_IB_MAX(d) - d->ib_n) take = QP_IB_MAX(d) - d->ib_n;
            if (take == 0) return PROVEN_QPACK_ENCODER_STREAM_ERROR;
            if (d->ib_cap < d->ib_n + take) {
                proven_size_t want = d->ib_cap ? d->ib_cap : 256;
                while (want < d->ib_n + take) want *= 2;
                if (want > QP_IB_MAX(d)) want = QP_IB_MAX(d);
                proven_byte_t *n = qp_alloc(d->alloc, want);
                if (!n) return PROVEN_QPACK_ENCODER_STREAM_ERROR;
                if (d->ib) { proven_sys_mem_copy(n, d->ib, d->ib_n); d->alloc.free_fn(d->alloc.ctx, d->ib); }
                d->ib = n; d->ib_cap = want;
            }
            proven_sys_mem_copy(d->ib + d->ib_n, in.ptr + off, take);
            d->ib_n += take;
            at = (proven_mem_view_t){ .ptr = d->ib, .size = d->ib_n };
        }
        proven_size_t done = 0;
        for (;;) {
            if (done == at.size) break;
            proven_size_t used = 0;
            const int r = qp_enc_instr(d, (proven_mem_view_t){ .ptr = at.ptr + done, .size = at.size - done }, &used);
            if (r == 2) return PROVEN_QPACK_ENCODER_STREAM_ERROR;
            if (r == 1) break;
            done += used;
        }
        if (d->ib_n == 0) {
            /* What is left is the start of an instruction: held for the next bytes. */
            const proven_size_t rest = at.size - done;
            if (rest > QP_IB_MAX(d)) return PROVEN_QPACK_ENCODER_STREAM_ERROR;
            off += done;
            if (rest == 0) continue;
            if (d->ib_cap < rest) {
                proven_size_t want = 256;
                while (want < rest) want *= 2;
                if (want > QP_IB_MAX(d)) want = QP_IB_MAX(d);
                proven_byte_t *n = qp_alloc(d->alloc, want);
                if (!n) return PROVEN_QPACK_ENCODER_STREAM_ERROR;
                if (d->ib) d->alloc.free_fn(d->alloc.ctx, d->ib);
                d->ib = n; d->ib_cap = want;
            }
            proven_sys_mem_copy(d->ib, in.ptr + off, rest);
            d->ib_n = rest;
            off = in.size;
        } else {
            proven_sys_mem_move(d->ib, d->ib + done, d->ib_n - done);
            d->ib_n -= done;
            off += take;
            if (d->ib_n >= QP_IB_MAX(d)) return PROVEN_QPACK_ENCODER_STREAM_ERROR;
        }
    }
    return 0;
}

/* The Required Insert Count from its encoded form (section 4.5.1.1). */
static proven_err_t qp_required(const proven_qpack_dec_t *d, proven_u64 enc, proven_u64 *ric) {
    *ric = 0;
    if (enc == 0) return PROVEN_OK;
    if (!d) return PROVEN_ERR_INVALID_FORMAT;          /* no table: only 0 */
    const proven_u64 max_entries = d->max_cap / 32, full = 2 * max_entries;
    if (enc > full) return PROVEN_ERR_INVALID_FORMAT;
    const proven_u64 max_value = d->inserts + max_entries;
    proven_u64 r = (max_value / full) * full + enc - 1;
    if (r > max_value) {
        if (r <= full) return PROVEN_ERR_INVALID_FORMAT;
        r -= full;
    }
    if (r == 0) return PROVEN_ERR_INVALID_FORMAT;
    *ric = r;
    return PROVEN_OK;
}

proven_err_t proven_qpack_dec_required(const proven_qpack_dec_t *d, proven_mem_view_t section, proven_u64 *required) {
    if (!required) return PROVEN_ERR_INVALID_ARG;
    *required = 0;
    proven_size_t pos = 0;
    proven_u64 enc = 0;
    if (!qp_int(section, &pos, 8, &enc)) return PROVEN_ERR_INVALID_FORMAT;
    return qp_required(d, enc, required);
}

/* A name or value of the table into the scratch: PROVEN_ERR_OUT_OF_BOUNDS when there is no room. */
static proven_err_t qp_from_table(proven_qp_run_t *r, const proven_qpack_dec_t *d, proven_u32 at, proven_u32 n, proven_u8str_view_t *out) {
    if (n > r->scratch.size - r->used) return PROVEN_ERR_OUT_OF_BOUNDS;
    qp_ring_copy(d, at, n, r->scratch.ptr + r->used);
    *out = (proven_u8str_view_t){ .ptr = r->scratch.ptr + r->used, .size = n };
    r->used += n;
    return PROVEN_OK;
}

proven_size_t proven_qpack_put_section_ack(proven_byte_t *out, proven_u64 stream_id) { proven_size_t at = 0; qp_put_int(out, &at, 7, 0x80, stream_id); return at; }
proven_size_t proven_qpack_put_stream_cancel(proven_byte_t *out, proven_u64 stream_id) { proven_size_t at = 0; qp_put_int(out, &at, 6, 0x40, stream_id); return at; }
proven_size_t proven_qpack_put_insert_count(proven_byte_t *out, proven_u64 increment) { proven_size_t at = 0; qp_put_int(out, &at, 6, 0x00, increment); return at; }

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
    proven_u64 ric = 0;
    return proven_qpack_decode_dyn((void *)0, section, headers, never_indexed, header_cap, scratch, max_list_bytes, count, &ric);
}

proven_err_t proven_qpack_decode_dyn(proven_qpack_dec_t *d, proven_mem_view_t section, proven_http_header_t *headers, bool *never_indexed, proven_size_t header_cap,
                                     proven_mem_mut_t scratch, proven_size_t max_list_bytes, proven_size_t *count, proven_u64 *required) {
    if (count) *count = 0;
    if (required) *required = 0;
    if (!count || !required || (section.size > 0 && !section.ptr) || (header_cap > 0 && !headers) || (scratch.size > 0 && !scratch.ptr)) return PROVEN_ERR_INVALID_ARG;
    proven_qp_run_t r = { .in = section, .scratch = scratch };

    /* The prefix: the Required Insert Count (only 0 without a table: section 4.5.1.1 - any
     * other could not come from a conformant encoder) and the Base, which may not be negative
     * (section 4.5.1.2). A section that needs inserts not yet made waits for them. */
    proven_u64 enc = 0, ric = 0, delta = 0;
    if (!qp_int(section, &r.pos, 8, &enc) || qp_required(d, enc, &ric) != PROVEN_OK) return PROVEN_ERR_INVALID_FORMAT;
    if (r.pos >= section.size) return PROVEN_ERR_INVALID_FORMAT;
    const bool sign = (section.ptr[r.pos] & 0x80) != 0;
    if (!qp_int(section, &r.pos, 7, &delta)) return PROVEN_ERR_INVALID_FORMAT;
    if (sign && delta >= ric) return PROVEN_ERR_INVALID_FORMAT;
    if (!sign && delta > PROVEN_QUIC_VARINT_MAX - ric) return PROVEN_ERR_INVALID_FORMAT;
    const proven_u64 base = sign ? ric - delta - 1 : ric + delta;
    *required = ric;
    if (ric > 0 && ric > d->inserts) return PROVEN_ERR_AGAIN;
    proven_u64 top = 0;                                /* one above the highest entry referred to */

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
        const qp_entry_t *dyn = (void *)0;           /* an entry of the dynamic table this line names */
        bool whole = false;                            /* ... and it is the whole field */
        if ((b & 0x80) != 0) {
            /* An indexed field line (1T). */
            if (!qp_int(section, &r.pos, 6, &index)) return PROVEN_ERR_INVALID_FORMAT;
            if ((b & 0x40) != 0) {
                if (index >= QPACK_STATIC_COUNT) return PROVEN_ERR_INVALID_FORMAT;
                name = qp_static_name((proven_size_t)index);
                value = qp_static_value((proven_size_t)index);
            } else {
                if (index >= base || !(dyn = qp_entry(d, base - 1 - index)) || base - 1 - index >= ric) return PROVEN_ERR_INVALID_FORMAT;
                if (base - index > top) top = base - index;
                whole = true;
            }
        } else if ((b & 0x40) != 0) {
            /* A literal with a name reference (01NT). */
            never = (b & 0x20) != 0;
            if (!qp_int(section, &r.pos, 4, &index)) return PROVEN_ERR_INVALID_FORMAT;
            if ((b & 0x10) != 0) {
                if (index >= QPACK_STATIC_COUNT) return PROVEN_ERR_INVALID_FORMAT;
                name = qp_static_name((proven_size_t)index);
            } else {
                if (index >= base || !(dyn = qp_entry(d, base - 1 - index)) || base - 1 - index >= ric) return PROVEN_ERR_INVALID_FORMAT;
                if (base - index > top) top = base - index;
            }
        } else if ((b & 0x20) != 0) {
            /* A literal with a literal name (001NH). */
            never = (b & 0x10) != 0;
            e = qp_string(&r, 3, 0x08, &name);
            if (e == PROVEN_OK) e = qp_string(&r, 7, 0x80, &value);
        } else if ((b & 0x10) != 0) {
            /* Indexed after the Base (0001). */
            if (!qp_int(section, &r.pos, 4, &index) || index > PROVEN_QUIC_VARINT_MAX - base || base + index >= ric || !(dyn = qp_entry(d, base + index))) return PROVEN_ERR_INVALID_FORMAT;
            if (base + index + 1 > top) top = base + index + 1;
            whole = true;
        } else {
            /* A literal named after the Base (0000N). */
            never = (b & 0x08) != 0;
            if (!qp_int(section, &r.pos, 3, &index) || index > PROVEN_QUIC_VARINT_MAX - base || base + index >= ric || !(dyn = qp_entry(d, base + index))) return PROVEN_ERR_INVALID_FORMAT;
            if (base + index + 1 > top) top = base + index + 1;
        }
        if (e == PROVEN_OK && dyn) {
            /* From the table: copied out, as a later insert may evict it. */
            e = qp_from_table(&r, d, dyn->at, dyn->nlen, &name);
            if (e == PROVEN_OK && whole) e = qp_from_table(&r, d, (dyn->at + dyn->nlen) % d->max_cap, dyn->vlen, &value);
        }
        if (e == PROVEN_OK && !whole && (b & 0xc0) == 0x40) e = qp_string(&r, 7, 0x80, &value);
        if (e == PROVEN_OK && !whole && (b & 0xf0) == 0x00) e = qp_string(&r, 7, 0x80, &value);
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
    /* A Required Insert Count above what the section needs could not come from a conformant
     * encoder (section 4.5.1.1). */
    if (ric > 0 && top != ric) return PROVEN_ERR_INVALID_FORMAT;
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
