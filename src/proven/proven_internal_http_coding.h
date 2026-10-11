#ifndef PROVEN_INTERNAL_HTTP_CODING_H
#define PROVEN_INTERNAL_HTTP_CODING_H

#include "proven/deflate.h"
#include "proven/http.h"

/*
 * Internal: content codings for the HTTP drivers - the rules that the two clients and the two
 * servers must agree on, written once. Not public API.
 */

/* The largest piece a driver decodes or compresses in one step. */
#define PROVEN_HTTP_CODING_PIECE 16384u

/* Bodies shorter than this are sent as they are: a gzip member costs 18 bytes before it
 * saves any. */
#define PROVEN_HTTP_CODING_MIN_BODY 256u

/* A response body being decoded. Zero-initialise, then proven_http_decoder_begin_. */
typedef struct {
    proven_allocator_t alloc;
    proven_inflate_t *z;
    proven_u8 coding;                  /* PROVEN_HTTP_CODING_GZIP or _DEFLATE */
    proven_u8 held;                    /* `deflate`: bytes kept until two tell zlib from raw */
    proven_u8 held_pos;
    bool any;                          /* some input has been seen */
    bool ended;                        /* a stream has ended and nothing came after it */
    proven_byte_t hold[2];
} proven_http_decoder_t_;

/* Whether a client that decodes would decode this response. */
bool proven_http_decoder_wanted_(proven_u16 status, const proven_http_header_t *headers, proven_size_t count, proven_http_coding_t *coding);

void proven_http_decoder_begin_(proven_http_decoder_t_ *d, proven_allocator_t alloc, proven_http_coding_t coding);

/* Decode: uses some of `in`, fills some of `out`. PROVEN_OK with nothing used and nothing
 * produced means it needs more of one or the other. PROVEN_ERR_INVALID_FORMAT for a stream
 * that is not what it claims or has bytes after its end; PROVEN_ERR_NOMEM. */
[[nodiscard]]
proven_err_t proven_http_decoder_step_(proven_http_decoder_t_ *d, proven_mem_view_t in, proven_size_t *consumed,
                                       proven_mem_mut_t out, proven_size_t *produced);

/* The body is over: PROVEN_OK when it ended where a stream ends, or had no bytes at all;
 * PROVEN_ERR_INVALID_FORMAT when the stream is unfinished. */
[[nodiscard]]
proven_err_t proven_http_decoder_end_(const proven_http_decoder_t_ *d);

void proven_http_decoder_free_(proven_http_decoder_t_ *d);

/* Whether a response that its handler asked to have compressed is: the request accepts gzip
 * (`accepts_gzip`), the status has a body of its own, and the handler's headers neither name
 * a coding already nor describe a range. */
bool proven_http_compress_applies_(bool accepts_gzip, proven_u16 status, const proven_http_header_t *headers, proven_size_t count);

/* Whether `Vary: Accept-Encoding` has to be added: false when the handler's own `Vary` lists
 * it, or is `*`. */
bool proven_http_compress_needs_vary_(const proven_http_header_t *headers, proven_size_t count);

/* A compressor for a streamed gzip response. `level` and `window_bits` as in the server's
 * configuration (zero: the defaults). */
[[nodiscard]]
proven_err_t proven_http_compress_create_(proven_allocator_t alloc, int level, int window_bits, proven_deflate_t **out);

/* Compress a whole body. On PROVEN_OK `*out` is a new allocation of `*out_cap` bytes holding
 * `*out_size` bytes of gzip - or null, when the body is too short to gain or did not shrink:
 * send it as it is. Free with the same allocator. */
[[nodiscard]]
proven_err_t proven_http_compress_all_(proven_allocator_t alloc, int level, int window_bits, proven_mem_view_t body,
                                       proven_byte_t **out, proven_size_t *out_cap, proven_size_t *out_size);

#endif /* PROVEN_INTERNAL_HTTP_CODING_H */
