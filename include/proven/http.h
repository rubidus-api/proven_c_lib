#ifndef PROVEN_HTTP_H
#define PROVEN_HTTP_H

#include "proven/types.h"
#include "proven/error.h"
#include "proven/memory.h"
#include "proven/u8str.h"
#include "proven/time.h"

/**
 * @file http.h
 * @brief HTTP/1.1 messages: parsing a head, framing a body, and writing both.
 *
 * This is a codec, not a client and not a server. It takes bytes you have and tells you what
 * they say; it writes bytes into memory you supply. It never reads, never writes to a socket,
 * never allocates, and keeps no state between calls except the small body-decoder struct you
 * own. That is what lets the same code sit under a blocking client, an event loop, a test with
 * no network, or a freestanding build.
 *
 * The parser is strict on purpose. An HTTP message is routinely read by more than one program
 * - a proxy in front, a server behind - and wherever two parsers read the same bytes
 * differently, one of them can be shown a request the other does not see. That is request
 * smuggling, and its raw material is leniency. So:
 *
 *   - a line ends in CRLF; a bare LF or a bare CR is refused;
 *   - there is no whitespace between a header name and its colon;
 *   - a header line that starts with a space (obsolete line folding) is refused;
 *   - `Content-Length` appears once, with digits only;
 *   - a message with both `Transfer-Encoding` and `Content-Length` is refused;
 *   - the only transfer coding is `chunked`.
 *
 * Each of these is something real software accepts and real attacks use.
 *
 * What is NOT here: HTTP/2 and HTTP/3; content codings (gzip); multipart bodies; cookies;
 * authentication. Trailers after a chunked body are checked and skipped, not handed back.
 *
 * Not part of the socket layer: `PROVEN_NO_NET` does not remove this header.
 */

// -----------------------------------------------------------------------------
// Heads
// -----------------------------------------------------------------------------

/** @brief One header field: two views into the bytes that were parsed. The value has no
 *         leading or trailing whitespace. */
typedef struct {
    proven_u8str_view_t name;
    proven_u8str_view_t value;
} proven_http_header_t;

typedef enum {
    PROVEN_HTTP_METHOD_OTHER = 0,   /**< a valid token this enum has no name for; see `method_text` */
    PROVEN_HTTP_GET,
    PROVEN_HTTP_HEAD,
    PROVEN_HTTP_POST,
    PROVEN_HTTP_PUT,
    PROVEN_HTTP_DELETE,
    PROVEN_HTTP_CONNECT,
    PROVEN_HTTP_OPTIONS,
    PROVEN_HTTP_TRACE,
    PROVEN_HTTP_PATCH
} proven_http_method_t;

/**
 * @brief A parsed request head. Every view points into the buffer that was parsed, so the
 *        buffer must outlive it and must not move.
 */
typedef struct {
    proven_http_method_t method;
    proven_u8str_view_t method_text;    /**< as written; methods are case-sensitive */
    proven_u8str_view_t target;         /**< as written: `/a?b`, `http://h/a`, `*`, or `host:port` */
    proven_u8 version_minor;            /**< 1 for HTTP/1.1, 0 for HTTP/1.0 */
    proven_http_header_t *headers;      /**< the array you supplied */
    proven_size_t header_count;
} proven_http_request_t;

/** @brief A parsed response head. Views point into the buffer that was parsed. */
typedef struct {
    proven_u16 status;                  /**< 100-999 */
    proven_u8str_view_t reason;         /**< may be empty; means nothing to a program */
    proven_u8 version_minor;
    proven_http_header_t *headers;
    proven_size_t header_count;
} proven_http_response_t;

/** @brief Largest head, in bytes, the parsers accept when you pass 0 for the limit. */
#define PROVEN_HTTP_DEFAULT_MAX_HEAD ((proven_size_t)16384)

