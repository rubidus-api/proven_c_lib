#ifndef PROVEN_INTERNAL_HTTP_CODING_H
#define PROVEN_INTERNAL_HTTP_CODING_H

#include "proven/deflate.h"
#include "proven/zstd.h"
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
    proven_zstd_decoder_t *zs;         /* `zstd`: up to the 8 MiB window RFC 9659 allows, and 384 KiB */
    proven_u8 coding;                  /* PROVEN_HTTP_CODING_GZIP, _DEFLATE or _ZSTD */
    proven_u8 held;                    /* `deflate`: bytes kept until two tell zlib from raw */
    proven_u8 held_pos;
    bool any;                          /* some input has been seen */
    bool ended;                        /* a stream has ended and nothing came after it */
    proven_byte_t hold[2];
} proven_http_decoder_t_;

/* Whether a client that decodes would decode this response; `zstd`: whether it asked for
 * `zstd` too. */
bool proven_http_decoder_wanted_(proven_u16 status, const proven_http_header_t *headers, proven_size_t count, bool zstd, proven_http_coding_t *coding);

/* The `Accept-Encoding` a client that decodes sends. */
proven_u8str_view_t proven_http_decoder_accept_(bool zstd);

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

/* How a server compresses: its configuration's fields, defaults not yet applied. */
typedef struct {
    int level, window_bits;            /* gzip */
    bool zstd;                         /* zstd offered */
    int zstd_level, zstd_window_log;
} proven_http_compress_cfg_t_;

/* Whether the fields are in range: gzip level -1 to 9 and window 0 or 9 to 15, zstd level 0
 * to 9 and window 0 or 10 to 23. */
bool proven_http_compress_cfg_valid_(const proven_http_compress_cfg_t_ *cfg);

/* The coding a response asked to be compressed goes in: zstd when it is offered and the
 * request weighs it at least as high as gzip; gzip when the request accepts that; identity
 * otherwise. */
proven_http_coding_t proven_http_compress_choose_(const proven_http_header_t *headers, proven_size_t count, bool zstd);

/* Its name in `Content-Encoding`. */
proven_u8str_view_t proven_http_coding_token_(proven_http_coding_t coding);

/* Whether a response that its handler asked to have compressed is: the request accepts a
 * coding the server offers (`accepted`), the status has a body of its own, and the handler's
 * headers neither name a coding already nor describe a range. */
bool proven_http_compress_applies_(bool accepted, proven_u16 status, const proven_http_header_t *headers, proven_size_t count);

/* Whether `Vary: Accept-Encoding` has to be added: false when the handler's own `Vary` lists
 * it, or is `*`. */
bool proven_http_compress_needs_vary_(const proven_http_header_t *headers, proven_size_t count);

/* A compressor for a streamed response, gzip or zstd. */
typedef struct proven_http_compressor proven_http_compressor_t_;

[[nodiscard]]
proven_err_t proven_http_compressor_create_(proven_allocator_t alloc, proven_http_coding_t coding, const proven_http_compress_cfg_t_ *cfg,
                                            proven_http_compressor_t_ **out);

void proven_http_compressor_destroy_(proven_http_compressor_t_ *c);

/* As proven_deflate: NONE, SYNC (for zstd, a block flush: all given so far is decodable) or
 * FINISH. */
[[nodiscard]]
proven_err_t proven_http_compressor_step_(proven_http_compressor_t_ *c, proven_mem_view_t in, proven_size_t *consumed,
                                          proven_mem_mut_t out, proven_size_t *produced, proven_deflate_flush_t flush, bool *done);

/* Compress a whole body. On PROVEN_OK `*out` is a new allocation of `*out_cap` bytes holding
 * `*out_size` bytes in `coding` - or null, when the body is too short to gain or did not
 * shrink: send it as it is. Free with the same allocator. */
[[nodiscard]]
proven_err_t proven_http_compress_all_(proven_allocator_t alloc, proven_http_coding_t coding, const proven_http_compress_cfg_t_ *cfg, proven_mem_view_t body,
                                       proven_byte_t **out, proven_size_t *out_cap, proven_size_t *out_size);

#endif /* PROVEN_INTERNAL_HTTP_CODING_H */
