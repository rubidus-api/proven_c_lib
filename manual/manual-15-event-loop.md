# Chapter 15: The event loop and the event-driven server

**Part V - Talking to the operating system. Prerequisites: [Chapter 9](manual-09-networking.md)
(sockets, deadlines, the selector); [Chapter 11](manual-11-http-client-server.md) (the HTTP
server this one is the other form of); [Chapter 14](manual-14-tls.md) if it is to speak TLS.**
**After this chapter** you can run a loop with timers and work that comes back from other
threads, serve HTTP from it without a call that waits, answer a request later, stream a response
to a client slower than your program, keep WebSocket connections open without a thread each,
make many HTTP requests at once from one thread, spread a server over several processor cores,
and say what a connection costs while it does nothing - and what was measured.

This chapter covers `loop.h`, `http_event.h`, `ws_event.h` and `http_event_client.h`. All are
hosted-only, and `PROVEN_NO_NET` leaves them out.

## Table of contents

1. [Two ways to serve](#1-two-ways-to-serve)
2. [A loop](#2-a-loop)
3. [Timers](#3-timers)
4. [Work that is done elsewhere](#4-work-that-is-done-elsewhere)
5. [A socket of your own](#5-a-socket-of-your-own)
6. [The event-driven server](#6-the-event-driven-server)
7. [Answering](#7-answering)
8. [Request bodies](#8-request-bodies)
9. [Limits, time and refusals](#9-limits-time-and-refusals)
10. [Threads, TLS and shutting down](#10-threads-tls-and-shutting-down)
11. [WebSocket on the loop](#11-websocket-on-the-loop)
12. [The event-driven client](#12-the-event-driven-client)
13. [Several loops](#13-several-loops)
14. [What a connection holds, what was measured, and what is not here](#14-what-a-connection-holds-what-was-measured-and-what-is-not-here)

## 1. Two ways to serve

The server of Chapter 11 hands your handler a request and lets it read and write with calls
that wait. Nothing is simpler to write: the handler is a straight line from the request to the
response. The price is that a request being handled occupies a thread for as long as it takes -
including all the time its client is slow.

The server of this chapter turns that around. **Nothing in it waits.** It sits on an event
loop, one thread that asks the system which sockets have something to say, deals with those,
and asks again. Your functions are told what happened - a request arrived, a piece of its body
arrived, there is room to write more - and return at once. A connection that is doing nothing
is then a small struct and nothing else: no thread, no stack, no buffer.

What you take on in exchange is real, and it is the whole of this chapter:

- **Your functions must not wait** - not on a socket, a file, or a lock someone may hold for
  long. Work that takes time goes to another thread and its result is posted back (section 4).
- **Writing can be refused.** When the client is not taking what was sent, the server stops
  accepting more from you and says when to continue (section 7).
- **What you are shown is short-lived.** The request head and the pieces of a body are views
  good only until your function returns. Copy what you keep.

Which to choose: the blocking server while the number of connections open at once is something
a pool of threads carries comfortably and the handlers are easier to write as straight lines;
this one when most connections are idle most of the time and there are many of them - long
polling, event streams, slow clients, a great many kept connections. Both parse HTTP with the
same code of [Chapter 10](manual-10-http.md) and refuse the same malformed requests.

## 2. A loop

```c
proven_err_t proven_loop_create(proven_allocator_t alloc, proven_loop_t **out);
proven_err_t proven_loop_create_ex(proven_allocator_t alloc, const proven_loop_options_t *options, proven_loop_t **out);
proven_loop_backend_t proven_loop_backend(const proven_loop_t *loop);
void proven_loop_destroy(proven_loop_t *loop);
proven_err_t proven_loop_run(proven_loop_t *loop);
proven_err_t proven_loop_poll(proven_loop_t *loop, proven_net_deadline_t until);
void proven_loop_stop(proven_loop_t *loop);
```

A `proven_loop_t` is a set of operations on sockets that complete (section 5), a set of timers,
a queue that other threads can post to, and a scratch buffer.

| Call | What it does | Returns |
|---|---|---|
| `proven_loop_create(alloc, &loop)` | Make a loop. | `proven_err_t`: `INVALID_ARG`; `NOMEM`; what opening the selector returned. |
| `proven_loop_create_ex(alloc, &options, &loop)` | Make a loop with the backend `options.backend` names (below). `options` may be null: then it is `proven_loop_create`. | `proven_err_t`: as `proven_loop_create`; `UNSUPPORTED` for a backend this system does not have. |
| `proven_loop_backend(loop)` | Which backend a loop was made with. | `proven_loop_backend_t`, never `AUTO`. |
| `proven_loop_run(loop)` | Wait, call what is ready, fire what is due, repeat - until `proven_loop_stop`. | `PROVEN_OK` after a stop; an error if waiting itself failed. |
| `proven_loop_poll(loop, until)` | One round: wait until something is ready or `until` passes, and handle it. `PROVEN_NET_DONT_WAIT` handles only what is ready now. | `proven_err_t`. |
| `proven_loop_stop(loop)` | Make `proven_loop_run` return. Safe from any thread and from a callback. | nothing. |
| `proven_loop_destroy(loop)` | Free the loop. What has already ended gets its last answer now - `closed` of a socket given to `proven_loop_close`, the `PROVEN_ERR_CANCELLED` of a cancelled accept - so that its memory can go; those functions only free. Anything else still attached, in flight or set is forgotten and nothing is called; the bytes waiting to be sent are freed. Null is accepted. | nothing. |

`proven_loop_run` is for a thread that does nothing else. `proven_loop_poll` is for a program
that already has a main loop of its own and gives this one a turn in it.

**Backends.** How a loop does its work is chosen when it is made, and nothing else about it
changes with the choice: the same calls, the same answers in the same order rules.

| `proven_loop_backend_t` | Where | How |
|---|---|---|
| `PROVEN_LOOP_BACKEND_AUTO` | everywhere | The completion port on Windows, the selector elsewhere - what `proven_loop_create` gives. |
| `PROVEN_LOOP_BACKEND_SELECTOR` | POSIX | epoll, kqueue or poll ([Chapter 9](manual-09-networking.md)): the loop is told a socket is ready and makes the call itself. |
| `PROVEN_LOOP_BACKEND_URING` | Linux 5.19 or later | io_uring: the receives, sends, accepts and connects are given to the kernel, which reports them done. Refused (`UNSUPPORTED`) on an older kernel, and where the system turns io_uring off - a container's seccomp filter, `io_uring_disabled`. Datagrams are sent with a plain call, as on the other backends. |
| `PROVEN_LOOP_BACKEND_IOCP` | Windows | The I/O completion port. |

io_uring is not the default on Linux: no measurement here has yet shown it faster for this
library's servers on a real machine, and it is the backend more often refused. A loop made with
it frees what the kernel still holds the way Windows does - cancelled and waited for, up to two
seconds, when the loop is destroyed. On Linux 6.0 or later a connection read with
`proven_loop_read` (section 5) is received for by the kernel into a pool of the loop's - by
default 64 buffers of 16 KiB, made at the first such read; `ring_buffers` and
`ring_buffer_bytes` in `proven_loop_options_t` change it - so that the read after a receive of
zero bytes is a copy, not a system call. A buffer is taken only when data arrives: an idle
connection still holds none. A connection whose data nobody reads holds four at most; then the
kernel's own window holds the peer, as on the other backends. Sends too go through the ring: a
send is copied and handed to the kernel in the loop's next round, together with the receives,
so that the reads and sends of every connection share the loop's one entry into the kernel;
`proven_loop_send_queued` counts the bytes until the kernel has taken them, and a close writes
to the socket what had not been handed over yet.

**The thread rule.** Everything in this chapter except `proven_loop_stop` and
`proven_loop_post` must be called on the loop's own thread - the one inside `proven_loop_run`
or `proven_loop_poll` - or before the loop starts. That covers the server and every stream
function of the later sections. Nothing checks it for you.

## 3. Timers

```c
void proven_loop_timer_set(proven_loop_t *loop, proven_loop_timer_t *timer, proven_u32 ms, proven_loop_fn fn, void *ctx);
void proven_loop_timer_cancel(proven_loop_t *loop, proven_loop_timer_t *timer);
bool proven_loop_timer_is_set(const proven_loop_timer_t *timer);
```

A `proven_loop_timer_t` is yours: put it inside the struct it belongs to, zero-initialised, and
do not copy or move it while it is set. Setting one allocates nothing.

- A timer **fires once**, `ms` milliseconds from now. It does not fire early, and on POSIX it
  fires within about a millisecond of that time; on Windows the wait ends at the system timer's
  interval, about 16 ms (until v0.70.0 every timer was rounded up to the loop's 16 ms tick). One
  that should repeat sets itself again in its own function.
- Setting a timer that is already set **moves** it.
- `proven_loop_timer_cancel` is safe inside any callback, another timer's included, and
  harmless when the timer is not set.
- Setting, cancelling and firing are constant-time, and a timer that is waiting is not looked
  at until its time. That is what lets every connection of a server have a timeout of its own.

Timers measure the monotonic clock of [Chapter 5](manual-05-hosted-services.md); setting the
wall clock does not move them.

## 4. Work that is done elsewhere

```c
proven_err_t proven_loop_post(proven_loop_t *loop, proven_loop_fn fn, void *ctx);
```

`proven_loop_post` asks the loop to call `fn(ctx)` on its own thread, soon. It is **safe from
any thread**, and it is how a result comes back: the callback hands the slow part to a worker
(the job system of [Chapter 6](manual-06-execution-and-platform.md)), returns, and the worker
posts a function that finishes the work where the loop's data may be touched.

- Functions are called in the order they were posted.
- The queue entry comes from the loop's allocator, so when other threads post, that allocator
  must be one that may be called from several threads. The heap allocator is.
- It returns `PROVEN_ERR_NOMEM` when that allocation fails, and `PROVEN_ERR_INVALID_ARG`.

The program below is a loop with nothing on it but timers and one job. The test suite compiles
and runs it:

<!-- example: manual/examples/en/ex_15_loop.c -->
```c
#include <stdatomic.h>

/*
 * An event loop by itself: timers, and work that is done on another thread and comes back.
 *
 * A loop's thread never waits for anything but "what is next". Whatever takes time - here a
 * worker that sleeps, standing in for a file or a database - happens elsewhere, and its result
 * is posted to the loop, where it runs between other events.
 */

typedef struct {
    proven_loop_t *loop;
    proven_loop_timer_t tick, late;
    int ticks;
    int order[8];
    int count;
    atomic_int from_worker;
} ex_app_t;

static void note(ex_app_t *app, int what) { if (app->count < 8) app->order[app->count++] = what; }

/* A timer fires once. One that should repeat sets itself again. */
static void on_tick(void *ctx) {
    ex_app_t *app = ctx;
    note(app, 1);
    if (++app->ticks < 3) proven_loop_timer_set(app->loop, &app->tick, 20, on_tick, app);
}

static void on_late(void *ctx) { note(ctx, 9); }

/* This runs on the loop's thread, although a worker thread asked for it. */
static void on_result(void *ctx) {
    ex_app_t *app = ctx;
    note(app, 5);
    proven_loop_timer_cancel(app->loop, &app->late);      /* it would have fired in five seconds; now it never will */
    proven_loop_stop(app->loop);
}

/* This runs on a worker thread. It may take as long as it likes: the loop is not waiting. */
static void slow_work(void *ctx) {
    ex_app_t *app = ctx;
    proven_time_sleep(400);
    atomic_store(&app->from_worker, 1);
    if (proven_loop_post(app->loop, on_result, app) != PROVEN_OK) proven_loop_stop(app->loop);   /* only a failed allocation refuses a post; then just end the run */
}

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();
    ex_app_t app = { 0 };

    // ---- a loop -------------------------------------------------------------------
    /* A loop is operations on sockets that complete, a set of timers, and a queue other threads can post to.
     * The options choose how it does its work; AUTO is what proven_loop_create gives. */
    EXAMPLE_REQUIRE(proven_loop_create_ex(heap, &(proven_loop_options_t){ .backend = PROVEN_LOOP_BACKEND_AUTO }, &app.loop) == PROVEN_OK, "a loop");
    EXAMPLE_REQUIRE(proven_loop_backend(app.loop) != PROVEN_LOOP_BACKEND_AUTO, "it says which backend does the work");

    // ---- timers --------------------------------------------------------------------
    /* The timer structs are yours: they live in your own state, and setting one allocates
     * nothing. A timer waiting to fire costs nothing either - it is not looked at until its time. */
    proven_loop_timer_set(app.loop, &app.tick, 20, on_tick, &app);
    proven_loop_timer_set(app.loop, &app.late, 5000, on_late, &app);
    EXAMPLE_REQUIRE(proven_loop_timer_is_set(&app.tick) && proven_loop_timer_is_set(&app.late), "two timers are set");

    // ---- work elsewhere ------------------------------------------------------------
    /* The job system runs slow_work on a worker thread. When it is done it posts on_result back. */
    proven_job_sys_t *workers = NULL;
    EXAMPLE_REQUIRE(proven_job_system_init(heap, 1, 4, &workers) == PROVEN_OK && proven_job_submit(workers, slow_work, &app), "a worker has the slow job");

    // ---- run -----------------------------------------------------------------------
    /* proven_loop_run returns when something calls proven_loop_stop - here on_result does. */
    proven_time_t began = proven_time_monotonic_now();
    EXAMPLE_REQUIRE(proven_loop_run(app.loop) == PROVEN_OK, "the loop ran and was stopped");
    proven_time_t took = proven_time_monotonic_now() - began;

    EXAMPLE_REQUIRE(app.ticks == 3 && app.count == 4 && app.order[0] == 1 && app.order[1] == 1 && app.order[2] == 1 && app.order[3] == 5,
                    "three ticks, then the worker's result, in that order");
    EXAMPLE_REQUIRE(atomic_load(&app.from_worker) == 1 && took < 2000000000, "the result came from the worker, and long before the five-second timer");
    EXAMPLE_REQUIRE(!proven_loop_timer_is_set(&app.late), "a cancelled timer is no longer set");

    proven_job_system_close(workers);
    proven_job_system_destroy(workers);
    proven_loop_destroy(app.loop);
    return EXAMPLE_OK();
}
```

## 5. A socket of your own

The server of the next section does this for you. It is here for a protocol that is not HTTP.

The loop does not tell you that a socket is ready and leave the rest to you. You start an
**operation** - a receive, an accept, a connect - and its function is called when it has
**finished**; you give the loop bytes to send, and it sends them. On Windows these are the I/O
completion port's operations; on Linux, the BSDs and macOS the loop does the same over the
selector of [Chapter 9](manual-09-networking.md) (epoll, kqueue). Your program sees the same
calls, answered in the same order, everywhere.

```c
proven_err_t proven_loop_attach(proven_loop_t *loop, proven_loop_sock_t *sock, const proven_net_conn_t *conn,
                                const proven_loop_sock_fns_t *fns, void *ctx);
proven_err_t proven_loop_connect(proven_loop_t *loop, proven_loop_sock_t *sock, proven_net_addr_t to, proven_net_conn_t *out,
                                 const proven_loop_sock_fns_t *fns, void *ctx);
proven_err_t proven_loop_recv(proven_loop_t *loop, proven_loop_sock_t *sock, proven_mem_mut_t buf);
void proven_loop_recv_cancel(proven_loop_t *loop, proven_loop_sock_t *sock);
proven_result_size_t proven_loop_read(proven_loop_t *loop, proven_loop_sock_t *sock, proven_mem_mut_t buf);
proven_result_size_t proven_loop_send(proven_loop_t *loop, proven_loop_sock_t *sock, proven_mem_view_t bytes);
proven_size_t proven_loop_send_queued(const proven_loop_sock_t *sock);
proven_err_t proven_loop_send_wait(proven_loop_t *loop, proven_loop_sock_t *sock, proven_size_t at_most);
void proven_loop_close(proven_loop_t *loop, proven_loop_sock_t *sock);
proven_err_t proven_loop_accept(proven_loop_t *loop, proven_loop_accept_t *acc, proven_net_listener_t *listener,
                                proven_loop_accept_fn fn, void *ctx);
void proven_loop_accept_cancel(proven_loop_t *loop, proven_loop_accept_t *acc);
proven_mem_mut_t proven_loop_scratch(proven_loop_t *loop);
proven_allocator_t proven_loop_allocator(const proven_loop_t *loop);
```

A connection given to the loop is a `proven_loop_sock_t` - yours, inside your connection struct,
not moved from `proven_loop_attach` until its `closed` - and a table of four functions,
`proven_loop_sock_fns_t`, usually one `static const` table for every connection of a kind:

| Function | Called when |
|---|---|
| `received(ctx, err, n)` | A receive has finished: data has arrived (a receive of zero bytes: `PROVEN_OK`, `n` 0), or `n` bytes are in your buffer; `PROVEN_ERR_EOF`, `PROVEN_ERR_RESET`; `PROVEN_ERR_CANCELLED`. |
| `writable(ctx, err)` | The bytes waiting to be sent are down to what `proven_loop_send_wait` asked for; or sending them failed (then also without a wait); or `PROVEN_ERR_CANCELLED` by a close. |
| `connected(ctx, err)` | A connect begun by `proven_loop_connect` has ended. |
| `closed(ctx)` | Last: after `proven_loop_close`, once nothing of the socket is in flight. Close the connection and free the memory here. |

| Call | What it does | Returns |
|---|---|---|
| `proven_loop_attach(loop, &sock, &conn, &fns, ctx)` | Give a connection to the loop. Nothing is started. | `proven_err_t`: `INVALID_ARG`. |
| `proven_loop_connect(loop, &sock, to, &conn, &fns, ctx)` | Begin connecting; the new connection is attached whatever the outcome, and `connected` says how it ended. | `proven_err_t`: a connect that failed at once, with nothing attached. |
| `proven_loop_recv(loop, &sock, buf)` | Receive. With `buf.size` 0, "tell me when there is something to read": nothing is taken and no buffer is held while it waits. With a buffer, what arrived is in it. | `proven_err_t`: `BUSY` with a receive in flight; `INVALID_STATE` after a close or before a connect has ended. |
| `proven_loop_recv_cancel(loop, &sock)` | Stop a receive in flight; it is answered with `PROVEN_ERR_CANCELLED`. | nothing. |
| `proven_loop_read(loop, &sock, buf)` | Read what has arrived, without waiting: what `proven_net_read` with `PROVEN_NET_DONT_WAIT` does on the connection - and on io_uring, a copy from the loop's pool (section 2). Use it for every read of an attached connection. | `proven_result_size_t`: the bytes read; `.err` `TIMEOUT` when nothing is there now, `EOF`, `RESET`. |
| `proven_loop_send(loop, &sock, bytes)` | Send. With nothing waiting the bytes go straight to the socket; what it does not take at once the loop copies and keeps, up to 64 KiB per connection. On io_uring every send is copied and handed to the kernel in the loop's next round (section 2). Your bytes are free when the call returns. | `proven_result_size_t`: the bytes taken; `.err` `AGAIN` when the rest did not fit; after a failed send, that failure. |
| `proven_loop_send_queued(&sock)` | How many bytes taken have not left yet. | `proven_size_t`. |
| `proven_loop_send_wait(loop, &sock, at_most)` | Have `writable` called once no more than `at_most` bytes wait (0: everything has left). | `proven_err_t`. |
| `proven_loop_close(loop, &sock)` | The end: what is in flight is answered `PROVEN_ERR_CANCELLED`, bytes still waiting are dropped (on io_uring, what the loop had not yet handed to the kernel is written to the socket first), then `closed`. | nothing. |
| `proven_loop_accept(loop, &acc, &listener, fn, ctx)` | Accept one connection: `fn(ctx, err, &conn)` once, with the connection - yours; copy it out - or the reason there is none. Arm it again from inside `fn`. `acc` is yours, zero-initialised before its first use. | `proven_err_t`: `BUSY` with an accept in flight. |
| `proven_loop_accept_cancel(loop, &acc)` | Stop accepting: one in flight is answered `PROVEN_ERR_CANCELLED`, and the listener may be closed. | nothing. |
| `proven_loop_scratch(loop)` | The loop's scratch buffer, 64 KiB. | `proven_mem_mut_t`. |
| `proven_loop_allocator(loop)` | The allocator the loop was made with. | `proven_allocator_t`. |

The rules that hold it together:

- **Every operation started is answered exactly once**, by its function, on the loop's thread -
  and never from inside the call that started it, even when it could finish at once. Success,
  failure and cancellation all come this way, so a function of yours never runs inside your own
  call to the loop.
- **One receive at a time per socket, one accept at a time per acceptor.** Sends are not
  operations of yours: the loop takes the bytes.
- **The end is `proven_loop_close`, then `closed`.** Until `closed` the loop may still use the
  socket's memory; afterwards it never touches it. Close the connection itself in `closed`. For
  a graceful end - everything sent first - wait with `proven_loop_send_wait(loop, &sock, 0)` and
  close in `writable`.
- **A receive of zero bytes is how ten thousand idle connections cost no read buffers.** When it
  is answered, read with `proven_loop_read` into the scratch buffer, use what arrived, and
  keep only what cannot be used yet. Its contents mean nothing once your function returns, or
  calls anything that may use it too.
- **Keep what the loop holds for a slow peer bounded.** When `proven_loop_send_queued` says much
  is waiting, stop reading and ask `proven_loop_send_wait` to say when to go on - the example does.
- Once a connection is attached, read it with `proven_loop_read` and send through
  `proven_loop_send`, so that bytes keep their order: on io_uring the loop's pool may hold bytes
  the socket no longer has. A connection is given to one loop, once.

The program below is an echo server and its client on one loop. The test suite compiles and runs
it:

<!-- example: manual/examples/en/ex_15_echo.c -->
```c
#include <string.h>

/*
 * An echo server and its client on one loop, with nothing but the loop's operations.
 *
 * The server takes connections with an accept it arms again from inside its own function, and
 * waits for each connection's data with a receive of zero bytes: an idle connection holds no
 * read buffer. When data has arrived it reads it into the loop's scratch buffer and sends it
 * back; what the socket does not take at once, the loop keeps. The client connects through the
 * loop too, and reads with a receive into a buffer of its own, under a timer.
 */

#define MESSAGE "every operation is answered exactly once"
#define ROUNDS 3

typedef struct ex_app ex_app_t;

typedef struct {                /* one connection of the server */
    ex_app_t *app;
    proven_loop_sock_t sock;
    proven_net_conn_t conn;
    bool ending, used;
} ex_peer_t;

struct ex_app {
    proven_loop_t *loop;
    proven_net_listener_t listener;
    proven_loop_accept_t acc;
    ex_peer_t peers[4];
    int peers_closed;
    /* the client */
    proven_loop_sock_t sock;
    proven_net_conn_t conn;
    proven_loop_timer_t deadline;
    proven_byte_t buf[64];
    proven_size_t have;
    int rounds;
    bool echoed, client_closed, accepting_ended;
};

// ---- the server's connections ------------------------------------------------------

static void peer_end(ex_peer_t *p) {
    /* The client has said all it will: close once everything owed to it has left. */
    p->ending = true;
    if (proven_loop_send_wait(p->app->loop, &p->sock, 0) != PROVEN_OK) proven_loop_close(p->app->loop, &p->sock);
}

/* Read what has arrived and send it back. A client that sends faster than it reads would make
 * the loop hold more and more for it, so past 32 KiB waiting the server stops reading until it
 * has caught up: a send wait, and reading resumes in `writable`. */
static void peer_read(ex_peer_t *p) {
    proven_loop_t *loop = p->app->loop;
    proven_mem_mut_t scratch = proven_loop_scratch(loop);
    scratch.size = 16384;
    for (;;) {
        if (proven_loop_send_queued(&p->sock) > 32768) {
            if (proven_loop_send_wait(loop, &p->sock, 0) != PROVEN_OK) proven_loop_close(loop, &p->sock);
            return;
        }
        proven_result_size_t r = proven_loop_read(loop, &p->sock, scratch);
        if (r.err == PROVEN_ERR_TIMEOUT) break;                 /* all of it read: wait for more */
        if (r.err != PROVEN_OK) { peer_end(p); return; }        /* PROVEN_ERR_EOF: the client is done */
        proven_result_size_t s = proven_loop_send(loop, &p->sock, (proven_mem_view_t){ scratch.ptr, r.value });
        if (s.err != PROVEN_OK) { proven_loop_close(loop, &p->sock); return; }
    }
    /* Nothing is held while this connection waits: a receive of zero bytes takes no buffer. */
    if (proven_loop_recv(loop, &p->sock, (proven_mem_mut_t){ 0 }) != PROVEN_OK) proven_loop_close(loop, &p->sock);
}

static void peer_received(void *ctx, proven_err_t err, proven_size_t n) {
    ex_peer_t *p = ctx;
    (void)n;                                                    /* a receive of zero bytes takes nothing */
    if (err != PROVEN_OK) return;                               /* PROVEN_ERR_CANCELLED: it is being closed */
    peer_read(p);
}

static void peer_writable(void *ctx, proven_err_t err) {
    ex_peer_t *p = ctx;
    if (err == PROVEN_ERR_CANCELLED) return;
    if (err != PROVEN_OK || p->ending) proven_loop_close(p->app->loop, &p->sock);
    else peer_read(p);
}

static void peer_closed(void *ctx) {
    ex_peer_t *p = ctx;
    (void)proven_net_close(&p->conn);                           /* the connection itself is closed here, last */
    p->used = false;
    /* The one client of this program has gone: stop taking connections. */
    if (++p->app->peers_closed == 1) proven_loop_accept_cancel(p->app->loop, &p->app->acc);
}

static const proven_loop_sock_fns_t peer_fns = { .received = peer_received, .writable = peer_writable, .closed = peer_closed };

static void on_accept(void *ctx, proven_err_t err, proven_net_conn_t *conn) {
    ex_app_t *app = ctx;
    if (err != PROVEN_OK) {                                     /* PROVEN_ERR_CANCELLED: the listener may go now */
        (void)proven_net_listener_close(&app->listener);
        app->accepting_ended = true;
        return;
    }
    ex_peer_t *p = NULL;
    for (int i = 0; i < 4 && !p; ++i) if (!app->peers[i].used) p = &app->peers[i];
    if (!p) { (void)proven_net_close(conn); }
    else {
        *p = (ex_peer_t){ .app = app, .conn = *conn, .used = true };    /* the connection is ours now */
        if (proven_loop_attach(app->loop, &p->sock, &p->conn, &peer_fns, p) != PROVEN_OK ||
            proven_loop_recv(app->loop, &p->sock, (proven_mem_mut_t){ 0 }) != PROVEN_OK) { (void)proven_net_close(&p->conn); p->used = false; }
    }
    /* Armed again from inside: a connection already waiting is taken without another round. */
    if (proven_loop_accept(app->loop, &app->acc, &app->listener, on_accept, app) != PROVEN_OK) app->accepting_ended = true;
}

// ---- the client --------------------------------------------------------------------

static void client_deadline(void *ctx) {
    ex_app_t *app = ctx;
    proven_loop_recv_cancel(app->loop, &app->sock);             /* its `received` comes with PROVEN_ERR_CANCELLED */
}

static void client_send(ex_app_t *app) {
    app->have = 0;
    proven_result_size_t s = proven_loop_send(app->loop, &app->sock, (proven_mem_view_t){ (const proven_byte_t *)MESSAGE, strlen(MESSAGE) });
    /* A receive into the client's own buffer, which is the loop's until `received`. */
    if (s.err != PROVEN_OK || proven_loop_recv(app->loop, &app->sock, (proven_mem_mut_t){ app->buf, sizeof app->buf }) != PROVEN_OK) {
        proven_loop_close(app->loop, &app->sock);
        return;
    }
    proven_loop_timer_set(app->loop, &app->deadline, 5000, client_deadline, app);
}

static void client_received(void *ctx, proven_err_t err, proven_size_t n) {
    ex_app_t *app = ctx;
    if (err != PROVEN_OK) { proven_loop_close(app->loop, &app->sock); return; }    /* the deadline passed, or the server went */
    app->have += n;
    if (app->have < strlen(MESSAGE)) {                          /* a stream: the echo may come in pieces */
        if (proven_loop_recv(app->loop, &app->sock, (proven_mem_mut_t){ app->buf + app->have, sizeof app->buf - app->have }) != PROVEN_OK)
            proven_loop_close(app->loop, &app->sock);
        return;
    }
    proven_loop_timer_cancel(app->loop, &app->deadline);
    if (memcmp(app->buf, MESSAGE, strlen(MESSAGE)) != 0) { proven_loop_close(app->loop, &app->sock); return; }
    if (++app->rounds < ROUNDS) { client_send(app); return; }
    app->echoed = true;
    proven_loop_close(app->loop, &app->sock);                   /* the server reads the end, and closes its side */
}

static void client_connected(void *ctx, proven_err_t err) {
    ex_app_t *app = ctx;
    if (err != PROVEN_OK) { proven_loop_close(app->loop, &app->sock); return; }
    client_send(app);
}

static void client_closed(void *ctx) {
    ex_app_t *app = ctx;
    (void)proven_net_close(&app->conn);
    app->client_closed = true;
}

static const proven_loop_sock_fns_t client_fns = {
    .received = client_received, .connected = client_connected, .closed = client_closed
};

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();
    static ex_app_t app;
    EXAMPLE_REQUIRE(proven_loop_create(heap, &app.loop) == PROVEN_OK, "a loop");

    // ---- the server ----------------------------------------------------------------
    proven_net_addr_t bound;
    EXAMPLE_REQUIRE(proven_net_listen(proven_net_addr_ipv4(127, 0, 0, 1, 0), 16, &app.listener, &bound) == PROVEN_OK, "a listener on a free port");
    EXAMPLE_REQUIRE(proven_loop_accept(app.loop, &app.acc, &app.listener, on_accept, &app) == PROVEN_OK, "accepting");

    // ---- the client ----------------------------------------------------------------
    EXAMPLE_REQUIRE(proven_loop_connect(app.loop, &app.sock, bound, &app.conn, &client_fns, &app) == PROVEN_OK, "a connect begun");

    // ---- run until everything has ended ----------------------------------------------
    proven_net_deadline_t end = proven_net_deadline_in(10000);
    while (!(app.client_closed && app.accepting_ended) && proven_time_monotonic_now() < end) {
        EXAMPLE_REQUIRE(proven_loop_poll(app.loop, proven_net_deadline_in(100)) == PROVEN_OK, "a round");
    }
    EXAMPLE_REQUIRE(app.echoed && app.rounds == ROUNDS, "the message came back three times");
    EXAMPLE_REQUIRE(app.peers_closed == 1 && app.accepting_ended && !proven_net_listener_is_open(&app.listener),
                    "the server closed its side after the client's, and stopped accepting");
    proven_loop_destroy(app.loop);
    return EXAMPLE_OK();
}
```

## 6. The event-driven server

```c
proven_err_t proven_http_event_server_create(proven_loop_t *loop, const proven_http_event_server_config_t *config,
                                             proven_http_event_server_t **out);
proven_err_t proven_http_event_server_listen(proven_http_event_server_t *server, proven_net_addr_t at, proven_net_addr_t *bound);
void proven_http_event_server_destroy(proven_http_event_server_t *server);
proven_size_t proven_http_event_server_connections(const proven_http_event_server_t *server);
proven_err_t proven_http_event_server_set_max_streams(proven_http_event_server_t *server, proven_size_t n);
proven_err_t proven_http_event_server_set_window(proven_http_event_server_t *server, proven_size_t n);
void proven_http_event_server_stop_listening(proven_http_event_server_t *server);
typedef void (*proven_http_event_drained_fn)(void *ctx, proven_err_t why);
proven_err_t proven_http_event_server_shutdown(proven_http_event_server_t *server, proven_u32 grace_ms,
                                               proven_http_event_drained_fn on_drained, void *ctx);
```

A server is made on a loop, told where to listen, and does nothing until the loop runs.

| Call | What it does | Returns |
|---|---|---|
| `proven_http_event_server_create(loop, &config, &server)` | Make a server. | `proven_err_t`: `INVALID_ARG` for no loop, no `on_request`, or a TLS configuration with no certificate; `NOMEM`. |
| `proven_http_event_server_listen(server, at, &bound)` | Listen at `at`; up to four addresses for one server. `bound` (optional) receives the address in use, with the port the system chose when `at` asked for port 0. | `proven_err_t`. |
| `proven_http_event_server_connections(server)` | Connections open now. | `proven_size_t`. |
| `proven_http_event_server_set_max_streams(server, n)` | Change `max_concurrent_streams` while the server runs: 1 to the configured value, for the connections open and those to come ([Chapter 17](manual-17-http2.md), sections 6 and 9). On the loop's thread. | `proven_err_t`: `INVALID_ARG`. |
| `proven_http_event_server_set_window(server, n)` | A ceiling, from now on, on the request body a client may send ahead of the handler - per stream and per connection, HTTP/2 and HTTP/3 - for the connections open and those to come. What was granted stands; growing windows stop at it ([Chapter 17](manual-17-http2.md), section 6). On the loop's thread. | `proven_err_t`: `INVALID_ARG` for 0 or more than 2^31-1. |
| `proven_http_event_server_stop_listening(server)` | Stop accepting; open connections go on, an HTTP/1.1 response from now on says `Connection: close`, HTTP/2 says goodbye. | nothing. |
| `proven_http_event_server_shutdown(server, grace_ms, on_drained, ctx)` | End gracefully (section 10); `on_drained(ctx, why)` when it is over. | `proven_err_t`: `INVALID_STATE` when already shutting down. |
| `proven_http_event_server_destroy(server)` | Close every connection and the listeners, and free the server. Exchanges still in progress get `on_done` with `PROVEN_ERR_RESET`. The loop is neither stopped nor freed. What the loop has not answered yet - an accept in flight, a connection's socket - keeps its memory until the loop's next round or `proven_loop_destroy` (section 5, rule 3). Null is accepted. | nothing. |

**Four functions.** The configuration carries them in `on`, and `ctx` is passed to each:

| Function | When | Notes |
|---|---|---|
| `on_request(ctx, stream, head)` | A request head has arrived and passed the server's checks. | Required. Answer here, or remember `stream` and answer later. `head` is good only until it returns. |
| `on_body(ctx, stream, piece, last)` | A piece of the request body; `last` is true with the final one, which may be empty. | Optional: without it a body is read and thrown away. `piece` is good only until it returns. |
| `on_writable(ctx, stream)` | Output that was held back has gone, and `proven_http_stream_write` will take more. | Optional. |
| `on_done(ctx, stream, why)` | The exchange is over. | Optional. Called **exactly once for every `on_request`**: `PROVEN_OK` when the whole response was handed to the connection, otherwise why it could not be. |

**A stream is one request and its response.** A `proven_http_stream_t *` is valid from
`on_request` until `on_done` returns, and not a moment longer. `on_done` is therefore the one
place to free what you attached to it, and to cancel a timer or forget a job that still holds
it: a function that arrives later with a stream that is over has nothing valid to call.

```c
void proven_http_stream_set_user(proven_http_stream_t *stream, void *user);
void *proven_http_stream_user(const proven_http_stream_t *stream);
proven_net_addr_t proven_http_stream_peer(const proven_http_stream_t *stream);
```

`proven_http_stream_set_user` attaches a pointer of yours to the stream and
`proven_http_stream_user` gives it back - a stream that was given none returns null, which an
`on_done` that frees must allow for. `proven_http_stream_peer` is the client's address.

## 7. Answering

```c
proven_err_t proven_http_stream_respond(proven_http_stream_t *stream, proven_u16 status,
                                        const proven_http_header_t *headers, proven_size_t header_count,
                                        proven_mem_view_t body);
proven_err_t proven_http_stream_begin(proven_http_stream_t *stream, proven_u16 status,
                                      const proven_http_header_t *headers, proven_size_t header_count,
                                      proven_u64 content_length);
proven_result_size_t proven_http_stream_write(proven_http_stream_t *stream, proven_mem_view_t data);
proven_err_t proven_http_stream_end(proven_http_stream_t *stream);
void proven_http_stream_abort(proven_http_stream_t *stream);
proven_size_t proven_http_stream_buffered(const proven_http_stream_t *stream);
```

**All of it at once.** `proven_http_stream_respond` sends a status, your headers and a body
held in memory. The body is copied whatever its size, so it is for bodies that are small.
`Content-Length`, `Date` and `Connection` are written for you and may not be among `headers`.
It may be called inside `on_request` or at any later time on the loop's thread, while the
stream is valid.

**In pieces.** `proven_http_stream_begin` sends the head and promises a body of
`content_length` bytes, or - given `PROVEN_HTTP_EVENT_LENGTH_UNKNOWN` - a body sent chunked,
whose length nobody knows yet. Then `proven_http_stream_write` offers bytes, and
`proven_http_stream_end` finishes.

**`proven_http_stream_write` may take fewer bytes than you offer** - as many as fit under
`max_buffered_output`, possibly none - and returns how many it took. That is not an error. It
means the client has not taken what was already sent, and the rule is: **keep the rest, return,
and offer it again when `on_writable` is called.** A producer written that way goes exactly as
fast as its slowest client, and the server's memory for that connection stays under the limit
whatever the size of the response.

| Call | Returns |
|---|---|
| `proven_http_stream_respond`, `proven_http_stream_begin` | `proven_err_t`: `INVALID_STATE` when a response was already begun or the stream is over; `INVALID_ARG` for a status outside 200-999 or a header the server writes itself; `OUT_OF_BOUNDS` when the response head does not fit `max_head_bytes`; `NOMEM`. |
| `proven_http_stream_write` | `proven_result_size_t`: the bytes taken. `INVALID_STATE` before `begin`, after `end`, or when the stream is over; `OUT_OF_BOUNDS` for more than the `Content-Length` promised. |
| `proven_http_stream_flush` | `proven_err_t`: hands over what a compressed response holds back (Chapter 16, section 5), and does nothing for any other. `INVALID_STATE` before `begin`, after `end`, or when the stream is over; `RESET` when the connection ended over it. |
| `proven_http_stream_end` | `proven_err_t`: `INVALID_STATE` when no response was begun or it was already ended; `INVALID_FORMAT` when fewer bytes were written than promised - the connection is then closed, so that the client cannot take a short body for a whole one. |
| `proven_http_stream_abort` | nothing. The connection is closed at once and `on_done` is called with `PROVEN_ERR_RESET`. |
| `proven_http_stream_buffered` | `proven_size_t`: response bytes held for this connection that the client has not accepted. |
| `proven_http_stream_compress` | nothing. Asks that this response be compressed if the client accepts gzip; before the response begins. [Chapter 16](manual-16-compression.md), section 6 - read its cautions first. |

Three details that follow from the protocol and need nothing from you: a response to `HEAD`,
and one with status 204 or 304, is sent without a body whatever you pass; a request that said
`Expect: 100-continue` is told to go on when its body is first wanted; and after the response
to a request that asked for it, or that could not be read to its end, the connection is closed
rather than kept.

`on_done` may be called **inside** `proven_http_stream_respond` or `proven_http_stream_end`,
when everything fits in the socket at once. After either returns, do not touch the stream or
what `on_done` freed.

The program below answers in four ways - at once, later from a timer, streamed with a pump
that gives way when it is refused, and after a body received in pieces. The test suite compiles
and runs it:

<!-- example: manual/examples/en/ex_15_http_event.c -->
```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * An HTTP server on an event loop: one thread, and no call in it that waits.
 *
 * The server tells these functions what happened, and they return at once. Four kinds of
 * answer are shown: at once; later, from a timer; streamed, giving way when the client is
 * slow; and after a request body has arrived piece by piece.
 *
 * The client is the blocking one of chapter 11, on the example's main thread.
 */

typedef struct {
    proven_loop_t *loop;
    int served;
    proven_http_event_server_t *server;
    proven_err_t ended;                /* what on_drained was told */
} ex_app_t;

/* What this program keeps for one request while it is being answered. */
typedef struct {
    ex_app_t *app;
    proven_http_stream_t *stream;
    proven_loop_timer_t timer;
    proven_size_t sent, total;         /* /count: how far the body has got */
    proven_size_t part;                /* /count: how much of the current line has gone */
    proven_size_t received;            /* /size: body bytes seen */
} ex_exchange_t;

/* Write lines until the server says "no more for now". It says so when the client has not
 * taken what was already sent; the rest waits for on_writable. */
static void count_pump(ex_exchange_t *x) {
    while (x->sent < x->total) {
        char line[32];
        int n = snprintf(line, sizeof line, "%u\n", (unsigned)x->sent);
        proven_result_size_t w = proven_http_stream_write(x->stream, (proven_mem_view_t){ (const proven_byte_t *)line + x->part, (proven_size_t)n - x->part });
        if (w.err != PROVEN_OK) return;                   /* the client left */
        x->part += w.value;                              /* a write may take part of what it is given */
        if (x->part < (proven_size_t)n) return;           /* held back: on_writable will say when */
        x->part = 0;
        x->sent++;
    }
    (void)proven_http_stream_end(x->stream);
}

static void answer_later(void *ctx) {
    ex_exchange_t *x = ctx;
    (void)proven_http_stream_respond(x->stream, 200, NULL, 0, proven_mem_view_from_u8(PROVEN_LIT("worth the wait\n")));
}

static void on_request(void *ctx, proven_http_stream_t *stream, const proven_http_request_t *head) {
    ex_app_t *app = ctx;
    ex_exchange_t *x = calloc(1, sizeof *x);
    if (!x) { proven_http_stream_abort(stream); return; }
    x->app = app;
    x->stream = stream;
    proven_http_stream_set_user(stream, x);

    /* `head` and everything in it are good only until this function returns. */
    if (proven_u8str_view_eq(head->target, PROVEN_LIT("/"))) {
        /* The simplest answer: all of it, now. */
        (void)proven_http_stream_respond(stream, 200, NULL, 0, proven_mem_view_from_u8(PROVEN_LIT("hello\n")));
    } else if (proven_u8str_view_eq(head->target, PROVEN_LIT("/slow"))) {
        /* Not now: remember the stream and answer from a timer. The loop goes on meanwhile. */
        proven_loop_timer_set(app->loop, &x->timer, 50, answer_later, x);
    } else if (proven_u8str_view_eq(head->target, PROVEN_LIT("/count"))) {
        /* A body made as it goes, sent chunked because its length is not known first. */
        x->total = 20000;
        if (proven_http_stream_begin(stream, 200, NULL, 0, PROVEN_HTTP_EVENT_LENGTH_UNKNOWN) == PROVEN_OK) count_pump(x);
    } else if (proven_u8str_view_eq(head->target, PROVEN_LIT("/size"))) {
        /* Nothing yet: the body is on its way, and on_body answers when it has all come. */
    } else {
        (void)proven_http_stream_respond(stream, 404, NULL, 0, proven_mem_view_from_u8(PROVEN_LIT("not here\n")));
    }
}

static void on_body(void *ctx, proven_http_stream_t *stream, proven_mem_view_t piece, bool last) {
    (void)ctx;
    ex_exchange_t *x = proven_http_stream_user(stream);
    x->received += piece.size;
    if (last) {
        char text[32];
        int n = snprintf(text, sizeof text, "%u bytes\n", (unsigned)x->received);
        (void)proven_http_stream_respond(stream, 200, NULL, 0, (proven_mem_view_t){ (const proven_byte_t *)text, (proven_size_t)n });
    }
}

/* The output that was held back has gone: carry on from where the writing stopped. */
static void on_writable(void *ctx, proven_http_stream_t *stream) {
    (void)ctx;
    ex_exchange_t *x = proven_http_stream_user(stream);
    if (x->total > 0) count_pump(x);
}

/* Called once for every on_request, whatever happened. The place to free what was attached. */
static void on_done(void *ctx, proven_http_stream_t *stream, proven_err_t why) {
    ex_app_t *app = ctx;
    ex_exchange_t *x = proven_http_stream_user(stream);
    if (!x) return;
    proven_loop_timer_cancel(app->loop, &x->timer);
    free(x);
    if (why == PROVEN_OK) app->served++;
}

/* The end, on the loop's thread: what is open finishes, and then the loop is stopped. */
static void on_drained(void *ctx, proven_err_t why) {
    ex_app_t *app = ctx;
    app->ended = why;
    proven_loop_stop(app->loop);
}
static void begin_end(void *ctx) {
    ex_app_t *app = ctx;
    if (proven_http_event_server_shutdown(app->server, 2000, on_drained, app) != PROVEN_OK) proven_loop_stop(app->loop);
}

static void run(void *arg) { (void)proven_loop_run(((ex_app_t *)arg)->loop); }

static char g_url[64];
static proven_u8str_view_t url_for(proven_u16 port, const char *path) {
    int n = snprintf(g_url, sizeof g_url, "http://127.0.0.1:%u%s", (unsigned)port, path);
    return (proven_u8str_view_t){ (const proven_byte_t *)g_url, (proven_size_t)n };
}

static bool body_equals(proven_http_client_response_t *resp, proven_allocator_t alloc, const char *want) {
    proven_u8str_t body = { 0 };
    bool ok = proven_http_client_read_all(resp, alloc, &body, 1024 * 1024) == PROVEN_OK &&
              proven_u8str_view_eq(proven_u8str_as_view(&body), (proven_u8str_view_t){ (const proven_byte_t *)want, strlen(want) });
    proven_u8str_destroy(alloc, &body);
    return ok;
}

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();
    ex_app_t app = { 0 };

    // ---- the server ----------------------------------------------------------------
    /* A server is made on a loop and listens; it does nothing until the loop runs. */
    EXAMPLE_REQUIRE(proven_loop_create(heap, &app.loop) == PROVEN_OK, "a loop");
    proven_http_event_server_config_t config = {
        .on = { on_request, on_body, on_writable, on_done },
        .ctx = &app,
        .max_buffered_output = 16 * 1024,        /* hold at most this much per connection that the client has not taken */
    };
    proven_http_event_server_t *server = NULL;
    proven_net_addr_t at;
    EXAMPLE_REQUIRE(proven_http_event_server_create(app.loop, &config, &server) == PROVEN_OK &&
                    proven_http_event_server_listen(server, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &at) == PROVEN_OK, "an event-driven server on a free port");
    app.server = server;

    /* This example gives the loop a thread of its own, so that its main thread can be a client. */
    proven_job_sys_t *threads = NULL;
    proven_job_group_t running;
    proven_job_group_init(&running);
    EXAMPLE_REQUIRE(proven_job_system_init(heap, 1, 4, &threads) == PROVEN_OK &&
                    proven_job_group_submit(threads, &running, run, &app) == PROVEN_OK, "the loop runs on another thread");

    // ---- four requests -------------------------------------------------------------
    proven_http_client_config_t client_config = { .alloc = heap, .max_idle_connections = 1 };
    proven_http_client_t *client = NULL;
    EXAMPLE_REQUIRE(proven_http_client_create(&client_config, &client) == PROVEN_OK, "a client");
    proven_http_client_response_t resp;

    EXAMPLE_REQUIRE(proven_http_client_get(client, url_for(at.port, "/"), &resp) == PROVEN_OK && body_equals(&resp, heap, "hello\n"), "answered at once");
    proven_http_client_finish(&resp);
    EXAMPLE_REQUIRE(proven_http_client_get(client, url_for(at.port, "/slow"), &resp) == PROVEN_OK && body_equals(&resp, heap, "worth the wait\n"), "answered 50 ms later, from a timer");
    proven_http_client_finish(&resp);

    /* Twenty thousand lines. Whenever the server holds back, the handler stops and on_writable resumes it. */
    EXAMPLE_REQUIRE(proven_http_client_get(client, url_for(at.port, "/count"), &resp) == PROVEN_OK && resp.status == 200, "a streamed response begins");
    proven_u8str_t counted = { 0 };
    EXAMPLE_REQUIRE(proven_http_client_read_all(&resp, heap, &counted, 1024 * 1024) == PROVEN_OK, "and is read to its end");
    proven_u8str_view_t all = proven_u8str_as_view(&counted);
    EXAMPLE_REQUIRE(all.size == 108890 && memcmp(all.ptr, "0\n1\n2\n", 6) == 0 && memcmp(all.ptr + all.size - 6, "19999\n", 6) == 0, "every line arrived, in order");
    proven_u8str_destroy(heap, &counted);
    proven_http_client_finish(&resp);

    static proven_byte_t upload[50000];
    proven_http_client_request_t post = { .method = PROVEN_LIT("POST"), .url = url_for(at.port, "/size"), .body = { upload, sizeof upload } };
    EXAMPLE_REQUIRE(proven_http_client_send(client, &post, &resp) == PROVEN_OK && body_equals(&resp, heap, "50000 bytes\n"), "a body counted piece by piece");
    proven_http_client_finish(&resp);
    proven_http_client_destroy(client);

    // ---- done ----------------------------------------------------------------------
    /* End gracefully, on the loop's thread; on_drained stops the loop. Then take the server
     * apart: nothing else is touching it by then. */
    app.ended = PROVEN_ERR_INVALID_STATE;
    EXAMPLE_REQUIRE(proven_loop_post(app.loop, begin_end, &app) == PROVEN_OK, "the end is asked for");
    proven_job_group_wait(threads, &running);
    EXAMPLE_REQUIRE(app.ended == PROVEN_OK && app.served == 4, "it ended with nothing cut, and four exchanges ended with on_done(PROVEN_OK)");
    proven_http_event_server_destroy(server);
    proven_loop_destroy(app.loop);
    proven_job_system_close(threads);
    proven_job_system_destroy(threads);
    return EXAMPLE_OK();
}
```

Look at `count_pump`: it is the shape every streamed response has. It is called from
`on_request` to start and from `on_writable` to go on, it keeps its position in the exchange's
own state, and it returns the moment a write takes less than was offered. Whether a given run
is ever refused depends on how fast the client reads; the function is correct either way.

## 8. Request bodies

```c
void proven_http_stream_pause(proven_http_stream_t *stream);
void proven_http_stream_resume(proven_http_stream_t *stream);
```

A body arrives through `on_body` in pieces of whatever size the network delivered, with the
framing - a `Content-Length` or chunks - already taken off. A request with no body gets no
`on_body` call at all; one with a body gets a last call with `last` true.

A handler may answer before the body has arrived, or without reading it. The rest of the body
is then read and thrown away if that is cheap, and the connection is closed after the response
if it is not.

`proven_http_stream_pause` is backpressure in the other direction: no more pieces are
delivered, and the connection is not read, until `proven_http_stream_resume`. A handler that
writes what it receives to something slower than the network uses the pair so that the client
is slowed instead of the body being piled up in memory. Pieces that were waiting may arrive
inside the call to `proven_http_stream_resume`.

## 9. Limits, time and refusals

Zero-initialise a `proven_http_event_server_config_t`, set `on.on_request`, and set what you
need. Every zero field has a default:

| Field | Meaning | Default |
|---|---|---|
| `alloc` | The allocator for connections and buffers. | the loop's |
| `max_connections` | Connections open at once. Further ones wait, unaccepted, in the system's backlog. | 10,000 |
| `max_head_bytes` | Largest request head; also the largest response head. | 16 KiB |
| `max_headers` | Most header fields in a request. | 64 |
| `max_body_bytes` | Largest request body. | 1 MiB |
| `max_buffered_output` | Response bytes held for one connection before writes are refused. | 64 KiB |
| `head_timeout_ms` | From a request's first byte to the end of its head. With TLS it covers the handshake too. | 10 s |
| `body_timeout_ms` | Between pieces of a request body. | 30 s |
| `write_timeout_ms` | Output held with none of it accepted by the client. | 30 s |
| `idle_timeout_ms` | An open connection with no request on it. | 60 s |
| `linger_ms` | How long a connection that is being closed after its answer is still read from, so that the answer is not lost to a reset. It counts as a connection until then. | 1 s |
| `tls` | A TLS configuration with a certificate, or null for plain HTTP. With its `early_data` (since v0.84.0) a request that arrives as TLS early data reaches its handler before the handshake is over, with `proven_http_stream_early` true, and may be answered at once; only GET, HEAD and OPTIONS without a body get that far - the server answers anything else that arrives early with 425 itself ([Chapter 14](manual-14-tls.md), section 6). | null |
| `http2` | Let a connection be HTTP/2 - by ALPN over TLS, by its first bytes in the clear - beside HTTP/1.1 on the same port, for the same handler: [Chapter 17, section 6](manual-17-http2.md#6-http2-in-the-event-driven-server). | false |
| `max_concurrent_streams` | HTTP/2: requests in progress on one connection. | 100 |
| `http2_window_bytes` | HTTP/2: request-body bytes a client may send on a stream, and on a connection as a whole, before the handler has taken them. What a paused request can make a connection hold; a large upload waits for room each time it has sent this much. With 0 the window starts there and grows while bodies are read - doubling while half of it is taken within two round trips, up to 16 MiB a stream and 24 MiB a connection (v0.75.0, as HTTP/3's since v0.71.0); a body not read stops the growth. A value set stays fixed. | 65,535, growing |
| `http3` | HTTP/3 too, over QUIC on a UDP socket beside each TCP listener on the same port, for the same handler, and `Alt-Svc` on the TCP responses: [Chapter 17, section 9](manual-17-http2.md#9-http3-in-the-event-driven-server). Needs `tls`, offering `h3`. | false |
| `http3_retry` | HTTP/3: answer each client's first packet with a Retry, so that no state is kept for an address that has not shown it receives - a round trip more per connection, for a server under load. | false |
| `http3_window_bytes` | HTTP/3: request-body bytes a client may send on a stream, and on a connection as a whole, before the handler has taken them (QUIC's flow control). | 256 KiB a stream and 1 MiB a connection, growing while bodies are read as fast as they come, up to 16 MiB and 24 MiB; a value set is fixed |

A request the server will not serve never reaches `on_request`. It is answered and the
connection closed:

| Status | For |
|---|---|
| 400 | A head that is not HTTP; an HTTP/1.1 request without exactly one `Host`; body framing that contradicts itself; a chunk that is not one. |
| 408 | A head begun and not finished within `head_timeout_ms`. |
| 413 | A body longer than `max_body_bytes`. |
| 431 | A head larger than `max_head_bytes`, or with more fields than `max_headers`. |
| 501 | A transfer coding that is not `chunked`. |
| 505 | A version that is not HTTP/1.0 or HTTP/1.1. |

When one of these happens after `on_request` - a body that turns out too long, or malformed
part-way - the exchange ends with `on_done` and the reason, and the client gets the status if a
response had not already begun. A connection that passes `body_timeout_ms`,
`write_timeout_ms` or `idle_timeout_ms` is closed; an exchange in progress on it ends with
`on_done(PROVEN_ERR_TIMEOUT)`. A client that goes away ends its exchange with
`PROVEN_ERR_RESET`.

Requests sent one after another on a connection without waiting - pipelined - are served in
order, one stream at a time.

## 10. Threads, TLS and shutting down

**Threads.** One loop is one thread, and the server belongs to it. A handler that must compute
or wait for something submits a job, returns, and the job posts the answer back:

1. `on_request` copies what it needs from the head, remembers the stream, and submits the job.
2. The job works on another thread, touching nothing of the server.
3. It calls `proven_loop_post` with a function that, on the loop's thread, calls
   `proven_http_stream_respond`.
4. If `on_done` came first - the client left - that function must find out and do nothing. The
   usual way is for `on_done` to mark the exchange's own state as finished and leave freeing it
   to whichever of the two runs last.

**TLS.** Give the configuration a `proven_tls_config_t` with a certificate
([Chapter 14](manual-14-tls.md), section 2) and every connection is TLS; nothing else in your
code changes. The handshake is carried by the loop like any other reading and writing, and is
subject to `head_timeout_ms`. The configuration must outlive the server. A handshake's public-key
work is done on the loop's thread, and while it runs nothing else is served: Chapter 14,
section 9, gives what one costs.

**Shutting down: `proven_http_event_server_shutdown(server, grace_ms, on_drained, ctx)`.** On the
loop's thread. From the call nothing new is taken; an HTTP/1.1 connection with no request on
it is closed; one where a request has begun to arrive, or is being answered, finishes it with
`Connection: close`; an HTTP/2 connection says goodbye (GOAWAY) and finishes its open
streams; a WebSocket is closed with 1001, "going away", and its peer's answer waited for. When
`grace_ms` has passed, what is left is ended - an exchange gets `on_done(PROVEN_ERR_RESET)`, a
WebSocket `on_closed`. `on_drained(ctx, why)` is called once, from the loop, when the last
connection has gone: `why` is `PROVEN_OK`, or `PROVEN_ERR_TIMEOUT` when the deadline cut
something. It may call `proven_loop_stop`; then destroy the server and the loop after the
loop has returned. A shutdown is final - the server listens and adopts no more. The deadline
bounds connections, not handlers: work a handler sent to another thread comes back to a
stream that is gone.

`proven_http_event_server_stop_listening` alone is the first step of that, without the rest:
nobody new, HTTP/1.1 responses say `Connection: close`, HTTP/2 says goodbye - and connections
end when their clients let them, or at `idle_timeout_ms`. Destroy the server on the loop's
thread or after the loop has stopped - not while another thread is inside `proven_loop_run`.
The example of section 7 ends with a shutdown.

## 11. WebSocket on the loop

[Chapter 12](manual-12-websocket.md)'s WebSocket connection has calls that wait, and so
occupies a thread for as long as it is open. `ws_event.h` is the other form: a request to the
server of this chapter is turned into a WebSocket inside `on_request`, and from then on the
loop tells your functions what arrived. A connection that is silent holds its state and no
buffer.

```c
proven_err_t proven_ws_event_accept(proven_http_stream_t *stream, const proven_http_request_t *head,
                                    const proven_ws_event_config_t *config, proven_ws_stream_t **out);
proven_err_t proven_ws_stream_send(proven_ws_stream_t *ws, bool text, proven_mem_view_t data);
proven_err_t proven_ws_stream_send_piece(proven_ws_stream_t *ws, bool text, proven_mem_view_t data, bool first, bool last);
proven_err_t proven_ws_stream_ping(proven_ws_stream_t *ws, proven_mem_view_t data);
void proven_ws_stream_close(proven_ws_stream_t *ws, proven_u16 code, proven_u8str_view_t reason);
void proven_ws_stream_abort(proven_ws_stream_t *ws);
void proven_ws_stream_pause(proven_ws_stream_t *ws);
void proven_ws_stream_resume(proven_ws_stream_t *ws);
void proven_ws_stream_set_user(proven_ws_stream_t *ws, void *user);
void *proven_ws_stream_user(const proven_ws_stream_t *ws);
proven_net_addr_t proven_ws_stream_peer(const proven_ws_stream_t *ws);
proven_size_t proven_ws_stream_buffered(const proven_ws_stream_t *ws);
```

**Accepting.** `proven_ws_event_accept` is called inside `on_request`, while `head` is valid.
It checks the request, answers 101, and turns the connection into a WebSocket. **The HTTP
exchange ends inside that call**: `on_done(PROVEN_OK)` is called for the stream, which is then
over, and `*out` is what you hold until `on_closed` returns.

| It returns | Meaning | What the handler does |
|---|---|---|
| `PROVEN_OK` | The connection is a WebSocket. | Attach its state to `*out` and return. |
| `PROVEN_ERR_NOT_FOUND` | The request did not ask for a WebSocket. Nothing was sent. | Answer it as the ordinary request it is. |
| `PROVEN_ERR_UNSUPPORTED` | An upgrade for a version other than 13. Nothing was sent. | Answer 426 with `Sec-WebSocket-Version: 13`. |
| `PROVEN_ERR_INVALID_FORMAT` | An upgrade, but malformed. Nothing was sent. | Answer 400. |
| `PROVEN_ERR_INVALID_ARG` | No `on_message`, or a subprotocol that is not a token. | A defect in the program. |
| `PROVEN_ERR_INVALID_STATE` | Not inside `on_request` for this stream, or a response was already begun. | A defect in the program. |
| `PROVEN_ERR_NOMEM` | | Answer 503, or abort. |
| `PROVEN_ERR_RESET` | The client vanished at that moment; `on_done` has been called. | Nothing: the stream is over. |

A `proven_ws_event_config_t` is zero-initialised, given `on.on_message`, and whatever else is
needed:

| Field | Meaning | Default |
|---|---|---|
| `on.on_message(ctx, ws, text, piece, first, last)` | A piece of a message. Required. | |
| `on.on_writable(ctx, ws)` | A send that was refused will now be taken. | none |
| `on.on_closed(ctx, ws, code, why)` | The connection is over. Called exactly once. | none |
| `ctx` | Passed to every callback. | |
| `subprotocol` | Named in the answer when not empty. Name one only if the client offered it (`proven_ws_request_offers`). | none |
| `max_message_bytes` | Largest message accepted, its pieces together. | 1 MiB |
| `max_buffered_output` | Output held before sends are refused. | 64 KiB |
| `ping_interval_ms` | Silence from the client after which it is sent a ping. | 30 s |
| `pong_timeout_ms` | Further silence after which the connection is ended. | 10 s |
| `close_timeout_ms` | How long to wait for the answer to a close, counted from the last output the client took. | 5 s |
| `compress`, `compress_level`, `compress_window_bits`, `compress_no_takeover`, `compress_peer_no_takeover` | Accept compressed messages when the client offers them: [Chapter 12](manual-12-websocket.md), section 9. Each side keeps its window between messages (about 64 KiB held per connection) unless `compress_no_takeover` is set or the client forbids it. A compressed message arrives inflated, in pieces of at most 16 KiB, and `max_message_bytes` counts the inflated size. | off; 6; 11 with takeover, 15 without; off |

**Messages arrive in pieces.** `on_message` is given each message in the pieces the network
delivered, with `first` and `last` marking its ends: a message of any size passes through
without being assembled or held, and `piece` is good only until the function returns. Text is
checked to be UTF-8 over the whole message, and a piece may end in the middle of a character.
A handler that wants whole messages collects the pieces itself, up to a limit of its own
choosing - the program below does. `max_message_bytes` is the server's limit, not a buffer:
a message past it is refused with close code 1009.

**A send is taken whole or not at all.** `proven_ws_stream_send` queues one message when less
than `max_buffered_output` is held, and otherwise takes nothing and returns
`PROVEN_ERR_AGAIN`: keep the message, and send it again when `on_writable` is called. That is
the difference from the HTTP body of section 7, whose write says how many bytes it took - a
frame cannot be half-sent without the library choosing where to cut your message. The most a
connection holds is therefore the limit and one message of your choosing; for a large message
produced as it goes, `proven_ws_stream_send_piece` sends it a fragment at a time under the same
rule. A handler that only echoes can stop reading while it waits, with
`proven_ws_stream_pause`, so that a client that sends faster than it reads is slowed rather
than buffered.

| Call | Returns |
|---|---|
| `proven_ws_stream_send`, `proven_ws_stream_send_piece` | `proven_err_t`: `AGAIN` when the output limit is reached, with nothing taken; `INVALID_STATE` after a close was sent, for `first` inside a message, or for a piece that is not `first` outside one; `RESET` when the connection is gone. |
| `proven_ws_stream_ping` | the same; `OUT_OF_BOUNDS` for more than 125 bytes. |
| `proven_ws_stream_close` | nothing. Sends a close with `code` and `reason`, and ends the connection when the client answers or the close timeout passes; `on_closed` follows later. A code that may not be sent becomes 1000. |
| `proven_ws_stream_abort` | nothing. Ends the connection at once with no close frame; `on_closed(1006, PROVEN_ERR_RESET)` is called inside the call. |
| `proven_ws_stream_pause`, `proven_ws_stream_resume` | nothing. Stop and restart the delivery of messages and the reading of the connection. |
| `proven_ws_stream_buffered` | `proven_size_t`: bytes queued for the client and not yet accepted by it. |
| `proven_ws_stream_compressed` | `bool`: whether `compress` was set and the client agreed. |
| `proven_ws_stream_set_compress` | nothing. Whether the messages sent from now on are compressed, on a connection that agreed to it (Chapter 12, section 9). |

**How it ends.** `on_closed` is called exactly once, and what it is given says how:

| `code` | `why` | What happened |
|---|---|---|
| the client's code (1005 if its close had none) | `PROVEN_OK` | The client closed, and was answered. |
| the code this side sent | `PROVEN_OK` | This side closed, and the client answered. |
| the code this side sent | `PROVEN_ERR_TIMEOUT` | This side closed, and the client never answered. |
| 1002, 1007, 1009 | `PROVEN_ERR_INVALID_FORMAT`, `PROVEN_ERR_INVALID_ENCODING`, `PROVEN_ERR_OUT_OF_BOUNDS` | The client broke the protocol, sent text that is not UTF-8, or a message past the limit; it was sent that code. |
| 1006 | `PROVEN_ERR_TIMEOUT` | It answered neither data nor a ping within the two liveness limits. |
| 1006 | `PROVEN_ERR_RESET` | It vanished without a close frame; or `proven_ws_stream_abort`; or the server was destroyed. |

Pings from the client are answered for you, and its pongs need nothing from you. Liveness is
judged by what is received: a client that is sent data and never reads it is not detected by
that alone - but nothing more is queued for it than the output limit, and once it stops
answering pings it is dropped.

The test suite compiles and runs this program:

<!-- example: manual/examples/en/ex_15_ws_event.c -->
```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * WebSocket on the event-driven server: connections that cost no thread.
 *
 * A request becomes a WebSocket inside on_request. From then on the loop tells these functions
 * what arrived, and they return at once. This server shouts back: every text message is
 * answered in capitals.
 *
 * The client is the blocking one of chapter 12, on the example's main thread.
 */

typedef struct {
    proven_loop_t *loop;
    int opened, closed;
    proven_u16 last_code;
} ex_app_t;

/* What this program keeps for one connection: the message being collected. */
typedef struct {
    ex_app_t *app;
    char text[4096];
    proven_size_t len;
    bool waiting;                 /* a reply the server would not take yet */
} ex_peer_t;

/* Send the reply. If the client is not taking what was sent before, the server refuses it:
 * keep it, stop reading from this client, and try again when on_writable says so. */
static void reply(proven_ws_stream_t *ws, ex_peer_t *peer) {
    proven_err_t err = proven_ws_stream_send(ws, true, (proven_mem_view_t){ (const proven_byte_t *)peer->text, peer->len });
    if (err == PROVEN_ERR_AGAIN) {
        peer->waiting = true;
        proven_ws_stream_pause(ws);
        return;
    }
    peer->waiting = false;
    peer->len = 0;
}

/* A message arrives in the pieces the network delivered. This handler wants whole messages,
 * so it collects them - up to a limit of its own, which is the point of doing it by hand. */
static void on_message(void *ctx, proven_ws_stream_t *ws, bool text, proven_mem_view_t piece, bool first, bool last) {
    (void)ctx;
    ex_peer_t *peer = proven_ws_stream_user(ws);
    if (first) peer->len = 0;
    if (!text || peer->len + piece.size > sizeof peer->text) {
        proven_ws_stream_close(ws, 1009, PROVEN_LIT("short text only"));       /* 1009: too big */
        return;
    }
    for (proven_size_t i = 0; i < piece.size; ++i) {
        proven_byte_t ch = piece.ptr[i];
        peer->text[peer->len++] = (char)(ch >= 'a' && ch <= 'z' ? ch - 32 : ch);
    }
    if (last) reply(ws, peer);
}

/* There is room again. */
static void on_writable(void *ctx, proven_ws_stream_t *ws) {
    (void)ctx;
    ex_peer_t *peer = proven_ws_stream_user(ws);
    if (!peer->waiting) return;
    reply(ws, peer);
    if (!peer->waiting) proven_ws_stream_resume(ws);
}

/* Called once, however the connection ended. The place to free what was attached. */
static void on_closed(void *ctx, proven_ws_stream_t *ws, proven_u16 code, proven_err_t why) {
    ex_app_t *app = ctx;
    (void)why;
    free(proven_ws_stream_user(ws));
    app->last_code = code;
    app->closed++;
}

static void on_request(void *ctx, proven_http_stream_t *stream, const proven_http_request_t *head) {
    ex_app_t *app = ctx;
    ex_peer_t *peer = calloc(1, sizeof *peer);
    if (!peer) { proven_http_stream_abort(stream); return; }
    peer->app = app;

    proven_ws_event_config_t config = {
        .on = { on_message, on_writable, on_closed },
        .ctx = app,
        .max_message_bytes = sizeof peer->text,
    };
    proven_ws_stream_t *ws = NULL;
    /* Check the request, answer 101, and turn the connection into a WebSocket. The HTTP
     * exchange ends inside this call (on_done is called, if there is one). */
    proven_err_t err = proven_ws_event_accept(stream, head, &config, &ws);
    if (err != PROVEN_OK) {
        free(peer);
        /* Nothing was sent: the request is still an ordinary one, and is answered as such. */
        if (err == PROVEN_ERR_UNSUPPORTED) {
            proven_http_header_t version = { PROVEN_LIT("Sec-WebSocket-Version"), PROVEN_LIT("13") };
            (void)proven_http_stream_respond(stream, 426, &version, 1, (proven_mem_view_t){ 0 });
        } else if (err != PROVEN_ERR_RESET) {
            (void)proven_http_stream_respond(stream, err == PROVEN_ERR_NOT_FOUND ? 404 : 400, NULL, 0, (proven_mem_view_t){ 0 });
        }
        return;
    }
    /* `stream` is over now. `ws` is valid until on_closed returns. */
    proven_ws_stream_set_user(ws, peer);
    app->opened++;
}

static void run(void *arg) { (void)proven_loop_run(((ex_app_t *)arg)->loop); }

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();
    ex_app_t app = { 0 };

    // ---- a server on a loop, on its own thread -------------------------------------
    EXAMPLE_REQUIRE(proven_loop_create(heap, &app.loop) == PROVEN_OK, "a loop");
    /* HTTP/2 too, and WebSocket on it (RFC 8441): a client that speaks HTTP/2 opens its
     * WebSocket as a stream of that connection. on_request above is the same for both - it is
     * this program's client, over HTTP/1.1, that the run below uses. */
    proven_http_event_server_config_t config = { .on = { .on_request = on_request }, .ctx = &app, .http2 = true, .http2_websocket = true };
    proven_http_event_server_t *server = NULL;
    proven_net_addr_t at;
    EXAMPLE_REQUIRE(proven_http_event_server_create(app.loop, &config, &server) == PROVEN_OK &&
                    proven_http_event_server_listen(server, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &at) == PROVEN_OK, "an event-driven server on a free port");
    proven_job_sys_t *threads = NULL;
    proven_job_group_t running;
    proven_job_group_init(&running);
    EXAMPLE_REQUIRE(proven_job_system_init(heap, 1, 4, &threads) == PROVEN_OK &&
                    proven_job_group_submit(threads, &running, run, &app) == PROVEN_OK, "the loop runs on another thread");

    // ---- a client ------------------------------------------------------------------
    /* A blocking client is the simplest way to show the other end. */
    proven_http_client_config_t client_config = { .alloc = heap };
    proven_http_client_t *client = NULL;
    EXAMPLE_REQUIRE(proven_http_client_create(&client_config, &client) == PROVEN_OK, "an HTTP client");
    char url[64];
    int n = snprintf(url, sizeof url, "ws://127.0.0.1:%u/shout", (unsigned)at.port);
    proven_ws_conn_config_t ws_config = { .alloc = heap };
    proven_ws_conn_t *conn = NULL;
    EXAMPLE_REQUIRE(proven_ws_conn_connect(client, (proven_u8str_view_t){ (const proven_byte_t *)url, (proven_size_t)n }, NULL, 0, PROVEN_LIT(""),
                                           &ws_config, &conn, NULL) == PROVEN_OK, "the server accepts a WebSocket");

    proven_ws_message_t msg;
    EXAMPLE_REQUIRE(proven_ws_conn_send_text(conn, PROVEN_LIT("hello, loop")) == PROVEN_OK &&
                    proven_ws_conn_receive(conn, proven_net_deadline_in(5000), &msg) == PROVEN_OK &&
                    msg.text && msg.data.size == 11 && memcmp(msg.data.ptr, "HELLO, LOOP", 11) == 0, "a message comes back in capitals");
    /* However the client cuts a message up, the handler sees one message. */
    EXAMPLE_REQUIRE(proven_ws_conn_send_part(conn, true, proven_mem_view_from_u8(PROVEN_LIT("in three ")), false) == PROVEN_OK &&
                    proven_ws_conn_send_part(conn, true, proven_mem_view_from_u8(PROVEN_LIT("pieces, ")), false) == PROVEN_OK &&
                    proven_ws_conn_send_part(conn, true, proven_mem_view_from_u8(PROVEN_LIT("one answer")), true) == PROVEN_OK &&
                    proven_ws_conn_receive(conn, proven_net_deadline_in(5000), &msg) == PROVEN_OK &&
                    msg.data.size == 27 && memcmp(msg.data.ptr, "IN THREE PIECES, ONE ANSWER", 27) == 0, "three fragments are one message to the handler");

    // ---- closing -------------------------------------------------------------------
    EXAMPLE_REQUIRE(proven_ws_conn_close(conn, 1000, PROVEN_LIT("done")) == PROVEN_OK, "the client's close is answered");
    proven_ws_conn_destroy(conn);
    proven_http_client_destroy(client);

    /* Stop the loop before looking at what its thread wrote. */
    proven_loop_stop(app.loop);
    proven_job_group_wait(threads, &running);
    EXAMPLE_REQUIRE(app.opened == 1 && app.closed == 1 && app.last_code == 1000, "one connection was opened, and closed once, with the client's code");
    proven_http_event_server_destroy(server);
    proven_loop_destroy(app.loop);
    proven_job_system_close(threads);
    proven_job_system_destroy(threads);
    return EXAMPLE_OK();
}
```

### Over HTTP/2 (RFC 8441)

With `http2_websocket` set (and `http2`), the server says so in its first HTTP/2 settings, and
a client that knows RFC 8441 opens its WebSocket as a stream of the HTTP/2
connection it already has, instead of a connection of its own. **The handler is the same**:
the request reaches `on_request` as CONNECT, with its path as the target and a `:protocol`
field first, `proven_ws_check_request` recognises it (with an empty key - there is none), and
`proven_ws_event_accept` answers 200 instead of 101. The program above enables it with one
line; its own client is HTTP/1.1, which the same server goes on serving.

- **Each such WebSocket holds a stream** - one of `max_concurrent_streams` (100 by default) -
  for as long as it is open. A connection full of WebSockets refuses new requests.
- Its frames travel in DATA frames, so HTTP/2's flow control is under the WebSocket's own:
  `http2_window_bytes` is what a client may send before the WebSocket has taken it (with 0, a
  window that grows while the WebSocket reads, as for any body), and
  `on_writable` comes when the stream's output has moved.
- Its end is END_STREAM from each side after the close handshake - until the client's arrives
  the stream keeps its place; a peer that resets the stream is a WebSocket closed with 1006 and
  `PROVEN_ERR_RESET`. A shutdown closes it with 1001. A handler that refuses the request (any
  answer but 2xx) ends the stream as it would a request's.
- Without `http2_websocket` a client must not try (the setting says so), and a stream that
  does is refused with PROTOCOL_ERROR.
- **Not here**: this library's own WebSocket client over HTTP/2 - `proven_ws_event_connect`
  makes a connection of its own, as before. Tested against Node's HTTP/2 client; no browser
  was tried.

### A connection this side makes

```c
proven_err_t proven_ws_event_connect(proven_loop_t *loop, const proven_ws_event_connect_options_t *options, proven_ws_stream_t **out);
```

A program on a loop is as often the one that asks. `proven_ws_event_connect` begins a
connection to a server and returns at once; what it gives back is a `proven_ws_stream_t`,
**the same type a server's connections are**. Every call of this section works on it as it
does on one of those, and its messages arrive through the same three callbacks: a handler
written for one side serves the other.

What differs is the beginning.

| | |
|---|---|
| `url` | `ws://` or `wss://`. Its host goes into `Host` and, for `wss`, is the name the certificate must be for |
| `address` | Where to connect. Needed unless the host is an IP address: **names are not resolved on a loop** (section 12 says why, and what to do) |
| `tls` | For `wss`. Without it a `wss` URL is refused - never connected in the clear instead |
| `headers`, `protocols` | Fields of your own (`Origin`, `Authorization`), and the subprotocols to offer |
| `connect_timeout_ms` | To be connected, secured and accepted. 0: 10 s |
| `on_open` | The server accepted. It is given the subprotocol the server chose |
| `ws` | A `proven_ws_event_config_t` - the callbacks, limits, pings and compression of the connection, as a server gives `proven_ws_event_accept`. `compress` here offers the extension |

**Nothing may be sent until the server has accepted**: the sending calls answer
`PROVEN_ERR_INVALID_STATE`, and `on_open` says when it has (sending from inside it is fine).
`proven_ws_stream_abort` ends an attempt that is still under way, and so does
`proven_ws_stream_close`.

**`on_closed` is called exactly once for every connection that was begun** - also for one
that never became a WebSocket. Its code is then 0, and `why` says what happened:

| `why`, with code 0 | What happened |
|---|---|
| `PROVEN_ERR_REFUSED` | Nothing listens there - or the server answered with anything but 101. No redirect is followed and no challenge answered |
| `PROVEN_ERR_TIMEOUT` | Not connected, secured and accepted within `connect_timeout_ms` |
| `PROVEN_ERR_INVALID_FORMAT` | The answer was not HTTP, or was a 101 that is not this handshake's: a wrong `Sec-WebSocket-Accept`, a subprotocol or an extension that was not offered |
| `PROVEN_ERR_RESET`, `PROVEN_ERR_UNREACHABLE`, a TLS error | The connection was lost, could not be made, or could not be secured |

A client's frames are masked, as the protocol requires of them; the keys come from a
generator seeded from the system when the connection is begun. Not here: a proxy,
reconnection (begin again), and WebSocket over HTTP/2.

This program has both ends on one loop. It is compiled and run by the test suite:

<!-- example: manual/examples/en/ex_15_ws_event_client.c -->
```c
#include <stdio.h>
#include <string.h>

/*
 * A WebSocket client on the loop: a connection that costs no thread on this side either.
 *
 * proven_ws_event_connect begins a connection and returns at once. What it gives back is a
 * proven_ws_stream_t - the type a server's connections are - so the calls and the callbacks
 * are the ones the server above uses. What is new is the beginning: on_open says when the
 * server has accepted, and until then nothing may be sent.
 *
 * One loop carries both ends here: the server that is asked, and the client that asks.
 */

typedef struct {
    proven_loop_t *loop;
    int opened, answers, closed, refused;
    proven_u16 code;
    char last[64];
} ex_app_t;

// ---- the server: it says what it was sent ----

static void served(void *ctx, proven_ws_stream_t *ws, bool text, proven_mem_view_t piece, bool first, bool last) {
    (void)ctx; (void)first;
    if (!last) return;                      /* this example's messages are small: one piece each */
    char reply[48];
    int n = snprintf(reply, sizeof reply, "%s, %u bytes", text ? "text" : "binary", (unsigned)piece.size);
    (void)proven_ws_stream_send(ws, true, (proven_mem_view_t){ (const proven_byte_t *)reply, (proven_size_t)n });
}

static void on_request(void *ctx, proven_http_stream_t *stream, const proven_http_request_t *head) {
    (void)ctx;
    proven_ws_event_config_t config = { .on = { .on_message = served } };
    proven_ws_stream_t *ws = NULL;
    if (proven_ws_event_accept(stream, head, &config, &ws) != PROVEN_OK) (void)proven_http_stream_respond(stream, 400, NULL, 0, (proven_mem_view_t){ 0 });
}

// ---- the client ----

/* The server accepted. From here on - and from inside this call - messages may be sent. */
static void on_open(void *ctx, proven_ws_stream_t *ws, proven_u8str_view_t protocol) {
    ex_app_t *app = ctx;
    (void)protocol;
    app->opened++;
    (void)proven_ws_stream_send(ws, true, proven_mem_view_from_u8(PROVEN_LIT("hello")));
}

/* The same callback a server has. This side asks twice and then says goodbye. */
static void on_message(void *ctx, proven_ws_stream_t *ws, bool text, proven_mem_view_t piece, bool first, bool last) {
    ex_app_t *app = ctx;
    (void)text; (void)first;
    if (!last) return;
    proven_size_t n = piece.size < sizeof app->last - 1 ? piece.size : sizeof app->last - 1;
    memcpy(app->last, piece.ptr, n);
    app->last[n] = '\0';
    printf("the server says: %s\n", app->last);
    if (++app->answers == 1) {
        static const proven_byte_t bytes[300] = { 1, 2, 3 };
        (void)proven_ws_stream_send(ws, false, (proven_mem_view_t){ bytes, sizeof bytes });
    } else {
        proven_ws_stream_close(ws, 1000, PROVEN_LIT("thank you"));
    }
}

/* Called exactly once for every connection that was begun - also for one that never opened:
 * its code is then 0, and `why` says what went wrong. */
static void on_closed(void *ctx, proven_ws_stream_t *ws, proven_u16 code, proven_err_t why) {
    ex_app_t *app = ctx;
    (void)ws;
    if (code == 0) {
        /* Never a WebSocket: nothing listened, the server answered something else, or time ran out. */
        printf("no connection: error %d\n", (int)why);
        app->refused++;
    } else {
        app->code = code;
        app->closed++;
    }
    /* Both connections of this program have ended: there is nothing left for the loop to do. */
    if (app->closed == 1 && app->refused == 1) proven_loop_stop(app->loop);
}

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();
    ex_app_t app = { 0 };
    EXAMPLE_REQUIRE(proven_loop_create(heap, &app.loop) == PROVEN_OK, "a loop");
    proven_http_event_server_config_t server_config = { .on = { .on_request = on_request } };
    proven_http_event_server_t *server = NULL;
    proven_net_addr_t at;
    EXAMPLE_REQUIRE(proven_http_event_server_create(app.loop, &server_config, &server) == PROVEN_OK, "a server");
    proven_err_t err = proven_http_event_server_listen(server, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &at);
    if (err == PROVEN_ERR_PERMISSION || err == PROVEN_ERR_UNSUPPORTED) {
        printf("no sockets in this environment; nothing to show\n");
        proven_http_event_server_destroy(server);
        proven_loop_destroy(app.loop);
        return EXAMPLE_OK();
    }
    EXAMPLE_REQUIRE(err == PROVEN_OK, "it listens");

    /* The host is an address, so none has to be given: names are not resolved on a loop. */
    char url[64];
    int n = snprintf(url, sizeof url, "ws://127.0.0.1:%u/chat", (unsigned)at.port);
    proven_ws_event_connect_options_t options = {
        .url = { (const proven_byte_t *)url, (proven_size_t)n },
        .on_open = on_open,
        .ws = { .on = { .on_message = on_message, .on_closed = on_closed }, .ctx = &app },
    };
    proven_ws_stream_t *ws = NULL;
    EXAMPLE_REQUIRE(proven_ws_event_connect(app.loop, &options, &ws) == PROVEN_OK && ws != NULL && app.opened == 0, "a connection is begun; nothing has happened yet");
    /* The call has returned and the connection is being made: this is too early to speak. */
    EXAMPLE_REQUIRE(proven_ws_stream_send(ws, true, proven_mem_view_from_u8(PROVEN_LIT("too early"))) == PROVEN_ERR_INVALID_STATE, "nothing can be sent before the server has accepted");

    /* And one that will fail, to see how that is told: a port that was just closed. */
    proven_net_listener_t unused;
    proven_net_addr_t nobody;
    EXAMPLE_REQUIRE(proven_net_listen(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), 1, &unused, &nobody) == PROVEN_OK && proven_net_listener_close(&unused) == PROVEN_OK, "a port nobody listens on");
    n = snprintf(url, sizeof url, "ws://127.0.0.1:%u/chat", (unsigned)nobody.port);
    options.url.size = (proven_size_t)n;
    proven_ws_stream_t *lost = NULL;
    err = proven_ws_event_connect(app.loop, &options, &lost);
    EXAMPLE_REQUIRE(err == PROVEN_OK || err == PROVEN_ERR_REFUSED, "an attempt that cannot succeed: refused now, or told later");
    if (err != PROVEN_OK) app.refused++;

    /* Everything else happens here. */
    if (!(app.closed == 1 && app.refused == 1)) EXAMPLE_REQUIRE(proven_loop_run(app.loop) == PROVEN_OK, "the loop runs until both have ended");
    EXAMPLE_REQUIRE(app.opened == 1 && app.answers == 2 && strcmp(app.last, "binary, 300 bytes") == 0, "it opened once, and two messages were answered");
    EXAMPLE_REQUIRE(app.closed == 1 && app.code == 1000 && app.refused == 1, "it closed once, with this side's code - and the other attempt was reported once");

    proven_http_event_server_destroy(server);
    proven_loop_destroy(app.loop);
    return EXAMPLE_OK();
}
```

**Two threads, one connection.** A program in which one thread waits for messages and another
decides what to send - a chat client, a feed with a control channel - cannot do that on a
blocking connection (Chapter 12 says why). It can on this one: the loop waits for messages
without a thread of its own, and the other thread hands it what to send with
`proven_loop_post`. The send runs on the loop's thread, the only one that touches the
connection; what crosses is a copy of the message, in memory the posted call frees. This
program is compiled and run by the test suite:

<!-- example: manual/examples/en/ex_15_ws_duplex.c -->
```c
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * One thread waits for messages while another decides what to send - a chat client's shape.
 *
 * A blocking connection (Chapter 12) cannot do it: it is one thread's, and a lock around
 * proven_ws_conn_receive would hold every send for as long as the receive waits. The loop's
 * connection can. It waits for messages without a thread of its own, and another thread hands
 * it what to send with proven_loop_post: the send itself happens on the loop's thread, the
 * only one that touches the connection.
 *
 * One loop carries both ends here: a server that answers each message, and the client.
 */

#define LINES 3

typedef struct {
    proven_loop_t *loop;
    proven_ws_stream_t *ws;           /* the client's connection: the loop's thread only */
    atomic_bool open;                 /* what the typing thread may read */
    int answers, closed;
} ex_app_t;

// ---- the server: it answers each message ------------------------------------

static void served(void *ctx, proven_ws_stream_t *ws, bool text, proven_mem_view_t piece, bool first, bool last) {
    (void)ctx; (void)text; (void)first;
    if (!last) return;                      /* this example's messages are small: one piece each */
    char reply[80];
    int n = snprintf(reply, sizeof reply, "got: %.*s", (int)piece.size, (const char *)piece.ptr);
    (void)proven_ws_stream_send(ws, true, (proven_mem_view_t){ (const proven_byte_t *)reply, (proven_size_t)n });
}

static void on_request(void *ctx, proven_http_stream_t *stream, const proven_http_request_t *head) {
    (void)ctx;
    proven_ws_event_config_t config = { .on = { .on_message = served } };
    proven_ws_stream_t *ws = NULL;
    if (proven_ws_event_accept(stream, head, &config, &ws) != PROVEN_OK) (void)proven_http_stream_respond(stream, 400, NULL, 0, (proven_mem_view_t){ 0 });
}

// ---- the client, on the loop's thread -----------------------------------------

static void on_open(void *ctx, proven_ws_stream_t *ws, proven_u8str_view_t protocol) {
    ex_app_t *app = ctx;
    (void)protocol;
    app->ws = ws;
    atomic_store(&app->open, true);
}

static void on_message(void *ctx, proven_ws_stream_t *ws, bool text, proven_mem_view_t piece, bool first, bool last) {
    ex_app_t *app = ctx;
    (void)text; (void)first;
    if (!last) return;
    printf("the server says: %.*s\n", (int)piece.size, (const char *)piece.ptr);
    if (++app->answers == LINES) proven_ws_stream_close(ws, 1000, PROVEN_LIT("bye"));
}

static void on_closed(void *ctx, proven_ws_stream_t *ws, proven_u16 code, proven_err_t why) {
    ex_app_t *app = ctx;
    (void)ws; (void)code; (void)why;
    app->ws = NULL;
    app->closed++;
    proven_loop_stop(app->loop);
}

// ---- what crosses from the typing thread ---------------------------------------

/* A line to send, in memory of its own: the typing thread has moved on by the time it is sent. */
typedef struct { ex_app_t *app; proven_size_t len; char text[64]; } ex_line_t;

/* On the loop's thread. */
static void send_here(void *ctx) {
    ex_line_t *line = ctx;
    if (line->app->ws) (void)proven_ws_stream_send(line->app->ws, true, (proven_mem_view_t){ (const proven_byte_t *)line->text, line->len });
    free(line);
}

/* The typing thread. It never touches the connection: it posts. */
static void typist(void *arg) {
    ex_app_t *app = arg;
    static const char *const lines[LINES] = { "hello", "how are you", "goodbye" };
    while (!atomic_load(&app->open)) proven_time_sleep(1);
    for (int i = 0; i < LINES; ++i) {
        ex_line_t *line = malloc(sizeof *line);
        if (!line) return;
        line->app = app;
        line->len = strlen(lines[i]);
        memcpy(line->text, lines[i], line->len);
        if (proven_loop_post(app->loop, send_here, line) != PROVEN_OK) free(line);
        proven_time_sleep(5);               /* someone typing */
    }
}

static void run(void *arg) { (void)proven_loop_run(((ex_app_t *)arg)->loop); }

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();
    static ex_app_t app;
    atomic_init(&app.open, false);
    EXAMPLE_REQUIRE(proven_loop_create(heap, &app.loop) == PROVEN_OK, "a loop");
    proven_http_event_server_config_t server_config = { .on = { .on_request = on_request } };
    proven_http_event_server_t *server = NULL;
    proven_net_addr_t at;
    EXAMPLE_REQUIRE(proven_http_event_server_create(app.loop, &server_config, &server) == PROVEN_OK, "a server");
    proven_err_t err = proven_http_event_server_listen(server, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &at);
    if (err == PROVEN_ERR_PERMISSION || err == PROVEN_ERR_UNSUPPORTED) {
        printf("no sockets in this environment; nothing to show\n");
        proven_http_event_server_destroy(server);
        proven_loop_destroy(app.loop);
        return EXAMPLE_OK();
    }
    EXAMPLE_REQUIRE(err == PROVEN_OK, "it listens");

    char url[64];
    int n = snprintf(url, sizeof url, "ws://127.0.0.1:%u/chat", (unsigned)at.port);
    proven_ws_event_connect_options_t options = {
        .url = { (const proven_byte_t *)url, (proven_size_t)n },
        .on_open = on_open,
        .ws = { .on = { .on_message = on_message, .on_closed = on_closed }, .ctx = &app },
    };
    proven_ws_stream_t *ws = NULL;
    EXAMPLE_REQUIRE(proven_ws_event_connect(app.loop, &options, &ws) == PROVEN_OK, "a connection is begun");

    // ---- two threads: the loop, and someone typing --------------------------------
    proven_job_sys_t *threads = NULL;
    proven_job_group_t looping, typing;
    proven_job_group_init(&looping);
    proven_job_group_init(&typing);
    EXAMPLE_REQUIRE(proven_job_system_init(heap, 2, 4, &threads) == PROVEN_OK &&
                    proven_job_group_submit(threads, &looping, run, &app) == PROVEN_OK &&
                    proven_job_group_submit(threads, &typing, typist, &app) == PROVEN_OK, "the loop on one thread, the typist on another");

    /* The loop stops when the connection has closed, after the third answer. */
    proven_job_group_wait(threads, &typing);
    proven_job_group_wait(threads, &looping);
    EXAMPLE_REQUIRE(app.answers == LINES && app.closed == 1, "three lines sent from the typing thread, three answers received on the loop's");

    /* Anything posted and not yet run is run here, before the loop goes. */
    EXAMPLE_REQUIRE(proven_loop_poll(app.loop, PROVEN_NET_DONT_WAIT) == PROVEN_OK, "a last round");
    proven_job_system_close(threads);
    proven_job_system_destroy(threads);
    proven_http_event_server_destroy(server);
    proven_loop_destroy(app.loop);
    return EXAMPLE_OK();
}
```

## 12. The event-driven client

```c
proven_err_t proven_http_event_client_create(proven_loop_t *loop, const proven_http_event_client_config_t *config,
                                             proven_http_event_client_t **out);
void proven_http_event_client_destroy(proven_http_event_client_t *client);
proven_size_t proven_http_event_client_requests(const proven_http_event_client_t *client);
proven_err_t proven_http_event_client_start(proven_http_event_client_t *client, const proven_http_event_request_options_t *options,
                                            proven_http_event_request_t **out);
proven_result_size_t proven_http_event_request_write(proven_http_event_request_t *request, proven_mem_view_t data);
proven_err_t proven_http_event_request_end(proven_http_event_request_t *request);
void proven_http_event_request_abort(proven_http_event_request_t *request);
void proven_http_event_request_pause(proven_http_event_request_t *request);
void proven_http_event_request_resume(proven_http_event_request_t *request);
void proven_http_event_request_set_user(proven_http_event_request_t *request, void *user);
void *proven_http_event_request_user(const proven_http_event_request_t *request);
```

[Chapter 11](manual-11-http-client-server.md)'s client sends a request and waits for the
answer. This one starts a request and returns; the response head, the pieces of its body and
the end arrive through callbacks on the loop's thread, and any number of requests are in
flight at once.

**It is deliberately narrow, and the two limits are worth knowing before anything else.**

- **A request is one exchange and nothing around it.** There are no redirects, answers to
  challenges, cookies or proxies. Chapter 11's client has all of them; use it wherever a
  thread per request in flight is affordable.
- **Names are not resolved.** Looking a name up can take seconds, and nothing on a loop may
  wait. A request says where to connect with `address`; only a URL whose host is an IP address
  needs none. For a name: call `proven_net_resolve` on a job, post the result to the loop, and
  start the request there, as the program at the end of this section does. The URL keeps the
  name - it is what goes into `Host`, and over TLS it is the name the server's certificate
  must be for.

A request is described by a `proven_http_event_request_options_t`. Everything it points to is
copied or used before `proven_http_event_client_start` returns:

| Field | Meaning |
|---|---|
| `method` | Empty means `GET`. `CONNECT` is refused. |
| `url` | Absolute, `http` or `https`, with no credentials in it. |
| `address` | Where to connect; its port is used as given. Null only when the URL's host is an IP address. |
| `headers`, `header_count` | Yours. `Host`, `Content-Length`, `Transfer-Encoding` and `Connection` are written for you and may not be among them. |
| `body` | A body held in memory, sent whole. |
| `body_length` | With no `body`: 0 for none; a length for a body written afterwards; `PROVEN_HTTP_EVENT_LENGTH_UNKNOWN` to write it chunked. |
| `on.on_response(ctx, request, head)` | The response head. Optional. Interim responses (100, 103) are passed over and not reported. |
| `on.on_body(ctx, request, piece, last)` | A piece of the body; `last` with the final one, which may be empty. A response with no body gets no call. Optional. |
| `on.on_writable(ctx, request)` | Request-body bytes that were held back have gone. Optional. |
| `on.on_done(ctx, request, why)` | The end. **Required**, and called exactly once for every request that was started. |
| `ctx` | Passed to every callback. |

**Connections are kept, and counted.** Since v0.36.0 an HTTP/1.1 request does not make a
connection of its own and close it. The client keeps the connections of each **origin** - the
URL's scheme, host and port, and the address connected to - and a request takes one of them:

- **one that is idle**, if its origin has one. A connection is kept when an exchange on it
  ended cleanly: the response was HTTP/1.1 without `Connection: close`, its body had an end
  of its own (a length, or chunks) and that end was reached, the request had been sent
  whole, and nothing came after the response. At most `max_kept_connections` (8) are kept
  in the whole client, the one idle longest making room for another, and none for longer
  than `idle_timeout_ms` (60 s);
- **or a new one**, while the origin has fewer than `max_connections_per_origin` (6), busy
  and idle together;
- **or it waits**, in the order of starting, for the next connection of its origin that comes
  free. **This is the part to know: the seventh request in flight to one origin is not sent
  until one of the six has ended.** A program that keeps more than six requests open to one
  server on purpose - long polls, streams that do not end - must raise
  `max_connections_per_origin`, or its seventh request waits and, after
  `connect_timeout_ms`, ends with `PROVEN_ERR_TIMEOUT`: waiting is on the connecting clock,
  which starts with the request. At most `max_waiting_requests` (1,000) wait in a client;
  the next `start` is `PROVEN_ERR_BUSY`. A waiting request holds its head and its body, can
  be written to (up to `max_buffered_output`), and can be aborted.

**A kept connection may be dead by the time it is used**: servers close idle connections
without asking, and the close can cross the next request on the wire. So when a request is
lost on a connection that had carried one before, and nothing at all had come back on it,
the client takes it that the server never saw the request, and **sends it once more, on a
new connection, without telling you** - one `on_done`, for whichever sending ends it. A
request whose body is written in pieces cannot be sent again, since the client does not
have the pieces; it ends with `PROVEN_ERR_AGAIN`, which means what it means for HTTP/2: not
acted on, and yours to start again. Once any byte of an answer has arrived, or on a
connection made for the request, a loss is `PROVEN_ERR_RESET` as it always was. (The reading
"never saw it" can be wrong - a server may take a request and die before it answers. It is
the reading every HTTP client that reuses connections makes, the blocking client of
Chapter 11 among them; `no_connection_reuse` is for a program that will not have it.)

`retry_unsent` - a number, zero by default - has the client begin a request again by itself
when the server cannot have acted on it. Over HTTP/1.1 that is one case, and a narrow one: a
connection that was made and then lost before any of the request had been given to it, which
in practice is a server that goes away while TLS is being agreed. Such a request ends with
`PROVEN_ERR_RESET` without the setting, and with it is begun on a new connection, so many
times, each with its own `connect_timeout_ms`. A request of which anything was sent is not
begun again, and neither is one whose body is written in pieces. (The sending-again on a
kept connection, above, needs no setting and is not counted. Over HTTP/2 the setting covers
more: Chapter 17.)

`retry_delay_ms` is how long the client waits before each such try, in either protocol: zero
by default, and the same for every try unless `retry_delay_max_ms` is set - then the wait
doubles with each try of a request, from `retry_delay_ms` up to that bound. Nothing is added
at random: requests that were refused together are tried again together. While it waits the request has no
connection - the one it lost is closed, and its place may go to a request that was waiting -
and `connect_timeout_ms` of the try begins when the wait ends. Aborting the request during
the wait ends it as any abort does. The sending-again on a kept connection is not delayed.
(With `max_connections`, below: when the wait is over the request takes a place as a new one
would, waiting for one if there is none; if as many wait already as may, it ends with
`PROVEN_ERR_BUSY` - the one case in which `on_done` says so, and not
`proven_http_event_client_start`.)

`max_connections` bounds the connections of the whole client, of both protocols, busy and
idle; zero, the default, is no bound. A request that would make one more first has an idle
connection closed for it - an HTTP/1.1 one, the one idle longest, before an HTTP/2 one with
nothing on it. With none idle the request waits, whichever its protocol. An HTTP/1.1 request
waits in the queue described above, and has the next place that is freed (a connection that
closes serves a request to its own origin first, then the one that has waited longest and
whose origin has room). An HTTP/2 request whose origin has no connection waits on no
connection at all, in the order of starting, and is put on one when a place can be had - or
on its origin's, if another request has made one meanwhile. Either wait is the wait for a
connection: `connect_timeout_ms` ends it with `PROVEN_ERR_TIMEOUT`, and no more requests
wait than `max_waiting_requests` (the next start is `PROVEN_ERR_BUSY`). When both protocols
wait, a place goes to an HTTP/1.1 request first. Setting `max_kept_connections` above
`max_connections` is `PROVEN_ERR_INVALID_ARG`.

`no_connection_reuse` gives back the old behaviour for connections - `Connection: close` in
every request, every connection closed when its response has ended - but not the old
absence of a limit: six at a time to an origin still.

**Starting.** When `proven_http_event_client_start` returns an error, the request was not
started and no callback is or will be made. When it returns `PROVEN_OK`, `on_done` will be
called exactly once - **never inside that call**, so code that starts a request can finish
what it was doing before anything happens to it. A request is valid from the start until
`on_done` returns.

| `start` returns | For |
|---|---|
| `PROVEN_ERR_INVALID_ARG` | No `on_done`; a URL that is not absolute `http` or `https`, or that carries credentials; `https` with no TLS configuration - refused, never sent in the clear; a name with no `address`; a header the client writes itself; a method that is not a token, or `CONNECT`. |
| `PROVEN_ERR_OUT_OF_BOUNDS` | A request head that does not fit `max_head_bytes`. |
| `PROVEN_ERR_NOMEM` | |
| `PROVEN_ERR_REFUSED`, `PROVEN_ERR_UNREACHABLE` | The connect failed at once. It may also fail later, and is then reported by `on_done`. |

**How it ends.** `on_done` says:

| `why` | What happened |
|---|---|
| `PROVEN_OK` | The whole response arrived - whatever its status: a 404 is a response. |
| `PROVEN_ERR_REFUSED`, `PROVEN_ERR_UNREACHABLE` | Nothing was listening, or there was no route. |
| `PROVEN_ERR_TIMEOUT` | One of the four time limits passed. |
| `PROVEN_ERR_RESET` | The server closed or was lost before the response was whole - a body cut short of its length is never reported as complete; or `proven_http_event_request_abort`; or the client was destroyed. |
| `PROVEN_ERR_INVALID_FORMAT` | The answer was not HTTP, its framing contradicted itself, or more than eight interim responses came in a row. |
| `PROVEN_ERR_OUT_OF_BOUNDS` | The head was larger than `max_head_bytes`, or the body larger than `max_body_bytes`. |
| `PROVEN_ERR_UNSUPPORTED` | The server answered 101: this client asks for no change of protocol. |
| `PROVEN_ERR_UNTRUSTED`, `PROVEN_ERR_NAME_MISMATCH`, `PROVEN_ERR_EXPIRED`, ... | TLS refused the server ([Chapter 14](manual-14-tls.md), section 8). The request was never sent. |

**A request body written as it goes** follows the rule of section 7.
`proven_http_event_request_write` takes as many bytes as fit under `max_buffered_output`,
possibly none, and says how many; the rest is offered again when `on_writable` is called. It
may be called at once after `start`, before the connection exists: what is taken waits.
`proven_http_event_request_end` finishes the body; ending short of a promised length ends the
request with `PROVEN_ERR_INVALID_FORMAT`.

| Call | Returns |
|---|---|
| `proven_http_event_client_create(loop, &config, &client)` | `proven_err_t`: `INVALID_ARG` for no loop; `NOMEM`. `config` may be null. |
| `proven_http_event_client_requests(client)` | `proven_size_t`: requests started and not ended - those waiting for a connection among them. |
| `proven_http_event_client_destroy(client)` | nothing. Every request, in flight or waiting, gets `on_done(PROVEN_ERR_RESET)`, and the kept connections are closed. Connections whose sockets the loop may still use keep their memory until the loop's next round or `proven_loop_destroy` (section 5, rule 3). Null is accepted. |
| `proven_http_event_request_write(request, data)` | `proven_result_size_t`: the bytes taken. `INVALID_STATE` when the request has no body to write or it was ended; `OUT_OF_BOUNDS` for more than the length promised. |
| `proven_http_event_request_end(request)` | `proven_err_t`: `INVALID_STATE` when there was nothing to end; `INVALID_FORMAT` when fewer bytes were written than promised - `on_done` is then called inside the call. |
| `proven_http_event_request_abort(request)` | nothing. `on_done(PROVEN_ERR_RESET)` is called inside the call. |
| `proven_http_event_request_pause`, `proven_http_event_request_resume` | nothing. Stop and restart the delivery of the response body. |

The configuration's fields, each with a default when zero:

| Field | Meaning | Default |
|---|---|---|
| `alloc` | The allocator for requests and buffers. | the loop's |
| `tls` | A TLS configuration that can verify servers, for `https`. Must outlive the client. A second connection to an origin resumes the TLS session the last one learned (since v0.72.0, RFC-0020): the client keeps the sessions of 64 origins for TCP and 64 for QUIC, the least recently used given up and wiped, and never offers TCP's over QUIC; a server that refuses the ticket is answered by a full handshake. The configuration's `no_resumption` turns it off - for a client that must check the certificate every time, or must not be linked from one connection to the next. A session is 2,048 bytes (since v0.73.0), so the sessions kept take at most 256 KiB. Its `early_data` (since v0.73.0 over HTTP/3, RFC-0021; since v0.84.0 over HTTP/1.1 and HTTP/2, RFC-0027) sends GET, HEAD and OPTIONS requests without a body as early data on a connection that resumes - one round trip sooner - and sends a request refused that way, or answered 425, again once after the handshake: [Chapter 14, section 6](manual-14-tls.md#6-resumption). | none: `https` is refused |
| `max_head_bytes` | Largest response head, and request head. | 16 KiB |
| `max_headers` | Most header fields in a response. | 64 |
| `max_body_bytes` | Largest response body. | no limit: it is delivered in pieces and never held |
| `max_buffered_output` | Request-body bytes held before writes are refused. | 64 KiB |
| `connect_timeout_ms` | To be connected, the TLS handshake included - and the wait for a connection, when the origin has as many as it may. It runs from the start of the request; a request that is sent again gets it anew. | 10 s |
| `response_timeout_ms` | From the request being sent to the end of the response head. | 30 s |
| `body_timeout_ms` | Between pieces of the response body. | 30 s |
| `write_timeout_ms` | Output held with none of it accepted by the server. | 30 s |
| `decompress` | Ask for gzip and decode it: `on_body` gets decoded pieces, and `max_body_bytes` counts them. [Chapter 16](manual-16-compression.md), section 6. | off |
| `http2`, `http2_cleartext` | Speak HTTP/2 to origins that agree to it (ALPN), or to every `http` origin: one connection kept for each, shared by its requests. [Chapter 17, section 7](manual-17-http2.md#7-http2-in-the-event-driven-client). | off |
| `idle_timeout_ms` | How long a connection with no request on it is kept, in either protocol. | 60 s |
| `max_waiting_requests` | Requests that may wait: for an HTTP/2 connection, on that connection; for an HTTP/1.1 connection, in the whole client. The next `start` is `PROVEN_ERR_BUSY`. | 1,000 |
| `max_connections_per_origin` | HTTP/1.1: connections to one origin at a time, busy and idle together; a request beyond that waits. | 6 |
| `max_kept_connections` | HTTP/1.1: idle connections kept in the whole client. Zero is the default, as every zero in this table is; `no_connection_reuse` is what keeps none. (It was `max_idle_connections` in v0.36.0 - the name of a field of chapter 11's client, where 0 keeps none. The two no longer share a name.) | 8 |
| `no_connection_reuse` | HTTP/1.1: `Connection: close` in every request, and no connection kept. | off |
| `retry_unsent` | How many times the client itself puts again a request the server did not act on (HTTP/2: `PROVEN_ERR_AGAIN`; HTTP/1.1: lost before any of it was sent), at once and without telling you. Never a body written in pieces, never after `on_response`. | 0: never |
| `retry_delay_ms` | The wait before each try `retry_unsent` makes, in either protocol; `connect_timeout_ms` of the try begins when it ends. | 0: none |
| `retry_delay_max_ms` | When set, the wait doubles with each try of a request, from `retry_delay_ms` up to this. Below `retry_delay_ms`, or with no `retry_delay_ms`, is `INVALID_ARG`. | 0: the same wait every time |
| `max_connections` | Connections of the whole client, both protocols, busy and idle. At the limit an idle one is closed for a new request; with none idle the request waits, in either protocol, for `connect_timeout_ms` at most. | 0: no limit |
| `http2_window_bytes` | HTTP/2: response-body bytes a server may send on a stream, and on a connection as a whole, before `on_body` has taken them. What a paused request can make a connection hold; a large download waits for room each time the server has sent this much (sixteen responses of 1 MiB at once, measured before v0.75.0: 568 a second at the default, 1,208 with 1 MiB). With 0 the window starts there and grows while bodies are read - doubling while half of it is taken within two round trips, up to 16 MiB a stream and 24 MiB a connection (v0.75.0, as HTTP/3's since v0.71.0); a body not read stops the growth. A value set stays fixed. One download of 64 MiB over a relay with 10 ms each way: 16 Mbit/s with the window fixed at 65,535, 1,310 growing. | 65,535, growing |
| `http3` | A request that asks for it (`http3` in its options, an `https` URL) goes over HTTP/3, on one QUIC connection kept for its origin: [Chapter 17, section 10](manual-17-http2.md#10-http3-in-the-event-driven-client). Never one that does not ask, and never by following `Alt-Svc`. **No fallback**: no answer over UDP is `PROVEN_ERR_TIMEOUT` after `connect_timeout_ms`. Needs `tls`, offering `h3`. | false |
| `http3_window_bytes` | HTTP/3: response-body bytes a server may send on a stream, and on a connection as a whole, before `on_body` has taken them (QUIC's flow control). | 256 KiB a stream and 1 MiB a connection, growing while bodies are read as fast as they come, up to 16 MiB and 24 MiB; a value set is fixed |

The test suite compiles and runs this program:

<!-- example: manual/examples/en/ex_15_http_event_client.c -->
```c
#include <stdio.h>
#include <string.h>

/*
 * The event-driven HTTP client: several requests in flight on one thread.
 *
 * A request is started and the call returns at once; what happens to it arrives through
 * callbacks, on the loop's thread. The server here is the event-driven one of this chapter, on
 * the same loop - this whole program is one thread and one worker for the one thing that waits.
 *
 * That one thing is looking a name up. The client does not resolve names: a lookup can take
 * seconds, and nothing on a loop may wait. So the third request shows the pattern - resolve on
 * a worker, post the answer back, start the request.
 */

typedef struct app ex_app_t;

/* What this program keeps for one request. */
typedef struct {
    ex_app_t *app;
    const char *path;
    int status;
    char body[64];
    proven_size_t len;
    proven_err_t why;
    bool done;
} ex_fetch_t;

struct app {
    proven_loop_t *loop;
    proven_http_event_client_t *client;
    proven_job_sys_t *workers;
    proven_u16 port;
    ex_fetch_t fetch[3];
    int finished;
    /* written by the worker, read on the loop after it has been posted */
    proven_net_addr_t found;
    proven_err_t lookup;
};

// ---- the server the requests go to -----------------------------------------

static void serve(void *ctx, proven_http_stream_t *stream, const proven_http_request_t *head) {
    (void)ctx;
    if (proven_u8str_view_eq(head->target, PROVEN_LIT("/a"))) (void)proven_http_stream_respond(stream, 200, NULL, 0, proven_mem_view_from_u8(PROVEN_LIT("first")));
    else if (proven_u8str_view_eq(head->target, PROVEN_LIT("/b"))) (void)proven_http_stream_respond(stream, 200, NULL, 0, proven_mem_view_from_u8(PROVEN_LIT("second")));
    else (void)proven_http_stream_respond(stream, 404, NULL, 0, proven_mem_view_from_u8(PROVEN_LIT("nothing here")));
}

// ---- the client's callbacks -------------------------------------------------

/* The head: good only until this function returns, so take what is wanted from it. */
static void on_response(void *ctx, proven_http_event_request_t *request, const proven_http_response_t *head) {
    (void)request;
    ((ex_fetch_t *)ctx)->status = head->status;
}

/* The body, a piece at a time. Nothing is collected for you; collect what you want. */
static void on_body(void *ctx, proven_http_event_request_t *request, proven_mem_view_t piece, bool last) {
    (void)request; (void)last;
    ex_fetch_t *f = ctx;
    for (proven_size_t i = 0; i < piece.size && f->len < sizeof f->body - 1; ++i) f->body[f->len++] = (char)piece.ptr[i];
}

/* Once for every request that was started, whatever happened to it. */
static void on_done(void *ctx, proven_http_event_request_t *request, proven_err_t why) {
    (void)request;
    ex_fetch_t *f = ctx;
    f->why = why;
    f->done = true;
    if (++f->app->finished == 3) proven_loop_stop(f->app->loop);
}

static proven_err_t fetch(ex_app_t *app, ex_fetch_t *f, const char *path, const proven_net_addr_t *address, const char *host) {
    char url[96];
    int n = snprintf(url, sizeof url, "http://%s:%u%s", host, (unsigned)app->port, path);
    f->app = app;
    f->path = path;
    proven_http_event_request_options_t options = {
        .url = { (const proven_byte_t *)url, (proven_size_t)n },
        .address = address,                               /* null is allowed only when the URL's host is an IP address */
        .on = { on_response, on_body, NULL, on_done },
        .ctx = f,
    };
    return proven_http_event_client_start(app->client, &options, NULL);     /* returns at once */
}

// ---- a name, resolved elsewhere ---------------------------------------------

/* On the loop's thread again: now there is an address, and the request can start.
 * The URL keeps the name - it is what goes into Host - and the address says where to connect. */
static void resolved(void *ctx) {
    ex_app_t *app = ctx;
    if (app->lookup != PROVEN_OK || fetch(app, &app->fetch[2], "/missing", &app->found, "localhost") != PROVEN_OK) {
        app->fetch[2].why = PROVEN_ERR_NOT_FOUND;
        if (++app->finished == 3) proven_loop_stop(app->loop);
    }
}

/* On a worker thread: this call may wait as long as the resolver takes. */
static void resolve(void *ctx) {
    ex_app_t *app = ctx;
    proven_net_addr_t all[8];
    proven_size_t count = 0;
    app->lookup = proven_net_resolve(PROVEN_LIT("localhost"), app->port, all, 8, &count);
    /* The server here listens on IPv4 only; a real program would try each address in turn. */
    bool have = false;
    for (proven_size_t i = 0; i < count && !have; ++i) {
        if (all[i].family == PROVEN_NET_FAMILY_IPV4) { app->found = all[i]; have = true; }
    }
    if (app->lookup == PROVEN_OK && !have) app->lookup = PROVEN_ERR_NOT_FOUND;
    if (proven_loop_post(app->loop, resolved, app) != PROVEN_OK) proven_loop_stop(app->loop);
}

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();
    ex_app_t app = { 0 };

    // ---- one loop, a server and a client on it ------------------------------------
    /* Nothing runs yet: a loop does nothing until it is run. */
    EXAMPLE_REQUIRE(proven_loop_create(heap, &app.loop) == PROVEN_OK, "a loop");
    proven_http_event_server_config_t server_config = { .on = { .on_request = serve } };
    proven_http_event_server_t *server = NULL;
    proven_net_addr_t at;
    EXAMPLE_REQUIRE(proven_http_event_server_create(app.loop, &server_config, &server) == PROVEN_OK &&
                    proven_http_event_server_listen(server, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &at) == PROVEN_OK, "a server to ask");
    app.port = at.port;
    EXAMPLE_REQUIRE(proven_http_event_client_create(app.loop, NULL, &app.client) == PROVEN_OK, "an event-driven client");
    EXAMPLE_REQUIRE(proven_job_system_init(heap, 1, 4, &app.workers) == PROVEN_OK, "one worker, for the lookup");

    // ---- three requests ------------------------------------------------------------
    /* Two by address. Both are started before either has been sent. */
    EXAMPLE_REQUIRE(fetch(&app, &app.fetch[0], "/a", NULL, "127.0.0.1") == PROVEN_OK &&
                    fetch(&app, &app.fetch[1], "/b", NULL, "127.0.0.1") == PROVEN_OK, "two requests started");
    EXAMPLE_REQUIRE(proven_http_event_client_requests(app.client) == 2 && !app.fetch[0].done && !app.fetch[1].done, "both in flight, neither answered: nothing has run yet");

    /* One by name. Without an address it is refused - so resolve first, on the worker. */
    ex_fetch_t unresolved = { 0 };
    EXAMPLE_REQUIRE(fetch(&app, &unresolved, "/missing", NULL, "localhost") == PROVEN_ERR_INVALID_ARG, "a name with no address is refused");
    EXAMPLE_REQUIRE(proven_job_submit(app.workers, resolve, &app), "the lookup is handed to a worker");

    // ---- run -----------------------------------------------------------------------
    /* on_done stops the loop when the third request has ended. */
    EXAMPLE_REQUIRE(proven_loop_run(app.loop) == PROVEN_OK, "the loop ran until all three were done");

    EXAMPLE_REQUIRE(app.fetch[0].why == PROVEN_OK && app.fetch[0].status == 200 && strcmp(app.fetch[0].body, "first") == 0, "the first answer");
    EXAMPLE_REQUIRE(app.fetch[1].why == PROVEN_OK && app.fetch[1].status == 200 && strcmp(app.fetch[1].body, "second") == 0, "the second answer");
    EXAMPLE_REQUIRE(app.fetch[2].why == PROVEN_OK && app.fetch[2].status == 404 && strcmp(app.fetch[2].body, "nothing here") == 0, "the third, by name: a 404 is a response, not an error");

    proven_job_system_close(app.workers);
    proven_job_system_destroy(app.workers);
    proven_http_event_client_destroy(app.client);
    proven_http_event_server_destroy(server);
    proven_loop_destroy(app.loop);
    return EXAMPLE_OK();
}
```

## 13. Several loops

One loop is one thread, and a connection belongs to one loop for its whole life: nothing about
it is ever touched by another thread, which is why nothing in this chapter takes a lock. A
server that needs more than one processor core is therefore **several loops, each with a
server of its own** - and something to share the incoming connections among them.

```c
proven_err_t proven_http_event_server_adopt(proven_http_event_server_t *server, proven_net_conn_t *conn);
```

`proven_http_event_server_adopt` gives a server a connection that was accepted somewhere
else. It becomes one of the server's exactly as if the server had accepted it; with TLS, the
handshake begins. So one thread listens and accepts, and deals each connection to the servers
in turn. Three rules make that correct:

- **`adopt` is called on the server's loop thread**, like everything else. The accepting
  thread does not call it: it posts a function to that loop (`proven_loop_post`), and the
  function calls it.
- **The connection travels in memory of its own.** A `proven_net_conn_t` is not to be copied
  while it is open, so what is posted is a small block holding it, freed by the function that
  adopts.
- **Taken apart in order:** the accepting thread first, so that nothing more is posted; then
  the loops; then - after one last round of each loop, for anything posted and not yet run -
  the servers.

| `adopt` returns | Meaning |
|---|---|
| `PROVEN_OK` | The server owns the connection; the caller's value is no longer open. |
| `PROVEN_ERR_BUSY` | The server is at `max_connections`. The connection is still the caller's, to close. |
| `PROVEN_ERR_INVALID_STATE` | The connection is not open, or the server was told `proven_http_event_server_stop_listening`. Still the caller's. |
| `PROVEN_ERR_INVALID_ARG`, `PROVEN_ERR_NOMEM` | Still the caller's. |

The other way to the same end needs no call at all: give each loop's server a listener of its
own on a different address or port, and let whatever is in front - a load balancer, or DNS -
spread the clients. The load measurements of the next section were made that way.

Whether more loops help depends on what the time goes on. They multiply what handlers can
compute; they do nothing for a handler that waits, which should not be on a loop at all
(section 10).

The test suite compiles and runs this program:

<!-- example: manual/examples/en/ex_15_loops.c -->
```c
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Several loops behind one port: one thread accepts, and deals connections to servers on
 * loops of their own.
 *
 * A connection belongs to one loop, and nothing about it is ever touched by another thread.
 * So scaling across processors is several loops, each with its own server - and something has
 * to share the incoming connections among them. Here that is a thread that accepts and deals
 * them out in turn, which works wherever the library does.
 */

enum { LOOPS = 2 };

/* One loop, its server, and what its handler counts. Only that loop's thread touches it. */
typedef struct {
    int index;
    proven_loop_t *loop;
    proven_http_event_server_t *server;
    int served;                       /* read by the main thread only after the loop has stopped */
} ex_worker_t;

static ex_worker_t g_workers[LOOPS];
static proven_net_listener_t g_listener;
static atomic_bool g_stop;

static void on_request(void *ctx, proven_http_stream_t *stream, const proven_http_request_t *head) {
    ex_worker_t *me = ctx;
    (void)head;
    char text[16];
    int n = snprintf(text, sizeof text, "loop %d", me->index);
    me->served++;
    (void)proven_http_stream_respond(stream, 200, NULL, 0, (proven_mem_view_t){ (const proven_byte_t *)text, (proven_size_t)n });
}

/* What crosses from the accepting thread to a loop. An open connection is not to be copied,
 * so it travels in memory of its own. */
typedef struct { ex_worker_t *to; proven_net_conn_t conn; } ex_handoff_t;

/* On the loop's thread - the only place a server may be given a connection. */
static void adopt_here(void *ctx) {
    ex_handoff_t *h = ctx;
    if (proven_http_event_server_adopt(h->to->server, &h->conn) != PROVEN_OK) (void)proven_net_close(&h->conn);   /* refused: still ours to close */
    free(h);
}

/* The accepting thread. It touches no server: it accepts, chooses, and posts. */
static void acceptor(void *arg) {
    (void)arg;
    int next = 0;
    while (!atomic_load(&g_stop)) {
        ex_handoff_t *h = malloc(sizeof *h);
        if (!h) break;
        /* A short wait at a time, so that the thread notices when it is told to stop. */
        if (proven_net_accept(&g_listener, proven_net_deadline_in(20), &h->conn, NULL) != PROVEN_OK) { free(h); continue; }
        h->to = &g_workers[next];
        next = (next + 1) % LOOPS;
        if (proven_loop_post(h->to->loop, adopt_here, h) != PROVEN_OK) { (void)proven_net_close(&h->conn); free(h); }
    }
}

static void run(void *arg) { (void)proven_loop_run(((ex_worker_t *)arg)->loop); }

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();

    // ---- two loops, each with a server that has no listener -----------------------
    /* A server with no listener serves only what it is given. */
    proven_job_sys_t *threads = NULL;
    proven_job_group_t loops, accepting;
    proven_job_group_init(&loops);
    proven_job_group_init(&accepting);
    EXAMPLE_REQUIRE(proven_job_system_init(heap, LOOPS + 1, 8, &threads) == PROVEN_OK, "three threads");
    for (int i = 0; i < LOOPS; ++i) {
        ex_worker_t *w = &g_workers[i];
        w->index = i;
        proven_http_event_server_config_t config = { .on = { .on_request = on_request }, .ctx = w };
        EXAMPLE_REQUIRE(proven_loop_create(heap, &w->loop) == PROVEN_OK &&
                        proven_http_event_server_create(w->loop, &config, &w->server) == PROVEN_OK &&
                        proven_job_group_submit(threads, &loops, run, w) == PROVEN_OK, "a loop on its own thread, with a server");
    }

    // ---- one listener, and the thread that deals ----------------------------------
    proven_net_addr_t at;
    EXAMPLE_REQUIRE(proven_net_listen(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), 64, &g_listener, &at) == PROVEN_OK &&
                    proven_job_group_submit(threads, &accepting, acceptor, NULL) == PROVEN_OK, "a listener and its accepting thread");

    // ---- eight connections ---------------------------------------------------------
    /* A client that keeps no connection open: eight requests are eight connections. */
    proven_http_client_config_t client_config = { .alloc = heap, .max_idle_connections = 0 };
    proven_http_client_t *client = NULL;
    EXAMPLE_REQUIRE(proven_http_client_create(&client_config, &client) == PROVEN_OK, "a client");
    char url[64];
    int n = snprintf(url, sizeof url, "http://127.0.0.1:%u/", (unsigned)at.port);
    int answers[LOOPS] = { 0 };
    for (int i = 0; i < 8; ++i) {
        proven_http_client_response_t response;
        proven_u8str_t body = { 0 };
        EXAMPLE_REQUIRE(proven_http_client_get(client, (proven_u8str_view_t){ (const proven_byte_t *)url, (proven_size_t)n }, &response) == PROVEN_OK &&
                        proven_http_client_read_all(&response, heap, &body, 64) == PROVEN_OK, "a request is answered");
        proven_u8str_view_t text = proven_u8str_as_view(&body);
        if (text.size == 6 && memcmp(text.ptr, "loop ", 5) == 0 && text.ptr[5] - '0' < LOOPS) answers[text.ptr[5] - '0']++;
        proven_u8str_destroy(heap, &body);
        proven_http_client_finish(&response);
    }
    proven_http_client_destroy(client);
    EXAMPLE_REQUIRE(answers[0] == 4 && answers[1] == 4, "dealt in turn: four connections were served by each loop");

    // ---- taking it apart, in this order --------------------------------------------
    /* The acceptor first, so that nothing more is posted. Then the loops. Then the servers. */
    atomic_store(&g_stop, true);
    proven_job_group_wait(threads, &accepting);
    (void)proven_net_listener_close(&g_listener);
    for (int i = 0; i < LOOPS; ++i) proven_loop_stop(g_workers[i].loop);
    proven_job_group_wait(threads, &loops);
    int served = 0;
    for (int i = 0; i < LOOPS; ++i) {
        /* Anything posted and not yet run is run here, before its server goes. */
        EXAMPLE_REQUIRE(proven_loop_poll(g_workers[i].loop, PROVEN_NET_DONT_WAIT) == PROVEN_OK, "a last round");
        served += g_workers[i].served;
        proven_http_event_server_destroy(g_workers[i].server);
        proven_loop_destroy(g_workers[i].loop);
    }
    EXAMPLE_REQUIRE(served == 8, "eight requests were served in all");
    proven_job_system_close(threads);
    proven_job_system_destroy(threads);
    return EXAMPLE_OK();
}
```

## 14. What a connection holds, what was measured, and what is not here

**What a connection holds.** A connection with no request on it holds its struct and no buffer:
requests are read into the loop's scratch buffer, and bytes are copied aside only when a
request is incomplete. (A compressor exists only while a compressed response is being
written.) On x86-64 Linux the registered tests measure 480 bytes of heap for an
idle plain HTTP connection, failing above 512, and 976 bytes for an idle plain WebSocket
connection, failing above 1 KiB. A TLS connection adds the engine's state, about 1.25 KiB while
idle (Chapter 14, section 9). After a quiet second between requests the server gives back the
expanded form of its record keys (wiped) and keeps the keys they were made from; the next
request remakes them, which takes about a tenth of a microsecond per direction on a processor
with AES instructions. An idle TLS connection quiet for a second measures 1,088 bytes in all,
and the test fails at 2 KiB - the target for TLS, where a plain connection's is 1 KiB. **An HTTP/2 connection is another size**: 2,360 bytes idle after
one small request, failing above 3 KiB, and growing with the header fields it is made to
remember to about 12.8 KiB when both of its compression tables are full ([Chapter 17, section 6](manual-17-http2.md#6-http2-in-the-event-driven-server)).
The numbers below are HTTP/1.1's; HTTP/2's, under the same load, are in
[Chapter 17, section 8](manual-17-http2.md#8-what-is-accepted-what-is-not-here-and-how-this-was-tested).

**What was measured.** One run of a load program that is not part of the test suite, on one
machine: a 16-thread x86-64 desktop processor at 3.5 GHz with 62 GB, Linux 6.12, GCC at `-O2`.
The server and the client are separate processes on that machine, talking over the loopback
interface, plain HTTP. Every connection makes one request and is then left open; after that a
stated share of them make requests back to back, each waiting for its answer, for ten seconds.
The handler answers two bytes. **Loopback is not a network** - there is no loss and no slow
peer here - and a handler that does real work will be slower than one that answers "ok".

One loop:

| Connections held | Making requests | Requests per second | Round trip: median, 99th percentile | Server's resident memory per connection |
|---|---|---|---|---|
| 1,000 | 10 | 169,000 | 0.05 ms, 0.14 ms | 479 B |
| 50,000 | 10 | 175,000 | 0.04 ms, 0.11 ms | 481 B |
| 10,000 | 100 | 184,000 | 0.5 ms, 0.9 ms | 486 B |
| 50,000 | 500 | 171,000 | 2.8 ms, 5.2 ms | 481 B |
| 100,000 | 1,000 | 155,000 | 6.4 ms, 9.7 ms | 480 B |

What the table says: **connections that are doing nothing cost a busy one nothing** - ten
connections are served as fast among fifty thousand as among a thousand - and they cost
memory in proportion, about 480 bytes each in the server. One loop does a fixed amount of work
per second, so when more connections ask at once each waits longer: the round trip grows with
the number asking, not with the number held. No connection was lost in any row. Opening
100,000 took 96 seconds, against 1.7 for 50,000. That appears to be the client's system
searching for a free port as its range runs out, not the server: the blocking server took as
long or longer for the same step.

The system's own cost for a socket is separate and larger: over the same runs the kernel's
slab memory grew by about 7.5 KiB per connection, for both of its ends together.

Several loops, with four client processes holding 12,500 connections each and 1% of them
making requests:

| Loops | Requests per second, all clients together | Round trip: median |
|---|---|---|
| 1 | 167,000 | 2.9 ms |
| 2 | 337,000 | 1.4 ms |
| 4 | 514,000 | 1.0 ms |

**The blocking server of Chapter 11, for comparison**, in the same program with its handler
on its loop's thread: it held the same 100,000 connections and answered slightly faster -
186,000 requests per second at 50,000 held, against 171,000. What it costs is memory: 39 KiB
allocated per connection, 10.9 KiB of it resident, against 480 bytes. And it is one thread
whose handler waits; the reasons to choose this chapter's server are the ones of section 1,
not raw speed for a trivial handler.

**On Windows, until v0.80.0, the ceiling was low.** There the loop waited with `WSAPoll`,
which looks at every socket on every call. Both ends of the same program on one Windows 11
virtual machine, ten connections making requests:

| Connections held | Requests per second | Round trip: median |
|---|---|---|
| 100 | 40,000 | 0.2 ms |
| 1,000 | 8,100 | 1.0 ms |
| 2,000 | 2,700 | 3.2 ms |
| 5,000 | 480 | 18 ms |
| 10,000 | 150 | 66 ms |

The client was on the same loop implementation, so each figure carries the cost twice. Asked
for 20,000 connections, that machine opened 16,369 before its ports ran out. Since v0.81.0 the
loop is built on an I/O completion port there (section 5), which is told only of the operations
that finish and does not look at idle sockets. The same program on the same machine, two runs in
a row (the two figures per cell; the client on the completion port as well):

| Connections held | Making requests | Requests per second | Round trip: median, 99th percentile | Server's private memory per connection |
|---|---|---|---|---|
| 100 | 10 | 42,000 / 46,000 | 0.18 / 0.16 ms, 0.8 / 0.4 ms | - |
| 1,000 | 10 | 50,000 / 50,000 | 0.15 ms, 0.4 ms | 1.2 KB |
| 2,000 | 10 | 55,000 / 51,000 | 0.15 ms, 0.3 / 0.4 ms | 1,065 B |
| 5,000 | 10 | 48,000 / 36,000 | 0.16 / 0.26 ms, 0.4 ms | 1,049 B |
| 10,000 | 10 | 53,000 / 51,000 | 0.15 ms, 0.4 ms | 1,040 B |
| 10,000 | 100 | 47,000 / 48,000 | 2.0 / 1.9 ms, 3.9 / 4.2 ms | 1,045 B |
| 16,372 (20,000 asked) | 9 | 53,000 / 54,000 | 0.13 ms, 0.3 ms | 1,047 B |

Ten connections are served as fast among sixteen thousand as among a thousand: at 10,000 held,
about 350 times the requests of `WSAPoll` and a round trip four hundred times shorter. What the
server's allocator holds is 536 bytes per idle connection in every row (the registered test's
figure); the process's private memory, with the heap's own bookkeeping, is about 1 KB per
connection. The machine is a virtual one on a shared host, so one row in a run can be a quarter
slower than the other run (5,000 held); the ports still ran out at about 16,370. The blocking
server of Chapter 11, whose selector is on the completion port too, measured 50,000 / 54,000
requests per second at 1,000 held and 53,000 / 52,000 at 10,000, a median of 0.15 ms, with 40 KB
of private memory per connection.

**What is not here.**

- **A load test worth the name.** The figures above are one machine talking to itself: no
  network, no slow or hostile clients, no TLS under load, one handler that does nothing.
- Upgrades other than WebSocket on this server; a WebSocket client through a proxy or over
  HTTP/2.
- **In the event-driven client:** redirects, authentication, cookies, proxies, and name
  resolution (section 12 says what to do instead); pipelining, and any order among waiting
  requests but the order of starting.
- **TLS handshakes off the loop's thread.** Each costs the loop about 2 ms during which it
  serves nothing else (Chapter 14, section 9).
- **Sending a file without copying it**, HTTP/2. (Compressed responses are in
  [Chapter 16](manual-16-compression.md), section 6: `proven_http_stream_compress` in the
  server - with `compress_zstd` in its configuration for Zstandard - and `decompress` - and
  `decompress_zstd` for Zstandard, `decompress_brotli` for Brotli - in the client's.)
- **Watching files, signals or child processes** with the loop. It watches sockets.

**How this was tested.** The registered tests drive the loop's timers and posts from one thread
and from several, including cancellation from inside the callbacks they concern; drive every
operation of section 5 on the system's selector and again on the poll kind - each answered once,
never inside the call that started it, through cancellation, close with everything in flight, a
send that fails, and three MiB to a slow reader; and run the server over loopback, plain and over TLS, through every
row of the tables in sections 7 and 9: answers at once, from a timer and from another thread,
uploads with pause and resume, a 16 MB download to a client that does not read until a write
has been refused, pipelining, each refusal, each timeout, clients that disappear, and a server
destroyed under an unanswered request - checking that `on_done` was called once for every
`on_request`. The WebSocket is run against the blocking client of Chapter 12 and against raw
sockets, through the rows of the tables of section 11 - all but `PROVEN_ERR_NOMEM`, and
`PROVEN_ERR_RESET` from `proven_ws_event_accept` and from the send calls, which are not
provoked. The client is run against this chapter's server on the same loop and against answers
the test scripts itself, through the rows of the tables of section 12 - all but
`PROVEN_ERR_UNREACHABLE`, `PROVEN_ERR_NOMEM`, an expired certificate, and three of the four
time limits: only the response timeout is made to pass. All of them were run several hundred
times under address and undefined-behaviour checking with the machine loaded, and under thread
checking.
Several loops are run as three servers on three threads behind one accepting thread, plain
and over TLS, under thread checking. Timers further away than one turn of the loop's wheel
(16.4 seconds) are tested with a clock the test moves. **Not done:** no run against a hostile
peer beyond the cases listed, no run of a published WebSocket conformance suite against this
server, and no load beyond the single run described above.