/**
 * @brief Parse a request head from the front of `data`.
 *
 * `data` is what you have read so far. The head is the request line and the header lines, up
 * to and including the empty line that ends them; `*head_size` is its length, and whatever
 * follows it in `data` is the start of the body (or of the next request).
 *
 * Nothing is copied. `headers` is an array you supply for the header fields; `out->headers`
 * points at it.
 *
 * Empty lines before the request line are skipped, as RFC 9112 section 2.2 asks of a server:
 * clients have long sent a stray CRLF after a body. They are counted in `*head_size`.
 *
 * @param max_head_bytes the largest head to accept; 0 means PROVEN_HTTP_DEFAULT_MAX_HEAD.
 *
 * @return PROVEN_ERR_NEED_MORE when `data` is a valid beginning that does not yet contain the
 *         whole head: read more and call again with the longer buffer.
 *         PROVEN_ERR_OUT_OF_BOUNDS when the head is larger than `max_head_bytes` - decided as
 *         soon as that many bytes hold no end, so an endless header costs the limit and no
 *         more - or has more fields than `header_cap` (answer 431).
 *         PROVEN_ERR_INVALID_FORMAT when the bytes are not an HTTP/1 request head (answer 400).
 *         PROVEN_ERR_UNSUPPORTED when the version is not HTTP/1.0 or HTTP/1.1 (answer 505).
 *         On any error `*out` and `*head_size` are not to be used.
 */
[[nodiscard]]
proven_err_t proven_http_parse_request(proven_mem_view_t data, proven_http_header_t *headers, proven_size_t header_cap,
                                       proven_size_t max_head_bytes, proven_http_request_t *out, proven_size_t *head_size);

/** @brief Parse a response head from the front of `data`. As proven_http_parse_request. */
[[nodiscard]]
proven_err_t proven_http_parse_response(proven_mem_view_t data, proven_http_header_t *headers, proven_size_t header_cap,
                                        proven_size_t max_head_bytes, proven_http_response_t *out, proven_size_t *head_size);

/**
 * @brief Find the first header field named `name` (ASCII case-insensitive).
 * @return true and `*value` when present; false otherwise.
 */
[[nodiscard]]
bool proven_http_header_find(const proven_http_header_t *headers, proven_size_t count,
                             proven_u8str_view_t name, proven_u8str_view_t *value);

/** @brief How many fields are named `name`. A message may repeat a field; some, like
 *         `Set-Cookie`, routinely do. */
[[nodiscard]]
proven_size_t proven_http_header_count(const proven_http_header_t *headers, proven_size_t count, proven_u8str_view_t name);

/**
 * @brief Whether any field named `name` lists `token` (ASCII case-insensitive) among its
 *        comma-separated values: `Connection: keep-alive, Upgrade` lists `upgrade`.
 */
[[nodiscard]]
bool proven_http_header_has_token(const proven_http_header_t *headers, proven_size_t count,
                                  proven_u8str_view_t name, proven_u8str_view_t token);

/** @brief The method a token names; PROVEN_HTTP_METHOD_OTHER for any other valid token.
 *         Case-sensitive: `get` is not `GET`. */
[[nodiscard]]
proven_http_method_t proven_http_method_from_text(proven_u8str_view_t text);

/** @brief The token for a method; an empty view for PROVEN_HTTP_METHOD_OTHER. */
[[nodiscard]]
proven_u8str_view_t proven_http_method_text(proven_http_method_t method);

/** @brief The standard reason phrase for a status code ("Not Found"); "Unknown" for one this
 *         library has no phrase for. */
[[nodiscard]]
proven_u8str_view_t proven_http_reason_phrase(proven_u16 status);

/**
 * @brief Whether the connection may carry another request after this one.
 *
 * HTTP/1.1 persists unless `Connection: close`; HTTP/1.0 closes unless `Connection: keep-alive`.
 */
[[nodiscard]]
bool proven_http_request_keep_alive(const proven_http_request_t *request);

/** @brief The same question for a response. A response whose body runs until the connection
 *         closes can never be followed by another; ask proven_http_response_framing as well. */
[[nodiscard]]
bool proven_http_response_keep_alive(const proven_http_response_t *response);

