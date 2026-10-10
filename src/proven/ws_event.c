#include "proven/ws_event.h"
#include "proven_internal_http_event.h"

#if !defined(PROVEN_FREESTANDING) && !defined(PROVEN_NO_NET)

#include "proven/time.h"

/*
 * WebSocket on a connection of the event-driven server.
 *
 * http_event.c still owns the connection - socket, TLS, output buffer, timer - and hands this
 * file the bytes that arrive (proven_internal_http_event.h). What is here is the protocol: the
 * decoder of ws.h for input, frames for output, and the three things a WebSocket connection
 * can be waiting for - a message, the answer to a ping, the answer to a close.
 *
 * Callbacks may call back in, close and abort included. So, as in http_event.c:
 *   - `conn` is null from the moment the connection is let go of or gone;
 *   - `closed` is set before on_closed is called, and nothing is delivered after it;
 *   - the stream is freed only when the outermost entry leaves (`depth`).
 */

#define WS_DEFAULT_MESSAGE ((proven_u64)1024 * 1024)
#define WS_DEFAULT_BUFFERED ((proven_size_t)64 * 1024)
#define WS_DEFAULT_PING_MS 30000u
#define WS_DEFAULT_PONG_MS 10000u
#define WS_DEFAULT_CLOSE_MS 5000u

struct proven_ws_stream {
    const proven_http_event_raw_ops_t *ops;      /* first: http_event.c finds the operations here */
    proven_http_stream_t *conn;
    proven_allocator_t alloc;
    proven_ws_event_callbacks_t on;
    void *ctx;
    void *user;
    proven_u64 max_message;
    proven_size_t max_buffered;
    proven_size_t close_left;                    /* after a close was sent: output the client had not taken when last looked */
    proven_u32 ping_ms, pong_ms, close_ms;
    proven_u32 depth;
    proven_net_addr_t peer;
    proven_ws_decoder_t dec;
    proven_u16 close_code;                       /* the code this side sent */
    bool closed, close_sent, in_message, want_writable, pinged, paused;
};

static proven_u8str_view_t ws_lit_n(const char *s, proven_size_t n) { return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)s, .size = n }; }
#define ws_lit(s) ws_lit_n((s), sizeof(s) - 1)

static void ws_enter(proven_ws_stream_t *ws) { ws->depth++; }
static void ws_leave(proven_ws_stream_t *ws) {
    if (--ws->depth > 0 || !ws->closed) return;
    ws->alloc.free_fn(ws->alloc.ctx, ws);
}

/* The one place on_closed is called from. The connection has been let go of, or is gone. */
static void ws_finish(proven_ws_stream_t *ws, proven_u16 code, proven_err_t why) {
    if (ws->closed) return;
    ws->closed = true;
    ws->conn = (void *)0;
    ws_enter(ws);
    if (ws->on.on_closed) ws->on.on_closed(ws->ctx, ws, code, why);
    ws_leave(ws);
}

/* Let go of the connection and end. `polite` flushes what is queued and waits a moment for
 * the client to close; otherwise the socket is closed now. */
static void ws_end(proven_ws_stream_t *ws, bool polite, proven_u16 code, proven_err_t why) {
    proven_http_stream_t *conn = ws->conn;
    ws->conn = (void *)0;
    if (conn) {
        proven_http_event_raw_timer_(conn, 0);
        if (polite) proven_http_event_raw_close_(conn);
        else proven_http_event_raw_kill_(conn);
    }
    ws_finish(ws, code, why);
}

/* Queue one frame. PROVEN_ERR_RESET when the connection is gone - on_closed has then been called. */
static proven_err_t ws_frame(proven_ws_stream_t *ws, proven_u8 opcode, bool fin, proven_mem_view_t data) {
    if (!ws->conn) return PROVEN_ERR_RESET;
    proven_byte_t header[PROVEN_WS_MAX_FRAME_HEADER];
    proven_size_t len = 0;
    proven_ws_frame_t f = { .fin = fin, .opcode = opcode, .masked = false, .length = data.size };
    proven_err_t e = proven_ws_frame_write((proven_mem_mut_t){ .ptr = header, .size = sizeof header }, &len, &f);
    if (e != PROVEN_OK) return e;
    if (!proven_http_event_raw_send_(ws->conn, (proven_mem_view_t){ .ptr = header, .size = len }, data)) return PROVEN_ERR_RESET;
    return PROVEN_OK;
}

