#include "proven.h"
#include "proven_test.h"
#include "../src/proven/proven_internal_http_event.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * An HTTP/2 connection with no socket.
 *
 * Two connections of this library are wired together in memory and made to carry requests,
 * bodies of every size through windows of every size, trailers, resets and goodbyes, with the
 * bytes delivered one at a time and in bulk. Then one connection is given a peer that is a
 * script: for each rule of RFC 9113 a few frames that break it, and the verdict the RFC
 * names - ignored, an error of the stream with its code, an error of the connection with its
 * code - read from the frames the connection answers with. Each bound on what a peer may make
 * a connection hold or do is driven to its edge from both sides. What this cannot show is
 * that another implementation means the same by these frames: that is a private program's
 * evidence, against Node's http2 and curl.
 */

static proven_u8str_view_t sv(const char *s) { return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)s, .size = strlen(s) }; }
#define H(n, v) { .name = { (const proven_byte_t *)(n), sizeof(n) - 1 }, .value = { (const proven_byte_t *)(v), sizeof(v) - 1 } }
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

// -----------------------------------------------------------------------------
// An allocator that counts, and fails on request
// -----------------------------------------------------------------------------

static struct { proven_size_t bytes, blocks; long calls, fail_at; } g_mem = { .fail_at = -1 };

static proven_result_mem_mut_t mem_alloc(void *ctx, proven_size_t size, proven_size_t align) {
    (void)ctx; (void)align;
    if (g_mem.fail_at >= 0 && g_mem.calls++ == g_mem.fail_at) return (proven_result_mem_mut_t){ .err = PROVEN_ERR_NOMEM };
    proven_byte_t *p = malloc(size + 16);
    if (!p) return (proven_result_mem_mut_t){ .err = PROVEN_ERR_NOMEM };
    memcpy(p, &size, sizeof size);
    g_mem.bytes += size; g_mem.blocks++;
    return (proven_result_mem_mut_t){ .err = PROVEN_OK, .value = { .ptr = p + 16, .size = size } };
}

static void mem_free(void *ctx, void *ptr) {
    (void)ctx;
    if (!ptr) return;
    proven_byte_t *p = (proven_byte_t *)ptr - 16;
    proven_size_t size;
    memcpy(&size, p, sizeof size);
    g_mem.bytes -= size; g_mem.blocks--;
    free(p);
}

static proven_result_mem_mut_t mem_realloc(void *ctx, void *old, proven_size_t old_size, proven_size_t size, proven_size_t align) {
    proven_result_mem_mut_t m = mem_alloc(ctx, size, align);
    if (m.err != PROVEN_OK) return m;
    if (old) { memcpy(m.value.ptr, old, old_size < size ? old_size : size); mem_free(ctx, old); }
    return m;
}

static proven_allocator_t g_alloc = { .alloc_fn = mem_alloc, .realloc_fn = mem_realloc, .free_fn = mem_free };

// -----------------------------------------------------------------------------
// A scripted peer: frames built by hand, and the answers read back
// -----------------------------------------------------------------------------

typedef struct { proven_byte_t b[1 << 19]; proven_size_t n; } buf_t;
static buf_t g_buf, g_rec;

static void put_frame(buf_t *b, proven_u8 type, proven_u8 flags, proven_u32 id, const void *payload, proven_size_t len) {
    proven_h2_frame_t f = { .length = (proven_u32)len, .type = type, .flags = flags, .stream_id = id };
    PROVEN_TEST_ASSERT(proven_h2_frame_write((proven_mem_mut_t){ b->b, sizeof b->b }, &b->n, &f) == PROVEN_OK && b->n + len <= sizeof b->b, "the script fits its buffer", "");
    if (len > 0) memcpy(b->b + b->n, payload, len);
    b->n += len;
}

static void put_u32_frame(buf_t *b, proven_u8 type, proven_u32 id, proven_u32 v) {
    proven_byte_t p[4] = { (proven_byte_t)(v >> 24), (proven_byte_t)(v >> 16), (proven_byte_t)(v >> 8), (proven_byte_t)v };
    put_frame(b, type, 0, id, p, 4);
}

static void put_setting(buf_t *b, proven_u16 id, proven_u32 v) {
    proven_byte_t p[6] = { (proven_byte_t)(id >> 8), (proven_byte_t)id, (proven_byte_t)(v >> 24), (proven_byte_t)(v >> 16), (proven_byte_t)(v >> 8), (proven_byte_t)v };
    put_frame(b, PROVEN_H2_SETTINGS, 0, 0, p, 6);
}

/* A header block from an encoder with no table, so that every block stands alone. */
static proven_hpack_encoder_t *g_enc;
static proven_byte_t g_block[1 << 17];

static proven_size_t block_of(const proven_http_header_t *h, proven_size_t count) {
    proven_size_t len = 0;
    PROVEN_TEST_ASSERT(proven_hpack_encode(g_enc, h, count, (void *)0, (proven_mem_mut_t){ g_block, sizeof g_block }, &len) == PROVEN_OK, "the script's header block encodes", "");
    return len;
}

static void put_headers(buf_t *b, proven_u32 id, proven_u8 flags, const proven_http_header_t *h, proven_size_t count) {
    proven_size_t len = block_of(h, count);
    put_frame(b, PROVEN_H2_HEADERS, flags, id, g_block, len);
}

static const proven_http_header_t GET[] = { H(":method", "GET"), H(":scheme", "https"), H(":authority", "example.org"), H(":path", "/") };
static const proven_http_header_t POST[] = { H(":method", "POST"), H(":scheme", "https"), H(":authority", "example.org"), H(":path", "/in") };
static const proven_http_header_t HEADM[] = { H(":method", "HEAD"), H(":scheme", "https"), H(":authority", "example.org"), H(":path", "/") };
static const proven_http_header_t OK200[] = { H(":status", "200"), H("content-type", "text/plain") };
static const proven_http_header_t TRAIL[] = { H("x-checksum", "abc") };

#define EH PROVEN_H2_FLAG_END_HEADERS
#define ES PROVEN_H2_FLAG_END_STREAM

/* A request that is whole in one frame. */
static void put_get(buf_t *b, proven_u32 id, bool fin) {
    put_headers(b, id, (proven_u8)(EH | (fin ? ES : 0)), fin ? GET : POST, 4);
}

/* What a connection has queued for its peer, read frame by frame and marked as sent. */
typedef struct {
    int goaway; proven_u32 goaway_code, goaway_last;
    int rst; proven_u32 rst_id[64], rst_code[64];
    int acks, pongs, headers, data, continuations, settings;
    proven_byte_t pong[8];
    proven_u64 window_conn, window_streams, data_bytes;
} seen_t;

static void drain(proven_h2_conn_t *c, seen_t *s) {
    proven_mem_view_t out = proven_h2_conn_pending_output(c);
    proven_size_t pos = out.size >= 24 && memcmp(out.ptr, PROVEN_H2_PREFACE, 24) == 0 ? 24 : 0;     /* a client's first words */
    while (pos < out.size) {
        proven_h2_frame_t f;
        PROVEN_TEST_ASSERT(proven_h2_frame_parse((proven_mem_view_t){ out.ptr + pos, out.size - pos }, &f) == PROVEN_OK && pos + 9 + f.length <= out.size, "what a connection queues is whole frames", "");
        const proven_byte_t *p = out.ptr + pos + 9;
        if (s) switch (f.type) {
            case PROVEN_H2_GOAWAY: s->goaway++; s->goaway_last = ((proven_u32)p[0] << 24 | (proven_u32)p[1] << 16 | (proven_u32)p[2] << 8 | p[3]); s->goaway_code = ((proven_u32)p[4] << 24 | (proven_u32)p[5] << 16 | (proven_u32)p[6] << 8 | p[7]); break;
            case PROVEN_H2_RST_STREAM: if (s->rst < 64) { s->rst_id[s->rst] = f.stream_id; s->rst_code[s->rst] = ((proven_u32)p[0] << 24 | (proven_u32)p[1] << 16 | (proven_u32)p[2] << 8 | p[3]); } s->rst++; break;
            case PROVEN_H2_SETTINGS: if (f.flags & PROVEN_H2_FLAG_ACK) s->acks++; else s->settings++; break;
            case PROVEN_H2_PING: if (f.flags & PROVEN_H2_FLAG_ACK) { s->pongs++; memcpy(s->pong, p, 8); } break;
            case PROVEN_H2_WINDOW_UPDATE: { proven_u32 inc = ((proven_u32)p[0] << 24 | (proven_u32)p[1] << 16 | (proven_u32)p[2] << 8 | p[3]); if (f.stream_id == 0) s->window_conn += inc; else s->window_streams += inc; break; }
            case PROVEN_H2_HEADERS: s->headers++; break;
            case PROVEN_H2_CONTINUATION: s->continuations++; break;
            case PROVEN_H2_DATA: s->data++; s->data_bytes += f.length; break;
            default: break;
        }
        pos += 9 + f.length;
    }
    proven_h2_conn_output_sent(c, out.size);
}

/* What a connection reported while it was fed. */
typedef struct {
    int n, closed, headers, data, trailers, resets, writable, goaways, pongs;
    proven_u32 close_code, reset_id, reset_code, last_headers_id;
    bool reset_end;                        /* the last STREAM_RESET said the response was complete */
    proven_u64 data_bytes;
    proven_err_t err;
    proven_byte_t text[400]; proven_size_t text_len;      /* of the last GOAWAY */
} log_t;

static bool g_consume = true;              /* say at once that every DATA piece was consumed */

static void note(proven_h2_conn_t *c, const proven_h2_event_t *ev, log_t *log) {
    log->n++;
    switch (ev->kind) {
        case PROVEN_H2_EVENT_HEADERS: log->headers++; log->last_headers_id = ev->stream_id; break;
        case PROVEN_H2_EVENT_DATA: log->data++; log->data_bytes += ev->data.size; if (g_consume) proven_h2_conn_consumed(c, ev->stream_id, ev->data.size); break;
        case PROVEN_H2_EVENT_TRAILERS: log->trailers++; break;
        case PROVEN_H2_EVENT_STREAM_RESET: log->resets++; log->reset_id = ev->stream_id; log->reset_code = ev->code; log->reset_end = ev->end_stream; break;
        case PROVEN_H2_EVENT_STREAM_WRITABLE: log->writable++; break;
        case PROVEN_H2_EVENT_GOAWAY:
            log->goaways++;
            log->text_len = ev->data.size < sizeof log->text ? ev->data.size : sizeof log->text;
            if (log->text_len > 0) memcpy(log->text, ev->data.ptr, log->text_len);
            break;
        case PROVEN_H2_EVENT_PONG: log->pongs++; break;
        case PROVEN_H2_EVENT_CLOSED: log->closed++; log->close_code = ev->code; break;
        default: break;
    }
}

/* Feed bytes `chunk` at a time, taking every event. */
static void feed(proven_h2_conn_t *c, const proven_byte_t *p, proven_size_t n, proven_size_t chunk, log_t *log) {
    proven_size_t pos = 0;
    for (;;) {
        proven_size_t take = n - pos < chunk ? n - pos : chunk;
        proven_size_t used = 0;
        proven_h2_event_t ev;
        proven_err_t err = proven_h2_conn_feed(c, (proven_mem_view_t){ take ? p + pos : (void *)0, take }, &used, &ev);
        if (err != PROVEN_OK) { log->err = err; return; }
        PROVEN_TEST_ASSERT(used <= take, "a feed never takes more than it was given", "");
        pos += used;
        if (ev.kind != PROVEN_H2_EVENT_NONE) note(c, &ev, log);
        if (ev.kind == PROVEN_H2_EVENT_CLOSED) return;
        if (ev.kind == PROVEN_H2_EVENT_NONE) {
            PROVEN_TEST_ASSERT(used == take, "no event means the input was used up", "");
            if (pos == n) return;
        }
    }
}

static proven_h2_conn_t *conn_new(const proven_h2_config_t *cfg) {
    proven_h2_conn_t *c = (void *)0;
    PROVEN_TEST_ASSERT(proven_h2_conn_create(cfg, &c) == PROVEN_OK && c, "a connection is made", "");
    return c;
}

/* A server that has seen the preface and the peer's (empty) settings, or a client that has
 * seen the server's. */
static proven_h2_conn_t *opened(const proven_h2_config_t *cfg) {
    proven_h2_conn_t *c = conn_new(cfg);
    buf_t *b = &g_rec;
    b->n = 0;
    if (cfg->server) { memcpy(b->b, PROVEN_H2_PREFACE, 24); b->n = 24; }
    put_frame(b, PROVEN_H2_SETTINGS, 0, 0, (void *)0, 0);
    put_frame(b, PROVEN_H2_SETTINGS, PROVEN_H2_FLAG_ACK, 0, (void *)0, 0);
    log_t log = { 0 };
    feed(c, b->b, b->n, b->n, &log);
    PROVEN_TEST_ASSERT(log.err == PROVEN_OK && log.n == 0, "the opening is taken without an event", "");
    seen_t seen = { 0 };
    drain(c, &seen);
    PROVEN_TEST_ASSERT(seen.settings == 1 && seen.acks == 1, "a connection opens with its settings and acknowledges the peer's", "");
    return c;
}

typedef enum { V_IGNORED, V_STREAM, V_CONN } verdict_t;
static proven_h2_config_t g_cfg;           /* the configuration of the connection under the script */
static void (*g_before)(proven_h2_conn_t *c);      /* something the application does first */
static proven_size_t g_split;              /* bytes of the script fed before `g_mid` runs */
static void (*g_mid)(proven_h2_conn_t *c);

/* Run the script in g_buf against a fresh connection, in bulk and a byte at a time, and hold
 * the connection to the verdict. */
static void verdict(const char *what, verdict_t want, proven_u32 code) {
    for (int pass = 0; pass < 2; pass++) {
        proven_h2_conn_t *c = opened(&g_cfg);
        if (g_before) g_before(c);
        drain(c, (void *)0);
        log_t log = { 0 };
        seen_t seen = { 0 };
        proven_size_t split = g_mid ? g_split : 0;
        feed(c, g_buf.b, split, pass == 0 ? (split ? split : 1) : 1, &log);
        if (g_mid) { g_mid(c); drain(c, (void *)0); }      /* what the application's own act queued is not the verdict */
        if (!log.closed && log.err == PROVEN_OK) feed(c, g_buf.b + split, g_buf.n - split, pass == 0 ? g_buf.n - split + 1 : 1, &log);
        drain(c, &seen);
        const char *hint = pass == 0 ? "fed in one piece" : "fed one byte at a time";
        PROVEN_TEST_ASSERT(log.err == PROVEN_OK, what, hint);
        if (want == V_CONN) {
            PROVEN_TEST_ASSERT(seen.goaway == 1 && seen.goaway_code == code, what, hint);
            PROVEN_TEST_ASSERT(log.closed == 1 && log.close_code == code && proven_h2_conn_is_closed(c), what, hint);
            proven_size_t used = 0; proven_h2_event_t ev;
            PROVEN_TEST_ASSERT(proven_h2_conn_feed(c, (proven_mem_view_t){ g_buf.b, 9 }, &used, &ev) == PROVEN_ERR_INVALID_STATE, "after the end was reported a feed is PROVEN_ERR_INVALID_STATE", hint);
        } else if (want == V_STREAM) {
            PROVEN_TEST_ASSERT(seen.goaway == 0 && !log.closed && !proven_h2_conn_is_closed(c), what, hint);
            PROVEN_TEST_ASSERT(seen.rst >= 1 && seen.rst_code[seen.rst - 1] == code, what, hint);
        } else {
            PROVEN_TEST_ASSERT(seen.goaway == 0 && seen.rst == 0 && !log.closed && !proven_h2_conn_is_closed(c), what, hint);
        }
        proven_h2_conn_destroy(c);
    }
    g_buf.n = 0;
    g_before = (void *)0;
    g_mid = (void *)0;
}

/* The script in g_buf against a fresh connection, fed `chunk` bytes at a time: what it said.
 * Leaves the script in place, for another pass; clear() ends it. */
static log_t told(proven_size_t chunk) {
    proven_h2_conn_t *c = opened(&g_cfg);
    if (g_before) g_before(c);
    drain(c, (void *)0);
    log_t log = { 0 };
    feed(c, g_buf.b, g_buf.n, chunk, &log);
    proven_h2_conn_destroy(c);
    return log;
}

static void clear(void) { g_buf.n = 0; g_before = (void *)0; g_mid = (void *)0; }

// -----------------------------------------------------------------------------
// Two connections in memory
// -----------------------------------------------------------------------------

#define MAX_STREAMS 64
typedef struct {
    proven_h2_conn_t *client, *server;
    proven_size_t chunk;
    /* per stream, by (id - 1) / 2 */
    proven_size_t req_len[MAX_STREAMS], req_sent[MAX_STREAMS], req_got[MAX_STREAMS];
    proven_size_t resp_sent[MAX_STREAMS], resp_got[MAX_STREAMS];
    bool req_done[MAX_STREAMS], answering[MAX_STREAMS], resp_done[MAX_STREAMS], trailers[MAX_STREAMS], got_trailers[MAX_STREAMS];
    int writable, bad;
    bool plain;                            /* bodies are not the pattern: do not compare them */
    log_t clog, slog;
} pair_t;

static proven_byte_t pat(proven_u32 stream, proven_size_t i, int salt) {
    return (proven_byte_t)(i * 131u + (i >> 8) * 7u + stream * 17u + (unsigned)salt);
}

static proven_byte_t g_body[1 << 18];

static void pair_event(pair_t *p, proven_h2_conn_t *c, const proven_h2_event_t *ev) {
    const bool server = c == p->server;
    note(c, ev, server ? &p->slog : &p->clog);
    const proven_u32 k = ev->stream_id ? (ev->stream_id - 1) / 2 : 0;
    if (ev->stream_id != 0 && k >= MAX_STREAMS) { p->bad++; return; }
    switch (ev->kind) {
        case PROVEN_H2_EVENT_HEADERS:
            if (server) {
                if (ev->request.path.size == 0) p->bad++;
                if (ev->end_stream) { p->req_done[k] = true; if (p->req_got[k] != p->req_len[k]) p->bad++; }
            } else {
                if (ev->status != 200) p->bad++;
                if (ev->end_stream) p->resp_done[k] = true;
            }
            break;
        case PROVEN_H2_EVENT_DATA: {
            proven_size_t *got = server ? &p->req_got[k] : &p->resp_got[k];
            for (proven_size_t i = 0; i < ev->data.size && !p->plain; i++)
                if (ev->data.ptr[i] != pat(ev->stream_id, *got + i, server ? 1 : 2)) { p->bad++; break; }
            *got += ev->data.size;
            if (ev->end_stream) { if (server) p->req_done[k] = true; else p->resp_done[k] = true; }
            break;
        }
        case PROVEN_H2_EVENT_TRAILERS:
            if (ev->field_count != 1 || ev->fields[0].value.size != 3 || memcmp(ev->fields[0].value.ptr, "abc", 3) != 0) p->bad++;
            if (server) p->req_done[k] = true; else { p->resp_done[k] = true; p->got_trailers[k] = true; }
            break;
        case PROVEN_H2_EVENT_STREAM_WRITABLE: p->writable++; break;
        default: break;
    }
}

/* Carry what `from` has queued to `to`, and take both ends' events. */
static void carry(pair_t *p, proven_h2_conn_t *from, proven_h2_conn_t *to) {
    static proven_byte_t wire[1 << 19];
    proven_mem_view_t out = proven_h2_conn_pending_output(from);
    PROVEN_TEST_ASSERT(out.size <= sizeof wire, "queued output stays within reason", "");
    if (out.size > 0) memcpy(wire, out.ptr, out.size);
    const proven_size_t n = out.size;
    proven_h2_conn_output_sent(from, n);
    proven_size_t pos = 0;
    for (int side = 0; side < 2; side++) {
        proven_h2_conn_t *c = side == 0 ? to : from;
        const proven_size_t end = side == 0 ? n : 0;
        if (side == 1) pos = 0;
        if (proven_h2_conn_is_closed(c) && (c == p->server ? p->slog.closed : p->clog.closed)) continue;
        for (;;) {
            proven_size_t take = end - pos < p->chunk ? end - pos : p->chunk;
            proven_size_t used = 0;
            proven_h2_event_t ev;
            proven_err_t err = proven_h2_conn_feed(c, (proven_mem_view_t){ take ? wire + pos : (void *)0, take }, &used, &ev);
            if (err != PROVEN_OK) { p->bad++; break; }
            pos += used;
            if (ev.kind != PROVEN_H2_EVENT_NONE) pair_event(p, c, &ev);
            if (ev.kind == PROVEN_H2_EVENT_CLOSED) break;
            if (ev.kind == PROVEN_H2_EVENT_NONE && pos == end) break;
        }
    }
}

static void pair_open(pair_t *p, const proven_h2_config_t *ccfg, const proven_h2_config_t *scfg, proven_size_t chunk) {
    memset(p, 0, sizeof *p);
    proven_h2_config_t a = *ccfg, b = *scfg;
    a.alloc = b.alloc = g_alloc;
    a.server = false; b.server = true;
    p->client = conn_new(&a);
    p->server = conn_new(&b);
    p->chunk = chunk;
    for (int i = 0; i < 3; i++) { carry(p, p->client, p->server); carry(p, p->server, p->client); }
    PROVEN_TEST_ASSERT(p->bad == 0 && p->clog.n == 0 && p->slog.n == 0 && proven_h2_conn_pending_output(p->client).size == 0 && proven_h2_conn_pending_output(p->server).size == 0,
        "two connections exchange settings and fall silent, with no event for the application", "");
}

static void pair_close(pair_t *p) {
    proven_h2_conn_destroy(p->client);
    proven_h2_conn_destroy(p->server);
}