// -----------------------------------------------------------------------------
// Bodies
// -----------------------------------------------------------------------------

typedef enum {
    PROVEN_HTTP_BODY_NONE = 0,      /**< no body follows the head */
    PROVEN_HTTP_BODY_LENGTH,        /**< exactly `length` bytes follow */
    PROVEN_HTTP_BODY_CHUNKED,       /**< chunks follow, ended by a zero-size chunk */
    PROVEN_HTTP_BODY_UNTIL_CLOSE    /**< responses only: the body is everything until the peer closes */
} proven_http_body_kind_t;

/** @brief How the body that follows a head is delimited. */
typedef struct {
    proven_http_body_kind_t kind;
    proven_u64 length;              /**< for PROVEN_HTTP_BODY_LENGTH */
} proven_http_framing_t;

/**
 * @brief Decide how a request's body is delimited (RFC 9112 section 6.3).
 *
 * `Transfer-Encoding: chunked` means chunked; `Content-Length` means that many bytes; neither
 * means no body. A request never runs until close.
 *
 * @return PROVEN_ERR_INVALID_FORMAT (answer 400) for both `Transfer-Encoding` and
 *         `Content-Length`; for more than one `Content-Length` field, or one that is not a
 *         plain decimal number or does not fit 64 bits; for `Transfer-Encoding` on an HTTP/1.0
 *         request. PROVEN_ERR_UNSUPPORTED (answer 501) for a transfer coding other than
 *         `chunked`.
 */
[[nodiscard]]
proven_err_t proven_http_request_framing(const proven_http_request_t *request, proven_http_framing_t *out);

/**
 * @brief Decide how a response's body is delimited.
 *
 * @param request_method the method of the request this answers: a response to HEAD has no
 *        body whatever its headers say, and neither has a 2xx response to CONNECT.
 *
 * Responses with status 1xx, 204 and 304 have no body. Otherwise chunked, then
 * `Content-Length`, and with neither the body is everything until the peer closes.
 *
 * @return as proven_http_request_framing.
 */
[[nodiscard]]
proven_err_t proven_http_response_framing(const proven_http_response_t *response, proven_http_method_t request_method,
                                          proven_http_framing_t *out);

/**
 * @brief The state of a body being decoded. Caller-owned; start it with proven_http_body_init.
 *
 * Its fields are exposed only so it can live on the stack. Do not read or write them.
 */
typedef struct {
    proven_u8 kind;
    proven_u8 state;
    proven_u8 size_digits;
    bool done;
    proven_u64 remaining;       /* bytes left in the body, or in the current chunk */
    proven_u64 total;           /* payload bytes delivered so far */
    proven_u64 max_body;
    proven_u32 line_bytes;      /* bytes of chunk-size line or trailer section seen */
} proven_http_body_t;

/** @brief Largest chunk-size line (size, extensions, CRLF) the decoder accepts. */
#define PROVEN_HTTP_MAX_CHUNK_LINE ((proven_u32)256)
/** @brief Largest trailer section after a chunked body the decoder accepts. */
#define PROVEN_HTTP_MAX_TRAILER_BYTES ((proven_u32)8192)

/**
 * @brief Begin decoding a body.
 * @param max_body_bytes the largest body to accept. There is no default: say how much a peer
 *        may send you. A `Content-Length` above it is refused at once.
 * @return PROVEN_ERR_OUT_OF_BOUNDS when `framing` already announces more than `max_body_bytes`
 *         (answer 413); PROVEN_ERR_INVALID_ARG for a framing kind that does not exist.
 */
[[nodiscard]]
proven_err_t proven_http_body_init(proven_http_body_t *body, proven_http_framing_t framing, proven_u64 max_body_bytes);

