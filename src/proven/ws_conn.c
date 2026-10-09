#include "proven/ws_conn.h"

#if !defined(PROVEN_FREESTANDING) && !defined(PROVEN_NO_NET)

#include "proven/random.h"
#include "proven/utf.h"
#include "proven/url.h"

/*
 * The WebSocket connection driver. The frames are ws.c's business and the opening handshake's
 * HTTP is the client's and the server's; what is here is the sequence: read, feed the decoder,
 * act on what it reports, and frame what is sent.
 */

#define WC_READ_SIZE 16384u
#define WC_SEND_SIZE 4096u
#define WC_DEFAULT_MAX_MESSAGE (1024u * 1024u)
#define WC_LINGER_MS 1000u             /* how long a refused peer's remaining bytes are read away */

struct proven_ws_conn {
    proven_ws_conn_config_t cfg;
    proven_transport_t t;
    proven_ws_decoder_t decoder;
    proven_chacha_rng_t masks;          /* a client's mask keys */
    proven_byte_t *message;             /* the message being assembled */
    proven_size_t message_len;
    proven_size_t message_cap;
    proven_size_t read_pos;
    proven_size_t read_len;
    proven_u64 pongs;
    proven_err_t failed;
    proven_u16 close_code;
    proven_u8 close_reason_len;
    proven_u8 protocol_len;
    bool is_server;
    bool message_text;
    bool sending;                       /* a message begun with send_part is open */
    bool close_sent;
    bool close_received;
    proven_byte_t close_reason[PROVEN_WS_MAX_CONTROL];
    proven_byte_t protocol[64];
    proven_byte_t read_buf[WC_READ_SIZE];
    proven_byte_t send_buf[WC_SEND_SIZE];
};

static proven_u8str_view_t wc_lit(const char *s) {
    proven_size_t n = 0;
    while (s[n] != '\0') n++;
    return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)s, .size = n };
}

static proven_net_deadline_t wc_write_deadline(const proven_ws_conn_t *c) {
    return proven_net_deadline_in(c->cfg.io_timeout_ms);
}

// -----------------------------------------------------------------------------
// Sending
// -----------------------------------------------------------------------------

/* One frame. A client's payload goes through send_buf to be masked; a server's goes as it is. */
static proven_err_t wc_send_frame(proven_ws_conn_t *c, bool fin, proven_u8 opcode, proven_mem_view_t data) {
    proven_ws_frame_t f = { .fin = fin, .opcode = opcode, .masked = !c->is_server, .length = data.size };
    if (f.masked) proven_chacha_rng_fill(&c->masks, f.mask, sizeof f.mask);
    proven_size_t n = 0;
    proven_err_t e = proven_ws_frame_write((proven_mem_mut_t){ .ptr = c->send_buf, .size = sizeof c->send_buf }, &n, &f);
    if (e != PROVEN_OK) return e;

    proven_size_t done = 0;
    for (;;) {
        /* The header and as much payload as fits leave together: a small frame is one write. */
        proven_size_t room = sizeof c->send_buf - n;
        proven_size_t take = data.size - done < room ? data.size - done : room;
        for (proven_size_t i = 0; i < take; ++i) c->send_buf[n + i] = data.ptr[done + i];
        if (f.masked) proven_ws_mask((proven_mem_mut_t){ .ptr = c->send_buf + n, .size = take }, f.mask, done);
        proven_result_size_t w = proven_transport_write_all(c->t, (proven_mem_view_t){ .ptr = c->send_buf, .size = n + take }, wc_write_deadline(c));
        if (w.err != PROVEN_OK) { c->failed = w.err; return w.err; }
        done += take;
        n = 0;
        if (done == data.size) return PROVEN_OK;
    }
}

static proven_err_t wc_send_close(proven_ws_conn_t *c, proven_u16 code, proven_u8str_view_t reason) {
    proven_byte_t payload[PROVEN_WS_MAX_CONTROL];
    proven_size_t n = 0;
    proven_err_t e = proven_ws_close_write((proven_mem_mut_t){ .ptr = payload, .size = sizeof payload }, &n, code, reason);
    if (e != PROVEN_OK) return e;
    c->close_sent = true;
    return wc_send_frame(c, true, PROVEN_WS_CLOSE, (proven_mem_view_t){ .ptr = payload, .size = n });
}

