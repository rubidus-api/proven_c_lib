#include "proven/http_event.h"
#include "proven_internal_http_event.h"
#include "proven_internal_http_coding.h"

#if !defined(PROVEN_FREESTANDING) && !defined(PROVEN_NO_NET)

#include "proven/h2.h"
#include "proven/time.h"

/*
 * HTTP/2 in the event-driven server.
 *
 * A connection that turns out to be HTTP/2 is handed over the way a WebSocket is
 * (proven_internal_http_event.h): the connection's record stays the transport - socket, TLS,
 * one timer - and this file owns what travels on it. The protocol itself is
 * proven_h2_conn_t (h2.h); what is here is the driver that h2.h leaves to its caller: bytes
 * in and out, time, and the handler's callbacks.
 *
 * A handler is given the same `proven_http_stream_t *` it gets for HTTP/1.1, but for HTTP/2
 * that pointer is to an h2s_t below - one stream of many - which shares only its first member
 * with the HTTP/1.1 struct. http_event.c reads `kind` there and sends every call here.
 *
 * Re-entrancy, as in http_event.c: a callback may respond, end, abort, pause or resume, and
 * any of those may end the stream or the connection. So a stream and the driver each count
 * the entries on the stack and are freed when the outermost leaves; nothing reads a field
 * after a callback without having looked at `dead`.
 *
 * One queue: the engine's pending output is offered straight to the transport, which holds
 * none of it. `max_buffered_output` is what the engine may hold before body bytes are refused.
 *
 * **on_writable is never called from inside a call the handler made.** A handler writes in a
 * loop and keeps its place in a variable it updates when the write returns; called again from
 * within that write, it would send the same bytes twice, in the wrong order. HTTP/1.1 cannot
 * do this to it (its write never calls back), and so neither may this: while such a call is on
 * the stack (`calls`), a stream that could be told is left as it is and the transport is asked
 * to report itself writable, which brings the round back from the loop.
 */

typedef struct h2d h2d_t;

typedef struct h2s {
    ev_head_t h;                       /* first: what http_event.c reads */
    h2d_t *d;                          /* not to be followed once `dead` */
    struct h2s *prev, *next;
    void *user;
    proven_byte_t *held;               /* request body that arrived while the handler had paused */
    proven_size_t held_len, held_cap;
    proven_byte_t *out;                /* response body made and not yet taken by the engine */
    proven_size_t out_len, out_off, out_cap;
    proven_deflate_t *zip;
    proven_u64 response_left;          /* body bytes still promised; LENGTH_UNKNOWN when not counted */
    proven_u64 body_seen;
    proven_time_t body_due;            /* when a request whose body is owed has been silent too long; 0: nothing is owed, or the handler paused it */
    proven_u32 id;
    proven_u32 mark;                   /* the driver's round in which this stream was last looked at */
    proven_http_method_t method;
    bool active;                       /* on_request was delivered and on_done has not been */
    bool response_begun, response_ended, bodiless, paused, dead;
    bool want_writable, accepts_gzip, compress_asked;
    bool in_done;                      /* the request's end has arrived */
    bool held_last;                    /* ... while paused: it is delivered with what is held */
    bool end_asked;                    /* END_STREAM goes when `out` has drained */
    bool engine_done;                  /* the engine has seen this side's end already (a response with no body) */
    bool window_shut;                  /* the engine took less than it was offered: the peer's window, and only the engine knows when it opens */
} h2s_t;

struct h2d {
    const proven_http_event_raw_ops_t *ops;      /* first: http_event.c finds the operations here */
    proven_http_stream_t *conn;        /* the transport; not to be followed once `dead` */
    proven_http_event_server_t *server;
    proven_h2_conn_t *engine;
    h2s_t *streams;
    proven_size_t stream_count;
    proven_u32 depth, round;
    bool dead;                         /* the transport is gone, or was let go of */
    bool pumping;
    bool asked;                        /* a request has arrived: from here on a quiet connection is idle, not slow */
    proven_u32 calls;                  /* calls a handler made that are now on the stack */
    proven_time_t quiet_due;           /* when the connection's own silence - output not moving, nothing asked, nothing open - has lasted too long; 0: none is being measured */
    bool over;                         /* nothing more will be taken: write what is owed and close */
    bool eof;                          /* the peer closed its sending side */
};

static proven_u8str_view_t h2_lit_n(const char *s, proven_size_t n) { return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)s, .size = n }; }
#define h2_lit(s) h2_lit_n((s), sizeof(s) - 1)

static proven_byte_t h2_lower(proven_byte_t c) { return (c >= 'A' && c <= 'Z') ? (proven_byte_t)(c + 32) : c; }

static bool h2_eq_nocase(proven_u8str_view_t a, proven_u8str_view_t b) {
    if (a.size != b.size) return false;
    for (proven_size_t i = 0; i < a.size; ++i) if (h2_lower(a.ptr[i]) != h2_lower(b.ptr[i])) return false;
    return true;
}

static void *h2_alloc(proven_allocator_t a, proven_size_t n) {
    proven_result_mem_mut_t m = a.alloc_fn(a.ctx, n, 16);
    return m.err == PROVEN_OK ? m.value.ptr : (void *)0;
}

/* Append to a buffer that grows; false when there is no memory. */
static bool h2_append(proven_allocator_t a, proven_byte_t **buf, proven_size_t *len, proven_size_t *cap, const proven_byte_t *p, proven_size_t n) {
    if (n == 0) return true;
    if (*len + n > *cap) {
        proven_size_t want = *cap ? *cap : 256;
        while (want < *len + n) { if (want > (proven_size_t)-1 / 2) return false; want *= 2; }
        proven_byte_t *q = h2_alloc(a, want);
        if (!q) return false;
        for (proven_size_t i = 0; i < *len; ++i) q[i] = (*buf)[i];
        if (*buf) a.free_fn(a.ctx, *buf);
        *buf = q;
        *cap = want;
    }
    for (proven_size_t i = 0; i < n; ++i) (*buf)[*len + i] = p[i];
    *len += n;
    return true;
}

// -----------------------------------------------------------------------------
// Lifetimes
// -----------------------------------------------------------------------------

