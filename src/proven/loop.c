#include "proven/loop.h"
#include "proven_internal_loop.h"

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

proven_err_t proven_loop_create(proven_allocator_t alloc, proven_loop_t **out) {
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
    proven_loop_task_t *t = atomic_exchange_explicit(&l->tasks, (proven_loop_task_t *)0, memory_order_acquire);
    while (t) { proven_loop_task_t *next = t->next; a.free_fn(a.ctx, t); t = next; }
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
    if (l->after_first) until = PROVEN_NET_DONT_WAIT;
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
    loop_run_after(l);
    loop_run_after(l);
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
