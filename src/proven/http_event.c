#include "proven/http_event.h"
#include "../../platform/proven_sys_mem.h"
#include "proven_internal_http_event.h"
#include "proven_internal_http_coding.h"

#if !defined(PROVEN_FREESTANDING) && !defined(PROVEN_NO_NET)

#include "proven/time.h"
#include "proven/h2.h"
#include "proven_internal_tls.h"

/*
 * The event-driven HTTP/1.1 server.
 *
 * One struct per connection and no buffer that outlives its use. Bytes are read into a buffer
 * the server owns and parsed where they lie; only what cannot be used yet - half a request
 * head, a pipelined request behind one being answered - is copied aside. Output is written to
 * the socket at once, and only what the socket would not take is kept. So a connection with
 * nothing in flight is the struct below and, with TLS, the engine's keys.
 *
 * Re-entrancy is the thing to keep in mind when changing this. A callback may respond, end,
 * abort, pause or resume, and any of those may finish the exchange and start on the next
 * pipelined request, or close the connection. So:
 *   - processing is one loop (ev_process) that is never entered twice for a connection: a
 *     nested call just returns, and the outer loop notices what changed;
 *   - nothing reads a field after a callback without having re-checked `dead`;
 *   - a connection is freed only when the outermost entry leaves (`depth`).
 */

static proven_u8str_view_t ev_lit_n(const char *s, proven_size_t n) { return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)s, .size = n }; }
#define ev_lit(s) ev_lit_n((s), sizeof(s) - 1)


static void *ev_alloc(proven_allocator_t a, proven_size_t n) {
    proven_result_mem_mut_t m = a.alloc_fn(a.ctx, n, 16);
    return m.err == PROVEN_OK ? m.value.ptr : (void *)0;
}

// -----------------------------------------------------------------------------
// Buffers
// -----------------------------------------------------------------------------

static proven_size_t buf_pending(const proven_ev_buf_t *b) { return b->len - b->off; }

static void buf_release(proven_http_event_server_t *s, proven_ev_buf_t *b) {
    if (b->ptr && !b->borrowed) s->alloc.free_fn(s->alloc.ctx, b->ptr);
    *b = (proven_ev_buf_t){ 0 };
}

/* Make the buffer own its pending bytes, with room for `more` behind them. */
static bool buf_own(proven_http_event_server_t *s, proven_ev_buf_t *b, proven_size_t more) {
    proven_size_t have = buf_pending(b);
    if (!b->borrowed && b->off == 0 && b->cap >= have + more) return true;
    if (!b->borrowed && b->cap >= have + more) {
        proven_sys_mem_move(b->ptr, b->ptr + b->off, have);
        b->len = have; b->off = 0;
        return true;
    }
    proven_size_t cap = 256;
    while (cap < have + more) { if (cap > (proven_size_t)-1 / 2) return false; cap *= 2; }
    proven_byte_t *p = ev_alloc(s->alloc, cap);
    if (!p) return false;
    proven_sys_mem_copy(p, b->ptr + b->off, have);
    if (b->ptr && !b->borrowed) s->alloc.free_fn(s->alloc.ctx, b->ptr);
    b->ptr = p; b->len = have; b->off = 0; b->cap = cap; b->borrowed = false;
    return true;
}

static bool buf_append(proven_http_event_server_t *s, proven_ev_buf_t *b, const proven_byte_t *p, proven_size_t n) {
    if (n == 0) return true;
    if (!buf_own(s, b, n)) return false;
    proven_sys_mem_copy(b->ptr + b->len, p, n);
    b->len += n;
    return true;
}

// -----------------------------------------------------------------------------
// A connection's life
// -----------------------------------------------------------------------------

static void ev_process(proven_http_stream_t *c);
static void ev_progress(proven_http_stream_t *c);
static void ev_update_listeners(proven_http_event_server_t *s);
static void ev_on_io(void *ctx, proven_u8 got);
static void ev_on_timer(void *ctx);

static void ev_enter(proven_http_stream_t *c) { c->h.depth++; }

static void ev_leave(proven_http_stream_t *c) {
    if (--c->h.depth > 0 || !c->dead) return;
    proven_http_event_server_t *s = c->h.server;
    buf_release(s, &c->stash);
    buf_release(s, &c->out);
    s->alloc.free_fn(s->alloc.ctx, c);
}

/* A shutdown is over when no connection is left: on_drained, from the loop's next round -
 * never from inside the call that ended the last one, which may be a handler's. */
static void ev_drain_done(void *ctx) {
    proven_http_event_server_t *s = ctx;
    if (s->drain_told) return;
    s->drain_told = true;
    proven_loop_timer_cancel(s->loop, &s->drain_deadline);
    if (s->on_drained) s->on_drained(s->drained_ctx, s->drain_cut ? PROVEN_ERR_TIMEOUT : PROVEN_OK);
}

static void ev_drain_check(proven_http_event_server_t *s) {
    if (s->draining && !s->drain_told && !s->stopped && s->conn_count == 0 && s->h3_count == 0 && !proven_loop_timer_is_set(&s->drain_done))
        proven_loop_timer_set(s->loop, &s->drain_done, 0, ev_drain_done, s);
}

/* End the connection now. `why` goes to on_done if an exchange was in progress. */
static void ev_kill(proven_http_stream_t *c, proven_err_t why) {
    if (c->dead) return;
    proven_http_event_server_t *s = c->h.server;
    c->dead = true;
    proven_loop_timer_cancel(s->loop, &c->timer);
    proven_loop_io_remove(s->loop, &c->io);           /* before the close */
    if (c->tls) { proven_tls_conn_destroy(c->tls); c->tls = (void *)0; }
    if (c->h.state != EV_RAW && c->zip) { proven_http_compressor_destroy_(c->zip); c->zip = (void *)0; }
    (void)proven_net_close(&c->sock);
    if (c->prev) c->prev->next = c->next;
    else s->conns = c->next;
    if (c->next) c->next->prev = c->prev;
    s->conn_count--;
    ev_update_listeners(s);
    ev_drain_check(s);
    if (c->active) {
        c->active = false;
        if (s->cfg.on.on_done) s->cfg.on.on_done(s->cfg.ctx, c, why);
    }
    if (c->h.state == EV_RAW && c->raw) {
        void *raw = c->raw;
        c->raw = (void *)0;
        (*(const proven_http_event_raw_ops_t *const *)raw)->on_closed(raw, why);
    }
}

static proven_size_t ev_buffered(const proven_http_stream_t *c) {
    proven_size_t n = buf_pending(&c->out);
    if (c->tls) n += proven_tls_pending_output(c->tls).size;
    return n;
}

/* Which timer a connection needs depends on what it is waiting for. */
static void ev_arm(proven_http_stream_t *c) {
    if (c->dead) return;
    proven_http_event_server_t *s = c->h.server;
    if (c->h.state == EV_RAW) return;                /* its owner sets the timer */
    proven_u32 ms = 0;
    /* Output the client has not taken comes first: a response on its way out is not cut
     * short by the brief wait that follows it. */
    if (ev_buffered(c) > 0) ms = s->cfg.write_timeout_ms;
    else if (c->h.state == EV_LINGER) ms = c->h.server->cfg.linger_ms;
    else if (c->h.state == EV_HEAD) ms = c->head_started ? s->cfg.head_timeout_ms : s->cfg.idle_timeout_ms;
    else if (c->h.state == EV_BODY && !c->paused) ms = s->cfg.body_timeout_ms;
    /* EV_RESPOND with nothing held: the handler is thinking. That is its business. */
    if (ms == 0) proven_loop_timer_cancel(s->loop, &c->timer);
    else proven_loop_timer_set(s->loop, &c->timer, ms, ev_on_timer, c);
}

static void ev_watch(proven_http_stream_t *c) {
    if (c->dead) return;
    proven_u8 want = 0;
    bool reading = !c->peer_eof && !((c->h.state == EV_BODY || c->h.state == EV_RAW) && c->paused) &&
                   /* while a response is owed, a pipelined request is left in the socket */
                   !(c->h.state == EV_RESPOND && buf_pending(&c->stash) > 0);
    if (reading) want |= PROVEN_NET_READABLE;
    if (ev_buffered(c) > 0 || (c->h.state == EV_RAW && c->raw_more)) want |= PROVEN_NET_WRITABLE;
    (void)proven_loop_io_set(c->h.server->loop, &c->io, want);
}

