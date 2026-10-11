#ifndef PROVEN_HTTP_EVENT_H
#define PROVEN_HTTP_EVENT_H

#include "proven/types.h"
#include "proven/error.h"
#include "proven/memory.h"
#include "proven/u8str.h"
#include "proven/allocator.h"
#include "proven/net.h"
#include "proven/http.h"
#include "proven/loop.h"
#include "proven/tls.h"

/**
 * @file http_event.h
 * @brief An HTTP/1.1 server driven by events, for many connections on one thread.
 *
 * http_server.h gives a handler a request and lets it read and write with calls that wait.
 * That is the simplest thing to write, and each request being handled occupies a thread - or
 * the whole server, if there is one thread. This unit is the other trade: **nothing here
 * waits.** The server sits on a `proven_loop_t` (loop.h) and tells your functions what
 * happened - a request arrived, a piece of its body, room to write more - and they return at
 * once. One thread can then carry very many connections, because a connection that is doing
 * nothing costs a small struct and no buffer.
 *
 * What you take on in exchange:
 *
 *   - **Your functions must not wait.** A handler that needs a file, a database or a
 *     computation hands it to another thread and answers when the result is posted back to
 *     the loop (proven_loop_post). A response may be sent at any later time; the stream stays
 *     valid until `on_done`.
 *   - **Writing can be refused.** proven_http_stream_write takes what fits under the output
 *     limit and says how much; you offer the rest when `on_writable` is called. That is
 *     backpressure: a slow client makes your producer slow, instead of making the server's
 *     memory grow.
 *   - **Views are short-lived.** The request head given to `on_request`, and the piece given
 *     to `on_body`, are good only until that function returns. Copy what you keep.
 *
 * Everything here is called on the loop's thread, and only there.
 *
 * Hosted only; `PROVEN_NO_NET` leaves it out.
 */

/** @brief One request and its response on a connection. Valid from `on_request` until
 *         `on_done` returns. */
typedef struct proven_http_stream proven_http_stream_t;

/** @brief The server. Opaque. */
typedef struct proven_http_event_server proven_http_event_server_t;

typedef struct {
    /** A request head has arrived and passed the server's checks. Required. Respond here, or
     *  remember `stream` and respond later from the loop's thread. */
    void (*on_request)(void *ctx, proven_http_stream_t *stream, const proven_http_request_t *head);
    /** A piece of the request body; `last` with the final one (which may be empty). Optional:
     *  without it a body is read and thrown away. */
    void (*on_body)(void *ctx, proven_http_stream_t *stream, proven_mem_view_t piece, bool last);
    /** Output that was held back has gone: proven_http_stream_write will take more. Optional. */
    void (*on_writable)(void *ctx, proven_http_stream_t *stream);
    /** The exchange is over, called exactly once for every `on_request`: PROVEN_OK when the
     *  whole response was handed to the connection, otherwise why it could not be (the client
     *  left, a timeout, a protocol error). Free what you attached to the stream here. Optional. */
    void (*on_done)(void *ctx, proven_http_stream_t *stream, proven_err_t why);
} proven_http_event_callbacks_t;

/**
 * @brief How a server behaves. Zero-initialise it, set `on.on_request`, and set what you
 *        need: every zero field has the default written beside it.
 */
typedef struct {
    proven_allocator_t alloc;            /**< not valid: the loop's allocator */
    proven_http_event_callbacks_t on;
    void *ctx;                           /**< passed to every callback */
    proven_size_t max_connections;       /**< open at once; further ones wait in the backlog. 0: 10,000 */
    proven_size_t max_head_bytes;        /**< largest request head. 0: 16 KiB */
    proven_size_t max_headers;           /**< most header fields in a request. 0: 64 */
    proven_u64 max_body_bytes;           /**< largest request body. 0: 1 MiB */
    proven_size_t max_buffered_output;   /**< response bytes held for one connection before writes are refused. 0: 64 KiB */
    proven_u32 head_timeout_ms;          /**< from a connection's (or a request's) first byte to the end of the head; with TLS it covers the handshake too. 0: 10 s */
    proven_u32 body_timeout_ms;          /**< between pieces of a request body. 0: 30 s */
    proven_u32 write_timeout_ms;         /**< output held with none of it accepted by the client. 0: 30 s */
    proven_u32 idle_timeout_ms;          /**< an open connection with no request on it. 0: 60 s */
    const proven_tls_config_t *tls;      /**< NULL: plain HTTP. Otherwise every connection is TLS; must outlive the server. */
    int compress_level;                  /**< for responses that ask to be compressed (proven_http_stream_compress): 1 (fastest) to 9 (smallest); -1 stores without compressing. 0: 6 */
    int compress_window_bits;            /**< and how far back the compressor looks, 9 to 15: each response being streamed compressed holds from about 11 KiB (9) to about 325 KiB (15) until it ends. 0: 15 */
} proven_http_event_server_config_t;

