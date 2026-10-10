#include "proven/loop.h"

#if !defined(PROVEN_FREESTANDING) && !defined(PROVEN_NO_NET)

#include "proven/time.h"
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
 */

#define LOOP_WHEEL_SLOTS 1024u
#define LOOP_TICK_NS 16000000
#define LOOP_EVENTS 64u
#define LOOP_SCRATCH ((proven_size_t)64 * 1024)

#define TIMER_IDLE 0
#define TIMER_WHEEL 1
#define TIMER_DUE 2

typedef struct loop_task {
    struct loop_task *next;
    proven_loop_fn fn;
    void *ctx;
} loop_task_t;

struct proven_loop {
    proven_allocator_t alloc;
    proven_net_selector_t *selector;
    proven_net_waker_t waker;
    bool waker_open;
    proven_loop_io_t waker_io;
    _Atomic(loop_task_t *) tasks;
    atomic_bool stop;
    proven_loop_timer_t *wheel[LOOP_WHEEL_SLOTS];
    proven_loop_timer_t *due;                     /* timers taken off the wheel, not yet fired */
    proven_u64 wheel_tick;
    proven_size_t timed_count;
    proven_net_ready_t batch[LOOP_EVENTS];        /* the events of the round being delivered */
    proven_size_t batch_count, batch_pos;
    proven_size_t io_count;
    proven_byte_t *scratch;
};

static void loop_waker_ready(void *ctx, proven_u8 got) {
    (void)got;
    proven_net_waker_drain(&((proven_loop_t *)ctx)->waker);
}

proven_err_t proven_loop_create(proven_allocator_t alloc, proven_loop_t **out) {
    if (out) *out = (void *)0;
    if (!out || !proven_alloc_is_valid(alloc)) return PROVEN_ERR_INVALID_ARG;
    proven_result_mem_mut_t m = alloc.alloc_fn(alloc.ctx, sizeof(proven_loop_t), 16);
    if (m.err != PROVEN_OK) return m.err;
    proven_loop_t *l = (proven_loop_t *)m.value.ptr;
    for (proven_size_t i = 0; i < sizeof *l; ++i) ((proven_byte_t *)l)[i] = 0;
    l->alloc = alloc;
    atomic_init(&l->tasks, (loop_task_t *)0);
    atomic_init(&l->stop, false);
    l->wheel_tick = (proven_u64)proven_time_monotonic_now() / LOOP_TICK_NS;
    proven_err_t e = proven_net_selector_create(alloc, &l->selector);
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

void proven_loop_destroy(proven_loop_t *l) {
    if (!l) return;
    proven_allocator_t a = l->alloc;
    loop_task_t *t = atomic_exchange_explicit(&l->tasks, (loop_task_t *)0, memory_order_acquire);
    while (t) { loop_task_t *next = t->next; a.free_fn(a.ctx, t); t = next; }
    if (l->waker_open) {
        if (l->waker_io.registered) (void)proven_net_selector_remove(l->selector, l->waker_io.handle);
        proven_net_waker_close(&l->waker);
    }
    if (l->selector) proven_net_selector_destroy(l->selector);
    if (l->scratch) a.free_fn(a.ctx, l->scratch);
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
// Timers
// -----------------------------------------------------------------------------

static void timer_unlink(proven_loop_t *l, proven_loop_timer_t *t) {
    proven_loop_timer_t **head = t->state == TIMER_DUE ? &l->due : &l->wheel[t->tick % LOOP_WHEEL_SLOTS];
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
    /* Rounded up to a tick, and never into a slot the wheel has already passed. */
    proven_u64 tick = (proven_u64)proven_net_deadline_in(ms) / LOOP_TICK_NS + 1;
    if (tick <= l->wheel_tick) tick = l->wheel_tick + 1;
    t->fn = fn; t->ctx = ctx; t->tick = tick;
    proven_size_t slot = (proven_size_t)(tick % LOOP_WHEEL_SLOTS);
    t->prev = (void *)0;
    t->next = l->wheel[slot];
    if (t->next) t->next->prev = t;
    l->wheel[slot] = t;
    t->state = TIMER_WHEEL;
    l->timed_count++;
}

/* When the next timer may be due: the time of the first slot that holds anything. A slot can
 * hold timers of a later turn, so this may be early - which costs one look - but never late. */
static proven_net_deadline_t loop_next_timer(const proven_loop_t *l) {
    if (l->timed_count == 0) return PROVEN_NET_NO_DEADLINE;
    for (proven_u64 t = l->wheel_tick + 1; t <= l->wheel_tick + LOOP_WHEEL_SLOTS; ++t) {
        if (l->wheel[t % LOOP_WHEEL_SLOTS]) return (proven_net_deadline_t)(t * LOOP_TICK_NS);
    }
    return PROVEN_NET_NO_DEADLINE;
}

static void loop_timers_advance(proven_loop_t *l) {
    proven_u64 target = (proven_u64)proven_time_monotonic_now() / LOOP_TICK_NS;
    if (target <= l->wheel_tick) return;
    if (l->timed_count > 0) {
        /* After a long sleep every slot is visited once; more turns would find nothing new. */
        proven_u64 from = target - l->wheel_tick > LOOP_WHEEL_SLOTS ? target - LOOP_WHEEL_SLOTS + 1 : l->wheel_tick + 1;
        for (proven_u64 s = from; s <= target && l->timed_count > 0; ++s) {
            proven_loop_timer_t *t = l->wheel[s % LOOP_WHEEL_SLOTS];
            while (t) {
                proven_loop_timer_t *next = t->next;
                if (t->tick <= target) {
                    timer_unlink(l, t);
                    t->next = l->due;
                    if (l->due) l->due->prev = t;
                    l->due = t;
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
    proven_result_mem_mut_t m = l->alloc.alloc_fn(l->alloc.ctx, sizeof(loop_task_t), 16);
    if (m.err != PROVEN_OK) return m.err;
    loop_task_t *t = (loop_task_t *)m.value.ptr;
    t->fn = fn; t->ctx = ctx;
    t->next = atomic_load_explicit(&l->tasks, memory_order_relaxed);
    while (!atomic_compare_exchange_weak_explicit(&l->tasks, &t->next, t, memory_order_release, memory_order_relaxed)) { }
    proven_net_waker_wake(&l->waker);
    return PROVEN_OK;
}

static bool loop_run_tasks(proven_loop_t *l) {
    loop_task_t *t = atomic_exchange_explicit(&l->tasks, (loop_task_t *)0, memory_order_acquire);
    if (!t) return false;
    /* The stack holds the newest first; reverse it to call in the order of posting. */
    loop_task_t *ordered = (void *)0;
    while (t) { loop_task_t *next = t->next; t->next = ordered; ordered = t; t = next; }
    while (ordered) {
        loop_task_t *next = ordered->next;
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

proven_err_t proven_loop_poll(proven_loop_t *l, proven_net_deadline_t until) {
    if (!l) return PROVEN_ERR_INVALID_ARG;
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
        proven_loop_io_t *io = (proven_loop_io_t *)ev.tag;
        if (io && io->registered) io->fn(io->ctx, ev.got);
    }
    l->batch_count = 0; l->batch_pos = 0;
    loop_timers_advance(l);
    (void)loop_run_tasks(l);
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
