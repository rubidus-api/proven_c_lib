#include "proven/http_event_client.h"
#include "../../platform/proven_sys_mem.h"
#include "proven_internal_http_event_client.h"
#include "proven_internal_tls.h"

#if !defined(PROVEN_FREESTANDING) && !defined(PROVEN_NO_NET)

#include "proven/url.h"
#include "proven_internal_http_coding.h"

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
        proven_sys_mem_move(b->ptr, b->ptr + b->off, have);
        b->len = have; b->off = 0;
        return true;
    }
    proven_size_t cap = 256;
    while (cap < have + more) { if (cap > (proven_size_t)-1 / 2) return false; cap *= 2; }
    proven_byte_t *p = cl_alloc(cl->alloc, cap);
    if (!p) return false;
    proven_sys_mem_copy(p, b->ptr + b->off, have);
    if (b->ptr && !b->borrowed) cl->alloc.free_fn(cl->alloc.ctx, b->ptr);
    b->ptr = p; b->len = have; b->off = 0; b->cap = cap; b->borrowed = false;
    return true;
}

static bool buf_append(proven_http_event_client_t *cl, cl_buf_t *b, const proven_byte_t *p, proven_size_t n) {
    if (n == 0) return true;
    if (!buf_own(cl, b, n)) return false;
    proven_sys_mem_copy(b->ptr + b->len, p, n);
    b->len += n;
    return true;
}

// -----------------------------------------------------------------------------
// A request's life
// -----------------------------------------------------------------------------

static void cl_process(proven_http_event_request_t *c);
static void cl_on_timer(void *ctx);
static void cl_enter(proven_http_event_request_t *c);
static void cl_leave(proven_http_event_request_t *c);

static void cl_enter(proven_http_event_request_t *c) { c->h.depth++; }

static void cl_leave(proven_http_event_request_t *c) {
    if (--c->h.depth > 0 || !c->dead) return;
    proven_http_event_client_t *cl = c->h.client;
    buf_release(cl, &c->stash);
    buf_release(cl, &c->out);
    if (c->dec) {
        proven_http_decoder_free_(&c->dec->d);
        cl->alloc.free_fn(cl->alloc.ctx, c->dec);
    }
    cl->alloc.free_fn(cl->alloc.ctx, c);
}

static bool cl_secured(const proven_http_event_request_t *c) { return c->k && (!c->k->tls || proven_tls_is_established(c->k->tls)); }

static proven_size_t cl_buffered(const proven_http_event_request_t *c) {
    proven_size_t n = buf_pending(&c->out);
    if (c->k && c->k->tls) n += proven_tls_pending_output(c->k->tls).size;
    return n;
}

// -----------------------------------------------------------------------------
// Connections: made for a request, kept when its exchange ended cleanly, given to the next
// -----------------------------------------------------------------------------

static void cl_on_io(void *ctx, proven_u8 got);
static void cl_on_idle_timer(void *ctx);
static void cl_kill(proven_http_event_request_t *c, proven_err_t why);
static void cl_watch(proven_http_event_request_t *c);
static void cl_arm(proven_http_event_request_t *c);
static void cl_unwait(proven_http_event_request_t *c);
static proven_err_t cl_conn_open(proven_http_event_request_t *c);
static void cl_conn_close(cl_conn_t *k, bool serve_waiting);
static proven_http_event_request_t *cl_waiter_with_room(const proven_http_event_client_t *cl);

static proven_u8str_view_t cl_host(const proven_http_event_request_t *c) {
    return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)(c + 1), .size = c->host_len };
}

static bool cl_same_addr(const proven_net_addr_t *a, const proven_net_addr_t *b) {
    if (a->family != b->family || a->port != b->port) return false;
    for (proven_size_t i = 0; i < sizeof a->ip; ++i) if (a->ip[i] != b->ip[i]) return false;
    return true;
}

/* Whether the connection leads where the request goes: scheme, host, port, and the address. */
static bool cl_leads_to(const cl_conn_t *k, const proven_http_event_request_t *c) {
    return k->https == c->https && k->port == c->port && cl_same_addr(&k->to, &c->to) &&
           cl_eq_nocase((proven_u8str_view_t){ .ptr = k->host, .size = k->host_len }, cl_host(c));
}

static proven_size_t cl_origin_conns(const proven_http_event_client_t *cl, const proven_http_event_request_t *c) {
    proven_size_t have = 0;
    for (const cl_conn_t *k = cl->conns; k; k = k->next) if (cl_leads_to(k, c)) have++;
    return have;
}

/* The request that has waited longest and whose origin may have one more connection. */
static proven_http_event_request_t *cl_waiter_with_room(const proven_http_event_client_t *cl) {
    for (proven_http_event_request_t *w = cl->wait_head; w; w = w->wnext)
        if (cl_origin_conns(cl, w) < cl->cfg.max_connections_per_origin) return w;
    return (void *)0;
}

/* Whether the client may have one more connection, of either protocol - an idle one closed
 * for it if that is what it takes: an HTTP/1.1 one, the one idle longest, before an HTTP/2
 * one. False: the limit is reached and every connection is in use. */
proven_u32 proven_http_event_client_retry_delay_(const proven_http_event_client_t *cl, proven_u32 tries) {
    const proven_u32 first = cl->cfg.retry_delay_ms, most = cl->cfg.retry_delay_max_ms;
    if (most == 0) return first;
    proven_u64 ms = first;
    for (proven_u32 i = 0; i < tries && ms < most; ++i) ms *= 2;
    return ms < most ? (proven_u32)ms : most;
}

bool proven_http_event_client_room_(proven_http_event_client_t *cl) {
    if (cl->cfg.max_connections == 0) return true;
    if (cl->conn_count + proven_http_event_client_h2_conns_(cl) < cl->cfg.max_connections) return true;
    cl_conn_t *oldest = (void *)0;
    for (cl_conn_t *i = cl->conns; i; i = i->next)
        if (i->idle && (!oldest || i->idle_since < oldest->idle_since)) oldest = i;
    if (oldest) { cl_conn_close(oldest, false); return true; }
    return proven_http_event_client_h2_close_idle_(cl);
}