/**
 * @brief Feed the decoder bytes that followed the head, and get body bytes back.
 *
 * Consumes some prefix of `in` - `*consumed` says how much - and sets `*payload` to the body
 * bytes that prefix contained, as a view INTO `in` (nothing is copied; for a chunked body the
 * chunk framing is simply stepped over). Call it again with what is left of `in`; when it has
 * consumed everything and `*done` is still false, read more.
 *
 * When `*done` becomes true the body is complete, and bytes of `in` beyond `*consumed` belong
 * to the next message. A body that runs until close is never done: when the peer closes, ask
 * proven_http_body_end.
 *
 * @return PROVEN_ERR_INVALID_FORMAT for malformed chunk framing or trailers;
 *         PROVEN_ERR_OUT_OF_BOUNDS when the body grows past the limit given to
 *         proven_http_body_init, or a chunk-size line or the trailers pass their bounds;
 *         PROVEN_ERR_INVALID_STATE when called again after an error. After any error the
 *         connection cannot be reused: where the next message starts is no longer known.
 */
[[nodiscard]]
proven_err_t proven_http_body_feed(proven_http_body_t *body, proven_mem_view_t in,
                                   proven_size_t *consumed, proven_mem_view_t *payload, bool *done);

/**
 * @brief The peer closed the connection: was the body complete?
 * @return PROVEN_OK when it was (always, for a body that runs until close);
 *         PROVEN_ERR_NEED_MORE when the connection ended in the middle of it - the message is
 *         truncated and must not be treated as whole.
 */
[[nodiscard]]
proven_err_t proven_http_body_end(const proven_http_body_t *body);

/** @brief Body bytes delivered so far. */
[[nodiscard]]
proven_u64 proven_http_body_received(const proven_http_body_t *body);

// -----------------------------------------------------------------------------
// Writing
// -----------------------------------------------------------------------------
//
// Each writer APPENDS to `out` at `*len` and advances `*len`. On any failure `*len` and the
// bytes before it are unchanged, so a message is built by a run of calls and one check of the
// last result is not enough: check each, or the first failure leaves a hole.

/**
 * @brief Append a request line: `GET /path HTTP/1.1` and CRLF.
 * @return PROVEN_ERR_INVALID_ARG when `method` is not a token or `target` is empty or holds a
 *         space or a control character - either would let a caller-supplied string end the
 *         line and start another. PROVEN_ERR_OUT_OF_BOUNDS when it does not fit.
 */
[[nodiscard]]
proven_err_t proven_http_write_request_line(proven_mem_mut_t out, proven_size_t *len,
                                            proven_u8str_view_t method, proven_u8str_view_t target);

/**
 * @brief Append a status line: `HTTP/1.1 404 Not Found` and CRLF.
 * @param reason the phrase to send; an empty view sends the standard one for `status`.
 * @return PROVEN_ERR_INVALID_ARG for a status outside 100-999 or a reason with a control
 *         character in it.
 */
[[nodiscard]]
proven_err_t proven_http_write_status_line(proven_mem_mut_t out, proven_size_t *len,
                                           proven_u16 status, proven_u8str_view_t reason);

/**
 * @brief Append one header field: `Name: value` and CRLF.
 *
 * @return PROVEN_ERR_INVALID_ARG when `name` is not a token, or `value` contains CR, LF, NUL or
 *         another control character (a tab is allowed), or starts or ends with whitespace.
 *         This is the check that stops header injection: a value taken from a request and
 *         written into a response cannot end the field and add one of its own.
 */
[[nodiscard]]
proven_err_t proven_http_write_header(proven_mem_mut_t out, proven_size_t *len,
                                      proven_u8str_view_t name, proven_u8str_view_t value);

/** @brief Append a header field whose value is a decimal number: `Content-Length: 1234`. */
[[nodiscard]]
proven_err_t proven_http_write_header_u64(proven_mem_mut_t out, proven_size_t *len,
                                          proven_u8str_view_t name, proven_u64 value);

/** @brief Append the empty line that ends a head. */
[[nodiscard]]
proven_err_t proven_http_write_head_end(proven_mem_mut_t out, proven_size_t *len);

