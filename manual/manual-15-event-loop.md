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
5. [Watching a socket of your own](#5-watching-a-socket-of-your-own)
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
void proven_loop_destroy(proven_loop_t *loop);
proven_err_t proven_loop_run(proven_loop_t *loop);
proven_err_t proven_loop_poll(proven_loop_t *loop, proven_net_deadline_t until);
void proven_loop_stop(proven_loop_t *loop);
```

A `proven_loop_t` is a selector ([Chapter 9](manual-09-networking.md), section 9), a set of
timers, a queue that other threads can post to, and a scratch buffer.

| Call | What it does | Returns |
|---|---|---|
| `proven_loop_create(alloc, &loop)` | Make a loop. | `proven_err_t`: `INVALID_ARG`; `NOMEM`; what opening the selector returned. |
| `proven_loop_run(loop)` | Wait, call what is ready, fire what is due, repeat - until `proven_loop_stop`. | `PROVEN_OK` after a stop; an error if waiting itself failed. |
| `proven_loop_poll(loop, until)` | One round: wait until something is ready or `until` passes, and handle it. `PROVEN_NET_DONT_WAIT` handles only what is ready now. | `proven_err_t`. |
| `proven_loop_stop(loop)` | Make `proven_loop_run` return. Safe from any thread and from a callback. | nothing. |
| `proven_loop_destroy(loop)` | Free the loop. Whatever is still registered or set is forgotten; nothing is called. Null is accepted. | nothing. |

`proven_loop_run` is for a thread that does nothing else. `proven_loop_poll` is for a program
that already has a main loop of its own and gives this one a turn in it.

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

- A timer **fires once**, `ms` milliseconds from now, rounded up to the loop's tick of 16 ms. It
  does not fire early. One that should repeat sets itself again in its own function.
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
} app_t;

static void note(app_t *app, int what) { if (app->count < 8) app->order[app->count++] = what; }

/* A timer fires once. One that should repeat sets itself again. */
static void on_tick(void *ctx) {
    app_t *app = ctx;
    note(app, 1);
    if (++app->ticks < 3) proven_loop_timer_set(app->loop, &app->tick, 20, on_tick, app);
}

static void on_late(void *ctx) { note(ctx, 9); }

/* This runs on the loop's thread, although a worker thread asked for it. */
static void on_result(void *ctx) {
    app_t *app = ctx;
    note(app, 5);
    proven_loop_timer_cancel(app->loop, &app->late);      /* it would have fired in five seconds; now it never will */
    proven_loop_stop(app->loop);
}

/* This runs on a worker thread. It may take as long as it likes: the loop is not waiting. */
static void slow_work(void *ctx) {
    app_t *app = ctx;
    proven_time_sleep(400);
    atomic_store(&app->from_worker, 1);
    if (proven_loop_post(app->loop, on_result, app) != PROVEN_OK) proven_loop_stop(app->loop);   /* only a failed allocation refuses a post; then just end the run */
}

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();
    app_t app = { 0 };

    // ---- a loop -------------------------------------------------------------------
    /* A loop is a selector, a set of timers, and a queue other threads can post to. */
    EXAMPLE_REQUIRE(proven_loop_create(heap, &app.loop) == PROVEN_OK, "a loop");

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

## 5. Watching a socket of your own

The server of the next section does this for you. It is here for a protocol that is not HTTP.

```c
proven_err_t proven_loop_io_add(proven_loop_t *loop, proven_loop_io_t *io, proven_net_handle_t handle,
                                proven_u8 want, proven_loop_io_fn fn, void *ctx);
proven_err_t proven_loop_io_set(proven_loop_t *loop, proven_loop_io_t *io, proven_u8 want);
void proven_loop_io_remove(proven_loop_t *loop, proven_loop_io_t *io);
proven_mem_mut_t proven_loop_scratch(proven_loop_t *loop);
proven_allocator_t proven_loop_allocator(const proven_loop_t *loop);
proven_size_t proven_loop_io_count(const proven_loop_t *loop);
```

| Call | What it does | Returns |
|---|---|---|
| `proven_loop_io_add(loop, &io, handle, want, fn, ctx)` | Watch a socket: `fn(ctx, got)` is called when it is ready for what `want` asks (`PROVEN_NET_READABLE`, `PROVEN_NET_WRITABLE`, both, or 0 for nothing yet). | `proven_err_t`: `INVALID_ARG`; `INVALID_STATE` if `io` is already registered; what the selector returned. |
| `proven_loop_io_set(loop, &io, want)` | Change what it is watched for. | `proven_err_t`. |
| `proven_loop_io_remove(loop, &io)` | Stop watching. | nothing. |
| `proven_loop_scratch(loop)` | The loop's scratch buffer, 64 KiB. | `proven_mem_mut_t`. |
| `proven_loop_allocator(loop)` | The allocator the loop was made with. | `proven_allocator_t`. |
| `proven_loop_io_count(loop)` | How many sockets are watched. | `proven_size_t`. |

A `proven_loop_io_t` is yours in the way a timer is: inside your connection struct,
zero-initialised, not moved while it is registered. Four things to know:

- **Readiness is level-triggered.** While there is something to read, the function is called
  every round. Read with `PROVEN_NET_DONT_WAIT` until the call says it would wait, or stop
  asking for `PROVEN_NET_READABLE`.
- **Ask for `PROVEN_NET_WRITABLE` only while you have something to write.** A socket with room
  is writable all the time, and a loop that is told so every round does nothing else.
- **Remove before closing.** `proven_loop_io_remove` is safe inside any callback, the socket's
  own included, and after it nothing more is delivered for that socket - not even an event
  already collected in the same round.
- **The scratch buffer is how thousands of connections share one read buffer.** Read into it,
  use what arrived, and keep only what cannot be used yet. Its contents mean nothing once your
  callback returns, or calls anything that may use it too.

## 6. The event-driven server

```c
proven_err_t proven_http_event_server_create(proven_loop_t *loop, const proven_http_event_server_config_t *config,
                                             proven_http_event_server_t **out);
proven_err_t proven_http_event_server_listen(proven_http_event_server_t *server, proven_net_addr_t at, proven_net_addr_t *bound);
void proven_http_event_server_destroy(proven_http_event_server_t *server);
proven_size_t proven_http_event_server_connections(const proven_http_event_server_t *server);
void proven_http_event_server_stop_listening(proven_http_event_server_t *server);
```

A server is made on a loop, told where to listen, and does nothing until the loop runs.

| Call | What it does | Returns |
|---|---|---|
| `proven_http_event_server_create(loop, &config, &server)` | Make a server. | `proven_err_t`: `INVALID_ARG` for no loop, no `on_request`, or a TLS configuration with no certificate; `NOMEM`. |
| `proven_http_event_server_listen(server, at, &bound)` | Listen at `at`; up to four addresses for one server. `bound` (optional) receives the address in use, with the port the system chose when `at` asked for port 0. | `proven_err_t`. |
| `proven_http_event_server_connections(server)` | Connections open now. | `proven_size_t`. |
| `proven_http_event_server_stop_listening(server)` | Stop accepting; open connections go on. | nothing. |
| `proven_http_event_server_destroy(server)` | Close every connection and the listeners, and free the server. Exchanges still in progress get `on_done` with `PROVEN_ERR_RESET`. The loop is neither stopped nor freed. Null is accepted. | nothing. |

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
} app_t;

/* What this program keeps for one request while it is being answered. */
typedef struct {
    app_t *app;
    proven_http_stream_t *stream;
    proven_loop_timer_t timer;
    proven_size_t sent, total;         /* /count: how far the body has got */
    proven_size_t received;            /* /size: body bytes seen */
} exchange_t;

/* Write lines until the server says "no more for now". It says so when the client has not
 * taken what was already sent; the rest waits for on_writable. */
static void count_pump(exchange_t *x) {
    while (x->sent < x->total) {
        char line[32];
        int n = snprintf(line, sizeof line, "%u\n", (unsigned)x->sent);
        proven_result_size_t w = proven_http_stream_write(x->stream, (proven_mem_view_t){ (const proven_byte_t *)line, (proven_size_t)n });
        if (w.err != PROVEN_OK) return;                   /* the client left */
        if (w.value < (proven_size_t)n) return;           /* held back: on_writable will say when */
        x->sent++;
    }
    (void)proven_http_stream_end(x->stream);
}

static void answer_later(void *ctx) {
    exchange_t *x = ctx;
    (void)proven_http_stream_respond(x->stream, 200, NULL, 0, proven_mem_view_from_u8(PROVEN_LIT("worth the wait\n")));
}

static void on_request(void *ctx, proven_http_stream_t *stream, const proven_http_request_t *head) {
    app_t *app = ctx;
    exchange_t *x = calloc(1, sizeof *x);
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
    exchange_t *x = proven_http_stream_user(stream);
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
    exchange_t *x = proven_http_stream_user(stream);
    if (x->total > 0) count_pump(x);
}

/* Called once for every on_request, whatever happened. The place to free what was attached. */
static void on_done(void *ctx, proven_http_stream_t *stream, proven_err_t why) {
    app_t *app = ctx;
    exchange_t *x = proven_http_stream_user(stream);
    if (!x) return;
    proven_loop_timer_cancel(app->loop, &x->timer);
    free(x);
    if (why == PROVEN_OK) app->served++;
}

static void run(void *arg) { (void)proven_loop_run(((app_t *)arg)->loop); }

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
    app_t app = { 0 };

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
    /* Stop the loop, then take the server apart. Nothing else is touching it by then. */
    proven_loop_stop(app.loop);
    proven_job_group_wait(threads, &running);
    EXAMPLE_REQUIRE(app.served == 4, "four exchanges ended with on_done(PROVEN_OK)");
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
| `tls` | A TLS configuration with a certificate, or null for plain HTTP. | null |

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

**Shutting down.** `proven_http_event_server_stop_listening` refuses nobody already connected
and accepts nobody new; wait until `proven_http_event_server_connections` falls, or for as
long as you are willing to, then `proven_http_event_server_destroy`, then stop and destroy the
loop. Destroy the server on the loop's thread or after the loop has stopped - not while another
thread is inside `proven_loop_run`.

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
} app_t;

/* What this program keeps for one connection: the message being collected. */
typedef struct {
    app_t *app;
    char text[4096];
    proven_size_t len;
    bool waiting;                 /* a reply the server would not take yet */
} peer_t;

/* Send the reply. If the client is not taking what was sent before, the server refuses it:
 * keep it, stop reading from this client, and try again when on_writable says so. */
static void reply(proven_ws_stream_t *ws, peer_t *peer) {
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
    peer_t *peer = proven_ws_stream_user(ws);
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
    peer_t *peer = proven_ws_stream_user(ws);
    if (!peer->waiting) return;
    reply(ws, peer);
    if (!peer->waiting) proven_ws_stream_resume(ws);
}

/* Called once, however the connection ended. The place to free what was attached. */
static void on_closed(void *ctx, proven_ws_stream_t *ws, proven_u16 code, proven_err_t why) {
    app_t *app = ctx;
    (void)why;
    free(proven_ws_stream_user(ws));
    app->last_code = code;
    app->closed++;
}

static void on_request(void *ctx, proven_http_stream_t *stream, const proven_http_request_t *head) {
    app_t *app = ctx;
    peer_t *peer = calloc(1, sizeof *peer);
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

static void run(void *arg) { (void)proven_loop_run(((app_t *)arg)->loop); }

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();
    app_t app = { 0 };

    // ---- a server on a loop, on its own thread -------------------------------------
    EXAMPLE_REQUIRE(proven_loop_create(heap, &app.loop) == PROVEN_OK, "a loop");
    proven_http_event_server_config_t config = { .on = { .on_request = on_request }, .ctx = &app };
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

- **One request, one connection, closed afterwards.** There is no connection reuse, and no
  redirects, answers to challenges, cookies or proxies. Chapter 11's client has all of them;
  use it wherever a thread per request in flight is affordable.
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
| `proven_http_event_client_requests(client)` | `proven_size_t`: requests in flight. |
| `proven_http_event_client_destroy(client)` | nothing. Every request in flight gets `on_done(PROVEN_ERR_RESET)`. Null is accepted. |
| `proven_http_event_request_write(request, data)` | `proven_result_size_t`: the bytes taken. `INVALID_STATE` when the request has no body to write or it was ended; `OUT_OF_BOUNDS` for more than the length promised. |
| `proven_http_event_request_end(request)` | `proven_err_t`: `INVALID_STATE` when there was nothing to end; `INVALID_FORMAT` when fewer bytes were written than promised - `on_done` is then called inside the call. |
| `proven_http_event_request_abort(request)` | nothing. `on_done(PROVEN_ERR_RESET)` is called inside the call. |
| `proven_http_event_request_pause`, `proven_http_event_request_resume` | nothing. Stop and restart the delivery of the response body. |

The configuration's fields, each with a default when zero:

| Field | Meaning | Default |
|---|---|---|
| `alloc` | The allocator for requests and buffers. | the loop's |
| `tls` | A TLS configuration that can verify servers, for `https`. Must outlive the client. | none: `https` is refused |
| `max_head_bytes` | Largest response head, and request head. | 16 KiB |
| `max_headers` | Most header fields in a response. | 64 |
| `max_body_bytes` | Largest response body. | no limit: it is delivered in pieces and never held |
| `max_buffered_output` | Request-body bytes held before writes are refused. | 64 KiB |
| `connect_timeout_ms` | To be connected, the TLS handshake included. | 10 s |
| `response_timeout_ms` | From the request being sent to the end of the response head. | 30 s |
| `body_timeout_ms` | Between pieces of the response body. | 30 s |
| `write_timeout_ms` | Output held with none of it accepted by the server. | 30 s |
| `decompress` | Ask for gzip and decode it: `on_body` gets decoded pieces, and `max_body_bytes` counts them. [Chapter 16](manual-16-compression.md), section 6. | off |

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

typedef struct app app_t;

/* What this program keeps for one request. */
typedef struct {
    app_t *app;
    const char *path;
    int status;
    char body[64];
    proven_size_t len;
    proven_err_t why;
    bool done;
} fetch_t;

struct app {
    proven_loop_t *loop;
    proven_http_event_client_t *client;
    proven_job_sys_t *workers;
    proven_u16 port;
    fetch_t fetch[3];
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
    ((fetch_t *)ctx)->status = head->status;
}

/* The body, a piece at a time. Nothing is collected for you; collect what you want. */
static void on_body(void *ctx, proven_http_event_request_t *request, proven_mem_view_t piece, bool last) {
    (void)request; (void)last;
    fetch_t *f = ctx;
    for (proven_size_t i = 0; i < piece.size && f->len < sizeof f->body - 1; ++i) f->body[f->len++] = (char)piece.ptr[i];
}

/* Once for every request that was started, whatever happened to it. */
static void on_done(void *ctx, proven_http_event_request_t *request, proven_err_t why) {
    (void)request;
    fetch_t *f = ctx;
    f->why = why;
    f->done = true;
    if (++f->app->finished == 3) proven_loop_stop(f->app->loop);
}

static proven_err_t fetch(app_t *app, fetch_t *f, const char *path, const proven_net_addr_t *address, const char *host) {
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
    app_t *app = ctx;
    if (app->lookup != PROVEN_OK || fetch(app, &app->fetch[2], "/missing", &app->found, "localhost") != PROVEN_OK) {
        app->fetch[2].why = PROVEN_ERR_NOT_FOUND;
        if (++app->finished == 3) proven_loop_stop(app->loop);
    }
}

/* On a worker thread: this call may wait as long as the resolver takes. */
static void resolve(void *ctx) {
    app_t *app = ctx;
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
    app_t app = { 0 };

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
    fetch_t unresolved = { 0 };
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
} worker_t;

static worker_t g_workers[LOOPS];
static proven_net_listener_t g_listener;
static atomic_bool g_stop;

static void on_request(void *ctx, proven_http_stream_t *stream, const proven_http_request_t *head) {
    worker_t *me = ctx;
    (void)head;
    char text[16];
    int n = snprintf(text, sizeof text, "loop %d", me->index);
    me->served++;
    (void)proven_http_stream_respond(stream, 200, NULL, 0, (proven_mem_view_t){ (const proven_byte_t *)text, (proven_size_t)n });
}

/* What crosses from the accepting thread to a loop. An open connection is not to be copied,
 * so it travels in memory of its own. */
typedef struct { worker_t *to; proven_net_conn_t conn; } handoff_t;

/* On the loop's thread - the only place a server may be given a connection. */
static void adopt_here(void *ctx) {
    handoff_t *h = ctx;
    if (proven_http_event_server_adopt(h->to->server, &h->conn) != PROVEN_OK) (void)proven_net_close(&h->conn);   /* refused: still ours to close */
    free(h);
}

/* The accepting thread. It touches no server: it accepts, chooses, and posts. */
static void acceptor(void *arg) {
    (void)arg;
    int next = 0;
    while (!atomic_load(&g_stop)) {
        handoff_t *h = malloc(sizeof *h);
        if (!h) break;
        /* A short wait at a time, so that the thread notices when it is told to stop. */
        if (proven_net_accept(&g_listener, proven_net_deadline_in(20), &h->conn, NULL) != PROVEN_OK) { free(h); continue; }
        h->to = &g_workers[next];
        next = (next + 1) % LOOPS;
        if (proven_loop_post(h->to->loop, adopt_here, h) != PROVEN_OK) { (void)proven_net_close(&h->conn); free(h); }
    }
}

static void run(void *arg) { (void)proven_loop_run(((worker_t *)arg)->loop); }

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
        worker_t *w = &g_workers[i];
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
written.) On x86-64 Linux the registered tests measure 448 bytes of heap for an
idle plain HTTP connection, failing above 512, and 928 bytes for an idle plain WebSocket
connection, failing above 1 KiB. A TLS connection adds the engine's state, about 1 KiB while
idle (Chapter 14, section 9).

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

**On Windows the ceiling is low, and here is where.** There the loop waits with `WSAPoll`,
which looks at every socket on every call. Both ends of the same program on one Windows 11
virtual machine, ten connections making requests:

| Connections held | Requests per second | Round trip: median |
|---|---|---|
| 100 | 40,000 | 0.2 ms |
| 1,000 | 8,100 | 1.0 ms |
| 2,000 | 2,700 | 3.2 ms |
| 5,000 | 480 | 18 ms |
| 10,000 | 150 | 66 ms |

The client is on the same loop implementation, so each figure carries the cost twice. Asked
for 20,000 connections, that machine opened 16,369 before its ports ran out. On Windows this
server is for hundreds of connections, or a few thousand at most; nothing in this release
changes that.

**What is not here.**

- **A load test worth the name.** The figures above are one machine talking to itself: no
  network, no slow or hostile clients, no TLS under load, one handler that does nothing.
- **A WebSocket client on the loop**, and upgrades other than WebSocket on this server.
  Chapter 12's client is the blocking one.
- **In the event-driven client:** connection reuse, redirects, authentication, cookies,
  proxies, and name resolution (section 12 says what to do instead).
- **TLS handshakes off the loop's thread.** Each costs the loop about 2 ms during which it
  serves nothing else (Chapter 14, section 9).
- **A faster wait on Windows** than `WSAPoll`.
- **Sending a file without copying it**, HTTP/2. (Compressed responses are in
  [Chapter 16](manual-16-compression.md), section 6: `proven_http_stream_compress` in the
  server, `decompress` in the client's configuration.)
- **Watching files, signals or child processes** with the loop. It watches sockets.

**How this was tested.** The registered tests drive the loop's timers, posts and socket
interest from one thread and from several, including removal and cancellation from inside the
callbacks they concern; and run the server over loopback, plain and over TLS, through every
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
