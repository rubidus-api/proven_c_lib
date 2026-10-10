#include "proven/http_event_client.h"

#if !defined(PROVEN_FREESTANDING) && !defined(PROVEN_NO_NET)

#include "proven/url.h"

/*
 * An event-driven HTTP/1.1 client on a loop.
 *
 * One request is one connection and one struct. Nothing here waits: the connect is begun and
 * finished when the socket says so, the request leaves as the server takes it, and the
 * response is parsed where it lies in the client's read buffer - only what cannot be used yet
 * is copied aside.
 *
 * Callbacks may call back in (write, end, abort, pause, resume). So, as in http_event.c:
 *   - cl_process is not re-entered: a nested call returns and the outer loop sees the change;
 *   - nothing reads a field after a callback without having re-checked `dead`;
 *   - a request is freed only when the outermost entry leaves (`depth`).
 * And on_done is never called from inside proven_http_event_client_start: that call only
 * begins the connect, and everything that can fail afterwards happens from the loop.
 */

#define CL_READ_BYTES ((proven_size_t)64 * 1024)
#define CL_PLAIN_BYTES ((proven_size_t)16 * 1024 + 512)
#define CL_MAX_INTERIM 8u

#define CL_CONNECTING 0     /* the connect has not finished */
#define CL_HEAD 1           /* connected; the response head has not arrived */
#define CL_BODY 2           /* reading the response body */

#define CL_T_NONE 0
#define CL_T_CONNECT 1
#define CL_T_WRITE 2
#define CL_T_RESPONSE 3
#define CL_T_BODY 4

typedef struct { proven_byte_t *ptr; proven_size_t len, cap, off; bool borrowed; } cl_buf_t;

struct proven_http_event_request {
    proven_http_event_client_t *client;
    struct proven_http_event_request *prev, *next;
    proven_net_conn_t sock;
    proven_loop_io_t io;
    proven_loop_timer_t timer;
    proven_tls_conn_t *tls;
    cl_buf_t out;                      /* request bytes the transport has not taken yet */
    cl_buf_t stash;                    /* response bytes not used yet */
    proven_http_body_t body;
    proven_http_event_client_callbacks_t on;
    void *ctx;
    void *user;
    proven_u64 send_left;              /* request-body bytes still promised; LENGTH_UNKNOWN when chunked */
    proven_u32 depth;
    proven_u32 interim;
    proven_http_method_t method;
    proven_u8 state;
    proven_u8 timer_kind;
    bool connected_at_start, streamed, chunked, request_ended, want_writable, paused, peer_eof, processing, dead;
};

struct proven_http_event_client {
    proven_loop_t *loop;
    proven_allocator_t alloc;
    proven_http_event_client_config_t cfg;
    proven_http_event_request_t *requests;
    proven_size_t request_count;
    proven_http_header_t *headers;
    proven_byte_t *rbuf, *pbuf, *hbuf;
};

static proven_u8str_view_t cl_lit_n(const char *s, proven_size_t n) { return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)s, .size = n }; }
#define cl_lit(s) cl_lit_n((s), sizeof(s) - 1)

static proven_byte_t cl_lower(proven_byte_t c) { return (c >= 'A' && c <= 'Z') ? (proven_byte_t)(c + 32) : c; }
static bool cl_eq_nocase(proven_u8str_view_t a, proven_u8str_view_t b) {
    if (a.size != b.size) return false;
    for (proven_size_t i = 0; i < a.size; ++i) if (cl_lower(a.ptr[i]) != cl_lower(b.ptr[i])) return false;
    return true;
}

static void *cl_alloc(proven_allocator_t a, proven_size_t n) {
    proven_result_mem_mut_t m = a.alloc_fn(a.ctx, n, 16);
    return m.err == PROVEN_OK ? m.value.ptr : (void *)0;
}

// -----------------------------------------------------------------------------
// Buffers
// -----------------------------------------------------------------------------

static proven_size_t buf_pending(const cl_buf_t *b) { return b->len - b->off; }

static void buf_release(proven_http_event_client_t *cl, cl_buf_t *b) {
    if (b->ptr && !b->borrowed) cl->alloc.free_fn(cl->alloc.ctx, b->ptr);
    *b = (cl_buf_t){ 0 };
}

