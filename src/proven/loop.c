#include "proven/loop.h"
#include "proven_internal_loop.h"

#if !defined(PROVEN_FREESTANDING) && !defined(PROVEN_NO_NET)

#include "proven/time.h"
#include "proven_internal_net.h"
#include <stdatomic.h>

/*
 * The event loop: a selector, a hashed timer wheel, a waker, and a queue other threads post to.
 *
 * Two things about its internals are worth knowing before changing it.
 *
 * Removal during dispatch. A callback may stop watching any socket and cancel any timer,
 * including ones whose events were collected in the same round and have not been delivered.
 * So removing a socket blanks its remaining events in the current batch, and due timers are
 * moved to a list of their own before any is fired, from which a cancel can take them.
 *
 * The post queue. Posting is a push onto a lock-free stack from any thread; the loop takes the
 * whole stack at once and reverses it, which gives the order of posting back.
 *
 * The operations (RFC-0025). Here they are done over the selector: a receive waits for
 * readable, a send writes what goes and waits for writable for the rest, an accept waits for
 * readable on the listener. Interest is dropped lazily - a socket stays asked for readable after
 * its receive is answered, because the next receive usually follows at once, and is dropped only
 * when the selector reports it with nothing in flight - so a connection that is read and armed
 * again costs no change of its watch. What must not be answered inside the call that started it
 * (rule 1 of loop.h) goes on the "done" list, a list of sockets with the bits of what is waiting
 * to be delivered; the list is taken whole and delivered after each part of a round, and a round
 * with anything on it does not sleep. A socket's memory is the caller's again only after
 * `closed`, which is always delivered from that list, after the batch of events being delivered
 * - so an event already collected for a closing socket still points at valid memory.
 *
 * Bytes waiting to be sent are in chunks of PROVEN_LOOP_CHUNK_ bytes from a pool of the loop's,
 * a whole TLS record each; a socket holds chunks only while bytes wait, and one gather write
 * sends a socket's chain.
 */

#define LOOP_WHEEL_SLOTS 1024u
#define LOOP_TICK_NS 16000000
#define LOOP_EVENTS 64u
#define LOOP_SCRATCH ((proven_size_t)64 * 1024)

#define TIMER_IDLE 0
#define TIMER_WHEEL 1
#define TIMER_DUE 2

/* What the selector's tags, and the done list, point at: the first byte tells which. A
 * proven_loop_io_t (internal readiness) has a zero there. */
#define KIND_IO 0
#define KIND_SOCK 1
#define KIND_ACCEPT 2

/* A socket's state. */
#define S_NONE 0
#define S_OPEN 1
#define S_CONNECTING 2
#define S_DEAD 3            /* a connect that failed: only proven_loop_close remains */
#define S_CLOSING 4

/* flags */
#define F_QUEUED 0x01       /* on the done list */
#define F_REG 0x02          /* registered with the selector */
#define F_RECV 0x04         /* a receive (an accept) is in flight */
#define F_WAIT 0x08         /* a send wait is asked for */

/* done: what is waiting to be delivered */
#define D_CONNECTED 0x01
#define D_RECEIVED 0x02
#define D_WRITABLE 0x04
#define D_FLUSH 0x08        /* not a callback: committed bytes to be written */
#define D_ACCEPT 0x10
#define D_CLOSED 0x20

#define LOOP_SEND_LIMIT_K 64u
#define LOOP_POOL_KEEP 32u

typedef struct loop_node {
    proven_u8 kind, flags, want, done, state, family;
    proven_u16 err;                       /* the receive's answer, or the accept's */
    struct loop_node *next;               /* on the done list */
} loop_node_t;

typedef struct loop_chunk {
    struct loop_chunk *next;              /* in its socket's chain, or in the pool */
    struct loop_chunk *all_prev, *all_next;    /* every chunk out of the pool, for destroy */
    struct loop_chunk *last;              /* in the first chunk of a socket's chain: its last */
    proven_u32 used, sent;
    proven_byte_t data[PROVEN_LOOP_CHUNK_];
} loop_chunk_t;

typedef struct loop_sock {
    loop_node_t node;                     /* node.err: the answer of a receive, or of a connect */
    proven_u16 send_err;                  /* stays once a send failed */
    proven_u16 limit_k;                   /* what proven_loop_send holds at most, in KiB; LIMIT_NONE: no bound */
    proven_u32 recv_cap;
    proven_u32 queued, wait_at;
    proven_uintptr_t handle;
    const proven_loop_sock_fns_t *fns;
    void *ctx;
    proven_byte_t *recv_buf;
    loop_chunk_t *head;                   /* bytes waiting to be sent; head->last is the chain's end */
} loop_sock_t;

#define LIMIT_NONE 0xffffu
static proven_u32 sock_limit(const loop_sock_t *s) { return s->limit_k == LIMIT_NONE ? 0xffffffffu : (proven_u32)s->limit_k * 1024u; }
static loop_chunk_t *sock_tail(const loop_sock_t *s) { return s->head ? s->head->last : (void *)0; }

typedef struct loop_acc {
    loop_node_t node;
    proven_uintptr_t handle;
    proven_net_listener_t *listener;
    proven_loop_accept_fn fn;
    void *ctx;
    proven_net_conn_t conn;               /* the accepted connection, until delivered */
} loop_acc_t;

_Static_assert(sizeof(loop_sock_t) <= sizeof(proven_loop_sock_t), "proven_loop_sock_t holds the loop's socket");
_Static_assert(_Alignof(loop_sock_t) <= _Alignof(proven_loop_sock_t), "and is aligned for it");
_Static_assert(sizeof(loop_acc_t) <= sizeof(proven_loop_accept_t), "proven_loop_accept_t holds the loop's acceptor");
_Static_assert(_Alignof(loop_acc_t) <= _Alignof(proven_loop_accept_t), "and is aligned for it");
_Static_assert(PROVEN_LOOP_CHUNK_ >= 16384 + 5 + 16 + 1, "a chunk holds a whole TLS record");

typedef struct loop_task {
    struct loop_task *next;
    proven_loop_fn fn;
    void *ctx;
} proven_loop_task_t;

