#ifndef PROVEN_LOOP_H
#define PROVEN_LOOP_H

#include "proven/types.h"
#include "proven/error.h"
#include "proven/memory.h"
#include "proven/allocator.h"
#include "proven/net.h"

/**
 * @file loop.h
 * @brief An event loop: one thread that waits for many things and is told which happened.
 *
 * The blocking calls of net.h give each connection a thread, or make every connection wait
 * its turn. That is the right tool for a hundred connections and the wrong one for tens of
 * thousands, most of which are doing nothing at any moment. A loop turns it around: the thread
 * asks the kernel once which sockets have something, handles those without ever waiting on
 * any one of them, and asks again.
 *
 * A `proven_loop_t` is four things in one:
 *
 *   - a **selector** (net.h): sockets registered once, with a function called when one is
 *     readable or writable;
 *   - **timers** that cost nothing while they wait - arming, cancelling and firing are
 *     constant-time, and an idle timer is never looked at;
 *   - a **waker** and a queue of **posted functions**, so that another thread can hand work
 *     to the loop's thread;
 *   - a **scratch buffer** every callback may use and none may keep.
 *
 * The rule that makes it work: **a callback must not wait.** Not on a socket, not on a file,
 * not on a lock that someone may hold for long. Whatever takes time goes to another thread
 * (job.h) and comes back through proven_loop_post.
 *
 * Everything but proven_loop_stop and proven_loop_post must be called from the loop's own
 * thread - the one inside proven_loop_run - or before it starts.
 *
 * Hosted only; `PROVEN_NO_NET` leaves it out.
 */

/** @brief A loop. Opaque. */
typedef struct proven_loop proven_loop_t;

/** @brief A function the loop calls on its thread. */
typedef void (*proven_loop_fn)(void *ctx);

/** @brief Called when a watched socket is ready; `got` is PROVEN_NET_READABLE,
 *         PROVEN_NET_WRITABLE and/or PROVEN_NET_FAILED. */
typedef void (*proven_loop_io_fn)(void *ctx, proven_u8 got);

/**
 * @brief One socket's registration. Yours to hold - inside your connection struct, say - for
 *        as long as the socket is watched. Zero-initialise it; do not copy or move it while
 *        it is registered.
 */
typedef struct proven_loop_io {
    proven_loop_io_fn fn;
    void *ctx;
    proven_net_handle_t handle;
    proven_u8 want;
    bool registered;
} proven_loop_io_t;

/**
 * @brief One timer. Yours to hold; zero-initialise it; do not copy or move it while it is set.
 *        A timer fires once.
 */
typedef struct proven_loop_timer {
    struct proven_loop_timer *prev, *next;
    proven_loop_fn fn;
    void *ctx;
    proven_u64 tick;
    proven_u8 state;
} proven_loop_timer_t;

/** @return PROVEN_ERR_INVALID_ARG, PROVEN_ERR_NOMEM, or what opening the selector returned. */
[[nodiscard]]
proven_err_t proven_loop_create(proven_allocator_t alloc, proven_loop_t **out);

/** @brief Free the loop. Whatever is still registered or set is simply forgotten: nothing is
 *         called. Null is accepted. */
void proven_loop_destroy(proven_loop_t *loop);

/**
 * @brief Run until proven_loop_stop: wait, call what is ready, fire what is due, repeat.
 * @return PROVEN_OK after a stop; an error if waiting itself failed.
 */
[[nodiscard]]
proven_err_t proven_loop_run(proven_loop_t *loop);

/**
 * @brief One round: wait until something is ready or `until` passes, and handle it.
 *        PROVEN_NET_DONT_WAIT handles only what is ready now. For a program that owns its
 *        own main loop.
 */
[[nodiscard]]
proven_err_t proven_loop_poll(proven_loop_t *loop, proven_net_deadline_t until);

/** @brief Make proven_loop_run return. Safe from any thread and from a callback. */
void proven_loop_stop(proven_loop_t *loop);

/**
 * @brief Have the loop call `fn(ctx)` on its own thread, soon. **Safe from any thread**: this
 *        is how work done elsewhere comes back. Functions are called in the order posted.
 *
 * The loop's allocator is used for the queue entry, so when other threads post it must be one
 * that is safe to call from several threads (the heap allocator is).
 *
 * @return PROVEN_ERR_INVALID_ARG, PROVEN_ERR_NOMEM.
 */
[[nodiscard]]
proven_err_t proven_loop_post(proven_loop_t *loop, proven_loop_fn fn, void *ctx);

/**
 * @brief Watch a socket. `fn(ctx, got)` is called whenever it is ready for what `want` asks
 *        (PROVEN_NET_READABLE and/or PROVEN_NET_WRITABLE; 0 watches for nothing yet).
 *
 * Readiness is level-triggered: as long as there is something to read, the function is called
 * each round. Read until the call says it would wait (PROVEN_NET_DONT_WAIT), or stop asking.
 *
 * @return PROVEN_ERR_INVALID_ARG, PROVEN_ERR_INVALID_STATE if `io` is already registered, or
 *         what the selector returned.
 */
[[nodiscard]]
proven_err_t proven_loop_io_add(proven_loop_t *loop, proven_loop_io_t *io, proven_net_handle_t handle,
                                proven_u8 want, proven_loop_io_fn fn, void *ctx);

/** @brief Change what a registered socket is watched for. */
[[nodiscard]]
proven_err_t proven_loop_io_set(proven_loop_t *loop, proven_loop_io_t *io, proven_u8 want);

/**
 * @brief Stop watching. **Do this before closing the socket.** Safe inside any callback,
 *        the socket's own included: nothing more is delivered for it, not even an event
 *        already collected in this round. Harmless when `io` is not registered.
 */
void proven_loop_io_remove(proven_loop_t *loop, proven_loop_io_t *io);

/**
 * @brief Call `fn(ctx)` once, `ms` milliseconds from now (rounded up to the loop's tick of
 *        16 ms). Setting a timer that is already set moves it.
 */
void proven_loop_timer_set(proven_loop_t *loop, proven_loop_timer_t *timer, proven_u32 ms, proven_loop_fn fn, void *ctx);

/** @brief Forget a timer. Safe inside any callback, another timer's included; harmless when
 *         it is not set. */
void proven_loop_timer_cancel(proven_loop_t *loop, proven_loop_timer_t *timer);

/** @brief True while the timer is set and has not fired. */
bool proven_loop_timer_is_set(const proven_loop_timer_t *timer);

/**
 * @brief The loop's scratch buffer: 64 KiB that any callback may use while it runs.
 *
 * It is how thousands of connections share one read buffer: read into it, use what arrived,
 * and keep only what cannot be used yet. Its contents mean nothing after the callback returns
 * or calls anything that may itself use it.
 */
proven_mem_mut_t proven_loop_scratch(proven_loop_t *loop);

/** @brief The allocator the loop was made with. */
proven_allocator_t proven_loop_allocator(const proven_loop_t *loop);

/** @brief How many sockets are being watched. */
proven_size_t proven_loop_io_count(const proven_loop_t *loop);

#endif /* PROVEN_LOOP_H */