/* Make the buffer own its pending bytes, with room for `more` behind them. */
static bool buf_own(proven_http_event_client_t *cl, cl_buf_t *b, proven_size_t more) {
    proven_size_t have = buf_pending(b);
    if (!b->borrowed && b->off == 0 && b->cap >= have + more) return true;
    if (!b->borrowed && b->cap >= have + more) {
        for (proven_size_t i = 0; i < have; ++i) b->ptr[i] = b->ptr[b->off + i];
        b->len = have; b->off = 0;
        return true;
    }
    proven_size_t cap = 256;
    while (cap < have + more) { if (cap > (proven_size_t)-1 / 2) return false; cap *= 2; }
    proven_byte_t *p = cl_alloc(cl->alloc, cap);
    if (!p) return false;
    for (proven_size_t i = 0; i < have; ++i) p[i] = b->ptr[b->off + i];
    if (b->ptr && !b->borrowed) cl->alloc.free_fn(cl->alloc.ctx, b->ptr);
    b->ptr = p; b->len = have; b->off = 0; b->cap = cap; b->borrowed = false;
    return true;
}

static bool buf_append(proven_http_event_client_t *cl, cl_buf_t *b, const proven_byte_t *p, proven_size_t n) {
    if (n == 0) return true;
    if (!buf_own(cl, b, n)) return false;
    for (proven_size_t i = 0; i < n; ++i) b->ptr[b->len + i] = p[i];
    b->len += n;
    return true;
}

// -----------------------------------------------------------------------------
// A request's life
// -----------------------------------------------------------------------------

static void cl_process(proven_http_event_request_t *c);
static void cl_on_timer(void *ctx);

static void cl_enter(proven_http_event_request_t *c) { c->depth++; }

static void cl_leave(proven_http_event_request_t *c) {
    if (--c->depth > 0 || !c->dead) return;
    proven_http_event_client_t *cl = c->client;
    buf_release(cl, &c->stash);
    buf_release(cl, &c->out);
    cl->alloc.free_fn(cl->alloc.ctx, c);
}

/* The end, good or bad: close, and tell the program once. */
static void cl_kill(proven_http_event_request_t *c, proven_err_t why) {
    if (c->dead) return;
    proven_http_event_client_t *cl = c->client;
    c->dead = true;
    proven_loop_timer_cancel(cl->loop, &c->timer);
    proven_loop_io_remove(cl->loop, &c->io);           /* before the close */
    if (c->tls) { proven_tls_conn_destroy(c->tls); c->tls = (void *)0; }
    if (proven_net_conn_is_open(&c->sock)) (void)proven_net_close(&c->sock);
    if (c->prev) c->prev->next = c->next;
    else cl->requests = c->next;
    if (c->next) c->next->prev = c->prev;
    cl->request_count--;
    c->on.on_done(c->ctx, c, why);
}

static bool cl_secured(const proven_http_event_request_t *c) { return !c->tls || proven_tls_is_established(c->tls); }

static proven_size_t cl_buffered(const proven_http_event_request_t *c) {
    proven_size_t n = buf_pending(&c->out);
    if (c->tls) n += proven_tls_pending_output(c->tls).size;
    return n;
}

/* Which time limit applies depends on what the request is waiting for. Limits that measure a
 * whole step (connecting, the wait for the head) are set once; those that measure silence
 * (a write not taken, a body not arriving) start again with every sign of life. */
static void cl_arm(proven_http_event_request_t *c) {
    if (c->dead) return;
    proven_http_event_client_t *cl = c->client;
    proven_u8 kind = CL_T_NONE;
    proven_u32 ms = 0;
    if (c->state == CL_CONNECTING || !cl_secured(c)) { kind = CL_T_CONNECT; ms = cl->cfg.connect_timeout_ms; }
    else if (cl_buffered(c) > 0) { kind = CL_T_WRITE; ms = cl->cfg.write_timeout_ms; }
    else if (c->state == CL_HEAD && c->request_ended) { kind = CL_T_RESPONSE; ms = cl->cfg.response_timeout_ms; }
    else if (c->state == CL_BODY && !c->paused) { kind = CL_T_BODY; ms = cl->cfg.body_timeout_ms; }
    /* Otherwise the program is producing a request body, or has paused: its business. */
    if (kind == CL_T_NONE) proven_loop_timer_cancel(cl->loop, &c->timer);
    else if (kind != c->timer_kind || kind == CL_T_WRITE || kind == CL_T_BODY) proven_loop_timer_set(cl->loop, &c->timer, ms, cl_on_timer, c);
    c->timer_kind = kind;
}