/* A place was freed by something other than an HTTP/1.1 connection closing - an HTTP/2
 * connection ended: a request that was waiting for one may have it. */
void proven_http_event_client_freed_(proven_http_event_client_t *cl) {
    while (cl->cfg.max_connections != 0 && !cl->closing) {
        proven_http_event_request_t *w = cl_waiter_with_room(cl);
        if (!w) {
            /* Nobody of this protocol: an HTTP/2 request may be waiting for the place. */
            proven_http_event_client_h2_freed_(cl);
            break;
        }
        if (!proven_http_event_client_room_(cl)) break;
        cl_unwait(w);
        proven_err_t e = cl_conn_open(w);
        if (e == PROVEN_OK) { cl_arm(w); break; }
        cl_enter(w);
        cl_kill(w, e);
        cl_leave(w);
    }
}

/* A new connection for a request that has none. On failure nothing is left of it. */
static proven_err_t cl_conn_open(proven_http_event_request_t *c) {
    proven_http_event_client_t *cl = c->h.client;
    cl_conn_t *k = cl_alloc(cl->alloc, sizeof *k);
    if (!k) return PROVEN_ERR_NOMEM;
    for (proven_size_t i = 0; i < sizeof *k; ++i) ((proven_byte_t *)k)[i] = 0;
    k->client = cl;
    k->https = c->https; k->port = c->port; k->to = c->to; k->host_len = c->host_len;
    for (proven_size_t i = 0; i < c->host_len; ++i) k->host[i] = cl_host(c).ptr[i];
    proven_err_t e = PROVEN_OK;
    if (k->https) e = proven_tls_client_create(cl->cfg.tls, cl_host(c), (void *)0, &k->tls);
    if (e == PROVEN_OK) {
        e = proven_net_connect_start(k->to, &k->sock);
        if (e == PROVEN_OK) k->connected_at_start = true;
        else if (e == PROVEN_ERR_AGAIN) e = PROVEN_OK;
    }
    /* Either way the socket is watched for writing: what follows happens from the loop, so
     * that nothing - not even a refusal - is reported from inside the call that began it. */
    if (e == PROVEN_OK) e = proven_loop_io_add(cl->loop, &k->io, proven_net_conn_handle(&k->sock), PROVEN_NET_WRITABLE, cl_on_io, k);
    if (e != PROVEN_OK) {
        if (k->tls) proven_tls_conn_destroy(k->tls);
        if (proven_net_conn_is_open(&k->sock)) (void)proven_net_close(&k->sock);
        cl->alloc.free_fn(cl->alloc.ctx, k);
        return e;
    }
    k->next = cl->conns;
    if (k->next) k->next->prev = k;
    cl->conns = k;
    cl->conn_count++;
    k->owner = c;
    c->k = k;
    c->state = CL_CONNECTING;
    return PROVEN_OK;
}

/* Give a request that has none a connection that is idle. Its bytes leave from the loop. */
static void cl_conn_give(cl_conn_t *k, proven_http_event_request_t *c) {
    proven_http_event_client_t *cl = k->client;
    proven_loop_timer_cancel(cl->loop, &k->timer);
    k->idle = false;
    cl->idle_count--;
    k->owner = c;
    c->k = k;
    c->state = CL_HEAD;
    /* It may have been closed by the server while it sat here: until something arrives on it,
     * a request that can be sent again - one whose body is not written in pieces - keeps what
     * it sent. Only here: a connection made for a request is that request's own, and when a
     * request is sent again it is on such a one, so it is sent again once. */
    c->stale = true;
    c->keep_out = !c->streamed;
    cl_watch(c);
    cl_arm(c);
}

/* Take a request out of the queue of those waiting. */
static void cl_unwait(proven_http_event_request_t *c) {
    proven_http_event_client_t *cl = c->h.client;
    proven_http_event_request_t *before = (void *)0;
    for (proven_http_event_request_t *w = cl->wait_head; w && w != c; w = w->wnext) before = w;
    if (before) before->wnext = c->wnext;
    else cl->wait_head = c->wnext;
    if (cl->wait_tail == c) cl->wait_tail = before;
    c->wnext = (void *)0;
    c->waiting = false;
    cl->wait_count--;
}

/* Close a connection that no request owns. `serve_waiting`: a request to the same place that
 * was waiting may now have a connection of its own - the first such, and if that cannot be
 * made, it ends and the next is tried. */
static void cl_conn_close(cl_conn_t *k, bool serve_waiting) {
    proven_http_event_client_t *cl = k->client;
    if (k->idle) { k->idle = false; cl->idle_count--; }
    proven_loop_timer_cancel(cl->loop, &k->timer);
    proven_loop_io_remove(cl->loop, &k->io);           /* before the close */
    if (k->tls) proven_tls_conn_destroy(k->tls);
    if (proven_net_conn_is_open(&k->sock)) (void)proven_net_close(&k->sock);
    if (k->prev) k->prev->next = k->next;
    else cl->conns = k->next;
    if (k->next) k->next->prev = k->prev;
    cl->conn_count--;
    k->owner = (void *)0;
    while (serve_waiting && !cl->closing) {
        proven_http_event_request_t *w = cl->wait_head;
        while (w && !cl_leads_to(k, w)) w = w->wnext;
        /* With a limit on the whole client, a request to another origin may have been waiting
         * for this place: the one that has waited longest and whose origin has room. */
        if (!w && cl->cfg.max_connections != 0) w = cl_waiter_with_room(cl);
        if (!w) {
            /* Nobody of this protocol: an HTTP/2 request may be waiting for the place. */
            if (cl->cfg.max_connections != 0) proven_http_event_client_h2_freed_(cl);
            break;
        }
        cl_unwait(w);
        proven_err_t e = cl_conn_open(w);
        if (e == PROVEN_OK) { cl_arm(w); break; }
        cl_enter(w);
        cl_kill(w, e);
        cl_leave(w);
    }
    cl->alloc.free_fn(cl->alloc.ctx, k);
}