static void d_pump(h2d_t *d);

static void d_enter(h2d_t *d) { d->depth++; }

static void d_leave(h2d_t *d) {
    if (--d->depth > 0 || !d->dead) return;
    proven_allocator_t a = d->server->alloc;
    proven_h2_conn_destroy(d->engine);
    a.free_fn(a.ctx, d);
}

static void s_enter(h2s_t *s) { s->h.depth++; }

static void s_leave(h2s_t *s) {
    if (--s->h.depth > 0 || !s->dead) return;
    proven_allocator_t a = s->h.server->alloc;
    a.free_fn(a.ctx, s);
}

/* The exchange is over: tell the handler, once, and let go of everything but the struct,
 * which goes when the outermost entry leaves. */
static void s_finish(h2s_t *s, proven_err_t why) {
    if (s->dead) return;
    proven_http_event_server_t *sv = s->h.server;
    proven_allocator_t a = sv->alloc;
    h2d_t *d = s->d;
    s_enter(s);
    s->dead = true;
    if (s->prev) s->prev->next = s->next;
    else d->streams = s->next;
    if (s->next) s->next->prev = s->prev;
    d->stream_count--;
    if (!d->dead) proven_h2_conn_stream_set_user(d->engine, s->id, (void *)0);
    if (s->zip) { proven_deflate_destroy(s->zip); s->zip = (void *)0; }
    if (s->held) { a.free_fn(a.ctx, s->held); s->held = (void *)0; s->held_len = s->held_cap = 0; }
    if (s->out) { a.free_fn(a.ctx, s->out); s->out = (void *)0; s->out_len = s->out_off = s->out_cap = 0; }
    if (s->active) {
        s->active = false;
        if (sv->cfg.on.on_done) sv->cfg.on.on_done(sv->cfg.ctx, (proven_http_stream_t *)(void *)s, why);
    }
    s_leave(s);
}

/* Give up on a stream from this side: the peer is told, the handler is told why. */
static void s_reset(h2s_t *s, proven_h2_error_t code, proven_err_t why) {
    if (s->dead) return;
    if (!s->d->dead && !s->engine_done) (void)proven_h2_conn_reset(s->d->engine, s->id, code);
    s_finish(s, why);
}

/* The transport is gone, or is being let go of: every exchange ends here. */
static void d_end_streams(h2d_t *d, proven_err_t why) {
    while (d->streams) s_finish(d->streams, why);
}

/* End the connection at once. */
static void d_kill(h2d_t *d, proven_err_t why) {
    if (d->dead) return;
    proven_http_stream_t *conn = d->conn;
    d_enter(d);
    d->dead = true;
    d_end_streams(d, why);
    proven_http_event_raw_kill_(conn);             /* detaches first: on_closed is not called */
    d_leave(d);
}

// -----------------------------------------------------------------------------
// Time
// -----------------------------------------------------------------------------

static proven_size_t d_queued(const h2d_t *d) {
    return proven_h2_conn_pending_output(d->engine).size + proven_http_event_raw_held_(d->conn);
}

/* A request's body is owed from now: it has `body_timeout_ms` to say something. */
static void s_expect_body(h2s_t *s) {
    s->body_due = s->in_done || s->paused ? 0 : proven_time_monotonic_now() + (proven_time_t)s->h.server->cfg.body_timeout_ms * 1000000;
}

/* One timer, for whatever the connection is waiting for. Output that does not move and a
 * connection with nothing on it are the connection's silences, measured from its last
 * activity. A request body is each stream's own: the timer is set for the earliest that is
 * due, so a request that has stalled is found though its neighbours are busy. */
static void d_arm(h2d_t *d) {
    if (d->dead) return;
    const proven_http_event_server_config_t *cfg = &d->server->cfg;
    proven_u32 ms = 0;
    if (d_queued(d) > 0) ms = cfg->write_timeout_ms;
    else if (!d->asked) ms = cfg->head_timeout_ms;
    else if (d->stream_count == 0) ms = cfg->idle_timeout_ms;
    const proven_time_t now = proven_time_monotonic_now();
    d->quiet_due = ms ? now + (proven_time_t)ms * 1000000 : 0;
    proven_time_t due = 0;
    for (const h2s_t *s = d->streams; s; s = s->next)
        if (s->body_due != 0 && (due == 0 || s->body_due < due)) due = s->body_due;
    if (due != 0) {
        const proven_time_t left = due > now ? (due - now) / 1000000 + 1 : 1;
        if (ms == 0 || (proven_time_t)ms > left) ms = (proven_u32)left;
    }
    /* With neither, every request is whole and the handlers are thinking: their business. */
    proven_http_event_raw_timer_(d->conn, ms);
}

// -----------------------------------------------------------------------------
// A stream's output
// -----------------------------------------------------------------------------

static proven_size_t s_out_pending(const h2s_t *s) { return s->out_len - s->out_off; }

/* How much more body the engine may be given before it holds `max_buffered_output`. */
static proven_size_t d_room(const h2d_t *d) {
    const proven_size_t held = proven_h2_conn_pending_output(d->engine).size, bound = d->server->cfg.max_buffered_output;
    return held < bound ? bound - held : 0;
}

/* Move what a stream has made towards the engine; send its end when all of it has gone; tell
 * a handler that was refused that it may write again. */