static void cl_watch(proven_http_event_request_t *c) {
    if (c->dead) return;
    proven_u8 want = 0;
    if (c->state == CL_CONNECTING) want = PROVEN_NET_WRITABLE;
    else {
        if (!c->peer_eof && !(c->state == CL_BODY && c->paused)) want |= PROVEN_NET_READABLE;
        if (cl_buffered(c) > 0) want |= PROVEN_NET_WRITABLE;
    }
    (void)proven_loop_io_set(c->client->loop, &c->io, want);
}

/* Push what is waiting towards the socket, without waiting. False when the request ended. */
static bool cl_flush(proven_http_event_request_t *c) {
    if (c->dead) return false;
    if (c->state == CL_CONNECTING) return true;
    for (;;) {
        if (c->tls && buf_pending(&c->out) > 0 && proven_tls_is_established(c->tls)) {
            proven_result_size_t w = proven_tls_write(c->tls, (proven_mem_view_t){ .ptr = c->out.ptr + c->out.off, .size = buf_pending(&c->out) });
            if (w.err != PROVEN_OK) { cl_kill(c, PROVEN_ERR_RESET); return false; }
            c->out.off += w.value;
        }
        proven_mem_view_t pending = c->tls ? proven_tls_pending_output(c->tls)
                                           : (proven_mem_view_t){ .ptr = c->out.ptr + c->out.off, .size = buf_pending(&c->out) };
        if (pending.size == 0) break;
        proven_result_size_t w = proven_net_write(&c->sock, pending, PROVEN_NET_DONT_WAIT);
        if (w.err == PROVEN_ERR_TIMEOUT || w.err == PROVEN_ERR_AGAIN) break;
        if (w.err != PROVEN_OK) { cl_kill(c, PROVEN_ERR_RESET); return false; }
        if (c->tls) proven_tls_output_sent(c->tls, w.value);
        else c->out.off += w.value;
        if (w.value == 0) break;
    }
    if (buf_pending(&c->out) == 0) buf_release(c->client, &c->out);
    return true;
}

/* Something was sent, or the state moved: see what that allows. */
static void cl_progress(proven_http_event_request_t *c) {
    if (c->dead) return;
    cl_enter(c);
    if (c->streamed && !c->request_ended && c->want_writable && cl_buffered(c) < c->client->cfg.max_buffered_output) {
        c->want_writable = false;
        if (c->on.on_writable) c->on.on_writable(c->ctx, c);
    }
    if (!c->dead) { cl_watch(c); cl_arm(c); }
    cl_leave(c);
}

// -----------------------------------------------------------------------------
// Input
// -----------------------------------------------------------------------------