/* A connection whose exchange ended cleanly: to the first request waiting for its origin, or
 * kept idle - and then the one idle longest goes if there are too many. */
static void cl_conn_keep(cl_conn_t *k) {
    proven_http_event_client_t *cl = k->client;
    k->owner = (void *)0;
    k->uses++;
    k->idle_since = proven_time_monotonic_now();
    k->idle = true;
    cl->idle_count++;
    for (proven_http_event_request_t *w = cl->wait_head; w; w = w->wnext) {
        if (!cl_leads_to(k, w)) continue;
        cl_unwait(w);
        cl_conn_give(k, w);
        return;
    }
    /* Nobody waits for this origin. With a limit on the whole client somebody may be waiting
     * for a place: an idle connection is what is closed to make one. */
    if (cl->cfg.max_connections != 0 && !cl->closing && (cl_waiter_with_room(cl) || proven_http_event_client_h2_waiting_(cl))) { cl_conn_close(k, true); return; }
    (void)proven_loop_io_set(cl->loop, &k->io, PROVEN_NET_READABLE);
    proven_loop_timer_set(cl->loop, &k->timer, cl->cfg.idle_timeout_ms, cl_on_idle_timer, k);
    if (cl->idle_count > cl->cfg.max_kept_connections) {
        cl_conn_t *oldest = (void *)0;
        for (cl_conn_t *i = cl->conns; i; i = i->next)
            if (!i->owner && i != k && (!oldest || i->idle_since < oldest->idle_since)) oldest = i;
        cl_conn_close(oldest ? oldest : k, false);
    }
}

static void cl_on_idle_timer(void *ctx) { cl_conn_close(ctx, false); }

/* Something happened on a connection nobody is using. Under TLS it may be the server's
 * housekeeping (a session ticket), which is read and kept; anything else - a close, or bytes
 * nobody asked for - ends it. */
static void cl_idle_io(cl_conn_t *k) {
    proven_http_event_client_t *cl = k->client;
    proven_result_size_t r = proven_net_read(&k->sock, (proven_mem_mut_t){ .ptr = cl->rbuf, .size = CL_READ_BYTES }, PROVEN_NET_DONT_WAIT);
    if (r.err == PROVEN_ERR_TIMEOUT || r.err == PROVEN_ERR_AGAIN) return;
    bool good = r.err == PROVEN_OK && k->tls != (void *)0;
    for (proven_size_t at = 0; good && at < r.value; ) {
        proven_size_t used = 0;
        if (proven_tls_feed(k->tls, (proven_mem_view_t){ .ptr = cl->rbuf + at, .size = r.value - at }, &used) != PROVEN_OK || used == 0) good = false;
        at += used;
    }
    if (good) {
        proven_result_size_t got = proven_tls_read(k->tls, (proven_mem_mut_t){ .ptr = cl->pbuf, .size = CL_PLAIN_BYTES });
        if (got.err == PROVEN_OK ? got.value > 0 : got.err == PROVEN_ERR_EOF) good = false;
        if (proven_tls_pending_output(k->tls).size > 0) good = false;       /* it wants to answer: not while nobody drives it */
    }
    if (!good) cl_conn_close(k, false);
}

/* Whether the connection of a request that has just ended well can carry another. */
static bool cl_reusable(proven_http_event_request_t *c) {
    proven_http_event_client_t *cl = c->h.client;
    cl_conn_t *k = c->k;
    if (!c->keep || !c->request_ended || c->peer_eof || cl_buffered(c) > 0 || buf_pending(&c->stash) > 0 || k->unfed > 0) return false;
    if (k->tls) {
        /* Nothing more may be waiting inside TLS either. (The stash is empty, so the buffer
         * this reads into holds nothing that is still wanted.) */
        proven_result_size_t got = proven_tls_read(k->tls, (proven_mem_mut_t){ .ptr = cl->pbuf, .size = CL_PLAIN_BYTES });
        if (got.err == PROVEN_OK ? got.value > 0 : got.err == PROVEN_ERR_EOF) return false;
    }
    return true;
}

/* A connection that had carried a request before was given to this one and turned out to be
 * dead before anything came back on it: the server closed it while it sat idle. The request is
 * sent once more, on a connection of its own. An error when it cannot be, and is to end. */
/* Its connection is closed and the request is as it was before any of it was sent. */
static void cl_unsend(proven_http_event_request_t *c, bool serve_waiting) {
    proven_http_event_client_t *cl = c->h.client;
    cl_conn_t *old = c->k;
    c->k = (void *)0;
    old->owner = (void *)0;
    cl_conn_close(old, serve_waiting);
    c->stale = c->keep_out = c->peer_eof = c->sent = false;
    c->out.off = 0;
    c->interim = 0;
    buf_release(cl, &c->stash);
    c->timer_kind = CL_T_NONE;
}

static proven_err_t cl_send_again(proven_http_event_request_t *c) {
    cl_unsend(c, false);                          /* its place is this request's own */
    proven_err_t e = cl_conn_open(c);
    if (e == PROVEN_OK) cl_arm(c);
    return e;
}

/* A connection for a request that has none: one of its origin's that is idle; or a new one,
 * while the origin has fewer than it may and the client has room; or the next that comes
 * free, for which it waits. PROVEN_ERR_BUSY: it would wait, and too many do. */