/**
 * @brief Append the line that starts a chunk of `size` bytes. Follow it with exactly `size`
 *        bytes of data and then proven_http_write_chunk_end.
 * @return PROVEN_ERR_INVALID_ARG for a size of 0: that is the last chunk, which
 *         proven_http_write_last_chunk writes.
 */
[[nodiscard]]
proven_err_t proven_http_write_chunk_begin(proven_mem_mut_t out, proven_size_t *len, proven_u64 size);

/** @brief Append the CRLF that follows a chunk's data. */
[[nodiscard]]
proven_err_t proven_http_write_chunk_end(proven_mem_mut_t out, proven_size_t *len);

/** @brief Append the zero-size chunk and empty trailer section that end a chunked body. */
[[nodiscard]]
proven_err_t proven_http_write_last_chunk(proven_mem_mut_t out, proven_size_t *len);

// -----------------------------------------------------------------------------
// Ranges
// -----------------------------------------------------------------------------

/** @brief Pass as `last` to ask for everything from `first` to the end. */
#define PROVEN_HTTP_RANGE_TO_END UINT64_MAX

/**
 * @brief Append a `Range` header asking for bytes `first` through `last`, inclusive:
 *        `Range: bytes=100-199`, or `Range: bytes=100-` with PROVEN_HTTP_RANGE_TO_END.
 * @return PROVEN_ERR_INVALID_ARG when `last` is before `first`.
 */
[[nodiscard]]
proven_err_t proven_http_write_range(proven_mem_mut_t out, proven_size_t *len, proven_u64 first, proven_u64 last);

/**
 * @brief Append a `Content-Range` header for a 206 response: `Content-Range: bytes 100-199/1000`.
 * @return PROVEN_ERR_INVALID_ARG unless `first <= last < total`.
 */
[[nodiscard]]
proven_err_t proven_http_write_content_range(proven_mem_mut_t out, proven_size_t *len,
                                             proven_u64 first, proven_u64 last, proven_u64 total);

/**
 * @brief Read the value of a request's `Range` header against a resource of `size` bytes.
 *
 * Takes one byte range in any of its three spellings - `bytes=100-199`, `bytes=100-` (to the
 * end) and `bytes=-500` (the last 500) - and gives back the first and last byte to send,
 * clamped to the resource.
 *
 * @return PROVEN_ERR_OUT_OF_BOUNDS when the range starts at or past the end, or asks for the
 *         last zero bytes, or `size` is 0 (answer 416);
 *         PROVEN_ERR_UNSUPPORTED for a unit other than `bytes`, or more than one range - a
 *         server may ignore `Range` and send the whole resource;
 *         PROVEN_ERR_INVALID_FORMAT for anything else (ignore the header, as RFC 9110 says).
 */
[[nodiscard]]
proven_err_t proven_http_range_parse(proven_u8str_view_t value, proven_u64 size, proven_u64 *first, proven_u64 *last);

/**
 * @brief Read the value of a response's `Content-Range` header: `bytes 100-199/1000`, or
 *        `bytes 100-199/` followed by `*` when the total is not known.
 * @param total receives the complete length; `*has_total` is false when it was `*`.
 * @return PROVEN_ERR_INVALID_FORMAT when it is not that, or the numbers contradict each other.
 */
[[nodiscard]]
proven_err_t proven_http_content_range_parse(proven_u8str_view_t value, proven_u64 *first, proven_u64 *last,
                                             proven_u64 *total, bool *has_total);

// -----------------------------------------------------------------------------
// Multipart form data
// -----------------------------------------------------------------------------

/** @brief Bytes in a boundary made by proven_http_multipart_boundary, without a NUL. */
#define PROVEN_HTTP_BOUNDARY_SIZE ((proven_size_t)40)

/**
 * @brief Make a boundary for a `multipart/form-data` body from 16 random bytes you supply.
 *
 * A boundary must not occur inside any part. 128 random bits make that as unlikely as a
 * collision of two UUIDs; draw them from proven_random_bytes or a seeded generator. The result
 * is 40 characters, all of which are safe in a header without quoting.
 */