/* Push what is waiting towards the socket, without waiting. False when the connection died. */
static bool ev_flush(proven_http_stream_t *c) {
    for (;;) {
        if (c->tls && buf_pending(&c->out) > 0 && proven_tls_is_established(c->tls)) {
            proven_result_size_t w = proven_tls_write(c->tls, (proven_mem_view_t){ .ptr = c->out.ptr + c->out.off, .size = buf_pending(&c->out) });
            if (w.err != PROVEN_OK) { ev_kill(c, PROVEN_ERR_RESET); return false; }
            c->out.off += w.value;
        }
        proven_mem_view_t pending = c->tls ? proven_tls_pending_output(c->tls)
                                           : (proven_mem_view_t){ .ptr = c->out.ptr + c->out.off, .size = buf_pending(&c->out) };
        if (pending.size == 0) break;
        proven_result_size_t w = proven_net_write(&c->sock, pending, PROVEN_NET_DONT_WAIT);
        if (w.err == PROVEN_ERR_TIMEOUT || w.err == PROVEN_ERR_AGAIN) break;
        if (w.err != PROVEN_OK) { ev_kill(c, PROVEN_ERR_RESET); return false; }
        if (c->tls) proven_tls_output_sent(c->tls, w.value);
        else c->out.off += w.value;
        if (w.value == 0) break;
    }
    if (buf_pending(&c->out) == 0) buf_release(c->h.server, &c->out);
    return true;
}

/* Queue bytes for the client and send what goes at once. False when the connection died. */
static bool ev_send(proven_http_stream_t *c, const proven_byte_t *p, proven_size_t n) {
    if (c->dead) return false;
    if (!c->tls && buf_pending(&c->out) == 0 && n > 0) {
        /* Nothing is waiting: offer it to the socket directly and keep only the remainder. */
        proven_result_size_t w = proven_net_write(&c->sock, (proven_mem_view_t){ .ptr = p, .size = n }, PROVEN_NET_DONT_WAIT);
        if (w.err == PROVEN_OK) { p += w.value; n -= w.value; }
        else if (w.err != PROVEN_ERR_TIMEOUT && w.err != PROVEN_ERR_AGAIN) { ev_kill(c, PROVEN_ERR_RESET); return false; }
    }
    if (!buf_append(c->h.server, &c->out, p, n)) { ev_kill(c, PROVEN_ERR_NOMEM); return false; }
    return ev_flush(c);
}

/* Close politely: say that nothing more will come, then read and discard until the client
 * closes or a moment has passed - a socket closed over unread input resets, and a reset can
 * take the response that was just sent with it. */
static void ev_linger(proven_http_stream_t *c) {
    if (c->dead) return;
    if (c->h.server->stopped) { ev_kill(c, PROVEN_ERR_RESET); return; }
    c->h.state = EV_LINGER;
    c->stash.len = 0; c->stash.off = 0;
    if (ev_buffered(c) > 0) { c->close_when_flushed = true; ev_watch(c); ev_arm(c); return; }
    if (c->tls) { (void)proven_tls_close(c->tls); if (!ev_flush(c)) return; }
    (void)proven_net_shutdown_write(&c->sock);
    if (c->peer_eof) { ev_kill(c, PROVEN_OK); return; }
    ev_watch(c);
    ev_arm(c);
}

/* The response is complete and gone: finish the exchange and go on to whatever is next. */
static void ev_finish(proven_http_stream_t *c) {
    proven_http_event_server_t *s = c->h.server;
    /* After stop_listening (or a shutdown) a connection is not kept, even when its response
     * began before and promised to keep it: it ends here. */
    bool reuse = c->keep && c->h.state == EV_RESPOND && !c->peer_eof && !s->stopped && !s->closed_to_new;
    c->active = false;
    ev_enter(c);
    if (s->cfg.on.on_done) s->cfg.on.on_done(s->cfg.ctx, c, PROVEN_OK);
    if (!c->dead) {
        c->user = (void *)0;
        c->response_begun = false; c->response_ended = false; c->chunked = false; c->bodiless = false;
        c->want_writable = false; c->paused = false; c->expect_continue = false;
        c->compress_asked = false;
        if (c->zip) { proven_http_compressor_destroy_(c->zip); c->zip = (void *)0; }
        if (reuse) {
            c->h.state = EV_HEAD;
            c->head_started = buf_pending(&c->stash) > 0;
            ev_watch(c);
            ev_arm(c);
            ev_process(c);                         /* a pipelined request may be waiting */
        } else {
            ev_linger(c);
        }
    }
    ev_leave(c);
}

/* Something was sent, or the state moved: see what that allows. */
static void ev_progress(proven_http_stream_t *c) {
    if (c->dead) return;
    proven_http_event_server_t *s = c->h.server;
    ev_enter(c);
    if (c->h.state == EV_LINGER) {
        if (c->close_when_flushed && ev_buffered(c) == 0) { c->close_when_flushed = false; ev_linger(c); }
    } else if (c->h.state == EV_RAW) {
        if (c->raw) (*(const proven_http_event_raw_ops_t *const *)c->raw)->on_progress(c->raw);
    } else if (c->active && c->response_ended && ev_buffered(c) == 0) {
        ev_finish(c);
    } else if (c->active && !c->response_ended && c->want_writable && ev_buffered(c) < s->cfg.max_buffered_output) {
        c->want_writable = false;
        if (s->cfg.on.on_writable) s->cfg.on.on_writable(s->cfg.ctx, c);
    }
    if (!c->dead) { ev_watch(c); ev_arm(c); }
    ev_leave(c);
}

/* Refuse a request the server will not pass on. */
static void ev_reject(proven_http_stream_t *c, proven_u16 status, proven_err_t why) {
    proven_http_event_server_t *s = c->h.server;
    if (!c->response_begun) {
        proven_byte_t msg[128];
        proven_mem_mut_t out = { .ptr = msg, .size = sizeof msg };
        proven_size_t len = 0;
        proven_err_t e = proven_http_write_status_line(out, &len, status, ev_lit(""));
        if (e == PROVEN_OK) e = proven_http_write_header(out, &len, ev_lit("Content-Length"), ev_lit("0"));
        if (e == PROVEN_OK) e = proven_http_write_header(out, &len, ev_lit("Connection"), ev_lit("close"));
        if (e == PROVEN_OK) e = proven_http_write_head_end(out, &len);
        if (e == PROVEN_OK && !ev_send(c, msg, len)) return;
    }
    c->keep = false;
    if (c->active) {
        c->active = false;
        ev_enter(c);
        if (s->cfg.on.on_done) s->cfg.on.on_done(s->cfg.ctx, c, why);
        ev_leave(c);
        if (c->dead) return;
    }
    ev_linger(c);
}

// -----------------------------------------------------------------------------
// Input
// -----------------------------------------------------------------------------