static void cl_process(proven_http_event_request_t *c) {
    proven_http_event_client_t *cl = c->client;
    if (c->processing || c->dead) return;         /* the outer call will see what changed */
    c->processing = true;
    cl_enter(c);
    while (!c->dead) {
        proven_mem_view_t in = { .ptr = c->stash.ptr + c->stash.off, .size = buf_pending(&c->stash) };
        if (c->state == CL_HEAD) {
            if (in.size == 0) break;
            proven_http_response_t res;
            proven_size_t head = 0;
            proven_err_t e = proven_http_parse_response(in, cl->headers, cl->cfg.max_headers, cl->cfg.max_head_bytes, &res, &head);
            if (e == PROVEN_ERR_NEED_MORE) {
                if (in.size >= cl->cfg.max_head_bytes) cl_kill(c, PROVEN_ERR_OUT_OF_BOUNDS);
                break;
            }
            if (e != PROVEN_OK) { cl_kill(c, e == PROVEN_ERR_OUT_OF_BOUNDS ? e : PROVEN_ERR_INVALID_FORMAT); break; }
            c->stash.off += head;                 /* consumed; the views stay good until this loop ends */
            if (res.status >= 100 && res.status < 200) {
                /* 100 Continue, 103 Early Hints: read and passed over. A server that sends them
                 * without end is not answering; and this client did not ask to change protocol. */
                if (res.status == 101) { cl_kill(c, PROVEN_ERR_UNSUPPORTED); break; }
                if (++c->interim > CL_MAX_INTERIM) { cl_kill(c, PROVEN_ERR_INVALID_FORMAT); break; }
                continue;
            }
            proven_http_framing_t framing = { 0 };
            if (proven_http_response_framing(&res, c->method, &framing) != PROVEN_OK) { cl_kill(c, PROVEN_ERR_INVALID_FORMAT); break; }
            if (proven_http_body_init(&c->body, framing, cl->cfg.max_body_bytes) != PROVEN_OK) { cl_kill(c, PROVEN_ERR_OUT_OF_BOUNDS); break; }
            bool no_body = framing.kind == PROVEN_HTTP_BODY_NONE || (framing.kind == PROVEN_HTTP_BODY_LENGTH && framing.length == 0);
            c->state = CL_BODY;
            if (c->on.on_response) c->on.on_response(c->ctx, c, &res);
            if (c->dead) break;
            if (no_body) { cl_kill(c, PROVEN_OK); break; }
            cl_arm(c);
            continue;
        }
        if (c->state != CL_BODY || c->paused || in.size == 0) break;
        proven_size_t used = 0;
        proven_mem_view_t payload = { 0 };
        bool done = false;
        proven_err_t e = proven_http_body_feed(&c->body, in, &used, &payload, &done);
        if (e != PROVEN_OK) { cl_kill(c, e); break; }
        c->stash.off += used;
        if ((payload.size > 0 || done) && c->on.on_body) c->on.on_body(c->ctx, c, payload, done);
        if (c->dead) break;
        if (done) { cl_kill(c, PROVEN_OK); break; }
        cl_arm(c);
        if (used == 0) break;                     /* it needs more than is here */
    }
    /* The server closed. That is the end of a body with no length of its own, and the loss
     * of anything else - but only once what arrived before it has been delivered. (What is
     * still in the stash here is the start of something that will now never be finished.) */
    if (!c->dead && c->peer_eof && !(c->state == CL_BODY && c->paused)) {
        if (c->state == CL_BODY && proven_http_body_end(&c->body) == PROVEN_OK) {
            if (c->on.on_body) c->on.on_body(c->ctx, c, (proven_mem_view_t){ 0 }, true);
            if (!c->dead) cl_kill(c, PROVEN_OK);
        } else {
            cl_kill(c, PROVEN_ERR_RESET);
        }
    }
    if (!c->dead) {
        /* Keep only what could not be used. */
        if (buf_pending(&c->stash) == 0) buf_release(cl, &c->stash);
        else if (!buf_own(cl, &c->stash, 0)) cl_kill(c, PROVEN_ERR_NOMEM);
    }
    c->processing = false;
    if (!c->dead) { cl_watch(c); cl_arm(c); }
    cl_leave(c);
}

/* Bytes of the response (after TLS, if any) have arrived in a buffer of the client's. */
static void cl_input(proven_http_event_request_t *c, const proven_byte_t *p, proven_size_t n) {
    proven_http_event_client_t *cl = c->client;
    if (c->dead || n == 0) return;
    if (buf_pending(&c->stash) == 0) {
        buf_release(cl, &c->stash);
        c->stash = (cl_buf_t){ .ptr = (proven_byte_t *)(proven_uintptr_t)p, .len = n, .cap = 0, .off = 0, .borrowed = true };
    } else if (!buf_append(cl, &c->stash, p, n)) {
        cl_kill(c, PROVEN_ERR_NOMEM);
        return;
    }
    cl_process(c);
}