static void ws_send_close(proven_ws_stream_t *ws, proven_u16 code, proven_u8str_view_t reason) {
    proven_byte_t payload[PROVEN_WS_MAX_CONTROL];
    proven_size_t len = 0;
    proven_mem_mut_t out = { .ptr = payload, .size = sizeof payload };
    if (code != PROVEN_WS_CLOSE_NO_STATUS && proven_ws_close_write(out, &len, code, reason) != PROVEN_OK) {
        len = 0;
        if (proven_ws_close_write(out, &len, code, ws_lit("")) != PROVEN_OK) len = 0;
    }
    ws->close_sent = true;
    ws->close_code = code;
    if (ws_frame(ws, PROVEN_WS_CLOSE, true, (proven_mem_view_t){ .ptr = payload, .size = len }) == PROVEN_OK && ws->conn) {
        ws->close_left = proven_http_stream_buffered(ws->conn);
    }
}

/* The client broke the protocol, or sent more than is accepted: say so and end. */
static void ws_fail(proven_ws_stream_t *ws, proven_err_t why) {
    proven_u16 code = proven_ws_close_code_for(why);
    if (!ws->close_sent) ws_send_close(ws, code, ws_lit(""));
    if (!ws->closed) ws_end(ws, true, code, why);
}

static void ws_arm(proven_ws_stream_t *ws) {
    if (!ws->conn) return;
    proven_http_event_raw_timer_(ws->conn, ws->close_sent ? ws->close_ms : ws->pinged ? ws->pong_ms : ws->ping_ms);
}

// -----------------------------------------------------------------------------
// What http_event.c calls
// -----------------------------------------------------------------------------

static proven_size_t ws_on_input(void *raw, proven_mem_mut_t in) {
    proven_ws_stream_t *ws = raw;
    proven_size_t used = 0;
    ws_enter(ws);
    /* Anything at all from the client says it is there - until this side has closed: after
     * that only its answer counts, and a client that keeps talking does not buy more time. */
    if (!ws->close_sent) {
        ws->pinged = false;
        ws_arm(ws);
    }
    while (used < in.size && !ws->closed && !ws->paused && ws->conn) {
        proven_size_t n = 0;
        proven_ws_event_t ev = { 0 };
        proven_err_t e = proven_ws_decoder_feed(&ws->dec, (proven_mem_mut_t){ .ptr = in.ptr + used, .size = in.size - used }, &n, &ev);
        used += n;
        if (e == PROVEN_ERR_EOF) { used = in.size; break; }          /* bytes after a close frame: nothing to do with them */
        if (e != PROVEN_OK) { ws_fail(ws, e); break; }
        if (ev.kind == PROVEN_WS_EVENT_NONE) break;
        if (ev.kind == PROVEN_WS_EVENT_DATA) {
            if (!ws->close_sent) ws->on.on_message(ws->ctx, ws, ev.text, ev.data, ev.first, ev.last);
        } else if (ev.kind == PROVEN_WS_EVENT_PING) {
            if (!ws->close_sent) (void)ws_frame(ws, PROVEN_WS_PONG, true, ev.data);
        } else if (ev.kind == PROVEN_WS_EVENT_CLOSE) {
            if (ws->close_sent) {
                /* The answer to this side's close: the handshake is complete. */
                ws_end(ws, true, ws->close_code, PROVEN_OK);
            } else {
                proven_u16 code = ev.close_code;
                ws_send_close(ws, code, ws_lit(""));
                if (!ws->closed) ws_end(ws, true, code, PROVEN_OK);
            }
            break;
        }
    }
    ws_leave(ws);
    return used;
}

static void ws_on_progress(void *raw) {
    proven_ws_stream_t *ws = raw;
    if (ws->closed || !ws->conn) return;
    /* A close waits behind whatever was queued before it: while the client is still taking
     * that, it is not late with its answer. Only output that actually went counts - this is
     * also called when nothing moved. */
    if (ws->close_sent) {
        proven_size_t left = proven_http_stream_buffered(ws->conn);
        if (left < ws->close_left) { ws->close_left = left; ws_arm(ws); }
        return;
    }
    if (!ws->want_writable) return;
    if (proven_http_stream_buffered(ws->conn) >= ws->max_buffered) return;
    ws->want_writable = false;
    ws_enter(ws);
    if (ws->on.on_writable) ws->on.on_writable(ws->ctx, ws);
    ws_leave(ws);
}

static void ws_on_timer(void *raw) {
    proven_ws_stream_t *ws = raw;
    if (ws->closed || !ws->conn) return;
    ws_enter(ws);
    if (ws->close_sent) {
        /* The client never answered the close. */
        ws_end(ws, false, ws->close_code, PROVEN_ERR_TIMEOUT);
    } else if (ws->paused) {
        /* The program is not reading; silence proves nothing. */
        ws_arm(ws);
    } else if (!ws->pinged) {
        ws->pinged = true;
        if (ws_frame(ws, PROVEN_WS_PING, true, (proven_mem_view_t){ 0 }) == PROVEN_OK) ws_arm(ws);
    } else {
        ws_end(ws, false, PROVEN_WS_CLOSE_ABNORMAL, PROVEN_ERR_TIMEOUT);
    }
    ws_leave(ws);
}