/**
 * @brief Make a server on `loop`. It serves once proven_http_event_server_listen has been
 *        called and the loop runs.
 * @return PROVEN_ERR_INVALID_ARG (no loop, no `on_request`, a TLS config with no certificate,
 *         a compression level or window outside its range),
 *         PROVEN_ERR_NOMEM.
 */
[[nodiscard]]
proven_err_t proven_http_event_server_create(proven_loop_t *loop, const proven_http_event_server_config_t *config,
                                             proven_http_event_server_t **out);

/** @brief Listen at `at` (up to four addresses per server). `bound` (optional) receives the
 *         address in use, with the port the system chose when `at` asked for port 0. */
[[nodiscard]]
proven_err_t proven_http_event_server_listen(proven_http_event_server_t *server, proven_net_addr_t at, proven_net_addr_t *bound);

/**
 * @brief Close every connection and the listeners, and free the server. Exchanges still in
 *        progress get `on_done` with PROVEN_ERR_RESET. The loop is not stopped or freed.
 *        Null is accepted.
 */
void proven_http_event_server_destroy(proven_http_event_server_t *server);

/**
 * @brief Take a connection that was accepted elsewhere: it becomes one of the server's, as if
 *        the server had accepted it (with TLS, the handshake begins).
 *
 * This is how several loops share one listening port: a thread accepts, and hands each
 * connection to one of several servers, each on a loop of its own. **Like everything else
 * here it must be called on the server's loop thread**, so the accepting thread posts it
 * (proven_loop_post) - and a `proven_net_conn_t` is not to be copied while open, so what is
 * posted carries the connection in memory of its own, not a copy on a stack.
 *
 * On PROVEN_OK the server owns the connection and `*conn` is no longer open. On any error it
 * is still yours, to close.
 *
 * @return PROVEN_ERR_INVALID_ARG; PROVEN_ERR_INVALID_STATE when `conn` is not open, or after
 *         proven_http_event_server_stop_listening; PROVEN_ERR_BUSY at `max_connections`;
 *         PROVEN_ERR_NOMEM; or what watching the socket returned.
 */
[[nodiscard]]
proven_err_t proven_http_event_server_adopt(proven_http_event_server_t *server, proven_net_conn_t *conn);

/** @brief Connections open now. */
proven_size_t proven_http_event_server_connections(const proven_http_event_server_t *server);

/** @brief Stop accepting - and adopting; connections that are open go on. For shutting down
 *         gracefully. (proven_http_event_server_listen afterwards takes connections again.) */
void proven_http_event_server_stop_listening(proven_http_event_server_t *server);

/**
 * @brief Ask that the response of this stream be compressed. Call it before the response
 *        begins - in `on_request` or later; afterwards it does nothing.
 *
 * Nothing is compressed unless asked for, response by response, because whether it is safe is
 * a property of the response: **do not ask for one that carries a secret together with text
 * the client chose**. The length of a compressed body tells an observer of the connection how
 * much the two have in common, TLS or not (the attack known as BREACH).
 *
 * The response then carries `Vary: Accept-Encoding` (unless your `Vary` already names it) and,
 * when the request's `Accept-Encoding` allows gzip, is sent as `Content-Encoding: gzip`. It
 * is sent as it is when the client does not accept gzip; when the status is 204, 206 or 304;
 * when your headers have a `Content-Encoding` or a `Content-Range` of their own; when memory
 * for the compressor cannot be had; and, with proven_http_stream_respond, when the body is
 * under 256 bytes or does not get smaller.
 *
 * With proven_http_stream_begin the body leaves chunked whatever length was given (the length
 * is still what you must write). proven_http_stream_write takes input while the compressed
 * output held for the connection is under its limit, and **what it takes may wait inside the
 * compressor** until more is written or the response ends: do not ask for a response that the
 * client must see piece by piece, such as an event stream. proven_http_stream_buffered counts
 * compressed bytes.
 */