static void ev_process(proven_http_stream_t *c) {
    proven_http_event_server_t *s = c->h.server;
    if (c->processing || c->dead) return;         /* the outer call will see what changed */
    c->processing = true;
    ev_enter(c);
    while (!c->dead) {
        proven_mem_view_t in = { .ptr = c->stash.ptr + c->stash.off, .size = buf_pending(&c->stash) };
        if (c->h.state == EV_LINGER) { c->stash.len = 0; c->stash.off = 0; break; }
        if (c->h.state == EV_RESPOND) break;        /* what follows belongs to the next request */
        if (c->h.state == EV_RAW) {
            if (c->paused || in.size == 0 || !c->raw) break;
            proven_size_t used = (*(const proven_http_event_raw_ops_t *const *)c->raw)->on_input(
                c->raw, (proven_mem_mut_t){ .ptr = c->stash.ptr + c->stash.off, .size = in.size });
            if (c->dead || c->h.state != EV_RAW) break;      /* closed from inside: the input is void */
            c->stash.off += used;
            if (used < in.size) break;
            continue;
        }
        if (c->h.state == EV_HEAD) {
            if (in.size == 0) break;
            c->head_started = true;
            if (s->cfg.http2 && !c->sniffed) {
                /* Once, before anything is parsed. Over TLS the handshake has said (ALPN). In
                 * the clear a client that knows begins with 24 fixed bytes, and what has
                 * arrived either is the beginning of those or is not: "PR" is, "PO" is POST. */
                static const char preface[] = PROVEN_H2_PREFACE;
                bool h2 = false;
                if (c->tls) {
                    proven_u8str_view_t chosen = proven_tls_alpn(c->tls);
                    h2 = chosen.size == 2 && chosen.ptr[0] == 'h' && chosen.ptr[1] == '2';
                } else {
                    proven_size_t n = in.size < PROVEN_H2_PREFACE_SIZE ? in.size : PROVEN_H2_PREFACE_SIZE, same = 0;
                    while (same < n && in.ptr[same] == (proven_byte_t)preface[same]) same++;
                    if (same == n && n < PROVEN_H2_PREFACE_SIZE) break;        /* so far it is: wait for the rest */
                    h2 = same == PROVEN_H2_PREFACE_SIZE;
                }
                c->sniffed = true;
                if (h2) {
                    if (proven_http_event_h2_take_(c) != PROVEN_OK) { ev_kill(c, PROVEN_ERR_NOMEM); break; }
                    continue;                      /* the same bytes, to their new owner */
                }
            }
            proven_http_request_t req;
            proven_size_t head = 0;
            proven_err_t e = proven_http_parse_request(in, s->headers, s->cfg.max_headers, s->cfg.max_head_bytes, &req, &head);
            if (e == PROVEN_ERR_NEED_MORE) {
                if (in.size >= s->cfg.max_head_bytes) { ev_reject(c, 431, PROVEN_ERR_OUT_OF_BOUNDS); }
                break;
            }
            proven_u16 refuse = 0;
            proven_http_framing_t framing = { 0 };
            if (e == PROVEN_ERR_OUT_OF_BOUNDS) refuse = 431;
            else if (e == PROVEN_ERR_UNSUPPORTED) refuse = 505;
            else if (e != PROVEN_OK) refuse = 400;
            if (!refuse) {
                e = proven_http_request_framing(&req, &framing);
                if (e == PROVEN_ERR_UNSUPPORTED) refuse = 501;
                else if (e != PROVEN_OK) refuse = 400;
            }
            if (!refuse && proven_http_body_init(&c->body, framing, s->cfg.max_body_bytes) != PROVEN_OK) refuse = 413;
            /* HTTP/1.1 requires Host; a server that guesses is the back end a smuggled request is aimed at. */
            if (!refuse && req.version_minor >= 1 && proven_http_header_count(s->headers, req.header_count, ev_lit("Host")) != 1) refuse = 400;
            if (refuse) { ev_reject(c, refuse, PROVEN_ERR_INVALID_FORMAT); break; }

            bool no_body = framing.kind == PROVEN_HTTP_BODY_NONE || (framing.kind == PROVEN_HTTP_BODY_LENGTH && framing.length == 0);
            c->method = req.method;
            c->h.version_minor = (proven_u8)req.version_minor;
            c->keep = proven_http_request_keep_alive(&req);
            c->compress_coding = (proven_u8)proven_http_compress_choose_(s->headers, req.header_count, s->cfg.compress_zstd);
            c->compress_asked = false;
            c->expect_continue = req.version_minor >= 1 && !no_body && proven_http_header_has_token(s->headers, req.header_count, ev_lit("Expect"), ev_lit("100-continue"));
            c->h.state = no_body ? EV_RESPOND : EV_BODY;
            c->head_started = false;
            c->active = true;
            c->stash.off += head;                 /* consumed; the views stay good until this loop ends */
            ev_arm(c);
            s->cfg.on.on_request(s->cfg.ctx, c, &req);
            if (c->dead) break;
            if (c->h.state == EV_BODY && c->expect_continue && !c->response_begun) {
                static const char go_on[] = "HTTP/1.1 100 Continue\r\n\r\n";
                c->expect_continue = false;
                if (!ev_send(c, (const proven_byte_t *)go_on, sizeof go_on - 1)) break;
            }
            ev_progress(c);
            continue;
        }
        /* EV_BODY */
        if (c->paused || in.size == 0) break;
        proven_size_t used = 0;
        proven_mem_view_t payload = { 0 };
        bool done = false;
        proven_err_t e = proven_http_body_feed(&c->body, in, &used, &payload, &done);
        if (e != PROVEN_OK) { ev_reject(c, e == PROVEN_ERR_OUT_OF_BOUNDS ? 413 : 400, e); break; }
        c->stash.off += used;
        if (done) c->h.state = EV_RESPOND;
        if ((payload.size > 0 || done) && s->cfg.on.on_body) s->cfg.on.on_body(s->cfg.ctx, c, payload, done);
        if (c->dead) break;
        ev_progress(c);
        if (c->dead) break;
        if (used == 0 && !done) break;            /* it needs more than is here */
    }
    if (!c->dead) {
        /* Keep only what could not be used; a connection with nothing pending holds no buffer. */
        if (buf_pending(&c->stash) == 0) buf_release(s, &c->stash);
        else if (!buf_own(s, &c->stash, 0)) ev_kill(c, PROVEN_ERR_NOMEM);
    }
    c->processing = false;
    if (!c->dead) { ev_watch(c); ev_arm(c); }
    ev_leave(c);
}

/* Bytes of the request stream (after TLS, if any) have arrived in a buffer of the server's. */
static void ev_input(proven_http_stream_t *c, const proven_byte_t *p, proven_size_t n) {
    proven_http_event_server_t *s = c->h.server;
    if (c->dead || n == 0) return;
    if (c->h.state == EV_LINGER) return;
    if (buf_pending(&c->stash) == 0) {
        /* The common case: parse where the bytes lie, and copy aside only what is left over. */
        buf_release(s, &c->stash);
        c->stash = (proven_ev_buf_t){ .ptr = (proven_byte_t *)(proven_uintptr_t)p, .len = n, .cap = 0, .off = 0, .borrowed = true };
    } else if (!buf_append(s, &c->stash, p, n)) {
        ev_kill(c, PROVEN_ERR_NOMEM);
        return;
    }
    ev_process(c);
}

/* The peer will send nothing more to a connection that another protocol owns. The owner says
 * whether what it still owes is to be written; if not, this is the end. False when it was. */
static bool ev_raw_eof(proven_http_stream_t *c) {
    const proven_http_event_raw_ops_t *ops = c->raw ? *(const proven_http_event_raw_ops_t *const *)c->raw : (void *)0;
    if (ops && ops->on_eof && ops->on_eof(c->raw) && !c->dead) return true;
    if (!c->dead) ev_kill(c, PROVEN_ERR_EOF);
    return false;
}

static void ev_readable(proven_http_stream_t *c) {
    proven_http_event_server_t *s = c->h.server;
    proven_result_size_t r = proven_net_read(&c->sock, (proven_mem_mut_t){ .ptr = s->rbuf, .size = EV_READ_BYTES }, PROVEN_NET_DONT_WAIT);
    if (r.err == PROVEN_ERR_TIMEOUT || r.err == PROVEN_ERR_AGAIN) return;
    if (r.err == PROVEN_ERR_EOF) {
        /* A client may close its sending side and still wait for the answer. */
        c->peer_eof = true;
        if (c->h.state == EV_RAW) { if (ev_raw_eof(c)) ev_watch(c); return; }
        if (c->h.state == EV_LINGER || !c->active || c->h.state == EV_BODY) { ev_kill(c, c->active ? PROVEN_ERR_RESET : PROVEN_OK); return; }
        c->keep = false;
        ev_watch(c);
        return;
    }
    if (r.err != PROVEN_OK) { ev_kill(c, PROVEN_ERR_RESET); return; }
    if (!c->tls) { ev_input(c, s->rbuf, r.value); return; }
    proven_size_t at = 0;
    while (at < r.value && !c->dead) {
        proven_size_t used = 0;
        proven_err_t e = proven_tls_feed(c->tls, (proven_mem_view_t){ .ptr = s->rbuf + at, .size = r.value - at }, &used);
        at += used;
        if (e != PROVEN_OK) {
            (void)ev_flush(c);                     /* the alert */
            ev_kill(c, e);
            return;
        }
        for (;;) {
            proven_result_size_t got = proven_tls_read(c->tls, (proven_mem_mut_t){ .ptr = s->pbuf, .size = EV_PLAIN_BYTES });
            if (got.err == PROVEN_ERR_EOF) {
                c->peer_eof = true;
                if (c->h.state == EV_RAW) { if (!ev_raw_eof(c)) return; break; }
                if (c->h.state == EV_LINGER || !c->active || c->h.state == EV_BODY) { ev_kill(c, c->active ? PROVEN_ERR_RESET : PROVEN_OK); return; }
                c->keep = false;
                break;
            }
            if (got.err != PROVEN_OK) break;
            ev_input(c, s->pbuf, got.value);
            if (c->dead) return;
        }
        if (c->peer_eof) break;
        if (used == 0) break;
    }
    if (!c->dead) { if (ev_flush(c)) ev_progress(c); }
}

static void ev_on_io(void *ctx, proven_u8 got) {
    proven_http_stream_t *c = ctx;
    ev_enter(c);
    if (!c->dead && (got & PROVEN_NET_WRITABLE)) { if (ev_flush(c)) ev_progress(c); }
    if (!c->dead && (got & (PROVEN_NET_READABLE | PROVEN_NET_FAILED))) ev_readable(c);
    ev_leave(c);
}

static void ev_on_timer(void *ctx) {
    proven_http_stream_t *c = ctx;
    ev_enter(c);
    if (c->h.state == EV_RAW) { if (c->raw) (*(const proven_http_event_raw_ops_t *const *)c->raw)->on_timer(c->raw); }
    else if (c->h.state == EV_HEAD && c->head_started && ev_buffered(c) == 0) ev_reject(c, 408, PROVEN_ERR_TIMEOUT);
    else ev_kill(c, c->active ? PROVEN_ERR_TIMEOUT : PROVEN_OK);
    ev_leave(c);
}

// -----------------------------------------------------------------------------
// Accepting
// -----------------------------------------------------------------------------

static void ev_update_listeners(proven_http_event_server_t *s) {
    bool want = !s->stopped && s->conn_count + s->h3_count < s->cfg.max_connections;
    if (want == s->accepting) return;
    for (proven_size_t i = 0; i < s->listener_count; ++i) (void)proven_loop_io_set(s->loop, &s->listeners[i].io, want ? PROVEN_NET_READABLE : 0);
    s->accepting = want;
}

