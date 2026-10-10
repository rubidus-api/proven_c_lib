#ifndef PROVEN_WS_EVENT_H
#define PROVEN_WS_EVENT_H

#include "proven/types.h"
#include "proven/error.h"
#include "proven/memory.h"
#include "proven/u8str.h"
#include "proven/net.h"
#include "proven/http.h"
#include "proven/http_event.h"
#include "proven/ws.h"

/**
 * @file ws_event.h
 * @brief WebSocket on the event-driven server: a connection that costs no thread.
 *
 * ws_conn.h gives a WebSocket connection calls that wait, and so a thread for as long as it is
 * open. Here a request to the server of http_event.h is turned into a WebSocket inside
 * `on_request`, and from then on your functions are told what arrived - nothing waits, and a
 * connection that is silent holds its state and no buffer.
 *
 * The same three rules as http_event.h: your functions must not wait; a send can be refused
 * (PROVEN_ERR_AGAIN, with `on_writable` to say when to try again); and a piece of a message is
 * good only until the function it was given to returns.
 *
 * Messages arrive **in the pieces the network delivered**, with `first` and `last` marking
 * their ends: a message of any size passes through without being assembled or held. A handler
 * that wants whole messages collects the pieces itself, up to a limit of its choosing.
 *
 * Everything here is called on the loop's thread, and only there.
 *
 * Hosted only; `PROVEN_NO_NET` leaves it out.
 */

/** @brief A WebSocket connection. Valid from proven_ws_event_accept until `on_closed` returns. */
typedef struct proven_ws_stream proven_ws_stream_t;

typedef struct {
    /** A piece of a text or binary message. Required. Text is checked to be UTF-8 across the
     *  whole message; `piece` may end in the middle of a character. */
    void (*on_message)(void *ctx, proven_ws_stream_t *ws, bool text, proven_mem_view_t piece, bool first, bool last);
    /** Output that was held back has gone: a send that was refused will now be taken. Optional. */
    void (*on_writable)(void *ctx, proven_ws_stream_t *ws);
    /** The connection is over, called exactly once. `code` is the close code that ended it -
     *  the peer's, or the one this side sent first - and 1006 when it ended without a close
     *  frame; `why` is PROVEN_OK after a close handshake, otherwise the reason (the peer
     *  vanished, a timeout, a protocol error). Free what you attached here. Optional. */
    void (*on_closed)(void *ctx, proven_ws_stream_t *ws, proven_u16 code, proven_err_t why);
} proven_ws_event_callbacks_t;

/** @brief How one WebSocket connection behaves. Zero-initialise it and set `on.on_message`;
 *         every zero field has the default written beside it. */
typedef struct {
    proven_ws_event_callbacks_t on;
    void *ctx;                             /**< passed to every callback */
    proven_u8str_view_t subprotocol;       /**< named in the answer when not empty. Offer it only if the client did (proven_ws_request_offers). */
    proven_u64 max_message_bytes;          /**< largest message accepted, its pieces together. 0: 1 MiB */
    proven_size_t max_buffered_output;     /**< output held before sends are refused. 0: 64 KiB */
    proven_u32 ping_interval_ms;           /**< silence from the client after which a ping is sent. 0: 30 s */
    proven_u32 pong_timeout_ms;            /**< further silence after which the connection is ended. 0: 10 s */
    proven_u32 close_timeout_ms;           /**< how long to wait for the client's answer to a close, counted from the last output it took. 0: 5 s */
} proven_ws_event_config_t;

/**
 * @brief Inside `on_request`: accept the request as a WebSocket.
 *
 * Checks the request (proven_ws_check_request), answers 101, and turns the connection into a
 * WebSocket. **The HTTP exchange ends here: `on_done(PROVEN_OK)` is called inside this call**,
 * after which `stream` is over and `*out` is what you hold.
 *
 * On any error nothing has been sent and the request is still yours to answer -
 * 426 with `Sec-WebSocket-Version: 13` for PROVEN_ERR_UNSUPPORTED, 400 for
 * PROVEN_ERR_INVALID_FORMAT, and whatever the page is for PROVEN_ERR_NOT_FOUND, which means
 * the request did not ask for a WebSocket at all.
 *
 * @return PROVEN_ERR_INVALID_ARG (no `on_message`, a subprotocol that is not a token);
 *         PROVEN_ERR_INVALID_STATE when not called inside `on_request` for this stream, or a
 *         response was already begun; the errors of proven_ws_check_request; PROVEN_ERR_NOMEM;
 *         PROVEN_ERR_RESET when the client vanished at that moment (`on_done` was called).
 */