static void s_push(h2s_t *s) {
    if (s->dead || !s->response_begun) return;
    h2d_t *d = s->d;
    proven_http_event_server_t *sv = s->h.server;
    s_enter(s);
    while (!s->dead && s_out_pending(s) > 0) {
        proven_size_t n = d_room(d);
        if (n == 0) break;
        if (n > s_out_pending(s)) n = s_out_pending(s);
        proven_result_size_t r = proven_h2_conn_send_data(d->engine, s->id, (proven_mem_view_t){ .ptr = s->out + s->out_off, .size = n }, false);
        if (r.err != PROVEN_OK) { s_finish(s, PROVEN_ERR_RESET); break; }
        s->out_off += r.value;
        if (r.value < n) { s->window_shut = true; break; }      /* the peer's window: the engine says when */
    }
    if (!s->dead && s_out_pending(s) == 0) {
        if (s->out) { sv->alloc.free_fn(sv->alloc.ctx, s->out); s->out = (void *)0; s->out_len = s->out_off = s->out_cap = 0; }
        if (s->end_asked) {
            proven_result_size_t r = s->engine_done ? (proven_result_size_t){ .err = PROVEN_OK, .value = 0 }
                                                    : proven_h2_conn_send_data(d->engine, s->id, (proven_mem_view_t){ .ptr = (void *)0, .size = 0 }, true);
            s_finish(s, r.err == PROVEN_OK ? PROVEN_OK : PROVEN_ERR_RESET);
        } else if (s->want_writable && !s->window_shut && d_room(d) > 0) {
            if (d->calls > 0) {
                proven_http_event_raw_again_(d->conn);
            } else {
                s->want_writable = false;
                if (sv->cfg.on.on_writable) sv->cfg.on.on_writable(sv->cfg.ctx, (proven_http_stream_t *)(void *)s);
            }
        }
    }
    s_leave(s);
}

// -----------------------------------------------------------------------------
// Requests
// -----------------------------------------------------------------------------

/* Answer a request the server will not pass on, with a status and nothing else. */
static void d_refuse(h2d_t *d, proven_u32 id, const char status[4]) {
    proven_http_header_t h[1] = { { .name = h2_lit(":status"), .value = h2_lit_n(status, 3) } };
    if (proven_h2_conn_submit_response(d->engine, id, h, 1, true) != PROVEN_OK) (void)proven_h2_conn_reset(d->engine, id, PROVEN_H2_REFUSED_STREAM);
}

static void d_request(h2d_t *d, const proven_h2_event_t *ev) {
    proven_http_event_server_t *sv = d->server;
    const proven_h2_request_head_t *rq = &ev->request;
    const bool connect = rq->path.size == 0 && rq->scheme.size == 0;
    /* The fields are copied into the server's array before the handler sees them - its own
     * answer takes the connection, and the event's list is not promised past that. */
    bool has_host = false, has_length = false, bad = false;
    proven_u64 length = 0;
    for (proven_size_t i = 0; i < rq->field_count; ++i) {
        const proven_http_header_t *f = &rq->fields[i];
        if (h2_eq_nocase(f->name, h2_lit("host"))) {
            /* A request that names two different targets is how one is smuggled past a
             * front end that looked at the other (RFC 9113, section 8.3.1). */
            if (has_host || (rq->authority.size > 0 && !h2_eq_nocase(f->value, rq->authority))) bad = true;
            has_host = true;
        } else if (h2_eq_nocase(f->name, h2_lit("content-length"))) {
            has_length = true;
            length = 0;
            for (proven_size_t k = 0; k < f->value.size && length <= sv->cfg.max_body_bytes; ++k) length = length * 10 + (proven_u64)(f->value.ptr[k] - '0');
        }
    }
    d->asked = true;
    if (bad) { (void)proven_h2_conn_reset(d->engine, ev->stream_id, PROVEN_H2_PROTOCOL_ERROR); return; }
    const proven_size_t add_host = !has_host && rq->authority.size > 0 ? 1 : 0;
    /* A handler tells whether a body is coming from the fields, as HTTP/1.1 taught it to:
     * a length, or "chunked". HTTP/2 may send a body with neither - the frames say where it
     * ends - so one of unannounced length is given the field that means exactly that. */
    const proven_size_t add_chunked = !ev->end_stream && !has_length ? 1 : 0;
    if (rq->field_count + add_host + add_chunked > sv->cfg.max_headers) { d_refuse(d, ev->stream_id, "431"); return; }
    if (length > sv->cfg.max_body_bytes) { d_refuse(d, ev->stream_id, "413"); return; }

    h2s_t *s = h2_alloc(sv->alloc, sizeof *s);
    if (!s) { d_refuse(d, ev->stream_id, "503"); return; }
    for (proven_size_t i = 0; i < sizeof *s; ++i) ((proven_byte_t *)s)[i] = 0;
    proven_size_t n = 0;
    if (add_host) sv->headers[n++] = (proven_http_header_t){ .name = h2_lit("host"), .value = rq->authority };
    for (proven_size_t i = 0; i < rq->field_count; ++i) sv->headers[n++] = rq->fields[i];
    if (add_chunked) sv->headers[n++] = (proven_http_header_t){ .name = h2_lit("transfer-encoding"), .value = h2_lit("chunked") };
    proven_http_request_t req = {
        .method = proven_http_method_from_text(rq->method), .method_text = rq->method,
        .target = connect ? rq->authority : rq->path,
        .version_minor = 1,                        /* what a handler branches on is 1.0 against 1.1: HTTP/2 is not the older one */
        .headers = sv->headers, .header_count = n,
    };
    s->h = (ev_head_t){ .server = sv, .version_minor = 1, .kind = EV_KIND_H2 };
    s->d = d;
    s->id = ev->stream_id;
    s->method = req.method;
    s->response_left = PROVEN_HTTP_EVENT_LENGTH_UNKNOWN;
    s->accepts_gzip = proven_http_accepts_coding(sv->headers, n, PROVEN_HTTP_CODING_GZIP);
    s->in_done = ev->end_stream;
    s_expect_body(s);
    s->active = true;
    s->next = d->streams;
    if (d->streams) d->streams->prev = s;
    d->streams = s;
    d->stream_count++;
    proven_h2_conn_stream_set_user(d->engine, s->id, s);
    const bool expects = !ev->end_stream && proven_http_header_has_token(sv->headers, n, h2_lit("expect"), h2_lit("100-continue"));

    s_enter(s);
    sv->cfg.on.on_request(sv->cfg.ctx, (proven_http_stream_t *)(void *)s, &req);
    if (!s->dead && !d->dead && expects && !s->response_begun) {
        proven_http_header_t go_on[1] = { { .name = h2_lit(":status"), .value = h2_lit("100") } };
        (void)proven_h2_conn_submit_response(d->engine, s->id, go_on, 1, false);
    }
    s_leave(s);
}