static void cl_readable(proven_http_event_request_t *c) {
    proven_http_event_client_t *cl = c->client;
    proven_result_size_t r = proven_net_read(&c->sock, (proven_mem_mut_t){ .ptr = cl->rbuf, .size = CL_READ_BYTES }, PROVEN_NET_DONT_WAIT);
    if (r.err == PROVEN_ERR_TIMEOUT || r.err == PROVEN_ERR_AGAIN) return;
    if (r.err == PROVEN_ERR_EOF) {
        /* Under TLS an end of the connection without the engine having been told is a cut,
         * not a close: a body that runs until close must not be taken for whole. */
        if (c->tls) { cl_kill(c, PROVEN_ERR_RESET); return; }
        c->peer_eof = true;
        cl_process(c);
        return;
    }
    if (r.err != PROVEN_OK) { cl_kill(c, r.err == PROVEN_ERR_REFUSED || r.err == PROVEN_ERR_UNREACHABLE ? r.err : PROVEN_ERR_RESET); return; }
    if (!c->tls) { cl_input(c, cl->rbuf, r.value); return; }
    proven_size_t at = 0;
    while (at < r.value && !c->dead) {
        proven_size_t used = 0;
        proven_err_t e = proven_tls_feed(c->tls, (proven_mem_view_t){ .ptr = cl->rbuf + at, .size = r.value - at }, &used);
        at += used;
        if (e != PROVEN_OK) {
            (void)cl_flush(c);                     /* the alert */
            cl_kill(c, e);
            return;
        }
        for (;;) {
            proven_result_size_t got = proven_tls_read(c->tls, (proven_mem_mut_t){ .ptr = cl->pbuf, .size = CL_PLAIN_BYTES });
            if (got.err == PROVEN_ERR_EOF) { c->peer_eof = true; cl_process(c); break; }
            if (got.err != PROVEN_OK) break;
            cl_input(c, cl->pbuf, got.value);
            if (c->dead) return;
        }
        if (c->dead || c->peer_eof) break;
        if (used == 0) break;
    }
    /* The handshake may just have finished: the request can leave now. */
    if (!c->dead) { if (cl_flush(c)) cl_progress(c); }
}

static void cl_on_io(void *ctx, proven_u8 got) {
    proven_http_event_request_t *c = ctx;
    cl_enter(c);
    if (!c->dead && c->state == CL_CONNECTING) {
        proven_err_t e = c->connected_at_start ? PROVEN_OK : proven_net_connect_finish(&c->sock);
        if (e == PROVEN_OK) {
            c->state = CL_HEAD;
            if (cl_flush(c)) cl_progress(c);
        } else if (e != PROVEN_ERR_AGAIN) {
            cl_kill(c, e);
        }
    } else {
        if (!c->dead && (got & PROVEN_NET_WRITABLE)) { if (cl_flush(c)) cl_progress(c); }
        if (!c->dead && (got & (PROVEN_NET_READABLE | PROVEN_NET_FAILED))) cl_readable(c);
    }
    cl_leave(c);
}

static void cl_on_timer(void *ctx) {
    proven_http_event_request_t *c = ctx;
    cl_enter(c);
    cl_kill(c, PROVEN_ERR_TIMEOUT);
    cl_leave(c);
}

// -----------------------------------------------------------------------------
// The client
// -----------------------------------------------------------------------------

proven_err_t proven_http_event_client_create(proven_loop_t *loop, const proven_http_event_client_config_t *config,
                                             proven_http_event_client_t **out) {
    if (out) *out = (void *)0;
    if (!loop || !out) return PROVEN_ERR_INVALID_ARG;
    proven_http_event_client_config_t cfg = config ? *config : (proven_http_event_client_config_t){ 0 };
    if (!proven_alloc_is_valid(cfg.alloc)) cfg.alloc = proven_loop_allocator(loop);
    if (cfg.max_head_bytes == 0) cfg.max_head_bytes = 16 * 1024;
    if (cfg.max_headers == 0) cfg.max_headers = 64;
    if (cfg.max_body_bytes == 0) cfg.max_body_bytes = UINT64_MAX;
    if (cfg.max_buffered_output == 0) cfg.max_buffered_output = 64 * 1024;
    if (cfg.connect_timeout_ms == 0) cfg.connect_timeout_ms = 10000;
    if (cfg.response_timeout_ms == 0) cfg.response_timeout_ms = 30000;
    if (cfg.body_timeout_ms == 0) cfg.body_timeout_ms = 30000;
    if (cfg.write_timeout_ms == 0) cfg.write_timeout_ms = 30000;
    proven_allocator_t a = cfg.alloc;
    proven_http_event_client_t *cl = cl_alloc(a, sizeof *cl);
    if (!cl) return PROVEN_ERR_NOMEM;
    *cl = (proven_http_event_client_t){ .loop = loop, .alloc = a, .cfg = cfg };
    cl->headers = cl_alloc(a, cfg.max_headers * sizeof(proven_http_header_t));
    cl->rbuf = cl_alloc(a, CL_READ_BYTES);
    cl->hbuf = cl_alloc(a, cfg.max_head_bytes);
    if (cfg.tls) cl->pbuf = cl_alloc(a, CL_PLAIN_BYTES);
    if (!cl->headers || !cl->rbuf || !cl->hbuf || (cfg.tls && !cl->pbuf)) { proven_http_event_client_destroy(cl); return PROVEN_ERR_NOMEM; }
    *out = cl;
    return PROVEN_OK;
}

