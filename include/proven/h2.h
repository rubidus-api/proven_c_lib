#ifndef PROVEN_H2_H
#define PROVEN_H2_H

#include "proven/types.h"
#include "proven/memory.h"
#include "proven/http.h"

/**
 * @file h2.h
 * @brief HTTP/2 (RFC 9113): frames, and the rules for a header list.
 *
 * HTTP/2 carries the same requests and responses as HTTP/1.1 in another shape. Everything is
 * a *frame* - nine bytes of header and a payload - on a numbered *stream*; many streams share
 * one connection; header fields travel compressed (hpack.h) and split across frames; and
 * what HTTP/1.1 said in its first line is said by *pseudo-header fields* (`:method`, `:path`,
 * `:status`).
 *
 * This header is the part of that with no state: reading and writing frames, the checks each
 * frame type demands, and the checks a decoded header list must pass before it is a request or
 * a response. It is pure computation, allocates nothing, and is available in a freestanding
 * build. It does not hold a connection together - which stream is open, how much may be sent
 * - and so it is not, alone, a way to speak HTTP/2.
 *
 * **Errors come in two sizes in HTTP/2**, and the functions here say which: a *connection
 * error* ends the connection (after a GOAWAY frame saying why), a *stream error* ends one
 * stream (with RST_STREAM) and leaves the rest alone. Each check reports the code to send.
 */

/** @brief What a client sends first on a connection, before any frame: 24 bytes. */
#define PROVEN_H2_PREFACE "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"
#define PROVEN_H2_PREFACE_SIZE ((proven_size_t)24)

/** @brief Every frame begins with this many bytes of header. */
#define PROVEN_H2_FRAME_HEADER_SIZE ((proven_size_t)9)

/** @brief The largest payload every endpoint must accept, and the largest any may allow. */
#define PROVEN_H2_MIN_MAX_FRAME_SIZE ((proven_u32)16384)
#define PROVEN_H2_MAX_MAX_FRAME_SIZE ((proven_u32)16777215)

/** @brief The largest flow-control window, and the one a stream begins with. */
#define PROVEN_H2_MAX_WINDOW ((proven_u32)0x7fffffff)
#define PROVEN_H2_DEFAULT_WINDOW ((proven_u32)65535)

typedef enum {
    PROVEN_H2_DATA = 0x0,
    PROVEN_H2_HEADERS = 0x1,
    PROVEN_H2_PRIORITY = 0x2,          /**< deprecated by RFC 9113: checked for size and ignored */
    PROVEN_H2_RST_STREAM = 0x3,
    PROVEN_H2_SETTINGS = 0x4,
    PROVEN_H2_PUSH_PROMISE = 0x5,
    PROVEN_H2_PING = 0x6,
    PROVEN_H2_GOAWAY = 0x7,
    PROVEN_H2_WINDOW_UPDATE = 0x8,
    PROVEN_H2_CONTINUATION = 0x9
} proven_h2_frame_type_t;

/** @brief Frame flags. A flag means something only on the frame types named. */
#define PROVEN_H2_FLAG_END_STREAM ((proven_u8)0x01)    /**< DATA, HEADERS: the sender's last frame on this stream */
#define PROVEN_H2_FLAG_ACK ((proven_u8)0x01)           /**< SETTINGS, PING: an acknowledgement */
#define PROVEN_H2_FLAG_END_HEADERS ((proven_u8)0x04)   /**< HEADERS, PUSH_PROMISE, CONTINUATION: the header block ends here */
#define PROVEN_H2_FLAG_PADDED ((proven_u8)0x08)        /**< DATA, HEADERS, PUSH_PROMISE: a length byte and padding surround the content */
#define PROVEN_H2_FLAG_PRIORITY ((proven_u8)0x20)      /**< HEADERS: five bytes of (deprecated) priority precede the block */

/** @brief Error codes, as sent in RST_STREAM and GOAWAY (RFC 9113, section 7). */
typedef enum {
    PROVEN_H2_NO_ERROR = 0x0,
    PROVEN_H2_PROTOCOL_ERROR = 0x1,
    PROVEN_H2_INTERNAL_ERROR = 0x2,
    PROVEN_H2_FLOW_CONTROL_ERROR = 0x3,
    PROVEN_H2_SETTINGS_TIMEOUT = 0x4,
    PROVEN_H2_STREAM_CLOSED = 0x5,
    PROVEN_H2_FRAME_SIZE_ERROR = 0x6,
    PROVEN_H2_REFUSED_STREAM = 0x7,
    PROVEN_H2_CANCEL = 0x8,
    PROVEN_H2_COMPRESSION_ERROR = 0x9,
    PROVEN_H2_CONNECT_ERROR = 0xa,
    PROVEN_H2_ENHANCE_YOUR_CALM = 0xb,
    PROVEN_H2_INADEQUATE_SECURITY = 0xc,
    PROVEN_H2_HTTP_1_1_REQUIRED = 0xd
} proven_h2_error_t;