/* Make a connection of the server's out of a socket. On failure the socket is untouched. */
static proven_err_t ev_take(proven_http_event_server_t *s, const proven_net_conn_t *sock, proven_net_addr_t peer) {
    proven_http_stream_t *c = ev_alloc(s->alloc, sizeof *c);
    if (!c) return PROVEN_ERR_NOMEM;
    for (proven_size_t i = 0; i < sizeof *c; ++i) ((proven_byte_t *)c)[i] = 0;
    c->h.server = s;
    c->sock = *sock;
    c->peer = peer;
    c->h.state = EV_HEAD;
    c->head_started = true;                        /* a new connection is expected to speak promptly */
    proven_err_t e = PROVEN_OK;
    if (s->cfg.tls) e = proven_tls_server_create(s->cfg.tls, &c->tls);
    if (e == PROVEN_OK) e = proven_loop_io_add(s->loop, &c->io, proven_net_conn_handle(&c->sock), PROVEN_NET_READABLE, ev_on_io, c);
    if (e != PROVEN_OK) {
        if (c->tls) proven_tls_conn_destroy(c->tls);
        s->alloc.free_fn(s->alloc.ctx, c);
        return e;
    }
    (void)proven_net_conn_set_nodelay(&c->sock, true);
    proven_net_addr_t local;
    if (proven_net_conn_local_addr(&c->sock, &local) == PROVEN_OK) c->local_port = local.port;
    c->next = s->conns;
    if (s->conns) s->conns->prev = c;
    s->conns = c;
    s->conn_count++;
    ev_arm(c);
    return PROVEN_OK;
}

static void ev_on_accept(void *ctx, proven_u8 got) {
    proven_ev_listener_t *l = ctx;
    proven_http_event_server_t *s = l->server;
    (void)got;
    while (s->conn_count + s->h3_count < s->cfg.max_connections) {
        proven_net_conn_t sock;
        proven_net_addr_t peer;
        if (proven_net_accept(&l->listener, PROVEN_NET_DONT_WAIT, &sock, &peer) != PROVEN_OK) break;
        if (ev_take(s, &sock, peer) != PROVEN_OK) { (void)proven_net_close(&sock); break; }
    }
    ev_update_listeners(s);
}

proven_err_t proven_http_event_server_adopt(proven_http_event_server_t *s, proven_net_conn_t *conn) {
    if (!s || !conn) return PROVEN_ERR_INVALID_ARG;
    if (!proven_net_conn_is_open(conn) || s->stopped || s->closed_to_new) return PROVEN_ERR_INVALID_STATE;
    if (s->conn_count + s->h3_count >= s->cfg.max_connections) return PROVEN_ERR_BUSY;
    proven_net_addr_t peer = { 0 };
    (void)proven_net_conn_peer_addr(conn, &peer);
    proven_err_t e = ev_take(s, conn, peer);
    if (e != PROVEN_OK) return e;
    *conn = (proven_net_conn_t){ 0 };              /* the server's now */
    ev_update_listeners(s);
    return PROVEN_OK;
}

// -----------------------------------------------------------------------------
// The server object
// -----------------------------------------------------------------------------

proven_err_t proven_http_event_server_create(proven_loop_t *loop, const proven_http_event_server_config_t *config,
                                             proven_http_event_server_t **out) {
    if (out) *out = (void *)0;
    if (!loop || !config || !out || !config->on.on_request) return PROVEN_ERR_INVALID_ARG;
    proven_allocator_t a = proven_alloc_is_valid(config->alloc) ? config->alloc : proven_loop_allocator(loop);
    proven_http_event_server_t *s = ev_alloc(a, sizeof *s);
    if (!s) return PROVEN_ERR_NOMEM;
    for (proven_size_t i = 0; i < sizeof *s; ++i) ((proven_byte_t *)s)[i] = 0;
    s->loop = loop;
    s->alloc = a;
    s->cfg = *config;
    const proven_http_compress_cfg_t_ cc = proven_http_event_compress_cfg_(&s->cfg);
    if (!proven_http_compress_cfg_valid_(&cc)) {
        a.free_fn(a.ctx, s);
        return PROVEN_ERR_INVALID_ARG;
    }
    if (s->cfg.max_connections == 0) s->cfg.max_connections = 10000;
    if (s->cfg.max_head_bytes == 0) s->cfg.max_head_bytes = PROVEN_HTTP_DEFAULT_MAX_HEAD;
    if (s->cfg.max_head_bytes < 256) s->cfg.max_head_bytes = 256;
    if (s->cfg.max_headers == 0) s->cfg.max_headers = 64;
    if (s->cfg.max_body_bytes == 0) s->cfg.max_body_bytes = 1024 * 1024;
    if (s->cfg.max_buffered_output == 0) s->cfg.max_buffered_output = 64 * 1024;
    if (s->cfg.head_timeout_ms == 0) s->cfg.head_timeout_ms = 10000;
    if (s->cfg.body_timeout_ms == 0) s->cfg.body_timeout_ms = 30000;
    if (s->cfg.write_timeout_ms == 0) s->cfg.write_timeout_ms = 30000;
    if (s->cfg.idle_timeout_ms == 0) s->cfg.idle_timeout_ms = 60000;
    if (s->cfg.linger_ms == 0) s->cfg.linger_ms = EV_LINGER_MS;
    if (s->cfg.max_concurrent_streams == 0) s->cfg.max_concurrent_streams = 100;
    if (s->cfg.http2_no_header_table) s->cfg.http2_header_table_bytes = 0;
    else if (s->cfg.http2_header_table_bytes == 0) s->cfg.http2_header_table_bytes = 4096;
    if (s->cfg.http2_header_table_bytes > ((proven_size_t)1 << 20)) { a.free_fn(a.ctx, s); return PROVEN_ERR_INVALID_ARG; }
    if (s->cfg.http2_window_bytes > 0x7fffffffu || (s->cfg.http2_websocket && !s->cfg.http2)) { a.free_fn(a.ctx, s); return PROVEN_ERR_INVALID_ARG; }
#if defined(PROVEN_NO_HTTP3)
    if (s->cfg.http3) { a.free_fn(a.ctx, s); return PROVEN_ERR_UNSUPPORTED; }
#endif
    if (s->cfg.http3 && (!s->cfg.tls || s->cfg.http3_window_bytes > 0x7fffffffu)) { a.free_fn(a.ctx, s); return PROVEN_ERR_INVALID_ARG; }
    if (s->cfg.tls) {
        /* What TLS may agree to and what the server will speak must be the same thing: a
         * client told "h2" by the handshake and answered in HTTP/1.1 has no way to recover. */
        bool offers_h2 = false, offers_h3 = false;
        for (proven_size_t i = 0;; ++i) {
            proven_u8str_view_t name = proven_tls_config_alpn(s->cfg.tls, i);
            if (name.size == 0) break;
            if (name.size == 2 && name.ptr[0] == 'h' && name.ptr[1] == '2') offers_h2 = true;
            if (name.size == 2 && name.ptr[0] == 'h' && name.ptr[1] == '3') offers_h3 = true;
        }
        if (offers_h2 != s->cfg.http2 || offers_h3 != s->cfg.http3) { a.free_fn(a.ctx, s); return PROVEN_ERR_INVALID_ARG; }
    }
    s->headers = ev_alloc(a, s->cfg.max_headers * sizeof(proven_http_header_t));
    s->rbuf = ev_alloc(a, EV_READ_BYTES);
    s->hbuf = ev_alloc(a, s->cfg.max_head_bytes);
    if (s->cfg.tls) s->pbuf = ev_alloc(a, EV_PLAIN_BYTES);
    bool ok = s->headers && s->rbuf && s->hbuf && (!s->cfg.tls || s->pbuf);
    if (ok && s->cfg.tls) {
        /* A config that cannot serve is found now, not at the first connection. */
        proven_tls_conn_t *probe = (void *)0;
        proven_err_t e = proven_tls_server_create(s->cfg.tls, &probe);
        if (e != PROVEN_OK) { proven_http_event_server_destroy(s); return e; }
        proven_tls_conn_destroy(probe);
    }
    if (!ok) { proven_http_event_server_destroy(s); return PROVEN_ERR_NOMEM; }
#if !defined(PROVEN_NO_HTTP3)
    if (s->cfg.http3 && proven_http_event_h3_create_(s) != PROVEN_OK) { proven_http_event_server_destroy(s); return PROVEN_ERR_NOMEM; }
#endif
    *out = s;
    return PROVEN_OK;
}