static proven_err_t cl_place(proven_http_event_request_t *c) {
    proven_http_event_client_t *cl = c->h.client;
    cl_conn_t *idle = (void *)0;
    proven_size_t have = 0;
    for (cl_conn_t *k = cl->conns; k; k = k->next) {
        if (!cl_leads_to(k, c)) continue;
        have++;
        if (k->idle && (!idle || k->idle_since > idle->idle_since)) idle = k;      /* the one used last is the likeliest to be alive */
    }
    if (idle) { cl_conn_give(idle, c); return PROVEN_OK; }      /* arms and watches */
    if (have < cl->cfg.max_connections_per_origin && proven_http_event_client_room_(cl)) {
        proven_err_t e = cl_conn_open(c);
        if (e == PROVEN_OK) cl_arm(c);
        return e;
    }
    if (cl->wait_count >= cl->cfg.max_waiting_requests) return PROVEN_ERR_BUSY;
    c->waiting = true;
    c->wnext = (void *)0;
    if (cl->wait_tail) cl->wait_tail->wnext = c;
    else cl->wait_head = c;
    cl->wait_tail = c;
    cl->wait_count++;
    cl_arm(c);
    return PROVEN_OK;
}

/* The wait before a try is over: the request is given a connection as a new one would be. */
static void cl_on_delay(void *ctx) {
    proven_http_event_request_t *c = ctx;
    cl_enter(c);
    c->timer_kind = CL_T_NONE;
    proven_err_t e = cl_place(c);
    if (e != PROVEN_OK) { c->no_resend = true; cl_kill(c, e); }
    cl_leave(c);
}

/* The end, good or bad: tell the program once, and let the connection go - to the next
 * request, or closed. */
static void cl_kill(proven_http_event_request_t *c, proven_err_t why) {
    if (c->dead) return;
    /* Lost on a connection that had been kept, before anything came back on it: the server
     * closed it while it sat idle - that is the reading taken here - and never saw this
     * request. One that can be sent again is; one whose body was written in pieces cannot be,
     * and its caller is told that it may be started again. */
    proven_http_event_client_t *cl = c->h.client;
    if (why == PROVEN_ERR_RESET && c->stale && !c->no_resend) {
        if (c->keep_out && cl_send_again(c) == PROVEN_OK) return;
        if (c->streamed) why = PROVEN_ERR_AGAIN;
    } else if (why == PROVEN_ERR_RESET && !c->no_resend && c->k && !c->streamed && !c->sent && c->tries < cl->cfg.retry_unsent) {
        /* Lost before any of the request was given to its connection - while TLS was being
         * agreed, mostly: the server cannot have acted on what it was never sent. With
         * retry_unsent that is tried again, on a new connection, so many times. */
        const proven_u32 wait_ms = proven_http_event_client_retry_delay_(cl, c->tries);
        c->tries++;
        if (cl->cfg.retry_delay_ms != 0) {
            /* Not at once: its connection goes (to a request that waits, if one does) and it
             * has none until the time has passed. */
            cl_unsend(c, true);
            /* (Giving the place away can end another request, whose caller is told there and
             * then - and may abort this one from inside that.) */
            if (c->dead) return;
            c->timer_kind = CL_T_DELAY;
            proven_loop_timer_set(cl->loop, &c->timer, wait_ms, cl_on_delay, c);
            return;
        }
        proven_err_t e = cl_send_again(c);
        if (e == PROVEN_OK) return;
        why = e;                                   /* a try that cannot be begun ends the request as that try would have */
    }
    c->dead = true;
    proven_loop_timer_cancel(cl->loop, &c->timer);
    if (c->waiting) cl_unwait(c);
    cl_conn_t *k = c->k;
    const bool keep = k && why == PROVEN_OK && cl_reusable(c);
    c->k = (void *)0;
    if (c->prev) c->prev->next = c->next;
    else cl->requests = c->next;
    if (c->next) c->next->prev = c->prev;
    cl->request_count--;
    /* Before the program is told: a request it starts from on_done finds the connection. */
    if (k) {
        k->owner = (void *)0;
        if (keep) cl_conn_keep(k);
        else cl_conn_close(k, true);
    }
    proven_http_event_request_t *face = c->face;
    c->on.on_done(c->ctx, face, why);
    if (face != c) proven_http_event_client_h2_forwarded_done_(face);
}

/* Which time limit applies depends on what the request is waiting for. Limits that measure a
 * whole step (connecting, the wait for the head) are set once; those that measure silence
 * (a write not taken, a body not arriving) start again with every sign of life. */
static void cl_arm(proven_http_event_request_t *c) {
    if (c->dead || c->timer_kind == CL_T_DELAY) return;        /* (waiting to be tried again: its timer is that wait) */
    proven_http_event_client_t *cl = c->h.client;
    proven_u8 kind = CL_T_NONE;
    proven_u32 ms = 0;
    /* A request that waits for a connection is on the same clock as one whose connection is
     * being made: it started when the request did, and is not started again. */
    if (!c->k || c->state == CL_CONNECTING || !cl_secured(c)) { kind = CL_T_CONNECT; ms = cl->cfg.connect_timeout_ms; }
    else if (cl_buffered(c) > 0) { kind = CL_T_WRITE; ms = cl->cfg.write_timeout_ms; }
    else if (c->state == CL_HEAD && c->request_ended) { kind = CL_T_RESPONSE; ms = cl->cfg.response_timeout_ms; }
    else if (c->state == CL_BODY && !c->paused) { kind = CL_T_BODY; ms = cl->cfg.body_timeout_ms; }
    /* Otherwise the program is producing a request body, or has paused: its business. */
    if (kind == CL_T_NONE) proven_loop_timer_cancel(cl->loop, &c->timer);
    else if (kind != c->timer_kind || kind == CL_T_WRITE || kind == CL_T_BODY) proven_loop_timer_set(cl->loop, &c->timer, ms, cl_on_timer, c);
    c->timer_kind = kind;
}

static void cl_watch(proven_http_event_request_t *c) {
    if (c->dead || !c->k) return;
    proven_u8 want = 0;
    if (c->state == CL_CONNECTING) want = PROVEN_NET_WRITABLE;
    else {
        if (!c->peer_eof && !(c->state == CL_BODY && c->paused)) want |= PROVEN_NET_READABLE;
        if (cl_buffered(c) > 0) want |= PROVEN_NET_WRITABLE;
    }
    (void)proven_loop_io_set(c->h.client->loop, &c->k->io, want);
}

