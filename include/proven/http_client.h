#ifndef PROVEN_HTTP_CLIENT_H
#define PROVEN_HTTP_CLIENT_H

#include "proven/types.h"
#include "proven/error.h"
#include "proven/memory.h"
#include "proven/u8str.h"
#include "proven/allocator.h"
#include "proven/stream.h"
#include "proven/net.h"
#include "proven/http.h"
#include "proven/http_cookie.h"

/**
 * @file http_client.h
 * @brief An HTTP/1.1 client: one request, one response, over connections it manages.
 *
 * The client does what stands between "I have a URL" and "I have the bytes": it resolves the
 * name, connects (directly or through a proxy), writes the request, reads and checks the
 * response head, follows redirects, answers an authentication challenge, keeps cookies if you
 * give it a jar, and hands the body back as a stream you read at your own pace. Connections
 * that can be reused are kept and reused.
 *
 * Three things it will not do behind your back:
 *
 *   - **Wait for ever.** Connecting has a time limit, and so has every read and write.
 *   - **Hold a whole body in memory.** The body is read by you, in pieces, into your buffer;
 *     proven_http_client_read_all exists for when you do want all of it, and takes a limit.
 *   - **Allocate from anywhere but the allocator you gave it.**
 *
 * And one thing it cannot do yet: **HTTPS.** This library has no TLS. A request for an
 * `https` URL fails with PROVEN_ERR_UNSUPPORTED unless you supply a `tls_wrap` function that
 * turns a connected transport into an encrypted one - the seam where TLS will attach. Until
 * then everything this client sends, passwords and cookies included, crosses the network in
 * the clear.
 *
 * A client is not safe to use from several threads at once; give each thread its own.
 *
 * Hosted only; `PROVEN_NO_NET` leaves it out.
 */

/**
 * @brief Turn a connected plaintext transport into an encrypted one for `host`.
 *
 * Called for every new connection to an `https` URL, after the TCP connection (and any proxy
 * tunnel) is established. On success `*out` must be a transport whose close also closes
 * `plain`; on failure the function must leave `plain` open - the client closes it.
 * Return PROVEN_ERR_UNTRUSTED when the peer's identity cannot be verified.
 */
typedef proven_err_t (*proven_http_tls_wrap_fn)(void *ctx, proven_transport_t plain, proven_u8str_view_t host,
                                                proven_net_deadline_t until, proven_transport_t *out);

/**
 * @brief How a client behaves. Zero-initialise it, set `alloc`, and set what you need: every
 *        zero field has the default written beside it. The strings are copied by
 *        proven_http_client_create and need not outlive it.
 */
typedef struct {
    proven_allocator_t alloc;               /**< required */
    proven_u32 connect_timeout_ms;          /**< resolving aside, the limit for connecting; 0: 30 s */
    proven_u32 io_timeout_ms;               /**< the limit for each read and each write; 0: 30 s */
    proven_size_t max_head_bytes;           /**< largest response head accepted; 0: 16 KiB */
    proven_u32 max_redirects;               /**< redirects followed per request; 0: none are followed */
    proven_size_t max_idle_connections;     /**< connections kept for reuse; 0: every connection is closed */
    proven_u8str_view_t proxy;              /**< `http://[user:pw@]host:port` or `socks5://[user:pw@]host:port`; empty: none */
    proven_u8str_view_t username;           /**< credentials offered when a server answers 401; empty: none */
    proven_u8str_view_t password;
    proven_u8str_view_t user_agent;         /**< the `User-Agent` to send; empty: none is sent */
    proven_http_cookie_jar_t *cookies;      /**< a jar to read and fill; NULL: cookies are ignored. Must outlive the client. */
    proven_http_tls_wrap_fn tls_wrap;       /**< NULL: an `https` URL is PROVEN_ERR_UNSUPPORTED */
    void *tls_ctx;
} proven_http_client_config_t;

/** @brief A client. Opaque; made by proven_http_client_create. */
typedef struct proven_http_client proven_http_client_t;

/**
 * @brief Make a client.
 * @return PROVEN_ERR_INVALID_ARG for an invalid allocator, a proxy URL that is not `http` or
 *         `socks5` with a host, or a user name or password with a control character;
 *         PROVEN_ERR_NOMEM.
 */
[[nodiscard]]
proven_err_t proven_http_client_create(const proven_http_client_config_t *config, proven_http_client_t **out);

/** @brief Close every idle connection and free the client. Finish every response first. */
void proven_http_client_destroy(proven_http_client_t *client);

/** @brief One request. Zero-initialise it and set `url`. */
typedef struct {
    proven_u8str_view_t method;             /**< empty: `GET` */
    proven_u8str_view_t url;                /**< absolute: `http://host[:port]/path?query` */
    const proven_http_header_t *headers;    /**< extra header fields to send */
    proven_size_t header_count;
    proven_mem_view_t body;                 /**< a body held in memory: sent with `Content-Length` */
    proven_reader_t body_stream;            /**< or, when valid, a body read from here and sent chunked */
    proven_u8str_view_t upgrade;            /**< a protocol to change to (`websocket`): sends `Connection: Upgrade` and `Upgrade`; empty: none */
} proven_http_client_request_t;

/**
 * @brief A response in progress. Caller-owned; filled by proven_http_client_send, read with
 *        proven_http_client_read, and released with proven_http_client_finish.
 *
 * The views are good until proven_http_client_finish. Do not copy the struct while it is live.
 */
typedef struct {
    proven_u16 status;
    proven_u8str_view_t reason;
    const proven_http_header_t *headers;
    proven_size_t header_count;
    proven_u8str_view_t url;                /**< where this response came from: the request URL, or the last redirect's target */
    proven_u32 redirects;                   /**< how many redirects were followed to get here */
    void *internal;
} proven_http_client_response_t;