proven_err_t proven_http_event_server_listen(proven_http_event_server_t *s, proven_net_addr_t at, proven_net_addr_t *bound) {
    if (!s) return PROVEN_ERR_INVALID_ARG;
    if (s->draining) return PROVEN_ERR_INVALID_STATE;
    if (s->listener_count == EV_MAX_LISTENERS) return PROVEN_ERR_OUT_OF_BOUNDS;
    proven_ev_listener_t *l = &s->listeners[s->listener_count];
    proven_net_addr_t got;
    proven_err_t e = proven_net_listen(at, 1024, &l->listener, &got);
    if (e != PROVEN_OK) return e;
    l->server = s;
    l->io = (proven_loop_io_t){ 0 };
    bool want = !s->stopped && s->conn_count < s->cfg.max_connections;
    e = proven_loop_io_add(s->loop, &l->io, proven_net_listener_handle(&l->listener), want ? PROVEN_NET_READABLE : 0, ev_on_accept, l);
    if (e != PROVEN_OK) { (void)proven_net_listener_close(&l->listener); return e; }
#if !defined(PROVEN_NO_HTTP3)
    /* HTTP/3 on the same address and port, or neither: a server that says `Alt-Svc` for a port
     * it does not serve sends its clients to nothing. */
    if (s->h3 && (e = proven_http_event_h3_listen_(s, got)) != PROVEN_OK) {
        proven_loop_io_remove(s->loop, &l->io);
        (void)proven_net_listener_close(&l->listener);
        return e;
    }
#endif
    s->listener_count++;
    s->accepting = want;
    s->closed_to_new = false;                      /* listening again is taking connections again */
    if (bound) *bound = got;
    return PROVEN_OK;
}

void proven_http_event_server_stop_listening(proven_http_event_server_t *s) {
    if (!s) return;
    for (proven_size_t i = 0; i < s->listener_count; ++i) {
        proven_loop_io_remove(s->loop, &s->listeners[i].io);
        (void)proven_net_listener_close(&s->listeners[i].listener);
    }
    s->listener_count = 0;
    s->accepting = false;
    s->closed_to_new = true;
#if !defined(PROVEN_NO_HTTP3)
    proven_http_event_h3_stop_(s);
#endif
    /* A connection that carries many exchanges is told, so that it can say goodbye and
     * finish the ones it has. (An HTTP/1.1 connection learns at its next response.) Telling
     * one may run a handler's callbacks, and those may end any other connection: so the list
     * is searched again from its head after each, for one that has not been told. */
    for (bool again = !s->stopped; again;) {
        again = false;
        for (proven_http_stream_t *c = s->conns; c; c = c->next) {
            if (c->h.state != EV_RAW || !c->raw || c->told_stop) continue;
            c->told_stop = true;
            const proven_http_event_raw_ops_t *ops = *(const proven_http_event_raw_ops_t *const *)c->raw;
            if (!ops->on_shutdown) continue;
            ev_enter(c);
            ops->on_shutdown(c->raw);
            ev_leave(c);
            again = true;
            break;
        }
    }
}

/* Every connection still open, ended: the same walk as destroy's. */
static void ev_end_all(proven_http_event_server_t *s) {
    while (s->conns) {
        proven_http_stream_t *c = s->conns;
        ev_enter(c);
        ev_kill(c, PROVEN_ERR_RESET);
        ev_leave(c);
    }
#if !defined(PROVEN_NO_HTTP3)
    proven_http_event_h3_end_all_(s);
#endif
}

static void ev_drain_deadline(void *ctx) {
    proven_http_event_server_t *s = ctx;
    if (s->conns || s->h3_count > 0) s->drain_cut = true;
    ev_end_all(s);
    ev_drain_check(s);
}

proven_err_t proven_http_event_server_shutdown(proven_http_event_server_t *s, proven_u32 grace_ms,
                                               proven_http_event_drained_fn on_drained, void *ctx) {
    if (!s) return PROVEN_ERR_INVALID_ARG;
    if (s->draining || s->stopped) return PROVEN_ERR_INVALID_STATE;
    s->draining = true;
    s->on_drained = on_drained;
    s->drained_ctx = ctx;
    /* Nothing new; HTTP/1.1 answers say Connection: close; HTTP/2 says goodbye. */
    proven_http_event_server_stop_listening(s);
    /* HTTP/1.1 with no request on it: nothing to finish. Ending one runs no callback - it has
     * no exchange and no other protocol - so one pass does them all. */
    for (proven_http_stream_t *c = s->conns, *next; c; c = next) {
        next = c->next;
        if (c->dead || c->h.state != EV_HEAD || c->head_started || c->active) continue;
        ev_enter(c);
        ev_kill(c, PROVEN_ERR_RESET);
        ev_leave(c);
    }
    /* A taken-over connection is told, at most once. Telling one may run callbacks that end
     * any connection: while the one told is still open its `next` is a live link and the walk
     * goes on from there; when it was ended, the walk starts again from the head (those told
     * are passed over), so the usual case is one pass. */
    for (proven_http_stream_t *c = s->conns; c;) {
        if (c->dead || c->h.state != EV_RAW || !c->raw || c->told_drain) { c = c->next; continue; }
        const proven_http_event_raw_ops_t *ops = *(const proven_http_event_raw_ops_t *const *)c->raw;
        c->told_drain = true;
        if (!ops->on_drain) { c = c->next; continue; }
        ev_enter(c);
        ops->on_drain(c->raw);
        const bool open = !c->dead;
        proven_http_stream_t *next = open ? c->next : (void *)0;
        ev_leave(c);
        c = open ? next : s->conns;
    }
    if (grace_ms == 0) ev_drain_deadline(s);
    else proven_loop_timer_set(s->loop, &s->drain_deadline, grace_ms, ev_drain_deadline, s);
    ev_drain_check(s);
    return PROVEN_OK;
}

void proven_http_event_server_destroy(proven_http_event_server_t *s) {
    if (!s) return;
    proven_loop_timer_cancel(s->loop, &s->drain_deadline);
    proven_loop_timer_cancel(s->loop, &s->drain_done);
    s->stopped = true;
    proven_http_event_server_stop_listening(s);
    while (s->conns) {
        proven_http_stream_t *c = s->conns;
        ev_enter(c);
        ev_kill(c, PROVEN_ERR_RESET);
        ev_leave(c);
    }
#if !defined(PROVEN_NO_HTTP3)
    proven_http_event_h3_destroy_(s);
#endif
    proven_allocator_t a = s->alloc;
    if (s->headers) a.free_fn(a.ctx, s->headers);
    if (s->rbuf) a.free_fn(a.ctx, s->rbuf);
    if (s->hbuf) a.free_fn(a.ctx, s->hbuf);
    if (s->pbuf) a.free_fn(a.ctx, s->pbuf);
    if (s->zbuf) a.free_fn(a.ctx, s->zbuf);
    if (s->h2buf) a.free_fn(a.ctx, s->h2buf);
    a.free_fn(a.ctx, s);
}

proven_size_t proven_http_event_server_connections(const proven_http_event_server_t *s) { return s ? s->conn_count + s->h3_count : 0; }

void proven_http_event_gone_(proven_http_event_server_t *s) {
    ev_update_listeners(s);
    ev_drain_check(s);
}

// -----------------------------------------------------------------------------
// Responding
// -----------------------------------------------------------------------------

static bool ev_is_driver_header(proven_u8str_view_t name) {
    return proven_u8str_view_eq_ascii_ci(name, ev_lit("Content-Length")) || proven_u8str_view_eq_ascii_ci(name, ev_lit("Transfer-Encoding")) ||
           proven_u8str_view_eq_ascii_ci(name, ev_lit("Connection")) || proven_u8str_view_eq_ascii_ci(name, ev_lit("Date"));
}