/** @brief Settings (RFC 9113, section 6.5.2). */
typedef enum {
    PROVEN_H2_SETTINGS_HEADER_TABLE_SIZE = 0x1,
    PROVEN_H2_SETTINGS_ENABLE_PUSH = 0x2,
    PROVEN_H2_SETTINGS_MAX_CONCURRENT_STREAMS = 0x3,
    PROVEN_H2_SETTINGS_INITIAL_WINDOW_SIZE = 0x4,
    PROVEN_H2_SETTINGS_MAX_FRAME_SIZE = 0x5,
    PROVEN_H2_SETTINGS_MAX_HEADER_LIST_SIZE = 0x6
} proven_h2_setting_id_t;

/** @brief One setting: an identifier and its value. */
typedef struct {
    proven_u16 id;
    proven_u32 value;
} proven_h2_setting_t;

/** @brief A frame header: everything about a frame but its payload. */
typedef struct {
    proven_u32 length;                 /**< bytes of payload that follow the header: at most 2^24-1 */
    proven_u8 type;                    /**< a proven_h2_frame_type_t, or a type this library does not know */
    proven_u8 flags;
    proven_u32 stream_id;              /**< 0 for the connection itself; the reserved top bit is dropped on reading */
} proven_h2_frame_t;

// -----------------------------------------------------------------------------
// Frames
// -----------------------------------------------------------------------------

/**
 * @brief Read a frame header from the front of `data`.
 * @return PROVEN_ERR_NEED_MORE when `data` is shorter than nine bytes. Nothing else can be
 *         wrong with nine bytes: what they may say is proven_h2_frame_check's business.
 */
[[nodiscard]]
proven_err_t proven_h2_frame_parse(proven_mem_view_t data, proven_h2_frame_t *out);

/**
 * @brief Append a frame header at `out.ptr[*len]`. The payload is yours to write after it.
 * @return PROVEN_ERR_INVALID_ARG for a length above 2^24-1 or a stream identifier above 2^31-1;
 *         PROVEN_ERR_OUT_OF_BOUNDS when `out` is too small, with `*len` unchanged.
 */
[[nodiscard]]
proven_err_t proven_h2_frame_write(proven_mem_mut_t out, proven_size_t *len, const proven_h2_frame_t *frame);

/**
 * @brief Check a frame header against the rules of its type, before its payload is read.
 *
 * Size and stream: no frame is longer than `max_frame_size` (what this side announced);
 * DATA, HEADERS, PRIORITY, RST_STREAM and CONTINUATION need a stream, SETTINGS, PING and
 * GOAWAY must not have one; RST_STREAM and WINDOW_UPDATE are four bytes, PING eight, PRIORITY
 * five, a SETTINGS frame a multiple of six and its acknowledgement empty, GOAWAY at least
 * eight. A type this library does not know passes, as the protocol requires: skip its
 * payload.
 *
 * @param code receives the error code to send when the frame is refused.
 * @param stream_error receives true when the fault ends only the frame's stream (RST_STREAM),
 *        false when it ends the connection (GOAWAY).
 * @return PROVEN_ERR_INVALID_FORMAT when the frame is refused.
 */
[[nodiscard]]
proven_err_t proven_h2_frame_check(const proven_h2_frame_t *frame, proven_u32 max_frame_size, proven_h2_error_t *code, bool *stream_error);

/**
 * @brief The content of a DATA or HEADERS payload: without its padding, and for HEADERS
 *        without the five bytes of priority when those are present.
 *
 * `*content` is a view into `payload`. For flow control the whole payload counts, padding
 * included; this says only where the content is.
 *
 * @return PROVEN_ERR_INVALID_FORMAT (a connection error, PROTOCOL_ERROR) when the padding is
 *         as long as the payload or longer, or the payload too short for what its flags
 *         promise; PROVEN_ERR_INVALID_ARG for another frame type.
 */
[[nodiscard]]
proven_err_t proven_h2_frame_content(const proven_h2_frame_t *frame, proven_mem_view_t payload, proven_mem_view_t *content);

/**
 * @brief Read the next setting of a SETTINGS payload, from `payload.ptr[*pos]`.
 *
 * A setting this library does not know is returned like any other: ignore it, as the protocol
 * requires. A known one outside its range is refused.
 *
 * @return PROVEN_ERR_EOF at the end of the payload. PROVEN_ERR_INVALID_FORMAT with `*code` -
 *         ENABLE_PUSH other than 0 or 1 and MAX_FRAME_SIZE outside 2^14..2^24-1
 *         (PROTOCOL_ERROR), INITIAL_WINDOW_SIZE above 2^31-1 (FLOW_CONTROL_ERROR): each a
 *         connection error.
 */
[[nodiscard]]
proven_err_t proven_h2_settings_next(proven_mem_view_t payload, proven_size_t *pos, proven_h2_setting_t *out, proven_h2_error_t *code);

/** @brief Append a whole SETTINGS frame carrying `count` settings (none: an empty frame). */
[[nodiscard]]
proven_err_t proven_h2_write_settings(proven_mem_mut_t out, proven_size_t *len, const proven_h2_setting_t *settings, proven_size_t count);