struct proven_loop {
    proven_allocator_t alloc;
    proven_net_selector_t *selector;
    proven_net_waker_t waker;
    bool waker_open;
    proven_loop_io_t waker_io;
    _Atomic(proven_loop_task_t *) tasks;
    atomic_bool stop;
    proven_loop_timer_t *wheel[LOOP_WHEEL_SLOTS];
    proven_loop_timer_t *due;                     /* timers taken off the wheel, not yet fired */
    proven_u64 wheel_tick;
    proven_size_t timed_count;
    proven_net_ready_t batch[LOOP_EVENTS];        /* the events of the round being delivered */
    proven_size_t batch_count, batch_pos;
    proven_size_t io_count;
    proven_byte_t *scratch;
    proven_loop_after_t *after_first, *after_last;    /* "after this round", in the order asked */
    proven_size_t after_count;
    proven_loop_clock_fn clock;                   /* tests only: null is the monotonic clock */
    void *clock_ctx;
    loop_node_t *done_first, *done_last;          /* what waits to be delivered */
    loop_chunk_t *pool;                           /* free chunks, at most LOOP_POOL_KEEP */
    proven_size_t pool_count;
    loop_chunk_t *chunks;                         /* every chunk out of the pool */
    loop_acc_t *delivering;                       /* the acceptor whose function runs now */
};

/* The one place the loop reads the time. */
static proven_time_t loop_now(const proven_loop_t *l) { return l->clock ? l->clock(l->clock_ctx) : proven_time_monotonic_now(); }

void proven_loop_test_clock_(proven_loop_t *l, proven_loop_clock_fn clock, void *ctx) {
    if (!l) return;
    l->clock = clock;
    l->clock_ctx = ctx;
    l->wheel_tick = (proven_u64)loop_now(l) / LOOP_TICK_NS;
}

static void loop_waker_ready(void *ctx, proven_u8 got) {
    (void)got;
    proven_net_waker_drain(&((proven_loop_t *)ctx)->waker);
}

static proven_err_t loop_create(proven_allocator_t alloc, proven_loop_t **out, bool poll_kind) {
    if (out) *out = (void *)0;
    if (!out || !proven_alloc_is_valid(alloc)) return PROVEN_ERR_INVALID_ARG;
    proven_result_mem_mut_t m = alloc.alloc_fn(alloc.ctx, sizeof(proven_loop_t), 16);
    if (m.err != PROVEN_OK) return m.err;
    proven_loop_t *l = (proven_loop_t *)m.value.ptr;
    for (proven_size_t i = 0; i < sizeof *l; ++i) ((proven_byte_t *)l)[i] = 0;
    l->alloc = alloc;
    atomic_init(&l->tasks, (proven_loop_task_t *)0);
    atomic_init(&l->stop, false);
    l->wheel_tick = (proven_u64)loop_now(l) / LOOP_TICK_NS;
    proven_err_t e = poll_kind ? proven_net_selector_create_poll(alloc, &l->selector) : proven_net_selector_create(alloc, &l->selector);
    if (e == PROVEN_OK) {
        m = alloc.alloc_fn(alloc.ctx, LOOP_SCRATCH, 16);
        if (m.err != PROVEN_OK) e = m.err;
        else l->scratch = m.value.ptr;
    }
    if (e == PROVEN_OK) {
        e = proven_net_waker_open(&l->waker);
        l->waker_open = e == PROVEN_OK;
    }
    if (e == PROVEN_OK) e = proven_loop_io_add(l, &l->waker_io, proven_net_waker_handle(&l->waker), PROVEN_NET_READABLE, loop_waker_ready, l);
    if (e != PROVEN_OK) { proven_loop_destroy(l); return e; }
    l->io_count = 0;                              /* the waker is the loop's own, not a watched socket */
    *out = l;
    return PROVEN_OK;
}

proven_err_t proven_loop_create(proven_allocator_t alloc, proven_loop_t **out) { return loop_create(alloc, out, false); }
proven_err_t proven_loop_create_poll_(proven_allocator_t alloc, proven_loop_t **out) { return loop_create(alloc, out, true); }

void proven_loop_destroy(proven_loop_t *l) {
    if (!l) return;
    proven_allocator_t a = l->alloc;
    /* What has already ended gets its last answer, so that its owner can free it: `closed` of a
     * socket that was closed, and an accept that was cancelled. Nothing else is called. */
    for (loop_node_t *n = l->done_first, *next; n; n = next) {
        next = n->next;
        if (n->kind == KIND_SOCK && (n->done & D_CLOSED) && n->state == S_CLOSING) {
            loop_sock_t *s = (loop_sock_t *)n;
            s->node.state = S_NONE;
            if (s->fns->closed) s->fns->closed(s->ctx);
        } else if (n->kind == KIND_ACCEPT && (n->done & D_ACCEPT) && n->err == (proven_u16)PROVEN_ERR_CANCELLED) {
            loop_acc_t *acc = (loop_acc_t *)n;
            acc->node.flags &= (proven_u8)~F_RECV;
            proven_net_conn_t none = { 0 };
            acc->fn(acc->ctx, PROVEN_ERR_CANCELLED, &none);
        }
    }
    l->done_first = l->done_last = (void *)0;
    proven_loop_task_t *t = atomic_exchange_explicit(&l->tasks, (proven_loop_task_t *)0, memory_order_acquire);
    while (t) { proven_loop_task_t *next = t->next; a.free_fn(a.ctx, t); t = next; }
    if (l->waker_open) {
        if (l->waker_io.registered) (void)proven_net_selector_remove(l->selector, l->waker_io.handle);
        proven_net_waker_close(&l->waker);
    }
    if (l->selector) proven_net_selector_destroy(l->selector);
    if (l->scratch) a.free_fn(a.ctx, l->scratch);
    while (l->pool) { loop_chunk_t *next = l->pool->next; a.free_fn(a.ctx, l->pool); l->pool = next; }
    while (l->chunks) { loop_chunk_t *next = l->chunks->all_next; a.free_fn(a.ctx, l->chunks); l->chunks = next; }
    a.free_fn(a.ctx, l);
}

proven_mem_mut_t proven_loop_scratch(proven_loop_t *l) {
    if (!l) return (proven_mem_mut_t){ .ptr = (void *)0, .size = 0 };
    return (proven_mem_mut_t){ .ptr = l->scratch, .size = LOOP_SCRATCH };
}

proven_allocator_t proven_loop_allocator(const proven_loop_t *l) { return l->alloc; }
proven_size_t proven_loop_io_count(const proven_loop_t *l) { return l ? l->io_count : 0; }

// -----------------------------------------------------------------------------
// Sockets
// -----------------------------------------------------------------------------