void proven_http_multipart_boundary(const proven_byte_t random[16], proven_byte_t out[PROVEN_HTTP_BOUNDARY_SIZE]);

/** @brief Append the header that announces the body: `Content-Type: multipart/form-data;
 *         boundary=...`. PROVEN_ERR_INVALID_ARG for a boundary that is empty, longer than 70
 *         characters, or holds a character that would need quoting. */
[[nodiscard]]
proven_err_t proven_http_multipart_write_content_type(proven_mem_mut_t out, proven_size_t *len, proven_u8str_view_t boundary);

/**
 * @brief Append the opening of one part: the boundary line, its `Content-Disposition`, an
 *        optional `Content-Type`, and the empty line. The part's bytes follow, written by you,
 *        and then proven_http_multipart_write_part_end.
 *
 * @param name the form field's name.
 * @param filename empty for an ordinary field; for a file, the name to report.
 * @param content_type empty to omit the header (a field's default is text/plain).
 *
 * A double quote, CR or LF in `name` or `filename` is written percent-encoded, as browsers do,
 * so neither can end the quoted string or the header.
 *
 * @return PROVEN_ERR_INVALID_ARG for an empty name, a control character in `content_type`, or a
 *         bad boundary.
 */
[[nodiscard]]
proven_err_t proven_http_multipart_write_part(proven_mem_mut_t out, proven_size_t *len, proven_u8str_view_t boundary,
                                              proven_u8str_view_t name, proven_u8str_view_t filename,
                                              proven_u8str_view_t content_type);

/** @brief Append the CRLF that follows a part's bytes. */
[[nodiscard]]
proven_err_t proven_http_multipart_write_part_end(proven_mem_mut_t out, proven_size_t *len);

/** @brief Append the closing boundary that ends the whole body. */
[[nodiscard]]
proven_err_t proven_http_multipart_write_end(proven_mem_mut_t out, proven_size_t *len, proven_u8str_view_t boundary);

// -----------------------------------------------------------------------------
// Dates
// -----------------------------------------------------------------------------

/** @brief Bytes in an HTTP date, without a NUL: `Sun, 06 Nov 1994 08:49:37 GMT`. */
#define PROVEN_HTTP_DATE_SIZE ((proven_size_t)29)

/**
 * @brief Write a wall-clock time as an HTTP date (IMF-fixdate), for `Date`, `Last-Modified`
 *        and `Expires`. Always 29 bytes, always GMT, never localized.
 * @param wall_ns a proven_time_now reading - NOT the monotonic clock, which has no date.
 * @return PROVEN_ERR_INVALID_ARG for a time before 1970.
 */
[[nodiscard]]
proven_err_t proven_http_date_format(proven_time_t wall_ns, proven_byte_t out[PROVEN_HTTP_DATE_SIZE]);

/**
 * @brief Read an HTTP date in any of the three forms a recipient must accept: IMF-fixdate, the
 *        obsolete RFC 850 form (`Sunday, 06-Nov-94 08:49:37 GMT`) and asctime
 *        (`Sun Nov  6 08:49:37 1994`).
 *
 * A two-digit year is read as RFC 9110 says: as the most recent year with those last two
 * digits that is not more than 50 years in the future of `now_ns`.
 *
 * @param now_ns the current wall-clock time, used only for that two-digit rule.
 * @return PROVEN_ERR_INVALID_FORMAT for anything else, including a date that does not exist
 *         (31 Feb), a year before 1970, and a weekday that does not match the date.
 *         PROVEN_ERR_OVERFLOW for a valid date later than proven_time_t can hold - its
 *         nanoseconds end in April 2262. Servers send such dates on purpose (`Expires` in the
 *         year 9999 means "never"): treat this answer as "far in the future", not as an error
 *         in the message.
 */
[[nodiscard]]
proven_err_t proven_http_date_parse(proven_u8str_view_t text, proven_time_t now_ns, proven_time_t *out_wall_ns);

#endif /* PROVEN_HTTP_H */