static void ws_on_closed(void *raw, proven_err_t why) {
    proven_ws_stream_t *ws = raw;
    /* An end of stream with no close frame is a client that vanished, whatever it meant. */
    if (why == PROVEN_ERR_EOF || why == PROVEN_OK) why = PROVEN_ERR_RESET;
    ws_finish(ws, ws->close_sent ? ws->close_code : PROVEN_WS_CLOSE_ABNORMAL, why);
}

static const proven_http_event_raw_ops_t ws_ops = {
    .on_input = ws_on_input, .on_progress = ws_on_progress, .on_timer = ws_on_timer, .on_closed = ws_on_closed,
};

// -----------------------------------------------------------------------------
// Public
// -----------------------------------------------------------------------------

static bool ws_is_token(proven_u8str_view_t s) {
    for (proven_size_t i = 0; i < s.size; ++i) {
        proven_byte_t c = s.ptr[i];
        bool alnum = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        bool mark = c == '!' || c == '#' || c == '$' || c == '%' || c == '&' || c == '\'' || c == '*' || c == '+' ||
                    c == '-' || c == '.' || c == '^' || c == '_' || c == '`' || c == '|' || c == '~';
        if (!alnum && !mark) return false;
    }
    return true;
}

proven_err_t proven_ws_event_accept(proven_http_stream_t *stream, const proven_http_request_t *head,
                                    const proven_ws_event_config_t *config, proven_ws_stream_t **out) {
    if (out) *out = (void *)0;
    if (!stream || !head || !config || !out || !config->on.on_message || !ws_is_token(config->subprotocol)) return PROVEN_ERR_INVALID_ARG;
    proven_u8str_view_t key;
    proven_err_t e = proven_ws_check_request(head, &key);
    if (e != PROVEN_OK) return e;
    proven_byte_t accept[PROVEN_WS_ACCEPT_SIZE];
    e = proven_ws_accept_key(key, accept);
    if (e != PROVEN_OK) return e;

    proven_byte_t text[512];
    proven_mem_mut_t answer = { .ptr = text, .size = sizeof text };
    proven_size_t len = 0;
    e = proven_http_write_status_line(answer, &len, 101, ws_lit(""));
    proven_byte_t date[PROVEN_HTTP_DATE_SIZE];
    if (e == PROVEN_OK && proven_http_date_format(proven_time_now(), date) == PROVEN_OK) {
        e = proven_http_write_header(answer, &len, ws_lit("Date"), (proven_u8str_view_t){ .ptr = date, .size = sizeof date });
    }
    if (e == PROVEN_OK) e = proven_http_write_header(answer, &len, ws_lit("Upgrade"), ws_lit("websocket"));
    if (e == PROVEN_OK) e = proven_http_write_header(answer, &len, ws_lit("Connection"), ws_lit("Upgrade"));
    if (e == PROVEN_OK) e = proven_http_write_header(answer, &len, ws_lit("Sec-WebSocket-Accept"), (proven_u8str_view_t){ .ptr = accept, .size = sizeof accept });
    if (e == PROVEN_OK && config->subprotocol.size > 0) e = proven_http_write_header(answer, &len, ws_lit("Sec-WebSocket-Protocol"), config->subprotocol);
    if (e == PROVEN_OK) e = proven_http_write_head_end(answer, &len);
    if (e != PROVEN_OK) return e == PROVEN_ERR_OUT_OF_BOUNDS ? PROVEN_ERR_INVALID_ARG : e;      /* a subprotocol too long to name */

    proven_allocator_t alloc = proven_http_event_raw_allocator_(stream);
    proven_result_mem_mut_t m = alloc.alloc_fn(alloc.ctx, sizeof(proven_ws_stream_t), 16);
    if (m.err != PROVEN_OK) return PROVEN_ERR_NOMEM;
    proven_ws_stream_t *ws = (proven_ws_stream_t *)(void *)m.value.ptr;
    *ws = (proven_ws_stream_t){
        .ops = &ws_ops, .alloc = alloc, .on = config->on, .ctx = config->ctx,
        .max_message = config->max_message_bytes ? config->max_message_bytes : WS_DEFAULT_MESSAGE,
        .max_buffered = config->max_buffered_output ? config->max_buffered_output : WS_DEFAULT_BUFFERED,
        .ping_ms = config->ping_interval_ms ? config->ping_interval_ms : WS_DEFAULT_PING_MS,
        .pong_ms = config->pong_timeout_ms ? config->pong_timeout_ms : WS_DEFAULT_PONG_MS,
        .close_ms = config->close_timeout_ms ? config->close_timeout_ms : WS_DEFAULT_CLOSE_MS,
        .peer = proven_http_stream_peer(stream),
    };
    proven_ws_decoder_init(&ws->dec, true, ws->max_message);

    e = proven_http_event_raw_begin_(stream, ws, (proven_mem_view_t){ .ptr = text, .size = len });
    if (e != PROVEN_OK) { alloc.free_fn(alloc.ctx, ws); return e; }
    ws->conn = stream;
    ws_arm(ws);
    *out = ws;
    return PROVEN_OK;
}