/* Send as much of a body as the connection takes. */
static void push_body(proven_h2_conn_t *c, proven_u32 id, proven_size_t len, proven_size_t *sent, int salt, bool with_trailers, pair_t *p) {
    if (*sent > len) return;                               /* the end has gone out */
    proven_size_t n = len - *sent;
    if (n > sizeof g_body) n = sizeof g_body;
    for (proven_size_t i = 0; i < n; i++) g_body[i] = pat(id, *sent + i, salt);
    const bool last = n == len - *sent;
    proven_result_size_t r = proven_h2_conn_send_data(c, id, (proven_mem_view_t){ g_body, n }, last && !with_trailers);
    if (r.err != PROVEN_OK || r.value > n) { p->bad++; return; }
    *sent += r.value;
    if (*sent == len && last) {
        if (with_trailers) {
            proven_err_t e = proven_h2_conn_send_trailers(c, id, TRAIL, 1);
            if (e == PROVEN_ERR_BUSY) return;
            if (e != PROVEN_OK) p->bad++;
        }
        *sent = len + 1;
    }
}

/* `count` requests with bodies of `len` bytes (stream k has len + k), each answered with a
 * body as long as the request's, all in flight together. */
static void exchange(pair_t *p, proven_u32 count, proven_size_t len, bool trailers) {
    proven_u32 ids[MAX_STREAMS];
    for (proven_u32 k = 0; k < count; k++) {
        p->req_len[k] = len + k;
        p->trailers[k] = trailers;
        PROVEN_TEST_ASSERT(proven_h2_conn_submit_request(p->client, POST, 4, false, &ids[k]) == PROVEN_OK && ids[k] == 2 * k + 1, "a request is begun and its stream numbered", "");
    }
    for (int round = 0; round < 4000000; round++) {
        bool all = true;
        for (proven_u32 k = 0; k < count; k++) {
            push_body(p->client, ids[k], p->req_len[k], &p->req_sent[k], 1, trailers, p);
            if (p->req_done[k] && !p->answering[k]) {
                proven_err_t e = proven_h2_conn_submit_response(p->server, ids[k], OK200, 2, false);
                if (e == PROVEN_OK) p->answering[k] = true; else if (e != PROVEN_ERR_BUSY) p->bad++;
            }
            if (p->answering[k]) push_body(p->server, ids[k], p->req_len[k], &p->resp_sent[k], 2, trailers, p);
            if (!p->resp_done[k]) all = false;
        }
        carry(p, p->client, p->server);
        carry(p, p->server, p->client);
        if (all || p->bad) break;
    }
    for (proven_u32 k = 0; k < count; k++) {
        PROVEN_TEST_ASSERT(p->bad == 0 && p->resp_done[k] && p->req_got[k] == p->req_len[k] && p->resp_got[k] == p->req_len[k] && p->got_trailers[k] == trailers,
            "every body arrives whole and unchanged, in both directions", "");
    }
    carry(p, p->client, p->server);
    carry(p, p->server, p->client);
    PROVEN_TEST_ASSERT(proven_h2_conn_stream_count(p->client) == 0 && proven_h2_conn_stream_count(p->server) == 0, "and no stream is left open at either end", "");
}

// -----------------------------------------------------------------------------
// Things an application does in the middle of a script
// -----------------------------------------------------------------------------

static void reset_1(proven_h2_conn_t *c) { PROVEN_TEST_ASSERT(proven_h2_conn_reset(c, 1, PROVEN_H2_CANCEL) == PROVEN_OK, "the application resets stream 1", ""); }
static void answer_1(proven_h2_conn_t *c) { PROVEN_TEST_ASSERT(proven_h2_conn_submit_response(c, 1, OK200, 2, true) == PROVEN_OK, "the application answers stream 1 in full", ""); }
static void goodbye(proven_h2_conn_t *c) { proven_h2_conn_shutdown(c, PROVEN_H2_NO_ERROR); }
static void ask_1(proven_h2_conn_t *c) { proven_u32 id; PROVEN_TEST_ASSERT(proven_h2_conn_submit_request(c, POST, 4, false, &id) == PROVEN_OK && id == 1, "the client begins stream 1", ""); }
static void ask_1_fin(proven_h2_conn_t *c) { proven_u32 id; PROVEN_TEST_ASSERT(proven_h2_conn_submit_request(c, GET, 4, true, &id) == PROVEN_OK && id == 1, "the client begins stream 1 and ends its side", ""); }
static void ask_head(proven_h2_conn_t *c) { proven_u32 id; PROVEN_TEST_ASSERT(proven_h2_conn_submit_request(c, HEADM, 4, true, &id) == PROVEN_OK && id == 1, "the client sends a HEAD request", ""); }

static proven_byte_t g_zero[70000];

/* A short conversation, used to try every allocation failing: true when it ran to its end. */
static bool conversation(void) {
    proven_h2_config_t cc = { .alloc = g_alloc }, sc = { .alloc = g_alloc, .server = true };
    proven_h2_conn_t *cl = (void *)0, *sv_ = (void *)0;
    bool ok = false;
    if (proven_h2_conn_create(&cc, &cl) != PROVEN_OK || proven_h2_conn_create(&sc, &sv_) != PROVEN_OK) goto out;
    proven_u32 id = 0;
    bool asked = false, answered = false, done = false;
    for (int round = 0; round < 40 && !done; round++) {
        if (!asked && round == 1) {
            if (proven_h2_conn_submit_request(cl, POST, 4, false, &id) != PROVEN_OK) goto out;
            proven_result_size_t r = proven_h2_conn_send_data(cl, id, (proven_mem_view_t){ g_zero, 20000 }, true);
            if (r.err != PROVEN_OK || r.value != 20000) goto out;
            asked = true;
        }
        for (int dir = 0; dir < 2; dir++) {
            proven_h2_conn_t *from = dir == 0 ? cl : sv_, *to = dir == 0 ? sv_ : cl;
            static proven_byte_t wire[1 << 17];
            proven_mem_view_t out = proven_h2_conn_pending_output(from);
            proven_size_t n = out.size, pos = 0;
            if (n > sizeof wire) goto out;
            if (n) memcpy(wire, out.ptr, n);
            proven_h2_conn_output_sent(from, n);
            for (;;) {
                proven_size_t used = 0; proven_h2_event_t ev;
                if (proven_h2_conn_feed(to, (proven_mem_view_t){ n - pos ? wire + pos : (void *)0, n - pos }, &used, &ev) != PROVEN_OK) goto out;
                pos += used;
                if (ev.kind == PROVEN_H2_EVENT_CLOSED) goto out;
                if (ev.kind == PROVEN_H2_EVENT_DATA) proven_h2_conn_consumed(to, ev.stream_id, ev.data.size);
                if (to == sv_ && (ev.kind == PROVEN_H2_EVENT_DATA || ev.kind == PROVEN_H2_EVENT_HEADERS) && ev.end_stream && !answered) {
                    if (proven_h2_conn_submit_response(sv_, ev.stream_id, OK200, 2, false) != PROVEN_OK) goto out;
                    proven_result_size_t r = proven_h2_conn_send_data(sv_, ev.stream_id, (proven_mem_view_t){ g_zero, 5000 }, true);
                    if (r.err != PROVEN_OK || r.value != 5000) goto out;
                    answered = true;
                }
                if (to == cl && ev.kind == PROVEN_H2_EVENT_DATA && ev.end_stream) done = true;
                if (ev.kind == PROVEN_H2_EVENT_NONE && pos == n) break;
            }
        }
    }
    ok = done;
out:
    proven_h2_conn_destroy(cl);
    proven_h2_conn_destroy(sv_);
    return ok;
}


/* Windows that grow (RFC-0019 section 10): a server whose clock is the test's. The opening is fed
 * at an arbitrary start and the peer's acknowledgement of its SETTINGS `rtt` microseconds later
 * (never, for 0);
 * then a request on stream 1 whose body comes in frames of 16 KiB, `step` microseconds apart, as
 * fast as the windows the server opens allow - consumed at once unless `unread`. Returns the
 * connection, open, for the caller to look at and destroy. */
static proven_u64 g_clock;
static proven_u64 test_clock(void) { return g_clock; }
static proven_h2_conn_t *grow_run(bool grow, proven_u64 rtt, proven_u64 step, proven_size_t total, bool unread, proven_size_t *sent_out) {
    static proven_byte_t piece[16384];
    const proven_u64 start = 5000000;               /* not 0: a clock's start must not stand in for a round trip */
    g_clock = start;
    const proven_h2_config_t cfg = { .alloc = g_alloc, .server = true };
    proven_h2_conn_t *c = conn_new(&cfg);
    if (grow) proven_h2_conn_grow_(c, 16384, 24576, test_clock);
    buf_t *b = &g_rec;
    b->n = 0;
    memcpy(b->b, PROVEN_H2_PREFACE, 24); b->n = 24;
    put_frame(b, PROVEN_H2_SETTINGS, 0, 0, (void *)0, 0);
    log_t log = { 0 };
    feed(c, b->b, b->n, b->n, &log);
    seen_t seen = { 0 };
    drain(c, &seen);
    if (rtt) {
        g_clock = start + rtt;
        b->n = 0;
        put_frame(b, PROVEN_H2_SETTINGS, PROVEN_H2_FLAG_ACK, 0, (void *)0, 0);
        feed(c, b->b, b->n, b->n, &log);
    }
    b->n = 0;
    put_headers(b, 1, EH, POST, 4);
    feed(c, b->b, b->n, b->n, &log);
    const bool consume = g_consume;
    g_consume = !unread;
    proven_size_t sent = 0;
    proven_u64 allowed_s = PROVEN_H2_DEFAULT_WINDOW, allowed_c = PROVEN_H2_DEFAULT_WINDOW;
    while (sent < total && log.err == PROVEN_OK && log.closed == 0) {
        proven_u64 n = total - sent < sizeof piece ? total - sent : sizeof piece;
        if (allowed_s - sent < n) n = allowed_s - sent;
        if (allowed_c - sent < n) n = allowed_c - sent;
        if (n == 0) break;                         /* held back by a window: the reader has stopped */
        g_clock += step;
        b->n = 0;
        put_frame(b, PROVEN_H2_DATA, 0, 1, piece, (proven_size_t)n);
        feed(c, b->b, b->n, b->n, &log);
        sent += (proven_size_t)n;
        drain(c, &seen);
        allowed_s = PROVEN_H2_DEFAULT_WINDOW + seen.window_streams;
        allowed_c = PROVEN_H2_DEFAULT_WINDOW + seen.window_conn;
    }
    g_consume = consume;
    PROVEN_TEST_ASSERT(log.err == PROVEN_OK && log.closed == 0 && log.resets == 0, "the body is taken within the windows the server opened", "");
    *sent_out = sent;
    return c;
}

/* The SETTINGS frames (not acknowledgements) a connection has queued: how many, and the
 * entries of the last. Marks everything as sent. */
typedef struct { int frames, entries; proven_u16 id[8]; proven_u32 value[8]; } said_t;

static said_t said(proven_h2_conn_t *c) {
    said_t r = { 0 };
    proven_mem_view_t out = proven_h2_conn_pending_output(c);
    proven_size_t pos = out.size >= 24 && memcmp(out.ptr, PROVEN_H2_PREFACE, 24) == 0 ? 24 : 0;
    while (pos + 9 <= out.size) {
        proven_h2_frame_t f;
        PROVEN_TEST_ASSERT(proven_h2_frame_parse((proven_mem_view_t){ out.ptr + pos, out.size - pos }, &f) == PROVEN_OK, "what a connection queues is whole frames", "");
        if (f.type == PROVEN_H2_SETTINGS && (f.flags & PROVEN_H2_FLAG_ACK) == 0) {
            r.frames++;
            r.entries = 0;
            proven_size_t at = 0; proven_h2_setting_t st; proven_h2_error_t code;
            while (r.entries < 8 && proven_h2_settings_next((proven_mem_view_t){ out.ptr + pos + 9, f.length }, &at, &st, &code) == PROVEN_OK) {
                r.id[r.entries] = st.id; r.value[r.entries] = st.value; r.entries++;
            }
        }
        pos += 9 + f.length;
    }
    proven_h2_conn_output_sent(c, out.size);
    return r;
}

static bool said_one(said_t r, proven_u16 id, proven_u32 value) {
    return r.frames == 1 && r.entries == 1 && r.id[0] == id && r.value[0] == value;
}

/* The peer's acknowledgement of one SETTINGS frame. */
static void ack(proven_h2_conn_t *c, log_t *log) {
    buf_t *b = &g_rec;
    b->n = 0;
    put_frame(b, PROVEN_H2_SETTINGS, PROVEN_H2_FLAG_ACK, 0, (void *)0, 0);
    feed(c, b->b, b->n, b->n, log);
}

/* `n` bytes of DATA on a stream, in frames of at most 16,384. */
static void put_data(buf_t *b, proven_u32 id, proven_size_t n) {
    while (n > 0) {
        proven_size_t k = n < 16384 ? n : 16384;
        put_frame(b, PROVEN_H2_DATA, 0, id, g_zero, k);
        n -= k;
    }
}