/* A piece of a request body, or its end. */
static void d_body(h2d_t *d, h2s_t *s, proven_mem_view_t piece, bool last) {
    proven_http_event_server_t *sv = d->server;
    s->body_seen += piece.size;
    if (s->body_seen > sv->cfg.max_body_bytes) {
        if (!s->response_begun) {
            /* A complete answer to an unfinished request tells the client to stop sending. */
            proven_http_header_t h[1] = { { .name = h2_lit(":status"), .value = h2_lit("413") } };
            if (proven_h2_conn_submit_response(d->engine, s->id, h, 1, true) == PROVEN_OK) s->engine_done = true;
        }
        s_reset(s, PROVEN_H2_CANCEL, PROVEN_ERR_OUT_OF_BOUNDS);
        return;
    }
    if (last) s->in_done = true;
    s_expect_body(s);                              /* it spoke: the wait for its next piece begins again */
    if (s->paused) {
        /* The handler said "not now", and on HTTP/1.1 that means no call until it resumes.
         * Here the bytes have already arrived, so they are kept - at most one window of
         * them, since the peer is not told they were consumed and can send no more. */
        if (!h2_append(sv->alloc, &s->held, &s->held_len, &s->held_cap, piece.ptr, piece.size)) { s_reset(s, PROVEN_H2_INTERNAL_ERROR, PROVEN_ERR_NOMEM); return; }
        if (last) s->held_last = true;
        return;
    }
    const proven_u32 id = s->id;
    s_enter(s);
    if ((piece.size > 0 || last) && sv->cfg.on.on_body) sv->cfg.on.on_body(sv->cfg.ctx, (proven_http_stream_t *)(void *)s, piece, last);
    s_leave(s);
    if (!d->dead) proven_h2_conn_consumed(d->engine, id, piece.size);      /* nothing, if the stream has ended */
}

static void d_event(h2d_t *d, const proven_h2_event_t *ev) {
    h2s_t *s = ev->user;
    switch (ev->kind) {
        case PROVEN_H2_EVENT_HEADERS: d_request(d, ev); break;
        case PROVEN_H2_EVENT_DATA:
            if (s) d_body(d, s, ev->data, ev->end_stream);
            else proven_h2_conn_consumed(d->engine, ev->stream_id, ev->data.size);
            break;
        case PROVEN_H2_EVENT_TRAILERS:             /* the fields are not passed on: the event server has no trailers */
            if (s) d_body(d, s, (proven_mem_view_t){ .ptr = (void *)0, .size = 0 }, true);
            break;
        case PROVEN_H2_EVENT_STREAM_RESET: if (s) { s->engine_done = true; s_finish(s, PROVEN_ERR_RESET); } break;
        case PROVEN_H2_EVENT_STREAM_WRITABLE: if (s) { s->window_shut = false; s_push(s); } break;
        case PROVEN_H2_EVENT_CLOSED:
            d->over = true;
            d_end_streams(d, PROVEN_ERR_RESET);
            break;
        default: break;                            /* GOAWAY, PONG: the engine has done what they ask */
    }
}

// -----------------------------------------------------------------------------
// The round: output to the transport, and what that makes due
// -----------------------------------------------------------------------------

static void d_pump(h2d_t *d) {
    if (d->pumping || d->dead) return;
    d->pumping = true;
    d_enter(d);
    for (;;) {
        bool progress = false;
        while (!d->dead) {
            /* Offered even when there is nothing: that withdraws a request to be called back. */
            proven_mem_view_t out = proven_h2_conn_pending_output(d->engine);
            proven_size_t n = proven_http_event_raw_offer_(d->conn, out);     /* a dead transport comes back through on_closed */
            if (d->dead || n == 0) break;
            proven_h2_conn_output_sent(d->engine, n);
            progress = true;
        }
        if (d->dead) break;
        /* What sending made due: a stream the engine had cut short. */
        for (;;) {
            proven_size_t used = 0;
            proven_h2_event_t ev;
            if (proven_h2_conn_feed(d->engine, (proven_mem_view_t){ .ptr = (void *)0, .size = 0 }, &used, &ev) != PROVEN_OK || ev.kind == PROVEN_H2_EVENT_NONE) break;
            d_event(d, &ev);
            progress = true;
            if (d->dead) break;
        }
        if (d->dead) break;
        /* And what this driver itself held back for the output bound. A handler's callback
         * may end any stream, so the list is searched again from its head after each. */
        d->round++;
        for (bool again = true; again && !d->dead;) {
            again = false;
            for (h2s_t *s = d->streams; s; s = s->next) {
                if (s->mark == d->round || s->window_shut || !(s_out_pending(s) > 0 || s->end_asked || s->want_writable)) continue;
                s->mark = d->round;
                if (d_room(d) == 0 && !(s->end_asked && s_out_pending(s) == 0)) continue;
                const proven_size_t before = proven_h2_conn_pending_output(d->engine).size;
                s_push(s);
                if (!d->dead && proven_h2_conn_pending_output(d->engine).size != before) progress = true;
                again = true;
                break;
            }
        }
        if (d->dead || !progress) break;
    }
    if (!d->dead) {
        /* Nothing more to take and nothing left to write: the connection is finished with. */
        if (d->eof && d->stream_count == 0) d->over = true;
        if (d->over && d_queued(d) == 0) {
            proven_http_stream_t *conn = d->conn;
            d->dead = true;
            proven_http_event_raw_close_(conn);    /* detached; closes politely */
        } else {
            d_arm(d);
        }
    }
    d->pumping = false;
    d_leave(d);
}

// -----------------------------------------------------------------------------
// What the connection's record asks of its owner
// -----------------------------------------------------------------------------

static proven_size_t h2_on_input(void *raw, proven_mem_mut_t in) {
    h2d_t *d = raw;
    d_enter(d);
    proven_size_t pos = 0;
    while (!d->dead && !d->over && pos < in.size) {
        proven_size_t used = 0;
        proven_h2_event_t ev;
        proven_err_t e = proven_h2_conn_feed(d->engine, (proven_mem_view_t){ .ptr = in.ptr + pos, .size = in.size - pos }, &used, &ev);
        if (e != PROVEN_OK) { d_kill(d, e == PROVEN_ERR_NOMEM ? PROVEN_ERR_NOMEM : PROVEN_ERR_RESET); break; }
        pos += used;
        if (ev.kind != PROVEN_H2_EVENT_NONE) d_event(d, &ev);
        else if (pos == in.size) break;
    }
    d_pump(d);
    d_leave(d);
    return in.size;                                /* all of it, always: what a stream cannot take yet is its own to keep */
}