proven_err_t proven_loop_io_add(proven_loop_t *l, proven_loop_io_t *io, proven_net_handle_t handle,
                                proven_u8 want, proven_loop_io_fn fn, void *ctx) {
    if (!l || !io || !fn || !handle.valid) return PROVEN_ERR_INVALID_ARG;
    if (io->registered) return PROVEN_ERR_INVALID_STATE;
    proven_err_t e = proven_net_selector_add(l->selector, handle, want, io);
    if (e != PROVEN_OK) return e;
    io->fn = fn; io->ctx = ctx; io->handle = handle; io->want = want; io->registered = true;
    l->io_count++;
    return PROVEN_OK;
}

proven_err_t proven_loop_io_set(proven_loop_t *l, proven_loop_io_t *io, proven_u8 want) {
    if (!l || !io || !io->registered) return PROVEN_ERR_INVALID_ARG;
    if (io->want == want) return PROVEN_OK;
    proven_err_t e = proven_net_selector_modify(l->selector, io->handle, want, io);
    if (e == PROVEN_OK) io->want = want;
    return e;
}

void proven_loop_io_remove(proven_loop_t *l, proven_loop_io_t *io) {
    if (!l || !io || !io->registered) return;
    (void)proven_net_selector_remove(l->selector, io->handle);
    io->registered = false;
    if (l->io_count > 0) l->io_count--;
    /* Events for it that this round has collected and not yet delivered must not arrive: the
     * memory behind `io` may be gone by then. */
    for (proven_size_t i = l->batch_pos; i < l->batch_count; ++i) {
        if (l->batch[i].tag == io) l->batch[i].tag = (void *)0;
    }
}

// -----------------------------------------------------------------------------
// Operations (RFC-0025), over the selector
// -----------------------------------------------------------------------------

static loop_sock_t *sock_of(proven_loop_sock_t *s) { return (loop_sock_t *)(void *)s; }
static loop_acc_t *acc_of(proven_loop_accept_t *a) { return (loop_acc_t *)(void *)a; }

static proven_net_conn_t sock_conn(const loop_sock_t *s) {
    proven_net_conn_t c = { 0 };
    c.internal.handle = s->handle;
    c.internal.open = true;
    c.internal.family = s->node.family;
    return c;
}

/* Ask the selector for `want` of a node's socket, registering it on first need. */
static proven_err_t node_watch(proven_loop_t *l, loop_node_t *n, proven_uintptr_t handle, proven_u8 want) {
    proven_net_handle_t h = { .raw = handle, .valid = true };
    proven_err_t e = PROVEN_OK;
    if (n->flags & F_REG) {
        if (n->want == want) return PROVEN_OK;
        e = proven_net_selector_modify(l->selector, h, want, n);
    } else {
        if (want == 0) return PROVEN_OK;
        e = proven_net_selector_add(l->selector, h, want, n);
        if (e == PROVEN_OK) n->flags |= F_REG;
    }
    if (e == PROVEN_OK) n->want = want;
    return e;
}

/* Forget a node's socket: nothing more from the selector, not even an event of this batch. */
static void node_unwatch(proven_loop_t *l, loop_node_t *n, proven_uintptr_t handle) {
    if (n->flags & F_REG) (void)proven_net_selector_remove(l->selector, (proven_net_handle_t){ .raw = handle, .valid = true });
    n->flags &= (proven_u8)~F_REG;
    n->want = 0;
    for (proven_size_t i = l->batch_pos; i < l->batch_count; ++i) {
        if (l->batch[i].tag == n) l->batch[i].tag = (void *)0;
    }
}

static void node_queue(proven_loop_t *l, loop_node_t *n, proven_u8 bits) {
    n->done |= bits;
    if (n->flags & F_QUEUED) return;
    n->flags |= F_QUEUED;
    n->next = (void *)0;
    if (l->done_last) l->done_last->next = n; else l->done_first = n;
    l->done_last = n;
}

// --- chunks

static loop_chunk_t *chunk_take(proven_loop_t *l) {
    loop_chunk_t *c = l->pool;
    if (c) { l->pool = c->next; l->pool_count--; }
    else {
        proven_result_mem_mut_t m = l->alloc.alloc_fn(l->alloc.ctx, sizeof(loop_chunk_t), 16);
        if (m.err != PROVEN_OK) return (void *)0;
        c = (loop_chunk_t *)m.value.ptr;
    }
    c->next = (void *)0;
    c->used = c->sent = 0;
    c->all_prev = (void *)0;
    c->all_next = l->chunks;
    if (l->chunks) l->chunks->all_prev = c;
    l->chunks = c;
    return c;
}

static void chunk_give(proven_loop_t *l, loop_chunk_t *c) {
    if (c->all_prev) c->all_prev->all_next = c->all_next; else l->chunks = c->all_next;
    if (c->all_next) c->all_next->all_prev = c->all_prev;
    if (l->pool_count >= LOOP_POOL_KEEP) { l->alloc.free_fn(l->alloc.ctx, c); return; }
    c->next = l->pool;
    l->pool = c;
    l->pool_count++;
}

static void sock_drop_chunks(proven_loop_t *l, loop_sock_t *s) {
    while (s->head) { loop_chunk_t *next = s->head->next; chunk_give(l, s->head); s->head = next; }
    s->queued = 0;
}

/* Room for `min` bytes at the end of the socket's chain. */
static proven_err_t sock_room(proven_loop_t *l, loop_sock_t *s, proven_size_t min, proven_mem_mut_t *room) {
    if (min == 0) min = 1;
    loop_chunk_t *tail = sock_tail(s);
    if (!tail || PROVEN_LOOP_CHUNK_ - tail->used < min) {
        loop_chunk_t *c = chunk_take(l);
        if (!c) return PROVEN_ERR_NOMEM;
        if (tail) tail->next = c; else s->head = c;
        s->head->last = tail = c;
    }
    *room = (proven_mem_mut_t){ .ptr = tail->data + tail->used, .size = PROVEN_LOOP_CHUNK_ - tail->used };
    return PROVEN_OK;
}

// --- delivery

static void sock_deliver_writable(loop_sock_t *s, proven_err_t err) {
    if (s->fns->writable) s->fns->writable(s->ctx, err);
}