/* Push what is waiting towards the socket, without waiting. False when the request ended. */
static bool cl_flush(proven_http_event_request_t *c) {
    if (c->dead) return false;
    if (!c->k || c->state == CL_CONNECTING) return true;
    cl_conn_t *k = c->k;
    for (;;) {
        if (k->tls && buf_pending(&c->out) > 0 && proven_tls_is_established(k->tls)) {
            proven_result_size_t w = proven_tls_write(k->tls, (proven_mem_view_t){ .ptr = c->out.ptr + c->out.off, .size = buf_pending(&c->out) });
            if (w.err != PROVEN_OK) { cl_kill(c, PROVEN_ERR_RESET); return false; }
            c->out.off += w.value;
            if (w.value > 0) c->sent = true;
        }
        proven_mem_view_t pending = k->tls ? proven_tls_pending_output(k->tls)
                                           : (proven_mem_view_t){ .ptr = c->out.ptr + c->out.off, .size = buf_pending(&c->out) };
        if (pending.size == 0) break;
        proven_result_size_t w = proven_net_write(&k->sock, pending, PROVEN_NET_DONT_WAIT);
        if (w.err == PROVEN_ERR_TIMEOUT || w.err == PROVEN_ERR_AGAIN) break;
        if (w.err != PROVEN_OK) { cl_kill(c, PROVEN_ERR_RESET); return false; }
        if (k->tls) proven_tls_output_sent(k->tls, w.value);
        else { c->out.off += w.value; if (w.value > 0) c->sent = true; }
        if (w.value == 0) break;
    }
    /* (Kept, when it may have to be sent again: see cl_conn_give.) */
    if (buf_pending(&c->out) == 0 && !c->keep_out) buf_release(c->h.client, &c->out);
    return true;
}

/* Something was sent, or the state moved: see what that allows. */
static void cl_progress(proven_http_event_request_t *c) {
    if (c->dead) return;
    cl_enter(c);
    if (c->streamed && !c->request_ended && c->want_writable && cl_buffered(c) < c->h.client->cfg.max_buffered_output) {
        c->want_writable = false;
        if (c->on.on_writable) c->on.on_writable(c->ctx, c->face);
    }
    if (!c->dead) { cl_watch(c); cl_arm(c); }
    cl_leave(c);
}

// -----------------------------------------------------------------------------
// Input
// -----------------------------------------------------------------------------

/* One step of a body that is decoded: take a payload from the framing, or decode one piece of
 * the payload in hand and deliver it. The payload stays in the stash until all of it has been
 * decoded, so a pause between two pieces loses nothing. False when the loop has nothing more
 * to do now. */
static bool cl_decode(proven_http_event_request_t *c) {
    proven_http_event_client_t *cl = c->h.client;
    cl_dec_t *x = c->dec;
    if (!x->have) {
        proven_mem_view_t in = { .ptr = c->stash.ptr + c->stash.off, .size = buf_pending(&c->stash) };
        if (in.size == 0) return false;
        proven_size_t used = 0;
        proven_mem_view_t payload = { 0 };
        bool done = false;
        proven_err_t e = proven_http_body_feed(&c->body, in, &used, &payload, &done);
        if (e != PROVEN_OK) { cl_kill(c, e); return false; }
        if (used == 0) return false;               /* it needs more than is here */
        x->pay_off = payload.size > 0 ? (proven_size_t)(payload.ptr - in.ptr) : used;
        x->pay_len = payload.size;
        x->pay_tail = used - x->pay_off - x->pay_len;
        x->pay_last = done;
        x->have = true;
    }
    proven_size_t used = 0, made = 0;
    proven_err_t e = proven_http_decoder_step_(&x->d, (proven_mem_view_t){ .ptr = c->stash.ptr + c->stash.off + x->pay_off, .size = x->pay_len }, &used,
                                               (proven_mem_mut_t){ .ptr = x->piece, .size = sizeof x->piece }, &made);
    if (e != PROVEN_OK) { cl_kill(c, e); return false; }
    x->pay_off += used;
    x->pay_len -= used;
    if (made > 0) {
        x->total += made;
        if (cl->cfg.max_body_bytes != 0 && x->total > cl->cfg.max_body_bytes) { cl_kill(c, PROVEN_ERR_OUT_OF_BOUNDS); return false; }
        if (c->on.on_body) c->on.on_body(c->ctx, c->face, (proven_mem_view_t){ .ptr = x->piece, .size = made }, false);
        if (!c->dead) cl_arm(c);
        return !c->dead;
    }
    if (x->pay_len > 0) {
        if (used > 0) return true;
        cl_kill(c, PROVEN_ERR_INVALID_FORMAT);     /* input it will not take, and nothing to give */
        return false;
    }
    /* The payload is used up and the decoder holds nothing more of it. */
    c->stash.off += x->pay_off + x->pay_tail;
    x->have = false;
    if (x->pay_last) {
        e = proven_http_decoder_end_(&x->d);
        if (e == PROVEN_OK && c->on.on_body) c->on.on_body(c->ctx, c->face, (proven_mem_view_t){ 0 }, true);
        if (!c->dead) cl_kill(c, e);
        return false;
    }
    cl_arm(c);
    return true;
}