/* The response head into the server's head buffer. */
static proven_err_t ev_build_head(proven_http_stream_t *c, proven_u16 status, const proven_http_header_t *headers, proven_size_t header_count,
                                  proven_u64 content_length, bool vary, proven_http_coding_t coding, proven_size_t *head_len) {
    proven_http_event_server_t *s = c->h.server;
    if (status < 200 || status > 999 || (header_count > 0 && !headers)) return PROVEN_ERR_INVALID_ARG;
    proven_mem_mut_t out = { .ptr = s->hbuf, .size = s->cfg.max_head_bytes };
    proven_size_t len = 0;
    proven_err_t e = proven_http_write_status_line(out, &len, status, ev_lit(""));
    proven_byte_t date[PROVEN_HTTP_DATE_SIZE];
    if (e == PROVEN_OK && proven_http_date_format(proven_time_now(), date) == PROVEN_OK) {
        e = proven_http_write_header(out, &len, ev_lit("Date"), (proven_u8str_view_t){ .ptr = date, .size = sizeof date });
    }
    for (proven_size_t i = 0; i < header_count && e == PROVEN_OK; ++i) {
        if (ev_is_driver_header(headers[i].name)) return PROVEN_ERR_INVALID_ARG;
        e = proven_http_write_header(out, &len, headers[i].name, headers[i].value);
    }
    if (e == PROVEN_OK && vary) e = proven_http_write_header(out, &len, ev_lit("Vary"), ev_lit("Accept-Encoding"));
    if (e == PROVEN_OK && coding != PROVEN_HTTP_CODING_IDENTITY) e = proven_http_write_header(out, &len, ev_lit("Content-Encoding"), proven_http_coding_token_(coding));
#if !defined(PROVEN_NO_HTTP3)
    if (e == PROVEN_OK && s->h3) {
        proven_byte_t alt[16];
        const proven_size_t n = proven_http_event_alt_svc_(s, c->local_port, headers, header_count, alt);
        if (n > 0) e = proven_http_write_header(out, &len, ev_lit("Alt-Svc"), (proven_u8str_view_t){ .ptr = alt, .size = n });
    }
#endif
    /* 204 and 304 never have a body; a response to HEAD has the headers of one and no bytes. */
    bool no_body_status = status == 204 || status == 304;
    bool chunked = false;
    /* Answering while the request body is still arriving, or while the client waits for its
     * 100 Continue: where the next request starts is no longer certain. */
    bool close = !c->keep || c->h.state == EV_BODY || c->expect_continue || c->peer_eof || s->stopped || s->closed_to_new;
    if (e == PROVEN_OK && !no_body_status) {
        if (content_length != PROVEN_HTTP_EVENT_LENGTH_UNKNOWN) e = proven_http_write_header_u64(out, &len, ev_lit("Content-Length"), content_length);
        else if (c->h.version_minor >= 1) { e = proven_http_write_header(out, &len, ev_lit("Transfer-Encoding"), ev_lit("chunked")); chunked = true; }
        else close = true;                         /* an HTTP/1.0 client reads until the connection closes */
    }
    if (e == PROVEN_OK) {
        if (close) e = proven_http_write_header(out, &len, ev_lit("Connection"), ev_lit("close"));
        else if (c->h.version_minor == 0) e = proven_http_write_header(out, &len, ev_lit("Connection"), ev_lit("keep-alive"));
    }
    if (e == PROVEN_OK) e = proven_http_write_head_end(out, &len);
    if (e != PROVEN_OK) return e;
    c->bodiless = no_body_status || c->method == PROVEN_HTTP_HEAD;
    c->chunked = chunked && !c->bodiless;
    c->keep = !close;
    c->response_left = c->chunked || c->bodiless || content_length == PROVEN_HTTP_EVENT_LENGTH_UNKNOWN ? PROVEN_HTTP_EVENT_LENGTH_UNKNOWN : content_length;
    *head_len = len;
    return PROVEN_OK;
}

/* What the handler's request for compression comes to for this response: the coding, or
 * identity for none. */
static void ev_compress_plan(const proven_http_stream_t *c, proven_u16 status, const proven_http_header_t *headers, proven_size_t header_count,
                             bool *vary, proven_http_coding_t *coding) {
    *vary = false;
    *coding = PROVEN_HTTP_CODING_IDENTITY;
    if (!c->compress_asked || (header_count > 0 && !headers)) return;
    *vary = proven_http_compress_needs_vary_(headers, header_count);
    if (proven_http_compress_applies_(c->compress_coding != PROVEN_HTTP_CODING_IDENTITY, status, headers, header_count)) *coding = (proven_http_coding_t)c->compress_coding;
}

void proven_http_stream_compress(proven_http_stream_t *c) {
    if (c && PROVEN_HTTP_EVENT_IS_H2_(c)) { proven_http_event_h2_compress_(c); return; }
#if !defined(PROVEN_NO_HTTP3)
    if (c && PROVEN_HTTP_EVENT_IS_H3_(c)) { proven_http_event_h3_compress_(c); return; }
#endif
    if (c && !c->dead && c->active && !c->response_begun) c->compress_asked = true;
}

proven_err_t proven_http_stream_begin(proven_http_stream_t *c, proven_u16 status,
                                      const proven_http_header_t *headers, proven_size_t header_count, proven_u64 content_length) {
    if (!c) return PROVEN_ERR_INVALID_ARG;
    if (PROVEN_HTTP_EVENT_IS_H2_(c)) return proven_http_event_h2_begin_(c, status, headers, header_count, content_length);
#if !defined(PROVEN_NO_HTTP3)
    if (PROVEN_HTTP_EVENT_IS_H3_(c)) return proven_http_event_h3_begin_(c, status, headers, header_count, content_length);
#endif
    if (c->dead || !c->active || c->response_begun) return PROVEN_ERR_INVALID_STATE;
    proven_http_event_server_t *s = c->h.server;
    proven_size_t len = 0;
    bool vary = false;
    proven_http_coding_t coding = PROVEN_HTTP_CODING_IDENTITY;
    ev_compress_plan(c, status, headers, header_count, &vary, &coding);
    proven_http_compressor_t_ *zip = (void *)0;
    /* Without the memory for a compressor the response goes as it is. (A response to HEAD
     * needs none: it has the headers and no bytes.) */
    if (coding != PROVEN_HTTP_CODING_IDENTITY && c->method != PROVEN_HTTP_HEAD) {
        if (!s->zbuf) s->zbuf = ev_alloc(s->alloc, PROVEN_HTTP_CODING_PIECE);
        const proven_http_compress_cfg_t_ cc = proven_http_event_compress_cfg_(&s->cfg);
        if (!s->zbuf || proven_http_compressor_create_(s->alloc, coding, &cc, &zip) != PROVEN_OK) coding = PROVEN_HTTP_CODING_IDENTITY;
    }
    /* Compressed, the length on the wire is not the one given: the body goes chunked. The
     * length given is still what the handler has promised to write. */
    proven_err_t e = ev_build_head(c, status, headers, header_count, coding != PROVEN_HTTP_CODING_IDENTITY ? PROVEN_HTTP_EVENT_LENGTH_UNKNOWN : content_length,
                                   vary, coding, &len);
    if (e != PROVEN_OK) {
        proven_http_compressor_destroy_(zip);
        return e;
    }
    if (zip) {
        c->zip = zip;
        c->response_left = content_length;
    }
    ev_enter(c);
    c->response_begun = true;
    c->expect_continue = false;
    bool alive = ev_send(c, c->h.server->hbuf, len);
    ev_leave(c);
    return alive ? PROVEN_OK : PROVEN_ERR_RESET;
}

/* Body bytes through the compressor and into the output queue, each piece that comes out as
 * a chunk (or bare, for a client that reads until close). False when the connection died. */
static bool ev_zip(proven_http_stream_t *c, const proven_byte_t *p, proven_size_t n, proven_deflate_flush_t flush) {
    proven_http_event_server_t *s = c->h.server;
    for (;;) {
        proven_size_t used = 0, made = 0;
        bool done = false;
        proven_err_t e = proven_http_compressor_step_(c->zip, (proven_mem_view_t){ .ptr = p, .size = n }, &used,
                                                      (proven_mem_mut_t){ .ptr = s->zbuf, .size = PROVEN_HTTP_CODING_PIECE }, &made, flush, &done);
        if (e != PROVEN_OK) { ev_kill(c, e); return false; }
        p += used;
        n -= used;
        if (made > 0) {
            bool ok = true;
            if (c->chunked) {
                proven_byte_t frame[24];
                proven_size_t fl = 0;
                (void)proven_http_write_chunk_begin((proven_mem_mut_t){ .ptr = frame, .size = sizeof frame }, &fl, made);
                ok = buf_append(s, &c->out, frame, fl) && buf_append(s, &c->out, s->zbuf, made) && buf_append(s, &c->out, (const proven_byte_t *)"\r\n", 2);
            } else {
                ok = buf_append(s, &c->out, s->zbuf, made);
            }
            if (!ok) { ev_kill(c, PROVEN_ERR_NOMEM); return false; }
        }
        if (flush == PROVEN_DEFLATE_FLUSH_FINISH ? done : (n == 0 && made < PROVEN_HTTP_CODING_PIECE)) break;
    }
    return ev_flush(c);
}