/* Write what waits; answer a wait that has come true, or the failure. Only ever from the loop. */
static void sock_flush(proven_loop_t *l, loop_sock_t *s) {
    proven_err_t e = PROVEN_OK;
    for (int pass = 0; pass < 4 && s->head; ++pass) {
        proven_mem_view_t vec[PROVEN_NET_VEC_MAX];
        proven_size_t count = 0, offered = 0;
        for (loop_chunk_t *c = s->head; c && count < PROVEN_NET_VEC_MAX; c = c->next) {
            if (c->used == c->sent) continue;
            vec[count++] = (proven_mem_view_t){ .ptr = c->data + c->sent, .size = c->used - c->sent };
            offered += c->used - c->sent;
        }
        proven_size_t sent = 0;
        proven_net_conn_t conn = sock_conn(s);
        if (offered > 0) e = proven_net_write_vec_(&conn, vec, count, &sent);
        s->queued -= (proven_u32)sent;
        while (s->head) {
            loop_chunk_t *c = s->head;
            proven_size_t here = c->used - c->sent;
            if (sent < here) { c->sent += (proven_u32)sent; break; }
            sent -= here;
            s->head = c->next;
            if (s->head) s->head->last = c->last;
            chunk_give(l, c);
        }
        if (e != PROVEN_OK || offered == 0 || s->queued > 0) break;
    }
    if (e != PROVEN_OK && e != PROVEN_ERR_TIMEOUT) {
        s->send_err = (proven_u16)e;
        sock_drop_chunks(l, s);
        (void)node_watch(l, &s->node, s->handle, (proven_u8)(s->node.want & ~PROVEN_NET_WRITABLE));
        s->node.flags &= (proven_u8)~F_WAIT;
        sock_deliver_writable(s, e);
        return;
    }
    proven_u8 want = s->queued > 0 ? (proven_u8)(s->node.want | PROVEN_NET_WRITABLE) : (proven_u8)(s->node.want & ~PROVEN_NET_WRITABLE);
    (void)node_watch(l, &s->node, s->handle, want);
    if ((s->node.flags & F_WAIT) && s->queued <= s->wait_at) {
        s->node.flags &= (proven_u8)~F_WAIT;
        sock_deliver_writable(s, PROVEN_OK);
    }
}

static void sock_connected(loop_sock_t *s, proven_err_t err) {
    if (s->node.state == S_CONNECTING) s->node.state = err == PROVEN_OK ? S_OPEN : S_DEAD;
    s->fns->connected(s->ctx, err);
}

/* What the done list holds for a socket, in this order; bits set meanwhile wait for the next
 * time. `closed` comes alone at the end: after it the memory is the caller's. */
static void sock_deliver(proven_loop_t *l, loop_sock_t *s) {
    proven_u8 bits = s->node.done;
    s->node.done = 0;
    /* Deferred, a receive is answered only when cancelled: nothing taken. */
    if (bits & D_CONNECTED) sock_connected(s, (proven_err_t)s->node.err);
    if (bits & D_RECEIVED) s->fns->received(s->ctx, (proven_err_t)s->node.err, 0);
    if (bits & D_WRITABLE) {
        proven_err_t e = s->node.state == S_CLOSING ? PROVEN_ERR_CANCELLED : (proven_err_t)s->send_err;
        sock_deliver_writable(s, e);
    }
    if ((bits & D_FLUSH) && s->node.state == S_OPEN && s->head && !(s->node.want & PROVEN_NET_WRITABLE)) sock_flush(l, s);
    if ((bits & D_CLOSED) && s->node.state == S_CLOSING) {
        s->node.state = S_NONE;
        if (s->fns->closed) s->fns->closed(s->ctx);
    }
}

static void acc_deliver(proven_loop_t *l, loop_acc_t *a) {
    a->node.done = 0;
    a->node.flags &= (proven_u8)~F_RECV;
    proven_err_t err = (proven_err_t)a->node.err;
    proven_net_conn_t conn = a->conn;
    a->conn = (proven_net_conn_t){ 0 };
    l->delivering = a;
    a->fn(a->ctx, err, &conn);
    if (l->delivering == a) l->delivering = (void *)0;
}

/* Deliver what is on the done list now; what is queued meanwhile waits for the next call. */
static void loop_run_done(proven_loop_t *l) {
    loop_node_t *n = l->done_first;
    l->done_first = l->done_last = (void *)0;
    while (n) {
        loop_node_t *next = n->next;
        n->flags &= (proven_u8)~F_QUEUED;
        if (n->kind == KIND_SOCK) sock_deliver(l, (loop_sock_t *)n);
        else if (n->kind == KIND_ACCEPT && (n->done & D_ACCEPT)) acc_deliver(l, (loop_acc_t *)n);
        n = next;
    }
}

// --- what the selector reports

static void sock_event(proven_loop_t *l, loop_sock_t *s, proven_u8 got) {
    if (s->node.state == S_CONNECTING) {
        if (!(got & (PROVEN_NET_WRITABLE | PROVEN_NET_FAILED)) || (s->node.done & D_CONNECTED)) return;
        proven_net_conn_t conn = sock_conn(s);
        proven_err_t e = proven_net_connect_result_(&conn);
        if (e == PROVEN_ERR_AGAIN) return;
        (void)node_watch(l, &s->node, s->handle, 0);
        sock_connected(s, e);
        return;
    }
    if (s->node.state != S_OPEN) return;
    if (got & (PROVEN_NET_READABLE | PROVEN_NET_FAILED)) {
        if (s->node.flags & F_RECV) {
            proven_err_t e = PROVEN_OK;
            proven_size_t n = 0;
            if (s->recv_cap > 0) {
                proven_net_conn_t conn = sock_conn(s);
                proven_result_size_t r = proven_net_read(&conn, (proven_mem_mut_t){ .ptr = s->recv_buf, .size = s->recv_cap }, PROVEN_NET_DONT_WAIT);
                e = r.err;
                n = r.value;
            }
            if (e != PROVEN_ERR_TIMEOUT) {
                s->node.flags &= (proven_u8)~F_RECV;
                s->fns->received(s->ctx, e, n);
            }
        } else if (s->node.want & PROVEN_NET_READABLE) {
            (void)node_watch(l, &s->node, s->handle, (proven_u8)(s->node.want & ~PROVEN_NET_READABLE));
        }
    }
    if (s->node.state != S_OPEN) return;
    if (got & (PROVEN_NET_WRITABLE | PROVEN_NET_FAILED)) {
        if (s->head) sock_flush(l, s);
        else if (s->node.want & PROVEN_NET_WRITABLE) (void)node_watch(l, &s->node, s->handle, (proven_u8)(s->node.want & ~PROVEN_NET_WRITABLE));
    }
}

/* Take one connection now. True when there is an answer to deliver: a connection, or a failure
 * worth reporting; a connection the peer aborted before it was taken is not. */
static bool acc_try(loop_acc_t *a) {
    proven_err_t e = proven_net_accept(a->listener, PROVEN_NET_DONT_WAIT, &a->conn, (void *)0);
    if (e == PROVEN_ERR_TIMEOUT || e == PROVEN_ERR_RESET) return false;
    a->node.err = (proven_u16)e;
    return true;
}