static void h2_on_progress(void *raw) { d_pump(raw); }

static void h2_on_timer(void *raw) {
    h2d_t *d = raw;
    if (d->dead) return;
    d_enter(d);
    /* Requests whose bodies have stopped arriving, first: they end, the others go on. */
    const proven_time_t now = proven_time_monotonic_now();
    for (bool again = true; again;) {
        again = false;
        for (h2s_t *s = d->streams; s; s = s->next) {
            if (s->body_due == 0 || s->body_due > now) continue;
            s_reset(s, PROVEN_H2_CANCEL, PROVEN_ERR_TIMEOUT);
            again = true;
            break;
        }
    }
    if (d->dead) { d_leave(d); return; }
    if (d->quiet_due == 0 || now < d->quiet_due) {
        d_pump(d);                                 /* a stream's hour, not the connection's: the round sets the timer again */
    } else if (d_queued(d) > 0 || !d->asked) {
        /* The client is not taking what it asked for, or never asked. */
        d_kill(d, PROVEN_ERR_TIMEOUT);
    } else if (d->stream_count == 0) {
        proven_h2_conn_shutdown(d->engine, PROVEN_H2_NO_ERROR);            /* idle: say goodbye */
        d_pump(d);
    } else {
        d_pump(d);
    }
    d_leave(d);
}

static void h2_on_closed(void *raw, proven_err_t why) {
    h2d_t *d = raw;
    if (d->dead) return;
    d_enter(d);
    d->dead = true;
    d_end_streams(d, why == PROVEN_OK ? PROVEN_ERR_RESET : why);
    d_leave(d);
}

/* The client will send nothing more. What it asked for in full is still answered. */
static bool h2_on_eof(void *raw) {
    h2d_t *d = raw;
    if (d->dead) return false;
    d_enter(d);
    d->eof = true;
    for (bool again = true; again;) {
        again = false;
        for (h2s_t *s = d->streams; s; s = s->next) {
            if (s->in_done) continue;
            s_reset(s, PROVEN_H2_CANCEL, PROVEN_ERR_RESET);
            again = true;
            break;
        }
    }
    const bool keep = !d->dead && (d->stream_count > 0 || d_queued(d) > 0);
    if (keep) d_pump(d);
    const bool still = keep && !d->dead;
    d_leave(d);
    return still;
}

static void h2_on_shutdown(void *raw) {
    h2d_t *d = raw;
    if (d->dead) return;
    d_enter(d);
    proven_h2_conn_shutdown(d->engine, PROVEN_H2_NO_ERROR);
    d_pump(d);
    d_leave(d);
}

static const proven_http_event_raw_ops_t h2_ops = {
    .on_input = h2_on_input, .on_progress = h2_on_progress, .on_timer = h2_on_timer, .on_closed = h2_on_closed,
    .on_eof = h2_on_eof, .on_shutdown = h2_on_shutdown,
};

/* RFC 9113, section 9.2: TLS 1.2 at least, and of 1.2 only the suites that keep forward
 * secrecy and authenticate what they encrypt. (TLS 1.3 has no others.) */
static bool h2_tls_adequate(const proven_tls_conn_t *tls) {
    const proven_u16 version = proven_tls_version(tls), suite = proven_tls_cipher_suite(tls);
    if (version >= 0x0304) return true;
    if (version < 0x0303) return false;
    return suite == 0xc02b || suite == 0xc02c || suite == 0xc02f || suite == 0xc030 ||
           suite == 0xcca8 || suite == 0xcca9 || suite == 0xccaa || suite == 0x009e || suite == 0x009f;
}

proven_err_t proven_http_event_h2_take_(proven_http_stream_t *conn) {
    proven_http_event_server_t *sv = conn->h.server;
    proven_u32 list = sv->cfg.max_head_bytes > ((proven_size_t)1 << 20) ? (proven_u32)1 << 20 : (proven_u32)sv->cfg.max_head_bytes;
    if (!sv->h2buf) {
        sv->h2buf_size = proven_h2_header_buffer_size(list);
        sv->h2buf = h2_alloc(sv->alloc, sv->h2buf_size);
        if (!sv->h2buf) return PROVEN_ERR_NOMEM;
    }
    h2d_t *d = h2_alloc(sv->alloc, sizeof *d);
    if (!d) return PROVEN_ERR_NOMEM;
    for (proven_size_t i = 0; i < sizeof *d; ++i) ((proven_byte_t *)d)[i] = 0;
    proven_h2_config_t cfg = {
        .alloc = sv->alloc, .server = true,
        .max_concurrent_streams = (proven_u32)(sv->cfg.max_concurrent_streams > 100000 ? 100000 : sv->cfg.max_concurrent_streams),
        .max_header_list_bytes = list,
        /* Twice the server's bound: the first half is for bodies, which this driver stops
         * offering at the bound; the rest is room for response heads and the protocol's own
         * answers, so that neither is refused merely because a body filled the first. */
        .max_buffered_output = sv->cfg.max_buffered_output < 512 ? 1024
                             : sv->cfg.max_buffered_output > ((proven_size_t)1 << 29) ? (proven_size_t)1 << 30 : sv->cfg.max_buffered_output * 2,
        .header_buffer = { .ptr = sv->h2buf, .size = sv->h2buf_size },
        .set_header_table = true, .header_table_bytes = (proven_u32)sv->cfg.http2_header_table_bytes,
    };
    if (proven_h2_conn_create(&cfg, &d->engine) != PROVEN_OK) { sv->alloc.free_fn(sv->alloc.ctx, d); return PROVEN_ERR_NOMEM; }
    d->ops = &h2_ops;
    d->conn = conn;
    d->server = sv;
    conn->h.state = EV_RAW;
    conn->raw = d;
    conn->paused = false;
    d_enter(d);
    if (conn->tls && !h2_tls_adequate(conn->tls)) proven_h2_conn_shutdown(d->engine, PROVEN_H2_INADEQUATE_SECURITY);
    d_pump(d);                                     /* the server's SETTINGS */
    d_leave(d);
    return PROVEN_OK;
}