proven_result_size_t proven_http_stream_write(proven_http_stream_t *c, proven_mem_view_t data) {
    if (!c || (data.size > 0 && !data.ptr)) return (proven_result_size_t){ .err = PROVEN_ERR_INVALID_ARG, .value = 0 };
    if (PROVEN_HTTP_EVENT_IS_H2_(c)) return proven_http_event_h2_write_(c, data);
#if !defined(PROVEN_NO_HTTP3)
    if (PROVEN_HTTP_EVENT_IS_H3_(c)) return proven_http_event_h3_write_(c, data);
#endif
    if (c->dead || !c->active || !c->response_begun || c->response_ended) return (proven_result_size_t){ .err = PROVEN_ERR_INVALID_STATE, .value = 0 };
    proven_http_event_server_t *s = c->h.server;
    if (c->bodiless) return (proven_result_size_t){ .err = PROVEN_OK, .value = data.size };     /* counted, not sent */
    if (c->response_left != PROVEN_HTTP_EVENT_LENGTH_UNKNOWN && data.size > c->response_left) return (proven_result_size_t){ .err = PROVEN_ERR_OUT_OF_BOUNDS, .value = 0 };
    ev_enter(c);
    proven_size_t done = 0;
    bool alive = true, tried = false;
    /* Take what fits under the limit, push it at the socket, and look again: a refusal must
     * mean that the socket itself is not taking more. Refusing while the socket would take it
     * leaves the caller waiting for an on_writable that nothing would ever trigger. */
    while (alive && done < data.size) {
        proven_size_t held = ev_buffered(c);
        proven_size_t room = held < s->cfg.max_buffered_output ? s->cfg.max_buffered_output - held : 0;
        if (room == 0) {
            if (tried) break;
            tried = true;
            alive = ev_flush(c);
            continue;
        }
        proven_size_t n = data.size - done < room ? data.size - done : room;
        if (c->zip) {
            /* A piece at a time: what the compressor gives back for it is queued whole, so the
             * queue passes the limit by no more than one piece's worth. */
            if (n > PROVEN_HTTP_CODING_PIECE) n = PROVEN_HTTP_CODING_PIECE;
            alive = ev_zip(c, data.ptr + done, n, PROVEN_DEFLATE_FLUSH_NONE);
            if (alive && c->response_left != PROVEN_HTTP_EVENT_LENGTH_UNKNOWN) c->response_left -= n;
        } else if (c->chunked) {
            proven_byte_t frame[24];
            proven_size_t fl = 0;
            (void)proven_http_write_chunk_begin((proven_mem_mut_t){ .ptr = frame, .size = sizeof frame }, &fl, n);
            /* Queued together so that a chunk is never left half-framed. */
            alive = buf_append(s, &c->out, frame, fl) && buf_append(s, &c->out, data.ptr + done, n) && buf_append(s, &c->out, (const proven_byte_t *)"\r\n", 2);
            if (!alive) ev_kill(c, PROVEN_ERR_NOMEM);
            else alive = ev_flush(c);
        } else {
            alive = ev_send(c, data.ptr + done, n);
            if (alive && c->response_left != PROVEN_HTTP_EVENT_LENGTH_UNKNOWN) c->response_left -= n;
        }
        done += n;
        tried = true;                              /* every pass through here ends with a flush */
    }
    if (alive) {
        if (done < data.size) c->want_writable = true;
        ev_watch(c);
        ev_arm(c);
    }
    ev_leave(c);
    return alive ? (proven_result_size_t){ .err = PROVEN_OK, .value = done } : (proven_result_size_t){ .err = PROVEN_ERR_RESET, .value = 0 };
}

proven_err_t proven_http_stream_flush(proven_http_stream_t *c) {
    if (!c) return PROVEN_ERR_INVALID_ARG;
    if (PROVEN_HTTP_EVENT_IS_H2_(c)) return proven_http_event_h2_flush_(c);
#if !defined(PROVEN_NO_HTTP3)
    if (PROVEN_HTTP_EVENT_IS_H3_(c)) return proven_http_event_h3_flush_(c);
#endif
    if (c->dead || !c->active || !c->response_begun || c->response_ended) return PROVEN_ERR_INVALID_STATE;
    if (!c->zip) return PROVEN_OK;                 /* nothing is held back: what was written is queued or gone */
    ev_enter(c);
    const bool alive = ev_zip(c, (void *)0, 0, PROVEN_DEFLATE_FLUSH_SYNC);
    if (alive) {
        ev_watch(c);
        ev_arm(c);
    }
    ev_leave(c);
    return alive ? PROVEN_OK : PROVEN_ERR_RESET;
}

proven_err_t proven_http_stream_end(proven_http_stream_t *c) {
    if (!c) return PROVEN_ERR_INVALID_ARG;
    if (PROVEN_HTTP_EVENT_IS_H2_(c)) return proven_http_event_h2_end_(c);
#if !defined(PROVEN_NO_HTTP3)
    if (PROVEN_HTTP_EVENT_IS_H3_(c)) return proven_http_event_h3_end_(c);
#endif
    if (c->dead || !c->active || !c->response_begun || c->response_ended) return PROVEN_ERR_INVALID_STATE;
    ev_enter(c);
    proven_err_t result = PROVEN_OK;
    if (c->response_left != PROVEN_HTTP_EVENT_LENGTH_UNKNOWN && c->response_left != 0) {
        /* Less was written than promised. The client must not take what it has for the whole. */
        ev_kill(c, PROVEN_ERR_INVALID_FORMAT);
        result = PROVEN_ERR_INVALID_FORMAT;
    } else {
        c->response_ended = true;
        bool alive = true;
        if (c->zip) {
            alive = ev_zip(c, (void *)0, 0, PROVEN_DEFLATE_FLUSH_FINISH);
            if (alive) { proven_http_compressor_destroy_(c->zip); c->zip = (void *)0; }
        }
        if (alive && c->chunked) alive = ev_send(c, (const proven_byte_t *)"0\r\n\r\n", 5);
        if (alive) ev_progress(c);
        else result = PROVEN_ERR_RESET;
    }
    ev_leave(c);
    return result;
}

proven_err_t proven_http_stream_respond(proven_http_stream_t *c, proven_u16 status,
                                        const proven_http_header_t *headers, proven_size_t header_count, proven_mem_view_t body) {
    if (!c || (body.size > 0 && !body.ptr)) return PROVEN_ERR_INVALID_ARG;
    if (PROVEN_HTTP_EVENT_IS_H2_(c)) return proven_http_event_h2_respond_(c, status, headers, header_count, body);
#if !defined(PROVEN_NO_HTTP3)
    if (PROVEN_HTTP_EVENT_IS_H3_(c)) return proven_http_event_h3_respond_(c, status, headers, header_count, body);
#endif
    if (c->dead || !c->active || c->response_begun) return PROVEN_ERR_INVALID_STATE;
    proven_http_event_server_t *s = c->h.server;
    proven_size_t len = 0;
    bool vary = false;
    proven_http_coding_t coding = PROVEN_HTTP_CODING_IDENTITY;
    ev_compress_plan(c, status, headers, header_count, &vary, &coding);
    /* Compressed first, because the head says how long the result is - for HEAD too, which
     * must announce what GET would send. Too short to gain, no smaller, or no memory: the
     * body goes as it is. */
    proven_byte_t *packed = (void *)0;
    proven_size_t packed_cap = 0, packed_size = 0;
    const proven_http_compress_cfg_t_ cc = proven_http_event_compress_cfg_(&s->cfg);
    if (coding != PROVEN_HTTP_CODING_IDENTITY && proven_http_compress_all_(s->alloc, coding, &cc, body, &packed, &packed_cap, &packed_size) != PROVEN_OK) {
        packed = (void *)0;
    }
    if (packed) body = (proven_mem_view_t){ .ptr = packed, .size = packed_size };
    proven_err_t e = ev_build_head(c, status, headers, header_count, body.size, vary, packed ? coding : PROVEN_HTTP_CODING_IDENTITY, &len);
    if (e != PROVEN_OK) {
        if (packed) s->alloc.free_fn(s->alloc.ctx, packed);
        return e;
    }
    ev_enter(c);
    c->response_begun = true;
    c->expect_continue = false;
    /* Head and body are queued as one, whatever the output limit: this call's contract is the
     * whole response. */
    bool alive = buf_append(s, &c->out, s->hbuf, len) && (c->bodiless || buf_append(s, &c->out, body.ptr, body.size));
    if (packed) s->alloc.free_fn(s->alloc.ctx, packed);
    if (!alive) { ev_kill(c, PROVEN_ERR_NOMEM); e = PROVEN_ERR_NOMEM; }
    else if (!ev_flush(c)) e = PROVEN_ERR_RESET;
    else {
        c->response_left = PROVEN_HTTP_EVENT_LENGTH_UNKNOWN;
        c->response_ended = true;
        ev_progress(c);
    }
    ev_leave(c);
    return e;
}

void proven_http_stream_abort(proven_http_stream_t *c) {
    if (!c) return;
    if (PROVEN_HTTP_EVENT_IS_H2_(c)) { proven_http_event_h2_abort_(c); return; }
#if !defined(PROVEN_NO_HTTP3)
    if (PROVEN_HTTP_EVENT_IS_H3_(c)) { proven_http_event_h3_abort_(c); return; }
#endif
    if (c->dead) return;
    ev_enter(c);
    ev_kill(c, PROVEN_ERR_RESET);
    ev_leave(c);
}

void proven_http_stream_pause(proven_http_stream_t *c) {
    if (!c) return;
    if (PROVEN_HTTP_EVENT_IS_H2_(c)) { proven_http_event_h2_pause_(c); return; }
#if !defined(PROVEN_NO_HTTP3)
    if (PROVEN_HTTP_EVENT_IS_H3_(c)) { proven_http_event_h3_pause_(c); return; }
#endif
    if (c->dead || !c->active) return;
    c->paused = true;
    ev_watch(c);
    ev_arm(c);
}