static void acc_event(proven_loop_t *l, loop_acc_t *a, proven_u8 got) {
    (void)got;
    if (!(a->node.flags & F_RECV)) {
        (void)node_watch(l, &a->node, a->handle, 0);
        return;
    }
    if ((a->node.done & D_ACCEPT) || !acc_try(a)) return;
    acc_deliver(l, a);
}

// --- the calls

proven_err_t proven_loop_attach(proven_loop_t *l, proven_loop_sock_t *sock, const proven_net_conn_t *conn,
                                const proven_loop_sock_fns_t *fns, void *ctx) {
    if (!l || !sock || !conn || !fns || !conn->internal.open) return PROVEN_ERR_INVALID_ARG;
    loop_sock_t *s = sock_of(sock);
    for (proven_size_t i = 0; i < sizeof *s; ++i) ((proven_byte_t *)s)[i] = 0;
    s->node.kind = KIND_SOCK;
    s->node.state = S_OPEN;
    s->node.family = conn->internal.family;
    s->handle = conn->internal.handle;
    s->fns = fns;
    s->ctx = ctx;
    s->limit_k = LOOP_SEND_LIMIT_K;
    return PROVEN_OK;
}

proven_err_t proven_loop_connect(proven_loop_t *l, proven_loop_sock_t *sock, proven_net_addr_t to, proven_net_conn_t *out,
                                 const proven_loop_sock_fns_t *fns, void *ctx) {
    if (!l || !sock || !out || !fns || !fns->connected) return PROVEN_ERR_INVALID_ARG;
    proven_err_t e = proven_net_connect_start(to, out);
    if (e != PROVEN_OK && e != PROVEN_ERR_AGAIN) return e;
    (void)proven_loop_attach(l, sock, out, fns, ctx);
    loop_sock_t *s = sock_of(sock);
    s->node.state = S_CONNECTING;
    if (e == PROVEN_OK) {                         /* connected already: answered from the loop */
        s->node.err = PROVEN_OK;
        node_queue(l, &s->node, D_CONNECTED);
        return PROVEN_OK;
    }
    e = node_watch(l, &s->node, s->handle, PROVEN_NET_WRITABLE);
    if (e != PROVEN_OK) {
        s->node.state = S_NONE;
        (void)proven_net_close(out);
        return e;
    }
    return PROVEN_OK;
}

proven_err_t proven_loop_recv(proven_loop_t *l, proven_loop_sock_t *sock, proven_mem_mut_t buf) {
    if (!l || !sock || (buf.size > 0 && !buf.ptr)) return PROVEN_ERR_INVALID_ARG;
    loop_sock_t *s = sock_of(sock);
    if (s->node.kind != KIND_SOCK || s->node.state != S_OPEN) return PROVEN_ERR_INVALID_STATE;
    if (!s->fns->received) return PROVEN_ERR_INVALID_ARG;
    if (s->node.flags & F_RECV) return PROVEN_ERR_BUSY;
    proven_err_t e = node_watch(l, &s->node, s->handle, (proven_u8)(s->node.want | PROVEN_NET_READABLE));
    if (e != PROVEN_OK) return e;
    s->recv_buf = buf.ptr;
    s->recv_cap = buf.size > 0x7fffffffu ? 0x7fffffffu : (proven_u32)buf.size;
    s->node.flags |= F_RECV;
    return PROVEN_OK;
}

void proven_loop_recv_cancel(proven_loop_t *l, proven_loop_sock_t *sock) {
    if (!l || !sock) return;
    loop_sock_t *s = sock_of(sock);
    if (s->node.kind != KIND_SOCK || !(s->node.flags & F_RECV)) return;
    s->node.flags &= (proven_u8)~F_RECV;
    s->node.err = (proven_u16)PROVEN_ERR_CANCELLED;
    node_queue(l, &s->node, D_RECEIVED);
}

/* Copy what the socket did not take into the chain, up to the limit. */
static proven_err_t sock_hold(proven_loop_t *l, loop_sock_t *s, const proven_byte_t *p, proven_size_t n, proven_size_t *taken) {
    *taken = 0;
    while (*taken < n) {
        const proven_u32 limit = sock_limit(s);
        if (s->queued >= limit) return PROVEN_ERR_AGAIN;
        proven_mem_mut_t room;
        proven_err_t e = sock_room(l, s, 1, &room);
        if (e != PROVEN_OK) return e;
        proven_size_t k = n - *taken;
        if (k > room.size) k = room.size;
        if (k > limit - s->queued) k = limit - s->queued;
        for (proven_size_t i = 0; i < k; ++i) room.ptr[i] = p[*taken + i];
        sock_tail(s)->used += (proven_u32)k;
        s->queued += (proven_u32)k;
        *taken += k;
    }
    return PROVEN_OK;
}

proven_result_size_t proven_loop_sendv_(proven_loop_t *l, proven_loop_sock_t *sock, const proven_mem_view_t *parts, proven_size_t count) {
    proven_result_size_t res = { .err = PROVEN_OK, .value = 0 };
    if (!l || !sock || (count > 0 && !parts) || count > PROVEN_NET_VEC_MAX) { res.err = PROVEN_ERR_INVALID_ARG; return res; }
    proven_size_t total = 0;
    for (proven_size_t i = 0; i < count; ++i) {
        if (parts[i].size > 0 && !parts[i].ptr) { res.err = PROVEN_ERR_INVALID_ARG; return res; }
        total += parts[i].size;
    }
    loop_sock_t *s = sock_of(sock);
    if (s->node.kind != KIND_SOCK || s->node.state != S_OPEN) { res.err = PROVEN_ERR_INVALID_STATE; return res; }
    if (s->send_err != PROVEN_OK) { res.err = (proven_err_t)s->send_err; return res; }
    proven_size_t gone = 0;
    if (s->queued == 0 && total > 0) {
        proven_net_conn_t conn = sock_conn(s);
        proven_err_t e;
        /* Small pieces are put together and sent with one plain send: cheaper than a gather
         * write's message header for the usual response of a few hundred bytes. */
        proven_byte_t small[2048];
        if (count > 1 && total <= sizeof small) {
            proven_size_t at = 0;
            for (proven_size_t i = 0; i < count; ++i) { for (proven_size_t k = 0; k < parts[i].size; ++k) small[at + k] = parts[i].ptr[k]; at += parts[i].size; }
            proven_result_size_t w = proven_net_write(&conn, (proven_mem_view_t){ .ptr = small, .size = total }, PROVEN_NET_DONT_WAIT);
            e = w.err;
            gone = w.value;
        } else if (count == 1) {
            proven_result_size_t w = proven_net_write(&conn, parts[0], PROVEN_NET_DONT_WAIT);
            e = w.err;
            gone = w.value;
        } else {
            e = proven_net_write_vec_(&conn, parts, count, &gone);
        }
        if (e != PROVEN_OK && e != PROVEN_ERR_TIMEOUT) {
            s->send_err = (proven_u16)e;
            sock_drop_chunks(l, s);
            res.value = gone;
            res.err = e;
            return res;
        }
    }
    res.value = gone;
    /* The rest, piece by piece from where the socket stopped. */
    proven_size_t at = 0;
    for (proven_size_t i = 0; i < count && res.err == PROVEN_OK; ++i) {
        const proven_size_t size = parts[i].size;
        if (at + size <= gone) { at += size; continue; }
        const proven_size_t skip = gone > at ? gone - at : 0;
        proven_size_t taken = 0;
        proven_err_t e = sock_hold(l, s, parts[i].ptr + skip, size - skip, &taken);
        res.value += taken;
        at += size;
        if (e != PROVEN_OK) res.err = e;
    }
    /* What waits was offered to the socket just now (or waits behind bytes that were): the
     * next try is when it is writable. */
    if (s->queued > 0) {
        proven_err_t e = node_watch(l, &s->node, s->handle, (proven_u8)(s->node.want | PROVEN_NET_WRITABLE));
        if (e != PROVEN_OK && res.err == PROVEN_OK) res.err = e;
    }
    return res;
}