static proven_err_t wc_can_send(const proven_ws_conn_t *c) {
    if (!c) return PROVEN_ERR_INVALID_ARG;
    if (c->failed != PROVEN_OK) return c->failed;
    if (c->close_sent || c->close_received) return PROVEN_ERR_INVALID_STATE;
    return PROVEN_OK;
}

proven_err_t proven_ws_conn_send_part(proven_ws_conn_t *conn, bool text, proven_mem_view_t data, bool last) {
    proven_err_t e = wc_can_send(conn);
    if (e != PROVEN_OK) return e;
    if (data.size > 0 && !data.ptr) return PROVEN_ERR_INVALID_ARG;
    proven_u8 opcode = (proven_u8)(conn->sending ? PROVEN_WS_CONTINUATION : (text ? PROVEN_WS_TEXT : PROVEN_WS_BINARY));
    e = wc_send_frame(conn, last, opcode, data);
    if (e == PROVEN_OK) conn->sending = !last;
    return e;
}

proven_err_t proven_ws_conn_send_binary(proven_ws_conn_t *conn, proven_mem_view_t data) {
    proven_err_t e = wc_can_send(conn);
    if (e != PROVEN_OK) return e;
    if (conn->sending) return PROVEN_ERR_INVALID_STATE;
    return proven_ws_conn_send_part(conn, false, data, true);
}

proven_err_t proven_ws_conn_send_text(proven_ws_conn_t *conn, proven_u8str_view_t text) {
    proven_err_t e = wc_can_send(conn);
    if (e != PROVEN_OK) return e;
    if (conn->sending) return PROVEN_ERR_INVALID_STATE;
    if (text.size > 0 && !text.ptr) return PROVEN_ERR_INVALID_ARG;
    for (proven_size_t pos = 0; pos < text.size;) {
        if (text.ptr[pos] < 0x80) { pos++; continue; }
        proven_utf8_char_t ch = proven_utf8_decode_next(text, pos);
        if (ch.err != PROVEN_OK) return PROVEN_ERR_INVALID_ENCODING;
        pos += ch.len;
    }
    return proven_ws_conn_send_part(conn, true, (proven_mem_view_t){ .ptr = text.ptr, .size = text.size }, true);
}

proven_err_t proven_ws_conn_ping(proven_ws_conn_t *conn, proven_mem_view_t data) {
    proven_err_t e = wc_can_send(conn);
    if (e != PROVEN_OK) return e;
    if (data.size > PROVEN_WS_MAX_CONTROL || (data.size > 0 && !data.ptr)) return PROVEN_ERR_INVALID_ARG;
    return wc_send_frame(conn, true, PROVEN_WS_PING, data);
}

// -----------------------------------------------------------------------------
// Receiving
// -----------------------------------------------------------------------------

static proven_err_t wc_message_append(proven_ws_conn_t *c, proven_mem_view_t piece) {
    if (piece.size > c->cfg.max_message_bytes - c->message_len) return PROVEN_ERR_OUT_OF_BOUNDS;
    proven_size_t need = c->message_len + piece.size;
    if (need > c->message_cap) {
        proven_size_t cap = c->message_cap ? c->message_cap : 1024;
        while (cap < need) cap = cap > c->cfg.max_message_bytes / 2 ? c->cfg.max_message_bytes : cap * 2;
        proven_allocator_t a = c->cfg.alloc;
        proven_result_mem_mut_t m = c->message ? a.realloc_fn(a.ctx, c->message, c->message_cap, cap, 16) : a.alloc_fn(a.ctx, cap, 16);
        if (!proven_is_ok(m.err)) return PROVEN_ERR_NOMEM;
        c->message = m.value.ptr;
        c->message_cap = cap;
    }
    for (proven_size_t i = 0; i < piece.size; ++i) c->message[c->message_len + i] = piece.ptr[i];
    c->message_len = need;
    return PROVEN_OK;
}