static void cl_process(proven_http_event_request_t *c) {
    proven_http_event_client_t *cl = c->h.client;
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
            proven_http_coding_t coding = PROVEN_HTTP_CODING_IDENTITY;
            if (cl->cfg.decompress && !no_body && proven_http_decoder_wanted_(res.status, res.headers, res.header_count, &coding)) {
                proven_result_mem_mut_t m = cl->alloc.alloc_fn(cl->alloc.ctx, sizeof(cl_dec_t), alignof(cl_dec_t));
                if (m.err != PROVEN_OK) { cl_kill(c, PROVEN_ERR_NOMEM); break; }
                c->dec = (cl_dec_t *)(void *)m.value.ptr;
                c->dec->total = 0;
                c->dec->have = false;
                proven_http_decoder_begin_(&c->dec->d, cl->alloc, coding);
            }
            c->state = CL_BODY;
            c->keep = !cl->cfg.no_connection_reuse && framing.kind != PROVEN_HTTP_BODY_UNTIL_CLOSE && proven_http_response_keep_alive(&res);
            if (c->on.on_response) c->on.on_response(c->ctx, c->face, &res);
            if (c->dead) break;
            if (no_body) { cl_kill(c, PROVEN_OK); break; }
            cl_arm(c);
            continue;
        }
        if (c->state != CL_BODY || c->paused) break;
        if (c->dec) {
            if (!cl_decode(c)) break;
            continue;
        }
        if (in.size == 0) break;
        proven_size_t used = 0;
        proven_mem_view_t payload = { 0 };
        bool done = false;
        proven_err_t e = proven_http_body_feed(&c->body, in, &used, &payload, &done);
        if (e != PROVEN_OK) { cl_kill(c, e); break; }
        c->stash.off += used;
        if ((payload.size > 0 || done) && c->on.on_body) c->on.on_body(c->ctx, c->face, payload, done);
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
            /* A body that was being decoded must also have ended where its stream ends. */
            proven_err_t ended = c->dec ? proven_http_decoder_end_(&c->dec->d) : PROVEN_OK;
            if (ended == PROVEN_OK && c->on.on_body) c->on.on_body(c->ctx, c->face, (proven_mem_view_t){ 0 }, true);
            if (!c->dead) cl_kill(c, ended);
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
    proven_http_event_client_t *cl = c->h.client;
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

/* Something of an answer came back on this connection: it was alive when the request went,
 * and nothing is sent twice now. */
static void cl_alive(proven_http_event_request_t *c) {
    if (!c->stale) return;
    c->stale = c->keep_out = false;
    if (buf_pending(&c->out) == 0) buf_release(c->h.client, &c->out);
}

static void cl_readable(proven_http_event_request_t *c) {
    proven_http_event_client_t *cl = c->h.client;
    cl_conn_t *k = c->k;
    /* The end has been seen and the socket has nothing more to say. A hang-up may still be
     * reported - some systems report one whether or not reading was asked for - and a request
     * that is paused over what it already holds must not take that for a connection cut
     * under it. */
    if (c->peer_eof) return;
    proven_result_size_t r = proven_net_read(&k->sock, (proven_mem_mut_t){ .ptr = cl->rbuf, .size = CL_READ_BYTES }, PROVEN_NET_DONT_WAIT);
    if (r.err == PROVEN_ERR_TIMEOUT || r.err == PROVEN_ERR_AGAIN) return;
    if (r.err == PROVEN_ERR_EOF) {
        /* Under TLS an end of the connection without the engine having been told is a cut,
         * not a close: a body that runs until close must not be taken for whole. */
        if (k->tls) { cl_kill(c, PROVEN_ERR_RESET); return; }
        c->peer_eof = true;
        cl_process(c);
        return;
    }
    if (r.err != PROVEN_OK) { cl_kill(c, r.err == PROVEN_ERR_REFUSED || r.err == PROVEN_ERR_UNREACHABLE ? r.err : PROVEN_ERR_RESET); return; }
    if (!k->tls) {
        if (r.value > 0) cl_alive(c);
        cl_input(c, cl->rbuf, r.value);
        return;
    }
    proven_size_t at = 0;
    while (at < r.value && !c->dead && c->k == k) {
        proven_size_t used = 0;
        proven_err_t e = proven_tls_feed(k->tls, (proven_mem_view_t){ .ptr = cl->rbuf + at, .size = r.value - at }, &used);
        at += used;
        k->unfed = r.value - at;                   /* what cl_reusable must know of, should the response end in here */
        if (e != PROVEN_OK) {
            (void)cl_flush(c);                     /* the alert */
            cl_kill(c, e);
            return;
        }
        for (;;) {
            proven_result_size_t got = proven_tls_read(k->tls, (proven_mem_mut_t){ .ptr = cl->pbuf, .size = CL_PLAIN_BYTES });
            if (got.err == PROVEN_ERR_EOF) { c->peer_eof = true; cl_process(c); break; }
            if (got.err != PROVEN_OK) break;
            /* Under TLS it is what the server said that counts, not that bytes came: its
             * goodbye to a connection it had closed is bytes too. */
            if (got.value > 0) cl_alive(c);
            cl_input(c, cl->pbuf, got.value);
            if (c->dead || c->k != k) return;
        }
        if (c->dead || c->k != k || c->peer_eof) break;
        if (used == 0) break;
    }
    if (c->dead || c->k != k) return;
    k->unfed = 0;
    /* The handshake may just have finished: the request can leave now. */
    if (cl_flush(c)) cl_progress(c);
}

static void cl_on_io(void *ctx, proven_u8 got) {
    cl_conn_t *k = ctx;
    proven_http_event_request_t *c = k->owner;
    if (!c) { cl_idle_io(k); return; }
    cl_enter(c);
    /* (`c->k != k`: the request was sent again on another connection, and this one is gone.) */
    if (!c->dead && c->state == CL_CONNECTING) {
        proven_err_t e = k->connected_at_start ? PROVEN_OK : proven_net_connect_finish(&k->sock);
        if (e == PROVEN_OK) {
            /* A request and the next one on the same connection are not to wait for each other's acknowledgements. */
            (void)proven_net_conn_set_nodelay(&k->sock, true);
            c->state = CL_HEAD;
            if (cl_flush(c)) cl_progress(c);
        } else if (e != PROVEN_ERR_AGAIN) {
            cl_kill(c, e);
        }
    } else {
        if (!c->dead && (got & PROVEN_NET_WRITABLE)) { if (cl_flush(c)) cl_progress(c); }
        if (!c->dead && c->k == k && (got & (PROVEN_NET_READABLE | PROVEN_NET_FAILED))) cl_readable(c);
    }
    cl_leave(c);
}