proven_result_size_t proven_loop_send(proven_loop_t *l, proven_loop_sock_t *sock, proven_mem_view_t bytes) {
    if (bytes.size > 0 && !bytes.ptr) return (proven_result_size_t){ .err = PROVEN_ERR_INVALID_ARG, .value = 0 };
    return proven_loop_sendv_(l, sock, &bytes, 1);
}

proven_size_t proven_loop_send_queued(const proven_loop_sock_t *sock) {
    if (!sock) return 0;
    const loop_sock_t *s = (const loop_sock_t *)(const void *)sock;
    return s->node.kind == KIND_SOCK ? s->queued : 0;
}

proven_err_t proven_loop_send_wait(proven_loop_t *l, proven_loop_sock_t *sock, proven_size_t at_most) {
    if (!l || !sock) return PROVEN_ERR_INVALID_ARG;
    loop_sock_t *s = sock_of(sock);
    if (s->node.kind != KIND_SOCK || s->node.state != S_OPEN) return PROVEN_ERR_INVALID_STATE;
    if (!s->fns->writable) return PROVEN_ERR_INVALID_ARG;
    if (s->queued <= at_most) {
        s->node.flags &= (proven_u8)~F_WAIT;
        node_queue(l, &s->node, D_WRITABLE);
        return PROVEN_OK;
    }
    s->wait_at = at_most > 0xffffffffu ? 0xffffffffu : (proven_u32)at_most;
    s->node.flags |= F_WAIT;
    return PROVEN_OK;
}

void proven_loop_send_limit_(proven_loop_sock_t *sock, proven_size_t bytes) {
    if (!sock) return;
    loop_sock_t *s = sock_of(sock);
    if (s->node.kind != KIND_SOCK) return;
    const proven_size_t k = (bytes + 1023u) / 1024u;            /* bytes of (proven_size_t)-1 wrap to 0: no bound either */
    s->limit_k = bytes == (proven_size_t)-1 || k >= LIMIT_NONE ? (proven_u16)LIMIT_NONE : (proven_u16)k;
}

proven_err_t proven_loop_reserve_(proven_loop_t *l, proven_loop_sock_t *sock, proven_size_t min, proven_mem_mut_t *room) {
    if (!l || !sock || !room || min > PROVEN_LOOP_CHUNK_) return PROVEN_ERR_INVALID_ARG;
    loop_sock_t *s = sock_of(sock);
    if (s->node.kind != KIND_SOCK || s->node.state != S_OPEN) return PROVEN_ERR_INVALID_STATE;
    if (s->send_err != PROVEN_OK) return (proven_err_t)s->send_err;
    if (s->queued >= sock_limit(s)) return PROVEN_ERR_AGAIN;
    return sock_room(l, s, min, room);
}

void proven_loop_commit_(proven_loop_t *l, proven_loop_sock_t *sock, proven_size_t n) {
    if (!l || !sock || n == 0) return;
    loop_sock_t *s = sock_of(sock);
    loop_chunk_t *tail = sock_tail(s);
    if (s->node.kind != KIND_SOCK || s->node.state != S_OPEN || !tail || n > PROVEN_LOOP_CHUNK_ - tail->used) return;
    tail->used += (proven_u32)n;
    s->queued += (proven_u32)n;
    node_queue(l, &s->node, D_FLUSH);
}

void proven_loop_close(proven_loop_t *l, proven_loop_sock_t *sock) {
    if (!l || !sock) return;
    loop_sock_t *s = sock_of(sock);
    if (s->node.kind != KIND_SOCK) return;
    proven_u8 state = s->node.state;
    if (state != S_OPEN && state != S_CONNECTING && state != S_DEAD) return;
    proven_u8 bits = D_CLOSED;
    if (s->node.flags & F_RECV) {
        s->node.flags &= (proven_u8)~F_RECV;
        s->node.err = (proven_u16)PROVEN_ERR_CANCELLED;
        bits |= D_RECEIVED;
    }
    if (state == S_CONNECTING) { s->node.err = (proven_u16)PROVEN_ERR_CANCELLED; bits |= D_CONNECTED; }
    if (s->node.flags & F_WAIT) { s->node.flags &= (proven_u8)~F_WAIT; bits |= D_WRITABLE; }
    sock_drop_chunks(l, s);
    node_unwatch(l, &s->node, s->handle);
    s->node.state = S_CLOSING;
    node_queue(l, &s->node, bits);
}