/* The peer did something the connection cannot continue after: tell it why, once, and stop. */
static proven_err_t wc_fail(proven_ws_conn_t *c, proven_err_t e) {
    if (!c->close_sent && c->failed == PROVEN_OK &&
        wc_send_close(c, proven_ws_close_code_for(e), (proven_u8str_view_t){0}) == PROVEN_OK) {
        /* The peer may be in the middle of sending - a frame past the limit, say. Closing the
         * socket with that unread would answer with a reset, and the reset would destroy the
         * close frame just sent before the peer read it. So: stop sending, and read the rest
         * away for a moment, until the peer closes or the moment is over. */
        (void)proven_transport_shutdown(c->t);
        proven_net_deadline_t until = proven_net_deadline_in(WC_LINGER_MS);
        for (;;) {
            proven_result_size_t r = proven_transport_read(c->t, (proven_mem_mut_t){ .ptr = c->read_buf, .size = sizeof c->read_buf }, until);
            if (r.err != PROVEN_OK) break;
        }
        c->read_pos = 0;
        c->read_len = 0;
    }
    c->failed = e;
    return e;
}

/*
 * Read and decode until a message is complete (returns PROVEN_OK with *out set), the peer
 * closes (PROVEN_ERR_EOF), or something ends the wait. With `discard` set, messages are
 * dropped and pings are not answered: that is the state after this side has sent its close.
 */
static proven_err_t wc_pump(proven_ws_conn_t *c, proven_net_deadline_t until, proven_ws_message_t *out, bool discard) {
    for (;;) {
        if (c->close_received) return PROVEN_ERR_EOF;
        if (c->failed != PROVEN_OK) return c->failed;
        if (c->read_pos == c->read_len) {
            c->read_pos = 0;
            c->read_len = 0;
            proven_result_size_t r = proven_transport_read(c->t, (proven_mem_mut_t){ .ptr = c->read_buf, .size = sizeof c->read_buf }, until);
            if (r.err == PROVEN_ERR_TIMEOUT) return PROVEN_ERR_TIMEOUT;
            if (r.err != PROVEN_OK) {
                /* The connection ended with no close frame: an abnormal closure. */
                c->close_code = PROVEN_WS_CLOSE_ABNORMAL;
                c->failed = r.err == PROVEN_ERR_EOF ? PROVEN_ERR_RESET : r.err;
                return c->failed;
            }
            c->read_len = r.value;
        }
        proven_size_t used = 0;
        proven_ws_event_t ev;
        proven_err_t e = proven_ws_decoder_feed(&c->decoder, (proven_mem_mut_t){ .ptr = c->read_buf + c->read_pos, .size = c->read_len - c->read_pos }, &used, &ev);
        c->read_pos += used;
        if (e != PROVEN_OK) return wc_fail(c, e);
        switch (ev.kind) {
            case PROVEN_WS_EVENT_NONE:
                break;
            case PROVEN_WS_EVENT_DATA:
                if (discard) break;
                if (ev.first) { c->message_len = 0; c->message_text = ev.text; }
                e = wc_message_append(c, ev.data);
                if (e != PROVEN_OK) return wc_fail(c, e);
                if (ev.last) {
                    out->text = c->message_text;
                    out->data = (proven_mem_view_t){ .ptr = c->message, .size = c->message_len };
                    return PROVEN_OK;
                }
                break;
            case PROVEN_WS_EVENT_PING:
                if (!discard && !c->close_sent) {
                    e = wc_send_frame(c, true, PROVEN_WS_PONG, ev.data);
                    if (e != PROVEN_OK) return e;
                }
                break;
            case PROVEN_WS_EVENT_PONG:
                c->pongs++;
                break;
            case PROVEN_WS_EVENT_CLOSE:
                c->close_received = true;
                c->close_code = ev.close_code;
                c->close_reason_len = (proven_u8)ev.data.size;
                for (proven_size_t i = 0; i < ev.data.size; ++i) c->close_reason[i] = ev.data.ptr[i];
                /* Answer with the same code, as the RFC asks; a close without one is answered
                 * without one. */
                if (!c->close_sent) (void)wc_send_close(c, ev.close_code, (proven_u8str_view_t){0});
                (void)proven_transport_shutdown(c->t);
                return PROVEN_ERR_EOF;
        }
    }
}