// -----------------------------------------------------------------------------
// What a handler calls, for a stream of an HTTP/2 connection
// -----------------------------------------------------------------------------

static h2s_t *h2s_of(proven_http_stream_t *stream) { return (h2s_t *)(void *)stream; }
static const h2s_t *h2s_of_const(const proven_http_stream_t *stream) { return (const h2s_t *)(const void *)stream; }

void proven_http_event_h2_compress_(proven_http_stream_t *stream) {
    h2s_t *s = h2s_of(stream);
    if (!s->dead && s->active && !s->response_begun) s->compress_asked = true;
}

static bool h2_is_driver_header(proven_u8str_view_t name) {
    return h2_eq_nocase(name, h2_lit("content-length")) || h2_eq_nocase(name, h2_lit("transfer-encoding")) ||
           h2_eq_nocase(name, h2_lit("connection")) || h2_eq_nocase(name, h2_lit("date"));
}

/* Fields that describe an HTTP/1.1 connection. A handler written for HTTP/1.1 may set them;
 * on HTTP/2 they are not sent, rather than the response being refused for them. */
static bool h2_is_hop_header(proven_u8str_view_t name) {
    return h2_eq_nocase(name, h2_lit("keep-alive")) || h2_eq_nocase(name, h2_lit("proxy-connection")) || h2_eq_nocase(name, h2_lit("upgrade"));
}

/* The response head, to the engine. `length`: the body's size on the wire, or LENGTH_UNKNOWN. */
static proven_err_t s_head(h2s_t *s, proven_u16 status, const proven_http_header_t *headers, proven_size_t header_count,
                           proven_u64 length, bool vary, bool gzip, bool end_stream) {
    proven_http_event_server_t *sv = s->h.server;
    h2d_t *d = s->d;
    if (status < 200 || status > 999 || (header_count > 0 && !headers)) return PROVEN_ERR_INVALID_ARG;
    for (proven_size_t i = 0; i < header_count; ++i) if (h2_is_driver_header(headers[i].name)) return PROVEN_ERR_INVALID_ARG;
    /* Names go out in lowercase, as HTTP/2 requires of them: written into the server's head
     * buffer, which is where an HTTP/1.1 head would have been built. */
    proven_http_header_t small[40], *list = small;
    if (header_count + 5 > sizeof small / sizeof small[0]) {
        list = h2_alloc(sv->alloc, (header_count + 5) * sizeof *list);
        if (!list) return PROVEN_ERR_NOMEM;
    }
    proven_byte_t code[3] = { (proven_byte_t)('0' + status / 100), (proven_byte_t)('0' + status / 10 % 10), (proven_byte_t)('0' + status % 10) };
    proven_byte_t date[PROVEN_HTTP_DATE_SIZE], digits[20];
    proven_size_t n = 0, at = 0, nd = 0;
    proven_err_t e = PROVEN_OK;
    list[n++] = (proven_http_header_t){ .name = h2_lit(":status"), .value = { .ptr = code, .size = 3 } };
    if (proven_http_date_format(proven_time_now(), date) == PROVEN_OK) list[n++] = (proven_http_header_t){ .name = h2_lit("date"), .value = { .ptr = date, .size = sizeof date } };
    for (proven_size_t i = 0; i < header_count && e == PROVEN_OK; ++i) {
        if (h2_is_hop_header(headers[i].name)) continue;
        if (at + headers[i].name.size > sv->cfg.max_head_bytes) { e = PROVEN_ERR_OUT_OF_BOUNDS; break; }
        for (proven_size_t k = 0; k < headers[i].name.size; ++k) sv->hbuf[at + k] = h2_lower(headers[i].name.ptr[k]);
        list[n++] = (proven_http_header_t){ .name = { .ptr = sv->hbuf + at, .size = headers[i].name.size }, .value = headers[i].value };
        at += headers[i].name.size;
    }
    const bool no_body_status = status == 204 || status == 304;
    if (e == PROVEN_OK) {
        if (vary) list[n++] = (proven_http_header_t){ .name = h2_lit("vary"), .value = h2_lit("accept-encoding") };
        if (gzip) list[n++] = (proven_http_header_t){ .name = h2_lit("content-encoding"), .value = h2_lit("gzip") };
        if (!no_body_status && length != PROVEN_HTTP_EVENT_LENGTH_UNKNOWN) {
            proven_u64 v = length;
            do { digits[sizeof digits - 1 - nd++] = (proven_byte_t)('0' + v % 10); v /= 10; } while (v > 0);
            list[n++] = (proven_http_header_t){ .name = h2_lit("content-length"), .value = { .ptr = digits + sizeof digits - nd, .size = nd } };
        }
        s->bodiless = no_body_status || s->method == PROVEN_HTTP_HEAD;
        e = proven_h2_conn_submit_response(d->engine, s->id, list, n, end_stream || s->bodiless);
    }
    if (list != small) sv->alloc.free_fn(sv->alloc.ctx, list);
    if (e == PROVEN_ERR_INVALID_ARG || e == PROVEN_ERR_OUT_OF_BOUNDS) { s->bodiless = false; return e; }      /* the handler's list: it may try another */
    if (e != PROVEN_OK) {
        /* No room, no memory, or the stream has gone: there is no response to go on with. */
        s_reset(s, PROVEN_H2_INTERNAL_ERROR, PROVEN_ERR_RESET);
        return PROVEN_ERR_RESET;
    }
    s->response_begun = true;
    if (end_stream || s->bodiless) s->engine_done = true;
    return PROVEN_OK;
}

static void s_compress_plan(const h2s_t *s, proven_u16 status, const proven_http_header_t *headers, proven_size_t header_count, bool *vary, bool *gzip) {
    *vary = false;
    *gzip = false;
    if (!s->compress_asked || (header_count > 0 && !headers)) return;
    *vary = proven_http_compress_needs_vary_(headers, header_count);
    *gzip = proven_http_compress_applies_(s->accepts_gzip, status, headers, header_count);
}