void proven_http_event_client_destroy(proven_http_event_client_t *cl) {
    if (!cl) return;
    while (cl->requests) {
        proven_http_event_request_t *c = cl->requests;
        cl_enter(c);
        cl_kill(c, PROVEN_ERR_RESET);
        cl_leave(c);
    }
    proven_allocator_t a = cl->alloc;
    if (cl->headers) a.free_fn(a.ctx, cl->headers);
    if (cl->rbuf) a.free_fn(a.ctx, cl->rbuf);
    if (cl->pbuf) a.free_fn(a.ctx, cl->pbuf);
    if (cl->hbuf) a.free_fn(a.ctx, cl->hbuf);
    a.free_fn(a.ctx, cl);
}

proven_size_t proven_http_event_client_requests(const proven_http_event_client_t *cl) { return cl ? cl->request_count : 0; }

static bool cl_is_reserved_header(proven_u8str_view_t name) {
    return cl_eq_nocase(name, cl_lit("Host")) || cl_eq_nocase(name, cl_lit("Content-Length")) ||
           cl_eq_nocase(name, cl_lit("Transfer-Encoding")) || cl_eq_nocase(name, cl_lit("Connection"));
}

/* Write the request head into the client's head buffer. */
static proven_err_t cl_build_head(proven_http_event_client_t *cl, const proven_http_event_request_options_t *o, const proven_url_t *u,
                                  proven_u8str_view_t method, proven_size_t *len) {
    proven_mem_mut_t out = { .ptr = cl->hbuf, .size = cl->cfg.max_head_bytes };
    /* The target is the path and the query, exactly as written. */
    proven_u8str_view_t target = cl_lit("/");
    if (u->path.size > 0) {
        target = u->path;
        if (u->has_query) target.size = (proven_size_t)(u->query.ptr + u->query.size - u->path.ptr);
    } else if (u->has_query) {
        return PROVEN_ERR_INVALID_ARG;             /* "http://host?q": write the slash */
    }
    proven_err_t e = proven_http_write_request_line(out, len, method, target);

    /* Host: the port is written only when it is not the scheme's own. */
    proven_byte_t host[300];
    proven_size_t hl = 0;
    if (u->host.size > 256) return PROVEN_ERR_INVALID_ARG;
    if (u->host_is_ipv6) host[hl++] = '[';
    for (proven_size_t i = 0; i < u->host.size; ++i) host[hl++] = u->host.ptr[i];
    if (u->host_is_ipv6) host[hl++] = ']';
    if (u->has_port && u->port != proven_url_default_port(u->scheme)) {
        host[hl++] = ':';
        char digits[5];
        int dn = 0;
        proven_u16 p = u->port;
        do { digits[dn++] = (char)('0' + p % 10); p /= 10; } while (p);
        while (dn > 0) host[hl++] = (proven_byte_t)digits[--dn];
    }
    if (e == PROVEN_OK) e = proven_http_write_header(out, len, cl_lit("Host"), (proven_u8str_view_t){ .ptr = host, .size = hl });
    for (proven_size_t i = 0; i < o->header_count && e == PROVEN_OK; ++i) {
        if (cl_is_reserved_header(o->headers[i].name)) return PROVEN_ERR_INVALID_ARG;
        e = proven_http_write_header(out, len, o->headers[i].name, o->headers[i].value);
    }
    bool body_method = cl_eq_nocase(method, cl_lit("POST")) || cl_eq_nocase(method, cl_lit("PUT")) || cl_eq_nocase(method, cl_lit("PATCH"));
    if (e == PROVEN_OK) {
        if (o->body.size > 0) e = proven_http_write_header_u64(out, len, cl_lit("Content-Length"), o->body.size);
        else if (o->body_length == PROVEN_HTTP_EVENT_LENGTH_UNKNOWN) e = proven_http_write_header(out, len, cl_lit("Transfer-Encoding"), cl_lit("chunked"));
        else if (o->body_length > 0 || body_method) e = proven_http_write_header_u64(out, len, cl_lit("Content-Length"), o->body_length);
    }
    if (e == PROVEN_OK) e = proven_http_write_header(out, len, cl_lit("Connection"), cl_lit("close"));
    if (e == PROVEN_OK) e = proven_http_write_head_end(out, len);
    return e;
}