proven_err_t proven_ws_conn_receive(proven_ws_conn_t *conn, proven_net_deadline_t until, proven_ws_message_t *out) {
    if (out) *out = (proven_ws_message_t){0};
    if (!conn || !out) return PROVEN_ERR_INVALID_ARG;
    if (conn->close_sent && !conn->close_received && conn->failed == PROVEN_OK) return PROVEN_ERR_INVALID_STATE;
    return wc_pump(conn, until, out, false);
}

proven_err_t proven_ws_conn_close(proven_ws_conn_t *conn, proven_u16 code, proven_u8str_view_t reason) {
    if (!conn) return PROVEN_ERR_INVALID_ARG;
    if (conn->failed != PROVEN_OK) return conn->failed;
    if (!conn->close_sent) {
        proven_err_t e = wc_send_close(conn, code, reason);
        if (e != PROVEN_OK) return e;
    }
    if (!conn->close_received) {
        proven_ws_message_t ignored;
        proven_err_t e = wc_pump(conn, proven_net_deadline_in(conn->cfg.close_timeout_ms), &ignored, true);
        /* A peer that drops the connection once it has our close has closed, if rudely. */
        if (e == PROVEN_ERR_RESET) { conn->failed = PROVEN_OK; conn->close_received = true; e = PROVEN_ERR_EOF; }
        if (e != PROVEN_ERR_EOF) return e;
    }
    (void)proven_transport_shutdown(conn->t);
    return PROVEN_OK;
}

// -----------------------------------------------------------------------------
// Making and destroying
// -----------------------------------------------------------------------------

proven_err_t proven_ws_conn_open(proven_transport_t transport, bool is_server, proven_mem_view_t early,
                                 const proven_ws_conn_config_t *config, proven_ws_conn_t **out) {
    if (out) *out = (void *)0;
    if (!out || !config || !proven_alloc_is_valid(config->alloc) || !proven_transport_is_valid(transport) ||
        (early.size > 0 && !early.ptr)) return PROVEN_ERR_INVALID_ARG;
    if (early.size > WC_READ_SIZE) return PROVEN_ERR_OUT_OF_BOUNDS;
    proven_result_mem_mut_t m = config->alloc.alloc_fn(config->alloc.ctx, sizeof(proven_ws_conn_t), 16);
    if (!proven_is_ok(m.err)) return PROVEN_ERR_NOMEM;
    proven_ws_conn_t *c = (void *)m.value.ptr;
    /* Field by field: the struct is mostly buffers, and zeroing 20 KiB to use none of it is waste. */
    c->cfg = *config;
    if (c->cfg.max_message_bytes == 0) c->cfg.max_message_bytes = WC_DEFAULT_MAX_MESSAGE;
    if (c->cfg.io_timeout_ms == 0) c->cfg.io_timeout_ms = 30000;
    if (c->cfg.close_timeout_ms == 0) c->cfg.close_timeout_ms = 5000;
    c->t = transport;
    c->message = (void *)0;
    c->message_len = 0;
    c->message_cap = 0;
    c->read_pos = 0;
    c->read_len = early.size;
    c->pongs = 0;
    c->failed = PROVEN_OK;
    c->close_code = 0;
    c->close_reason_len = 0;
    c->protocol_len = 0;
    c->is_server = is_server;
    c->message_text = false;
    c->sending = false;
    c->close_sent = false;
    c->close_received = false;
    for (proven_size_t i = 0; i < early.size; ++i) c->read_buf[i] = early.ptr[i];
    proven_ws_decoder_init(&c->decoder, is_server, c->cfg.max_message_bytes);
    if (!is_server) {
        /* Mask keys must be unpredictable to the application's peer; one draw from the system
         * seeds a generator for the life of the connection. */
        proven_byte_t seed[PROVEN_CHACHA_SEED_SIZE];
        if (!proven_random_bytes(seed, sizeof seed)) {
            config->alloc.free_fn(config->alloc.ctx, c);
            return PROVEN_ERR_IO;
        }
        proven_chacha_rng_seed(&c->masks, seed);
    }
    *out = c;
    return PROVEN_OK;
}