void proven_http_stream_compress(proven_http_stream_t *stream);

/** @brief The length to give proven_http_stream_begin when it is not known in advance. */
#define PROVEN_HTTP_EVENT_LENGTH_UNKNOWN UINT64_MAX

/**
 * @brief Send a whole response: status, your headers, a body held in memory.
 *
 * The body is copied, whatever its size - for a large or generated body use
 * proven_http_stream_begin and proven_http_stream_write, which respect the output limit.
 * `Content-Length`, `Date` and `Connection` are written for you and may not be among
 * `headers`.
 *
 * @return PROVEN_ERR_INVALID_STATE when a response was already begun or the stream is over;
 *         PROVEN_ERR_INVALID_ARG for a status outside 200-999 or a header the server writes
 *         itself; PROVEN_ERR_OUT_OF_BOUNDS when the head does not fit `max_head_bytes`;
 *         PROVEN_ERR_NOMEM.
 */
[[nodiscard]]
proven_err_t proven_http_stream_respond(proven_http_stream_t *stream, proven_u16 status,
                                        const proven_http_header_t *headers, proven_size_t header_count,
                                        proven_mem_view_t body);

/**
 * @brief Begin a response whose body will follow in pieces.
 * @param content_length the body's size, or PROVEN_HTTP_EVENT_LENGTH_UNKNOWN to send it chunked.
 * @return as proven_http_stream_respond.
 */
[[nodiscard]]
proven_err_t proven_http_stream_begin(proven_http_stream_t *stream, proven_u16 status,
                                      const proven_http_header_t *headers, proven_size_t header_count,
                                      proven_u64 content_length);

/**
 * @brief Offer body bytes. **It may take fewer than you offer** - as many as fit under the
 *        output limit, possibly none. Keep the rest and offer it again when `on_writable` is
 *        called.
 * @return the number of bytes taken; PROVEN_ERR_INVALID_STATE before proven_http_stream_begin,
 *         after proven_http_stream_end, or when the stream is over; PROVEN_ERR_OUT_OF_BOUNDS
 *         for more than the Content-Length that was promised.
 */
[[nodiscard]]
proven_result_size_t proven_http_stream_write(proven_http_stream_t *stream, proven_mem_view_t data);

/**
 * @brief Finish the response. `on_done` follows when everything has been handed to the
 *        connection - possibly inside this call.
 * @return PROVEN_ERR_INVALID_STATE when no response was begun or it was already ended;
 *         PROVEN_ERR_INVALID_FORMAT when fewer bytes were written than the Content-Length
 *         promised (the connection is then closed: the client must not take it for whole).
 */
[[nodiscard]]
proven_err_t proven_http_stream_end(proven_http_stream_t *stream);

/** @brief Give up on this exchange: the connection is closed at once and `on_done` is called
 *         with PROVEN_ERR_RESET. */
void proven_http_stream_abort(proven_http_stream_t *stream);

/** @brief Stop delivering the request body (and stop reading the connection) until
 *         proven_http_stream_resume: backpressure in the other direction. */
void proven_http_stream_pause(proven_http_stream_t *stream);
/** @brief Deliver again. Pieces that were waiting may arrive inside this call. */
void proven_http_stream_resume(proven_http_stream_t *stream);

/** @brief Attach a pointer of yours to the stream, and get it back. */
void proven_http_stream_set_user(proven_http_stream_t *stream, void *user);
void *proven_http_stream_user(const proven_http_stream_t *stream);

/** @brief The client's address. */
proven_net_addr_t proven_http_stream_peer(const proven_http_stream_t *stream);

/** @brief Response bytes held for this connection and not yet accepted by the client. */
proven_size_t proven_http_stream_buffered(const proven_http_stream_t *stream);

#endif /* PROVEN_HTTP_EVENT_H */
