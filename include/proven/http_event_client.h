#ifndef PROVEN_HTTP_EVENT_CLIENT_H
#define PROVEN_HTTP_EVENT_CLIENT_H

#include "proven/types.h"
#include "proven/error.h"
#include "proven/memory.h"
#include "proven/u8str.h"
#include "proven/allocator.h"
#include "proven/net.h"
#include "proven/http.h"
#include "proven/loop.h"
#include "proven/tls.h"
#include "proven/http_event.h"

/**
 * @file http_event_client.h
 * @brief An HTTP/1.1 client driven by events: many requests in flight on one thread.
 *
 * http_client.h sends a request and waits for the answer; a thousand requests at once would be
 * a thousand threads. This client sits on a `proven_loop_t` (loop.h): a request is started,
 * the call returns at once, and your functions are told what happened - the response head, a
 * piece of its body, the end.
 *
 * **It is deliberately narrow.** One request uses one connection, which is closed afterwards.
 * There are no redirects, no answers to authentication challenges, no cookies, no proxies and
 * no connection reuse here; http_client.h has all of those. And **names are not resolved**:
 * looking a name up can take seconds, and nothing on a loop may wait. Give the address to
 * connect to, having resolved it on another thread (proven_net_resolve from a job, the result
 * posted back with proven_loop_post). A URL whose host is an IP address needs none.
 *
 * The rules of the loop apply: your functions must not wait; the views they are given - the
 * response head, a piece of the body - are good only until they return; and everything here
 * is called on the loop's thread, and only there.
 *
 * Hosted only; `PROVEN_NO_NET` leaves it out.
 */

/** @brief The client. Opaque. */
typedef struct proven_http_event_client proven_http_event_client_t;

/** @brief One request and its response. Valid from proven_http_event_client_start until
 *         `on_done` returns. */
typedef struct proven_http_event_request proven_http_event_request_t;

typedef struct {
    /** The response head has arrived. Optional. Interim responses (100, 103) are passed over
     *  and not reported. */
    void (*on_response)(void *ctx, proven_http_event_request_t *request, const proven_http_response_t *head);
    /** A piece of the response body; `last` with the final one (which may be empty). A
     *  response with no body gets no call. Optional: without it the body is thrown away. */
    void (*on_body)(void *ctx, proven_http_event_request_t *request, proven_mem_view_t piece, bool last);
    /** Request-body bytes that were held back have gone: proven_http_event_request_write will
     *  take more. Optional. */
    void (*on_writable)(void *ctx, proven_http_event_request_t *request);
    /** The end, called exactly once for every request that was started: PROVEN_OK when the
     *  whole response arrived, otherwise why not. Required. Free what you attached here. */
    void (*on_done)(void *ctx, proven_http_event_request_t *request, proven_err_t why);
} proven_http_event_client_callbacks_t;

/** @brief How a client behaves. Zero-initialise it; every zero field has the default written
 *         beside it. */
typedef struct {
    proven_allocator_t alloc;              /**< not valid: the loop's allocator */
    const proven_tls_config_t *tls;        /**< for `https`. NULL: an `https` URL is refused, never fetched in the clear. Must outlive the client. */
    proven_size_t max_head_bytes;          /**< largest response head, and request head. 0: 16 KiB */
    proven_size_t max_headers;             /**< most header fields in a response. 0: 64 */
    proven_u64 max_body_bytes;             /**< largest response body - as sent, and with `decompress` as decoded too. 0: no limit - it is delivered in pieces and never held */
    proven_size_t max_buffered_output;     /**< request-body bytes held before writes are refused. 0: 64 KiB */
    proven_u32 connect_timeout_ms;         /**< to be connected, the TLS handshake included. 0: 10 s */
    proven_u32 response_timeout_ms;        /**< from the request being sent to the end of the response head. 0: 30 s */
    proven_u32 body_timeout_ms;            /**< between pieces of the response body. 0: 30 s */
    proven_u32 write_timeout_ms;           /**< output held with none of it accepted by the server. 0: 30 s */
    bool decompress;                       /**< ask for compressed responses (`Accept-Encoding: gzip`, unless the request has its own or a `Range`) and decode a body sent with `Content-Encoding: gzip` or `deflate`: `on_body` gets decoded pieces of at most 16 KiB, the head stays as the server sent it, and a damaged or unfinished stream ends the request with PROVEN_ERR_INVALID_FORMAT. A 206 and other codings are delivered as sent. Costs about 52 KiB while such a response is arriving. false: bodies arrive as sent */
} proven_http_event_client_config_t;

