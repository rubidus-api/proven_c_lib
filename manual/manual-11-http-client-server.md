# Chapter 11: An HTTP Client and Server

**Part V - Talking to the operating system. Prerequisites: [Chapter 9](manual-09-networking.md)
(addresses, deadlines, the transport) and [Chapter 10](manual-10-http.md) (requests, responses,
headers); [Chapter 6](manual-06-execution-and-platform.md) section 1 if you will run handlers on
a job system.**
**After this chapter** you can serve HTTP from a function of your own, with every wait bounded
and every malformed request refused before it reaches you; and fetch a URL - through redirects,
a login, a proxy - reading the body at your own pace.

This chapter covers `http_server.h` and `http_client.h`. Both need sockets: they are not part of
a [freestanding](manual-freestanding.md) build, and `PROVEN_NO_NET` leaves them out.

## Table of contents

1. [What these are, and the one thing they are not](#1-what-these-are-and-the-one-thing-they-are-not)
2. [A server](#2-a-server)
3. [Inside a handler](#3-inside-a-handler)
4. [What the server does without asking you](#4-what-the-server-does-without-asking-you)
5. [Two ways to run handlers](#5-two-ways-to-run-handlers)
6. [A client](#6-a-client)
7. [One request, one response](#7-one-request-one-response)
8. [Redirects, challenges and cookies](#8-redirects-challenges-and-cookies)
9. [Connections and proxies](#9-connections-and-proxies)
10. [Where TLS will attach](#10-where-tls-will-attach)
11. [Changing protocol](#11-changing-protocol)
12. [What is not here](#12-what-is-not-here)

## 1. What these are, and the one thing they are not

Chapter 10 is a codec: it turns bytes into messages and back, and never touches a socket.
Chapter 9 is sockets, and knows nothing of HTTP. These two headers are the part in between -
the loop that accepts, reads, parses, times out and keeps connections open, and its mirror
image on the client side.

They are drivers and nothing above that. The server has no router, no static-file handler, no
sessions and no templates: it calls one function with one request. The client has no retry
policy, no cache and no JSON: it sends one request and gives you one response. What a program
builds on them is the program's.

**TLS is a choice you make, and the default is none.** A server speaks plain HTTP unless its
config is given a TLS configuration, and a client refuses an `https` URL with
`PROVEN_ERR_UNSUPPORTED` - it does not quietly fetch it over `http` instead - unless it is
given the TLS wrap. Both are one field each; [Chapter 14](manual-14-tls.md) is how. Without
them, everything these two send and receive can be read, and changed, by anyone on the path:

- Give the server a TLS configuration, or put it behind something that terminates TLS, or
  keep it on the loopback interface or a network you trust.
- Do not send a password, a token or a session cookie through a client over `http://` to
  another machine.

That is a statement of what the code does, not a recommendation to be weighed.

## 2. A server

```text
static void handle(void *ctx, proven_http_exchange_t *x) {
    (void)proven_http_exchange_respond(x, 200, NULL, 0, body);
}

proven_http_server_config_t config = {0};
config.alloc = proven_heap_allocator();
config.handler = handle;
proven_http_server_create(&config, &server);
proven_http_server_listen(server, proven_net_addr_any(PROVEN_NET_FAMILY_IPV4, 8080), NULL);
proven_http_server_run(server);          /* until proven_http_server_stop */
proven_http_server_destroy(server);
```

### Reference

| API | Intent | Return |
|---|---|---|
| `proven_http_server_create(&config, &server)` | Make a server. It does not listen yet. | `proven_err_t`: `INVALID_ARG` without an allocator or a handler; `NOMEM`; `BUSY` when out of sockets. |
| `proven_http_server_listen(server, at, &bound)` | Listen at an address. Up to four - an IPv4 and an IPv6 one, say. `bound` (optional) receives the address actually bound, so port 0 can be used. | the errors of `proven_net_listen`; `OUT_OF_BOUNDS` for a fifth. |
| `proven_http_server_run(server)` | Serve until stopped. | `proven_err_t`: `PROVEN_OK` after a stop. |
| `proven_http_server_poll(server, until)` | One round: wait until something is ready or until `until`, and handle it. For a program with a loop of its own. | `proven_err_t`: `PROVEN_OK` when something was handled, `TIMEOUT` when nothing was. |
| `proven_http_server_stop(server)` | Make `run` return. Safe from any thread and from a handler. | none. |
| `proven_http_server_destroy(server)` | Close every connection and listener and free the server. Not from a handler. | none. |
| `proven_http_server_connection_count(server)` | Connections open now. For the loop's thread. | `proven_size_t`. |

```text
typedef struct {
    proven_allocator_t alloc;          /* required */
    proven_http_handler_fn handler;    /* required: void (*)(void *ctx, proven_http_exchange_t *) */
    void *handler_ctx;
    proven_job_sys_t *jobs;            /* NULL: handlers run on the loop's thread (section 5) */
    proven_size_t max_connections;     /* 0: 64 */
    proven_size_t max_head_bytes;      /* 0: 16 KiB - request head, and response head */
    proven_size_t max_headers;         /* 0: 64 fields */
    proven_u64 max_body_bytes;         /* 0: 1 MiB */
    proven_u32 head_timeout_ms;        /* 0: 10 s */
    proven_u32 body_timeout_ms;        /* 0: 30 s */
    proven_u32 write_timeout_ms;       /* 0: 30 s */
    proven_u32 idle_timeout_ms;        /* 0: 60 s */
    const proven_tls_config_t *tls;    /* NULL: plain HTTP */
} proven_http_server_config_t;
```

Zero the struct and set what you need. Every limit has a default, and none can be turned off:
there is no "unlimited".

### What each limit is for

| Limit | What it stops |
|---|---|
| `head_timeout_ms` | A client that opens a connection and sends a request one byte a minute. It has this long from the first byte to the end of the head, then it gets a `408` and is closed. A new connection that sends nothing at all gets the same time. |
| `body_timeout_ms` | The same trick played with the body: each read of the body may wait this long. |
| `write_timeout_ms` | A client that asks for a large response and never reads it. Each write may wait this long. |
| `idle_timeout_ms` | Connections kept open and unused. Closed without a word, as HTTP allows. |
| `tls` | A TLS configuration with a certificate ([Chapter 14](manual-14-tls.md)): every connection is then HTTPS. It must outlive the server. The handshake shares `head_timeout_ms` with the first request's head. |
| `max_head_bytes`, `max_headers` | A head that never ends, or has ten thousand fields: `431`. |
| `max_body_bytes` | A body larger than you are prepared to take: `413`, before it is read when it was announced, while it is read when it was chunked. |
| `max_connections` | More connections than you have memory for. The next client waits in the listen backlog; it is not refused. |

Memory per connection is fixed when it is accepted: `2 * max_head_bytes` plus 4 KiB plus the
header array - about 38 KiB with the defaults, so 64 connections are about 2.4 MiB. Nothing a
client sends makes a connection cost more.

### Cautions, and what goes wrong

**`proven_net_addr_any` is every interface.** A server listening there is reachable from
wherever the machine is, in the clear. For a service meant for the same machine, listen on
`proven_net_addr_loopback`.

**An IPv6 listener does not take IPv4.** As Chapter 9 says, IPv6 sockets here are IPv6-only on
every platform. For both, call `proven_http_server_listen` twice.

**`destroy` is not `stop`.** `stop` makes `run` return, and the server stays stopped: `run`
called again returns at once. `destroy` frees it. From a handler, call `stop`, never `destroy`.

**What an open connection costs the loop.** The loop waits on a selector (Chapter 9,
section 9) and keeps its timeouts on a timer wheel, so a round of the loop costs in proportion
to the connections that have something to do, not to the connections that are open: with
twenty thousand idle keep-alive connections held, a request on one more takes as long as with
none (measured on Linux, where the selector is `epoll`). On Windows the selector is built on
`WSAPoll` and every round still looks at every connection.

That removes one limit and leaves two. Each connection still holds its buffers from the moment
it is accepted (below), and each request still occupies a thread while its handler runs
(section 5). A server for tens of thousands of *busy* connections needs neither of those to be
true, and this one does not provide that yet. Timeouts are kept to about a sixtieth of a
second: a connection may live that much longer than its limit, never shorter.

## 3. Inside a handler

The handler is called with a request whose head has been read, parsed and checked. The body has
not been read: that is the handler's choice.

### Reference

| API | Intent | Return |
|---|---|---|
| `proven_http_exchange_request(x)` | The request: method, target, headers (Chapter 10's `proven_http_request_t`). | pointer; views are good until the handler returns. |
| `proven_http_exchange_peer(x)` | The client's address. | `proven_net_addr_t`. |
| `proven_http_exchange_read(x, dest)` | Read more of the request body. | `proven_result_size_t`: `.err` is `EOF` when the body is complete; `TIMEOUT`; `OUT_OF_BOUNDS` past `max_body_bytes`; `INVALID_FORMAT` for bad chunk framing; `RESET` when the client left. |
| `proven_http_exchange_respond(x, status, headers, count, body)` | Send a whole response, with a body held in memory. | `proven_err_t`: `INVALID_STATE` when a response was already begun; `INVALID_ARG` for a header the server owns or a value the writers refuse; `OUT_OF_BOUNDS` when the head does not fit; `TIMEOUT`, `RESET`. |
| `proven_http_exchange_begin(x, status, headers, count, content_length)` | Begin a response whose body follows in pieces. `PROVEN_HTTP_LENGTH_UNKNOWN` when you do not know the length: the body is then sent chunked. | as `respond`. |
| `proven_http_exchange_write(x, data)` | Write more of that body. | `proven_err_t`: `OUT_OF_BOUNDS` past the length announced; `INVALID_STATE`; `TIMEOUT`, `RESET`. |
| `proven_http_exchange_end(x)` | Finish it. Optional: returning from the handler does the same. | `proven_err_t`. |
| `proven_http_exchange_compress(x)` | Ask that this response be compressed, if the client accepts gzip. Before the response begins. [Chapter 16](manual-16-compression.md), section 6 - read its cautions before using it. | none. |

### Cautions, and what goes wrong

**The target is the client's text.** `request->target` is what was sent - `/a/b?x=1`, or an
absolute URL, undecoded. Split it with `proven_url_split_target` before comparing, and never
build a file name from it without `proven_url_path_resolve` (Chapter 10, section 3).

Wrong:

```text
snprintf(file, sizeof file, "/var/www%.*s", (int)req->target.size, req->target.ptr);   /* wrong: "/../../etc/passwd" */
```

Correct - `proven_url_split_target` for the path, `proven_url_path_resolve` on it, and only a
`PROVEN_OK` result joined to the root.

**Do not write `Content-Length`, `Transfer-Encoding`, `Connection` or `Date` yourself.** The
server writes them, from what you actually send. Giving one is `PROVEN_ERR_INVALID_ARG`, and
nothing is sent - two `Content-Length` fields that disagree are exactly the ambiguity that
request smuggling is made of, and here it cannot be produced.

**A header value taken from a request cannot split your response.** A value with a line break
in it is refused by the writers, so `Location: ` followed by text a client chose cannot add a
header or a second response. After that refusal nothing has been sent, and you may send another
response - an error page - instead.

**Check the result of a write and stop when it fails.** `PROVEN_ERR_RESET` and
`PROVEN_ERR_TIMEOUT` mean the client is gone or not reading. A handler that carries on
generating a large body for nobody holds a connection - and in the single-thread model the whole
server - for nothing.

**If you return without sending anything, the client gets a `500`.** That is the server keeping
its promise that every request gets an answer; it is not a way to send errors. When reading the
body failed, the status is the one that failure calls for instead - `408`, `413` or `400`.

**Everything from the exchange ends when the handler returns** - the request, its views, the
exchange itself. A handler that hands the exchange to another thread and returns has handed over
a dangling pointer. To answer later, do not return: wait in the handler (with handlers on a job
system, section 5, that costs one worker).

**Announce a length only if you will write that many bytes.** Writing more is refused. Writing
fewer cannot be repaired - the client is waiting for the rest - so the connection is closed at
that point and the client sees a truncated response, which is the truth.

## 4. What the server does without asking you

A request that is malformed, ambiguous or too large never reaches the handler:

| The request | The answer |
|---|---|
| Not parseable as HTTP/1.x: bare line feeds, a space before a colon, control bytes, a folded header | `400` |
| HTTP/1.1 without exactly one `Host` | `400` |
| Both `Content-Length` and `Transfer-Encoding`; two lengths that differ; a length that is not a number | `400` |
| A head larger than `max_head_bytes`, or more than `max_headers` fields | `431` |
| Another HTTP version | `505` |
| A transfer coding other than `chunked` | `501` |
| A body announced larger than `max_body_bytes` | `413` |
| A head not finished within `head_timeout_ms` | `408` |
| The job system cannot take another request (section 5) | `503` |

Each of these closes the connection, in the way the end of this section describes. They are deliberately strict: where two servers read one
request differently, an attacker puts one request inside another, and the cure is to refuse
what is ambiguous rather than pick a reading.

And around a request that does reach the handler:

- **Keep-alive.** The connection stays open for the next request unless the client asked to
  close it, the request was HTTP/1.0 without `keep-alive`, or the response could not be framed.
- **Pipelining.** Requests sent back to back are answered in order.
- **An unread body.** If the handler did not read the request body, the server reads and
  discards it - up to 64 KiB - so the next request starts in the right place. Past that it closes.
- **`Expect: 100-continue`.** The `100 Continue` is sent when the handler first reads the body.
  A handler that refuses by the headers alone never invites the body, and the connection is
  then closed, since whether a body follows is the client's secret.
- **`HEAD`.** The handler answers as for `GET`; the server sends the head - `Content-Length`
  included - and none of the body.
- **`204` and `304`** are sent without a body and without a length.
- **HTTP/1.0.** A response of unknown length cannot be chunked for a 1.0 client; it is sent
  until the connection closes.
- **`Date`** on every response.
- **Closing without destroying the answer.** When the server closes a connection it first says so - it stops sending - and then keeps reading and discarding what the client sends, for up to a second, until the client closes too. A socket closed with unread input answers with a reset, and a reset makes the client's system discard the response it had not yet read: a client told `413` in the middle of an upload would see a broken connection instead. The second is fixed and is not a setting.

## 5. Two ways to run handlers

`jobs` in the configuration chooses.

**`jobs == NULL`: on the loop's thread.** One thread does everything. While a handler runs - or
waits for a slow client to send its body, or to read its response - no other connection is
served. That is fine for handlers that compute and answer; it is simple, and nothing needs a
lock. It is wrong for a handler that waits on something slow.

**`jobs` set: on a job system's workers.** Each request is handed to the job system of
[Chapter 6](manual-06-execution-and-platform.md), and the loop goes straight back to accepting
and reading. The handler reads the body and writes the response from its worker; the connection
belongs to it until it returns. Handlers run at the same time, and so:

- whatever they share needs the protection shared data always needs;
- `alloc` in the configuration is called from several threads and must be thread-safe - the
  heap allocator is; an arena is not;
- when the job queue is full the request is answered `503` and the connection closed. Size the
  queue for the load you mean to carry, and treat `503` as the server saying it has enough.

The handler is written the same way for both. What it may not do in either: block for ever. A
handler that never returns holds a connection for ever, and with it a worker - or the server.

**Slow clients cost a worker, not the loop - but they do cost a worker.** With `jobs` set, a
client that sends its body slowly occupies the worker running that handler until
`body_timeout_ms`. Enough such clients occupy every worker. The timeouts bound it; they do not
make it free. If that matters, read the request body in the handler only when you must, and
keep the timeouts short.

`proven_http_server_destroy` waits for handlers that are running on workers. Close the job
system after destroying the server, not before.

Compiled and run by the test suite:

<!-- example: manual/examples/en/ex_11_http_server.c -->
```c
/*
 * A complete HTTP server: one function that answers requests, and a loop that does the rest.
 *
 * The handler sees a request that has already been read, checked and limited. It reads the
 * body if it wants it, and sends one response - whole, or in pieces.
 */

typedef struct {
    proven_http_server_t *server;
    int served;
} app_t;

static void handle(void *ctx, proven_http_exchange_t *x) {
    app_t *app = ctx;
    const proven_http_request_t *req = proven_http_exchange_request(x);
    app->served++;

    /* The target is a path and perhaps a query; split it before comparing. */
    proven_u8str_view_t path, query;
    bool has_query;
    if (proven_url_split_target(req->target, &path, &query, &has_query) != PROVEN_OK) {
        (void)proven_http_exchange_respond(x, 400, NULL, 0, proven_mem_view_from_u8(PROVEN_LIT("bad target\n")));
        return;
    }

    if (req->method == PROVEN_HTTP_GET && proven_u8str_view_eq(path, PROVEN_LIT("/"))) {
        /* The simplest answer: a status, your headers, a body in memory. Content-Length,
         * Date and Connection are written for you. */
        proven_http_header_t type = { PROVEN_LIT("Content-Type"), PROVEN_LIT("text/plain; charset=utf-8") };
        (void)proven_http_exchange_respond(x, 200, &type, 1, proven_mem_view_from_u8(PROVEN_LIT("hello\n")));

    } else if (req->method == PROVEN_HTTP_POST && proven_u8str_view_eq(path, PROVEN_LIT("/shout"))) {
        /* Read the body as it comes and answer in pieces, without knowing the length first:
         * the response goes out chunked. Nothing here holds the whole body. */
        if (proven_http_exchange_begin(x, 200, NULL, 0, PROVEN_HTTP_LENGTH_UNKNOWN) != PROVEN_OK) return;
        proven_byte_t piece[256];
        for (;;) {
            proven_result_size_t got = proven_http_exchange_read(x, (proven_mem_mut_t){ piece, sizeof piece });
            if (got.err != PROVEN_OK) break;          /* PROVEN_ERR_EOF: the body is complete */
            for (proven_size_t i = 0; i < got.value; ++i) {
                if (piece[i] >= 'a' && piece[i] <= 'z') piece[i] = (proven_byte_t)(piece[i] - 32);
            }
            if (proven_http_exchange_write(x, (proven_mem_view_t){ piece, got.value }) != PROVEN_OK) return;   /* the client left */
        }
        (void)proven_http_exchange_end(x);

    } else if (proven_u8str_view_eq(path, PROVEN_LIT("/whoami"))) {
        proven_net_addr_t peer = proven_http_exchange_peer(x);
        proven_byte_t text[PROVEN_NET_ADDR_TEXT_MAX];
        proven_size_t n = 0;
        if (proven_net_addr_format(&peer, (proven_mem_mut_t){ text, sizeof text }, &n) != PROVEN_OK) return;   /* returning with nothing sent: a 500 */
        (void)proven_http_exchange_respond(x, 200, NULL, 0, (proven_mem_view_t){ text, n });

    } else if (proven_u8str_view_eq(path, PROVEN_LIT("/quit"))) {
        (void)proven_http_exchange_respond(x, 200, NULL, 0, proven_mem_view_from_u8(PROVEN_LIT("bye\n")));
        proven_http_server_stop(app->server);         /* run() returns after this request */

    } else {
        (void)proven_http_exchange_respond(x, 404, NULL, 0, proven_mem_view_from_u8(PROVEN_LIT("no such page\n")));
    }
}

static void serve(void *arg) {
    app_t *app = arg;
    (void)proven_http_server_run(app->server);        /* until stop */
}

/* GET or POST `path` and compare the whole body. */
static bool fetch_is(proven_http_client_t *client, proven_u16 port, const char *method, const char *path, const char *body, proven_u16 status, const char *expect) {
    char url[128];
    int n = snprintf(url, sizeof url, "http://127.0.0.1:%u%s", (unsigned)port, path);
    proven_http_client_request_t req = {
        .method = proven_u8str_view_from_cstr(method),
        .url = { (const proven_byte_t *)url, (proven_size_t)n },
        .body = proven_mem_view_from_u8(proven_u8str_view_from_cstr(body)),
    };
    proven_http_client_response_t resp;
    bool ok = proven_http_client_send(client, &req, &resp) == PROVEN_OK && resp.status == status;
    proven_u8str_t text = {0};
    ok = ok && proven_http_client_read_all(&resp, proven_heap_allocator(), &text, 4096) == PROVEN_OK;
    ok = ok && (expect == NULL || proven_u8str_view_eq(proven_u8str_as_view(&text), proven_u8str_view_from_cstr(expect)));
    proven_u8str_destroy(proven_heap_allocator(), &text);
    proven_http_client_finish(&resp);
    return ok;
}

int main(void) {
    static app_t app;
    proven_allocator_t heap = proven_heap_allocator();

    /* Zero the configuration and set what you need; every zero field has a default. */
    proven_http_server_config_t config = {0};
    config.alloc = heap;
    config.handler = handle;
    config.handler_ctx = &app;
    config.max_body_bytes = 64 * 1024;        /* a larger request body is answered 413 */
    config.head_timeout_ms = 5000;            /* a client has this long to send its request head */
    proven_err_t err = proven_http_server_create(&config, &app.server);
    EXAMPLE_REQUIRE(err == PROVEN_OK, "a server");

    /* Port 0 lets the system choose; `at` says what it chose. */
    proven_net_addr_t at;
    err = proven_http_server_listen(app.server, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &at);
    if (err == PROVEN_ERR_PERMISSION || err == PROVEN_ERR_UNSUPPORTED) {
        printf("no sockets in this environment; nothing to show\n");
        proven_http_server_destroy(app.server);
        return EXAMPLE_OK();
    }
    EXAMPLE_REQUIRE(err == PROVEN_OK, "it listens");

    /* A program with a loop of its own calls poll: one round, bounded by a deadline. */
    EXAMPLE_REQUIRE(proven_http_server_poll(app.server, proven_net_deadline_in(10)) == PROVEN_ERR_TIMEOUT, "nobody has connected yet");

    /* Most programs call run. Here it runs on another thread, so this one can be the client. */
    proven_job_sys_t *threads = NULL;
    proven_job_group_t running;
    proven_job_group_init(&running);
    EXAMPLE_REQUIRE(proven_job_system_init(heap, 1, 4, &threads) == PROVEN_OK &&
                    proven_job_group_submit(threads, &running, serve, &app) == PROVEN_OK, "the server loop is started");

    proven_http_client_config_t client_config = {0};
    client_config.alloc = heap;
    client_config.max_idle_connections = 2;
    proven_http_client_t *client = NULL;
    EXAMPLE_REQUIRE(proven_http_client_create(&client_config, &client) == PROVEN_OK, "a client");

    EXAMPLE_REQUIRE(fetch_is(client, at.port, "GET", "/", "", 200, "hello\n"), "GET /");
    EXAMPLE_REQUIRE(fetch_is(client, at.port, "POST", "/shout", "a body, read and answered in pieces", 200, "A BODY, READ AND ANSWERED IN PIECES"), "POST /shout");
    EXAMPLE_REQUIRE(fetch_is(client, at.port, "GET", "/whoami?verbose=1", "", 200, NULL), "GET /whoami");
    EXAMPLE_REQUIRE(fetch_is(client, at.port, "GET", "/nothing-here", "", 404, "no such page\n"), "a 404 is the handler's to send");
    EXAMPLE_REQUIRE(fetch_is(client, at.port, "GET", "/quit", "", 200, "bye\n"), "GET /quit");

    /* The handler called stop, so run returns; then the server can be taken apart. */
    proven_job_group_wait(threads, &running);
    EXAMPLE_REQUIRE(app.served == 5, "five requests reached the handler");
    EXAMPLE_REQUIRE(proven_http_server_connection_count(app.server) <= 1, "all on one connection, which is still open or just closed");

    proven_http_client_destroy(client);
    proven_http_server_destroy(app.server);   /* closes what is left */
    proven_job_system_close(threads);
    proven_job_system_destroy(threads);
    return EXAMPLE_OK();
}
```

## 6. A client

```text
proven_http_client_config_t config = {0};
config.alloc = proven_heap_allocator();
proven_http_client_create(&config, &client);

proven_http_client_response_t resp;
if (proven_http_client_get(client, PROVEN_LIT("http://example.com/"), &resp) == PROVEN_OK) {
    /* resp.status, resp.headers; then proven_http_client_read until PROVEN_ERR_EOF */
}
proven_http_client_finish(&resp);
proven_http_client_destroy(client);
```

### Reference

| API | Intent | Return |
|---|---|---|
| `proven_http_client_create(&config, &client)` | Make a client. The configuration's strings are copied. | `proven_err_t`: `INVALID_ARG` for an invalid allocator, a proxy that is not `http` or `socks5`, or a control character in a credential; `NOMEM`. |
| `proven_http_client_destroy(client)` | Close idle connections and free it. Finish every response first. | none. |
| `proven_http_client_send(client, &request, &response)` | Send a request and read the head of the response. | `proven_err_t`; see section 7. |
| `proven_http_client_get(client, url, &response)` | `send` with only a URL. | the same. |
| `proven_http_client_read(&response, dest)` | Read more of the body. | `proven_result_size_t`: `.err` is `EOF` at the end; `RESET` for a body cut short; `TIMEOUT`; `INVALID_FORMAT`. |
| `proven_http_client_read_all(&response, alloc, &out, max_bytes)` | Read the rest of the body, appending to a string. | `proven_err_t`: `OUT_OF_BOUNDS` when the body is longer than `max_bytes` - `out` then holds that many. |
| `proven_http_client_finish(&response)` | Release a response; the connection is kept or closed. Safe after a failed send. | none. |

```text
typedef struct {
    proven_allocator_t alloc;               /* required */
    proven_u32 connect_timeout_ms;          /* 0: 30 s */
    proven_u32 io_timeout_ms;               /* 0: 30 s, for each read and each write */
    proven_size_t max_head_bytes;           /* 0: 16 KiB */
    proven_u32 max_redirects;               /* 0: none are followed */
    proven_size_t max_idle_connections;     /* 0: every connection is closed after use */
    proven_u8str_view_t proxy;              /* "http://host:port" or "socks5://host:port" */
    proven_u8str_view_t username, password; /* offered when a server answers 401 */
    proven_u8str_view_t user_agent;         /* empty: none is sent */
    proven_http_cookie_jar_t *cookies;      /* NULL: cookies are ignored */
    proven_http_tls_wrap_fn tls_wrap;       /* NULL: an https URL is PROVEN_ERR_UNSUPPORTED */
    void *tls_ctx;
} proven_http_client_config_t;
```

**A zeroed configuration does the least.** With only `alloc` set, a client follows no redirect,
keeps no connection, offers no credentials, stores no cookie and sends no `User-Agent`. Each of
those is something you turn on, knowing you did.

**One client, one thread.** A client is not safe to use from several threads at once. Give each
thread its own; they are cheap.

## 7. One request, one response

```text
typedef struct {
    proven_u8str_view_t method;             /* empty: GET */
    proven_u8str_view_t url;                /* absolute: http://host[:port]/path?query */
    const proven_http_header_t *headers;    /* extra header fields */
    proven_size_t header_count;
    proven_mem_view_t body;                 /* a body in memory: sent with Content-Length */
    proven_reader_t body_stream;            /* or a stream: sent chunked */
} proven_http_client_request_t;

typedef struct {
    proven_u16 status;
    proven_u8str_view_t reason;
    const proven_http_header_t *headers;
    proven_size_t header_count;
    proven_u8str_view_t url;                /* where this response came from, after redirects */
    proven_u32 redirects;
    void *internal;
} proven_http_client_response_t;
```

`proven_http_client_send` returns when the status and headers have arrived. The body is still
on the connection, and you read it in pieces into a buffer of your own - so a response of any
size costs the memory you chose, and a download can go straight to a file.

**An error status is not an error.** A `404` or a `500` is a response: `send` returns
`PROVEN_OK` and you look at `status`. `send` fails when there is *no* response to look at:

| `send` returns | Because |
|---|---|
| `PROVEN_ERR_INVALID_FORMAT` | The URL is not absolute, or the response was not HTTP - or was more than eight interim (`1xx`) responses with no final one. |
| `PROVEN_ERR_UNSUPPORTED` | A scheme other than `http` - or `https` without a `tls_wrap`. |
| `PROVEN_ERR_INVALID_ARG` | A header the client writes itself was given: `Host`, `Content-Length`, `Transfer-Encoding`, `Connection`. Or both a body and a stream. |
| `PROVEN_ERR_NOT_FOUND` | The host name does not resolve. |
| `PROVEN_ERR_REFUSED`, `PROVEN_ERR_UNREACHABLE`, `PROVEN_ERR_TIMEOUT`, `PROVEN_ERR_RESET` | The connection: Chapter 9's meanings. |
| `PROVEN_ERR_OUT_OF_BOUNDS` | The request or the response head is larger than `max_head_bytes`. |
| `PROVEN_ERR_PERMISSION` | A proxy refused. |
| `PROVEN_ERR_UNTRUSTED`, `PROVEN_ERR_EXPIRED`, `PROVEN_ERR_NOT_YET_VALID`, `PROVEN_ERR_NAME_MISMATCH`, `PROVEN_ERR_PROTOCOL` | From `tls_wrap`: the server could not be verified, or the TLS handshake failed (Chapter 14, section 8). Nothing of the request was sent. |

### Cautions, and what goes wrong

**Always call `finish`.** A response holds a connection and memory until it is finished.
`finish` is safe on a response whose `send` failed, so the simple shape is the right one: `send`,
use it if that succeeded, `finish` either way.

**`PROVEN_ERR_EOF` is the end of the body; `PROVEN_ERR_RESET` is not.** A body that stops before
its announced length - the server crashed, the network dropped - is reported as
`PROVEN_ERR_RESET`, never as `EOF`. Treat only `EOF` as a complete body.

Wrong:

```text
while (proven_http_client_read(&resp, buf).err == PROVEN_OK) save(buf);   /* wrong: stops on RESET and calls the file done */
commit_download();
```

Correct - keep the result, and commit only when it is `PROVEN_ERR_EOF`.

**`read_all` needs a limit, and you choose it.** A server decides how much it sends. `max_bytes`
is the most you will hold; a longer body is `PROVEN_ERR_OUT_OF_BOUNDS`, not an allocation that
grows until the machine has none left.

**The response's views die with it.** `headers`, `reason` and `url` point into memory that
`finish` frees. Copy what you keep.

**A stream body is sent once.** A body read from `body_stream` cannot be read again, so a
request that has one is not repeated - not for a redirect that needs the body, not for a
challenge, not when a kept connection turns out to be dead. You get the response that would
have caused the repeat, and decide.

**`io_timeout_ms` bounds each wait, not the whole request.** A server that sends one byte every
ten seconds never trips a thirty-second timeout. For a limit on the whole exchange, note the
time before `send` and stop reading when you have had enough.

Name resolution has no deadline, as in Chapter 9: `connect_timeout_ms` starts once the name has
resolved.

## 8. Redirects, challenges and cookies

**Redirects** (`301`, `302`, `303`, `307`, `308`) are followed up to `max_redirects`, each
`Location` resolved against the URL that sent it. `response.url` and `response.redirects` say
where you ended up and how.

| Status | The next request |
|---|---|
| `303` | `GET`, without the body - whatever the method was |
| `301`, `302` | `GET` without the body if the method was `POST`; otherwise the same request again |
| `307`, `308` | The same request again, body included |

A redirect is *not* followed, and its `3xx` response is handed to you instead, when:

- the limit is reached;
- it leads from `https` to `http` - the client does not step down from an encrypted connection
  because a server said to;
- it leads to a scheme other than `http` and `https`;
- the request would have to be sent again but its body was a stream.

**Credentials stay with the host they were given for.** When a redirect leads to another host,
port or scheme, the `Authorization`, `Cookie` and `Proxy-Authorization` headers you supplied are
left behind, and a `401` from the new place is not answered with the configured user name and
password. A server you trust with a token must not be able to send that token to a server of
its choosing.

**What the client does not check is where a redirect leads.** A server can redirect to
`http://127.0.0.1/` or to an address inside your network. If the URLs come from people you do
not trust - a "fetch this link" feature - the destination of every hop is yours to check: set
`max_redirects` to 0, read `Location` yourself, and decide.

**Challenges.** With `username` set, a `401` carrying a Digest or Basic challenge is answered
once: Digest if it is offered, Basic otherwise. A Digest nonce reported `stale` is answered once
more. A wrong password therefore costs two requests, not a loop. Credentials are never sent
before a server asks for them - and when it does ask over plain HTTP with Basic, they are sent
readable. Section 1 applies.

**Cookies.** With a jar in `cookies`, every response's `Set-Cookie` headers are stored and every
request carries the cookies that match. A `Cookie` header of your own in a request replaces the
jar's for that request. The jar is the host-only one of Chapter 10, section 12, with all that
implies.

## 9. Connections and proxies

**Reuse.** With `max_idle_connections` above zero, a connection whose response was read to its
end goes back to the client and carries the next request to the same scheme, host and port. A
response finished *before* its body was read cannot be reused - the rest of the body is still
on it - and is closed. To keep the connection, read to `PROVEN_ERR_EOF` before `finish`.

**A kept connection can be dead.** Servers close idle connections without notice. If a request
on a reused connection fails before any byte of an answer arrives, the client sends it again on
a new connection, once. That is safe exactly because nothing came back - and it is not done for
a stream body, which cannot be sent twice.

**Proxies.** `proxy` is `http://[user:password@]host:port` or
`socks5://[user:password@]host:port`.

| Proxy | For an `http` URL | For an `https` URL |
|---|---|---|
| HTTP | The proxy is sent the whole URL and fetches it. | The proxy is asked to `CONNECT` to `host:port`; the request then travels through that tunnel. |
| SOCKS5 | A tunnel to `host:port`, then an ordinary request. | The same, with TLS inside it. |

- The target's name is given to the proxy, not resolved here - with SOCKS5 as a name, which is
  what `socks5h` means elsewhere. The machine running the client does not look the name up.
- Credentials in the proxy URL are sent to the proxy - `Proxy-Authorization: Basic` for an HTTP
  proxy, the user/password method of RFC 1929 for SOCKS5 - and to nobody else: they do not
  travel inside a tunnel to the origin. Both forms cross the network to the proxy readable.
- A proxy that refuses (`407`, `403`, a SOCKS5 "not allowed") is `PROVEN_ERR_PERMISSION`.
- An HTTP proxy sees, and can change, everything in a plain `http` exchange. That is what a
  proxy is.

Proxy settings are not read from the environment. `HTTP_PROXY` and its relatives are a
convention with a history of surprises; if your program honours them, it reads them and sets
`proxy` itself.

## 10. Where TLS will attach

```text
typedef proven_err_t (*proven_http_tls_wrap_fn)(void *ctx, proven_transport_t plain,
                                                proven_u8str_view_t host,
                                                proven_net_deadline_t until,
                                                proven_transport_t *out);
```

For an `https` URL the client connects - through the proxy tunnel if there is one - and then
calls `tls_wrap` with the connected transport and the host name. The function performs the
handshake and returns a transport that encrypts; the client speaks HTTP over that and never
learns the difference. This is Chapter 9's transport interface doing the job it was shaped for.

The library supplies one: `proven_tls_http_wrap`, with a `proven_tls_config_t *` as its
context ([Chapter 14](manual-14-tls.md), section 3). The seam stays an interface so that the
client's behaviour around it - no downgrade on redirect, credentials kept to their origin, the
tunnel before the handshake - does not depend on which TLS is behind it, and so that a program
with another TLS implementation can attach that instead.

**If you write one, it must verify the server.** `host` is passed so that the certificate can be
checked against it. A wrap that encrypts without verifying gives a connection that is private
from everyone except whoever is in the middle of it - and returns `PROVEN_OK`, so nothing
downstream can tell. Return `PROVEN_ERR_UNTRUSTED` when the peer cannot be verified; the client
passes it up and sends nothing.

Compiled and run by the test suite:

<!-- example: manual/examples/en/ex_11_http_client.c -->
```c
/*
 * An HTTP client: from a URL to the bytes, with what lies between - a redirect, a login, a
 * cookie - handled by the client and visible in the response.
 *
 * The server it talks to is in this same program, on another thread, so the example needs no
 * network. Its handler is the first half of the file; the client is in main.
 */

static proven_http_server_t *g_server;

static void site(void *ctx, proven_http_exchange_t *x) {
    (void)ctx;
    const proven_http_request_t *req = proven_http_exchange_request(x);
    proven_u8str_view_t value;

    if (proven_u8str_view_eq(req->target, PROVEN_LIT("/old-home"))) {
        proven_http_header_t h = { PROVEN_LIT("Location"), PROVEN_LIT("/home") };
        (void)proven_http_exchange_respond(x, 301, &h, 1, (proven_mem_view_t){0});
    } else if (proven_u8str_view_eq(req->target, PROVEN_LIT("/home"))) {
        proven_http_header_t h = { PROVEN_LIT("Set-Cookie"), PROVEN_LIT("visited=yes; Path=/") };
        (void)proven_http_exchange_respond(x, 200, &h, 1, proven_mem_view_from_u8(PROVEN_LIT("welcome home")));
    } else if (proven_u8str_view_eq(req->target, PROVEN_LIT("/cookie"))) {
        bool has = proven_http_header_find(req->headers, req->header_count, PROVEN_LIT("Cookie"), &value);
        (void)proven_http_exchange_respond(x, 200, NULL, 0, proven_mem_view_from_u8(has ? value : PROVEN_LIT("none")));
    } else if (proven_u8str_view_eq(req->target, PROVEN_LIT("/private"))) {
        /* "Basic" + base64("ada:lovelace") */
        if (proven_http_header_find(req->headers, req->header_count, PROVEN_LIT("Authorization"), &value) &&
            proven_u8str_view_eq(value, PROVEN_LIT("Basic YWRhOmxvdmVsYWNl"))) {
            (void)proven_http_exchange_respond(x, 200, NULL, 0, proven_mem_view_from_u8(PROVEN_LIT("the private page")));
        } else {
            proven_http_header_t h = { PROVEN_LIT("WWW-Authenticate"), PROVEN_LIT("Basic realm=\"example\"") };
            (void)proven_http_exchange_respond(x, 401, &h, 1, proven_mem_view_from_u8(PROVEN_LIT("who are you?")));
        }
    } else if (proven_u8str_view_eq(req->target, PROVEN_LIT("/count"))) {
        /* Answers with how many bytes the request body had. */
        proven_byte_t piece[512];
        proven_u64 total = 0;
        for (;;) {
            proven_result_size_t got = proven_http_exchange_read(x, (proven_mem_mut_t){ piece, sizeof piece });
            if (got.err != PROVEN_OK) break;
            total += got.value;
        }
        char text[32];
        int n = snprintf(text, sizeof text, "%llu bytes", (unsigned long long)total);
        (void)proven_http_exchange_respond(x, 200, NULL, 0, (proven_mem_view_t){ (const proven_byte_t *)text, (proven_size_t)n });
    } else {
        (void)proven_http_exchange_respond(x, 404, NULL, 0, proven_mem_view_from_u8(PROVEN_LIT("not found")));
    }
}

static void serve(void *arg) {
    (void)arg;
    (void)proven_http_server_run(g_server);
}

static char g_url[128];
static proven_u8str_view_t url_for(proven_u16 port, const char *path) {
    int n = snprintf(g_url, sizeof g_url, "http://127.0.0.1:%u%s", (unsigned)port, path);
    return (proven_u8str_view_t){ (const proven_byte_t *)g_url, (proven_size_t)n };
}

static bool view_is(proven_u8str_view_t v, const char *text) {
    return proven_u8str_view_eq(v, proven_u8str_view_from_cstr(text));
}

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();

    // ---- the server, to have something to talk to ---------------------------
    proven_http_server_config_t server_config = {0};
    server_config.alloc = heap;
    server_config.handler = site;
    EXAMPLE_REQUIRE(proven_http_server_create(&server_config, &g_server) == PROVEN_OK, "a server");
    proven_net_addr_t at;
    proven_err_t err = proven_http_server_listen(g_server, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &at);
    if (err == PROVEN_ERR_PERMISSION || err == PROVEN_ERR_UNSUPPORTED) {
        printf("no sockets in this environment; nothing to show\n");
        proven_http_server_destroy(g_server);
        return EXAMPLE_OK();
    }
    EXAMPLE_REQUIRE(err == PROVEN_OK, "it listens");
    proven_job_sys_t *threads = NULL;
    proven_job_group_t running;
    proven_job_group_init(&running);
    EXAMPLE_REQUIRE(proven_job_system_init(heap, 1, 4, &threads) == PROVEN_OK &&
                    proven_job_group_submit(threads, &running, serve, NULL) == PROVEN_OK, "its loop runs on another thread");

    // ---- the client ----------------------------------------------------------
    proven_http_cookie_jar_t jar;
    EXAMPLE_REQUIRE(proven_http_cookie_jar_init(&jar, heap, 32) == PROVEN_OK, "a cookie jar");

    /* Zero it and set what you need. With every field zero but the allocator, a client
     * follows no redirect, keeps no connection, sends no credentials and holds no cookies. */
    proven_http_client_config_t config = {0};
    config.alloc = heap;
    config.max_redirects = 5;
    config.max_idle_connections = 4;          /* keep connections for the next request */
    config.io_timeout_ms = 5000;              /* no read or write waits longer */
    config.user_agent = PROVEN_LIT("manual-example/1.0");
    config.username = PROVEN_LIT("ada");      /* offered only when a server asks */
    config.password = PROVEN_LIT("lovelace");
    config.cookies = &jar;
    proven_http_client_t *client = NULL;
    EXAMPLE_REQUIRE(proven_http_client_create(&config, &client) == PROVEN_OK, "a client");

    /* GET. The call returns when the status and headers have arrived; the body is still on
     * the wire, and you read it at your own pace into your own buffer. */
    proven_http_client_response_t resp;
    err = proven_http_client_get(client, url_for(at.port, "/old-home"), &resp);
    EXAMPLE_REQUIRE(err == PROVEN_OK && resp.status == 200, "the 301 was followed to a 200");
    EXAMPLE_REQUIRE(resp.redirects == 1 && resp.url.size > 5 && view_is((proven_u8str_view_t){ resp.url.ptr + resp.url.size - 5, 5 }, "/home"),
                    "and the response says where it ended up");
    proven_byte_t buf[64];
    proven_size_t have = 0;
    for (;;) {
        proven_result_size_t got = proven_http_client_read(&resp, (proven_mem_mut_t){ buf + have, sizeof buf - have });
        if (got.err == PROVEN_ERR_EOF) break;             /* the whole body has been read */
        EXAMPLE_REQUIRE(got.err == PROVEN_OK, "a piece of the body");
        if (got.err != PROVEN_OK) break;                  /* PROVEN_ERR_RESET: it was cut short */
        have += got.value;
    }
    EXAMPLE_REQUIRE(view_is((proven_u8str_view_t){ buf, have }, "welcome home"), "the body");
    /* Always finish. Read to the end, the connection goes back to the client for reuse. */
    proven_http_client_finish(&resp);

    /* That response set a cookie; the jar has it and the next request sends it. */
    EXAMPLE_REQUIRE(proven_http_cookie_jar_count(&jar) == 1, "one cookie in the jar");
    proven_u8str_t text = {0};
    err = proven_http_client_get(client, url_for(at.port, "/cookie"), &resp);
    EXAMPLE_REQUIRE(err == PROVEN_OK && proven_http_client_read_all(&resp, heap, &text, 1024) == PROVEN_OK, "read_all: the whole body, up to a limit you name");
    EXAMPLE_REQUIRE(view_is(proven_u8str_as_view(&text), "visited=yes"), "the server saw the cookie");
    proven_http_client_finish(&resp);

    /* A page behind a login. The first answer is a 401 with a challenge; the client answers
     * it with the configured credentials and you see only the result. */
    EXAMPLE_REQUIRE(proven_u8str_reset(&text) == PROVEN_OK, "reuse the string");
    err = proven_http_client_get(client, url_for(at.port, "/private"), &resp);
    EXAMPLE_REQUIRE(err == PROVEN_OK && resp.status == 200 && proven_http_client_read_all(&resp, heap, &text, 1024) == PROVEN_OK, "let in");
    EXAMPLE_REQUIRE(view_is(proven_u8str_as_view(&text), "the private page"), "the page");
    proven_http_client_finish(&resp);

    /* A request with everything set: method, extra headers, and a body from a stream, which
     * is sent chunked - so a file or a pipe can be uploaded without knowing its size. */
    proven_reader_view_t source;
    proven_http_header_t headers[1] = { { PROVEN_LIT("Content-Type"), PROVEN_LIT("text/plain") } };
    proven_http_client_request_t req = {
        .method = PROVEN_LIT("POST"),
        .url = url_for(at.port, "/count"),
        .headers = headers,
        .header_count = 1,
        .body_stream = proven_reader_from_view(&source, PROVEN_LIT("nineteen bytes here")),
    };
    EXAMPLE_REQUIRE(proven_u8str_reset(&text) == PROVEN_OK, "reuse the string");
    err = proven_http_client_send(client, &req, &resp);
    EXAMPLE_REQUIRE(err == PROVEN_OK && proven_http_client_read_all(&resp, heap, &text, 1024) == PROVEN_OK, "a POST from a stream");
    EXAMPLE_REQUIRE(view_is(proven_u8str_as_view(&text), "19 bytes"), "the server counted them");
    proven_http_client_finish(&resp);

    /* An error status is a response, not an error: the call succeeds and you look at status. */
    err = proven_http_client_get(client, url_for(at.port, "/missing"), &resp);
    EXAMPLE_REQUIRE(err == PROVEN_OK && resp.status == 404, "a 404 arrives as a response");
    proven_http_client_finish(&resp);

    /* An error is when there is no response to look at. */
    err = proven_http_client_get(client, PROVEN_LIT("https://127.0.0.1/"), &resp);
    EXAMPLE_REQUIRE(err == PROVEN_ERR_UNSUPPORTED, "https without a tls_wrap: the client says so rather than sending in the clear");
    proven_http_client_finish(&resp);         /* harmless after a failure */

    proven_u8str_destroy(heap, &text);
    proven_http_client_destroy(client);
    proven_http_cookie_jar_destroy(&jar);

    proven_http_server_stop(g_server);
    proven_job_group_wait(threads, &running);
    proven_http_server_destroy(g_server);
    proven_job_system_close(threads);
    proven_job_system_destroy(threads);
    return EXAMPLE_OK();
}
```

## 11. Changing protocol

A request may ask that the connection stop carrying HTTP: `Connection: Upgrade` and
`Upgrade: <protocol>`. If the server agrees it answers `101 Switching Protocols`, and every
byte after that belongs to the other protocol. Both drivers can hand the connection over at
that point.

| API | Intent | Return |
|---|---|---|
| `proven_http_exchange_upgrade(x, protocol, headers, count, &transport, &early)` | In a handler: answer `101` with `Upgrade: protocol` and `Connection: Upgrade`, and take the connection out of the server. | `proven_err_t`: `INVALID_STATE` when a response was begun, the request has an unread body, or it is HTTP/1.0; `INVALID_ARG` for a header the server owns (`Upgrade` included); `NOMEM`; `OUT_OF_BOUNDS`; `TIMEOUT`, `RESET`. |
| `request.upgrade` | In a client request: the protocol to ask for. The client writes `Connection: Upgrade` and `Upgrade` itself. | - |
| `proven_http_client_upgrade(&response, &transport, &early)` | After a `101`: take the connection out of the client. | `proven_err_t`: `INVALID_STATE` when the response is not a `101` or was already taken. |

Either way you receive a `proven_transport_t` that **owns** the connection. It is not the
server's or the client's any more: it does not count against `max_connections`, none of their
timeouts apply to it, it will not be reused, and it stays open until you call
`proven_transport_close`. A server handler may keep it after it returns.

**`early` is the start of the other protocol.** A peer need not wait for the handshake to
finish before sending, so the first bytes of the new protocol can arrive in the same read as
the HTTP head. They are in `early` - a view that dies with the exchange, or at
`proven_http_client_finish` - and a program that ignores them loses the beginning of the
conversation, on some connections and not others.

**Check before you agree.** `proven_http_exchange_upgrade` writes the `101`; it does not judge
whether the request was a well-formed request for the protocol you mean. For WebSocket that
check, and the reply header it needs, are `proven_ws_check_request` and `proven_ws_accept_key`
- or use [Chapter 12](manual-12-websocket.md)'s `proven_ws_conn_accept`, which does all of it.

**After the hand-over, the limits are yours.** The server's promise that every wait is bounded
ends at the `101`. A read on the transport with `PROVEN_NET_NO_DEADLINE` waits for ever.

Compiled and run by the test suite:

<!-- example: manual/examples/en/ex_11_http_upgrade.c -->
```c
/*
 * Changing protocol: a request asks to stop speaking HTTP on this connection, the server
 * agrees with a 101, and from then on the same connection carries something else.
 *
 * The "something else" here is a toy - lines of text, sent back in capitals - because the
 * point is the hand-over. WebSocket is the protocol people usually mean, and chapter 12's
 * connection does these steps for you.
 */

static proven_http_server_t *g_server;

static void handle(void *ctx, proven_http_exchange_t *x) {
    (void)ctx;
    const proven_http_request_t *request = proven_http_exchange_request(x);
    /* Is this a request to change to OUR protocol? Checking is the handler's job. */
    if (!proven_http_header_has_token(request->headers, request->header_count, PROVEN_LIT("Upgrade"), PROVEN_LIT("shout")) ||
        !proven_http_header_has_token(request->headers, request->header_count, PROVEN_LIT("Connection"), PROVEN_LIT("Upgrade"))) {
        (void)proven_http_exchange_respond(x, 426, NULL, 0, proven_mem_view_from_u8(PROVEN_LIT("this address speaks only shout\n")));
        return;
    }

    /* The server writes the 101, and the connection is ours: a transport that owns the socket. */
    proven_transport_t t;
    proven_mem_view_t early;                /* bytes the client sent without waiting for the 101 */
    if (proven_http_exchange_upgrade(x, PROVEN_LIT("shout"), NULL, 0, &t, &early) != PROVEN_OK) return;

    /* From here on this is not HTTP and none of the server's limits apply: every wait needs
     * a deadline of our own. `early` dies when the handler returns, so use it first. */
    proven_byte_t line[64];
    proven_size_t n = early.size < sizeof line ? early.size : sizeof line;
    for (proven_size_t i = 0; i < n; ++i) line[i] = early.ptr[i];
    while (n < sizeof line && (n == 0 || line[n - 1] != '\n')) {
        proven_result_size_t got = proven_transport_read(t, (proven_mem_mut_t){ line + n, sizeof line - n }, proven_net_deadline_in(5000));
        if (got.err != PROVEN_OK) break;
        n += got.value;
    }
    for (proven_size_t i = 0; i < n; ++i) if (line[i] >= 'a' && line[i] <= 'z') line[i] = (proven_byte_t)(line[i] - 32);
    (void)proven_transport_write_all(t, (proven_mem_view_t){ line, n }, proven_net_deadline_in(5000));
    /* It could also be kept and used after the handler returns. Either way, closing it is ours. */
    (void)proven_transport_close(t);
}

static void serve(void *arg) {
    (void)arg;
    (void)proven_http_server_run(g_server);
}

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();
    proven_http_server_config_t server_config = {0};
    server_config.alloc = heap;
    server_config.handler = handle;
    EXAMPLE_REQUIRE(proven_http_server_create(&server_config, &g_server) == PROVEN_OK, "a server");
    proven_net_addr_t at;
    proven_err_t err = proven_http_server_listen(g_server, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &at);
    if (err == PROVEN_ERR_PERMISSION || err == PROVEN_ERR_UNSUPPORTED) {
        printf("no sockets in this environment; nothing to show\n");
        proven_http_server_destroy(g_server);
        return EXAMPLE_OK();
    }
    EXAMPLE_REQUIRE(err == PROVEN_OK, "it listens");
    proven_job_sys_t *threads = NULL;
    proven_job_group_t running;
    proven_job_group_init(&running);
    EXAMPLE_REQUIRE(proven_job_system_init(heap, 1, 4, &threads) == PROVEN_OK &&
                    proven_job_group_submit(threads, &running, serve, NULL) == PROVEN_OK, "its loop runs on another thread");

    proven_http_client_config_t client_config = { .alloc = heap, .max_idle_connections = 2 };
    proven_http_client_t *client = NULL;
    EXAMPLE_REQUIRE(proven_http_client_create(&client_config, &client) == PROVEN_OK, "a client");
    char url[64];
    int url_len = snprintf(url, sizeof url, "http://127.0.0.1:%u/", (unsigned)at.port);

    /* `upgrade` names the protocol; the client writes `Connection: Upgrade` and `Upgrade`. */
    proven_http_client_request_t request = { .url = { (const proven_byte_t *)url, (proven_size_t)url_len }, .upgrade = PROVEN_LIT("shout") };
    proven_http_client_response_t response;
    err = proven_http_client_send(client, &request, &response);
    EXAMPLE_REQUIRE(err == PROVEN_OK && response.status == 101, "the server agreed: 101 Switching Protocols");

    /* A 101 is a response like any other until you take its connection. */
    proven_transport_t t;
    proven_mem_view_t early;
    EXAMPLE_REQUIRE(proven_http_client_upgrade(&response, &t, &early) == PROVEN_OK, "the connection is ours now");
    proven_http_client_finish(&response);       /* still required; it no longer touches the connection */

    proven_byte_t reply[16];
    proven_size_t have = 0;
    EXAMPLE_REQUIRE(proven_transport_write_all(t, proven_mem_view_from_u8(PROVEN_LIT("hello\n")), proven_net_deadline_in(5000)).err == PROVEN_OK, "a line, in the new protocol");
    while (have < 6) {
        proven_result_size_t got = proven_transport_read(t, (proven_mem_mut_t){ reply + have, sizeof reply - have }, proven_net_deadline_in(5000));
        if (got.err != PROVEN_OK) break;
        have += got.value;
    }
    EXAMPLE_REQUIRE(have == 6 && proven_u8str_view_eq((proven_u8str_view_t){ reply, 6 }, PROVEN_LIT("HELLO\n")), "and the answer, in capitals");
    (void)proven_transport_close(t);            /* closes the socket and frees the transport */

    /* Without `upgrade` the same address answers as HTTP, and there is nothing to take. */
    err = proven_http_client_get(client, (proven_u8str_view_t){ (const proven_byte_t *)url, (proven_size_t)url_len }, &response);
    EXAMPLE_REQUIRE(err == PROVEN_OK && response.status == 426, "an ordinary request is told what this address speaks");
    EXAMPLE_REQUIRE(proven_http_client_upgrade(&response, &t, &early) == PROVEN_ERR_INVALID_STATE, "a response that is not a 101 has no connection to give");
    proven_http_client_finish(&response);

    proven_http_client_destroy(client);
    proven_http_server_stop(g_server);
    proven_job_group_wait(threads, &running);
    proven_http_server_destroy(g_server);
    proven_job_system_close(threads);
    proven_job_system_destroy(threads);
    return EXAMPLE_OK();
}
```

## 12. What is not here

- **TLS**, in both directions. Section 1.
- **HTTP/2 and HTTP/3.**
- **Compression, unless asked for.** With nothing set, no `Accept-Encoding` is sent, no
  content coding is decoded, and no response is compressed. Setting `decompress` in the
  client's configuration, and calling `proven_http_exchange_compress` in a handler, turn on
  gzip in each direction: [Chapter 16](manual-16-compression.md), section 6.
- **WebSocket** is not in these two headers: it is [Chapter 12](manual-12-websocket.md), built on section 11.
- **A router, static files, sessions, middleware** on the server; **retries, caching, a
  connection limit per host** on the client.
- **Trailers**, in either direction.
- **`Expect: 100-continue` from the client.** A body is sent without asking first.
- **Proxy auto-configuration, `NO_PROXY`, and Digest authentication to a proxy.**
- **Graceful shutdown with a drain period.** `stop` ends the loop; connections waiting for a
  request are closed by `destroy`.
