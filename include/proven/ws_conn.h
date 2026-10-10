#ifndef PROVEN_WS_CONN_H
#define PROVEN_WS_CONN_H

#include "proven/types.h"
#include "proven/error.h"
#include "proven/memory.h"
#include "proven/u8str.h"
#include "proven/allocator.h"
#include "proven/net.h"
#include "proven/ws.h"
#include "proven/http_client.h"
#include "proven/http_server.h"

/**
 * @file ws_conn.h
 * @brief A WebSocket connection: the codec of ws.h driven over a transport.
 *
 * A connection is made in one of three ways - as a client from a URL, as a server from a
 * request inside an HTTP handler, or over a transport you upgraded yourself - and from then on
 * both ends are the same thing: send a message, receive a message, close.
 *
 * What the connection does for you:
 *
 *   - **Pings are answered.** A ping that arrives while you are receiving gets its pong; you
 *     never see it.
 *   - **Messages arrive whole.** However the peer fragmented one, proven_ws_conn_receive gives
 *     it back in one piece, in memory the connection owns, up to `max_message_bytes`.
 *   - **Violations end the connection, with the reason.** A frame the protocol forbids, text
 *     that is not UTF-8, a message past the limit: the peer is sent a close with the code that
 *     says which, and you get the error.
 *   - **Closing is a handshake.** proven_ws_conn_close sends a close and waits, for a bounded
 *     time, for the peer's.
 *
 * What it does not do: reconnect, send pings on a timer, or compress. And it carries no more
 * protection than the connection under it - a `ws://` connection is readable by anyone on the
 * path. `wss://` is the same over TLS: the HTTP client's `tls_wrap`, the HTTP server's `tls`.
 *
 * **One thread at a time.** A connection is not safe to use from two threads at once - not
 * even one sending while another receives, because receiving answers pings. Give the
 * connection to one thread, or guard it.
 *
 * Hosted only; `PROVEN_NO_NET` leaves it out.
 */

/** @brief A WebSocket connection. Opaque; made by one of the three functions below. */
typedef struct proven_ws_conn proven_ws_conn_t;

/**
 * @brief How a connection behaves. Zero-initialise it and set `alloc`; every zero field has
 *        the default written beside it.
 */
typedef struct {
    proven_allocator_t alloc;           /**< required: the connection, its buffers, and the message being assembled */
    proven_size_t max_message_bytes;    /**< largest message accepted, fragments together; 0: 1 MiB */
    proven_u32 io_timeout_ms;           /**< the limit for each write; 0: 30 s */
    proven_u32 close_timeout_ms;        /**< how long proven_ws_conn_close waits for the peer's close; 0: 5 s */
} proven_ws_conn_config_t;

/** @brief A received message. `data` is memory the connection owns, good until the next
 *         proven_ws_conn_receive, _close or _destroy. */
typedef struct {
    bool text;                          /**< text - UTF-8, already checked - rather than binary */
    proven_mem_view_t data;
} proven_ws_message_t;

/**
 * @brief Connect as a client.
 *
 * Sends the upgrade request through `client` - so its proxy, its timeouts, its credentials and
 * its `tls_wrap` apply - checks the answer, and takes the connection out of the client. The
 * client is not needed afterwards.
 *
 * @param url `ws://host[:port]/path` or `wss://...` (`http://` and `https://` mean the same).
 *        `wss` needs the client's `tls_wrap`; without one it is PROVEN_ERR_UNSUPPORTED.
 * @param headers extra request headers - `Origin`, a token - or NULL.
 * @param protocols the subprotocols to offer, as they go in the header (`chat, superchat`), or
 *        empty. What the server chose is proven_ws_conn_protocol.
 * @param http_status optional: receives the HTTP status of the answer - 101 on success, and
 *        otherwise what the server said instead (401, 404, ...), or 0 if there was no answer.
 *
 * @return PROVEN_ERR_REFUSED when the server answered with something other than 101 - see
 *         `*http_status`; PROVEN_ERR_INVALID_FORMAT when its 101 was not a correct answer to
 *         this handshake (a wrong accept value, a subprotocol or an extension that was not
 *         offered); PROVEN_ERR_INVALID_ARG; PROVEN_ERR_NOMEM; PROVEN_ERR_IO when no random
 *         bytes could be drawn for the key; and the errors of proven_http_client_send.
 */