/** @brief One request. Zero-initialise it and set `url` and `on.on_done`. Everything it points
 *         to is copied or used before proven_http_event_client_start returns. */
typedef struct {
    proven_u8str_view_t method;            /**< empty: GET */
    proven_u8str_view_t url;               /**< absolute, `http` or `https`; its host goes into `Host` and, for `https`, is the name the certificate must be for */
    const proven_net_addr_t *address;      /**< where to connect. NULL only when the URL's host is an IP address. Its port is used as given. */
    const proven_http_header_t *headers;   /**< yours; `Host`, `Content-Length`, `Transfer-Encoding` and `Connection` are written for you and may not be here */
    proven_size_t header_count;
    proven_mem_view_t body;                /**< a body held in memory, sent whole */
    proven_u64 body_length;                /**< when `body` is empty: 0 for no body; a length for a body written afterwards with proven_http_event_request_write; PROVEN_HTTP_EVENT_LENGTH_UNKNOWN to write it chunked */
    proven_http_event_client_callbacks_t on;
    void *ctx;                             /**< passed to every callback */
} proven_http_event_request_options_t;

/** @return PROVEN_ERR_INVALID_ARG (no loop), PROVEN_ERR_NOMEM. */
[[nodiscard]]
proven_err_t proven_http_event_client_create(proven_loop_t *loop, const proven_http_event_client_config_t *config,
                                             proven_http_event_client_t **out);

/** @brief End every request still in flight - each gets `on_done` with PROVEN_ERR_RESET - and
 *         free the client. The loop is not stopped or freed. Null is accepted. */
void proven_http_event_client_destroy(proven_http_event_client_t *client);

/** @brief Requests in flight now. */
proven_size_t proven_http_event_client_requests(const proven_http_event_client_t *client);

/**
 * @brief Start a request. Returns at once; what happens to it arrives through the callbacks.
 *
 * When this returns an error the request was not started and no callback is or will be made.
 * When it returns PROVEN_OK, `on_done` will be called exactly once - never inside this call.
 *
 * @param out optional: receives the request, for the calls below.
 * @return PROVEN_ERR_INVALID_ARG for no `on_done`; a URL that is not absolute `http` or
 *         `https`, or that carries credentials; an `https` URL with no TLS configuration; a
 *         host that is a name with no `address` given; a header the client writes itself; a
 *         method that is not a token, or CONNECT. PROVEN_ERR_OUT_OF_BOUNDS when the request
 *         head does not fit `max_head_bytes`. PROVEN_ERR_NOMEM. And what beginning to connect
 *         returned, when it failed at once (PROVEN_ERR_UNREACHABLE, PROVEN_ERR_REFUSED).
 */
[[nodiscard]]
proven_err_t proven_http_event_client_start(proven_http_event_client_t *client, const proven_http_event_request_options_t *options,
                                            proven_http_event_request_t **out);

/**
 * @brief Offer request-body bytes, for a request started with a `body_length`. **It may take
 *        fewer than you offer** - as many as fit under `max_buffered_output`, possibly none.
 *        Keep the rest and offer it again when `on_writable` is called. It may be called
 *        before the connection is made: what is taken waits.
 * @return the number of bytes taken; PROVEN_ERR_INVALID_STATE when the request has no body to
 *         write, or it was ended; PROVEN_ERR_OUT_OF_BOUNDS for more than the length promised.
 */
[[nodiscard]]
proven_result_size_t proven_http_event_request_write(proven_http_event_request_t *request, proven_mem_view_t data);

/**
 * @brief Finish the request body.
 * @return PROVEN_ERR_INVALID_STATE when there was none to finish; PROVEN_ERR_INVALID_FORMAT
 *         when fewer bytes were written than promised - the request is then ended, and
 *         `on_done` is called with that error inside this call.
 */
[[nodiscard]]
proven_err_t proven_http_event_request_end(proven_http_event_request_t *request);

/** @brief Give up: the connection is closed at once and `on_done` is called with
 *         PROVEN_ERR_RESET, inside this call. */
void proven_http_event_request_abort(proven_http_event_request_t *request);

/** @brief Stop delivering the response body (and stop reading the connection) until
 *         proven_http_event_request_resume. */
void proven_http_event_request_pause(proven_http_event_request_t *request);
/** @brief Deliver again. Pieces that were waiting may arrive inside this call. */
void proven_http_event_request_resume(proven_http_event_request_t *request);

/** @brief Attach a pointer of yours to the request, and get it back. */
void proven_http_event_request_set_user(proven_http_event_request_t *request, void *user);
void *proven_http_event_request_user(const proven_http_event_request_t *request);

#endif /* PROVEN_HTTP_EVENT_CLIENT_H */