/** @brief Append the acknowledgement of a SETTINGS frame. */
[[nodiscard]]
proven_err_t proven_h2_write_settings_ack(proven_mem_mut_t out, proven_size_t *len);

/** @brief Append a PING frame with eight bytes of your choosing, or its acknowledgement
 *         carrying the eight that were received. */
[[nodiscard]]
proven_err_t proven_h2_write_ping(proven_mem_mut_t out, proven_size_t *len, const proven_byte_t opaque[8], bool ack);

/** @brief Append a WINDOW_UPDATE frame: `increment` more bytes may be sent on `stream_id`
 *         (0: on the connection). PROVEN_ERR_INVALID_ARG for an increment of 0 or above 2^31-1. */
[[nodiscard]]
proven_err_t proven_h2_write_window_update(proven_mem_mut_t out, proven_size_t *len, proven_u32 stream_id, proven_u32 increment);

/** @brief Append a RST_STREAM frame. PROVEN_ERR_INVALID_ARG for stream 0. */
[[nodiscard]]
proven_err_t proven_h2_write_rst_stream(proven_mem_mut_t out, proven_size_t *len, proven_u32 stream_id, proven_h2_error_t code);

/** @brief Append a GOAWAY frame: no stream above `last_stream_id` was or will be acted on. */
[[nodiscard]]
proven_err_t proven_h2_write_goaway(proven_mem_mut_t out, proven_size_t *len, proven_u32 last_stream_id, proven_h2_error_t code, proven_mem_view_t debug);

/** @brief Read the payload of a WINDOW_UPDATE frame that passed proven_h2_frame_check.
 *  @return PROVEN_ERR_INVALID_FORMAT for an increment of zero: PROTOCOL_ERROR, on the stream
 *          when the frame names one, on the connection otherwise. */
[[nodiscard]]
proven_err_t proven_h2_read_window_update(proven_mem_view_t payload, proven_u32 *increment);

/** @brief Read the payload of a RST_STREAM frame that passed proven_h2_frame_check. */
[[nodiscard]]
proven_err_t proven_h2_read_rst_stream(proven_mem_view_t payload, proven_u32 *code);

/** @brief Read the payload of a GOAWAY frame that passed proven_h2_frame_check. `*debug` is a
 *         view into `payload`. */
[[nodiscard]]
proven_err_t proven_h2_read_goaway(proven_mem_view_t payload, proven_u32 *last_stream_id, proven_u32 *code, proven_mem_view_t *debug);

// -----------------------------------------------------------------------------
// A header list as a message (RFC 9113, section 8)
// -----------------------------------------------------------------------------

/** @brief What a request's pseudo-header fields say. Views into the header list. */
typedef struct {
    proven_u8str_view_t method;        /**< `:method` */
    proven_u8str_view_t scheme;        /**< `:scheme`; empty for CONNECT */
    proven_u8str_view_t authority;     /**< `:authority`; empty when the client sent none */
    proven_u8str_view_t path;          /**< `:path`: path and query; empty for CONNECT */
    const proven_http_header_t *fields;    /**< the ordinary fields, which follow the pseudo-header fields */
    proven_size_t field_count;
} proven_h2_request_head_t;

/**
 * @brief Check a decoded header list as a request, and take it apart.
 *
 * Malformed - PROVEN_ERR_INVALID_FORMAT, a stream error of type PROTOCOL_ERROR, never to be
 * passed on - is: a field name with an uppercase letter, a space, a control or a non-ASCII
 * byte, or a colon anywhere but first; a value with NUL, CR or LF, or beginning or ending
 * with a space or tab; a pseudo-header field after an ordinary one, twice, or unknown;
 * `:status` in a request; `:method`, `:scheme` or `:path` missing, or `:path` empty for
 * `http` and `https` (a CONNECT request has `:method` and `:authority` and neither of the
 * other two); `connection`, `proxy-connection`, `keep-alive`, `transfer-encoding` or
 * `upgrade`; `te` with anything but `trailers`.
 *
 * These are the rules a message smuggled through a translation to HTTP/1.1 would break.
 */
[[nodiscard]]
proven_err_t proven_h2_request_head(const proven_http_header_t *headers, proven_size_t count, proven_h2_request_head_t *out);

/**
 * @brief Check a decoded header list as a response. As proven_h2_request_head, with `:status`
 *        - three digits - as the one pseudo-header field, required.
 * @param fields receives the ordinary fields; @param field_count how many.
 */
[[nodiscard]]
proven_err_t proven_h2_response_head(const proven_http_header_t *headers, proven_size_t count, proven_u16 *status,
                                     const proven_http_header_t **fields, proven_size_t *field_count);

/** @brief Check a decoded header list as trailers: ordinary fields under the same rules, and
 *         no pseudo-header field at all. */
[[nodiscard]]
proven_err_t proven_h2_trailers_check(const proven_http_header_t *headers, proven_size_t count);

#endif /* PROVEN_H2_H */
