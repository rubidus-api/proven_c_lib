#ifndef PROVEN_INTERNAL_HTTP_EVENT_H
#define PROVEN_INTERNAL_HTTP_EVENT_H

#include "proven/http_event.h"

/*
 * Internal: a connection of the event-driven server taken over by another protocol.
 *
 * After an upgrade the connection record stays what it was - socket, TLS engine, output
 * buffer, flushing, one timer - and stops being HTTP: bytes that arrive go to the functions
 * below instead of the request parser. ws_event.c is built on this. Not public API.
 *
 * The protocol's own object must begin with a pointer to its operations, and is what every
 * operation receives. All of this runs on the loop's thread.
 */
typedef struct {
    /* Bytes arrived, in memory that may be written (a payload is unmasked where it lies).
     * Returns how many were used; fewer than given means "not now" - paused, or ended. */
    proven_size_t (*on_input)(void *raw, proven_mem_mut_t in);
    /* Output moved towards the client: there may be room for more. */
    void (*on_progress)(void *raw);
    /* The timer set with proven_http_event_raw_timer_ fired. */
    void (*on_timer)(void *raw);
    /* The connection is gone, and was not detached first. Called once. */
    void (*on_closed)(void *raw, proven_err_t why);
} proven_http_event_raw_ops_t;

/* Inside on_request, with no response begun: send `head` (a complete response head), end the
 * HTTP exchange with on_done(PROVEN_OK), and hand the connection to `raw`.
 * PROVEN_ERR_INVALID_STATE when the stream cannot be taken over now; PROVEN_ERR_RESET when the
 * connection died while the head was being sent - `raw` was then not attached. */
[[nodiscard]]
proven_err_t proven_http_event_raw_begin_(proven_http_stream_t *stream, void *raw, proven_mem_view_t head);

/* Queue `a` then `b` for the client and send what goes at once. False when the connection is
 * gone (on_closed has then been called, unless detached). */
bool proven_http_event_raw_send_(proven_http_stream_t *stream, proven_mem_view_t a, proven_mem_view_t b);

/* Fire on_timer in `ms` milliseconds; 0 cancels. */
void proven_http_event_raw_timer_(proven_http_stream_t *stream, proven_u32 ms);

/* Stop or resume reading the connection. Resuming may deliver input inside the call. */
void proven_http_event_raw_pause_(proven_http_stream_t *stream, bool paused);

/* Let go of the connection: no operation is called after this. Then either close politely -
 * flush what is queued, say that nothing more will come, wait a moment for the client to
 * close - or at once. After either the stream must not be used. */
void proven_http_event_raw_close_(proven_http_stream_t *stream);
void proven_http_event_raw_kill_(proven_http_stream_t *stream);

proven_allocator_t proven_http_event_raw_allocator_(const proven_http_stream_t *stream);

#endif /* PROVEN_INTERNAL_HTTP_EVENT_H */