proven_err_t proven_http_event_client_start(proven_http_event_client_t *cl, const proven_http_event_request_options_t *o,
                                            proven_http_event_request_t **out) {
    if (out) *out = (void *)0;
    if (!cl || !o || !o->on.on_done || (o->header_count > 0 && !o->headers) || (o->body.size > 0 && !o->body.ptr)) return PROVEN_ERR_INVALID_ARG;
    proven_url_t u;
    if (proven_url_parse(o->url, &u) != PROVEN_OK || u.host.size == 0 || u.has_userinfo) return PROVEN_ERR_INVALID_ARG;
    bool https = proven_url_scheme_is(&u, cl_lit("https"));
    if (!https && !proven_url_scheme_is(&u, cl_lit("http"))) return PROVEN_ERR_INVALID_ARG;
    if (https && !cl->cfg.tls) return PROVEN_ERR_INVALID_ARG;      /* never in the clear instead */
    proven_u8str_view_t method = o->method.size ? o->method : cl_lit("GET");
    if (cl_eq_nocase(method, cl_lit("CONNECT"))) return PROVEN_ERR_INVALID_ARG;

    proven_net_addr_t to;
    if (o->address) to = *o->address;
    else if (proven_net_addr_parse(u.host, u.has_port ? u.port : proven_url_default_port(u.scheme), &to) != PROVEN_OK) return PROVEN_ERR_INVALID_ARG;

    proven_size_t head_len = 0;
    proven_err_t e = cl_build_head(cl, o, &u, method, &head_len);
    if (e != PROVEN_OK) return e;

    proven_http_event_request_t *c = cl_alloc(cl->alloc, sizeof *c);
    if (!c) return PROVEN_ERR_NOMEM;
    *c = (proven_http_event_request_t){
        .client = cl, .on = o->on, .ctx = o->ctx, .state = CL_CONNECTING,
        .method = cl_eq_nocase(method, cl_lit("HEAD")) ? PROVEN_HTTP_HEAD : PROVEN_HTTP_GET,
    };
    if (o->body.size == 0 && o->body_length != 0) {
        c->streamed = true;
        c->chunked = o->body_length == PROVEN_HTTP_EVENT_LENGTH_UNKNOWN;
        c->send_left = o->body_length;
    } else {
        c->request_ended = true;
    }
    if (!buf_append(cl, &c->out, cl->hbuf, head_len) || !buf_append(cl, &c->out, o->body.ptr, o->body.size)) e = PROVEN_ERR_NOMEM;
    if (e == PROVEN_OK && https) e = proven_tls_client_create(cl->cfg.tls, u.host, (void *)0, &c->tls);
    if (e == PROVEN_OK) {
        e = proven_net_connect_start(to, &c->sock);
        if (e == PROVEN_OK) c->connected_at_start = true;
        else if (e == PROVEN_ERR_AGAIN) e = PROVEN_OK;
    }
    /* Either way the socket is watched for writing: what follows happens from the loop, so
     * that nothing - not even a refusal - is reported from inside this call. */
    if (e == PROVEN_OK) e = proven_loop_io_add(cl->loop, &c->io, proven_net_conn_handle(&c->sock), PROVEN_NET_WRITABLE, cl_on_io, c);
    if (e != PROVEN_OK) {
        if (c->tls) proven_tls_conn_destroy(c->tls);
        if (proven_net_conn_is_open(&c->sock)) (void)proven_net_close(&c->sock);
        buf_release(cl, &c->out);
        cl->alloc.free_fn(cl->alloc.ctx, c);
        return e;
    }
    c->next = cl->requests;
    if (c->next) c->next->prev = c;
    cl->requests = c;
    cl->request_count++;
    cl_arm(c);
    if (out) *out = c;
    return PROVEN_OK;
}