/* Whether a data or ping frame may be queued now. */
static proven_err_t ws_may_send(proven_ws_stream_t *ws) {
    if (!ws) return PROVEN_ERR_INVALID_ARG;
    if (ws->closed || !ws->conn) return PROVEN_ERR_RESET;
    if (ws->close_sent) return PROVEN_ERR_INVALID_STATE;
    if (proven_http_stream_buffered(ws->conn) >= ws->max_buffered) { ws->want_writable = true; return PROVEN_ERR_AGAIN; }
    return PROVEN_OK;
}

proven_err_t proven_ws_stream_send_piece(proven_ws_stream_t *ws, bool text, proven_mem_view_t data, bool first, bool last) {
    if (ws && !ws->closed && ws->conn && !ws->close_sent && first == ws->in_message) return PROVEN_ERR_INVALID_STATE;
    if (data.size > 0 && !data.ptr) return PROVEN_ERR_INVALID_ARG;
    proven_err_t e = ws_may_send(ws);
    if (e != PROVEN_OK) return e;
    ws_enter(ws);
    e = ws_frame(ws, first ? (text ? PROVEN_WS_TEXT : PROVEN_WS_BINARY) : PROVEN_WS_CONTINUATION, last, data);
    if (e == PROVEN_OK) ws->in_message = !last;
    ws_leave(ws);
    return e;
}

proven_err_t proven_ws_stream_send(proven_ws_stream_t *ws, bool text, proven_mem_view_t data) {
    return proven_ws_stream_send_piece(ws, text, data, true, true);
}

proven_err_t proven_ws_stream_ping(proven_ws_stream_t *ws, proven_mem_view_t data) {
    if (data.size > PROVEN_WS_MAX_CONTROL) return PROVEN_ERR_OUT_OF_BOUNDS;
    if (data.size > 0 && !data.ptr) return PROVEN_ERR_INVALID_ARG;
    proven_err_t e = ws_may_send(ws);
    if (e != PROVEN_OK) return e;
    ws_enter(ws);
    e = ws_frame(ws, PROVEN_WS_PING, true, data);
    ws_leave(ws);
    return e;
}

void proven_ws_stream_close(proven_ws_stream_t *ws, proven_u16 code, proven_u8str_view_t reason) {
    if (!ws || ws->closed || !ws->conn || ws->close_sent) return;
    if (!proven_ws_close_code_is_valid(code)) code = PROVEN_WS_CLOSE_NORMAL;
    ws_enter(ws);
    ws_send_close(ws, code, reason);
    ws_arm(ws);
    ws_leave(ws);
}

void proven_ws_stream_abort(proven_ws_stream_t *ws) {
    if (!ws || ws->closed) return;
    ws_enter(ws);
    ws_end(ws, false, PROVEN_WS_CLOSE_ABNORMAL, PROVEN_ERR_RESET);
    ws_leave(ws);
}

void proven_ws_stream_pause(proven_ws_stream_t *ws) {
    if (!ws || ws->closed || !ws->conn || ws->paused) return;
    ws->paused = true;
    proven_http_event_raw_pause_(ws->conn, true);
}

void proven_ws_stream_resume(proven_ws_stream_t *ws) {
    if (!ws || ws->closed || !ws->conn || !ws->paused) return;
    ws->paused = false;
    ws_enter(ws);
    proven_http_event_raw_pause_(ws->conn, false);
    ws_leave(ws);
}

void proven_ws_stream_set_user(proven_ws_stream_t *ws, void *user) { if (ws) ws->user = user; }
void *proven_ws_stream_user(const proven_ws_stream_t *ws) { return ws ? ws->user : (void *)0; }
proven_net_addr_t proven_ws_stream_peer(const proven_ws_stream_t *ws) { return ws ? ws->peer : (proven_net_addr_t){ 0 }; }
proven_size_t proven_ws_stream_buffered(const proven_ws_stream_t *ws) { return ws && ws->conn ? proven_http_stream_buffered(ws->conn) : 0; }

#endif