proven_err_t proven_http_event_h2_begin_(proven_http_stream_t *stream, proven_u16 status, const proven_http_header_t *headers, proven_size_t header_count,
                                         proven_u64 content_length) {
    h2s_t *s = h2s_of(stream);
    if (s->dead || !s->active || s->response_begun) return PROVEN_ERR_INVALID_STATE;
    proven_http_event_server_t *sv = s->h.server;
    h2d_t *d = s->d;
    bool vary = false, gzip = false;
    s_compress_plan(s, status, headers, header_count, &vary, &gzip);
    proven_deflate_t *zip = (void *)0;
    if (gzip && s->method != PROVEN_HTTP_HEAD) {
        if (!sv->zbuf) sv->zbuf = h2_alloc(sv->alloc, PROVEN_HTTP_CODING_PIECE);
        if (!sv->zbuf || proven_http_compress_create_(sv->alloc, sv->cfg.compress_level, sv->cfg.compress_window_bits, &zip) != PROVEN_OK) gzip = false;
    }
    s_enter(s);
    d_enter(d);
    d->calls++;
    proven_err_t e = s_head(s, status, headers, header_count, gzip ? PROVEN_HTTP_EVENT_LENGTH_UNKNOWN : content_length, vary, gzip, false);
    if (e != PROVEN_OK || s->dead) {
        if (zip) proven_deflate_destroy(zip);
    } else {
        s->zip = zip;
        s->response_left = s->bodiless ? PROVEN_HTTP_EVENT_LENGTH_UNKNOWN : content_length;
    }
    d_pump(d);
    d->calls--;
    d_leave(d);
    s_leave(s);
    return e;
}

/* Body bytes through the compressor into the stream's own queue. False when there is no memory. */
static bool s_zip(h2s_t *s, const proven_byte_t *p, proven_size_t n, proven_deflate_flush_t flush) {
    proven_http_event_server_t *sv = s->h.server;
    for (;;) {
        proven_size_t used = 0, made = 0;
        bool done = false;
        proven_err_t e = proven_deflate(s->zip, (proven_mem_view_t){ .ptr = p, .size = n }, &used,
                                        (proven_mem_mut_t){ .ptr = sv->zbuf, .size = PROVEN_HTTP_CODING_PIECE }, &made, flush, &done);
        if (e != PROVEN_OK) return false;
        p += used;
        n -= used;
        if (s->out_off > 0 && s->out_off == s->out_len) s->out_off = s->out_len = 0;
        if (!h2_append(sv->alloc, &s->out, &s->out_len, &s->out_cap, sv->zbuf, made)) return false;
        if (flush == PROVEN_DEFLATE_FLUSH_FINISH ? done : (n == 0 && made < PROVEN_HTTP_CODING_PIECE)) return true;
    }
}

proven_result_size_t proven_http_event_h2_write_(proven_http_stream_t *stream, proven_mem_view_t data) {
    h2s_t *s = h2s_of(stream);
    if (s->dead || !s->active || !s->response_begun || s->response_ended) return (proven_result_size_t){ .err = PROVEN_ERR_INVALID_STATE, .value = 0 };
    if (s->bodiless) return (proven_result_size_t){ .err = PROVEN_OK, .value = data.size };     /* counted, not sent */
    if (s->response_left != PROVEN_HTTP_EVENT_LENGTH_UNKNOWN && data.size > s->response_left) return (proven_result_size_t){ .err = PROVEN_ERR_OUT_OF_BOUNDS, .value = 0 };
    h2d_t *d = s->d;
    s_enter(s);
    d_enter(d);
    d->calls++;
    proven_size_t done = 0;
    bool alive = true;
    for (int tries = 0; alive && done < data.size && tries < 2;) {
        /* What this stream made earlier goes first; while any of that waits, nothing new is taken. */
        if (s_out_pending(s) > 0) { s_push(s); if (s->dead) { alive = false; break; } }
        proven_size_t room = s_out_pending(s) > 0 ? 0 : d_room(d);
        if (room == 0) {
            /* A refusal must mean that the client is not taking more: look once more after
             * offering what is queued to the transport. */
            tries++;
            d_pump(d);
            if (s->dead) alive = false;
            continue;
        }
        proven_size_t n = data.size - done < room ? data.size - done : room;
        if (s->zip) {
            if (n > PROVEN_HTTP_CODING_PIECE) n = PROVEN_HTTP_CODING_PIECE;
            if (!s_zip(s, data.ptr + done, n, PROVEN_DEFLATE_FLUSH_NONE)) { s_reset(s, PROVEN_H2_INTERNAL_ERROR, PROVEN_ERR_NOMEM); alive = false; break; }
        } else {
            proven_result_size_t r = proven_h2_conn_send_data(d->engine, s->id, (proven_mem_view_t){ .ptr = data.ptr + done, .size = n }, false);
            if (r.err != PROVEN_OK) { s_finish(s, PROVEN_ERR_RESET); alive = false; break; }
            s->window_shut = r.value < n;                  /* the peer's window: on_writable then waits for the engine's word */
            n = r.value;
            if (s->window_shut) { if (s->response_left != PROVEN_HTTP_EVENT_LENGTH_UNKNOWN) s->response_left -= n; done += n; break; }
        }
        if (s->response_left != PROVEN_HTTP_EVENT_LENGTH_UNKNOWN) s->response_left -= n;
        done += n;
    }
    if (alive && !s->dead) {
        if (s_out_pending(s) > 0) s_push(s);
        if (!s->dead && done < data.size) s->want_writable = true;
    }
    alive = alive && !s->dead;
    d_pump(d);
    d->calls--;
    d_leave(d);
    s_leave(s);
    return alive ? (proven_result_size_t){ .err = PROVEN_OK, .value = done } : (proven_result_size_t){ .err = PROVEN_ERR_RESET, .value = 0 };
}

