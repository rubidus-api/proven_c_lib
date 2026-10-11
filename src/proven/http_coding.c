#include "proven_internal_http_coding.h"

/*
 * Content codings for the HTTP drivers: when a body is decoded, when a response is
 * compressed, and the two small machines that do it over deflate.h.
 */

static proven_u8str_view_t hc_lit_n(const char *s, proven_size_t n) {
    return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)s, .size = n };
}
#define HC_LIT(s) hc_lit_n("" s, sizeof(s) - 1)

// -----------------------------------------------------------------------------
// Decoding
// -----------------------------------------------------------------------------

bool proven_http_decoder_wanted_(proven_u16 status, const proven_http_header_t *headers, proven_size_t count, proven_http_coding_t *coding) {
    /* 206 carries a range of the encoded bytes: a piece of a gzip stream is not one. */
    if (status == 206) return false;
    proven_http_coding_t c = proven_http_content_coding(headers, count);
    if (c != PROVEN_HTTP_CODING_GZIP && c != PROVEN_HTTP_CODING_DEFLATE) return false;
    *coding = c;
    return true;
}

void proven_http_decoder_begin_(proven_http_decoder_t_ *d, proven_allocator_t alloc, proven_http_coding_t coding) {
    *d = (proven_http_decoder_t_){ .alloc = alloc, .coding = (proven_u8)coding };
}

void proven_http_decoder_free_(proven_http_decoder_t_ *d) {
    if (!d) return;
    if (d->z) proven_inflate_destroy(d->z);
    d->z = (void *)0;
}

/* `deflate` is a zlib stream by the standard and raw DEFLATE from some servers. A zlib header
 * is method 8, a window of at most 32 KiB, and sixteen bits divisible by 31; a raw stream
 * begins like that only when it opens with a stored block whose padding bits are not zero. */
static bool hc_is_zlib(const proven_byte_t h[2]) {
    return (h[0] & 0x0fu) == 8u && (h[0] >> 4) <= 7u && (((unsigned)h[0] << 8) | h[1]) % 31u == 0u;
}

proven_err_t proven_http_decoder_step_(proven_http_decoder_t_ *d, proven_mem_view_t in, proven_size_t *consumed,
                                       proven_mem_mut_t out, proven_size_t *produced) {
    *consumed = 0;
    *produced = 0;
    for (;;) {
        if (!d->z) {
            proven_deflate_format_t format = PROVEN_DEFLATE_GZIP;
            if (d->coding == PROVEN_HTTP_CODING_DEFLATE) {
                while (d->held < 2 && *consumed < in.size) d->hold[d->held++] = in.ptr[(*consumed)++];
                if (d->held > 0) d->any = true;
                if (d->held < 2) return PROVEN_OK;
                format = hc_is_zlib(d->hold) ? PROVEN_DEFLATE_ZLIB : PROVEN_DEFLATE_RAW;
            } else if (*consumed == in.size) {
                return PROVEN_OK;
            }
            proven_err_t e = proven_inflate_create(d->alloc, format, &d->z);
            if (e != PROVEN_OK) return e;
        }
        /* The two bytes that were held go in first. */
        bool from_hold = d->held_pos < d->held;
        proven_mem_view_t src = from_hold ? (proven_mem_view_t){ .ptr = d->hold + d->held_pos, .size = (proven_size_t)(d->held - d->held_pos) }
                                          : (proven_mem_view_t){ .ptr = in.ptr + *consumed, .size = in.size - *consumed };
        if (d->ended) {
            if (src.size == 0) return PROVEN_OK;
            /* More bytes after the end of a stream: another gzip member, or nothing valid. */
            if (d->coding != PROVEN_HTTP_CODING_GZIP) return PROVEN_ERR_INVALID_FORMAT;
            proven_inflate_reset(d->z);
            d->ended = false;
        }
        if (src.size > 0) d->any = true;
        /* With no input left this still runs: output the inflater is holding comes out. */
        proven_size_t used = 0, made = 0;
        bool done = false;
        proven_err_t e = proven_inflate(d->z, src, &used, (proven_mem_mut_t){ .ptr = out.ptr + *produced, .size = out.size - *produced }, &made, &done);
        if (e != PROVEN_OK) return e == PROVEN_ERR_NOMEM ? e : PROVEN_ERR_INVALID_FORMAT;
        if (from_hold) d->held_pos = (proven_u8)(d->held_pos + used);
        else *consumed += used;
        *produced += made;
        if (done) d->ended = true;
        else if (used == 0 && made == 0) return PROVEN_OK;      /* it needs room, or more input */
    }
}

proven_err_t proven_http_decoder_end_(const proven_http_decoder_t_ *d) {
    return (!d->any || d->ended) ? PROVEN_OK : PROVEN_ERR_INVALID_FORMAT;
}

// -----------------------------------------------------------------------------
// Compressing
// -----------------------------------------------------------------------------

bool proven_http_compress_applies_(bool accepts_gzip, proven_u16 status, const proven_http_header_t *headers, proven_size_t count) {
    if (!accepts_gzip) return false;
    if (status == 204 || status == 206 || status == 304) return false;
    proven_u8str_view_t value;
    if (proven_http_header_find(headers, count, HC_LIT("Content-Encoding"), &value)) return false;
    if (proven_http_header_find(headers, count, HC_LIT("Content-Range"), &value)) return false;
    return true;
}

bool proven_http_compress_needs_vary_(const proven_http_header_t *headers, proven_size_t count) {
    return !proven_http_header_has_token(headers, count, HC_LIT("Vary"), HC_LIT("Accept-Encoding")) &&
           !proven_http_header_has_token(headers, count, HC_LIT("Vary"), HC_LIT("*"));
}

proven_err_t proven_http_compress_create_(proven_allocator_t alloc, int level, int window_bits, proven_deflate_t **out) {
    proven_deflate_options_t o = { .alloc = alloc, .format = PROVEN_DEFLATE_GZIP, .level = level, .window_bits = window_bits };
    return proven_deflate_create(&o, out);
}

proven_err_t proven_http_compress_all_(proven_allocator_t alloc, int level, int window_bits, proven_mem_view_t body,
                                       proven_byte_t **out, proven_size_t *out_cap, proven_size_t *out_size) {
    *out = (void *)0;
    *out_cap = 0;
    *out_size = 0;
    if (body.size < PROVEN_HTTP_CODING_MIN_BODY) return PROVEN_OK;
    /* No window larger than the body can use: the compressor's state follows the window. */
    int limit = window_bits == 0 ? 15 : window_bits;
    int bits = 9;
    while (bits < limit && ((proven_size_t)1 << bits) < body.size) bits++;
    /* The result must be smaller than the body to be worth sending, so that is all the room
     * it gets. */
    proven_size_t cap = body.size - 1;
    proven_result_mem_mut_t m = alloc.alloc_fn(alloc.ctx, cap, 1);
    if (m.err != PROVEN_OK) return m.err;
    proven_deflate_options_t o = { .alloc = alloc, .format = PROVEN_DEFLATE_GZIP, .level = level, .window_bits = bits };
    proven_size_t written = 0;
    proven_err_t e = proven_deflate_all(&o, body, (proven_mem_mut_t){ .ptr = m.value.ptr, .size = cap }, &written);
    if (e != PROVEN_OK) {
        alloc.free_fn(alloc.ctx, m.value.ptr);
        return e == PROVEN_ERR_OUT_OF_BOUNDS ? PROVEN_OK : e;      /* it did not shrink */
    }
    *out = m.value.ptr;
    *out_cap = cap;
    *out_size = written;
    return PROVEN_OK;
}