/**
 * @brief Send a request and read the head of the response.
 *
 * Returns once the response's status and headers are in `*response`; the body has not been
 * read. Whatever the outcome, call proven_http_client_finish on `response` afterwards - on
 * failure it has nothing to release and is harmless.
 *
 * **Headers the client writes itself** - `Host`, `Content-Length`, `Transfer-Encoding`,
 * `Connection` - may not be given in `headers` (PROVEN_ERR_INVALID_ARG): two of each is how
 * a message becomes ambiguous.
 *
 * **Redirects** (301, 302, 303, 307, 308) are followed up to `max_redirects`. 303 always, and
 * 301 and 302 for a `POST`, continue as `GET` without the body; 307 and 308 repeat the request
 * as it was. A redirect is NOT followed, and its 3xx response is returned to you instead, when
 * it leads from `https` to `http`, when the limit is reached, or when the request would have to
 * be repeated but its body was a stream that cannot be read twice. When a redirect leaves the
 * original host, `Authorization`, `Cookie` and `Proxy-Authorization` headers you supplied are
 * not sent on.
 *
 * **Authentication.** With a user name configured, a 401 carrying a Digest or Basic challenge
 * is answered once (Digest preferred; a `stale` nonce is retried once more). Credentials are
 * never sent before a server asks. Over plain HTTP, Basic sends the password in the clear.
 *
 * **A request with a stream body** is sent once: it is not retried on a reused connection that
 * turns out to be dead, and not repeated for a redirect or a challenge.
 *
 * Interim responses (`100 Continue`, `103 Early Hints`) are read and passed over; more than
 * eight before a final response is PROVEN_ERR_INVALID_FORMAT.
 *
 * @return PROVEN_ERR_INVALID_FORMAT for a URL that is not absolute or a malformed response;
 *         PROVEN_ERR_UNSUPPORTED for a scheme other than `http` and (with `tls_wrap`) `https`;
 *         the connection errors of net.h - PROVEN_ERR_NOT_FOUND (no such host),
 *         PROVEN_ERR_REFUSED, PROVEN_ERR_TIMEOUT, PROVEN_ERR_UNREACHABLE, PROVEN_ERR_RESET;
 *         PROVEN_ERR_OUT_OF_BOUNDS when the request or the response head exceeds
 *         `max_head_bytes`; PROVEN_ERR_PERMISSION when a proxy refuses the request;
 *         PROVEN_ERR_UNTRUSTED from `tls_wrap`; PROVEN_ERR_NOMEM.
 *         A response with an error STATUS - 404, 500 - is not an error here: it is a response.
 */
[[nodiscard]]
proven_err_t proven_http_client_send(proven_http_client_t *client, const proven_http_client_request_t *request,
                                     proven_http_client_response_t *response);

/** @brief `GET` a URL: proven_http_client_send with nothing else set. */
[[nodiscard]]
proven_err_t proven_http_client_get(proven_http_client_t *client, proven_u8str_view_t url,
                                    proven_http_client_response_t *response);

/**
 * @brief Read more of the response body into `dest`.
 *
 * Returns as soon as some bytes are available. `.err` is PROVEN_ERR_EOF when the body is
 * complete - never a zero-byte success. A body that is cut short by the peer closing is
 * PROVEN_ERR_RESET, not EOF: half a body must not look like a whole one.
 *
 * @return also PROVEN_ERR_TIMEOUT (no data within `io_timeout_ms`; the read may be repeated)
 *         and PROVEN_ERR_INVALID_FORMAT (malformed chunk framing).
 */
[[nodiscard]]
proven_result_size_t proven_http_client_read(proven_http_client_response_t *response, proven_mem_mut_t dest);

/**
 * @brief Read the rest of the body and append it to `out`, growing it with `alloc`.
 * @param max_bytes the most to accept. A body longer than this is PROVEN_ERR_OUT_OF_BOUNDS -
 *        `out` then holds the first `max_bytes` - rather than however much a server sends.
 */
[[nodiscard]]
proven_err_t proven_http_client_read_all(proven_http_client_response_t *response, proven_allocator_t alloc,
                                         proven_u8str_t *out, proven_size_t max_bytes);

/**
 * @brief Take the connection of a `101 Switching Protocols` response out of the client.
 *
 * After a request sent with `upgrade` set, a 101 means the server agreed and the connection now
 * speaks the other protocol. `*out` is a transport that owns it: use it for as long as you
 * like and close it with proven_transport_close. The client will not reuse it or close it,
 * and proven_http_client_finish - still to be called - no longer touches it.
 *
 * `*early` is what the server sent after its response head: the first bytes of the new
 * protocol, if they arrived with the 101. It is a view into the response and dies at
 * proven_http_client_finish; copy it first.
 *
 * Checking that the 101 is the answer you asked for is yours to do before this - for
 * WebSocket, proven_ws_check_response; ws_conn.h does both steps.
 *
 * The transport is freed, with the client's allocator, when it is closed: the allocator must
 * outlive it, though the client need not.
 *
 * @return PROVEN_ERR_INVALID_STATE when the response is not a 101, was already upgraded, or
 *         was never successfully sent.
 */
[[nodiscard]]
proven_err_t proven_http_client_upgrade(proven_http_client_response_t *response, proven_transport_t *out, proven_mem_view_t *early);

/**
 * @brief Release a response.
 *
 * If the body was read to its end and the server allows it, the connection goes back to the
 * client for the next request; otherwise it is closed. Safe on a zeroed response and after a
 * failed send; the struct is zeroed.
 */
void proven_http_client_finish(proven_http_client_response_t *response);

#endif /* PROVEN_HTTP_CLIENT_H */