proven_result_size_t proven_http_event_request_write(proven_http_event_request_t *c, proven_mem_view_t data) {
    if (!c || (data.size > 0 && !data.ptr)) return (proven_result_size_t){ .err = PROVEN_ERR_INVALID_ARG, .value = 0 };
    if (c->dead || !c->streamed || c->request_ended) return (proven_result_size_t){ .err = PROVEN_ERR_INVALID_STATE, .value = 0 };
    if (!c->chunked && data.size > c->send_left) return (proven_result_size_t){ .err = PROVEN_ERR_OUT_OF_BOUNDS, .value = 0 };
    proven_http_event_client_t *cl = c->client;
    cl_enter(c);
    proven_size_t done = 0;
    bool alive = true, tried = false;
    /* Take what fits under the limit, push it at the socket, and look again: a refusal must
     * mean that the socket itself is not taking more (or that there is no connection yet). */
    while (alive && done < data.size) {
        proven_size_t held = cl_buffered(c);
        proven_size_t room = held < cl->cfg.max_buffered_output ? cl->cfg.max_buffered_output - held : 0;
        if (room == 0) {
            if (tried || c->state == CL_CONNECTING || !cl_secured(c)) break;
            tried = true;
            alive = cl_flush(c);
            continue;
        }
        proven_size_t n = data.size - done < room ? data.size - done : room;
        if (c->chunked) {
            proven_byte_t frame[24];
            proven_size_t fl = 0;
            (void)proven_http_write_chunk_begin((proven_mem_mut_t){ .ptr = frame, .size = sizeof frame }, &fl, n);
            /* Queued together so that a chunk is never left half-framed. */
            alive = buf_append(cl, &c->out, frame, fl) && buf_append(cl, &c->out, data.ptr + done, n) && buf_append(cl, &c->out, (const proven_byte_t *)"\r\n", 2);
        } else {
            alive = buf_append(cl, &c->out, data.ptr + done, n);
            if (alive) c->send_left -= n;
        }
        if (!alive) cl_kill(c, PROVEN_ERR_NOMEM);
        else alive = cl_flush(c);
        done += n;
        tried = true;                              /* every pass through here ends with a flush */
    }
    if (alive) {
        if (done < data.size) c->want_writable = true;
        cl_watch(c);
        cl_arm(c);
    }
    cl_leave(c);
    return alive ? (proven_result_size_t){ .err = PROVEN_OK, .value = done } : (proven_result_size_t){ .err = PROVEN_ERR_RESET, .value = 0 };
}

proven_err_t proven_http_event_request_end(proven_http_event_request_t *c) {
    if (!c) return PROVEN_ERR_INVALID_ARG;
    if (c->dead || !c->streamed || c->request_ended) return PROVEN_ERR_INVALID_STATE;
    cl_enter(c);
    proven_err_t result = PROVEN_OK;
    if (!c->chunked && c->send_left != 0) {
        /* Less was written than promised: the server would wait for the rest for ever. */
        cl_kill(c, PROVEN_ERR_INVALID_FORMAT);
        result = PROVEN_ERR_INVALID_FORMAT;
    } else {
        c->request_ended = true;
        bool alive = true;
        if (c->chunked) {
            alive = buf_append(c->client, &c->out, (const proven_byte_t *)"0\r\n\r\n", 5);
            if (!alive) cl_kill(c, PROVEN_ERR_NOMEM);
        }
        if (alive) alive = cl_flush(c);
        if (alive) cl_progress(c);
        else result = PROVEN_ERR_RESET;
    }
    cl_leave(c);
    return result;
}

void proven_http_event_request_abort(proven_http_event_request_t *c) {
    if (!c || c->dead) return;
    cl_enter(c);
    cl_kill(c, PROVEN_ERR_RESET);
    cl_leave(c);
}

void proven_http_event_request_pause(proven_http_event_request_t *c) {
    if (!c || c->dead || c->paused) return;
    c->paused = true;
    cl_watch(c);
    cl_arm(c);
}

void proven_http_event_request_resume(proven_http_event_request_t *c) {
    if (!c || c->dead || !c->paused) return;
    c->paused = false;
    cl_enter(c);
    cl_process(c);
    if (!c->dead) { cl_watch(c); cl_arm(c); }
    cl_leave(c);
}

void proven_http_event_request_set_user(proven_http_event_request_t *c, void *user) { if (c) c->user = user; }
void *proven_http_event_request_user(const proven_http_event_request_t *c) { return c ? c->user : (void *)0; }

#endif