void proven_ws_conn_destroy(proven_ws_conn_t *conn) {
    if (!conn) return;
    proven_allocator_t a = conn->cfg.alloc;
    (void)proven_transport_close(conn->t);
    if (conn->message) a.free_fn(a.ctx, conn->message);
    a.free_fn(a.ctx, conn);
}

static void wc_set_protocol(proven_ws_conn_t *c, proven_u8str_view_t p) {
    c->protocol_len = (proven_u8)(p.size < sizeof c->protocol ? p.size : sizeof c->protocol);
    for (proven_size_t i = 0; i < c->protocol_len; ++i) c->protocol[i] = p.ptr[i];
}

proven_err_t proven_ws_conn_accept(proven_http_exchange_t *exchange, proven_u8str_view_t protocol,
                                   const proven_ws_conn_config_t *config, proven_ws_conn_t **out) {
    if (out) *out = (void *)0;
    if (!exchange || !out || !config || !proven_alloc_is_valid(config->alloc) || protocol.size > 64) return PROVEN_ERR_INVALID_ARG;
    const proven_http_request_t *req = proven_http_exchange_request(exchange);
    proven_u8str_view_t key;
    proven_err_t e = proven_ws_check_request(req, &key);
    if (e == PROVEN_ERR_NOT_FOUND) return e;
    if (e == PROVEN_ERR_UNSUPPORTED) {
        proven_http_header_t h = { wc_lit("Sec-WebSocket-Version"), wc_lit("13") };
        (void)proven_http_exchange_respond(exchange, 426, &h, 1, (proven_mem_view_t){0});
        return e;
    }
    if (e != PROVEN_OK) {
        (void)proven_http_exchange_respond(exchange, 400, (void *)0, 0, (proven_mem_view_t){0});
        return PROVEN_ERR_INVALID_FORMAT;
    }
    if (protocol.size > 0 && !proven_ws_request_offers(req, protocol)) return PROVEN_ERR_INVALID_ARG;

    proven_byte_t accept[PROVEN_WS_ACCEPT_SIZE];
    e = proven_ws_accept_key(key, accept);
    if (e != PROVEN_OK) return e;
    proven_http_header_t headers[2] = {
        { wc_lit("Sec-WebSocket-Accept"), { .ptr = accept, .size = sizeof accept } },
        { wc_lit("Sec-WebSocket-Protocol"), protocol },
    };
    proven_transport_t t;
    proven_mem_view_t early;
    e = proven_http_exchange_upgrade(exchange, wc_lit("websocket"), headers, protocol.size > 0 ? 2 : 1, &t, &early);
    if (e != PROVEN_OK) return e;
    e = proven_ws_conn_open(t, true, early, config, out);
    if (e != PROVEN_OK) { (void)proven_transport_close(t); return e; }
    wc_set_protocol(*out, protocol);
    return PROVEN_OK;
}