[[nodiscard]]
proven_err_t proven_ws_conn_connect(proven_http_client_t *client, proven_u8str_view_t url,
                                    const proven_http_header_t *headers, proven_size_t header_count,
                                    proven_u8str_view_t protocols, const proven_ws_conn_config_t *config,
                                    proven_ws_conn_t **out, proven_u16 *http_status);

/**
 * @brief Accept as a server, inside an HTTP handler.
 *
 * Checks that the request is a WebSocket upgrade, answers 101, and takes the connection out of
 * the server (proven_http_exchange_upgrade). The connection then lives as long as you keep it:
 * use it in the handler, or hand it to a thread of your own and return.
 *
 * **Where it runs matters.** With handlers on the loop's thread, a handler that stays to talk
 * WebSocket stops the server for everyone else: hand the connection off and return. With
 * handlers on a job system, a handler that stays occupies one worker for as long as the
 * connection lasts.
 *
 * @param protocol the subprotocol to select - it must be one the request offers
 *        (proven_ws_request_offers) - or empty for none.
 *
 * @return PROVEN_ERR_NOT_FOUND when the request is not a WebSocket upgrade at all: nothing was
 *         sent, answer it as an ordinary request. PROVEN_ERR_UNSUPPORTED (a 426 was sent) for
 *         another protocol version; PROVEN_ERR_INVALID_FORMAT (a 400 was sent) for a malformed
 *         upgrade. PROVEN_ERR_INVALID_ARG for a `protocol` the request does not offer, and
 *         PROVEN_ERR_NOMEM: nothing was sent and you may still respond. Otherwise the errors of
 *         proven_http_exchange_upgrade - and PROVEN_ERR_OUT_OF_BOUNDS when the client sent more
 *         than 16 KiB of frames along with its handshake: the 101 has gone by then, and the
 *         connection is closed.
 */
[[nodiscard]]
proven_err_t proven_ws_conn_accept(proven_http_exchange_t *exchange, proven_u8str_view_t protocol,
                                   const proven_ws_conn_config_t *config, proven_ws_conn_t **out);

/**
 * @brief Make a connection over a transport that is already past its opening handshake.
 *
 * For a transport you upgraded yourself, or a pair of in-memory transports in a test. On
 * success the connection owns the transport and closes it; on failure it is still yours.
 *
 * @param is_server true for the server's end: it expects masked frames and sends unmasked ones.
 * @param early bytes of the WebSocket stream that were read together with the HTTP handshake.
 *        They are copied.
 * @return PROVEN_ERR_INVALID_ARG; PROVEN_ERR_NOMEM; PROVEN_ERR_OUT_OF_BOUNDS when `early` is
 *         larger than the connection's read buffer (16 KiB); PROVEN_ERR_IO when a client could
 *         not draw random bytes for its masks.
 */
[[nodiscard]]
proven_err_t proven_ws_conn_open(proven_transport_t transport, bool is_server, proven_mem_view_t early,
                                 const proven_ws_conn_config_t *config, proven_ws_conn_t **out);

/**
 * @brief Send a text message.
 * @return PROVEN_ERR_INVALID_ENCODING when `text` is not UTF-8 - nothing is sent, because the
 *         peer would have to end the connection over it. PROVEN_ERR_INVALID_STATE after a
 *         close, or in the middle of a message begun with proven_ws_conn_send_part.
 *         PROVEN_ERR_TIMEOUT or PROVEN_ERR_RESET when the peer does not take it; the
 *         connection is then finished and every later call returns the same error.
 */
[[nodiscard]]
proven_err_t proven_ws_conn_send_text(proven_ws_conn_t *conn, proven_u8str_view_t text);

/** @brief Send a binary message. Errors as proven_ws_conn_send_text, without the encoding check. */
[[nodiscard]]
proven_err_t proven_ws_conn_send_binary(proven_ws_conn_t *conn, proven_mem_view_t data);

