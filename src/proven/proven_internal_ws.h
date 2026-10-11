#ifndef PROVEN_INTERNAL_WS_H
#define PROVEN_INTERNAL_WS_H

#include "proven/deflate.h"
#include "proven/ws.h"

/*
 * Internal: what the two WebSocket drivers share for permessage-deflate (RFC 7692), so that
 * the rules exist once. Not public API.
 *
 * Every message is compressed on its own - no context takeover, in either direction - so all
 * of this lives from the first frame of a message to its last and no longer.
 */

/* The largest piece a driver inflates into or compresses out of in one step. */
#define PROVEN_WS_DEFLATE_PIECE 16384u

/* Messages shorter than this are sent as they are: there is nothing in them to refer back to. */
#define PROVEN_WS_DEFLATE_MIN 64u

/* The UTF-8 check of ws.c, for text that was inflated by a driver. */
[[nodiscard]]
proven_err_t proven_ws_utf8_feed_(proven_byte_t held[4], proven_u8 *held_len, const proven_byte_t *p, proven_size_t n);

/* A compressed message being received. Zero-initialise; proven_ws_inflate_begin_ for each
 * message, proven_ws_inflate_end_ after it (or when it is abandoned). */
typedef struct {
    proven_inflate_t *z;
    proven_u64 total;                  /* decoded bytes so far */
    proven_byte_t held[4];             /* a character cut by the end of a decoded piece */
    proven_u8 held_len;
    proven_u8 tail;                    /* how many of the four closing bytes have been fed */
    bool text;
} proven_ws_inflate_t_;

[[nodiscard]]
proven_err_t proven_ws_inflate_begin_(proven_ws_inflate_t_ *m, proven_allocator_t alloc, bool text);

/* Decode: takes some of `in` (the compressed bytes of one piece of the message), fills some
 * of `out`. With `last` - no more compressed bytes will come - the four bytes the sender left
 * off are fed after `in`, and `*done` is set once everything has come out. Call again while
 * it consumes or produces. PROVEN_ERR_INVALID_FORMAT for bytes that are not DEFLATE,
 * PROVEN_ERR_INVALID_ENCODING for text that is not UTF-8, PROVEN_ERR_OUT_OF_BOUNDS when the
 * decoded message passes `max_bytes` (0: no limit). */
[[nodiscard]]
proven_err_t proven_ws_inflate_step_(proven_ws_inflate_t_ *m, proven_mem_view_t in, proven_size_t *consumed, bool last,
                                     proven_mem_mut_t out, proven_size_t *produced, proven_u64 max_bytes, bool *done);

void proven_ws_inflate_end_(proven_ws_inflate_t_ *m);

/* A message being sent compressed. The window is no larger than `size_hint` needs (0: not
 * known - the whole of `window_bits`). */
[[nodiscard]]
proven_err_t proven_ws_deflate_begin_(proven_allocator_t alloc, int level, int window_bits, proven_size_t size_hint, proven_deflate_t **out);

/* Compress: takes some of `in`, fills some of `out`. With `flush`, what was given so far is
 * made decodable; with `last` as well, the four closing bytes are left off (`*tail` carries
 * that between calls: zero it for each message). `*done` is set when everything asked for has
 * come out; call again with more room until then. */
[[nodiscard]]
proven_err_t proven_ws_deflate_step_(proven_deflate_t *z, proven_mem_view_t in, proven_size_t *consumed, bool flush, bool last,
                                     proven_mem_mut_t out, proven_size_t *produced, proven_byte_t tail[4], proven_u8 *tail_len, bool *done);

#endif /* PROVEN_INTERNAL_WS_H */