int main(void) {
    PROVEN_TEST_SUITE("h2_conn: an HTTP/2 connection without a socket",
        "Two connections wired together in memory carry requests, bodies, trailers, resets and goodbyes at every delivery size; a scripted peer breaks each rule of RFC 9113 and the connection answers with the verdict the RFC names; and every bound on what a peer may make a connection hold or do is driven to its edge.",
        "Inspect src/proven/h2_conn.c: h2c_on_header and h2c_on_fixed for the per-frame rules, h2c_on_block for what a header list is taken to be, h2c_stream_fault and h2c_fail for the two kinds of error, h2c_conn_credit and h2c_stream_credit for the windows.");

    PROVEN_TEST_ASSERT(proven_hpack_encoder_create(g_alloc, 0, &g_enc) == PROVEN_OK, "an encoder with no table for the scripts", "");
    const proven_h2_config_t base = { .alloc = g_alloc };

    PROVEN_TEST_SECTION("the opening", "What each end says first, and what a server does with bytes that are not HTTP/2.", "Check proven_h2_conn_create and RS_PREFACE in h2c_read.");
    {
        proven_h2_config_t cc = base;
        proven_h2_conn_t *c = conn_new(&cc);
        proven_mem_view_t out = proven_h2_conn_pending_output(c);
        proven_h2_frame_t f;
        PROVEN_TEST_ASSERT(out.size > 33 && memcmp(out.ptr, PROVEN_H2_PREFACE, 24) == 0 && proven_h2_frame_parse((proven_mem_view_t){ out.ptr + 24, 9 }, &f) == PROVEN_OK
            && f.type == PROVEN_H2_SETTINGS && f.flags == 0 && f.stream_id == 0 && out.size == 24 + 9 + f.length,
            "a client begins with the preface and its SETTINGS frame, and nothing else at the default window", "");
        /* The settings a client sends: no push, and its limits. */
        bool push_off = false; proven_size_t pos = 0; proven_h2_setting_t st; proven_h2_error_t code;
        while (proven_h2_settings_next((proven_mem_view_t){ out.ptr + 33, f.length }, &pos, &st, &code) == PROVEN_OK)
            if (st.id == PROVEN_H2_SETTINGS_ENABLE_PUSH && st.value == 0) push_off = true;
        PROVEN_TEST_ASSERT(push_off, "a client's settings forbid server push", "");
        proven_h2_conn_destroy(c);

        /* A server that takes extended CONNECT says so in its first SETTINGS; one that does not, does not. */
        for (int offer = 0; offer < 2; ++offer) {
            proven_h2_config_t sc = base;
            sc.server = true; sc.enable_connect_protocol = offer == 1;
            proven_h2_conn_t *s1 = conn_new(&sc);
            proven_mem_view_t o = proven_h2_conn_pending_output(s1);
            proven_h2_frame_t sf;
            bool said = false; pos = 0;
            PROVEN_TEST_ASSERT(proven_h2_frame_parse((proven_mem_view_t){ o.ptr, 9 }, &sf) == PROVEN_OK && sf.type == PROVEN_H2_SETTINGS, "a server's first frame is SETTINGS", "");
            while (proven_h2_settings_next((proven_mem_view_t){ o.ptr + 9, sf.length }, &pos, &st, &code) == PROVEN_OK)
                if (st.id == PROVEN_H2_SETTINGS_ENABLE_CONNECT_PROTOCOL && st.value == 1) said = true;
            PROVEN_TEST_ASSERT(said == (offer == 1), offer ? "enable_connect_protocol: the server says ENABLE_CONNECT_PROTOCOL of 1 at once" : "without it, the server says nothing of extended CONNECT", "");
            proven_h2_conn_destroy(s1);
        }
        cc.enable_connect_protocol = true;           /* on a client it is ignored: nothing changes */
        c = conn_new(&cc);
        out = proven_h2_conn_pending_output(c);
        PROVEN_TEST_ASSERT(proven_h2_frame_parse((proven_mem_view_t){ out.ptr + 24, 9 }, &f) == PROVEN_OK, "a client's SETTINGS", "");
        { bool said = false; pos = 0;
          while (proven_h2_settings_next((proven_mem_view_t){ out.ptr + 33, f.length }, &pos, &st, &code) == PROVEN_OK) if (st.id == PROVEN_H2_SETTINGS_ENABLE_CONNECT_PROTOCOL) said = true;
          PROVEN_TEST_ASSERT(!said, "a client never says ENABLE_CONNECT_PROTOCOL", ""); }
        proven_h2_conn_destroy(c);
        cc.enable_connect_protocol = false;

        cc.server = true; cc.initial_window = 1 << 20;
        c = conn_new(&cc);
        seen_t seen = { 0 };
        out = proven_h2_conn_pending_output(c);
        PROVEN_TEST_ASSERT(out.size > 9 && out.ptr[3] == PROVEN_H2_SETTINGS, "a server begins with its SETTINGS frame", "");
        drain(c, &seen);
        PROVEN_TEST_ASSERT(seen.settings == 1 && seen.window_conn == (1u << 20) - 65535u, "and a window above the default is given to the connection as well, by an update", "");
        /* Something that is not HTTP/2. */
        proven_size_t used = 7; proven_h2_event_t ev;
        PROVEN_TEST_ASSERT(proven_h2_conn_feed(c, proven_mem_view_from_u8(sv("GET / HTTP/1.1\r\n\r\n")), &used, &ev) == PROVEN_ERR_INVALID_FORMAT && used == 0,
            "a server given HTTP/1.1 says PROVEN_ERR_INVALID_FORMAT at the first byte, having taken none", "");
        PROVEN_TEST_ASSERT(proven_h2_conn_pending_output(c).size == 0 && proven_h2_conn_is_closed(c), "and says nothing to it", "");
        PROVEN_TEST_ASSERT(proven_h2_conn_feed(c, proven_mem_view_from_u8(sv("x")), &used, &ev) == PROVEN_ERR_INVALID_STATE, "after which it takes nothing", "");
        proven_h2_conn_destroy(c);
        /* The preface, wrong in its last byte, a byte at a time. */
        cc.initial_window = 0;
        c = conn_new(&cc);
        proven_byte_t pre[24]; memcpy(pre, PROVEN_H2_PREFACE, 24); pre[23] = 'x';
        log_t log = { 0 };
        feed(c, pre, 24, 1, &log);
        PROVEN_TEST_ASSERT(log.err == PROVEN_ERR_INVALID_FORMAT, "a preface wrong in its last byte is refused there", "");
        proven_h2_conn_destroy(c);

        /* Configurations that are refused. */
        proven_h2_conn_t *bad = (void *)0;
        proven_h2_config_t x = base; x.max_frame_size = 16383;
        PROVEN_TEST_ASSERT(proven_h2_conn_create(&x, &bad) == PROVEN_ERR_INVALID_ARG && !bad, "a frame size below 16,384 is refused", "");
        x = base; x.max_frame_size = (1u << 20) + 1;
        PROVEN_TEST_ASSERT(proven_h2_conn_create(&x, &bad) == PROVEN_ERR_INVALID_ARG, "and one above 1 MiB", "");
        x = base; x.initial_window = 0x80000000u;
        PROVEN_TEST_ASSERT(proven_h2_conn_create(&x, &bad) == PROVEN_ERR_INVALID_ARG, "a window above 2^31-1 is refused", "");
        x = base; x.max_header_list_bytes = (1u << 20) + 1;
        PROVEN_TEST_ASSERT(proven_h2_conn_create(&x, &bad) == PROVEN_ERR_INVALID_ARG, "a header list limit above 1 MiB is refused", "");
        x = base; x.alloc = (proven_allocator_t){ 0 };
        PROVEN_TEST_ASSERT(proven_h2_conn_create(&x, &bad) == PROVEN_ERR_INVALID_ARG, "an allocator is required", "");
        proven_h2_conn_destroy((void *)0);
    }

    PROVEN_TEST_SECTION("requests and bodies", "Bodies of every size through windows of every size, delivered a byte at a time and in bulk, many streams at once.", "Check proven_h2_conn_send_data and RS_DATA in h2c_read; h2c_can_write for PROVEN_H2_EVENT_STREAM_WRITABLE.");
    {
        static const proven_size_t sizes[] = { 0, 1, 100, 16383, 16384, 16385, 65535, 65536, 200000 };
        static const proven_u32 windows[] = { 0, 1000, 1u << 20 };
        static const proven_size_t chunks[] = { 1, 7, 1 << 19 };
        static pair_t p;
        int writable = 0;
        for (proven_size_t w = 0; w < COUNT(windows); w++) for (proven_size_t s = 0; s < COUNT(sizes); s++) for (proven_size_t k = 0; k < COUNT(chunks); k++) {
            if (chunks[k] == 1 && sizes[s] > 70000 && windows[w] == 1000) continue;        /* covered by the larger chunk; slow to no purpose */
            proven_h2_config_t cfg = base;
            cfg.initial_window = windows[w];
            pair_open(&p, &cfg, &cfg, chunks[k]);
            exchange(&p, 1, sizes[s], false);
            writable += p.writable;
            PROVEN_TEST_ASSERT(p.clog.closed == 0 && p.slog.closed == 0 && p.clog.resets == 0 && p.slog.resets == 0, "with no error on the way", "");
            pair_close(&p);
        }
        PROVEN_TEST_ASSERT(writable > 0, "a send that was cut short is told when it may go on", "");

        /* Many streams at once, with trailers, through different settings at the two ends. */
        for (proven_size_t k = 0; k < COUNT(chunks); k++) {
            proven_h2_config_t cc = base, sc = base;
            cc.max_frame_size = 65536; cc.initial_window = 30000; cc.set_header_table = true; cc.header_table_bytes = 0;
            sc.initial_window = 100000; sc.set_header_table = true; sc.header_table_bytes = 65536; sc.max_concurrent_streams = 50;
            pair_open(&p, &cc, &sc, chunks[k]);
            exchange(&p, 50, 3000, true);
            PROVEN_TEST_ASSERT(p.clog.trailers == 50 && p.slog.trailers == 50, "fifty streams interleave, each ending in trailers", "");
            /* The limit the server announced holds on the client's side. */
            proven_u32 id;
            for (int i = 0; i < 50; i++) PROVEN_TEST_ASSERT(proven_h2_conn_submit_request(p.client, POST, 4, false, &id) == PROVEN_OK, "fifty more streams are begun", "");
            PROVEN_TEST_ASSERT(proven_h2_conn_submit_request(p.client, POST, 4, false, &id) == PROVEN_ERR_BUSY && proven_h2_conn_stream_count(p.client) == 50,
                "and the fifty-first is PROVEN_ERR_BUSY: the peer said fifty", "");
            pair_close(&p);
        }
    }

    PROVEN_TEST_SECTION("what a stream may carry", "Interim responses, HEAD, a reset from either side, a response that ends before its request, user pointers.", "Check h2c_on_block, h2c_local_end and proven_h2_conn_reset.");
    {
        static pair_t p;
        pair_open(&p, &base, &base, 1 << 19);
        proven_u32 id = 0;
        int mark = 0;
        PROVEN_TEST_ASSERT(proven_h2_conn_submit_request(p.client, GET, 4, true, &id) == PROVEN_OK && id == 1, "a GET with no body", "");
        proven_h2_conn_stream_set_user(p.client, id, &mark);
        PROVEN_TEST_ASSERT(proven_h2_conn_stream_user(p.client, id) == &mark && proven_h2_conn_stream_user(p.client, 3) == (void *)0, "a pointer is attached to an open stream and to no other", "");
        carry(&p, p.client, p.server);
        PROVEN_TEST_ASSERT(p.slog.headers == 1 && p.req_done[0], "the server sees a request that is already complete", "");
        static const proven_http_header_t early[] = { H(":status", "103"), H("link", "</style.css>; rel=preload") };
        PROVEN_TEST_ASSERT(proven_h2_conn_submit_response(p.server, 1, early, 2, true) == PROVEN_ERR_INVALID_ARG, "an interim response cannot end the stream", "");
        PROVEN_TEST_ASSERT(proven_h2_conn_submit_response(p.server, 1, early, 2, false) == PROVEN_OK, "an interim response is sent", "");
        PROVEN_TEST_ASSERT(proven_h2_conn_send_data(p.server, 1, proven_mem_view_from_u8(sv("x")), false).err == PROVEN_ERR_INVALID_STATE, "no body before the final response", "");
        PROVEN_TEST_ASSERT(proven_h2_conn_submit_response(p.server, 1, OK200, 2, false) == PROVEN_OK, "then the final one", "");
        PROVEN_TEST_ASSERT(proven_h2_conn_submit_response(p.server, 1, OK200, 2, false) == PROVEN_ERR_INVALID_STATE, "and not a second", "");
        PROVEN_TEST_ASSERT(proven_h2_conn_submit_response(p.server, 3, OK200, 2, false) == PROVEN_ERR_NOT_FOUND, "nor one for a stream that is not open", "");
        PROVEN_TEST_ASSERT(proven_h2_conn_submit_response(p.client, 1, OK200, 2, false) == PROVEN_ERR_INVALID_ARG && proven_h2_conn_submit_request(p.server, GET, 4, true, &id) == PROVEN_ERR_INVALID_ARG,
            "a client does not answer and a server does not ask", "");
        PROVEN_TEST_ASSERT(proven_h2_conn_submit_request(p.client, OK200, 2, true, &id) == PROVEN_ERR_INVALID_ARG, "a request needs a request's header list", "");
        PROVEN_TEST_ASSERT(proven_h2_conn_send_room(p.server, 1) == 65536 - proven_h2_conn_pending_output(p.server).size && proven_h2_conn_send_room(p.server, 3) == 0,
            "the room to send is what the windows and the output bound leave, and none for a stream that is not open", "");
        /* The client is told of both heads; the status of the first is 103. */
        proven_mem_view_t out = proven_h2_conn_pending_output(p.server);
        static proven_byte_t wire[4096];
        memcpy(wire, out.ptr, out.size);
        proven_size_t n = out.size, used = 0;
        proven_h2_conn_output_sent(p.server, n);
        proven_h2_event_t ev;
        PROVEN_TEST_ASSERT(proven_h2_conn_feed(p.client, (proven_mem_view_t){ wire, n }, &used, &ev) == PROVEN_OK && ev.kind == PROVEN_H2_EVENT_HEADERS && ev.status == 103 && !ev.end_stream
            && ev.user == &mark && ev.field_count == 1 && ev.fields[0].name.size == 4, "the client is given the interim response, with the pointer it attached", "");
        proven_size_t more = 0;
        PROVEN_TEST_ASSERT(proven_h2_conn_feed(p.client, (proven_mem_view_t){ wire + used, n - used }, &more, &ev) == PROVEN_OK && ev.kind == PROVEN_H2_EVENT_HEADERS && ev.status == 200 && used + more == n,
            "and then the final one", "");
        PROVEN_TEST_ASSERT(proven_h2_conn_send_trailers(p.server, 1, OK200, 2) == PROVEN_ERR_INVALID_ARG, "trailers hold no pseudo-header field", "");
        PROVEN_TEST_ASSERT(proven_h2_conn_send_data(p.server, 1, (proven_mem_view_t){ (void *)0, 0 }, true).err == PROVEN_OK && proven_h2_conn_stream_count(p.server) == 0,
            "a body of nothing ends the stream", "");
        carry(&p, p.server, p.client);
        PROVEN_TEST_ASSERT(p.clog.data == 1 && p.clog.data_bytes == 0 && p.resp_done[0] && proven_h2_conn_stream_count(p.client) == 0, "and the client is told so with an empty last piece", "");

        /* A reset from the client; then one from the server. */
        PROVEN_TEST_ASSERT(proven_h2_conn_submit_request(p.client, POST, 4, false, &id) == PROVEN_OK && id == 3, "a second request", "");
        carry(&p, p.client, p.server);
        PROVEN_TEST_ASSERT(proven_h2_conn_reset(p.client, 3, PROVEN_H2_CANCEL) == PROVEN_OK && proven_h2_conn_reset(p.client, 3, PROVEN_H2_CANCEL) == PROVEN_ERR_NOT_FOUND, "the client gives it up, once", "");
        carry(&p, p.client, p.server);
        PROVEN_TEST_ASSERT(p.slog.resets == 1 && p.slog.reset_id == 3 && p.slog.reset_code == PROVEN_H2_CANCEL && proven_h2_conn_stream_count(p.server) == 0, "the server is told, with the code", "");
        p.plain = true;
        PROVEN_TEST_ASSERT(proven_h2_conn_submit_request(p.client, POST, 4, false, &id) == PROVEN_OK && id == 5, "a third request", "");
        carry(&p, p.client, p.server);
        PROVEN_TEST_ASSERT(proven_h2_conn_reset(p.server, 5, PROVEN_H2_INTERNAL_ERROR) == PROVEN_OK, "the server gives this one up", "");
        /* The client had more on the way: it is dropped at the server, and is no fault. */
        PROVEN_TEST_ASSERT(proven_h2_conn_send_data(p.client, 5, (proven_mem_view_t){ g_zero, 30000 }, false).value == 30000, "while the client was still sending", "");
        carry(&p, p.client, p.server);
        carry(&p, p.server, p.client);
        PROVEN_TEST_ASSERT(p.clog.resets == 1 && p.clog.reset_id == 5 && p.clog.reset_code == PROVEN_H2_INTERNAL_ERROR && p.slog.resets == 1 && !proven_h2_conn_is_closed(p.server),
            "the client is told, and what crossed the reset is dropped without a word", "");

        /* A response that is complete before the request's body. */
        PROVEN_TEST_ASSERT(proven_h2_conn_submit_request(p.client, POST, 4, false, &id) == PROVEN_OK && id == 7, "a fourth request, its body unfinished", "");
        carry(&p, p.client, p.server);
        PROVEN_TEST_ASSERT(proven_h2_conn_submit_response(p.server, 7, OK200, 2, true) == PROVEN_OK && proven_h2_conn_stream_count(p.server) == 0, "the server answers in full and is done with the stream", "");
        carry(&p, p.server, p.client);
        PROVEN_TEST_ASSERT(p.clog.resets == 2 && p.clog.reset_id == 7 && p.clog.reset_code == PROVEN_H2_NO_ERROR && p.clog.headers == 1 && proven_h2_conn_stream_count(p.client) == 0,
            "the client gets the response and then a reset with NO_ERROR: stop sending", "");
        PROVEN_TEST_ASSERT(p.bad == 0, "nothing unexpected happened on the way", "");

        /* A header list larger than the peer's frames goes out as HEADERS and CONTINUATION. */
        {
            static pair_t q;
            proven_h2_config_t roomy = base; roomy.max_header_list_bytes = 65536;
            static const proven_size_t pieces[] = { 1, 1 << 19 };
            for (int table = 0; table < 2; table++) for (proven_size_t piece = 0; piece < COUNT(pieces); piece++) {
                const proven_size_t chunk = pieces[piece];
                roomy.set_header_table = table == 1; roomy.header_table_bytes = 0;
                pair_open(&q, &roomy, &roomy, chunk);
                static char big[40001];
                for (proven_size_t i = 0; i < 40000; i++) big[i] = (char)('!' + (i * 7 + i / 13) % 90);     /* does not compress */
                proven_http_header_t rq[6]; memcpy(rq, GET, sizeof GET);
                rq[4] = (proven_http_header_t){ sv("x-big"), { (const proven_byte_t *)big, 40000 } }; rq[5] = (proven_http_header_t){ sv("x-after"), sv("still here") };
                proven_u32 bid;
                PROVEN_TEST_ASSERT(proven_h2_conn_submit_request(q.client, rq, 6, true, &bid) == PROVEN_OK, "a request with a field of 40,000 bytes", "");
                /* The frames it went out in. */
                proven_mem_view_t o = proven_h2_conn_pending_output(q.client);
                proven_size_t at = 0; int heads = 0, conts = 0; bool sized = true, ended = false;
                while (at < o.size) {
                    proven_h2_frame_t f;
                    PROVEN_TEST_ASSERT(proven_h2_frame_parse((proven_mem_view_t){ o.ptr + at, o.size - at }, &f) == PROVEN_OK && at + 9 + f.length <= o.size && f.stream_id == bid, "whole frames of the one stream", "");
                    if (f.type == PROVEN_H2_HEADERS) { heads++; if ((f.flags & ES) == 0 || (f.flags & EH) != 0) sized = false; }
                    else if (f.type == PROVEN_H2_CONTINUATION) { conts++; if (ended) sized = false; ended = (f.flags & EH) != 0; }
                    if (f.length > 16384 || (!ended && f.length != 16384)) sized = false;
                    at += 9 + f.length;
                }
                PROVEN_TEST_ASSERT(heads == 1 && conts == 2 && sized && ended,
                    "goes out as one HEADERS frame and two CONTINUATION frames: full frames, END_STREAM on the first, END_HEADERS on the last only", "");
                /* The receiving end sees one request, with the field whole. */
                o = proven_h2_conn_pending_output(q.client);
                static proven_byte_t w[70000];
                memcpy(w, o.ptr, o.size);
                proven_size_t wn = o.size, wpos = 0;
                proven_h2_conn_output_sent(q.client, wn);
                bool found = false;
                for (;;) {
                    proven_size_t take = wn - wpos < chunk ? wn - wpos : chunk, u = 0; proven_h2_event_t e;
                    PROVEN_TEST_ASSERT(proven_h2_conn_feed(q.server, (proven_mem_view_t){ take ? w + wpos : (void *)0, take }, &u, &e) == PROVEN_OK, "the block is taken", "");
                    wpos += u;
                    if (e.kind == PROVEN_H2_EVENT_HEADERS) {
                        found = e.end_stream && e.request.field_count == 2 && e.request.fields[0].value.size == 40000 && memcmp(e.request.fields[0].value.ptr, big, 40000) == 0
                            && e.request.fields[1].value.size == 10 && memcmp(e.request.fields[1].value.ptr, "still here", 10) == 0;
                    }
                    if (e.kind == PROVEN_H2_EVENT_NONE && wpos == wn) break;
                }
                PROVEN_TEST_ASSERT(found, "and arrives as one request with the field whole and the one after it in place", "");
                /* And the same in a response. */
                proven_http_header_t rs[3] = { OK200[0], { sv("x-big"), { (const proven_byte_t *)big, 40000 } }, { sv("x-after"), sv("still here") } };
                PROVEN_TEST_ASSERT(proven_h2_conn_submit_response(q.server, bid, rs, 3, true) == PROVEN_OK, "a response with the same field", "");
                o = proven_h2_conn_pending_output(q.server);
                memcpy(w, o.ptr, o.size); wn = o.size; wpos = 0;
                proven_h2_conn_output_sent(q.server, wn);
                found = false;
                for (;;) {
                    proven_size_t take = wn - wpos < chunk ? wn - wpos : chunk, u = 0; proven_h2_event_t e;
                    PROVEN_TEST_ASSERT(proven_h2_conn_feed(q.client, (proven_mem_view_t){ take ? w + wpos : (void *)0, take }, &u, &e) == PROVEN_OK, "the block is taken", "");
                    wpos += u;
                    if (e.kind == PROVEN_H2_EVENT_HEADERS) found = e.status == 200 && e.end_stream && e.field_count == 2 && e.fields[0].value.size == 40000 && memcmp(e.fields[0].value.ptr, big, 40000) == 0;
                    if (e.kind == PROVEN_H2_EVENT_NONE && wpos == wn) break;
                }
                PROVEN_TEST_ASSERT(found && proven_h2_conn_stream_count(q.client) == 0 && proven_h2_conn_stream_count(q.server) == 0, "arrives as one response", "");
                /* A list larger than the peer said it takes is not sent at all. */
                static char huge[70001]; memset(huge, 'a', 70000);
                rq[4].value = (proven_u8str_view_t){ (const proven_byte_t *)huge, 70000 };
                PROVEN_TEST_ASSERT(proven_h2_conn_submit_request(q.client, rq, 6, true, &bid) == PROVEN_ERR_OUT_OF_BOUNDS && proven_h2_conn_pending_output(q.client).size == 0 && proven_h2_conn_stream_count(q.client) == 0,
                    "a list larger than the peer said it accepts is PROVEN_ERR_OUT_OF_BOUNDS, with nothing queued and no stream begun", "");
                pair_close(&q);
            }
        }

        /* A ping, and its answer. */
        static const proven_byte_t eight[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
        PROVEN_TEST_ASSERT(proven_h2_conn_ping(p.client, eight) == PROVEN_OK, "a ping is sent", "");
        carry(&p, p.client, p.server);
        out = proven_h2_conn_pending_output(p.server);
        PROVEN_TEST_ASSERT(out.size == 17 && out.ptr[3] == PROVEN_H2_PING && out.ptr[4] == PROVEN_H2_FLAG_ACK && memcmp(out.ptr + 9, eight, 8) == 0, "the peer answers with the same eight bytes", "");
        memcpy(wire, out.ptr, 17); proven_h2_conn_output_sent(p.server, 17);
        PROVEN_TEST_ASSERT(proven_h2_conn_feed(p.client, (proven_mem_view_t){ wire, 17 }, &used, &ev) == PROVEN_OK && ev.kind == PROVEN_H2_EVENT_PONG && ev.data.size == 8 && memcmp(ev.data.ptr, eight, 8) == 0,
            "and the application is given them", "");
        pair_close(&p);
    }

    PROVEN_TEST_SECTION("flow control", "A receiver that does not consume stops its sender; what nobody consumes is given back.", "Check proven_h2_conn_consumed, h2c_remove (what a stream held goes back to the connection) and the padding credit in h2c_on_fixed.");
    {
        static pair_t p;
        pair_open(&p, &base, &base, 1 << 19);
        proven_u32 id;
        g_consume = false;
        p.plain = true;
        PROVEN_TEST_ASSERT(proven_h2_conn_submit_request(p.client, POST, 4, false, &id) == PROVEN_OK, "a request", "");
        carry(&p, p.client, p.server);
        proven_size_t sent = 0;
        for (int i = 0; i < 20; i++) {
            proven_result_size_t r = proven_h2_conn_send_data(p.client, id, (proven_mem_view_t){ g_zero, 70000 }, false);
            PROVEN_TEST_ASSERT(r.err == PROVEN_OK, "sending is never an error for want of room", "");
            sent += r.value;
            carry(&p, p.client, p.server);
            carry(&p, p.server, p.client);
        }
        PROVEN_TEST_ASSERT(sent == 65535 && proven_h2_conn_send_room(p.client, id) == 0 && p.slog.data_bytes == 65535 && p.clog.writable == 0,
            "a server that consumes nothing is sent exactly one window, and no more", "");
        proven_h2_conn_consumed(p.server, id, 100);
        carry(&p, p.server, p.client);
        PROVEN_TEST_ASSERT(proven_h2_conn_send_room(p.client, id) == 0, "consuming a little does not yet open the window: updates are batched", "");
        proven_h2_conn_consumed(p.server, id, 1u << 30);
        carry(&p, p.server, p.client);
        PROVEN_TEST_ASSERT(p.clog.writable == 1 && proven_h2_conn_send_room(p.client, id) == 65535, "consuming the rest does, and the sender is told once", "");
        proven_h2_conn_consumed(p.server, 99, 1000);
        pair_close(&p);

        /* What a stream held when it ended goes back to the connection. */
        pair_open(&p, &base, &base, 1 << 19);
        p.plain = true;
        for (int i = 0; i < 12; i++) {
            PROVEN_TEST_ASSERT(proven_h2_conn_submit_request(p.client, POST, 4, false, &id) == PROVEN_OK, "a request", "");
            proven_result_size_t r = proven_h2_conn_send_data(p.client, id, (proven_mem_view_t){ g_zero, 30000 }, i % 2 == 0);
            PROVEN_TEST_ASSERT(r.err == PROVEN_OK && r.value == 30000, "thirty thousand bytes go out on each of twelve streams in turn, though nobody consumes any", "");
            carry(&p, p.client, p.server);
            if (i % 2 == 0) PROVEN_TEST_ASSERT(proven_h2_conn_submit_response(p.server, id, OK200, 2, true) == PROVEN_OK, "the server answers one", "");
            else PROVEN_TEST_ASSERT(proven_h2_conn_reset(p.server, id, PROVEN_H2_CANCEL) == PROVEN_OK, "and resets the next", "");
            carry(&p, p.server, p.client);
        }
        PROVEN_TEST_ASSERT(p.bad == 0 && !proven_h2_conn_is_closed(p.client) && !proven_h2_conn_is_closed(p.server), "the connection's window did not run dry", "");
        pair_close(&p);
        g_consume = true;

        /* Before the peer has acknowledged a larger window, the default is the one in force -
         * for what may arrive and for when room is given back. */
        {
            proven_h2_config_t big = base; big.server = true; big.initial_window = 1 << 20;
            proven_h2_conn_t *c = conn_new(&big);
            g_buf.n = 24; memcpy(g_buf.b, PROVEN_H2_PREFACE, 24);
            put_frame(&g_buf, PROVEN_H2_SETTINGS, 0, 0, (void *)0, 0);     /* the client's settings, and no acknowledgement of the server's yet */
            put_get(&g_buf, 1, false);
            for (int i = 0; i < 3; i++) put_frame(&g_buf, PROVEN_H2_DATA, 0, 1, g_zero, 16384);
            put_frame(&g_buf, PROVEN_H2_DATA, 0, 1, g_zero, 16383);
            log_t log = { 0 }; seen_t seen = { 0 };
            feed(c, g_buf.b, g_buf.n, g_buf.n, &log);
            drain(c, &seen);
            PROVEN_TEST_ASSERT(!log.closed && log.data_bytes == 65535 && seen.window_streams == 32768,
                "a client that fills the default window before acknowledging a larger one is given room back at half of the default, not half of the larger", "");
            g_buf.n = 0;
            g_consume = false;
            for (int i = 0; i < 2; i++) put_frame(&g_buf, PROVEN_H2_DATA, 0, 1, g_zero, 16384);        /* exactly the room that was given back */
            feed(c, g_buf.b, g_buf.n, g_buf.n, &log);
            drain(c, &seen);
            PROVEN_TEST_ASSERT(!log.closed && seen.rst == 0, "that room can be filled", "");
            g_buf.n = 0;
            put_frame(&g_buf, PROVEN_H2_DATA, 0, 1, g_zero, 1);
            feed(c, g_buf.b, g_buf.n, g_buf.n, &log);
            seen = (seen_t){ 0 };
            drain(c, &seen);
            PROVEN_TEST_ASSERT(!log.closed && seen.rst == 1 && seen.rst_code[0] == PROVEN_H2_FLOW_CONTROL_ERROR, "and one byte past the default, before the acknowledgement, is still one too many", "");
            g_consume = true;
            proven_h2_conn_destroy(c);
            /* After the acknowledgement the larger window is in force. */
            c = opened(&big);
            g_buf.n = 0; put_get(&g_buf, 1, false);
            for (int i = 0; i < 20; i++) put_frame(&g_buf, PROVEN_H2_DATA, 0, 1, g_zero, 16384);
            log = (log_t){ 0 };
            feed(c, g_buf.b, g_buf.n, g_buf.n, &log);
            PROVEN_TEST_ASSERT(!log.closed && log.resets == 0 && log.data_bytes == 20 * 16384, "after it, a megabyte is the window", "");
            proven_h2_conn_destroy(c);
            g_buf.n = 0;
        }

        /* And the other way about: a window smaller than the default, and a client that sent
         * a frame's worth before it heard of it. */
        {
            proven_h2_config_t small = base; small.server = true; small.initial_window = 1000;
            proven_h2_conn_t *c = conn_new(&small);
            g_buf.n = 24; memcpy(g_buf.b, PROVEN_H2_PREFACE, 24);
            put_frame(&g_buf, PROVEN_H2_SETTINGS, 0, 0, (void *)0, 0);
            put_get(&g_buf, 1, false);
            put_frame(&g_buf, PROVEN_H2_DATA, 0, 1, g_zero, 16384);        /* allowed: the client still holds the default window */
            log_t log = { 0 }; seen_t seen = { 0 };
            feed(c, g_buf.b, g_buf.n, g_buf.n, &log);
            g_buf.n = 0; put_frame(&g_buf, PROVEN_H2_SETTINGS, PROVEN_H2_FLAG_ACK, 0, (void *)0, 0);
            feed(c, g_buf.b, g_buf.n, g_buf.n, &log);
            drain(c, &seen);
            PROVEN_TEST_ASSERT(!log.closed && log.data_bytes == 16384 && seen.window_streams == 16384,
                "what a client sent under the default window is given back in full once it acknowledges a smaller one: it is not left below zero, waiting", "");
            g_consume = false;
            g_buf.n = 0; put_frame(&g_buf, PROVEN_H2_DATA, 0, 1, g_zero, 1000);
            feed(c, g_buf.b, g_buf.n, g_buf.n, &log);
            seen = (seen_t){ 0 }; drain(c, &seen);
            PROVEN_TEST_ASSERT(!log.closed && seen.rst == 0, "the smaller window is then the window", "");
            g_buf.n = 0; put_frame(&g_buf, PROVEN_H2_DATA, 0, 1, g_zero, 1);
            feed(c, g_buf.b, g_buf.n, g_buf.n, &log);
            drain(c, &seen);
            PROVEN_TEST_ASSERT(!log.closed && seen.rst == 1 && seen.rst_code[0] == PROVEN_H2_FLOW_CONTROL_ERROR, "and one byte past it is one too many", "");
            g_consume = true;
            proven_h2_conn_destroy(c);
            g_buf.n = 0;
        }

        /* Padding counts against the windows and is given back by the connection itself. */
        g_cfg = base; g_cfg.server = true;
        put_get(&g_buf, 1, false);
        static proven_byte_t padded[257];
        padded[0] = 255; padded[1] = 'x';
        for (int i = 0; i < 1000; i++) put_frame(&g_buf, PROVEN_H2_DATA, PROVEN_H2_FLAG_PADDED, 1, padded, 257);
        verdict("a thousand DATA frames of one byte and 255 of padding - four windows' worth - are no error", V_IGNORED, 0);

        /* Data for a stream this side reset is dropped, and its room given back. */
        put_get(&g_buf, 1, false);
        g_split = g_buf.n; g_mid = reset_1;
        for (int i = 0; i < 20; i++) put_frame(&g_buf, PROVEN_H2_DATA, 0, 1, g_zero, 16384);
        put_get(&g_buf, 3, true);
        verdict("320,000 bytes for a stream the application reset are dropped, and the next request is served", V_IGNORED, 0);

        /* One byte past a stream's window; one byte past the connection's. */
        g_cfg.initial_window = 100;
        put_get(&g_buf, 1, false);
        put_frame(&g_buf, PROVEN_H2_DATA, 0, 1, g_zero, 100);
        g_consume = false;
        verdict("a stream's window, exactly filled, is no error", V_IGNORED, 0);
        put_get(&g_buf, 1, false);
        put_frame(&g_buf, PROVEN_H2_DATA, 0, 1, g_zero, 101);
        verdict("one byte past a stream's window is FLOW_CONTROL_ERROR for the stream", V_STREAM, PROVEN_H2_FLOW_CONTROL_ERROR);
        g_cfg.initial_window = 0;
        for (proven_u32 s = 1; s <= 7; s += 2) { put_get(&g_buf, s, false); put_frame(&g_buf, PROVEN_H2_DATA, 0, s, g_zero, 16383); }
        put_frame(&g_buf, PROVEN_H2_DATA, 0, 1, g_zero, 3);
        verdict("the connection's window, exactly filled across four streams, is no error", V_IGNORED, 0);
        for (proven_u32 s = 1; s <= 7; s += 2) { put_get(&g_buf, s, false); put_frame(&g_buf, PROVEN_H2_DATA, 0, s, g_zero, 16383); }
        put_frame(&g_buf, PROVEN_H2_DATA, 0, 1, g_zero, 4);
        verdict("one byte past the connection's window is FLOW_CONTROL_ERROR for the connection", V_CONN, PROVEN_H2_FLOW_CONTROL_ERROR);
        g_consume = true;
    }

    {
        /* Two responses at once through the connection's one window: a long one and a short
         * one. The window is given to those that wait in turn - the short one does not wait
         * for the long one to finish. */
        pair_t p;
        proven_h2_config_t cfg = { 0 };
        pair_open(&p, &cfg, &cfg, 1 << 16);
        static proven_byte_t zeros[1 << 16];
        const proven_size_t want[2] = { 4u << 20, 200000 };
        proven_size_t sent[2] = { 0, 0 }, got[2] = { 0, 0 };
        proven_u32 ids[2] = { 0, 0 };
        bool ok = proven_h2_conn_submit_request(p.client, GET, 4, true, &ids[0]) == PROVEN_OK && proven_h2_conn_submit_request(p.client, GET, 4, true, &ids[1]) == PROVEN_OK;
        carry(&p, p.client, p.server);
        for (int k = 0; k < 2 && ok; k++) ok = proven_h2_conn_submit_response(p.server, ids[k], OK200, 2, false) == PROVEN_OK;
        bool turn[2] = { true, true };
        static proven_byte_t wire[1 << 19];
        proven_size_t long_sent_when_short_ended = 0;
        for (int round = 0; round < 4000 && ok && got[1] < want[1]; round++) {
            for (int k = 0; k < 2; k++) {
                if (!turn[k] || sent[k] >= want[k]) continue;
                turn[k] = false;                                   /* until the engine says the stream may send again */
                proven_size_t n = want[k] - sent[k] < sizeof zeros ? want[k] - sent[k] : sizeof zeros;
                proven_result_size_t w = proven_h2_conn_send_data(p.server, ids[k], (proven_mem_view_t){ zeros, n }, sent[k] + n == want[k]);
                ok = ok && w.err == PROVEN_OK;
                sent[k] += w.value;
                if (w.value == n) turn[k] = true;
            }
            for (int dir = 0; dir < 2; dir++) {
                proven_h2_conn_t *from = dir == 0 ? p.server : p.client, *to = dir == 0 ? p.client : p.server;
                proven_mem_view_t out = proven_h2_conn_pending_output(from);
                const proven_size_t n = out.size;
                ok = ok && n <= sizeof wire;
                if (!ok) break;
                if (n > 0) memcpy(wire, out.ptr, n);
                proven_h2_conn_output_sent(from, n);
                for (proven_size_t pos = 0;;) {
                    proven_size_t used = 0;
                    proven_h2_event_t ev;
                    if (proven_h2_conn_feed(to, (proven_mem_view_t){ wire + pos, n - pos }, &used, &ev) != PROVEN_OK) { ok = false; break; }
                    pos += used;
                    const int k = ev.stream_id == ids[1] ? 1 : 0;
                    if (ev.kind == PROVEN_H2_EVENT_DATA) { got[k] += ev.data.size; proven_h2_conn_consumed(to, ev.stream_id, ev.data.size); }
                    else if (ev.kind == PROVEN_H2_EVENT_STREAM_WRITABLE && sent[k] < want[k]) {
                        /* As an application does: it writes when it is told it may. */
                        proven_size_t m = want[k] - sent[k] < sizeof zeros ? want[k] - sent[k] : sizeof zeros;
                        proven_result_size_t w = proven_h2_conn_send_data(to, ev.stream_id, (proven_mem_view_t){ zeros, m }, sent[k] + m == want[k]);
                        if (w.err != PROVEN_OK) { ok = false; break; }
                        sent[k] += w.value;
                        turn[k] = w.value == m;
                    }
                    else if (ev.kind == PROVEN_H2_EVENT_NONE && pos == n) break;
                }
            }
            long_sent_when_short_ended = sent[0];
        }
        PROVEN_TEST_ASSERT(ok && got[1] == want[1], "a short response beside a long one arrives whole", "");
        PROVEN_TEST_ASSERT(long_sent_when_short_ended < want[0] / 4 && long_sent_when_short_ended >= want[1] / 2,
            "while the long one is under way: the connection's window went to the two in turn, about as much to each", "");
        pair_close(&p);
    }

    PROVEN_TEST_SECTION("the verdict on each frame", "For each rule of RFC 9113 sections 5 and 6, frames that break it and the answer the RFC names.", "Check h2c_on_header, h2c_on_fixed and h2c_closed_frame against the section of the RFC the row names.");
    {
        g_cfg = base; g_cfg.server = true;
        static const proven_byte_t z8[8] = { 0 };

        /* Before anything: the first frame. */
        {
            proven_h2_conn_t *c = conn_new(&g_cfg);
            memcpy(g_buf.b, PROVEN_H2_PREFACE, 24); g_buf.n = 24;
            put_frame(&g_buf, PROVEN_H2_PING, 0, 0, z8, 8);
            log_t log = { 0 }; seen_t seen = { 0 };
            feed(c, g_buf.b, g_buf.n, 1, &log);
            drain(c, &seen);
            PROVEN_TEST_ASSERT(log.closed == 1 && log.close_code == PROVEN_H2_PROTOCOL_ERROR && seen.goaway == 1, "3.4: a first frame that is not SETTINGS is PROTOCOL_ERROR", "");
            proven_h2_conn_destroy(c);
            g_buf.n = 0;
        }

        /* Section 5.1: stream states. */
        put_frame(&g_buf, PROVEN_H2_DATA, 0, 1, "x", 1);
        verdict("5.1 idle: DATA on a stream never opened is PROTOCOL_ERROR", V_CONN, PROVEN_H2_PROTOCOL_ERROR);
        put_u32_frame(&g_buf, PROVEN_H2_RST_STREAM, 1, PROVEN_H2_CANCEL);
        verdict("5.1 idle: RST_STREAM on a stream never opened is PROTOCOL_ERROR", V_CONN, PROVEN_H2_PROTOCOL_ERROR);
        put_u32_frame(&g_buf, PROVEN_H2_WINDOW_UPDATE, 1, 10);
        verdict("5.1 idle: WINDOW_UPDATE on a stream never opened is PROTOCOL_ERROR", V_CONN, PROVEN_H2_PROTOCOL_ERROR);
        put_frame(&g_buf, PROVEN_H2_PRIORITY, 0, 1, z8, 5);
        verdict("5.1 idle: PRIORITY is allowed anywhere, and ignored", V_IGNORED, 0);
        put_get(&g_buf, 2, true);
        verdict("5.1.1: a client opening an even stream is PROTOCOL_ERROR", V_CONN, PROVEN_H2_PROTOCOL_ERROR);
        put_get(&g_buf, 5, false); put_get(&g_buf, 3, false);
        verdict("5.1.1: a stream number lower than one already used is PROTOCOL_ERROR", V_CONN, PROVEN_H2_PROTOCOL_ERROR);
        put_get(&g_buf, 1, true); put_frame(&g_buf, PROVEN_H2_DATA, 0, 1, "x", 1);
        verdict("5.1 half-closed (remote): DATA after END_STREAM is STREAM_CLOSED for the stream", V_STREAM, PROVEN_H2_STREAM_CLOSED);
        put_get(&g_buf, 1, true); put_get(&g_buf, 1, true);
        verdict("5.1 half-closed (remote): HEADERS after END_STREAM is STREAM_CLOSED for the stream", V_STREAM, PROVEN_H2_STREAM_CLOSED);
        put_get(&g_buf, 1, true); put_u32_frame(&g_buf, PROVEN_H2_WINDOW_UPDATE, 1, 10); put_frame(&g_buf, PROVEN_H2_PRIORITY, 0, 1, z8, 5);
        verdict("5.1 half-closed (remote): WINDOW_UPDATE and PRIORITY are taken", V_IGNORED, 0);
        put_get(&g_buf, 1, false); put_u32_frame(&g_buf, PROVEN_H2_RST_STREAM, 1, PROVEN_H2_CANCEL); put_frame(&g_buf, PROVEN_H2_DATA, 0, 1, "x", 1);
        verdict("5.1 closed: DATA after the peer's own RST_STREAM is STREAM_CLOSED for the stream", V_STREAM, PROVEN_H2_STREAM_CLOSED);
        put_get(&g_buf, 1, false); put_u32_frame(&g_buf, PROVEN_H2_RST_STREAM, 1, PROVEN_H2_CANCEL); put_get(&g_buf, 1, true);
        verdict("5.1 closed: HEADERS after the peer's own RST_STREAM is STREAM_CLOSED for the stream", V_STREAM, PROVEN_H2_STREAM_CLOSED);
        put_get(&g_buf, 1, false); put_u32_frame(&g_buf, PROVEN_H2_RST_STREAM, 1, PROVEN_H2_CANCEL); put_u32_frame(&g_buf, PROVEN_H2_RST_STREAM, 1, PROVEN_H2_CANCEL);
        put_u32_frame(&g_buf, PROVEN_H2_WINDOW_UPDATE, 1, 10);
        verdict("5.1 closed: a second RST_STREAM and a late WINDOW_UPDATE are ignored", V_IGNORED, 0);
        put_get(&g_buf, 1, true); g_split = g_buf.n; g_mid = answer_1;
        put_frame(&g_buf, PROVEN_H2_DATA, 0, 1, "x", 1);
        verdict("5.1 closed: DATA after both sides ended is STREAM_CLOSED for the connection", V_CONN, PROVEN_H2_STREAM_CLOSED);
        put_get(&g_buf, 1, true); g_split = g_buf.n; g_mid = answer_1;
        put_get(&g_buf, 1, true);
        verdict("5.1 closed: HEADERS after both sides ended is STREAM_CLOSED for the connection", V_CONN, PROVEN_H2_STREAM_CLOSED);
        put_get(&g_buf, 1, true); g_split = g_buf.n; g_mid = answer_1;
        put_u32_frame(&g_buf, PROVEN_H2_WINDOW_UPDATE, 1, 10); put_u32_frame(&g_buf, PROVEN_H2_RST_STREAM, 1, PROVEN_H2_CANCEL); put_frame(&g_buf, PROVEN_H2_PRIORITY, 0, 1, z8, 5);
        verdict("5.1 closed: WINDOW_UPDATE, RST_STREAM and PRIORITY after both sides ended are taken", V_IGNORED, 0);
        put_get(&g_buf, 1, false); g_split = g_buf.n; g_mid = reset_1;
        put_frame(&g_buf, PROVEN_H2_DATA, 0, 1, "x", 1); put_headers(&g_buf, 1, EH | ES, TRAIL, 1);
        verdict("5.1 closed: DATA and HEADERS that crossed this side's reset are dropped", V_IGNORED, 0);
        put_get(&g_buf, 1, false); g_split = g_buf.n; g_mid = answer_1;
        put_frame(&g_buf, PROVEN_H2_DATA, ES, 1, "x", 1);
        verdict("8.1: the rest of a request whose response is already complete is dropped", V_IGNORED, 0);

        /* A stream that depends on itself (RFC 7540, section 5.3.1; priorities are otherwise ignored). */
        { proven_size_t n = block_of(GET, 4); static proven_byte_t pl[300]; pl[0] = 0; pl[1] = 0; pl[2] = 0; pl[3] = 1; pl[4] = 16; memcpy(pl + 5, g_block, n); put_frame(&g_buf, PROVEN_H2_HEADERS, EH | ES | PROVEN_H2_FLAG_PRIORITY, 1, pl, 5 + n); put_get(&g_buf, 3, true); }
        verdict("5.3.1: HEADERS whose priority names its own stream is PROTOCOL_ERROR for the stream, and the next request is understood", V_STREAM, PROVEN_H2_PROTOCOL_ERROR);
        { static const proven_byte_t self[5] = { 0, 0, 0, 1, 16 }; put_get(&g_buf, 1, false); put_frame(&g_buf, PROVEN_H2_PRIORITY, 0, 1, self, 5); }
        verdict("5.3.1: a PRIORITY frame that names its own stream is PROTOCOL_ERROR for the stream", V_STREAM, PROVEN_H2_PROTOCOL_ERROR);
        { static const proven_byte_t other[5] = { 0x80, 0, 0, 3, 255 }; put_get(&g_buf, 1, false); put_frame(&g_buf, PROVEN_H2_PRIORITY, 0, 1, other, 5); put_frame(&g_buf, PROVEN_H2_PRIORITY, 0, 9, other, 5); }
        verdict("5.3.1: any other PRIORITY frame, for an open stream or an idle one, is ignored", V_IGNORED, 0);

        /* Section 4.2 and 6: sizes. */
        put_frame(&g_buf, PROVEN_H2_DATA, 0, 1, g_zero, 16385);
        verdict("4.2: a frame larger than this side's limit is FRAME_SIZE_ERROR", V_CONN, PROVEN_H2_FRAME_SIZE_ERROR);
        put_frame(&g_buf, 0x77, 0xff, 9, g_zero, 300);
        verdict("4.1: a frame of a type nobody knows is ignored", V_IGNORED, 0);
        put_frame(&g_buf, PROVEN_H2_PRIORITY, 0, 1, z8, 4);
        verdict("6.3: a PRIORITY frame of four bytes is FRAME_SIZE_ERROR for the stream", V_STREAM, PROVEN_H2_FRAME_SIZE_ERROR);
        put_get(&g_buf, 1, false); put_frame(&g_buf, PROVEN_H2_RST_STREAM, 0, 1, z8, 5);
        verdict("6.4: an RST_STREAM frame of five bytes is FRAME_SIZE_ERROR", V_CONN, PROVEN_H2_FRAME_SIZE_ERROR);
        put_frame(&g_buf, PROVEN_H2_RST_STREAM, 0, 0, z8, 4);
        verdict("6.4: RST_STREAM on stream 0 is PROTOCOL_ERROR", V_CONN, PROVEN_H2_PROTOCOL_ERROR);

        /* Section 6.1 and 6.2: padding. */
        put_get(&g_buf, 1, false); { proven_byte_t pd[4] = { 4, 0, 0, 0 }; put_frame(&g_buf, PROVEN_H2_DATA, PROVEN_H2_FLAG_PADDED, 1, pd, 4); }
        verdict("6.1: padding as long as the frame is PROTOCOL_ERROR", V_CONN, PROVEN_H2_PROTOCOL_ERROR);
        put_get(&g_buf, 1, false); { proven_byte_t pd[4] = { 3, 0, 0, 0 }; put_frame(&g_buf, PROVEN_H2_DATA, PROVEN_H2_FLAG_PADDED | ES, 1, pd, 4); }
        verdict("6.1: a DATA frame that is all padding is taken", V_IGNORED, 0);
        put_get(&g_buf, 1, false); put_frame(&g_buf, PROVEN_H2_DATA, PROVEN_H2_FLAG_PADDED, 1, g_zero, 0);
        verdict("6.1: a padded DATA frame with no room for the pad length is PROTOCOL_ERROR", V_CONN, PROVEN_H2_PROTOCOL_ERROR);
        { proven_size_t n = block_of(GET, 4); static proven_byte_t pl[300]; pl[0] = (proven_byte_t)(n + 1); memcpy(pl + 1, g_block, n); put_frame(&g_buf, PROVEN_H2_HEADERS, EH | ES | PROVEN_H2_FLAG_PADDED, 1, pl, 1 + n); }
        verdict("6.2: HEADERS padding longer than the frame is PROTOCOL_ERROR", V_CONN, PROVEN_H2_PROTOCOL_ERROR);
        { proven_size_t n = block_of(GET, 4); static proven_byte_t pl[300]; memset(pl, 0, sizeof pl); pl[0] = 9; pl[5] = 200; memcpy(pl + 6, g_block, n); put_frame(&g_buf, PROVEN_H2_HEADERS, EH | ES | PROVEN_H2_FLAG_PADDED | PROVEN_H2_FLAG_PRIORITY, 1, pl, 6 + n + 9); }
        verdict("6.2: HEADERS with padding and the deprecated priority fields is read past both", V_IGNORED, 0);
        put_frame(&g_buf, PROVEN_H2_HEADERS, EH | PROVEN_H2_FLAG_PRIORITY, 1, z8, 4);
        verdict("6.2: HEADERS too short for its priority fields is PROTOCOL_ERROR", V_CONN, PROVEN_H2_PROTOCOL_ERROR);

        /* Section 6.5: settings. */
        put_frame(&g_buf, PROVEN_H2_SETTINGS, PROVEN_H2_FLAG_ACK, 0, z8, 6);
        verdict("6.5: a SETTINGS acknowledgement with a payload is FRAME_SIZE_ERROR", V_CONN, PROVEN_H2_FRAME_SIZE_ERROR);
        put_frame(&g_buf, PROVEN_H2_SETTINGS, 0, 0, z8, 5);
        verdict("6.5: a SETTINGS frame of five bytes is FRAME_SIZE_ERROR", V_CONN, PROVEN_H2_FRAME_SIZE_ERROR);
        put_frame(&g_buf, PROVEN_H2_SETTINGS, 0, 1, z8, 0);
        verdict("6.5: SETTINGS on a stream is PROTOCOL_ERROR", V_CONN, PROVEN_H2_PROTOCOL_ERROR);
        put_setting(&g_buf, PROVEN_H2_SETTINGS_ENABLE_PUSH, 2);
        verdict("6.5.2: ENABLE_PUSH of 2 is PROTOCOL_ERROR", V_CONN, PROVEN_H2_PROTOCOL_ERROR);
        put_setting(&g_buf, PROVEN_H2_SETTINGS_ENABLE_PUSH, 1);
        verdict("6.5.2: a client may say it accepts push; a server that never pushes takes it", V_IGNORED, 0);
        put_setting(&g_buf, PROVEN_H2_SETTINGS_ENABLE_CONNECT_PROTOCOL, 2);
        verdict("RFC 8441, 3: ENABLE_CONNECT_PROTOCOL of 2 is PROTOCOL_ERROR", V_CONN, PROVEN_H2_PROTOCOL_ERROR);
        g_cfg.server = false;
        put_setting(&g_buf, PROVEN_H2_SETTINGS_ENABLE_CONNECT_PROTOCOL, 1); put_setting(&g_buf, PROVEN_H2_SETTINGS_ENABLE_CONNECT_PROTOCOL, 1);
        verdict("RFC 8441, 3: a server may say ENABLE_CONNECT_PROTOCOL of 1, and say it again", V_IGNORED, 0);
        put_setting(&g_buf, PROVEN_H2_SETTINGS_ENABLE_CONNECT_PROTOCOL, 1); put_setting(&g_buf, PROVEN_H2_SETTINGS_ENABLE_CONNECT_PROTOCOL, 0);
        verdict("RFC 8441, 3: and may not take it back: 1 then 0 is PROTOCOL_ERROR", V_CONN, PROVEN_H2_PROTOCOL_ERROR);
        g_cfg.server = true;
        put_setting(&g_buf, PROVEN_H2_SETTINGS_INITIAL_WINDOW_SIZE, 0x80000000u);
        verdict("6.5.2: an initial window above 2^31-1 is FLOW_CONTROL_ERROR", V_CONN, PROVEN_H2_FLOW_CONTROL_ERROR);
        put_setting(&g_buf, PROVEN_H2_SETTINGS_MAX_FRAME_SIZE, 16383);
        verdict("6.5.2: a frame size below 16,384 is PROTOCOL_ERROR", V_CONN, PROVEN_H2_PROTOCOL_ERROR);
        put_setting(&g_buf, PROVEN_H2_SETTINGS_MAX_FRAME_SIZE, 1u << 24);
        verdict("6.5.2: a frame size above 2^24-1 is PROTOCOL_ERROR", V_CONN, PROVEN_H2_PROTOCOL_ERROR);
        put_setting(&g_buf, 0x99, 12345); put_setting(&g_buf, 0x08, 1);
        verdict("6.5.2: settings nobody knows are ignored", V_IGNORED, 0);
        put_get(&g_buf, 1, false); put_u32_frame(&g_buf, PROVEN_H2_WINDOW_UPDATE, 1, 0x7fffffffu - 65535u); put_setting(&g_buf, PROVEN_H2_SETTINGS_INITIAL_WINDOW_SIZE, 65536);
        verdict("6.9.2: a settings change that takes a stream's window past 2^31-1 is FLOW_CONTROL_ERROR", V_CONN, PROVEN_H2_FLOW_CONTROL_ERROR);

        /* Section 6.6: push. */
        put_frame(&g_buf, PROVEN_H2_PUSH_PROMISE, EH, 1, z8, 4);
        verdict("6.6: PUSH_PROMISE from a client is PROTOCOL_ERROR", V_CONN, PROVEN_H2_PROTOCOL_ERROR);

        /* Section 6.7: ping. */
        put_frame(&g_buf, PROVEN_H2_PING, 0, 1, z8, 8);
        verdict("6.7: PING on a stream is PROTOCOL_ERROR", V_CONN, PROVEN_H2_PROTOCOL_ERROR);
        put_frame(&g_buf, PROVEN_H2_PING, 0, 0, z8, 7);
        verdict("6.7: a PING of seven bytes is FRAME_SIZE_ERROR", V_CONN, PROVEN_H2_FRAME_SIZE_ERROR);

        /* Section 6.8: goodbye. */
        put_frame(&g_buf, PROVEN_H2_GOAWAY, 0, 1, z8, 8);
        verdict("6.8: GOAWAY on a stream is PROTOCOL_ERROR", V_CONN, PROVEN_H2_PROTOCOL_ERROR);
        put_frame(&g_buf, PROVEN_H2_GOAWAY, 0, 0, z8, 7);
        verdict("6.8: a GOAWAY of seven bytes is FRAME_SIZE_ERROR", V_CONN, PROVEN_H2_FRAME_SIZE_ERROR);

        /* Section 6.9: window updates. */
        put_u32_frame(&g_buf, PROVEN_H2_WINDOW_UPDATE, 0, 0);
        verdict("6.9: an increment of zero for the connection is PROTOCOL_ERROR", V_CONN, PROVEN_H2_PROTOCOL_ERROR);
        put_get(&g_buf, 1, false); put_u32_frame(&g_buf, PROVEN_H2_WINDOW_UPDATE, 1, 0);
        verdict("6.9: an increment of zero for a stream is PROTOCOL_ERROR for the stream", V_STREAM, PROVEN_H2_PROTOCOL_ERROR);
        put_u32_frame(&g_buf, PROVEN_H2_WINDOW_UPDATE, 0, 0x7fffffffu - 65535u);
        verdict("6.9.1: the connection's window at exactly 2^31-1 is taken", V_IGNORED, 0);
        put_u32_frame(&g_buf, PROVEN_H2_WINDOW_UPDATE, 0, 0x7fffffffu - 65534u);
        verdict("6.9.1: the connection's window past 2^31-1 is FLOW_CONTROL_ERROR", V_CONN, PROVEN_H2_FLOW_CONTROL_ERROR);
        put_get(&g_buf, 1, false); put_u32_frame(&g_buf, PROVEN_H2_WINDOW_UPDATE, 1, 0x7fffffffu - 65534u);
        verdict("6.9.1: a stream's window past 2^31-1 is FLOW_CONTROL_ERROR for the stream", V_STREAM, PROVEN_H2_FLOW_CONTROL_ERROR);
        put_frame(&g_buf, PROVEN_H2_WINDOW_UPDATE, 0, 0, z8, 3);
        verdict("6.9: a WINDOW_UPDATE of three bytes is FRAME_SIZE_ERROR", V_CONN, PROVEN_H2_FRAME_SIZE_ERROR);

        /* Section 6.10: header blocks in pieces. */
        put_frame(&g_buf, PROVEN_H2_CONTINUATION, EH, 1, z8, 0);
        verdict("6.10: CONTINUATION with no HEADERS before it is PROTOCOL_ERROR", V_CONN, PROVEN_H2_PROTOCOL_ERROR);
        { proven_size_t n = block_of(GET, 4);
          put_frame(&g_buf, PROVEN_H2_HEADERS, ES, 1, g_block, 3); put_frame(&g_buf, PROVEN_H2_CONTINUATION, 0, 1, g_block + 3, 1); put_frame(&g_buf, PROVEN_H2_CONTINUATION, 0, 1, g_block + 4, 0);
          put_frame(&g_buf, PROVEN_H2_CONTINUATION, EH, 1, g_block + 4, n - 4); }
        verdict("6.10: a header block in four frames is one request", V_IGNORED, 0);
        { (void)block_of(GET, 4); put_frame(&g_buf, PROVEN_H2_HEADERS, ES, 1, g_block, 3); put_frame(&g_buf, PROVEN_H2_PING, 0, 0, z8, 8); }
        verdict("6.10: a PING inside a header block is PROTOCOL_ERROR", V_CONN, PROVEN_H2_PROTOCOL_ERROR);
        { (void)block_of(GET, 4); put_frame(&g_buf, PROVEN_H2_HEADERS, ES, 1, g_block, 3); put_frame(&g_buf, PROVEN_H2_CONTINUATION, EH, 3, g_block + 3, 1); }
        verdict("6.10: CONTINUATION for another stream is PROTOCOL_ERROR", V_CONN, PROVEN_H2_PROTOCOL_ERROR);
        { (void)block_of(GET, 4); put_frame(&g_buf, PROVEN_H2_HEADERS, ES, 1, g_block, 3); put_frame(&g_buf, 0x77, 0, 1, z8, 1); }
        verdict("6.10: a frame of an unknown type inside a header block is PROTOCOL_ERROR", V_CONN, PROVEN_H2_PROTOCOL_ERROR);
        { proven_size_t n = block_of(GET, 4); put_frame(&g_buf, PROVEN_H2_HEADERS, EH, 1, g_block, n); put_frame(&g_buf, PROVEN_H2_CONTINUATION, EH, 1, z8, 0); }
        verdict("6.10: CONTINUATION after a block that had ended is PROTOCOL_ERROR", V_CONN, PROVEN_H2_PROTOCOL_ERROR);

        /* Section 4.3: compression. */
        { static const proven_byte_t badidx[] = { 0xff, 0xff, 0x7f }; put_frame(&g_buf, PROVEN_H2_HEADERS, EH | ES, 1, badidx, 3); }
        verdict("4.3: a header block that is not HPACK is COMPRESSION_ERROR", V_CONN, PROVEN_H2_COMPRESSION_ERROR);
        { static const proven_byte_t badidx[] = { 0xff, 0xff, 0x7f }; put_get(&g_buf, 1, false); g_split = g_buf.n; g_mid = reset_1; put_frame(&g_buf, PROVEN_H2_HEADERS, EH | ES, 1, badidx, 3); }
        verdict("4.3: and so is one for a stream this side had reset: every block is decoded", V_CONN, PROVEN_H2_COMPRESSION_ERROR);

        /* Section 8: what a header list must be. */
        { static const proven_http_header_t h[] = { H(":method", "GET"), H(":scheme", "https"), H(":path", "/"), H("Accept", "x") }; put_headers(&g_buf, 1, EH | ES, h, 4); }
        verdict("8.2.1: a field name with an uppercase letter is PROTOCOL_ERROR for the stream", V_STREAM, PROVEN_H2_PROTOCOL_ERROR);
        { static const proven_http_header_t h[] = { H(":method", "GET"), H(":scheme", "https") }; put_headers(&g_buf, 1, EH | ES, h, 2); }
        verdict("8.3.1: a request with no :path is PROTOCOL_ERROR for the stream", V_STREAM, PROVEN_H2_PROTOCOL_ERROR);
        { static const proven_http_header_t h[] = { H(":method", "CONNECT"), H(":protocol", "websocket"), H(":scheme", "https"), H(":authority", "example.org"), H(":path", "/chat") };
          put_headers(&g_buf, 1, EH, h, 5); }
        verdict("RFC 8441, 4: an extended CONNECT to a server that did not offer it is PROTOCOL_ERROR for the stream", V_STREAM, PROVEN_H2_PROTOCOL_ERROR);
        g_cfg.enable_connect_protocol = true;
        { static const proven_http_header_t h[] = { H(":method", "CONNECT"), H(":protocol", "websocket"), H(":scheme", "https"), H(":authority", "example.org"), H(":path", "/chat") };
          put_headers(&g_buf, 1, EH, h, 5); put_frame(&g_buf, PROVEN_H2_DATA, 0, 1, "abc", 3); }
        verdict("RFC 8441, 4: to one that did, it is taken, and data follows on the stream", V_IGNORED, 0);
        { static const proven_http_header_t h[] = { H(":method", "CONNECT"), H(":protocol", "websocket"), H(":scheme", "https"), H(":authority", "example.org") };
          put_headers(&g_buf, 1, EH, h, 4); }
        verdict("RFC 8441, 4: but not without :path", V_STREAM, PROVEN_H2_PROTOCOL_ERROR);
        g_cfg.enable_connect_protocol = false;
        { static const proven_http_header_t h[] = { H(":method", "GET"), H(":scheme", "https"), H(":path", "/"), H("connection", "close") }; put_headers(&g_buf, 1, EH | ES, h, 4); }
        verdict("8.2.2: a connection field is PROTOCOL_ERROR for the stream", V_STREAM, PROVEN_H2_PROTOCOL_ERROR);
        { static const proven_http_header_t h[] = { H(":method", "POST"), H(":scheme", "https"), H(":path", "/"), H("content-length", "3") };
          put_headers(&g_buf, 1, EH, h, 4); put_frame(&g_buf, PROVEN_H2_DATA, ES, 1, "abc", 3); }
        verdict("8.1.1: a body of the length it announced is taken", V_IGNORED, 0);
        { static const proven_http_header_t h[] = { H(":method", "POST"), H(":scheme", "https"), H(":path", "/"), H("content-length", "3") };
          put_headers(&g_buf, 1, EH, h, 4); put_frame(&g_buf, PROVEN_H2_DATA, ES, 1, "ab", 2); }
        verdict("8.1.1: a body shorter than it announced is PROTOCOL_ERROR for the stream", V_STREAM, PROVEN_H2_PROTOCOL_ERROR);
        { static const proven_http_header_t h[] = { H(":method", "POST"), H(":scheme", "https"), H(":path", "/"), H("content-length", "3") };
          put_headers(&g_buf, 1, EH, h, 4); put_frame(&g_buf, PROVEN_H2_DATA, 0, 1, "abcd", 4); }
        verdict("8.1.1: a body longer than it announced is PROTOCOL_ERROR for the stream, at the byte that passes it", V_STREAM, PROVEN_H2_PROTOCOL_ERROR);
        { static const proven_http_header_t h[] = { H(":method", "POST"), H(":scheme", "https"), H(":path", "/"), H("content-length", "3") }; put_headers(&g_buf, 1, EH | ES, h, 4); }
        verdict("8.1.1: a request that announces a body and ends without one is PROTOCOL_ERROR for the stream", V_STREAM, PROVEN_H2_PROTOCOL_ERROR);
        { static const proven_http_header_t h[] = { H(":method", "POST"), H(":scheme", "https"), H(":path", "/"), H("content-length", "3"), H("content-length", "4") }; put_headers(&g_buf, 1, EH, h, 5); }
        verdict("8.1.1: two lengths that disagree are PROTOCOL_ERROR for the stream", V_STREAM, PROVEN_H2_PROTOCOL_ERROR);
        { static const proven_http_header_t h[] = { H(":method", "POST"), H(":scheme", "https"), H(":path", "/"), H("content-length", "1x") }; put_headers(&g_buf, 1, EH, h, 4); }
        verdict("8.1.1: a length that is not a number is PROTOCOL_ERROR for the stream", V_STREAM, PROVEN_H2_PROTOCOL_ERROR);
        put_get(&g_buf, 1, false); put_headers(&g_buf, 1, EH, TRAIL, 1);
        verdict("8.1: trailers that do not end the stream are PROTOCOL_ERROR for the stream", V_STREAM, PROVEN_H2_PROTOCOL_ERROR);
        put_get(&g_buf, 1, false); put_headers(&g_buf, 1, EH | ES, GET, 4);
        verdict("8.1: trailers with a pseudo-header field are PROTOCOL_ERROR for the stream", V_STREAM, PROVEN_H2_PROTOCOL_ERROR);
        put_get(&g_buf, 1, false); put_frame(&g_buf, PROVEN_H2_DATA, 0, 1, "abc", 3); put_headers(&g_buf, 1, EH | ES, TRAIL, 1);
        verdict("8.1: a request, a body and trailers are taken", V_IGNORED, 0);

        /* A header list over the limit ends its stream, and the next request is still understood. */
        g_cfg.max_header_list_bytes = 200;
        { static char big[300]; memset(big, 'a', sizeof big - 1);
          proven_http_header_t h[5]; memcpy(h, GET, sizeof GET); h[4] = (proven_http_header_t){ sv("x-long"), sv(big) };
          put_headers(&g_buf, 1, EH | ES, h, 5); put_get(&g_buf, 3, true); }
        verdict("10.5.1: a header list over the limit is PROTOCOL_ERROR for its stream only", V_STREAM, PROVEN_H2_PROTOCOL_ERROR);
        { (void)block_of(GET, 4); put_frame(&g_buf, PROVEN_H2_HEADERS, ES, 1, g_block, 3);
          for (int i = 0; i < 3; i++) put_frame(&g_buf, PROVEN_H2_CONTINUATION, 0, 1, g_zero, 200); }
        verdict("10.5.1: a header block of more than twice the limit ends the connection: it cannot be decoded, nor skipped", V_CONN, PROVEN_H2_ENHANCE_YOUR_CALM);
        g_cfg.max_header_list_bytes = 0;

        /* The client's side of the same rules. */
        g_cfg.server = false;
        put_headers(&g_buf, 1, EH | ES, OK200, 2);
        verdict("client: a response on a stream it never opened is PROTOCOL_ERROR", V_CONN, PROVEN_H2_PROTOCOL_ERROR);
        put_headers(&g_buf, 2, EH | ES, OK200, 2);
        verdict("client: a response on an even stream is PROTOCOL_ERROR", V_CONN, PROVEN_H2_PROTOCOL_ERROR);
        put_setting(&g_buf, PROVEN_H2_SETTINGS_ENABLE_PUSH, 1);
        verdict("client: a server that sets ENABLE_PUSH is PROTOCOL_ERROR", V_CONN, PROVEN_H2_PROTOCOL_ERROR);
        g_before = ask_1; put_frame(&g_buf, PROVEN_H2_PUSH_PROMISE, EH, 1, z8, 4);
        verdict("client: PUSH_PROMISE, which its settings forbade, is PROTOCOL_ERROR", V_CONN, PROVEN_H2_PROTOCOL_ERROR);
        g_before = ask_1; put_frame(&g_buf, PROVEN_H2_DATA, 0, 1, "x", 1);
        verdict("client: DATA before a response head is PROTOCOL_ERROR for the stream", V_STREAM, PROVEN_H2_PROTOCOL_ERROR);
        g_before = ask_1; put_headers(&g_buf, 1, EH, GET, 4);
        verdict("client: a response with no :status is PROTOCOL_ERROR for the stream", V_STREAM, PROVEN_H2_PROTOCOL_ERROR);
        { static const proven_http_header_t h[] = { H(":status", "100") }; g_before = ask_1; put_headers(&g_buf, 1, EH | ES, h, 1); }
        verdict("client: an interim response that ends the stream is PROTOCOL_ERROR for the stream", V_STREAM, PROVEN_H2_PROTOCOL_ERROR);
        { static const proven_http_header_t h[] = { H(":status", "101") }; g_before = ask_1; put_headers(&g_buf, 1, EH, h, 1); }
        verdict("client: 101, which HTTP/2 does not have, is PROTOCOL_ERROR for the stream", V_STREAM, PROVEN_H2_PROTOCOL_ERROR);
        { static const proven_http_header_t h[] = { H(":status", "100") }; g_before = ask_1; put_headers(&g_buf, 1, EH, h, 1); put_headers(&g_buf, 1, EH, h, 1); put_headers(&g_buf, 1, EH, OK200, 2);
          put_frame(&g_buf, PROVEN_H2_DATA, 0, 1, "abc", 3); put_headers(&g_buf, 1, EH | ES, TRAIL, 1); }
        verdict("client: two interim responses, a final one, a body and trailers are taken", V_IGNORED, 0);
        { static const proven_http_header_t h[] = { H(":status", "200"), H("content-length", "1234") }; g_before = ask_head; put_headers(&g_buf, 1, EH | ES, h, 2); }
        verdict("client: the response to HEAD announces a length and carries no body", V_IGNORED, 0);
        { static const proven_http_header_t h[] = { H(":status", "304"), H("content-length", "1234") }; g_before = ask_1_fin; put_headers(&g_buf, 1, EH | ES, h, 2); }
        verdict("client: so does 304", V_IGNORED, 0);
        { static const proven_http_header_t h[] = { H(":status", "200"), H("content-length", "1234") }; g_before = ask_1_fin; put_headers(&g_buf, 1, EH | ES, h, 2); }
        verdict("client: any other response that announces a body and has none is PROTOCOL_ERROR for the stream", V_STREAM, PROVEN_H2_PROTOCOL_ERROR);
        g_before = ask_1_fin; put_headers(&g_buf, 1, EH | ES, OK200, 2); put_frame(&g_buf, PROVEN_H2_DATA, 0, 1, "x", 1);
        verdict("client: DATA after both sides ended is STREAM_CLOSED for the connection", V_CONN, PROVEN_H2_STREAM_CLOSED);
        g_before = ask_1; put_headers(&g_buf, 1, EH | ES, OK200, 2); put_u32_frame(&g_buf, PROVEN_H2_RST_STREAM, 1, PROVEN_H2_NO_ERROR);
        verdict("client: a complete response and then RST_STREAM with NO_ERROR is an ordinary end", V_IGNORED, 0);

        /* What a reset with NO_ERROR says of the response: complete (end_stream) after its
         * END_STREAM, or after every byte its content-length declared; otherwise not. Each
         * script fed whole and a byte at a time. */
        {
            static const proven_http_header_t L5[] = { H(":status", "200"), H("content-length", "5") };
            static const proven_http_header_t L0[] = { H(":status", "200"), H("content-length", "0") };
            static const proven_http_header_t N0[] = { H(":status", "204"), H("content-length", "0") };
            struct { const char *what; void (*before)(proven_h2_conn_t *); const proven_http_header_t *head; const char *body; proven_size_t pieces; proven_u32 code; bool end; } cases[] = {
                { "after END_STREAM", ask_1, (void *)0, "", 0, PROVEN_H2_NO_ERROR, true },
                { "every declared byte, in one piece", ask_1, L5, "hello", 1, PROVEN_H2_NO_ERROR, true },
                { "every declared byte, in two pieces", ask_1, L5, "hello", 2, PROVEN_H2_NO_ERROR, true },
                { "a declared length of zero and no body", ask_1, L0, "", 0, PROVEN_H2_NO_ERROR, true },
                { "one byte short", ask_1, L5, "hell", 1, PROVEN_H2_NO_ERROR, false },
                { "no declared length", ask_1, OK200, "hello", 1, PROVEN_H2_NO_ERROR, false },
                { "every declared byte, but CANCEL", ask_1, L5, "hello", 1, PROVEN_H2_CANCEL, false },
                { "every declared byte, but INTERNAL_ERROR", ask_1, L5, "hello", 1, PROVEN_H2_INTERNAL_ERROR, false },
                { "a response to HEAD, whose length describes no body", ask_head, L5, "", 0, PROVEN_H2_NO_ERROR, false },
                { "204, whose length describes no body either", ask_1, N0, "", 0, PROVEN_H2_NO_ERROR, false },
            };
            proven_size_t right = 0, tried = 0;
            for (proven_size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
                for (int pass = 0; pass < 2; pass++) {
                    g_before = cases[i].before;
                    if (!cases[i].head) put_headers(&g_buf, 1, EH | ES, OK200, 2);
                    else {
                        put_headers(&g_buf, 1, EH, cases[i].head, 2);
                        const proven_size_t n = strlen(cases[i].body), half = cases[i].pieces == 2 ? n / 2 : n;
                        if (cases[i].pieces >= 1) put_frame(&g_buf, PROVEN_H2_DATA, 0, 1, cases[i].body, half);
                        if (cases[i].pieces == 2) put_frame(&g_buf, PROVEN_H2_DATA, 0, 1, cases[i].body + half, n - half);
                    }
                    put_u32_frame(&g_buf, PROVEN_H2_RST_STREAM, 1, cases[i].code);
                    log_t l = told(pass == 0 ? g_buf.n + 1 : 1);
                    tried++;
                    if (l.err == PROVEN_OK && l.resets == 1 && l.reset_code == cases[i].code && l.reset_end == cases[i].end && !l.closed) right++;
                    else PROVEN_TEST_ASSERT(false, cases[i].what, pass == 0 ? "fed in one piece" : "fed one byte at a time");
                    clear();
                }
            }
            PROVEN_TEST_ASSERT(tried == 20 && right == tried,
                "client: a reset with NO_ERROR says the response is complete after END_STREAM or every declared byte (in one piece or two, and a length of zero) - and not one byte short, without a length, with another code, or after HEAD or a 204", "");
        }
        g_cfg.server = true;
    }

    PROVEN_TEST_SECTION("what a peer may do without progress", "Each bound driven to its edge: one under passes, one over ends the connection with ENHANCE_YOUR_CALM.", "Check h2c_debt, h2c_ctl, H2C_EMPTY_RUN_MAX and the refusal of streams in h2c_on_block.");
    {
        g_cfg = base; g_cfg.server = true;
        /* Rapid reset: a hundred is the default allowance. */
        for (proven_u32 i = 0; i < 100; i++) { put_get(&g_buf, 2 * i + 1, false); put_u32_frame(&g_buf, PROVEN_H2_RST_STREAM, 2 * i + 1, PROVEN_H2_CANCEL); }
        verdict("a hundred streams opened and reset at once are borne", V_IGNORED, 0);
        for (proven_u32 i = 0; i < 101; i++) { put_get(&g_buf, 2 * i + 1, false); put_u32_frame(&g_buf, PROVEN_H2_RST_STREAM, 2 * i + 1, PROVEN_H2_CANCEL); }
        verdict("the hundred and first ends the connection with ENHANCE_YOUR_CALM", V_CONN, PROVEN_H2_ENHANCE_YOUR_CALM);
        /* NO_ERROR is an end only from a server: a client's requests reset that way count the same. */
        for (proven_u32 i = 0; i < 101; i++) { put_get(&g_buf, 2 * i + 1, true); put_u32_frame(&g_buf, PROVEN_H2_RST_STREAM, 2 * i + 1, PROVEN_H2_NO_ERROR); }
        verdict("a hundred and one requests, each ended and then reset with NO_ERROR, end the connection the same way", V_CONN, PROVEN_H2_ENHANCE_YOUR_CALM);
        put_get(&g_buf, 1, true); put_u32_frame(&g_buf, PROVEN_H2_RST_STREAM, 1, PROVEN_H2_NO_ERROR);
        {
            log_t l = told(g_buf.n + 1);
            PROVEN_TEST_ASSERT(l.resets == 1 && l.reset_code == PROVEN_H2_NO_ERROR && !l.reset_end, "and a server's reset event never says a response is complete (end_stream stays false)", "");
            clear();
        }
        g_cfg.max_reset_debt = 3;
        for (proven_u32 i = 0; i < 3; i++) { put_get(&g_buf, 2 * i + 1, false); put_u32_frame(&g_buf, PROVEN_H2_RST_STREAM, 2 * i + 1, PROVEN_H2_CANCEL); }
        { static const proven_http_header_t h[] = { H(":method", "GET") }; put_headers(&g_buf, 7, EH | ES, h, 1); }
        verdict("a stream this side must reset for the peer's fault counts the same as one the peer reset", V_CONN, PROVEN_H2_ENHANCE_YOUR_CALM);
        /* A stream that ends properly takes one off. */
        {
            proven_h2_conn_t *c = opened(&g_cfg);
            log_t log = { 0 };
            proven_u32 id = 1;
            for (int round = 0; round < 50 && !log.closed; round++) {
                for (int i = 0; i < 3; i++, id += 2) { g_buf.n = 0; put_get(&g_buf, id, false); put_u32_frame(&g_buf, PROVEN_H2_RST_STREAM, id, PROVEN_H2_CANCEL); feed(c, g_buf.b, g_buf.n, g_buf.n, &log); }
                for (int i = 0; i < 3; i++, id += 2) { g_buf.n = 0; put_get(&g_buf, id, true); feed(c, g_buf.b, g_buf.n, g_buf.n, &log);
                    PROVEN_TEST_ASSERT(proven_h2_conn_submit_response(c, id, OK200, 2, true) == PROVEN_OK, "a request is answered", ""); }
                drain(c, (void *)0);
            }
            PROVEN_TEST_ASSERT(!log.closed && log.resets == 150 && log.headers == 300, "resets matched by streams that finish never end the connection: 150 of each, at an allowance of 3", "");
            g_buf.n = 0;
            for (int i = 0; i < 4; i++, id += 2) { put_get(&g_buf, id, false); put_u32_frame(&g_buf, PROVEN_H2_RST_STREAM, id, PROVEN_H2_CANCEL); }
            feed(c, g_buf.b, g_buf.n, g_buf.n, &log);
            PROVEN_TEST_ASSERT(log.closed == 1 && log.close_code == PROVEN_H2_ENHANCE_YOUR_CALM, "four in a row then do", "");
            proven_h2_conn_destroy(c);
            g_buf.n = 0;
        }
        g_cfg.max_reset_debt = 0;

        /* Streams beyond the limit are refused, not opened. */
        g_cfg.max_concurrent_streams = 3;
        {
            proven_h2_conn_t *c = opened(&g_cfg);
            log_t log = { 0 }; seen_t seen = { 0 };
            for (proven_u32 i = 0; i < 5; i++) put_get(&g_buf, 2 * i + 1, false);
            put_frame(&g_buf, PROVEN_H2_DATA, 0, 9, "late", 4);
            feed(c, g_buf.b, g_buf.n, 1, &log);
            drain(c, &seen);
            PROVEN_TEST_ASSERT(log.headers == 3 && proven_h2_conn_stream_count(c) == 3 && seen.rst == 2 && seen.rst_id[0] == 7 && seen.rst_id[1] == 9 && seen.rst_code[0] == PROVEN_H2_REFUSED_STREAM && !log.closed,
                "the fourth and fifth streams at a limit of three are refused with REFUSED_STREAM, and what follows on them is dropped", "");
            proven_h2_conn_destroy(c);
            g_buf.n = 0;
        }
        /* The memory of how streams ended is as long as the limit: resets of every open stream are all forgiven. */
        {
            proven_h2_conn_t *c = opened(&g_cfg);
            log_t log = { 0 }; seen_t seen = { 0 };
            for (proven_u32 i = 0; i < 3; i++) put_get(&g_buf, 2 * i + 1, false);
            feed(c, g_buf.b, g_buf.n, g_buf.n, &log);
            for (proven_u32 i = 0; i < 3; i++) PROVEN_TEST_ASSERT(proven_h2_conn_reset(c, 2 * i + 1, PROVEN_H2_CANCEL) == PROVEN_OK, "the application resets all three", "");
            drain(c, (void *)0);
            g_buf.n = 0;
            for (proven_u32 i = 0; i < 3; i++) put_frame(&g_buf, PROVEN_H2_DATA, 0, 2 * i + 1, "late", 4);
            feed(c, g_buf.b, g_buf.n, g_buf.n, &log);
            drain(c, &seen);
            PROVEN_TEST_ASSERT(!log.closed && seen.rst == 0 && seen.goaway == 0, "data in flight for every stream this side reset is forgiven", "");
            proven_h2_conn_destroy(c);
            g_buf.n = 0;
        }
        g_cfg.max_concurrent_streams = 0;

        /* Frames that carry nothing and end nothing. */
        put_get(&g_buf, 1, false);
        for (int i = 0; i < 16; i++) put_frame(&g_buf, PROVEN_H2_DATA, 0, 1, g_zero, 0);
        put_frame(&g_buf, PROVEN_H2_DATA, 0, 1, "x", 1);
        for (int i = 0; i < 16; i++) put_frame(&g_buf, PROVEN_H2_DATA, 0, 1, g_zero, 0);
        verdict("sixteen empty DATA frames in a row are borne, twice, with a byte between", V_IGNORED, 0);
        put_get(&g_buf, 1, false);
        for (int i = 0; i < 17; i++) put_frame(&g_buf, PROVEN_H2_DATA, 0, 1, g_zero, 0);
        verdict("the seventeenth ends the connection", V_CONN, PROVEN_H2_ENHANCE_YOUR_CALM);
        (void)block_of(GET, 4);
        put_frame(&g_buf, PROVEN_H2_HEADERS, 0, 1, g_block, 1);
        for (int i = 0; i < 17; i++) put_frame(&g_buf, PROVEN_H2_CONTINUATION, 0, 1, g_zero, 0);
        verdict("and so does the seventeenth empty CONTINUATION", V_CONN, PROVEN_H2_ENHANCE_YOUR_CALM);

        /* Answers the peer asks for and does not read. */
        g_cfg.max_buffered_output = 1024;
        {
            static const proven_byte_t z8[8] = { 0 };
            proven_h2_conn_t *c = opened(&g_cfg);
            log_t log = { 0 };
            g_buf.n = 0; put_frame(&g_buf, PROVEN_H2_PING, 0, 0, z8, 8);
            for (int i = 0; i < 60; i++) feed(c, g_buf.b, g_buf.n, g_buf.n, &log);
            PROVEN_TEST_ASSERT(!log.closed && proven_h2_conn_pending_output(c).size == 60 * 17, "sixty unread answers to PING fit a bound of 1,024 bytes", "");
            feed(c, g_buf.b, g_buf.n, g_buf.n, &log);
            PROVEN_TEST_ASSERT(log.closed == 1 && log.close_code == PROVEN_H2_ENHANCE_YOUR_CALM, "the sixty-first ends the connection: a peer that asks must take the answers", "");
            proven_h2_conn_destroy(c);
            c = opened(&g_cfg);
            log = (log_t){ 0 };
            for (int i = 0; i < 5000; i++) { feed(c, g_buf.b, g_buf.n, g_buf.n, &log); drain(c, (void *)0); }
            PROVEN_TEST_ASSERT(!log.closed, "five thousand that are read are no trouble", "");
            proven_h2_conn_destroy(c);
            g_buf.n = 0;
        }
        g_cfg.max_buffered_output = 0;

        /* Output for a peer that does not read: the application is refused, the connection lives. */
        {
            static pair_t p;
            proven_h2_config_t cc = base, sc = base;
            cc.initial_window = 1 << 24;
            pair_open(&p, &cc, &sc, 1 << 19);
            proven_u32 a, b;
            PROVEN_TEST_ASSERT(proven_h2_conn_submit_request(p.client, GET, 4, true, &a) == PROVEN_OK && proven_h2_conn_submit_request(p.client, GET, 4, true, &b) == PROVEN_OK, "two requests", "");
            carry(&p, p.client, p.server);
            PROVEN_TEST_ASSERT(proven_h2_conn_submit_response(p.server, a, OK200, 2, false) == PROVEN_OK, "the first is answered", "");
            proven_size_t total = 0;
            for (int i = 0; i < 40; i++) total += proven_h2_conn_send_data(p.server, a, (proven_mem_view_t){ g_zero, sizeof g_zero }, false).value;
            proven_size_t queued = proven_h2_conn_pending_output(p.server).size;
            PROVEN_TEST_ASSERT(total <= 65536 && total > 60000 && queued <= 65536 + 9 * 8, "with nobody reading, about 64 KiB of body is taken and no more, whatever the windows allow", "");
            PROVEN_TEST_ASSERT(proven_h2_conn_submit_response(p.server, b, OK200, 2, true) == PROVEN_ERR_BUSY && proven_h2_conn_stream_count(p.server) == 2, "and a response for another stream is PROVEN_ERR_BUSY", "");
            PROVEN_TEST_ASSERT(proven_h2_conn_send_room(p.server, a) == 0, "the room to send says so", "");
            carry(&p, p.server, p.client);
            PROVEN_TEST_ASSERT(p.slog.writable == 2 && !p.slog.closed, "when the output has gone, both streams are told they may go on", "");
            PROVEN_TEST_ASSERT(proven_h2_conn_submit_response(p.server, b, OK200, 2, true) == PROVEN_OK, "and the response goes out", "");
            pair_close(&p);
        }
    }

    PROVEN_TEST_SECTION("goodbye", "GOAWAY from either side: open streams finish, new ones are not begun, and the end is reported when the last has ended.", "Check proven_h2_conn_shutdown, the GOAWAY case of h2c_on_fixed and the sweeps at the top of proven_h2_conn_feed.");
    {
        static pair_t p;
        pair_open(&p, &base, &base, 1);
        proven_u32 a, b, x;
        PROVEN_TEST_ASSERT(proven_h2_conn_submit_request(p.client, GET, 4, true, &a) == PROVEN_OK, "a request", "");
        carry(&p, p.client, p.server);
        /* A second request is on its way when the server says goodbye. */
        PROVEN_TEST_ASSERT(proven_h2_conn_submit_request(p.client, GET, 4, true, &b) == PROVEN_OK, "and a second, not yet delivered", "");
        proven_h2_conn_shutdown(p.server, PROVEN_H2_NO_ERROR);
        proven_h2_conn_shutdown(p.server, PROVEN_H2_NO_ERROR);
        carry(&p, p.client, p.server);
        PROVEN_TEST_ASSERT(p.slog.headers == 1 && proven_h2_conn_stream_count(p.server) == 1 && !p.slog.closed, "the server does not act on a stream begun after its goodbye", "");
        carry(&p, p.server, p.client);
        PROVEN_TEST_ASSERT(p.clog.goaways == 1 && p.clog.resets == 1 && p.clog.reset_id == b && p.clog.reset_code == PROVEN_H2_REFUSED_STREAM && !p.clog.closed && proven_h2_conn_stream_count(p.client) == 1,
            "the client is told once, and that its second request was not acted on: it can be sent elsewhere", "");
        PROVEN_TEST_ASSERT(proven_h2_conn_submit_request(p.client, GET, 4, true, &x) == PROVEN_ERR_INVALID_STATE, "no new request after a goodbye", "");
        PROVEN_TEST_ASSERT(proven_h2_conn_submit_response(p.server, a, OK200, 2, false) == PROVEN_OK && proven_h2_conn_send_data(p.server, a, proven_mem_view_from_u8(sv("bye")), true).value == 3, "the open stream is answered", "");
        carry(&p, p.server, p.client);
        carry(&p, p.client, p.server);
        PROVEN_TEST_ASSERT(p.clog.data_bytes == 3 && p.clog.closed == 1 && p.clog.close_code == PROVEN_H2_NO_ERROR && p.slog.closed == 1 && p.slog.close_code == PROVEN_H2_NO_ERROR,
            "and when it has ended both ends report the end, with NO_ERROR", "");
        proven_size_t used; proven_h2_event_t ev;
        PROVEN_TEST_ASSERT(proven_h2_conn_feed(p.client, (proven_mem_view_t){ (void *)0, 0 }, &used, &ev) == PROVEN_ERR_INVALID_STATE && proven_h2_conn_ping(p.client, (const proven_byte_t *)"12345678") == PROVEN_ERR_INVALID_STATE
            && proven_h2_conn_send_data(p.client, a, (proven_mem_view_t){ (void *)0, 0 }, true).err == PROVEN_ERR_INVALID_STATE && proven_h2_conn_stream_count(p.client) == 0, "after which nothing is taken", "");
        pair_close(&p);

        /* A goodbye with an error is the end at once. */
        pair_open(&p, &base, &base, 1 << 19);
        PROVEN_TEST_ASSERT(proven_h2_conn_submit_request(p.client, POST, 4, false, &a) == PROVEN_OK, "a request", "");
        carry(&p, p.client, p.server);
        proven_h2_conn_shutdown(p.client, PROVEN_H2_INTERNAL_ERROR);
        PROVEN_TEST_ASSERT(proven_h2_conn_is_closed(p.client), "a goodbye with an error code ends the connection at once", "");
        carry(&p, p.client, p.server);
        PROVEN_TEST_ASSERT(p.slog.goaways == 1 && p.clog.closed == 1 && p.clog.close_code == PROVEN_H2_INTERNAL_ERROR, "the peer is told why", "");
        pair_close(&p);

        g_cfg = base; g_cfg.server = true;
        { static const proven_byte_t g1[8] = { 0, 0, 0, 5, 0, 0, 0, 0 }, g2[8] = { 0, 0, 0, 7, 0, 0, 0, 0 }; put_get(&g_buf, 1, false); put_frame(&g_buf, PROVEN_H2_GOAWAY, 0, 0, g1, 8); put_frame(&g_buf, PROVEN_H2_GOAWAY, 0, 0, g2, 8); }
        verdict("6.8: a second GOAWAY that names a later stream than the first is PROTOCOL_ERROR", V_CONN, PROVEN_H2_PROTOCOL_ERROR);
        g_before = goodbye;
        put_get(&g_buf, 1, false);
        {
            proven_h2_conn_t *c = opened(&g_cfg);
            proven_h2_conn_shutdown(c, PROVEN_H2_NO_ERROR);
            seen_t seen = { 0 }; log_t log = { 0 };
            drain(c, &seen);
            PROVEN_TEST_ASSERT(seen.goaway == 1 && seen.goaway_code == PROVEN_H2_NO_ERROR && seen.goaway_last == 0, "a goodbye before any stream names stream 0", "");
            feed(c, g_buf.b, 0, 1, &log);
            PROVEN_TEST_ASSERT(log.closed == 1 && log.close_code == PROVEN_H2_NO_ERROR, "and with no stream open the end follows at once", "");
            proven_h2_conn_destroy(c);
        }
        g_buf.n = 0; g_before = (void *)0;
    }

    PROVEN_TEST_SECTION("the text of a goodbye", "A GOAWAY frame may carry text that says why. The event carries its first 256 bytes, however the frame arrived.", "Check the RS_TEXT case of h2c_read and h2c_goaway_event.");
    {
        proven_h2_config_t cfg = base;
        static proven_byte_t pay[8 + 300];
        static const proven_size_t chunks[] = { 1, 7, 1 << 16 };
        for (proven_size_t i = 0; i < sizeof pay; i++) pay[i] = i < 8 ? (proven_byte_t)(i == 3 ? 1 : 0) : (proven_byte_t)('a' + i % 26);
        for (proven_size_t k = 0; k < COUNT(chunks); k++) {
            static const proven_size_t lens[] = { 0, 1, 27, 256, 257, 300 };
            for (proven_size_t j = 0; j < COUNT(lens); j++) {
                proven_h2_conn_t *c = opened(&cfg);
                log_t log = { 0 };
                proven_u32 id;
                PROVEN_TEST_ASSERT(proven_h2_conn_submit_request(c, POST, 4, false, &id) == PROVEN_OK, "a request, so that the connection stays", "");
                g_buf.n = 0;
                put_frame(&g_buf, PROVEN_H2_GOAWAY, 0, 0, pay, 8 + lens[j]);
                put_frame(&g_buf, PROVEN_H2_PING, 0, 0, "12345678", 8);          /* what follows the text is read as a frame */
                feed(c, g_buf.b, g_buf.n, chunks[k], &log);
                const proven_size_t want = lens[j] < 256 ? lens[j] : 256;
                PROVEN_TEST_ASSERT(log.err == PROVEN_OK && log.goaways == 1 && log.text_len == want && memcmp(log.text, pay + 8, want) == 0,
                    "the event carries the text, or its first 256 bytes, and nothing when there is none", "");
                seen_t seen = { 0 };
                drain(c, &seen);
                PROVEN_TEST_ASSERT(seen.pongs == 1 && !log.closed, "and the frame after it is read as a frame", "");
                proven_h2_conn_destroy(c);
            }
        }
        /* A second GOAWAY with text reuses the memory; one with none clears it. */
        {
            proven_h2_conn_t *c = opened(&cfg);
            proven_u32 id;
            PROVEN_TEST_ASSERT(proven_h2_conn_submit_request(c, POST, 4, false, &id) == PROVEN_OK, "a request, so that the connection stays", "");
            log_t log = { 0 };
            g_buf.n = 0;
            put_frame(&g_buf, PROVEN_H2_GOAWAY, 0, 0, pay, 8 + 5);
            feed(c, g_buf.b, g_buf.n, 3, &log);
            PROVEN_TEST_ASSERT(log.goaways == 1 && log.text_len == 5 && !log.closed, "a first goodbye with five bytes", "");
            const proven_size_t blocks = g_mem.blocks;
            g_buf.n = 0;
            put_frame(&g_buf, PROVEN_H2_GOAWAY, 0, 0, pay, 8 + 9);
            put_frame(&g_buf, PROVEN_H2_GOAWAY, 0, 0, pay, 8);
            feed(c, g_buf.b, 9 + 17, 3, &log);
            PROVEN_TEST_ASSERT(log.goaways == 2 && log.text_len == 9 && g_mem.blocks == blocks, "a second with nine, in the same memory", "");
            feed(c, g_buf.b + 9 + 17, 17, 3, &log);
            PROVEN_TEST_ASSERT(log.goaways == 3 && log.text_len == 0, "and a third with none says none", "");
            proven_h2_conn_destroy(c);
            /* No memory for the text: the goodbye is reported without it. */
            c = opened(&cfg);
            log = (log_t){ 0 };
            PROVEN_TEST_ASSERT(proven_h2_conn_submit_request(c, POST, 4, false, &id) == PROVEN_OK, "a request", "");
            g_buf.n = 0;
            put_frame(&g_buf, PROVEN_H2_GOAWAY, 0, 0, pay, 8 + 9);
            put_frame(&g_buf, PROVEN_H2_PING, 0, 0, "12345678", 8);
            g_mem.calls = 0; g_mem.fail_at = 0;
            feed(c, g_buf.b, g_buf.n, 4, &log);
            g_mem.fail_at = -1;
            seen_t seen = { 0 };
            drain(c, &seen);
            PROVEN_TEST_ASSERT(log.err == PROVEN_OK && log.goaways == 1 && log.text_len == 0 && seen.pongs == 1, "with no memory for the text the goodbye is reported without it, and the frames stay in step", "");
            proven_h2_conn_destroy(c);
        }
        /* The same frame cut short at every byte and with every bit changed, to each role, a
         * byte at a time and whole: the reader keeps to its input and to the text's memory. */
        {
            static buf_t rec, mut;
            long runs = 0;
            const proven_size_t blocks_before = g_mem.blocks;
            for (int role = 0; role < 2; role++) {
                proven_h2_config_t rc = base; rc.server = role == 0;
                rec.n = 0;
                if (rc.server) { memcpy(rec.b, PROVEN_H2_PREFACE, 24); rec.n = 24; }
                put_frame(&rec, PROVEN_H2_SETTINGS, 0, 0, (void *)0, 0);
                put_frame(&rec, PROVEN_H2_GOAWAY, 0, 0, pay, 8 + 30);
                put_frame(&rec, PROVEN_H2_GOAWAY, 0, 0, pay, 8 + 300);
                put_frame(&rec, PROVEN_H2_PING, 0, 0, "12345678", 8);
                for (proven_size_t i = 0; i < rec.n + rec.n * 8; i++) {
                    proven_size_t n = rec.n;
                    memcpy(mut.b, rec.b, n);
                    if (i < rec.n) n = i;
                    else mut.b[(i - rec.n) / 8] ^= (proven_byte_t)(1u << ((i - rec.n) % 8));
                    for (int whole = 0; whole < 2; whole++) {
                        proven_h2_conn_t *c = conn_new(&rc);
                        proven_u32 id;
                        if (!rc.server) PROVEN_TEST_ASSERT(proven_h2_conn_submit_request(c, POST, 4, false, &id) == PROVEN_OK, "a request, so that the connection stays", "");
                        proven_size_t pos = 0;
                        for (;;) {
                            const proven_size_t take = whole ? n - pos : (n - pos ? 1 : 0);
                            proven_size_t used = 0; proven_h2_event_t ev;
                            proven_err_t err = proven_h2_conn_feed(c, (proven_mem_view_t){ take ? mut.b + pos : (void *)0, take }, &used, &ev);
                            if (err != PROVEN_OK) { PROVEN_TEST_ASSERT(err == PROVEN_ERR_INVALID_FORMAT && rc.server && pos + used < 24, "the only error of a feed is a preface that is not one", ""); break; }
                            PROVEN_TEST_ASSERT(used <= take, "a feed stays inside its input", "");
                            pos += used;
                            if (ev.kind == PROVEN_H2_EVENT_GOAWAY) {
                                PROVEN_TEST_ASSERT(ev.data.size <= 256 && (ev.data.size == 0 || ev.data.ptr != (void *)0), "a goodbye's text is at most what is kept", "");
                                volatile proven_byte_t sum = 0;
                                for (proven_size_t k = 0; k < ev.data.size; k++) sum = (proven_byte_t)(sum + ev.data.ptr[k]);     /* every byte of it is there to read */
                            }
                            if (ev.kind == PROVEN_H2_EVENT_CLOSED) break;
                            if (ev.kind == PROVEN_H2_EVENT_NONE && pos == n) break;
                        }
                        proven_h2_conn_destroy(c);
                        runs++;
                    }
                }
            }
            PROVEN_TEST_ASSERT(runs > 4000 && g_mem.blocks == blocks_before, "thousands of damaged goodbyes, and nothing is left allocated", "");
        }
        g_buf.n = 0;
    }

    PROVEN_TEST_SECTION("fields no one may remember", "proven_h2_conn_next_sensitive marks fields of the next header list: they go out in the never-indexed form, and the marks are for that list only.", "Check h2c_take_sensitive and the three callers of h2c_send_block.");
    {
        proven_h2_config_t cfg = base;
        static const proven_http_header_t KEYED[] = { H(":method", "GET"), H(":scheme", "https"), H(":authority", "example.org"), H(":path", "/"), H("x-api-key", "k-0123456789"), H("x-plain", "remember me") };
        static const bool marks[6] = { false, false, false, false, true, false };
        proven_h2_conn_t *c = opened(&cfg);
        proven_hpack_decoder_t *d = (void *)0;
        PROVEN_TEST_ASSERT(proven_hpack_decoder_create(g_alloc, 4096, &d) == PROVEN_OK, "a decoder for what the connection sends", "");
        static proven_http_header_t got[16];
        static proven_byte_t scratch[4096];
        bool never[16];
        proven_u32 id;
        /* 0: marked. 1: not marked - the marks were for one list. 2: marked, then a list that is refused; 3: the list after that. */
        for (int round = 0; round < 4; round++) {
            if (round == 0 || round == 2) proven_h2_conn_next_sensitive(c, marks);
            if (round == 2) {
                PROVEN_TEST_ASSERT(proven_h2_conn_submit_request(c, KEYED + 1, 5, true, &id) == PROVEN_ERR_INVALID_ARG, "a list that is refused", "");
                continue;
            }
            PROVEN_TEST_ASSERT(proven_h2_conn_submit_request(c, KEYED, 6, true, &id) == PROVEN_OK, "a request with a key in it", "");
            proven_mem_view_t out = proven_h2_conn_pending_output(c);
            proven_size_t count = 0;
            PROVEN_TEST_ASSERT(out.size > 9 && proven_hpack_decode_marked(d, (proven_mem_view_t){ out.ptr + 9, out.size - 9 }, got, never, 16, (proven_mem_mut_t){ scratch, sizeof scratch }, 0, &count) == PROVEN_OK && count == 6,
                "what was queued is one header block of six fields", "");
            proven_h2_conn_output_sent(c, out.size);
            if (count != 6) break;
            PROVEN_TEST_ASSERT(!never[0] && !never[1] && !never[2] && !never[3] && !never[5], "no other field is marked", "");
            if (round == 0) PROVEN_TEST_ASSERT(never[4] && proven_hpack_decoder_table_count(d) == 2, "the marked field is sent never-indexed, and is in no table", "");
            if (round == 1) PROVEN_TEST_ASSERT(!never[4] && proven_hpack_decoder_table_count(d) == 3, "the next list is not marked: the marks were for one", "");
            if (round == 3) PROVEN_TEST_ASSERT(!never[4], "nor is the list after one that was refused", "");
        }
        proven_h2_conn_next_sensitive(c, marks);
        proven_h2_conn_next_sensitive(c, (void *)0);
        PROVEN_TEST_ASSERT(proven_h2_conn_submit_request(c, KEYED, 6, true, &id) == PROVEN_OK, "marks taken back", "");
        {
            proven_mem_view_t out = proven_h2_conn_pending_output(c);
            proven_size_t count = 0;
            PROVEN_TEST_ASSERT(proven_hpack_decode_marked(d, (proven_mem_view_t){ out.ptr + 9, out.size - 9 }, got, never, 16, (proven_mem_mut_t){ scratch, sizeof scratch }, 0, &count) == PROVEN_OK && count == 6 && !never[4], "mark nothing", "");
        }
        proven_hpack_decoder_destroy(d);
        proven_h2_conn_destroy(c);
        /* A server's response and its trailers. */
        cfg.server = true;
        c = opened(&cfg);
        PROVEN_TEST_ASSERT(proven_hpack_decoder_create(g_alloc, 4096, &d) == PROVEN_OK, "a decoder", "");
        g_buf.n = 0;
        put_get(&g_buf, 1, true);
        log_t log = { 0 };
        feed(c, g_buf.b, g_buf.n, g_buf.n, &log);
        g_buf.n = 0;
        static const proven_http_header_t SET[] = { H(":status", "200"), H("x-token", "t-abcdefgh") };
        static const proven_http_header_t TR[] = { H("x-signature", "s-abcdefgh") };
        static const bool second[2] = { false, true }, first[1] = { true };
        proven_h2_conn_next_sensitive(c, second);
        PROVEN_TEST_ASSERT(log.headers == 1 && proven_h2_conn_submit_response(c, 1, SET, 2, false) == PROVEN_OK, "a response with a token in it", "");
        proven_mem_view_t out = proven_h2_conn_pending_output(c);
        proven_size_t count = 0;
        PROVEN_TEST_ASSERT(proven_hpack_decode_marked(d, (proven_mem_view_t){ out.ptr + 9, out.size - 9 }, got, never, 16, (proven_mem_mut_t){ scratch, sizeof scratch }, 0, &count) == PROVEN_OK && count == 2 && !never[0] && never[1],
            "the token is sent never-indexed", "");
        proven_h2_conn_output_sent(c, out.size);
        proven_h2_conn_next_sensitive(c, first);
        PROVEN_TEST_ASSERT(proven_h2_conn_send_trailers(c, 1, TR, 1) == PROVEN_OK, "and trailers", "");
        out = proven_h2_conn_pending_output(c);
        PROVEN_TEST_ASSERT(proven_hpack_decode_marked(d, (proven_mem_view_t){ out.ptr + 9, out.size - 9 }, got, never, 16, (proven_mem_mut_t){ scratch, sizeof scratch }, 0, &count) == PROVEN_OK && count == 1 && never[0],
            "likewise", "");
        proven_hpack_decoder_destroy(d);
        proven_h2_conn_destroy(c);
    }

    PROVEN_TEST_SECTION("hostile input", "Every beginning of a recorded conversation, and every single-bit change of it, given to a fresh connection of each role.", "A crash or a sanitizer report here is a missing length check in h2c_read or one of the frame handlers.");
    {
        /* Record both directions of a conversation with a bit of everything in it. */
        static buf_t c2s, s2c;
        static pair_t p;
        proven_h2_config_t cfg = base; cfg.initial_window = 5000;
        proven_h2_config_t a = cfg, b = cfg; a.alloc = b.alloc = g_alloc; b.server = true;
        proven_h2_conn_t *cl = conn_new(&a), *sr = conn_new(&b);
        proven_u32 ids[3]; proven_size_t sent[3] = { 0 }, back[3] = { 0 }; bool answered[3] = { false }, done[3] = { false };
        static const proven_byte_t eight[8] = { 'p', 'i', 'n', 'g', 'p', 'o', 'n', 'g' };
        for (int round = 0; round < 200; round++) {
            if (round == 1) for (int k = 0; k < 3; k++) PROVEN_TEST_ASSERT(proven_h2_conn_submit_request(cl, POST, 4, false, &ids[k]) == PROVEN_OK, "a request for the record", "");
            if (round == 3) PROVEN_TEST_ASSERT(proven_h2_conn_ping(cl, eight) == PROVEN_OK, "a ping for the record", "");
            if (round >= 1) for (int k = 0; k < 3; k++) {
                if (sent[k] <= 12000) { proven_result_size_t r = proven_h2_conn_send_data(cl, ids[k], (proven_mem_view_t){ g_zero, 12000 - sent[k] }, true); sent[k] += r.value; if (sent[k] == 12000 && r.err == PROVEN_OK && (r.value > 0 || round > 150)) sent[k] = 12001; }
                if (answered[k] && back[k] <= 9000) { proven_result_size_t r = proven_h2_conn_send_data(sr, ids[k], (proven_mem_view_t){ g_zero, 9000 - back[k] }, k != 2); back[k] += r.value;
                    if (back[k] == 9000) { back[k] = 9001; if (k == 2) PROVEN_TEST_ASSERT(proven_h2_conn_send_trailers(sr, ids[k], TRAIL, 1) == PROVEN_OK, "trailers for the record", ""); } }
            }
            for (int dir = 0; dir < 2; dir++) {
                proven_h2_conn_t *from = dir == 0 ? cl : sr, *to = dir == 0 ? sr : cl;
                buf_t *rec = dir == 0 ? &c2s : &s2c;
                proven_mem_view_t out = proven_h2_conn_pending_output(from);
                PROVEN_TEST_ASSERT(rec->n + out.size <= sizeof rec->b, "the record fits", "");
                if (out.size) memcpy(rec->b + rec->n, out.ptr, out.size);
                proven_size_t start = rec->n, n = out.size, pos = 0;
                rec->n += n;
                proven_h2_conn_output_sent(from, n);
                for (;;) {
                    proven_size_t used = 0; proven_h2_event_t ev;
                    PROVEN_TEST_ASSERT(proven_h2_conn_feed(to, (proven_mem_view_t){ n - pos ? rec->b + start + pos : (void *)0, n - pos }, &used, &ev) == PROVEN_OK, "the recorded conversation runs", "");
                    pos += used;
                    if (ev.kind == PROVEN_H2_EVENT_DATA) proven_h2_conn_consumed(to, ev.stream_id, ev.data.size);
                    if (to == sr && ev.kind == PROVEN_H2_EVENT_DATA && ev.end_stream) { int k = (int)(ev.stream_id - 1) / 2; answered[k] = true; PROVEN_TEST_ASSERT(proven_h2_conn_submit_response(sr, ev.stream_id, OK200, 2, false) == PROVEN_OK, "an answer for the record", ""); }
                    if (to == cl && (ev.kind == PROVEN_H2_EVENT_DATA || ev.kind == PROVEN_H2_EVENT_TRAILERS) && ev.end_stream) done[(ev.stream_id - 1) / 2] = true;
                    if (ev.kind == PROVEN_H2_EVENT_NONE && pos == n) break;
                }
            }
            if (done[0] && done[1] && done[2]) break;
        }
        PROVEN_TEST_ASSERT(done[0] && done[1] && done[2] && c2s.n > 36000 && s2c.n > 27000, "a conversation of three requests with bodies, a ping and trailers is recorded", "");
        proven_h2_conn_destroy(cl); proven_h2_conn_destroy(sr);
        (void)p;

        /* Replay against a fresh connection that answers as the original did. */
        static buf_t mut;
        long runs = 0;
        for (int role = 0; role < 2; role++) {
            const buf_t *rec = role == 0 ? &c2s : &s2c;
            proven_h2_config_t rc = cfg; rc.alloc = g_alloc; rc.server = role == 0;
            /* Beginnings: every one of the first 2,000, then every 97th. Bit changes: every bit of the first 400 bytes, then one bit in every 53rd byte. */
            for (int mode = 0; mode < 2; mode++) {
                const proven_size_t cases = mode == 0 ? rec->n : 400 * 8 + rec->n / 53;
                for (proven_size_t i = 0; i < cases; i++) {
                    proven_size_t n = rec->n;
                    if (mode == 0) { if (i > 2000 && i % 97 != 0) continue; n = i; memcpy(mut.b, rec->b, n); }
                    else {
                        memcpy(mut.b, rec->b, n);
                        if (i < 400 * 8) mut.b[i / 8] ^= (proven_byte_t)(1u << (i % 8));
                        else { proven_size_t at = (i - 400 * 8) * 53; mut.b[at] ^= (proven_byte_t)(1u << (at % 8)); }
                    }
                    proven_h2_conn_t *c = conn_new(&rc);
                    if (role == 1) { proven_u32 id; for (int k = 0; k < 3; k++) PROVEN_TEST_ASSERT(proven_h2_conn_submit_request(c, POST, 4, k == 2, &id) == PROVEN_OK, "the client's requests are begun", ""); }
                    proven_size_t pos = 0;
                    for (;;) {
                        proven_size_t used = 0; proven_h2_event_t ev;
                        proven_err_t err = proven_h2_conn_feed(c, (proven_mem_view_t){ n - pos ? mut.b + pos : (void *)0, n - pos }, &used, &ev);
                        if (err != PROVEN_OK) { PROVEN_TEST_ASSERT(err == PROVEN_ERR_INVALID_FORMAT && role == 0 && pos + used < 24, "the only error of a feed is a preface that is not one", ""); break; }
                        PROVEN_TEST_ASSERT(used <= n - pos && proven_h2_conn_stream_count(c) <= 100, "a feed stays inside its input and its stream limit", "");
                        pos += used;
                        if (ev.kind == PROVEN_H2_EVENT_DATA) {
                            PROVEN_TEST_ASSERT(ev.data.size == 0 || (ev.data.ptr >= mut.b && ev.data.ptr + ev.data.size <= mut.b + n), "a DATA event lies inside what was fed", "");
                            proven_h2_conn_consumed(c, ev.stream_id, ev.data.size);
                        }
                        if (role == 0 && ev.kind == PROVEN_H2_EVENT_HEADERS) {
                            proven_err_t e = proven_h2_conn_submit_response(c, ev.stream_id, OK200, 2, false);
                            PROVEN_TEST_ASSERT(e == PROVEN_OK || e == PROVEN_ERR_BUSY || e == PROVEN_ERR_OUT_OF_BOUNDS, "a request that was reported can be answered, unless a changed setting says the peer takes no list so large", "");
                            (void)proven_h2_conn_send_data(c, ev.stream_id, (proven_mem_view_t){ g_zero, 3000 }, true);
                        }
                        PROVEN_TEST_ASSERT(proven_h2_conn_pending_output(c).size <= 65536 + 70000, "what is queued stays within its bound", "");
                        if (ev.kind == PROVEN_H2_EVENT_CLOSED) break;
                        if (ev.kind == PROVEN_H2_EVENT_NONE && pos == n) break;
                    }
                    proven_h2_conn_destroy(c);
                    runs++;
                }
            }
        }
        PROVEN_TEST_INFO("hostile input: {} replays", PROVEN_ARG((long long)runs));
    }

    PROVEN_TEST_SECTION("finding a stream", "Thousands of streams opened and ended in no order, a few of them open far longer than the rest, with every open stream asked for after every change.", "Check h2c_peek, h2c_map, h2c_add and h2c_remove: the map from a stream's number to its place is kept through growth and through every stream that moves to a place another left, and a stream whose slot was taken is still found.");
    {
        proven_h2_config_t cc = base; cc.server = false; cc.max_concurrent_streams = 50;
        proven_h2_conn_t *c = opened(&cc);
        static int tags[4];                               /* user pointers that say which stream they were given to */
        proven_u32 live[40], n = 0, opened_count = 0, wrong = 0, ghosts = 0, state = 0x9e3779b9u;
        proven_u32 gone[8] = { 0 };
        for (int step = 0; step < 6000 && wrong == 0; step++) {
            state ^= state << 13; state ^= state >> 17; state ^= state << 5;
            /* Three times it is emptied altogether: the table goes back and is made again. */
            const bool empty_it = step == 1500 || step == 3000 || step == 4500;
            if (!empty_it && (n == 0 || (n < 40 && state % 100 < 52))) {
                proven_u32 id = 0;
                if (proven_h2_conn_submit_request(c, POST, 4, false, &id) != PROVEN_OK || id != 2 * opened_count + 1) { wrong++; break; }
                opened_count++;
                proven_h2_conn_stream_set_user(c, id, &tags[(id >> 1) % 4]);
                live[n++] = id;
            } else {
                /* Mostly one of the newest: the oldest stay, and the numbers run on past them
                 * until a new stream's slot is one an old stream still holds. */
                do {
                    const proven_u32 k = empty_it ? n - 1 : state % 8 == 0 ? (state >> 8) % n : n - 1 - (state >> 8) % (n < 6 ? n : 6);
                    const proven_u32 id = live[k];
                    if (proven_h2_conn_reset(c, id, PROVEN_H2_CANCEL) != PROVEN_OK) wrong++;
                    gone[step % 8] = id;
                    live[k] = live[--n];
                } while (empty_it && n > 0);
            }
            proven_h2_conn_output_sent(c, proven_h2_conn_pending_output(c).size);
            if (empty_it) {
                /* A call with nothing to read is where an idle connection lets go of the table. */
                proven_size_t used = 0; proven_h2_event_t ev;
                if (proven_h2_conn_feed(c, (proven_mem_view_t){ 0 }, &used, &ev) != PROVEN_OK || ev.kind != PROVEN_H2_EVENT_NONE) wrong++;
            }
            if (proven_h2_conn_stream_count(c) != n) wrong++;
            for (proven_u32 i = 0; i < n; i++) if (proven_h2_conn_stream_user(c, live[i]) != &tags[(live[i] >> 1) % 4]) wrong++;
            for (int i = 0; i < 8; i++) {
                bool is_live = false;
                for (proven_u32 k = 0; k < n; k++) if (live[k] == gone[i]) is_live = true;
                if (gone[i] != 0 && !is_live && proven_h2_conn_stream_user(c, gone[i]) != NULL) ghosts++;
            }
            if (proven_h2_conn_stream_user(c, 2 * opened_count + 1) != NULL) ghosts++;      /* nor one not yet begun */
        }
        PROVEN_TEST_ASSERT(wrong == 0, "every open stream is found, with what was attached to it, after every change", "");
        PROVEN_TEST_ASSERT(ghosts == 0, "and a stream that has ended, or has not begun, is not", "");
        PROVEN_TEST_ASSERT(opened_count > 2500, "thousands of streams came and went", "");
        proven_h2_conn_destroy(c);
    }

    PROVEN_TEST_SECTION("windows that grow", "With growth turned on (an internal call the event-driven drivers make), a stream's window doubles when its reader takes half of it within two round trips of its last move, up to the cap, and the connection's follows; a slow reader, an unread stream, a connection without the call and one whose SETTINGS were never acknowledged keep the window they started with (RFC-0019 section 10).", "Check h2c_stream_credit, h2c_conn_follow and proven_h2_conn_grow_ in src/proven/h2_conn.c.");
    {
        proven_size_t sent = 0;
        proven_h2_conn_t *c = grow_run(true, 10000, 10, (proven_size_t)40 << 20, false, &sent);
        PROVEN_TEST_ASSERT(sent == (proven_size_t)40 << 20 && proven_h2_conn_test_window_(c, 1) == 16u << 20 && proven_h2_conn_test_window_(c, 0) == 24u << 20,
            "a reader that keeps up, a 10 ms round trip, 16 KiB every 10 us: the stream's window reaches 16 MiB and the connection's 24 MiB", "");
        proven_h2_conn_destroy(c);
        c = grow_run(true, 10000, 1000, (proven_size_t)2 << 20, false, &sent);
        const proven_u32 w = proven_h2_conn_test_window_(c, 1);
        PROVEN_TEST_ASSERT(w > PROVEN_H2_DEFAULT_WINDOW && w < 16u << 20, "16 KiB every millisecond: it grows while half a window takes under 20 ms, and stops there", "");
        proven_h2_conn_destroy(c);
        c = grow_run(true, 10000, 15000, (proven_size_t)1 << 20, false, &sent);
        PROVEN_TEST_ASSERT(proven_h2_conn_test_window_(c, 1) == PROVEN_H2_DEFAULT_WINDOW && proven_h2_conn_test_window_(c, 0) == PROVEN_H2_DEFAULT_WINDOW,
            "16 KiB every 15 ms: half a window takes 30 ms, more than two round trips - nothing grows", "");
        proven_h2_conn_destroy(c);
        c = grow_run(true, 10000, 100, (proven_size_t)1 << 20, true, &sent);
        PROVEN_TEST_ASSERT(sent == PROVEN_H2_DEFAULT_WINDOW && proven_h2_conn_test_window_(c, 1) == PROVEN_H2_DEFAULT_WINDOW,
            "a body nobody reads: the sender stops at the first window, and it never grows", "");
        proven_h2_conn_destroy(c);
        c = grow_run(false, 10000, 100, (proven_size_t)4 << 20, false, &sent);
        PROVEN_TEST_ASSERT(sent == (proven_size_t)4 << 20 && proven_h2_conn_test_window_(c, 1) == PROVEN_H2_DEFAULT_WINDOW && proven_h2_conn_test_window_(c, 0) == PROVEN_H2_DEFAULT_WINDOW,
            "without the call nothing grows, however fast it is read: the public engine is as it was", "");
        proven_h2_conn_destroy(c);
        c = grow_run(true, 0, 100, (proven_size_t)4 << 20, false, &sent);
        PROVEN_TEST_ASSERT(proven_h2_conn_test_window_(c, 1) == PROVEN_H2_DEFAULT_WINDOW, "and nothing grows before the round trip is known (no SETTINGS acknowledgement)", "");
        proven_h2_conn_destroy(c);
    }

    PROVEN_TEST_SECTION("settings changed after the opening", "proven_h2_conn_set_max_streams and proven_h2_conn_set_initial_window: one frame in flight, the larger value enforced until the peer acknowledges, the values at creation as ceilings.", "Check h2c_settings_change, h2c_settings_acked and the enforced number in h2c_on_block.");
    {
        /* Streams: lowered, and the old number holds until the acknowledgement. */
        proven_h2_config_t sc = base;
        sc.server = true; sc.max_concurrent_streams = 10;
        proven_h2_conn_t *c = opened(&sc);
        PROVEN_TEST_ASSERT(proven_h2_conn_set_max_streams(c, 2) == PROVEN_OK, "a server lowers its streams from 10 to 2", "");
        PROVEN_TEST_ASSERT(said_one(said(c), PROVEN_H2_SETTINGS_MAX_CONCURRENT_STREAMS, 2), "one SETTINGS frame goes out, carrying only that setting", "");
        log_t log = { 0 };
        g_buf.n = 0;
        for (proven_u32 id = 1; id <= 9; id += 2) put_get(&g_buf, id, false);
        feed(c, g_buf.b, g_buf.n, g_buf.n, &log);
        seen_t seen = { 0 };
        drain(c, &seen);
        PROVEN_TEST_ASSERT(log.err == PROVEN_OK && log.headers == 5 && seen.rst == 0, "before the peer acknowledges, the old number holds: five streams are taken", "");
        ack(c, &log);
        g_buf.n = 0;
        put_get(&g_buf, 11, false);
        feed(c, g_buf.b, g_buf.n, g_buf.n, &log);
        seen = (seen_t){ 0 };
        drain(c, &seen);
        PROVEN_TEST_ASSERT(log.headers == 5 && seen.rst == 1 && seen.rst_id[0] == 11 && seen.rst_code[0] == PROVEN_H2_REFUSED_STREAM,
            "after the acknowledgement a stream over the new number is refused, safe to send again", "");
        PROVEN_TEST_ASSERT(proven_h2_conn_stream_count(c) == 5 && !proven_h2_conn_is_closed(c), "and the streams already open are not touched", "");

        /* Bounds. */
        PROVEN_TEST_ASSERT(proven_h2_conn_set_max_streams(c, 11) == PROVEN_ERR_INVALID_ARG, "above the number given at creation: PROVEN_ERR_INVALID_ARG", "");
        PROVEN_TEST_ASSERT(proven_h2_conn_set_initial_window(c, 65536) == PROVEN_ERR_INVALID_ARG, "a window above the one given at creation: PROVEN_ERR_INVALID_ARG", "");
        PROVEN_TEST_ASSERT(proven_h2_conn_set_max_streams((void *)0, 1) == PROVEN_ERR_INVALID_ARG && proven_h2_conn_set_initial_window((void *)0, 1) == PROVEN_ERR_INVALID_ARG, "no connection: PROVEN_ERR_INVALID_ARG", "");
        PROVEN_TEST_ASSERT(proven_h2_conn_set_max_streams(c, 10) == PROVEN_OK && said_one(said(c), PROVEN_H2_SETTINGS_MAX_CONCURRENT_STREAMS, 10), "raised back to the ceiling", "");
        proven_h2_conn_shutdown(c, PROVEN_H2_NO_ERROR);
        drain(c, (void *)0);
        PROVEN_TEST_ASSERT(proven_h2_conn_set_max_streams(c, 3) == PROVEN_ERR_INVALID_STATE && proven_h2_conn_set_initial_window(c, 100) == PROVEN_ERR_INVALID_STATE, "after a goodbye: PROVEN_ERR_INVALID_STATE", "");
        proven_h2_conn_destroy(c);

        /* One frame in flight: calls made meanwhile become one frame, with the last values. */
        c = opened(&sc);
        log = (log_t){ 0 };
        PROVEN_TEST_ASSERT(proven_h2_conn_set_max_streams(c, 5) == PROVEN_OK && said_one(said(c), PROVEN_H2_SETTINGS_MAX_CONCURRENT_STREAMS, 5), "a change goes out at once when none is in flight", "");
        PROVEN_TEST_ASSERT(proven_h2_conn_set_max_streams(c, 4) == PROVEN_OK && proven_h2_conn_set_max_streams(c, 3) == PROVEN_OK
            && proven_h2_conn_set_initial_window(c, 30000) == PROVEN_OK, "calls while one is in flight succeed", "");
        PROVEN_TEST_ASSERT(said(c).frames == 0, "and send nothing yet", "");
        ack(c, &log);
        said_t r = said(c);
        bool streams3 = false, window30000 = false;
        for (int i = 0; i < r.entries; i++) {
            if (r.id[i] == PROVEN_H2_SETTINGS_MAX_CONCURRENT_STREAMS && r.value[i] == 3) streams3 = true;
            if (r.id[i] == PROVEN_H2_SETTINGS_INITIAL_WINDOW_SIZE && r.value[i] == 30000) window30000 = true;
        }
        PROVEN_TEST_ASSERT(log.err == PROVEN_OK && r.frames == 1 && r.entries == 2 && streams3 && window30000, "the acknowledgement lets one frame go, with the latest of each", "");
        ack(c, &log);
        PROVEN_TEST_ASSERT(said(c).frames == 0, "nothing more is owed", "");
        PROVEN_TEST_ASSERT(proven_h2_conn_set_max_streams(c, 3) == PROVEN_OK && said(c).frames == 0, "a value equal to the one in force sends nothing", "");
        proven_h2_conn_destroy(c);

        /* The window lowered: what the peer sent before it read the change is taken; after the
         * acknowledgement every open stream's window moves down by the difference. */
        g_consume = false;
        sc.max_concurrent_streams = 0;
        c = opened(&sc);
        log = (log_t){ 0 };
        g_buf.n = 0;
        put_get(&g_buf, 1, false);
        feed(c, g_buf.b, g_buf.n, g_buf.n, &log);
        PROVEN_TEST_ASSERT(proven_h2_conn_set_initial_window(c, 16384) == PROVEN_OK && said_one(said(c), PROVEN_H2_SETTINGS_INITIAL_WINDOW_SIZE, 16384), "a server lowers the initial window from 65,535 to 16,384", "");
        g_buf.n = 0;
        put_data(&g_buf, 1, 30000);
        feed(c, g_buf.b, g_buf.n, g_buf.n, &log);
        seen = (seen_t){ 0 };
        drain(c, &seen);
        PROVEN_TEST_ASSERT(log.data_bytes == 30000 && seen.rst == 0 && !log.closed, "30,000 bytes sent under the old window are taken", "");
        ack(c, &log);
        g_buf.n = 0;
        put_data(&g_buf, 1, 1);
        feed(c, g_buf.b, g_buf.n, g_buf.n, &log);
        seen = (seen_t){ 0 };
        drain(c, &seen);
        PROVEN_TEST_ASSERT(seen.rst == 1 && seen.rst_id[0] == 1 && seen.rst_code[0] == PROVEN_H2_FLOW_CONTROL_ERROR && !log.closed,
            "after the acknowledgement that stream has 16,384 less room than it was given: one more byte is its error", "");
        g_buf.n = 0;
        put_get(&g_buf, 3, false);
        put_data(&g_buf, 3, 16384);
        feed(c, g_buf.b, g_buf.n, g_buf.n, &log);
        seen = (seen_t){ 0 };
        drain(c, &seen);
        PROVEN_TEST_ASSERT(seen.rst == 0 && log.data_bytes == 30000 + 16384, "a new stream may send 16,384", "");
        g_buf.n = 0;
        put_data(&g_buf, 3, 1);
        feed(c, g_buf.b, g_buf.n, g_buf.n, &log);
        seen = (seen_t){ 0 };
        drain(c, &seen);
        PROVEN_TEST_ASSERT(seen.rst == 1 && seen.rst_id[0] == 3 && seen.rst_code[0] == PROVEN_H2_FLOW_CONTROL_ERROR, "and not one byte more", "");
        proven_h2_conn_destroy(c);

        /* The window raised: the peer may use it before its acknowledgement arrives. */
        sc.initial_window = 1 << 20;
        c = opened(&sc);
        log = (log_t){ 0 };
        PROVEN_TEST_ASSERT(proven_h2_conn_set_initial_window(c, 16384) == PROVEN_OK, "lowered", "");
        said(c);
        ack(c, &log);
        PROVEN_TEST_ASSERT(proven_h2_conn_set_initial_window(c, 1 << 20) == PROVEN_OK && said_one(said(c), PROVEN_H2_SETTINGS_INITIAL_WINDOW_SIZE, 1u << 20), "and raised back", "");
        g_buf.n = 0;
        put_get(&g_buf, 1, false);
        put_data(&g_buf, 1, 100000);
        feed(c, g_buf.b, g_buf.n, g_buf.n, &log);
        seen = (seen_t){ 0 };
        drain(c, &seen);
        PROVEN_TEST_ASSERT(seen.rst == 0 && !log.closed && log.data_bytes == 100000, "a raise holds at once: 100,000 bytes before the acknowledgement are taken", "");
        ack(c, &log);
        PROVEN_TEST_ASSERT(!log.closed && said(c).frames == 0, "and its acknowledgement changes nothing more", "");
        proven_h2_conn_destroy(c);

        /* A change made before the opening is acknowledged: the opening's value holds first. */
        c = conn_new(&sc);
        said(c);
        log = (log_t){ 0 };
        g_buf.n = 0;
        memcpy(g_buf.b, PROVEN_H2_PREFACE, 24); g_buf.n = 24;
        put_frame(&g_buf, PROVEN_H2_SETTINGS, 0, 0, (void *)0, 0);
        feed(c, g_buf.b, g_buf.n, g_buf.n, &log);
        drain(c, (void *)0);
        PROVEN_TEST_ASSERT(proven_h2_conn_set_initial_window(c, 16384) == PROVEN_OK && said_one(said(c), PROVEN_H2_SETTINGS_INITIAL_WINDOW_SIZE, 16384), "a change before the opening is acknowledged goes out at once", "");
        ack(c, &log);                                  /* the opening's */
        g_buf.n = 0;
        put_get(&g_buf, 1, false);
        put_data(&g_buf, 1, 70000);
        feed(c, g_buf.b, g_buf.n, g_buf.n, &log);
        seen = (seen_t){ 0 };
        drain(c, &seen);
        PROVEN_TEST_ASSERT(seen.rst == 0 && log.data_bytes == 70000, "the first acknowledgement is the opening's: its window of 1 MiB holds", "");
        ack(c, &log);                                  /* the change's */
        g_buf.n = 0;
        put_data(&g_buf, 1, 1);
        feed(c, g_buf.b, g_buf.n, g_buf.n, &log);
        seen = (seen_t){ 0 };
        drain(c, &seen);
        PROVEN_TEST_ASSERT(seen.rst == 1 && seen.rst_code[0] == PROVEN_H2_FLOW_CONTROL_ERROR, "the second is the change's: the stream is now 70,000 into a window of 16,384", "");
        proven_h2_conn_destroy(c);
        g_consume = true;

        /* A client: its own limit holds at once. */
        proven_h2_config_t cc = base;
        c = opened(&cc);
        proven_u32 id = 0;
        PROVEN_TEST_ASSERT(proven_h2_conn_set_max_streams(c, 1) == PROVEN_OK && said_one(said(c), PROVEN_H2_SETTINGS_MAX_CONCURRENT_STREAMS, 1), "a client lowers its streams to 1, and says so", "");
        PROVEN_TEST_ASSERT(proven_h2_conn_submit_request(c, GET, 4, true, &id) == PROVEN_OK, "it opens one", "");
        PROVEN_TEST_ASSERT(proven_h2_conn_submit_request(c, GET, 4, true, &id) == PROVEN_ERR_BUSY, "and a second is PROVEN_ERR_BUSY at once, with no acknowledgement", "");
        proven_h2_conn_destroy(c);
    }


    PROVEN_TEST_SECTION("memory", "What an idle connection keeps, what a stream costs, and that an allocation failing anywhere is survived.", "Check the release at the end of proven_h2_conn_feed and the paths that set `oom`.");
    {
        proven_hpack_encoder_destroy(g_enc); g_enc = (void *)0;
        PROVEN_TEST_ASSERT(g_mem.blocks == 0 && g_mem.bytes == 0, "every connection made so far gave back all it took", "");
        for (int tables = 1; tables >= 0; tables--) {
            proven_h2_config_t sc = base; sc.server = true;
            if (tables == 0) sc.set_header_table = true;       /* and no header table: nothing is kept for compression */
            proven_h2_conn_t *c = opened(&sc);
            const proven_size_t idle = g_mem.bytes;
            PROVEN_TEST_ASSERT(g_mem.blocks == 1 && idle <= 400, "a connection that has only opened holds one block, of at most 400 bytes", "");
            /* With a table of its own this time, so that the connection's decoder fills one too -
             * and from another allocator, so that what is counted is the connection's alone. */
            PROVEN_TEST_ASSERT(proven_hpack_encoder_create(proven_heap_allocator(), tables ? 4096 : 0, &g_enc) == PROVEN_OK, "the script's encoder again", "");
            const proven_size_t enc_bytes = 0;
            log_t log = { 0 };
            g_buf.n = 0;
            for (proven_u32 i = 0; i < 100; i++) put_get(&g_buf, 2 * i + 1, false);
            feed(c, g_buf.b, g_buf.n, g_buf.n, &log);
            const proven_size_t busy = g_mem.bytes - enc_bytes;
            PROVEN_TEST_ASSERT(log.headers == 100 && proven_h2_conn_stream_count(c) == 100, "a hundred streams are open", "");
            PROVEN_TEST_INFO("memory: {} bytes for a connection that has only opened; {} bytes with 100 open streams and the buffers for one header list", PROVEN_ARG((long long)idle), PROVEN_ARG((long long)busy));
            PROVEN_TEST_ASSERT(busy < idle + 100 * 64 + 16384 * 5 + 2048 + (tables ? 7168u : 0u), "a stream costs under 64 bytes, on top of one header list's buffers", "");
            for (proven_u32 i = 0; i < 100; i++) PROVEN_TEST_ASSERT(proven_h2_conn_submit_response(c, 2 * i + 1, OK200, 2, true) == PROVEN_OK, "each is answered", "");
            drain(c, (void *)0);
            feed(c, g_buf.b, 0, 1, &log);
            const proven_size_t after = g_mem.bytes - enc_bytes;
            PROVEN_TEST_INFO("memory: {} bytes idle again, in {} blocks (the struct, the HPACK state and the record of how streams ended)", PROVEN_ARG((long long)after), PROVEN_ARG((long long)g_mem.blocks));
            PROVEN_TEST_ASSERT(after < (tables ? 3072u : 1280u) && g_mem.blocks <= 6,
                "idle again, the buffers and the stream table are gone: what stays is the struct, the record of endings and the HPACK state - under 3 KiB after a hundred small requests with both header tables in use (they grow as they are used, to about 6 KiB each at most), under 1,280 bytes with none", "");
            proven_h2_conn_destroy(c);
            proven_hpack_encoder_destroy(g_enc); g_enc = (void *)0;
            PROVEN_TEST_ASSERT(g_mem.blocks == 0 && g_mem.bytes == 0, "and destroying it gives back everything", "");

        }

        /* A smaller header table takes effect when the peer has acknowledged it, not when it
         * was sent: a peer's first requests are written before it has read these settings,
         * under the size every connection starts with. */
        for (int follows = 0; follows < 2; follows++) {
            proven_h2_config_t sc = { .alloc = g_alloc, .server = true, .set_header_table = true, .header_table_bytes = 0 };
            proven_h2_conn_t *c = conn_new(&sc);
            /* :method GET, :scheme https, :path /, :authority added to the table, a field
             * added to the table - and then that field again, by its place in the table. */
            static const proven_byte_t early[] = { 0x82, 0x87, 0x84, 0x41, 0x0b, 'e', 'x', 'a', 'm', 'p', 'l', 'e', '.', 'o', 'r', 'g',
                                                   0x40, 0x06, 'x', '-', 'm', 'a', 'r', 'k', 0x03, 'o', 'n', 'e', 0xbe };
            log_t log = { 0 };
            g_buf.n = 0;
            memcpy(g_buf.b, PROVEN_H2_PREFACE, 24); g_buf.n = 24;
            put_frame(&g_buf, PROVEN_H2_SETTINGS, 0, 0, (void *)0, 0);
            put_frame(&g_buf, PROVEN_H2_HEADERS, EH | ES, 1, early, sizeof early);
            feed(c, g_buf.b, g_buf.n, g_buf.n, &log);
            PROVEN_TEST_ASSERT(log.err == PROVEN_OK && log.headers == 1 && !log.closed,
                "a request written before the peer read \"no header table\" may still use the table it began with", "");
            drain(c, (void *)0);
            /* The acknowledgement: from here the peer has to say that it followed. */
            static const proven_byte_t stale[] = { 0x82, 0x87, 0x84, 0xbf, 0xbe };
            static const proven_byte_t followed[] = { 0x20, 0x82, 0x87, 0x84, 0x01, 0x0b, 'e', 'x', 'a', 'm', 'p', 'l', 'e', '.', 'o', 'r', 'g' };
            g_buf.n = 0;
            put_frame(&g_buf, PROVEN_H2_SETTINGS, PROVEN_H2_FLAG_ACK, 0, (void *)0, 0);
            if (follows) put_frame(&g_buf, PROVEN_H2_HEADERS, EH | ES, 3, followed, sizeof followed);
            else put_frame(&g_buf, PROVEN_H2_HEADERS, EH | ES, 3, stale, sizeof stale);
            feed(c, g_buf.b, g_buf.n, g_buf.n, &log);
            if (follows) PROVEN_TEST_ASSERT(log.err == PROVEN_OK && log.headers == 2 && !log.closed, "once acknowledged, a block that opens by setting the table to nothing is read", "");
            else PROVEN_TEST_ASSERT(log.closed == 1 && log.close_code == PROVEN_H2_COMPRESSION_ERROR, "once acknowledged, a block that goes on using the old table is a compression error", "");
            proven_h2_conn_destroy(c);
        }

        /* Every allocation of a short conversation fails once. */
        g_mem.calls = 0; g_mem.fail_at = 1000000;
        PROVEN_TEST_ASSERT(conversation(), "the conversation runs when memory is to be had", "");
        const long total = g_mem.calls;
        int survived = 0;
        for (long at = 0; at < total; at++) {
            g_mem.calls = 0; g_mem.fail_at = at;
            if (!conversation()) survived++;
            PROVEN_TEST_ASSERT(g_mem.blocks == 0 && g_mem.bytes == 0, "a conversation cut short by a failed allocation leaks nothing", "");
        }
        g_mem.fail_at = -1;
        /* Not every failure is the end: the record of how streams ended is done without. */
        PROVEN_TEST_ASSERT(total >= 12 && survived >= total - 2, "each of the conversation's allocations failing ends it cleanly, or is done without", "");
        PROVEN_TEST_INFO("memory: {} allocations in a conversation, each failed once; {} of the failures ended it", PROVEN_ARG((long long)total), PROVEN_ARG((long long)survived));
    }

    PROVEN_TEST_PASS("h2_conn: an HTTP/2 connection without a socket");
    return 0;
}
