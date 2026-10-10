# Chapter 15: The event loop and the event-driven server

**Part V - Talking to the operating system. Prerequisites: [Chapter 9](manual-09-networking.md)
(sockets, deadlines, the selector); [Chapter 11](manual-11-http-client-server.md) (the HTTP
server this one is the other form of); [Chapter 14](manual-14-tls.md) if it is to speak TLS.**
**After this chapter** you can run a loop with timers and work that comes back from other
threads, serve HTTP from it without a call that waits, answer a request later, stream a response
to a client slower than your program, and say what a connection costs while it does nothing.

This chapter covers `loop.h` and `http_event.h`. Both are hosted-only, and `PROVEN_NO_NET`
leaves them out.

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
11. [What a connection holds, and what is not here](#11-what-a-connection-holds-and-what-is-not-here)

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

## 11. What a connection holds, and what is not here

**What a connection holds.** A connection with no request on it holds its struct and no buffer:
requests are read into the loop's scratch buffer, and bytes are copied aside only when a
request is incomplete. On x86-64 Linux the registered test measures 440 bytes of heap for an
idle plain connection and fails above 512. A TLS connection adds the engine's state, about
1 KiB while idle (Chapter 14, section 9). The system's own cost for an open socket is separate
and is usually the larger part.

**What is not here.**

- **How many connections one loop carries**, measured. The design is for tens of thousands; the
  measurements belong to a later release, and until then the bounded claims are the ones above.
- **More than one loop.** A server here uses one thread. Running several loops across
  processor cores is a later release.
- **WebSocket and other upgrades** on this server, and **an event-driven client**. Chapter 12's
  WebSocket and Chapter 11's client are the blocking ones.
- **Sending a file without copying it**, response compression, HTTP/2.
- **Watching files, signals or child processes** with the loop. It watches sockets.

**How this was tested.** The registered tests drive the loop's timers, posts and socket
interest from one thread and from several, including removal and cancellation from inside the
callbacks they concern; and run the server over loopback, plain and over TLS, through every
row of the tables in sections 7 and 9: answers at once, from a timer and from another thread,
uploads with pause and resume, a 16 MB download to a client that does not read until a write
has been refused, pipelining, each refusal, each timeout, clients that disappear, and a server
destroyed under an unanswered request - checking that `on_done` was called once for every
`on_request`. They were run several hundred times under address and undefined-behaviour
checking with the machine loaded, and under thread checking. **Not done:** no load test, no
measurement of throughput or latency, no run against a hostile client beyond the cases listed.