static void cl_on_timer(void *ctx) {
    proven_http_event_request_t *c = ctx;
    cl_enter(c);
    c->no_resend = true;
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
    if (cfg.idle_timeout_ms == 0) cfg.idle_timeout_ms = 60000;
    if (cfg.max_waiting_requests == 0) cfg.max_waiting_requests = 1000;
    if (cfg.max_connections_per_origin == 0) cfg.max_connections_per_origin = 6;
    if (cfg.max_kept_connections == 0) cfg.max_kept_connections = 8;
    if (cfg.http2_window_bytes > 0x7fffffffu) return PROVEN_ERR_INVALID_ARG;
    /* More kept than there may be at all: said together they contradict. (Left at its
     * default the number kept needs no lowering: there are never more idle than there are.) */
    if (cfg.max_connections != 0 && config && config->max_kept_connections > cfg.max_connections) return PROVEN_ERR_INVALID_ARG;
    /* A bound on a wait that is not there bounds nothing, and is a mistake rather than a wish. */
    if (cfg.retry_delay_max_ms != 0 && (cfg.retry_delay_ms == 0 || cfg.retry_delay_max_ms < cfg.retry_delay_ms)) return PROVEN_ERR_INVALID_ARG;
    if (cfg.tls) {
        /* What TLS may agree to and what the client will speak must be the same thing. */
        bool offers_h2 = false;
        for (proven_size_t i = 0;; ++i) {
            proven_u8str_view_t name = proven_tls_config_alpn(cfg.tls, i);
            if (name.size == 0) break;
            if (name.size == 2 && name.ptr[0] == 'h' && name.ptr[1] == '2') offers_h2 = true;
        }
        if (offers_h2 != cfg.http2) return PROVEN_ERR_INVALID_ARG;
    } else if (cfg.http2) {
        return PROVEN_ERR_INVALID_ARG;             /* HTTP/2 by ALPN needs the TLS that carries ALPN */
    }
    proven_allocator_t a = cfg.alloc;
    proven_http_event_client_t *cl = cl_alloc(a, sizeof *cl);
    if (!cl) return PROVEN_ERR_NOMEM;
    *cl = (proven_http_event_client_t){ .loop = loop, .alloc = a, .cfg = cfg };
    cl->headers = cl_alloc(a, cfg.max_headers * sizeof(proven_http_header_t));
    cl->rbuf = cl_alloc(a, CL_READ_BYTES);
    cl->hbuf = cl_alloc(a, cfg.max_head_bytes);
    if (cfg.tls) cl->pbuf = cl_alloc(a, CL_PLAIN_BYTES);
    /* (An HTTP/2 connection in the clear has no TLS to read through, but shares the buffer's use.) */
    if (!cl->headers || !cl->rbuf || !cl->hbuf || (cfg.tls && !cl->pbuf)) { proven_http_event_client_destroy(cl); return PROVEN_ERR_NOMEM; }
    *out = cl;
    return PROVEN_OK;
}

void proven_http_event_client_destroy(proven_http_event_client_t *cl) {
    if (!cl) return;
    cl->closing = true;
    proven_http_event_client_h2_destroy_(cl);
    while (cl->requests) {
        proven_http_event_request_t *c = cl->requests;
        cl_enter(c);
        c->no_resend = true;
        cl_kill(c, PROVEN_ERR_RESET);
        cl_leave(c);
    }
    /* What is left is idle - or was made, a moment ago, for a request that had been waiting:
     * that request is in the list above, so the loop has ended it by now. */
    while (cl->conns) cl_conn_close(cl->conns, false);
    proven_allocator_t a = cl->alloc;
    if (cl->headers) a.free_fn(a.ctx, cl->headers);
    if (cl->rbuf) a.free_fn(a.ctx, cl->rbuf);
    if (cl->pbuf) a.free_fn(a.ctx, cl->pbuf);
    if (cl->hbuf) a.free_fn(a.ctx, cl->hbuf);
    if (cl->h2buf) a.free_fn(a.ctx, cl->h2buf);
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
    if (e == PROVEN_OK && cl->cfg.decompress &&
        !proven_http_header_find(o->headers, o->header_count, cl_lit("Accept-Encoding"), &(proven_u8str_view_t){0}) &&
        !proven_http_header_find(o->headers, o->header_count, cl_lit("Range"), &(proven_u8str_view_t){0})) {
        e = proven_http_write_header(out, len, cl_lit("Accept-Encoding"), cl_lit("gzip"));
    }
    bool body_method = cl_eq_nocase(method, cl_lit("POST")) || cl_eq_nocase(method, cl_lit("PUT")) || cl_eq_nocase(method, cl_lit("PATCH"));
    if (e == PROVEN_OK) {
        if (o->body.size > 0) e = proven_http_write_header_u64(out, len, cl_lit("Content-Length"), o->body.size);
        else if (o->body_length == PROVEN_HTTP_EVENT_LENGTH_UNKNOWN) e = proven_http_write_header(out, len, cl_lit("Transfer-Encoding"), cl_lit("chunked"));
        else if (o->body_length > 0 || body_method) e = proven_http_write_header_u64(out, len, cl_lit("Content-Length"), o->body_length);
    }
    /* HTTP/1.1 keeps a connection unless told otherwise. */
    if (e == PROVEN_OK && cl->cfg.no_connection_reuse) e = proven_http_write_header(out, len, cl_lit("Connection"), cl_lit("close"));
    if (e == PROVEN_OK) e = proven_http_write_head_end(out, len);
    return e;
}

proven_err_t proven_http_event_client_start(proven_http_event_client_t *cl, const proven_http_event_request_options_t *o,
                                            proven_http_event_request_t **out) {
    return proven_http_event_client_start_h1_(cl, o, (void *)0, out);
}