proven_err_t proven_ws_conn_connect(proven_http_client_t *client, proven_u8str_view_t url,
                                    const proven_http_header_t *headers, proven_size_t header_count,
                                    proven_u8str_view_t protocols, const proven_ws_conn_config_t *config,
                                    proven_ws_conn_t **out, proven_u16 *http_status) {
    enum { MAX_EXTRA = 28, URL_MAX = 2048 };
    if (out) *out = (void *)0;
    if (http_status) *http_status = 0;
    if (!client || !out || !config || !proven_alloc_is_valid(config->alloc) || (header_count > 0 && !headers) ||
        header_count > MAX_EXTRA || url.size < 6 || url.size > URL_MAX - 2 || !url.ptr) return PROVEN_ERR_INVALID_ARG;

    /* ws and wss are http and https with another name: the same port, the same connection. */
    proven_byte_t http_url[URL_MAX];
    proven_size_t ul = 0;
    proven_size_t skip = 0;
    bool is_ws = (url.ptr[0] | 0x20) == 'w' && (url.ptr[1] | 0x20) == 's';
    if (is_ws && url.ptr[2] == ':') { skip = 2; http_url[ul++] = 'h'; http_url[ul++] = 't'; http_url[ul++] = 't'; http_url[ul++] = 'p'; }
    else if (is_ws && (url.ptr[2] | 0x20) == 's' && url.ptr[3] == ':') { skip = 3; http_url[ul++] = 'h'; http_url[ul++] = 't'; http_url[ul++] = 't'; http_url[ul++] = 'p'; http_url[ul++] = 's'; }
    for (proven_size_t i = skip; i < url.size; ++i) http_url[ul++] = url.ptr[i];

    proven_byte_t random[16], key[PROVEN_WS_KEY_SIZE];
    if (!proven_random_bytes(random, sizeof random)) return PROVEN_ERR_IO;
    proven_ws_make_key(random, key);

    proven_http_header_t all[MAX_EXTRA + 3];
    proven_size_t n = 0;
    for (proven_size_t i = 0; i < header_count; ++i) all[n++] = headers[i];
    all[n++] = (proven_http_header_t){ wc_lit("Sec-WebSocket-Key"), { .ptr = key, .size = sizeof key } };
    all[n++] = (proven_http_header_t){ wc_lit("Sec-WebSocket-Version"), wc_lit("13") };
    if (protocols.size > 0) all[n++] = (proven_http_header_t){ wc_lit("Sec-WebSocket-Protocol"), protocols };

    proven_http_client_request_t req = {
        .method = wc_lit("GET"),
        .url = { .ptr = http_url, .size = ul },
        .headers = all,
        .header_count = n,
        .upgrade = wc_lit("websocket"),
    };
    proven_http_client_response_t resp;
    proven_err_t e = proven_http_client_send(client, &req, &resp);
    if (e != PROVEN_OK) { proven_http_client_finish(&resp); return e; }
    if (http_status) *http_status = resp.status;
    if (resp.status != 101) { proven_http_client_finish(&resp); return PROVEN_ERR_REFUSED; }

    proven_http_response_t head = { .status = resp.status, .version_minor = 1, .headers = (proven_http_header_t *)resp.headers, .header_count = resp.header_count };
    proven_u8str_view_t chosen;
    e = proven_ws_check_response(&head, (proven_u8str_view_t){ .ptr = key, .size = sizeof key }, protocols, &chosen);
    proven_transport_t t = {0};
    proven_mem_view_t early = {0};
    if (e == PROVEN_OK) e = proven_http_client_upgrade(&resp, &t, &early);
    if (e == PROVEN_OK) {
        e = proven_ws_conn_open(t, false, early, config, out);
        if (e != PROVEN_OK) (void)proven_transport_close(t);
        else wc_set_protocol(*out, chosen);
    }
    proven_http_client_finish(&resp);
    return e;
}

proven_u8str_view_t proven_ws_conn_protocol(const proven_ws_conn_t *conn) {
    proven_u8str_view_t none = {0};
    return conn ? (proven_u8str_view_t){ .ptr = conn->protocol, .size = conn->protocol_len } : none;
}

proven_u16 proven_ws_conn_close_code(const proven_ws_conn_t *conn) {
    return conn ? conn->close_code : 0;
}

proven_u8str_view_t proven_ws_conn_close_reason(const proven_ws_conn_t *conn) {
    proven_u8str_view_t none = {0};
    return conn ? (proven_u8str_view_t){ .ptr = conn->close_reason, .size = conn->close_reason_len } : none;
}

proven_u64 proven_ws_conn_pong_count(const proven_ws_conn_t *conn) {
    return conn ? conn->pongs : 0;
}

#else
/* A translation unit must not be empty. */
typedef int proven_ws_conn_unused_t;
#endif