[[nodiscard]]
proven_err_t proven_ws_event_accept(proven_http_stream_t *stream, const proven_http_request_t *head,
                                    const proven_ws_event_config_t *config, proven_ws_stream_t **out);

/**
 * @brief Send one whole message.
 *
 * Taken whole or not at all: when less than `max_buffered_output` is held it is queued
 * (copied) and sent as the client takes it; otherwise nothing is taken and the answer is
 * PROVEN_ERR_AGAIN - keep the message and send it again when `on_writable` is called. The most
 * a connection holds is therefore the limit and one message of your choosing.
 *
 * @return PROVEN_ERR_AGAIN as described; PROVEN_ERR_INVALID_STATE after a close was sent or
 *         received, or in the middle of a message begun with proven_ws_stream_send_piece;
 *         PROVEN_ERR_RESET when the connection is gone.
 */
[[nodiscard]]
proven_err_t proven_ws_stream_send(proven_ws_stream_t *ws, bool text, proven_mem_view_t data);

/**
 * @brief Send one piece of a message that is produced as it goes: `first` with the first
 *        piece, `last` with the final one (which may be empty). Each piece is taken or
 *        refused as proven_ws_stream_send says. Text must be UTF-8 over the whole message;
 *        this side does not check what it sends.
 * @return as proven_ws_stream_send; PROVEN_ERR_INVALID_STATE also for `first` inside a
 *         message, or a piece that is not `first` outside one.
 */
[[nodiscard]]
proven_err_t proven_ws_stream_send_piece(proven_ws_stream_t *ws, bool text, proven_mem_view_t data, bool first, bool last);

/** @brief Send a ping with up to 125 bytes. Pings from the client are answered for you, and
 *         one is sent for you after `ping_interval_ms` of silence; this is for a program that
 *         wants its own. PROVEN_ERR_OUT_OF_BOUNDS for more than 125 bytes; otherwise as
 *         proven_ws_stream_send. */
[[nodiscard]]
proven_err_t proven_ws_stream_ping(proven_ws_stream_t *ws, proven_mem_view_t data);

/**
 * @brief Close: send a close frame with `code` and `reason` (at most 123 bytes of UTF-8), and
 *        end the connection when the client answers or `close_timeout_ms` passes. `on_closed`
 *        follows, later. A code that may not be sent (proven_ws_close_code_is_valid) becomes
 *        1000, and a reason that does not fit is left out. Harmless when a close is already
 *        under way.
 */
void proven_ws_stream_close(proven_ws_stream_t *ws, proven_u16 code, proven_u8str_view_t reason);

/** @brief End the connection at once, with no close frame. `on_closed` is called with 1006
 *         and PROVEN_ERR_RESET, inside this call. */
void proven_ws_stream_abort(proven_ws_stream_t *ws);

/** @brief Stop delivering messages (and stop reading the connection) until
 *         proven_ws_stream_resume. */
void proven_ws_stream_pause(proven_ws_stream_t *ws);
/** @brief Deliver again. Pieces that were waiting may arrive inside this call. */
void proven_ws_stream_resume(proven_ws_stream_t *ws);

/** @brief Attach a pointer of yours to the connection, and get it back. */
void proven_ws_stream_set_user(proven_ws_stream_t *ws, void *user);
void *proven_ws_stream_user(const proven_ws_stream_t *ws);

/** @brief The client's address. */
proven_net_addr_t proven_ws_stream_peer(const proven_ws_stream_t *ws);

/** @brief Bytes queued for the client and not yet accepted by it. */
proven_size_t proven_ws_stream_buffered(const proven_ws_stream_t *ws);

#endif /* PROVEN_WS_EVENT_H */