void proven_http_stream_resume(proven_http_stream_t *c) {
    if (!c) return;
    if (PROVEN_HTTP_EVENT_IS_H2_(c)) { proven_http_event_h2_resume_(c); return; }
#if !defined(PROVEN_NO_HTTP3)
    if (PROVEN_HTTP_EVENT_IS_H3_(c)) { proven_http_event_h3_resume_(c); return; }
#endif
    if (c->dead || !c->paused) return;
    c->paused = false;
    ev_enter(c);
    ev_watch(c);
    ev_arm(c);
    ev_process(c);
    ev_leave(c);
}

void proven_http_stream_set_user(proven_http_stream_t *c, void *user) {
    if (!c) return;
    if (PROVEN_HTTP_EVENT_IS_H2_(c)) { proven_http_event_h2_set_user_(c, user); return; }
#if !defined(PROVEN_NO_HTTP3)
    if (PROVEN_HTTP_EVENT_IS_H3_(c)) { proven_http_event_h3_set_user_(c, user); return; }
#endif
    c->user = user;
}

void *proven_http_stream_user(const proven_http_stream_t *c) {
    if (!c) return (void *)0;
#if !defined(PROVEN_NO_HTTP3)
    if (PROVEN_HTTP_EVENT_IS_H3_(c)) return proven_http_event_h3_user_(c);
#endif
    return PROVEN_HTTP_EVENT_IS_H2_(c) ? proven_http_event_h2_user_(c) : c->user;
}

proven_net_addr_t proven_http_stream_peer(const proven_http_stream_t *c) {
#if !defined(PROVEN_NO_HTTP3)
    if (PROVEN_HTTP_EVENT_IS_H3_(c)) return proven_http_event_h3_peer_(c);
#endif
    return PROVEN_HTTP_EVENT_IS_H2_(c) ? proven_http_event_h2_peer_(c) : c->peer;
}

proven_size_t proven_http_stream_buffered(const proven_http_stream_t *c) {
    if (!c) return 0;
    if (PROVEN_HTTP_EVENT_IS_H2_(c)) return proven_http_event_h2_buffered_(c);
#if !defined(PROVEN_NO_HTTP3)
    if (PROVEN_HTTP_EVENT_IS_H3_(c)) return proven_http_event_h3_buffered_(c);
#endif
    return !c->dead ? ev_buffered(c) : 0;
}

int proven_http_stream_protocol(const proven_http_stream_t *c) {
    return !c ? 1 : PROVEN_HTTP_EVENT_IS_H2_(c) ? 2 : PROVEN_HTTP_EVENT_IS_H3_(c) ? 3 : 1;
}

bool proven_http_stream_early(const proven_http_stream_t *c) {
#if !defined(PROVEN_NO_HTTP3)
    if (c && PROVEN_HTTP_EVENT_IS_H3_(c)) return proven_http_event_h3_early_(c);
#endif
    (void)c;
    return false;
}

// -----------------------------------------------------------------------------
// A connection taken over by another protocol (proven_internal_http_event.h)
// -----------------------------------------------------------------------------

proven_err_t proven_http_event_raw_begin_(proven_http_stream_t *c, void *raw, proven_mem_view_t head) {
    /* Only from inside on_request (`processing`): input that follows is then delivered by the
     * loop that is already running, and not before the caller has the new object. */
    if (!c || !raw) return PROVEN_ERR_INVALID_STATE;
    if (PROVEN_HTTP_EVENT_IS_H2_(c)) return PROVEN_ERR_UNSUPPORTED;       /* extended CONNECT (RFC 8441) is not here */
    if (c->dead || !c->active || !c->processing || c->response_begun || c->h.state != EV_RESPOND) return PROVEN_ERR_INVALID_STATE;
    proven_http_event_server_t *s = c->h.server;
    ev_enter(c);
    c->response_begun = true; c->response_ended = true;
    if (!ev_send(c, head.ptr, head.size)) { ev_leave(c); return PROVEN_ERR_RESET; }   /* on_done was called by the kill */
    /* The HTTP exchange is over; the connection goes on as something else. */
    c->active = false;
    if (s->cfg.on.on_done) s->cfg.on.on_done(s->cfg.ctx, c, PROVEN_OK);
    if (c->dead) { ev_leave(c); return PROVEN_ERR_RESET; }
    c->user = (void *)0;
    c->response_begun = false; c->response_ended = false; c->chunked = false; c->bodiless = false;
    c->want_writable = false; c->paused = false; c->expect_continue = false; c->keep = false;
    c->h.state = EV_RAW;
    c->raw = raw;
    proven_loop_timer_cancel(s->loop, &c->timer);
    ev_watch(c);
    ev_leave(c);
    return PROVEN_OK;
}

bool proven_http_event_raw_send_(proven_http_stream_t *c, proven_mem_view_t a, proven_mem_view_t b) {
    if (c && PROVEN_HTTP_EVENT_IS_H2_(c)) return proven_http_event_h2_raw_send_(c, a, b);
    if (!c || c->dead) return false;
    ev_enter(c);
    /* Both parts are queued before anything is sent, so that a frame's header and payload
     * leave in one write. */
    bool ok = buf_append(c->h.server, &c->out, a.ptr, a.size) && buf_append(c->h.server, &c->out, b.ptr, b.size);
    if (!ok) ev_kill(c, PROVEN_ERR_NOMEM);
    else ok = ev_flush(c);
    if (ok) ev_watch(c);
    ev_leave(c);
    return ok;
}

void proven_http_event_raw_timer_(proven_http_stream_t *c, proven_u32 ms) {
    if (c && PROVEN_HTTP_EVENT_IS_H2_(c)) { proven_http_event_h2_raw_timer_(c, ms); return; }
    if (!c || c->dead) return;
    if (ms == 0) proven_loop_timer_cancel(c->h.server->loop, &c->timer);
    else proven_loop_timer_set(c->h.server->loop, &c->timer, ms, ev_on_timer, c);
}

void proven_http_event_raw_pause_(proven_http_stream_t *c, bool paused) {
    if (c && PROVEN_HTTP_EVENT_IS_H2_(c)) { proven_http_event_h2_raw_pause_(c, paused); return; }
    if (!c || c->dead || c->h.state != EV_RAW || c->paused == paused) return;
    c->paused = paused;
    ev_enter(c);
    if (!paused) ev_process(c);
    if (!c->dead) ev_watch(c);
    ev_leave(c);
}

void proven_http_event_raw_close_(proven_http_stream_t *c) {
    if (c && PROVEN_HTTP_EVENT_IS_H2_(c)) { proven_http_event_h2_raw_close_(c); return; }
    if (!c || c->dead) return;
    c->raw = (void *)0;
    ev_enter(c);
    ev_linger(c);
    ev_leave(c);
}

void proven_http_event_raw_kill_(proven_http_stream_t *c) {
    if (c && PROVEN_HTTP_EVENT_IS_H2_(c)) { proven_http_event_h2_raw_kill_(c); return; }
    if (!c || c->dead) return;
    c->raw = (void *)0;
    ev_enter(c);
    ev_kill(c, PROVEN_OK);
    ev_leave(c);
}

proven_allocator_t proven_http_event_raw_allocator_(const proven_http_stream_t *c) { return c->h.server->alloc; }

proven_size_t proven_http_event_raw_held_(const proven_http_stream_t *c) { return c && !c->dead ? ev_buffered(c) : 0; }

void proven_http_event_raw_again_(proven_http_stream_t *c) {
    if (!c || c->dead || c->h.state != EV_RAW) return;
    c->raw_more = true;
    ev_watch(c);
}

proven_size_t proven_http_event_raw_offer_(proven_http_stream_t *c, proven_mem_view_t data) {
    if (!c || c->dead || c->h.state != EV_RAW) return 0;
    proven_size_t taken = 0;
    ev_enter(c);
    if (c->tls) {
        /* A record's worth at a time, and only when the last has left: TLS's own queue stays
         * one record long, whatever the owner has waiting. */
        if (proven_tls_pending_output(c->tls).size == 0 && data.size > 0) {
            proven_mem_view_t piece = { .ptr = data.ptr, .size = data.size < 16384 ? data.size : 16384 };
            proven_result_size_t w = proven_tls_write(c->tls, piece);
            if (w.err != PROVEN_OK) ev_kill(c, PROVEN_ERR_RESET);
            else taken = w.value;
        }
        if (!c->dead) (void)ev_flush(c);
    } else if (data.size > 0 && buf_pending(&c->out) == 0) {
        proven_result_size_t w = proven_net_write(&c->sock, data, PROVEN_NET_DONT_WAIT);
        if (w.err == PROVEN_OK) taken = w.value;
        else if (w.err != PROVEN_ERR_TIMEOUT && w.err != PROVEN_ERR_AGAIN) ev_kill(c, PROVEN_ERR_RESET);
    }
    if (c->dead) taken = 0;
    else { c->raw_more = taken < data.size; ev_watch(c); }
    ev_leave(c);
    return taken;
}

#endif