proven_err_t proven_http_event_h2_end_(proven_http_stream_t *stream) {
    h2s_t *s = h2s_of(stream);
    if (s->dead || !s->active || !s->response_begun || s->response_ended) return PROVEN_ERR_INVALID_STATE;
    h2d_t *d = s->d;
    proven_err_t result = PROVEN_OK;
    s_enter(s);
    d_enter(d);
    d->calls++;
    if (s->response_left != PROVEN_HTTP_EVENT_LENGTH_UNKNOWN && s->response_left != 0) {
        /* Less was written than promised. The client must not take what it has for the whole. */
        s_reset(s, PROVEN_H2_INTERNAL_ERROR, PROVEN_ERR_INVALID_FORMAT);
        result = PROVEN_ERR_INVALID_FORMAT;
    } else {
        s->response_ended = true;
        if (s->zip) {
            if (!s_zip(s, (void *)0, 0, PROVEN_DEFLATE_FLUSH_FINISH)) { s_reset(s, PROVEN_H2_INTERNAL_ERROR, PROVEN_ERR_NOMEM); result = PROVEN_ERR_RESET; }
            else { proven_deflate_destroy(s->zip); s->zip = (void *)0; }
        }
        if (!s->dead) {
            s->end_asked = true;
            s_push(s);                             /* the end goes with, or after, what is queued */
        }
    }
    d_pump(d);
    d->calls--;
    d_leave(d);
    s_leave(s);
    return result;
}

proven_err_t proven_http_event_h2_respond_(proven_http_stream_t *stream, proven_u16 status, const proven_http_header_t *headers, proven_size_t header_count,
                                           proven_mem_view_t body) {
    h2s_t *s = h2s_of(stream);
    if (s->dead || !s->active || s->response_begun) return PROVEN_ERR_INVALID_STATE;
    proven_http_event_server_t *sv = s->h.server;
    h2d_t *d = s->d;
    bool vary = false, gzip = false;
    s_compress_plan(s, status, headers, header_count, &vary, &gzip);
    proven_byte_t *packed = (void *)0;
    proven_size_t packed_cap = 0, packed_size = 0;
    if (gzip && proven_http_compress_all_(sv->alloc, sv->cfg.compress_level, sv->cfg.compress_window_bits, body, &packed, &packed_cap, &packed_size) != PROVEN_OK) packed = (void *)0;
    if (packed) body = (proven_mem_view_t){ .ptr = packed, .size = packed_size };
    s_enter(s);
    d_enter(d);
    d->calls++;
    proven_err_t e = s_head(s, status, headers, header_count, body.size, vary, packed != (void *)0, body.size == 0);
    if (e == PROVEN_OK && !s->dead) {
        s->response_ended = true;
        s->end_asked = true;
        /* The whole response is this call's contract, whatever the output bound: what the
         * engine does not take now is kept, and follows as the peer's window opens. */
        if (!s->bodiless && body.size > 0) {
            proven_result_size_t r = proven_h2_conn_send_data(d->engine, s->id, body, true);
            if (r.err != PROVEN_OK) { s_finish(s, PROVEN_ERR_RESET); e = PROVEN_ERR_RESET; }
            else if (r.value == body.size) s->engine_done = true;
            else if (!h2_append(sv->alloc, &s->out, &s->out_len, &s->out_cap, body.ptr + r.value, body.size - r.value)) { s_reset(s, PROVEN_H2_INTERNAL_ERROR, PROVEN_ERR_NOMEM); e = PROVEN_ERR_NOMEM; }
        }
        if (!s->dead) s_push(s);
    }
    if (packed) sv->alloc.free_fn(sv->alloc.ctx, packed);
    d_pump(d);
    d->calls--;
    d_leave(d);
    s_leave(s);
    return e;
}

void proven_http_event_h2_abort_(proven_http_stream_t *stream) {
    h2s_t *s = h2s_of(stream);
    if (s->dead) return;
    h2d_t *d = s->d;
    s_enter(s);
    d_enter(d);
    d->calls++;
    s_reset(s, PROVEN_H2_INTERNAL_ERROR, PROVEN_ERR_RESET);
    d_pump(d);
    d->calls--;
    d_leave(d);
    s_leave(s);
}

void proven_http_event_h2_pause_(proven_http_stream_t *stream) {
    h2s_t *s = h2s_of(stream);
    if (s->dead || !s->active) return;
    s->paused = true;
    s_expect_body(s);
    d_arm(s->d);
}

void proven_http_event_h2_resume_(proven_http_stream_t *stream) {
    h2s_t *s = h2s_of(stream);
    if (s->dead || !s->paused) return;
    proven_http_event_server_t *sv = s->h.server;
    h2d_t *d = s->d;
    s->paused = false;
    s_expect_body(s);
    s_enter(s);
    d_enter(d);
    d->calls++;
    if (s->held_len > 0 || s->held_last) {
        /* What arrived meanwhile, as one piece - in a buffer that is no longer the stream's,
         * so that a handler that pauses again inside the call starts a new one. */
        proven_byte_t *held = s->held;
        const proven_size_t n = s->held_len;
        const bool last = s->held_last;
        const proven_u32 id = s->id;
        s->held = (void *)0; s->held_len = s->held_cap = 0; s->held_last = false;
        if (sv->cfg.on.on_body) sv->cfg.on.on_body(sv->cfg.ctx, stream, (proven_mem_view_t){ .ptr = held, .size = n }, last);
        if (held) sv->alloc.free_fn(sv->alloc.ctx, held);
        if (!d->dead) proven_h2_conn_consumed(d->engine, id, n);
    }
    d_pump(d);
    d->calls--;
    d_leave(d);
    s_leave(s);
}

void proven_http_event_h2_set_user_(proven_http_stream_t *stream, void *user) { h2s_of(stream)->user = user; }
void *proven_http_event_h2_user_(const proven_http_stream_t *stream) { return h2s_of_const(stream)->user; }
proven_net_addr_t proven_http_event_h2_peer_(const proven_http_stream_t *stream) {
    const h2s_t *s = h2s_of_const(stream);
    return s->dead || s->d->dead ? (proven_net_addr_t){ 0 } : s->d->conn->peer;      /* the connection's: a stream has none of its own */
}

proven_size_t proven_http_event_h2_buffered_(const proven_http_stream_t *stream) {
    const h2s_t *s = h2s_of_const(stream);
    if (s->dead) return 0;
    return s_out_pending(s) + d_queued(s->d);
}

#endif
