#ifndef PROVEN_INTERNAL_LOOP_H
#define PROVEN_INTERNAL_LOOP_H

#include "proven/loop.h"
#include "proven/time.h"

/*
 * Internal: the loop's clock, for tests. Not public API.
 *
 * Timers are measured on the monotonic clock. A test of a timer that is a minute away cannot
 * wait a minute, so it gives the loop a clock of its own and moves it. Every reading the loop
 * makes - where the wheel stands, when a timer is due - goes through this one function.
 *
 * Call it on a loop with no timer set: the wheel is put where the new clock says "now" is.
 * With such a clock, proven_loop_poll must be given PROVEN_NET_DONT_WAIT: how long to sleep
 * is still decided against the real clock.
 */
typedef proven_time_t (*proven_loop_clock_fn)(void *ctx);
void proven_loop_test_clock_(proven_loop_t *loop, proven_loop_clock_fn clock, void *ctx);

/*
 * Internal: "after this round". Not public API.
 *
 * Something on the loop's thread that wants to be called back soon, from the loop and not
 * from where it stands - a connection whose caller has just given it something to send - has
 * had two ways: a post, which wakes the loop with a system call, and asking to be told the
 * socket is writable, which is two changes of the socket's watch and a turn of the loop. This
 * is the third, and costs none of that: `fn` runs when the round being delivered has been
 * delivered, or, asked for between rounds, at the start of the next before the loop sleeps.
 *
 * Loop thread only. Asking again while one is waiting changes nothing (it runs once, with
 * the first `fn` and `ctx`). They run in the order asked. One asked for from inside another
 * runs in the same round, twice over at most; after that at the next round, which does not
 * sleep first. Cancel before the memory goes.
 */
typedef struct proven_loop_after {
    struct proven_loop_after *next, *prev;
    proven_loop_fn fn;
    void *ctx;
    bool waiting;
} proven_loop_after_t;
void proven_loop_after_(proven_loop_t *loop, proven_loop_after_t *after, proven_loop_fn fn, void *ctx);
void proven_loop_after_cancel_(proven_loop_t *loop, proven_loop_after_t *after);

/*
 * Internal: the readiness calls the drivers used before RFC-0025, kept under their old names
 * only until each driver has moved to the operations of loop.h (RFC-0025 section 12, steps 2 to
 * 5, all in one release). Not public API.
 *
 * `fn(ctx, got)` is called whenever the socket is ready for what `want` asks (level-triggered).
 * proven_loop_io_remove before closing the socket; it is safe inside any callback, and nothing
 * more is delivered for it, not even an event already collected in this round.
 */
typedef void (*proven_loop_io_fn)(void *ctx, proven_u8 got);
typedef struct proven_loop_io {
    proven_u8 kind_;            /* 0: what the loop's selector tags tell apart; leave it 0 */
    proven_loop_io_fn fn;
    void *ctx;
    proven_net_handle_t handle;
    proven_u8 want;
    bool registered;
} proven_loop_io_t;
proven_err_t proven_loop_io_add(proven_loop_t *loop, proven_loop_io_t *io, proven_net_handle_t handle,
                                proven_u8 want, proven_loop_io_fn fn, void *ctx);
proven_err_t proven_loop_io_set(proven_loop_t *loop, proven_loop_io_t *io, proven_u8 want);
void proven_loop_io_remove(proven_loop_t *loop, proven_loop_io_t *io);
proven_size_t proven_loop_io_count(const proven_loop_t *loop);

/* Internal, for tests: a loop over the poll kind of selector, so that Linux runs the code the
 * Windows build runs over WSAPoll. */
proven_err_t proven_loop_create_poll_(proven_allocator_t alloc, proven_loop_t **out);

/*
 * Internal: writing in place into the loop's send buffers (RFC-0025 section 12, D-2). For the
 * drivers, which format a response head or seal a TLS record straight into the buffer that is
 * sent. reserve: room for at least `min` bytes (at most PROVEN_LOOP_CHUNK_) at the end of what is
 * waiting, perhaps more; PROVEN_ERR_AGAIN past the connection's limit, and the errors of
 * proven_loop_send. commit: `n` bytes of that room were written; they go after what was waiting,
 * from the loop, in one gather write with the rest. A reservation is committed before anything
 * else is sent on the socket: proven_loop_send with nothing committed writes straight to the
 * socket and would put its bytes ahead of the room reserved.
 */
#define PROVEN_LOOP_CHUNK_ ((proven_size_t)16416)
/* How much the loop holds for the socket before proven_loop_send takes no more (64 KiB unless
 * set; at most 0xffffffff). For a driver that bounds what it queues by its own rule. */
void proven_loop_send_limit_(proven_loop_sock_t *sock, proven_size_t bytes);
/* proven_loop_send of several pieces in order (at most PROVEN_NET_VEC_MAX): with nothing waiting
 * they go to the socket in one gather write, and only what it does not take is copied - a
 * response head and its body, a frame's header and payload, leave together without a copy. */
proven_result_size_t proven_loop_sendv_(proven_loop_t *loop, proven_loop_sock_t *sock, const proven_mem_view_t *parts, proven_size_t count);
proven_err_t proven_loop_reserve_(proven_loop_t *loop, proven_loop_sock_t *sock, proven_size_t min, proven_mem_mut_t *room);
void proven_loop_commit_(proven_loop_t *loop, proven_loop_sock_t *sock, proven_size_t n);

#endif /* PROVEN_INTERNAL_LOOP_H */