proven_err_t proven_loop_accept(proven_loop_t *l, proven_loop_accept_t *acc, proven_net_listener_t *listener,
                                proven_loop_accept_fn fn, void *ctx) {
    if (!l || !acc || !listener || !fn || !proven_net_listener_is_open(listener)) return PROVEN_ERR_INVALID_ARG;
    loop_acc_t *a = acc_of(acc);
    if (a->node.kind != KIND_ACCEPT || a->listener != listener) {
        if (a->node.kind == KIND_ACCEPT && (a->node.flags & F_RECV)) return PROVEN_ERR_BUSY;
        if (a->node.kind == KIND_ACCEPT) node_unwatch(l, &a->node, a->handle);
        for (proven_size_t i = 0; i < sizeof *a; ++i) ((proven_byte_t *)a)[i] = 0;
        a->node.kind = KIND_ACCEPT;
        a->listener = listener;
        a->handle = listener->internal.handle;
    }
    if (a->node.flags & F_RECV) return PROVEN_ERR_BUSY;
    a->fn = fn;
    a->ctx = ctx;
    /* Armed again from inside its own function: there is often another connection waiting
     * already, and taking it now saves a round. It is still answered from the loop. */
    if (l->delivering == a && acc_try(a)) {
        a->node.flags |= F_RECV;
        node_queue(l, &a->node, D_ACCEPT);
        return PROVEN_OK;
    }
    proven_err_t e = node_watch(l, &a->node, a->handle, PROVEN_NET_READABLE);
    if (e != PROVEN_OK) return e;
    a->node.flags |= F_RECV;
    return PROVEN_OK;
}

void proven_loop_accept_cancel(proven_loop_t *l, proven_loop_accept_t *acc) {
    if (!l || !acc) return;
    loop_acc_t *a = acc_of(acc);
    if (a->node.kind != KIND_ACCEPT) return;
    node_unwatch(l, &a->node, a->handle);
    if (!(a->node.flags & F_RECV)) return;
    (void)proven_net_close(&a->conn);             /* one taken and not yet delivered */
    a->node.err = (proven_u16)PROVEN_ERR_CANCELLED;
    node_queue(l, &a->node, D_ACCEPT);
}

// -----------------------------------------------------------------------------
// Timers
// -----------------------------------------------------------------------------

/* A timer's `tick` holds its due time in nanoseconds on the loop's clock (RFC-0019 T-1); its
 * slot is the tick of the wheel that time falls in. */
static proven_size_t timer_slot(const proven_loop_timer_t *t) { return (proven_size_t)((t->tick / LOOP_TICK_NS) % LOOP_WHEEL_SLOTS); }

static void timer_unlink(proven_loop_t *l, proven_loop_timer_t *t) {
    proven_loop_timer_t **head = t->state == TIMER_DUE ? &l->due : &l->wheel[timer_slot(t)];
    if (t->prev) t->prev->next = t->next;
    else *head = t->next;
    if (t->next) t->next->prev = t->prev;
    t->prev = (void *)0; t->next = (void *)0;
    if (t->state == TIMER_WHEEL) l->timed_count--;
    t->state = TIMER_IDLE;
}

void proven_loop_timer_cancel(proven_loop_t *l, proven_loop_timer_t *t) {
    if (!l || !t || t->state == TIMER_IDLE) return;
    timer_unlink(l, t);
}

bool proven_loop_timer_is_set(const proven_loop_timer_t *t) { return t && t->state != TIMER_IDLE; }

void proven_loop_timer_set(proven_loop_t *l, proven_loop_timer_t *t, proven_u32 ms, proven_loop_fn fn, void *ctx) {
    if (!l || !t || !fn) return;
    proven_loop_timer_cancel(l, t);
    /* Due at the time asked for (RFC-0019 T-1: no longer rounded up to the next tick); never in
     * a tick the wheel has already passed. */
    proven_u64 due = (proven_u64)loop_now(l) + (proven_u64)ms * 1000000u;
    if (due / LOOP_TICK_NS < l->wheel_tick) due = l->wheel_tick * (proven_u64)LOOP_TICK_NS;
    t->fn = fn; t->ctx = ctx; t->tick = due;
    proven_size_t slot = timer_slot(t);
    t->prev = (void *)0;
    t->next = l->wheel[slot];
    if (t->next) t->next->prev = t;
    l->wheel[slot] = t;
    t->state = TIMER_WHEEL;
    l->timed_count++;
}

/* When the next timer is due: from the slot the wheel stands in, the first slot holding a timer
 * of its own turn gives the earliest due time among those. The scan covers one whole turn and
 * the current slot again at its end (its timers of the next turn, exact). When every timer belongs
 * to a later turn - or a crowded slot has shown no timer of its own turn among the first
 * LOOP_LOOK timers looked at - the boundary of a slot after the current one: an early look, never
 * a time already past (which would spin), never late. */
#define LOOP_LOOK 32u
static proven_net_deadline_t loop_next_timer(const proven_loop_t *l) {
    if (l->timed_count == 0) return PROVEN_NET_NO_DEADLINE;
    proven_u64 first_any = 0;
    bool any = false;
    unsigned looked = 0;
    for (proven_u64 s = l->wheel_tick; s <= l->wheel_tick + LOOP_WHEEL_SLOTS; ++s) {
        const proven_loop_timer_t *t = l->wheel[s % LOOP_WHEEL_SLOTS];
        if (!t) continue;
        if (!any && s > l->wheel_tick) { any = true; first_any = s; }
        proven_u64 best = 0;
        bool found = false;
        for (; t; t = t->next) {
            if (t->tick / LOOP_TICK_NS == s && (!found || t->tick < best)) { best = t->tick; found = true; }
            if (!found && ++looked >= LOOP_LOOK) return (proven_net_deadline_t)((s > l->wheel_tick ? s : l->wheel_tick + 1) * LOOP_TICK_NS);
        }
        if (found) return (proven_net_deadline_t)best;
    }
    return any ? (proven_net_deadline_t)(first_any * LOOP_TICK_NS) : PROVEN_NET_NO_DEADLINE;
}

static void loop_timers_advance(proven_loop_t *l) {
    const proven_u64 now = (proven_u64)loop_now(l);
    proven_u64 target = now / LOOP_TICK_NS;
    if (target < l->wheel_tick) target = l->wheel_tick;
    if (l->timed_count > 0) {
        /* From the slot the wheel stands in (its timers may have come due within the tick) to
         * the one `now` falls in; after a long sleep every slot is visited once. */
        proven_u64 from = target - l->wheel_tick >= LOOP_WHEEL_SLOTS ? target - LOOP_WHEEL_SLOTS + 1 : l->wheel_tick;
        /* Slots are visited in the order of their times, and each timer that is due goes into
         * the due list by its due time, so that timers which come due together fire earliest
         * first - within a slot too. Walked back from the tail: they mostly come in order. */
        proven_loop_timer_t *tail = l->due;
        while (tail && tail->next) tail = tail->next;
        for (proven_u64 s = from; s <= target && l->timed_count > 0; ++s) {
            proven_loop_timer_t *t = l->wheel[s % LOOP_WHEEL_SLOTS];
            while (t) {
                proven_loop_timer_t *next = t->next;
                if (t->tick <= now) {
                    timer_unlink(l, t);
                    proven_loop_timer_t *after = tail;
                    while (after && after->tick > t->tick) after = after->prev;
                    t->prev = after;
                    t->next = after ? after->next : l->due;
                    if (t->next) t->next->prev = t;
                    else tail = t;
                    if (after) after->next = t;
                    else l->due = t;
                    t->state = TIMER_DUE;
                }
                t = next;
            }
        }
    }
    l->wheel_tick = target;
    /* Fire them one at a time from the list's head: a callback may cancel any of the others. */
    while (l->due) {
        proven_loop_timer_t *t = l->due;
        proven_loop_fn fn = t->fn;
        void *ctx = t->ctx;
        timer_unlink(l, t);
        fn(ctx);
    }
}