/**
 * @brief Send a message in pieces, when it is produced as it goes or its size is not known.
 *
 * The first call begins a message as text or binary; each call sends one fragment; the call
 * with `last` true ends it. `text` is read on the first call only. Text sent this way is NOT
 * checked: the pieces together must be UTF-8, and a character may be cut between two of them.
 * Pings may be sent between pieces; other messages may not.
 */
[[nodiscard]]
proven_err_t proven_ws_conn_send_part(proven_ws_conn_t *conn, bool text, proven_mem_view_t data, bool last);

/**
 * @brief Send a ping with up to 125 bytes of payload. The pong is counted when it arrives
 *        during a proven_ws_conn_receive - see proven_ws_conn_pong_count.
 * @return PROVEN_ERR_INVALID_ARG for a longer payload.
 */
[[nodiscard]]
proven_err_t proven_ws_conn_ping(proven_ws_conn_t *conn, proven_mem_view_t data);

/**
 * @brief Receive the next message.
 *
 * Waits until a whole message has arrived or until `until`. Pings are answered and pongs
 * counted on the way.
 *
 * @return PROVEN_ERR_TIMEOUT when no complete message arrived in time; nothing is lost, call
 *         again. PROVEN_ERR_EOF when the peer closed: its code and reason are
 *         proven_ws_conn_close_code and proven_ws_conn_close_reason, and the close has been
 *         answered. PROVEN_ERR_RESET when the connection ended without a close frame (code
 *         1006). PROVEN_ERR_INVALID_FORMAT, PROVEN_ERR_INVALID_ENCODING and
 *         PROVEN_ERR_OUT_OF_BOUNDS when the peer broke the protocol, sent text that is not
 *         UTF-8, or sent a message past `max_message_bytes`: it has been sent a close saying
 *         so (1002, 1007, 1009), and what it was still sending has been read away for up to a
 *         second so that the close reaches it. After anything but TIMEOUT the connection is
 *         finished.
 */
[[nodiscard]]
proven_err_t proven_ws_conn_receive(proven_ws_conn_t *conn, proven_net_deadline_t until, proven_ws_message_t *out);

/**
 * @brief Close: send a close frame and wait for the peer's.
 *
 * Messages that arrive while waiting are discarded. Safe to call when the peer has already
 * closed, or twice. The connection still has to be destroyed.
 *
 * @param code PROVEN_WS_CLOSE_NORMAL usually; any code proven_ws_close_code_is_valid accepts,
 *        or PROVEN_WS_CLOSE_NO_STATUS for a close frame with no code.
 * @param reason at most 123 bytes of UTF-8, for a person reading a log; may be empty.
 * @return PROVEN_OK when both sides have closed; PROVEN_ERR_TIMEOUT when the peer did not
 *         answer within `close_timeout_ms`; PROVEN_ERR_INVALID_ARG for a code or reason that
 *         may not be sent (nothing is sent); the connection's error if it had already failed.
 */
proven_err_t proven_ws_conn_close(proven_ws_conn_t *conn, proven_u16 code, proven_u8str_view_t reason);

/** @brief Close the transport and free the connection. Without a proven_ws_conn_close first,
 *         the peer sees the connection drop (code 1006). NULL is ignored. */
void proven_ws_conn_destroy(proven_ws_conn_t *conn);

/** @brief The subprotocol in use; empty when none was agreed. Good until destroy. */
[[nodiscard]]
proven_u8str_view_t proven_ws_conn_protocol(const proven_ws_conn_t *conn);

/** @brief The peer's close code once proven_ws_conn_receive has returned PROVEN_ERR_EOF;
 *         PROVEN_WS_CLOSE_ABNORMAL (1006) when the connection ended without one; 0 while open. */
[[nodiscard]]
proven_u16 proven_ws_conn_close_code(const proven_ws_conn_t *conn);

/** @brief The reason that came with the peer's close; often empty. Good until destroy. */
[[nodiscard]]
proven_u8str_view_t proven_ws_conn_close_reason(const proven_ws_conn_t *conn);

/** @brief How many pongs have arrived. Send a ping, receive for a while, and see whether this
 *         moved: that is how a program finds out a silent connection is dead. */
[[nodiscard]]
proven_u64 proven_ws_conn_pong_count(const proven_ws_conn_t *conn);

#endif /* PROVEN_WS_CONN_H */