proven_err_t proven_http_event_client_start_h1_(proven_http_event_client_t *cl, const proven_http_event_request_options_t *o,
                                                proven_http_event_request_t *face, proven_http_event_request_t **out) {
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

    /* With HTTP/2 asked for, the request goes to the connection the client keeps for its
     * origin - unless that origin has shown that it speaks only HTTP/1.1. */
    if (!face && (https ? cl->cfg.http2 : cl->cfg.http2_cleartext)) {
        for (proven_size_t i = 0; i < o->header_count; ++i) if (cl_is_reserved_header(o->headers[i].name)) return PROVEN_ERR_INVALID_ARG;
        proven_err_t h2e = proven_http_event_client_h2_start_(cl, o, https, &u, to, out);
        if (h2e != PROVEN_ERR_UNSUPPORTED) return h2e;
    }

    proven_size_t head_len = 0;
    proven_err_t e = cl_build_head(cl, o, &u, method, &head_len);
    if (e != PROVEN_OK) return e;

    if (u.host.size > 255) return PROVEN_ERR_INVALID_ARG;
    proven_http_event_request_t *c = cl_alloc(cl->alloc, sizeof *c + u.host.size);
    if (!c) return PROVEN_ERR_NOMEM;
    *c = (proven_http_event_request_t){
        .h = { .client = cl, .kind = CL_KIND_H1 }, .on = o->on, .ctx = o->ctx, .state = CL_CONNECTING,
        .method = cl_eq_nocase(method, cl_lit("HEAD")) ? PROVEN_HTTP_HEAD : PROVEN_HTTP_GET,
        .https = https, .to = to, .port = u.has_port ? u.port : proven_url_default_port(u.scheme), .host_len = (proven_u8)u.host.size,
    };
    for (proven_size_t i = 0; i < u.host.size; ++i) ((proven_byte_t *)(c + 1))[i] = u.host.ptr[i];
    if (o->body.size == 0 && o->body_length != 0) {
        c->streamed = true;
        c->chunked = o->body_length == PROVEN_HTTP_EVENT_LENGTH_UNKNOWN;
        c->send_left = o->body_length;
    } else {
        c->request_ended = true;
    }
    if (!buf_append(cl, &c->out, cl->hbuf, head_len) || !buf_append(cl, &c->out, o->body.ptr, o->body.size)) e = PROVEN_ERR_NOMEM;
    c->face = face ? face : c;
    if (e == PROVEN_OK) e = cl_place(c);
    if (e != PROVEN_OK) {
        buf_release(cl, &c->out);
        cl->alloc.free_fn(cl->alloc.ctx, c);
        return e;
    }
    c->next = cl->requests;
    if (c->next) c->next->prev = c;
    cl->requests = c;
    cl->request_count++;
    if (out) *out = c;
    return PROVEN_OK;
}

proven_result_size_t proven_http_event_request_write(proven_http_event_request_t *c, proven_mem_view_t data) {
    if (!c || (data.size > 0 && !data.ptr)) return (proven_result_size_t){ .err = PROVEN_ERR_INVALID_ARG, .value = 0 };
    if (PROVEN_HTTP_EVENT_REQUEST_IS_H2_(c)) return proven_http_event_client_h2_write_(c, data);
    if (c->dead || !c->streamed || c->request_ended) return (proven_result_size_t){ .err = PROVEN_ERR_INVALID_STATE, .value = 0 };
    if (!c->chunked && data.size > c->send_left) return (proven_result_size_t){ .err = PROVEN_ERR_OUT_OF_BOUNDS, .value = 0 };
    proven_http_event_client_t *cl = c->h.client;
    cl_enter(c);
    proven_size_t done = 0;
    bool alive = true, tried = false;
    /* Take what fits under the limit, push it at the socket, and look again: a refusal must
     * mean that the socket itself is not taking more (or that there is no connection yet). */
    while (alive && done < data.size) {
        proven_size_t held = cl_buffered(c);
        proven_size_t room = held < cl->cfg.max_buffered_output ? cl->cfg.max_buffered_output - held : 0;
        if (room == 0) {
            if (tried || !c->k || c->state == CL_CONNECTING || !cl_secured(c)) break;
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
    if (PROVEN_HTTP_EVENT_REQUEST_IS_H2_(c)) return proven_http_event_client_h2_end_(c);
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
            alive = buf_append(c->h.client, &c->out, (const proven_byte_t *)"0\r\n\r\n", 5);
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
    if (!c) return;
    if (PROVEN_HTTP_EVENT_REQUEST_IS_H2_(c)) { proven_http_event_client_h2_abort_(c); return; }
    if (c->dead) return;
    cl_enter(c);
    c->no_resend = true;
    cl_kill(c, PROVEN_ERR_RESET);
    cl_leave(c);
}

void proven_http_event_request_pause(proven_http_event_request_t *c) {
    if (!c) return;
    if (PROVEN_HTTP_EVENT_REQUEST_IS_H2_(c)) { proven_http_event_client_h2_pause_(c); return; }
    if (c->dead || c->paused) return;
    c->paused = true;
    cl_watch(c);
    cl_arm(c);
}

void proven_http_event_request_resume(proven_http_event_request_t *c) {
    if (!c) return;
    if (PROVEN_HTTP_EVENT_REQUEST_IS_H2_(c)) { proven_http_event_client_h2_resume_(c); return; }
    if (c->dead || !c->paused) return;
    c->paused = false;
    cl_enter(c);
    cl_process(c);
    if (!c->dead) { cl_watch(c); cl_arm(c); }
    cl_leave(c);
}

void proven_http_event_request_set_user(proven_http_event_request_t *c, void *user) {
    if (!c) return;
    if (PROVEN_HTTP_EVENT_REQUEST_IS_H2_(c)) proven_http_event_client_h2_set_user_(c, user);
    else c->user = user;
}

void *proven_http_event_request_user(const proven_http_event_request_t *c) {
    if (!c) return (void *)0;
    return PROVEN_HTTP_EVENT_REQUEST_IS_H2_(c) ? proven_http_event_client_h2_user_(c) : c->user;
}

int proven_http_event_request_protocol(const proven_http_event_request_t *c) {
    return c && PROVEN_HTTP_EVENT_REQUEST_IS_H2_(c) && !proven_http_event_client_h2_is_h1_(c) ? 2 : 1;
}

#endif