// -----------------------------------------------------------------------------
// Posting, running
// -----------------------------------------------------------------------------

proven_err_t proven_loop_post(proven_loop_t *l, proven_loop_fn fn, void *ctx) {
    if (!l || !fn) return PROVEN_ERR_INVALID_ARG;
    proven_result_mem_mut_t m = l->alloc.alloc_fn(l->alloc.ctx, sizeof(proven_loop_task_t), 16);
    if (m.err != PROVEN_OK) return m.err;
    proven_loop_task_t *t = (proven_loop_task_t *)m.value.ptr;
    t->fn = fn; t->ctx = ctx;
    t->next = atomic_load_explicit(&l->tasks, memory_order_relaxed);
    while (!atomic_compare_exchange_weak_explicit(&l->tasks, &t->next, t, memory_order_release, memory_order_relaxed)) { }
    proven_net_waker_wake(&l->waker);
    return PROVEN_OK;
}

static bool loop_run_tasks(proven_loop_t *l) {
    proven_loop_task_t *t = atomic_exchange_explicit(&l->tasks, (proven_loop_task_t *)0, memory_order_acquire);
    if (!t) return false;
    /* The stack holds the newest first; reverse it to call in the order of posting. */
    proven_loop_task_t *ordered = (void *)0;
    while (t) { proven_loop_task_t *next = t->next; t->next = ordered; ordered = t; t = next; }
    while (ordered) {
        proven_loop_task_t *next = ordered->next;
        proven_loop_fn fn = ordered->fn;
        void *ctx = ordered->ctx;
        l->alloc.free_fn(l->alloc.ctx, ordered);
        fn(ctx);
        ordered = next;
    }
    return true;
}

void proven_loop_stop(proven_loop_t *l) {
    if (!l) return;
    atomic_store_explicit(&l->stop, true, memory_order_release);
    proven_net_waker_wake(&l->waker);
}

void proven_loop_after_(proven_loop_t *l, proven_loop_after_t *a, proven_loop_fn fn, void *ctx) {
    if (!l || !a || !fn || a->waiting) return;
    a->fn = fn; a->ctx = ctx; a->waiting = true;
    a->next = (void *)0; a->prev = l->after_last;
    if (l->after_last) l->after_last->next = a; else l->after_first = a;
    l->after_last = a;
    l->after_count++;
}

void proven_loop_after_cancel_(proven_loop_t *l, proven_loop_after_t *a) {
    if (!l || !a || !a->waiting) return;
    if (a->prev) a->prev->next = a->next; else l->after_first = a->next;
    if (a->next) a->next->prev = a->prev; else l->after_last = a->prev;
    a->waiting = false;
    a->next = a->prev = (void *)0;
    l->after_count--;
}

/* As many as are waiting now, in order; one asked for meanwhile waits for the next call. Each
 * is taken off the list before it runs, so it may ask again or be cancelled by another. */
static void loop_run_after(proven_loop_t *l) {
    for (proven_size_t n = l->after_count; n > 0 && l->after_first; --n) {
        proven_loop_after_t *a = l->after_first;
        proven_loop_after_cancel_(l, a);
        a->fn(a->ctx);
    }
}

proven_err_t proven_loop_poll(proven_loop_t *l, proven_net_deadline_t until) {
    if (!l) return PROVEN_ERR_INVALID_ARG;
    /* What was asked for between rounds runs before the loop sleeps; what that asks for in
     * its turn must not wait behind the sleep either. */
    loop_run_after(l);
    loop_run_done(l);
    if (l->after_first || l->done_first) until = PROVEN_NET_DONT_WAIT;
    /* Work posted before this round must not wait behind a sleep. */
    if (loop_run_tasks(l)) until = PROVEN_NET_DONT_WAIT;
    proven_net_deadline_t next = loop_next_timer(l);
    if (next != PROVEN_NET_NO_DEADLINE && (until == PROVEN_NET_NO_DEADLINE || (until != PROVEN_NET_DONT_WAIT && next < until))) until = next;
    proven_size_t count = 0;
    proven_err_t e = proven_net_selector_wait(l->selector, l->batch, LOOP_EVENTS, until, &count);
    if (e != PROVEN_OK && e != PROVEN_ERR_TIMEOUT) return e;
    l->batch_count = count;
    for (l->batch_pos = 0; l->batch_pos < l->batch_count; ) {
        proven_net_ready_t ev = l->batch[l->batch_pos++];
        if (!ev.tag) continue;
        switch (*(const proven_u8 *)ev.tag) {
        case KIND_IO: {
            proven_loop_io_t *io = (proven_loop_io_t *)ev.tag;
            if (io->registered) io->fn(io->ctx, ev.got);
            break;
        }
        case KIND_SOCK: sock_event(l, (loop_sock_t *)ev.tag, ev.got); break;
        case KIND_ACCEPT: acc_event(l, (loop_acc_t *)ev.tag, ev.got); break;
        default: break;
        }
    }
    l->batch_count = 0; l->batch_pos = 0;
    loop_run_done(l);
    loop_timers_advance(l);
    loop_run_done(l);
    (void)loop_run_tasks(l);
    loop_run_done(l);
    loop_run_after(l);
    loop_run_after(l);
    loop_run_done(l);
    return PROVEN_OK;
}

proven_err_t proven_loop_run(proven_loop_t *l) {
    if (!l) return PROVEN_ERR_INVALID_ARG;
    while (!atomic_load_explicit(&l->stop, memory_order_acquire)) {
        proven_err_t e = proven_loop_poll(l, PROVEN_NET_NO_DEADLINE);
        if (e != PROVEN_OK) return e;
    }
    atomic_store_explicit(&l->stop, false, memory_order_release);     /* a loop can be run again */
    return PROVEN_OK;
}

#endif
