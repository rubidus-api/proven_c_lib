#ifndef PROVEN_ZSTD_H
#define PROVEN_ZSTD_H

#include "proven/types.h"
#include "proven/error.h"
#include "proven/memory.h"
#include "proven/allocator.h"

/**
 * @file zstd.h
 * @brief Zstandard decompression (RFC 8878).
 *
 * Zstandard is a newer compression format than DEFLATE: HTTP's `Content-Encoding: zstd`
 * (RFC 9659), `.zst` files, and some package and archive formats. It decodes faster than
 * DEFLATE and its compressors trade speed for size over a wider range.
 *
 * Only decompression is here. The decoder works as deflate.h's does - a state object, and a
 * step that takes what input there is and fills what output space there is - with two
 * differences that the format makes necessary:
 *
 *   - **A step may allocate.** Zstandard reads parts of each block from its end, so a block
 *     (at most 128 KiB compressed) is held whole before it is decoded, and its output is
 *     decoded whole too, next to the history that later blocks refer to - the frame's
 *     window. These buffers grow as a frame's output grows, never past what the frame's
 *     header says it needs, and are kept for the next frame. At most, a decoder holds its
 *     window limit plus 384 KiB plus about 16 KiB of tables. A step can therefore return
 *     PROVEN_ERR_NOMEM.
 *   - **Output can be held back** by up to one block: a block's bytes appear once it is
 *     decoded, and wait in the decoder while the output space given is too small.
 *
 * **The window limit is the caller's.** Each frame states how far back it refers, up to
 * terabytes; a frame that needs more than the decoder's limit is refused before anything is
 * allocated for it. RFC 9659 has HTTP's `zstd` coding limited to 8 MiB (8,388,608 bytes),
 * which is the default here. Frames that need a dictionary are refused: content that is sent
 * over HTTP does not use one (RFC 8878, section 7.2).
 *
 * A Zstandard stream is a sequence of frames. Skippable frames (magic numbers 0x184D2A50 to
 * 0x184D2A5F) are read past without producing anything. Malformed input is an error, never a
 * read or a write outside the buffers, and the output is bounded by the caller as with
 * deflate.h.
 *
 * Pure computation: allocation through the allocator given, no OS; part of the freestanding
 * profile.
 */

/** @brief The default window limit: 8 MiB, which RFC 9659 requires an HTTP decoder to accept. */
#define PROVEN_ZSTD_WINDOW_DEFAULT ((proven_u64)8 * 1024 * 1024)

/** @brief A decompressor. Opaque; about 16 KiB when made, growing as described above. */
typedef struct proven_zstd_decoder proven_zstd_decoder_t;

/**
 * @brief Make a decompressor.
 * @param window_max The largest window a frame may need: 0 for PROVEN_ZSTD_WINDOW_DEFAULT;
 *                   otherwise at least 1 KiB (the smallest window a frame can state) and at
 *                   most 2 GiB.
 * @return PROVEN_ERR_INVALID_ARG for a bad allocator, limit or out pointer; PROVEN_ERR_NOMEM.
 */
[[nodiscard]]
proven_err_t proven_zstd_decoder_create(proven_allocator_t alloc, proven_u64 window_max, proven_zstd_decoder_t **out);

/** @brief Free a decompressor and its buffers. NULL is allowed. */
void proven_zstd_decoder_destroy(proven_zstd_decoder_t *z);

/** @brief Forget the stream in progress and expect the beginning of a frame. The buffers are
 *         kept. */
void proven_zstd_decoder_reset(proven_zstd_decoder_t *z);

/**
 * @brief Decompress: consume some of `in`, produce some of `out`.
 *
 * Call it again with the rest of the input, or with more output space, until `*done`.
 * `*done` is true when the input given so far has ended where a frame ends - a frame that
 * produces data, or a skippable one - and all of its data has been produced. The step stops
 * there: input after a frame's end is not consumed by that call, and `*consumed` says where
 * the frame ended. To go on with the frames that follow, call it again with that input; the
 * next frame begins without a reset, and `*done` is false until it ends.
 *
 * @param z         The decompressor.
 * @param in        Compressed bytes; may be empty.
 * @param consumed  Out: how many of them were taken.
 * @param out       Space for decompressed bytes; may be empty. Only the first `*produced`
 *                  bytes are written to.
 * @param produced  Out: how many were written.
 * @param done      Out: true at the end of a frame, as above.
 * @return PROVEN_OK, also when no progress was possible; PROVEN_ERR_INVALID_FORMAT for data
 *         that is not Zstandard, is damaged, or whose checksum or stated size does not match;
 *         PROVEN_ERR_OUT_OF_BOUNDS for a frame whose window is above the limit;
 *         PROVEN_ERR_UNSUPPORTED for a frame that needs a dictionary; PROVEN_ERR_NOMEM;
 *         PROVEN_ERR_INVALID_ARG. After an error every call returns that error until reset.
 */
[[nodiscard]]
proven_err_t proven_zstd_decode(proven_zstd_decoder_t *z, proven_mem_view_t in, proven_size_t *consumed,
                                proven_mem_mut_t out, proven_size_t *produced, bool *done);

/**
 * @brief Decompress every frame of a whole buffer into a buffer of the caller's.
 *
 * The size of `out` is the limit: a stream that would produce more is refused, however
 * small it is itself. The window limit is the default.
 *
 * @param written  Out: how many bytes were produced.
 * @return PROVEN_ERR_OUT_OF_BOUNDS when the data does not fit in `out` (or a window is above
 *         the limit); PROVEN_ERR_INVALID_FORMAT also for an empty `in` and when `in` ends
 *         inside a frame; otherwise as proven_zstd_decode.
 */
[[nodiscard]]
proven_err_t proven_zstd_decode_all(proven_allocator_t alloc, proven_mem_view_t in, proven_mem_mut_t out, proven_size_t *written);

#endif
