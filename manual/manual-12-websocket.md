# Chapter 12: WebSocket

**Part V - Talking to the operating system. Prerequisites: [Chapter 11](manual-11-http-client-server.md)
(the HTTP client and server, and its section on changing protocol); [Chapter 10](manual-10-http.md)
for the codec half alone.**
**After this chapter** you can open a WebSocket connection from either side, exchange text and
binary messages, close it properly, and say what your program does when the other side breaks
the rules - or you can take the frames apart yourself, with no socket in sight.

This chapter covers `ws.h` and `ws_conn.h`, and - in section 9 - compressed messages in both
of them and in the event-driven server of Chapter 15. `ws.h` is pure text-and-bytes handling, available in
a [freestanding](manual-freestanding.md) build and untouched by `PROVEN_NO_NET`. `ws_conn.h`
needs sockets: it is hosted-only and `PROVEN_NO_NET` leaves it out.

## Table of contents

1. [What WebSocket is, in one page](#1-what-websocket-is-in-one-page)
2. [A connection](#2-a-connection)
3. [Sending and receiving](#3-sending-and-receiving)
4. [Closing, and being closed on](#4-closing-and-being-closed-on)
5. [Where a server's connections live](#5-where-a-servers-connections-live)
6. [The codec: the handshake](#6-the-codec-the-handshake)
7. [The codec: frames](#7-the-codec-frames)
8. [The codec: the decoder](#8-the-codec-the-decoder)
9. [Compressed messages](#9-compressed-messages)
10. [What is not here](#10-what-is-not-here)

## 1. What WebSocket is, in one page

HTTP is one question and one answer. WebSocket (RFC 6455) is what a connection becomes when
both sides want to speak whenever they have something to say: after one HTTP request and a
`101` response, the same TCP connection carries *messages* in both directions until one side
closes it.

```text
client                                         server
  | GET /chat HTTP/1.1                           |
  | Upgrade: websocket                           |
  | Sec-WebSocket-Key: <16 random bytes>         |
  |--------------------------------------------->|
  |            HTTP/1.1 101 Switching Protocols  |
  |            Sec-WebSocket-Accept: <the answer>|
  |<---------------------------------------------|
  |  message  --->         <---  message         |      any number, either way, any time
  |  ping     --->         <---  pong            |
  |  close    --->         <---  close           |      then the connection ends
```

A message is **text** (UTF-8, and checked) or **binary** (any bytes). On the wire a message is
one or more *frames*; a long or streamed message is split into fragments, and the receiver
joins them. Three small *control* frames travel between them: **ping** and **pong**, to find
out whether the other end is still there, and **close**, which carries a code and a reason.

Two rules look odd until you know why. A client **masks** every frame - XORs the payload with a
random four-byte key - and a server never does. The mask hides nothing; it stops a script in a
web page from choosing the exact bytes that cross the network, which was once enough to poison
a caching proxy. And the handshake's key and answer are not authentication: they only prove
that the server really speaks WebSocket and is not an HTTP server echoing things back.

**`ws://` is not protected.** A `ws://` connection is readable and changeable by anyone on the path,
exactly as Chapter 11 says of `http://`. `wss://` is the same protocol over TLS: give the HTTP
client the TLS wrap, or the HTTP server a TLS configuration ([Chapter 14](manual-14-tls.md)), and
nothing else in this chapter changes.

## 2. A connection

```text
/* a client */
proven_ws_conn_config_t config = { .alloc = proven_heap_allocator() };
proven_ws_conn_t *ws;
if (proven_ws_conn_connect(http_client, PROVEN_LIT("ws://example.com/feed"), NULL, 0,
                           PROVEN_LIT(""), &config, &ws, NULL) == PROVEN_OK) {
    proven_ws_conn_send_text(ws, PROVEN_LIT("hello"));
    ...
    proven_ws_conn_close(ws, PROVEN_WS_CLOSE_NORMAL, PROVEN_LIT(""));
    proven_ws_conn_destroy(ws);
}
```

### Reference

| API | Intent | Return |
|---|---|---|
| `proven_ws_conn_connect(client, url, headers, count, protocols, &config, &ws, &http_status)` | Connect as a client, through an HTTP client - so its proxy, timeouts and `tls_wrap` apply. `url` is `ws://` or `wss://`. `http_status` (optional) receives what the server answered. | `proven_err_t`: `REFUSED` when the answer was not `101` (see `http_status`); `INVALID_FORMAT` when the `101` did not answer this handshake; `UNSUPPORTED` for `wss` without `tls_wrap`; `IO` when no random bytes could be drawn; the errors of `proven_http_client_send`. |
| `proven_ws_conn_accept(x, protocol, &config, &ws)` | Accept as a server, inside an HTTP handler: check the request, answer `101`, take the connection. `protocol` is the subprotocol to select, or empty. | `proven_err_t`: `NOT_FOUND` - not an upgrade, nothing sent, answer it as HTTP; `UNSUPPORTED` - another version, a `426` was sent; `INVALID_FORMAT` - a malformed upgrade, a `400` was sent; `INVALID_ARG` - a `protocol` the client did not offer, nothing sent. |
| `proven_ws_conn_open(transport, is_server, early, &config, &ws)` | Make a connection over a transport already past its handshake. On success it owns the transport. | `proven_err_t`: `INVALID_ARG`; `NOMEM`; `OUT_OF_BOUNDS` when `early` exceeds 16 KiB; `IO`. |
| `proven_ws_conn_destroy(ws)` | Close the transport and free the connection. | none. |
| `proven_ws_conn_protocol(ws)` | The subprotocol agreed; empty when none. | `proven_u8str_view_t`. |

```text
typedef struct {
    proven_allocator_t alloc;           /* required */
    proven_size_t max_message_bytes;    /* 0: 1 MiB - the largest message accepted */
    proven_u32 io_timeout_ms;           /* 0: 30 s - for each write */
    proven_u32 close_timeout_ms;        /* 0: 5 s - how long close waits for the peer */
} proven_ws_conn_config_t;
```

A connection costs about 21 KiB when it is made - a 16 KiB read buffer and a 4 KiB send buffer
- plus the largest message it has had to assemble, which is kept for the next one and is never
more than `max_message_bytes`.

### Cautions, and what goes wrong

**A connection is for one thread at a time.** Not even "one thread sends while another
receives": receiving answers pings, which is a send. Two threads on one connection will
interleave the bytes of two frames, and the peer will close with a protocol error that
points nowhere near the cause. Give the connection to one thread. One lock around every call
keeps the bytes apart, but `proven_ws_conn_receive` holds it for as long as it waits, and every
send waits with it - so a program in which one thread waits for messages while another sends (a
chat client) wants the loop's connection instead: [Chapter 15](manual-15-event-loop.md),
section 11, "Two threads, one connection", has the program.

**`max_message_bytes` is how much a peer can make you hold.** A message is assembled in memory
before you see it. The limit is the most one connection will allocate for that, whatever is
sent; a message past it ends the connection with code 1009. Set it for the messages you expect,
not for the largest you can imagine.

**Subprotocols are an agreement, not a decoration.** The client offers names; the server picks
one of them or none. `proven_ws_conn_accept` refuses a name the client did not offer, and
`proven_ws_conn_connect` refuses a server that picks one. If your protocol has versions, this
is where the two sides find out which one they share - check `proven_ws_conn_protocol` and do
not assume.

**The `Origin` header is the server's to check.** A browser sends `Origin` with every WebSocket
handshake and does *not* apply the same-origin policy to it: any web page can open a WebSocket
to your server, and the browser will attach the user's cookies. If the server is reachable from
browsers and uses cookies, compare `Origin` with the sites you expect before accepting, or any
page the user visits can act as them.

`proven_ws_check_origin(&request, allowed, count)` is that comparison. An origin is written as
a browser writes it - `https://app.example`, the port only when it is not the scheme's own, no
path, no final slash - and is compared whole, letter case aside.

| Result | Meaning | What a server does |
|---|---|---|
| `PROVEN_OK` | One `Origin`, and it is in the list. | Accepts. |
| `PROVEN_ERR_NOT_FOUND` | No `Origin`: the client is not a browser. | Decides - a program that is not a browser carries no user's cookies, and could have written any `Origin` it liked. |
| `PROVEN_ERR_PERMISSION` | An origin that is not in the list (`null` among them), or two `Origin` fields. | Answers `403`. |

It works on the parsed request, so it is the same call in a handler of the blocking server, in
`on_request` of the event-driven one, and with the codec alone.

## 3. Sending and receiving

| API | Intent | Return |
|---|---|---|
| `proven_ws_conn_send_text(ws, text)` | Send a text message. | `proven_err_t`: `INVALID_ENCODING` when it is not UTF-8 - nothing is sent; `INVALID_STATE` after a close or inside a message begun with `_send_part`; `TIMEOUT`, `RESET`. |
| `proven_ws_conn_send_binary(ws, data)` | Send a binary message. | the same, without the encoding check. |
| `proven_ws_conn_send_part(ws, text, data, last)` | Send one fragment of a message; `last` ends it. `text` is read on the first call. | the same. Text sent this way is **not** checked. |
| `proven_ws_conn_ping(ws, data)` | Send a ping, with at most 125 bytes. | `proven_err_t`: `INVALID_ARG` for more. |
| `proven_ws_conn_receive(ws, until, &message)` | Wait for the next whole message, or until `until`. | `proven_err_t`: `TIMEOUT` - nothing lost, call again; `EOF` - the peer closed; `RESET` - the connection dropped; `INVALID_FORMAT`, `INVALID_ENCODING`, `OUT_OF_BOUNDS` - the peer broke a rule and has been told. |
| `proven_ws_conn_pong_count(ws)` | How many pongs have arrived. | `proven_u64`. |

```text
typedef struct {
    bool text;                  /* text (UTF-8, checked) rather than binary */
    proven_mem_view_t data;     /* owned by the connection; good until the next receive */
} proven_ws_message_t;
```

`proven_ws_conn_receive` does the protocol's housekeeping while it waits: a **ping is answered**
with its pong and you never see it; a **pong is counted**; **fragments are joined**, with
control frames between them handled where they arrive. What it returns is always a whole
message.

### Cautions, and what goes wrong

**A refusal takes up to a second.** When the peer breaks a rule the connection sends its close and then reads away what the peer is still sending, for at most a second, before `receive` returns - otherwise closing the socket would reset the connection and destroy the close before the peer read it.

**`PROVEN_ERR_TIMEOUT` is the only error you can receive after.** It means no complete message
arrived yet; whatever part of one did arrive is kept, and the next call continues. Every other
result ends the connection: the same error comes back from every later call, and what is left
to do is `proven_ws_conn_destroy`.

**A message's bytes are good until the next receive.** `message.data` points into memory the
connection reuses. Copy what you keep.

Wrong:

```text
proven_ws_conn_receive(ws, until, &first);
proven_ws_conn_receive(ws, until, &second);
use(first.data);                     /* wrong: first.data now holds the second message, or freed memory */
```

Correct - use or copy `first.data` before receiving again.

**Pings are only answered while you are receiving.** The connection has no thread of its own.
A program that sends for a long time without calling `proven_ws_conn_receive` answers no pings,
and a peer that uses pings to detect dead connections will conclude this one is dead. If you
send a great deal, receive now and then - with `PROVEN_NET_DONT_WAIT` if there may be nothing.

**Nothing sends pings for you either.** To find a silent connection that has died - a laptop
that closed, a NAT that forgot - send `proven_ws_conn_ping`, receive for as long as you are
prepared to wait, and see whether `proven_ws_conn_pong_count` moved. A TCP connection whose
other end vanished without a word can otherwise look open for hours.

**`_send_part` does not check text.** `proven_ws_conn_send_text` refuses text that is not
UTF-8, because the peer must drop the connection when it receives such text. Fragments cannot
be checked one by one - a character may be cut between two of them - so there the pieces
*together* must be UTF-8, and making sure is yours.

**A write that times out ends the connection.** `io_timeout_ms` bounds each write. When it
expires part of a frame may have gone, and there is no way to continue from the middle of a
frame: the connection is finished.

## 4. Closing, and being closed on

| API | Intent | Return |
|---|---|---|
| `proven_ws_conn_close(ws, code, reason)` | Send a close frame and wait for the peer's. Messages that arrive meanwhile are discarded. | `proven_err_t`: `PROVEN_OK` when both sides have closed; `TIMEOUT` when the peer did not answer in `close_timeout_ms`; `INVALID_ARG` for a code or reason that may not be sent. |
| `proven_ws_conn_close_code(ws)` | The peer's close code; 1006 when the connection ended without one; 0 while open. | `proven_u16`. |
| `proven_ws_conn_close_reason(ws)` | The reason that came with it. | `proven_u8str_view_t`. |

Closing is a small handshake: one side sends a close frame, the other answers with one, and
then the TCP connection ends. Skipping it - destroying a connection that was never closed -
works, and the peer sees code 1006, "abnormal closure", which is also what it sees when the
network fails. A peer that logs or retries on 1006 will do so. Close first.

| Code | Meaning | Sent by |
|---|---|---|
| 1000 | Normal: the purpose is fulfilled. | you |
| 1001 | Going away: the server is shutting down, the page is being left. | you |
| 1002 | Protocol error. | the connection, for a frame the protocol forbids |
| 1003 | Unsupported data: text where binary was wanted, or the reverse. | you |
| 1007 | Invalid data: text that is not UTF-8. | the connection |
| 1008 | Policy violation. | you |
| 1009 | Message too big. | the connection, past `max_message_bytes` |
| 1011 | Internal error. | you |
| 3000-4999 | Yours and your application's to define. | you |
| 1005, 1006 | Never sent: "no code was given" and "no close frame came". | nobody; you only read them |

**When the peer closes, `receive` returns `PROVEN_ERR_EOF` and the close has been answered.**
Read the code and the reason if you want them, then destroy. Nothing more can be sent.

**The reason is for a person reading a log.** At most 123 bytes of UTF-8. Do not put anything
in it that a program must parse, and remember it crosses the network readable.

**A close does not flush the other direction.** Messages the peer sent before it saw your close
are discarded by `proven_ws_conn_close`. If you need them, stop sending and keep receiving
until the peer closes, rather than closing first.

## 5. Where a server's connections live

`proven_ws_conn_accept` takes the connection out of the HTTP server (Chapter 11, section 11).
From that moment the server's loop, its connection limit and its timeouts have nothing to do
with it, and the question is which thread will call `proven_ws_conn_receive`.

**The handler hands it off and returns.** This is the shape that works with either way of
running handlers, and the one the example uses: give the connection to a thread or a job of
your own, and return. The server goes on serving.

**The handler stays and talks.** Simple to write. With handlers on a job system it occupies one
worker for as long as the connection lasts - so the number of workers is the number of
WebSocket connections you can have, and the next client's handshake waits in the queue until
one closes. With handlers on the loop's thread it stops the whole server for that time, which
is almost never what was meant.

Either way **a WebSocket connection costs a thread here.** `proven_ws_conn_receive` waits on one
connection; there is no call that waits on many. A hundred connections are a hundred threads
blocked in receive. That is a workable design for tens or hundreds of connections and the wrong
one for tens of thousands - for which you would drive the codec of sections 6-8 from a
readiness loop of your own (`proven_net_poll`, Chapter 9), one decoder per connection.

**No limit is applied for you.** The HTTP server's `max_connections` stops counting a
connection when it is upgraded. If clients can open WebSockets without bound, count them
yourself and answer the handshake with a `503` when you have enough.

Compiled and run by the test suite:

<!-- example: manual/examples/en/ex_12_ws_echo.c -->
```c
/*
 * A WebSocket server and a client, in one program.
 *
 * The server is an ordinary HTTP server whose handler accepts the upgrade. The handler does not
 * stay to talk: it hands the connection to a worker and returns, so the server's loop is free
 * for the next request. The client connects through an HTTP client and from then on the two
 * ends are the same kind of thing.
 */

static proven_http_server_t *g_server;
static proven_job_sys_t *g_workers;

/* One connection's life, on a worker: echo each message until the peer closes. */
static void talk(void *arg) {
    proven_ws_conn_t *ws = arg;
    for (;;) {
        proven_ws_message_t msg;
        /* Pings are answered inside receive; a peer silent for 30 s is given up on. */
        proven_err_t err = proven_ws_conn_receive(ws, proven_net_deadline_in(30000), &msg);
        if (err != PROVEN_OK) break;        /* PROVEN_ERR_EOF: the peer closed, and was answered */
        err = msg.text ? proven_ws_conn_send_text(ws, (proven_u8str_view_t){ msg.data.ptr, msg.data.size })
                       : proven_ws_conn_send_binary(ws, msg.data);
        if (err != PROVEN_OK) break;
    }
    proven_ws_conn_destroy(ws);             /* always: it owns the socket now */
}

static void handle(void *ctx, proven_http_exchange_t *x) {
    (void)ctx;
    proven_ws_conn_config_t config = { .alloc = proven_heap_allocator(), .max_message_bytes = 64 * 1024 };
    proven_ws_conn_t *ws = NULL;
    /* Select a subprotocol only if the client offered it. */
    const proven_http_request_t *request = proven_http_exchange_request(x);
    proven_u8str_view_t protocol = proven_ws_request_offers(request, PROVEN_LIT("echo.v1")) ? PROVEN_LIT("echo.v1") : PROVEN_LIT("");

    proven_err_t err = proven_ws_conn_accept(x, protocol, &config, &ws);
    if (err == PROVEN_ERR_NOT_FOUND) {
        /* Not an upgrade at all - an ordinary request to this address. Nothing was sent. */
        (void)proven_http_exchange_respond(x, 200, NULL, 0, proven_mem_view_from_u8(PROVEN_LIT("this is a WebSocket endpoint\n")));
        return;
    }
    if (err != PROVEN_OK) return;           /* a bad handshake was already answered 400 or 426 */

    /* The connection no longer belongs to the server. Give it a thread and return. */
    if (proven_job_submit_ex(g_workers, talk, ws) != PROVEN_OK) proven_ws_conn_destroy(ws);
}

static void serve(void *arg) {
    (void)arg;
    (void)proven_http_server_run(g_server);
}

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();

    // ---- the server ----------------------------------------------------------
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
    /* One thread for the server's loop, and workers for connections: each open WebSocket
     * occupies one for as long as it lasts. */
    proven_job_sys_t *loop_thread = NULL;
    proven_job_group_t running;
    proven_job_group_init(&running);
    EXAMPLE_REQUIRE(proven_job_system_init(heap, 1, 4, &loop_thread) == PROVEN_OK &&
                    proven_job_system_init(heap, 4, 16, &g_workers) == PROVEN_OK &&
                    proven_job_group_submit(loop_thread, &running, serve, NULL) == PROVEN_OK, "the loop and the workers are started");

    // ---- the client ----------------------------------------------------------
    proven_http_client_config_t http_config = { .alloc = heap };
    proven_http_client_t *http = NULL;
    EXAMPLE_REQUIRE(proven_http_client_create(&http_config, &http) == PROVEN_OK, "an HTTP client to connect through");

    char url[64];
    int n = snprintf(url, sizeof url, "ws://127.0.0.1:%u/talk", (unsigned)at.port);
    proven_ws_conn_config_t config = { .alloc = heap };
    proven_ws_conn_t *ws = NULL;
    proven_u16 status = 0;
    err = proven_ws_conn_connect(http, (proven_u8str_view_t){ (const proven_byte_t *)url, (proven_size_t)n }, NULL, 0,
                                 PROVEN_LIT("echo.v1, echo.v0"), &config, &ws, &status);
    EXAMPLE_REQUIRE(err == PROVEN_OK && status == 101, "connected: the server answered 101 and the answer checked out");
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_ws_conn_protocol(ws), PROVEN_LIT("echo.v1")), "the subprotocol the server chose");
    /* The connection is its own thing now; the HTTP client is not needed to use it. */
    proven_http_client_destroy(http);

    proven_ws_message_t msg;
    EXAMPLE_REQUIRE(proven_ws_conn_send_text(ws, PROVEN_LIT("hello")) == PROVEN_OK, "a text message");
    EXAMPLE_REQUIRE(proven_ws_conn_receive(ws, proven_net_deadline_in(5000), &msg) == PROVEN_OK && msg.text &&
                    proven_u8str_view_eq((proven_u8str_view_t){ msg.data.ptr, msg.data.size }, PROVEN_LIT("hello")), "comes back, whole");

    proven_byte_t bytes[4] = { 0x00, 0xff, 0x10, 0x80 };
    EXAMPLE_REQUIRE(proven_ws_conn_send_binary(ws, (proven_mem_view_t){ bytes, sizeof bytes }) == PROVEN_OK &&
                    proven_ws_conn_receive(ws, proven_net_deadline_in(5000), &msg) == PROVEN_OK && !msg.text && msg.data.size == 4, "binary is any bytes");

    /* Text is checked before it is sent: the peer would have to drop the connection over this. */
    EXAMPLE_REQUIRE(proven_ws_conn_send_text(ws, PROVEN_LIT("not \xff text")) == PROVEN_ERR_INVALID_ENCODING, "not UTF-8: refused here, nothing sent");

    /* A message produced in pieces goes out in fragments; the other end still sees one message. */
    EXAMPLE_REQUIRE(proven_ws_conn_send_part(ws, true, proven_mem_view_from_u8(PROVEN_LIT("one, ")), false) == PROVEN_OK &&
                    proven_ws_conn_send_part(ws, true, proven_mem_view_from_u8(PROVEN_LIT("two, ")), false) == PROVEN_OK &&
                    proven_ws_conn_send_part(ws, true, proven_mem_view_from_u8(PROVEN_LIT("three")), true) == PROVEN_OK, "three fragments");
    EXAMPLE_REQUIRE(proven_ws_conn_receive(ws, proven_net_deadline_in(5000), &msg) == PROVEN_OK && msg.data.size == 15, "one message of fifteen bytes");

    /* Is the other end alive? Ping, then receive for a while: the pong is counted on the way. */
    EXAMPLE_REQUIRE(proven_ws_conn_ping(ws, proven_mem_view_from_u8(PROVEN_LIT("still there?"))) == PROVEN_OK, "a ping");
    err = proven_ws_conn_receive(ws, proven_net_deadline_in(200), &msg);
    EXAMPLE_REQUIRE(err == PROVEN_ERR_TIMEOUT && proven_ws_conn_pong_count(ws) == 1, "no message came, but the pong did: the peer is alive");

    /* Closing is a handshake: our close, then the peer's. */
    EXAMPLE_REQUIRE(proven_ws_conn_close(ws, PROVEN_WS_CLOSE_NORMAL, PROVEN_LIT("done")) == PROVEN_OK, "closed, both ways");
    EXAMPLE_REQUIRE(proven_ws_conn_close_code(ws) == PROVEN_WS_CLOSE_NORMAL && proven_ws_conn_close_reason(ws).size == 0, "the peer echoed the code, with no reason of its own");
    proven_ws_conn_destroy(ws);

    // ---- a connection over any transport ------------------------------------
    /* The handshake is HTTP's; what follows needs only a transport. Here: a socket pair. */
    proven_net_conn_t a, b;
    proven_ws_conn_t *left = NULL, *right = NULL;
    EXAMPLE_REQUIRE(proven_net_pair(&a, &b) == PROVEN_OK, "two joined sockets");
    EXAMPLE_REQUIRE(proven_ws_conn_open(proven_net_conn_transport(&a), false, (proven_mem_view_t){0}, &config, &left) == PROVEN_OK &&
                    proven_ws_conn_open(proven_net_conn_transport(&b), true, (proven_mem_view_t){0}, &config, &right) == PROVEN_OK, "a client end and a server end");
    EXAMPLE_REQUIRE(proven_ws_conn_send_text(left, PROVEN_LIT("no handshake needed")) == PROVEN_OK &&
                    proven_ws_conn_receive(right, proven_net_deadline_in(1000), &msg) == PROVEN_OK && msg.data.size == 19, "a message across");
    proven_ws_conn_destroy(left);
    proven_ws_conn_destroy(right);

    proven_http_server_stop(g_server);
    proven_job_group_wait(loop_thread, &running);
    proven_http_server_destroy(g_server);
    proven_job_system_close(g_workers);         /* waits for the worker that is finishing the conversation */
    proven_job_system_destroy(g_workers);
    proven_job_system_close(loop_thread);
    proven_job_system_destroy(loop_thread);
    return EXAMPLE_OK();
}
```

## 6. The codec: the handshake

`ws.h` is the protocol without a connection: you give it bytes and it tells you what they mean.
Use it when the transport is not a socket this library opened, when you serve many connections
from one loop, or on a target with no sockets at all.

| API | Intent | Return |
|---|---|---|
| `proven_ws_make_key(random, out)` | A client's `Sec-WebSocket-Key` (24 characters) from 16 random bytes you supply. | none. |
| `proven_ws_accept_key(key, out)` | The `Sec-WebSocket-Accept` value (28 characters) that answers a key. | `proven_err_t`: `INVALID_FORMAT` unless the key is 24 characters of Base64 for 16 bytes. |
| `proven_ws_check_request(&request, &key)` | A server's check of a parsed request - also an HTTP/2 extended CONNECT as the event-driven server presents it (Chapter 15, section 11), whose `key` is empty. | `proven_err_t`: `PROVEN_OK` - answer 101 (over HTTP/2, 200: `proven_ws_event_accept` does either); `NOT_FOUND` - not a WebSocket upgrade; `UNSUPPORTED` - another version, answer 426 with `Sec-WebSocket-Version: 13`; `INVALID_FORMAT` - answer 400. |
| `proven_ws_request_offers(&request, name)` | Whether the request offers a subprotocol, compared exactly. | `bool`. |
| `proven_ws_check_origin(&request, allowed, count)` | Whether the request's `Origin` is one of the origins served (section 2). | `proven_err_t`: `PROVEN_OK`; `NOT_FOUND` - no `Origin`; `PERMISSION` - another origin, or two fields; `INVALID_ARG`. |
| `proven_ws_check_response(&response, key, offered, &protocol)` | A client's check of the answer: 101, the right accept value, a subprotocol that was offered, no extension. | `proven_err_t`: `INVALID_FORMAT` otherwise. |

The accept value is Base64 of the SHA-1 of the key followed by a fixed string from the RFC.
SHA-1 is broken as a signature and this use does not care: nothing here is a secret, and the
value only has to be something an HTTP server that is *not* a WebSocket server would not
produce by reflex. That is also all it proves. It does not authenticate the server and it does
not authenticate the client; anyone can compute it.

**A response that names an extension is refused** by `proven_ws_check_response`: a client
that offered none is being told of frames it did not ask for. A client that offered
compression checks with `proven_ws_check_response_deflate` instead (section 9), which
accepts that one extension in the form it was offered and nothing else.

## 7. The codec: frames

```text
typedef struct {
    bool fin;                   /* the last frame of its message */
    proven_u8 opcode;           /* PROVEN_WS_TEXT, _BINARY, _CONTINUATION, _CLOSE, _PING, _PONG */
    bool masked;
    proven_byte_t mask[4];
    proven_u64 length;          /* payload bytes that follow the header */
} proven_ws_frame_t;
```

| API | Intent | Return |
|---|---|---|
| `proven_ws_frame_parse(data, &frame, &header_size)` | Parse a frame header - 2 to 14 bytes - from the front of `data`. | `proven_err_t`: `NEED_MORE`; `INVALID_FORMAT` for a reserved bit (the first is reported in `rsv1` where a message begins: section 9), an undefined opcode, a length not in its shortest form, or a control frame that is fragmented or over 125 bytes. |
| `proven_ws_frame_write(out, &len, &frame)` | Append a frame header. The payload is yours to write after it. | `proven_err_t`: `INVALID_ARG` for a frame the parser would refuse; `OUT_OF_BOUNDS`. |
| `proven_ws_mask(data, mask, offset)` | Mask or unmask payload bytes in place. `offset` is where `data` starts within the payload. | none. |
| `proven_ws_close_write(out, &len, code, reason)` | Append a close frame's payload. | `proven_err_t`: `INVALID_ARG` for a code that may not be sent or a reason over 123 bytes; `INVALID_ENCODING`; `OUT_OF_BOUNDS`. |
| `proven_ws_close_parse(payload, &code, &reason)` | Read one. An empty payload is code 1005. | `proven_err_t`: `INVALID_FORMAT`; `INVALID_ENCODING`. |
| `proven_ws_close_code_is_valid(code)` | Whether a code may appear in a close frame. | `bool`. |
| `proven_ws_close_code_for(err)` | The close code that reports a decoder error: 1002, 1007, 1009, or 1011. | `proven_u16`. |

**A length has one spelling.** Up to 125 it is in the second byte; up to 65535 in two more;
beyond that in eight. `proven_ws_frame_write` always uses the shortest, and
`proven_ws_frame_parse` refuses any other - two encodings of one frame are how a filter and the
program behind it come to disagree about where a frame ends.

**A client must mask with a key the application cannot predict.** Take the four bytes from
`proven_random_bytes` or a generator seeded from it - a counter or a constant defeats the
purpose. A server sets `masked` to false and writes the payload as it is.

**The codec has no random source.** Keys and masks are arguments, as the multipart boundary is
in Chapter 10, so the same code runs where there is no operating system to ask.

## 8. The codec: the decoder

Parsing headers is the small part. What a receiver has to get right is the stream: frames
arriving in arbitrary pieces, fragments to join, control frames between fragments, text to
check across fragment boundaries, and a list of things to refuse. `proven_ws_decoder_t` is that.

| API | Intent | Return |
|---|---|---|
| `proven_ws_decoder_init(&decoder, from_client, max_message_bytes)` | Begin. `from_client` true: every frame must be masked (a server's decoder). False: none may be. | none. |
| `proven_ws_decoder_feed(&decoder, in, &consumed, &event)` | Consume bytes of `in` until there is something to report or `in` is used up. | `proven_err_t`: `INVALID_FORMAT` (close 1002); `INVALID_ENCODING` (1007); `OUT_OF_BOUNDS` (1009); `EOF` for bytes after a close frame. |

```text
typedef struct {
    proven_ws_event_kind_t kind;   /* PROVEN_WS_EVENT_NONE, _DATA, _PING, _PONG, _CLOSE */
    bool text, first, last;        /* DATA: text or binary; the ends of the message */
    proven_mem_view_t data;        /* DATA: a piece, unmasked, inside `in`. PING, PONG: the payload. CLOSE: the reason. */
    proven_u16 close_code;         /* CLOSE */
} proven_ws_event_t;
```

The loop is the one from Chapter 10's body decoder: feed what you read; act on the event; feed
the rest; when the event is `NONE`, read more.

**A message passes through without being held.** A `DATA` event is a *piece* of a message, in
the buffer you fed, unmasked where it lay. `first` and `last` mark the message's ends. A
gigabyte message is so many events and no allocation - and joining the pieces, if you want
them joined, is yours, with a limit of your choosing. (`ws_conn.h` joins them for you, up to
`max_message_bytes`.)

**`in` is written to.** Unmasking happens in place, which is why `in` is not `const`. Do not
feed memory you need unchanged.

**What the decoder refuses**, each with the error that names its close code:

| The stream | Error |
|---|---|
| A frame from a client that is not masked, or one from a server that is | `INVALID_FORMAT` |
| A reserved bit (the first only where it may not be, or when compression was not agreed: section 9), an undefined opcode, a length not in its shortest form | `INVALID_FORMAT` |
| A control frame over 125 bytes, or fragmented | `INVALID_FORMAT` |
| A continuation with no message open; a new message inside an open one | `INVALID_FORMAT` |
| A close frame with a one-byte payload, or a code that may not be sent | `INVALID_FORMAT` |
| Text that is not UTF-8, checked across fragments; a close reason that is not | `INVALID_ENCODING` |
| A message, fragments together, past the limit | `OUT_OF_BOUNDS` |

After any of these the decoder repeats the error. That is deliberate: once a frame is wrong,
where the next one starts is not known, and guessing is how one bad frame becomes a different
conversation.

**Answering is yours.** The decoder reports a ping; it does not send the pong. It reports a
close; it does not send the answering close. It has no way to send anything - which is what
lets it run anywhere.

Compiled and run by the test suite:

<!-- example: manual/examples/en/ex_12_ws_frames.c -->
```c
#include <string.h>

/*
 * The WebSocket codec by itself: the handshake's two values, a frame written and read, and a
 * decoder fed a stream in awkward pieces. No socket anywhere - the "network" is an array.
 */

int main(void) {
    // ---- The opening handshake ----------------------------------------------
    /* A client makes a key from sixteen random bytes; the server answers with a value only
     * something that speaks WebSocket would compute. These are the values of RFC 6455. */
    proven_byte_t accept[PROVEN_WS_ACCEPT_SIZE];
    EXAMPLE_REQUIRE(proven_ws_accept_key(PROVEN_LIT("dGhlIHNhbXBsZSBub25jZQ=="), accept) == PROVEN_OK &&
                    memcmp(accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=", sizeof accept) == 0, "the RFC's key and its answer");

    proven_byte_t random[16], key[PROVEN_WS_KEY_SIZE];
    EXAMPLE_REQUIRE(proven_random_bytes(random, sizeof random), "sixteen random bytes");
    proven_ws_make_key(random, key);
    EXAMPLE_REQUIRE(proven_ws_accept_key((proven_u8str_view_t){ key, sizeof key }, accept) == PROVEN_OK, "a key of our own, and its answer");

    /* A server looks at a parsed request and learns which of four things it is. */
    static const char upgrade[] = "GET /chat HTTP/1.1\r\nHost: example.com\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                                  "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Protocol: chat, superchat\r\n"
                                  "Origin: http://example.com\r\nSec-WebSocket-Version: 13\r\n\r\n";
    proven_http_header_t fields[16];
    proven_http_request_t request;
    proven_size_t head = 0;
    proven_u8str_view_t client_key;
    EXAMPLE_REQUIRE(proven_http_parse_request((proven_mem_view_t){ (const proven_byte_t *)upgrade, sizeof upgrade - 1 }, fields, 16, 0, &request, &head) == PROVEN_OK, "the request parses");
    EXAMPLE_REQUIRE(proven_ws_check_request(&request, &client_key) == PROVEN_OK, "it is a WebSocket upgrade: answer 101");
    EXAMPLE_REQUIRE(proven_ws_request_offers(&request, PROVEN_LIT("superchat")) && !proven_ws_request_offers(&request, PROVEN_LIT("mqtt")), "and it offers these subprotocols");
    /* A server that knows its users by a cookie checks where the page came from. */
    const proven_u8str_view_t served[] = { PROVEN_LIT("https://app.example"), PROVEN_LIT("http://example.com") };
    EXAMPLE_REQUIRE(proven_ws_check_origin(&request, served, 2) == PROVEN_OK && proven_ws_check_origin(&request, served, 1) == PROVEN_ERR_PERMISSION,
                    "its Origin is one the server serves; were it not, the answer would be 403");

    /* The client checks the answer: the right accept value, a subprotocol it offered, nothing else. */
    static const char answer[] = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                                 "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\nSec-WebSocket-Protocol: chat\r\n\r\n";
    proven_http_response_t response;
    proven_u8str_view_t chosen;
    EXAMPLE_REQUIRE(proven_http_parse_response((proven_mem_view_t){ (const proven_byte_t *)answer, sizeof answer - 1 }, fields, 16, 0, &response, &head) == PROVEN_OK, "the response parses");
    EXAMPLE_REQUIRE(proven_ws_check_response(&response, client_key, PROVEN_LIT("chat, superchat"), &chosen) == PROVEN_OK &&
                    proven_u8str_view_eq(chosen, PROVEN_LIT("chat")), "it answers this handshake, and chose chat");

    // ---- One frame -----------------------------------------------------------
    /* A client's text frame: a header, then the payload XORed with a four-byte key. */
    proven_byte_t wire[256];
    proven_mem_mut_t out = { wire, sizeof wire };
    proven_size_t len = 0;
    proven_ws_frame_t frame = { .fin = true, .opcode = PROVEN_WS_TEXT, .masked = true, .mask = { 0x37, 0xfa, 0x21, 0x3d }, .length = 5 };
    EXAMPLE_REQUIRE(proven_ws_frame_write(out, &len, &frame) == PROVEN_OK && len == 6, "a six-byte header");
    memcpy(wire + len, "Hello", 5);
    proven_ws_mask((proven_mem_mut_t){ wire + len, 5 }, frame.mask, 0);
    len += 5;
    EXAMPLE_REQUIRE(memcmp(wire, "\x81\x85\x37\xfa\x21\x3d\x7f\x9f\x4d\x51\x58", 11) == 0, "byte for byte the masked Hello of RFC 6455");

    proven_ws_frame_t parsed;
    proven_size_t header_size = 0;
    EXAMPLE_REQUIRE(proven_ws_frame_parse((proven_mem_view_t){ wire, 3 }, &parsed, &header_size) == PROVEN_ERR_NEED_MORE, "half a header: read more");
    EXAMPLE_REQUIRE(proven_ws_frame_parse((proven_mem_view_t){ wire, len }, &parsed, &header_size) == PROVEN_OK &&
                    parsed.opcode == PROVEN_WS_TEXT && parsed.masked && parsed.length == 5 && header_size == 6, "the header, read back");

    // ---- A stream, decoded ---------------------------------------------------
    /* A text message in two fragments with a ping between them, then a close. */
    len = 0;
    frame = (proven_ws_frame_t){ .fin = false, .opcode = PROVEN_WS_TEXT, .length = 3 };
    EXAMPLE_REQUIRE(proven_ws_frame_write(out, &len, &frame) == PROVEN_OK, "first fragment");
    memcpy(wire + len, "Hel", 3); len += 3;
    frame = (proven_ws_frame_t){ .fin = true, .opcode = PROVEN_WS_PING, .length = 0 };
    EXAMPLE_REQUIRE(proven_ws_frame_write(out, &len, &frame) == PROVEN_OK, "a ping, in the middle of the message");
    frame = (proven_ws_frame_t){ .fin = true, .opcode = PROVEN_WS_CONTINUATION, .length = 2 };
    EXAMPLE_REQUIRE(proven_ws_frame_write(out, &len, &frame) == PROVEN_OK, "last fragment");
    memcpy(wire + len, "lo", 2); len += 2;
    proven_byte_t close_payload[PROVEN_WS_MAX_CONTROL];
    proven_size_t close_len = 0;
    EXAMPLE_REQUIRE(proven_ws_close_write((proven_mem_mut_t){ close_payload, sizeof close_payload }, &close_len, PROVEN_WS_CLOSE_NORMAL, PROVEN_LIT("bye")) == PROVEN_OK, "a close payload");
    frame = (proven_ws_frame_t){ .fin = true, .opcode = PROVEN_WS_CLOSE, .length = close_len };
    EXAMPLE_REQUIRE(proven_ws_frame_write(out, &len, &frame) == PROVEN_OK, "the close frame");
    memcpy(wire + len, close_payload, close_len); len += close_len;

    /* A client decodes what a server sent: `false` - these frames must NOT be masked. The
     * stream arrives four bytes at a time; the decoder does not care. */
    proven_ws_decoder_t decoder;
    proven_ws_decoder_init(&decoder, false, 1024);
    char message[32];
    proven_size_t message_len = 0;
    int pings = 0;
    proven_u16 close_code = 0;
    for (proven_size_t pos = 0; pos < len;) {
        proven_size_t n = len - pos < 4 ? len - pos : 4;
        proven_mem_mut_t piece = { wire + pos, n };
        while (piece.size > 0) {
            proven_size_t used = 0;
            proven_ws_event_t ev;
            EXAMPLE_REQUIRE(proven_ws_decoder_feed(&decoder, piece, &used, &ev) == PROVEN_OK, "the stream decodes");
            piece.ptr += used;
            piece.size -= used;
            if (ev.kind == PROVEN_WS_EVENT_DATA) {
                memcpy(message + message_len, ev.data.ptr, ev.data.size);      /* a piece of the message */
                message_len += ev.data.size;
            } else if (ev.kind == PROVEN_WS_EVENT_PING) {
                pings++;                                                       /* a real program answers with a pong */
            } else if (ev.kind == PROVEN_WS_EVENT_CLOSE) {
                close_code = ev.close_code;
            }
        }
        pos += n;
    }
    EXAMPLE_REQUIRE(message_len == 5 && memcmp(message, "Hello", 5) == 0, "two fragments, one message");
    EXAMPLE_REQUIRE(pings == 1 && close_code == PROVEN_WS_CLOSE_NORMAL, "the ping between them, and the close");

    // ---- What a receiver refuses --------------------------------------------
    /* A masked frame from a server breaks the protocol; the error names the close code to send. */
    proven_byte_t bad[] = { 0x81, 0x81, 1, 2, 3, 4, 'x' ^ 1 };
    proven_ws_event_t ev;
    proven_size_t used = 0;
    proven_ws_decoder_init(&decoder, false, 1024);
    proven_err_t err = proven_ws_decoder_feed(&decoder, (proven_mem_mut_t){ bad, sizeof bad }, &used, &ev);
    EXAMPLE_REQUIRE(err == PROVEN_ERR_INVALID_FORMAT && proven_ws_close_code_for(err) == PROVEN_WS_CLOSE_PROTOCOL_ERROR, "refused: close with 1002");

    /* A close frame's payload, read by itself. Code 1005 means "no code was given" and may
     * not itself be sent. */
    proven_u16 code = 0;
    proven_u8str_view_t reason;
    EXAMPLE_REQUIRE(proven_ws_close_parse((proven_mem_view_t){ close_payload, close_len }, &code, &reason) == PROVEN_OK &&
                    code == 1000 && proven_u8str_view_eq(reason, PROVEN_LIT("bye")), "code and reason");
    EXAMPLE_REQUIRE(proven_ws_close_code_is_valid(4000) && !proven_ws_close_code_is_valid(PROVEN_WS_CLOSE_NO_STATUS), "which codes may travel");

    return EXAMPLE_OK();
}
```

## 9. Compressed messages

```c
bool proven_ws_conn_compressed(const proven_ws_conn_t *conn);            /* ws_conn.h */
bool proven_ws_stream_compressed(const proven_ws_stream_t *ws);          /* ws_event.h, Chapter 15 */

#define PROVEN_WS_DEFLATE_OFFER "permessage-deflate; client_no_context_takeover; server_no_context_takeover"
proven_err_t proven_ws_deflate_accept(const proven_http_request_t *request, proven_mem_mut_t out, proven_size_t *len, int *window_bits);
proven_err_t proven_ws_check_response_deflate(const proven_http_response_t *response, proven_u8str_view_t key,
                                              proven_u8str_view_t offered, proven_u8str_view_t *protocol, bool *deflate);
void proven_ws_decoder_allow_compressed(proven_ws_decoder_t *decoder);
```

A WebSocket connection can agree, in its handshake, to compress its messages: the extension
is called `permessage-deflate` (RFC 7692), every browser offers it, and it is the DEFLATE of
[Chapter 16](manual-16-compression.md). A page of JSON or a batch of log lines shrinks to a
third or less; a message of a few dozen bytes gains little, for the reason given below.

**It is off until you ask.** Set `compress` in the connection's configuration - in a
`proven_ws_conn_config_t`, or in the `proven_ws_event_config_t` of Chapter 15. A client then
offers the extension and a server accepts it when offered; `proven_ws_conn_compressed` and
`proven_ws_stream_compressed` say whether the other side agreed. **Nothing else changes**:
the same calls send and receive, and a connection whose peer did not agree simply goes on
uncompressed.

| | With compression agreed |
|---|---|
| A message you send | Compressed when it is 64 bytes or more; shorter ones go as they are |
| A message you receive | Inflated before you see it: whole from `proven_ws_conn_receive`, in pieces of at most 16 KiB from the event-driven server |
| `max_message_bytes` | Counts the **inflated** size. A few hundred bytes that unfold past it end the connection with 1009, as an oversized plain message does |
| Text | Checked to be UTF-8 after inflating; 1007 if it is not |
| Bytes that do not inflate | 1007 |
| A frame marked compressed on a connection that did not agree | 1002 |

**Every message is compressed on its own.** The extension allows each side to keep its
32 KiB window from one message to the next, which helps small, similar messages - and costs
a compressor and a decompressor per connection for as long as it is open. This library
always negotiates the other form ("no context takeover", in both directions): a compressor
exists for the length of one send, a decompressor from the first frame of a compressed
message to its last, and **a connection that is idle holds no compression state at all**.
Ten thousand quiet connections cost what they cost without compression. The price is that a
short message has only itself to refer back to.

### Example: one field, and what it changes

<!-- example: manual/examples/en/ex_12_ws_compressed.c -->
```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Compressed WebSocket messages, the way a program uses them: one field in a configuration.
 *
 * The server here is the event-driven one, the client the blocking one. Each sets `compress`;
 * the handshake agrees; after that every call is used exactly as before, and what crosses
 * the network is a fraction of what was said.
 */

typedef struct {
    proven_loop_t *loop;
    bool compressed;                   /* what the server found agreed */
    proven_size_t received, largest, pieces;
} ex_app_t;

/* A compressed message arrives decoded, in pieces of at most 16 KiB. Nothing holds the whole
 * of it unless you do. This handler counts, and answers when the message is complete. */
static void on_message(void *ctx, proven_ws_stream_t *ws, bool text, proven_mem_view_t piece, bool first, bool last) {
    ex_app_t *app = ctx;
    (void)text;
    if (first) { app->received = 0; app->pieces = 0; app->largest = 0; }
    app->received += piece.size;
    app->pieces++;
    if (piece.size > app->largest) app->largest = piece.size;
    if (!last) return;
    char answer[64];
    int n = snprintf(answer, sizeof answer, "%u bytes", (unsigned)app->received);
    /* Sent like any message. Short ones go as they are: under 64 bytes there is nothing to gain. */
    (void)proven_ws_stream_send(ws, true, (proven_mem_view_t){ (const proven_byte_t *)answer, (proven_size_t)n });
}

static void on_request(void *ctx, proven_http_stream_t *stream, const proven_http_request_t *head) {
    ex_app_t *app = ctx;
    proven_ws_event_config_t config = {
        .on = { .on_message = on_message },
        .ctx = app,
        .compress = true,              /* accept the extension when the client offers it */
        .max_message_bytes = 256 * 1024,   /* the limit on a message AS DECODED: a few bytes cannot become a gigabyte */
    };
    proven_ws_stream_t *ws = NULL;
    if (proven_ws_event_accept(stream, head, &config, &ws) != PROVEN_OK) {
        (void)proven_http_stream_respond(stream, 400, NULL, 0, (proven_mem_view_t){ 0 });
        return;
    }
    /* Whether it was agreed depends on the client too. Nothing else has to be done either way. */
    app->compressed = proven_ws_stream_compressed(ws);
    /* The same switch on this side, for a server whose answers must not be compressed:
     * proven_ws_stream_set_compress(ws, false). Here they may be. */
    proven_ws_stream_set_compress(ws, true);
}

static void run(void *arg) { (void)proven_loop_run(((ex_app_t *)arg)->loop); }

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();
    ex_app_t app = { 0 };
    EXAMPLE_REQUIRE(proven_loop_create(heap, &app.loop) == PROVEN_OK, "a loop");
    proven_http_event_server_config_t server_config = { .on = { .on_request = on_request }, .ctx = &app };
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
    proven_job_sys_t *threads = NULL;
    proven_job_group_t running;
    proven_job_group_init(&running);
    EXAMPLE_REQUIRE(proven_job_system_init(heap, 1, 4, &threads) == PROVEN_OK &&
                    proven_job_group_submit(threads, &running, run, &app) == PROVEN_OK, "the loop runs on another thread");

    // ---- a client that asks for compression ---------------------------------------
    proven_http_client_config_t client_config = { .alloc = heap };
    proven_http_client_t *client = NULL;
    EXAMPLE_REQUIRE(proven_http_client_create(&client_config, &client) == PROVEN_OK, "an HTTP client");
    char url[64];
    int n = snprintf(url, sizeof url, "ws://127.0.0.1:%u/", (unsigned)at.port);
    proven_ws_conn_config_t ws_config = { .alloc = heap, .compress = true };       /* offer the extension */
    proven_ws_conn_t *conn = NULL;
    EXAMPLE_REQUIRE(proven_ws_conn_connect(client, (proven_u8str_view_t){ (const proven_byte_t *)url, (proven_size_t)n }, NULL, 0, PROVEN_LIT(""),
                                           &ws_config, &conn, NULL) == PROVEN_OK, "connected");
    EXAMPLE_REQUIRE(proven_ws_conn_compressed(conn), "and the server agreed to compress");

    /* A hundred thousand bytes of a report. On the wire it is a few hundred. */
    static char report[100000];
    for (proven_size_t i = 0; i < sizeof report; ++i) report[i] = "temperature 21.5, humidity 40\n"[i % 30];
    proven_ws_message_t msg;
    EXAMPLE_REQUIRE(proven_ws_conn_send_text(conn, (proven_u8str_view_t){ (const proven_byte_t *)report, sizeof report }) == PROVEN_OK &&
                    proven_ws_conn_receive(conn, proven_net_deadline_in(5000), &msg) == PROVEN_OK &&
                    msg.data.size == 12 && memcmp(msg.data.ptr, "100000 bytes", 12) == 0, "the server received all of it");

    /* The limit is on the decoded size. Three hundred thousand of one letter are a few hundred
     * bytes compressed - and still too much: the server ends the connection with 1009. */
    /* One message kept out of the compressor - what a message that carries a secret beside
     * text the other side chose needs. The switch is read when a message begins. */
    proven_ws_conn_set_compress(conn, false);
    EXAMPLE_REQUIRE(proven_ws_conn_send_text(conn, (proven_u8str_view_t){ (const proven_byte_t *)report, 1000 }) == PROVEN_OK &&
                    proven_ws_conn_receive(conn, proven_net_deadline_in(5000), &msg) == PROVEN_OK &&
                    msg.data.size == 10 && memcmp(msg.data.ptr, "1000 bytes", 10) == 0, "a thousand bytes, sent as they are");
    proven_ws_conn_set_compress(conn, true);

    static char flood[300000];
    memset(flood, 'x', sizeof flood);
    EXAMPLE_REQUIRE(proven_ws_conn_send_text(conn, (proven_u8str_view_t){ (const proven_byte_t *)flood, sizeof flood }) == PROVEN_OK, "a message that inflates past the server's limit");
    EXAMPLE_REQUIRE(proven_ws_conn_receive(conn, proven_net_deadline_in(5000), &msg) == PROVEN_ERR_EOF && proven_ws_conn_close_code(conn) == 1009, "is answered with 1009, however small it was on the wire");
    proven_ws_conn_destroy(conn);
    proven_http_client_destroy(client);

    proven_loop_stop(app.loop);
    proven_job_group_wait(threads, &running);
    EXAMPLE_REQUIRE(app.compressed, "the server found the extension agreed as well");
    proven_http_event_server_destroy(server);
    proven_loop_destroy(app.loop);
    proven_job_system_close(threads);
    proven_job_system_destroy(threads);
    return EXAMPLE_OK();
}
```

### Example: the extension with the codec alone

For a program that drives the codec itself (sections 6 to 8): the handshake functions, the
bit in the frame, and the four bytes.

<!-- example: manual/examples/en/ex_12_ws_deflate.c -->
```c
#include <stdio.h>
#include <string.h>

/*
 * Compressed WebSocket messages with the codec alone: no socket, no driver.
 *
 * Three things make up the extension called permessage-deflate. The handshake agrees on it.
 * One bit in a frame's header says "this message is compressed". And the payload is DEFLATE
 * with its last four bytes left off. The codec does the first two; the third is deflate.h.
 *
 * A program that uses ws_conn.h or ws_event.h sets one field and sees none of this.
 */

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();

    // ---- the handshake ---------------------------------------------------------------
    /* A client asks with one header. PROVEN_WS_DEFLATE_OFFER is the form this library speaks:
     * each message compressed on its own, in both directions. */
    proven_http_header_t asked[] = {
        { PROVEN_LIT("Sec-WebSocket-Extensions"), PROVEN_LIT(PROVEN_WS_DEFLATE_OFFER) },
    };
    proven_http_request_t request = { .headers = asked, .header_count = 1 };

    /* A server looks at what was offered and writes its answer - or finds nothing it can
     * meet (PROVEN_ERR_NOT_FOUND), and the connection goes on uncompressed. */
    proven_byte_t answer[PROVEN_WS_DEFLATE_ANSWER_MAX];
    proven_size_t answer_len = 0;
    int window_bits = 0;
    EXAMPLE_REQUIRE(proven_ws_deflate_accept(&request, (proven_mem_mut_t){ answer, sizeof answer }, &answer_len, &window_bits) == PROVEN_OK, "the offer can be met");
    printf("the server answers: %.*s\n", (int)answer_len, (const char *)answer);
    EXAMPLE_REQUIRE(window_bits == 15, "and may compress with the full window");

    /* A browser's offer looks different, and is met as well. */
    proven_http_header_t browser[] = { { PROVEN_LIT("Sec-WebSocket-Extensions"), PROVEN_LIT("permessage-deflate; client_max_window_bits") } };
    proven_http_request_t from_browser = { .headers = browser, .header_count = 1 };
    proven_size_t len2 = 0;
    EXAMPLE_REQUIRE(proven_ws_deflate_accept(&from_browser, (proven_mem_mut_t){ answer, sizeof answer }, &len2, &window_bits) == PROVEN_OK, "a browser's offer too");

    /* The client checks the whole answer: the usual things, and that the extension came back
     * in the form it asked for. (The key and the accept value are the pair in RFC 6455.) */
    proven_http_header_t answered[] = {
        { PROVEN_LIT("Upgrade"), PROVEN_LIT("websocket") },
        { PROVEN_LIT("Connection"), PROVEN_LIT("Upgrade") },
        { PROVEN_LIT("Sec-WebSocket-Accept"), PROVEN_LIT("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") },
        { PROVEN_LIT("Sec-WebSocket-Extensions"), { answer, answer_len } },
    };
    proven_http_response_t response = { .status = 101, .headers = answered, .header_count = 4 };
    proven_u8str_view_t protocol;
    bool compressed = false;
    EXAMPLE_REQUIRE(proven_ws_check_response_deflate(&response, PROVEN_LIT("dGhlIHNhbXBsZSBub25jZQ=="), PROVEN_LIT(""), &protocol, &compressed) == PROVEN_OK && compressed,
                    "the client finds the extension agreed");

    // ---- a compressed message arrives -------------------------------------------------
    /* This frame is in RFC 7692: the text "Hello", compressed, from a server. Its first
     * byte is c1 where an uncompressed text frame has 81: the extra bit is RSV1. */
    proven_byte_t wire[] = { 0xc1, 0x07, 0xf2, 0x48, 0xcd, 0xc9, 0xc9, 0x07, 0x00 };

    proven_ws_decoder_t decoder;
    proven_ws_event_t event;
    proven_size_t used = 0;
    /* A decoder that was not told about the extension refuses the bit, as it must. */
    proven_byte_t copy[sizeof wire];
    memcpy(copy, wire, sizeof wire);
    proven_ws_decoder_init(&decoder, false, 0);
    EXAMPLE_REQUIRE(proven_ws_decoder_feed(&decoder, (proven_mem_mut_t){ copy, sizeof copy }, &used, &event) == PROVEN_ERR_INVALID_FORMAT, "without the extension, the bit is an error");

    proven_ws_decoder_init(&decoder, false, 0);
    proven_ws_decoder_allow_compressed(&decoder);
    EXAMPLE_REQUIRE(proven_ws_decoder_feed(&decoder, (proven_mem_mut_t){ wire, sizeof wire }, &used, &event) == PROVEN_OK &&
                    event.kind == PROVEN_WS_EVENT_DATA && event.compressed && event.last, "with it, the message is handed over, marked compressed");

    /* The decoder does not inflate: it allocates nothing. The bytes it hands over are raw
     * DEFLATE whose last four bytes - always 00 00 ff ff - the sender left off. Put them
     * back and inflate. The limit on what comes out is the size of your buffer. */
    proven_inflate_t *z = NULL;
    EXAMPLE_REQUIRE(proven_inflate_create(heap, PROVEN_DEFLATE_RAW, &z) == PROVEN_OK, "a decompressor for raw DEFLATE");
    static const proven_byte_t tail[4] = { 0x00, 0x00, 0xff, 0xff };
    proven_byte_t text[64];
    proven_size_t taken = 0, made = 0, total = 0;
    bool ended = false;
    EXAMPLE_REQUIRE(proven_inflate(z, event.data, &taken, (proven_mem_mut_t){ text, sizeof text }, &made, &ended) == PROVEN_OK && taken == event.data.size, "the payload");
    total = made;
    EXAMPLE_REQUIRE(proven_inflate(z, (proven_mem_view_t){ tail, 4 }, &taken, (proven_mem_mut_t){ text + total, sizeof text - total }, &made, &ended) == PROVEN_OK && taken == 4, "then the four bytes");
    total += made;
    EXAMPLE_REQUIRE(total == 5 && memcmp(text, "Hello", 5) == 0, "and the message is Hello");
    printf("the message: %.*s\n", (int)total, (const char *)text);
    /* Now - not before - it is text, and yours to check: the decoder could not. */
    proven_inflate_destroy(z);

    // ---- a compressed message is sent --------------------------------------------------
    /* The other way: compress with a sync flush, which ends in those same four bytes, and
     * leave them off. The frame that carries it has `rsv1` set. */
    static const char said[] = "to be, or not to be, that is the question: to be, or not to be";
    proven_deflate_options_t options = { .alloc = heap, .format = PROVEN_DEFLATE_RAW };
    proven_deflate_t *c = NULL;
    EXAMPLE_REQUIRE(proven_deflate_create(&options, &c) == PROVEN_OK, "a compressor");
    proven_byte_t packed[128];
    EXAMPLE_REQUIRE(proven_deflate(c, (proven_mem_view_t){ (const proven_byte_t *)said, sizeof said - 1 }, &taken, (proven_mem_mut_t){ packed, sizeof packed }, &made,
                                   PROVEN_DEFLATE_FLUSH_SYNC, &ended) == PROVEN_OK && taken == sizeof said - 1 && made > 4, "compressed, with a sync flush");
    EXAMPLE_REQUIRE(memcmp(packed + made - 4, tail, 4) == 0, "which ends in 00 00 ff ff");
    proven_size_t payload = made - 4;
    proven_deflate_destroy(c);

    proven_ws_frame_t frame = { .fin = true, .opcode = PROVEN_WS_TEXT, .rsv1 = true, .length = payload };
    proven_byte_t header[PROVEN_WS_MAX_FRAME_HEADER];
    proven_size_t header_len = 0;
    EXAMPLE_REQUIRE(proven_ws_frame_write((proven_mem_mut_t){ header, sizeof header }, &header_len, &frame) == PROVEN_OK && header[0] == 0xc1, "a frame header with the bit");
    printf("%u bytes of text go out as %u\n", (unsigned)(sizeof said - 1), (unsigned)(header_len + payload));
    EXAMPLE_REQUIRE(header_len + payload < sizeof said - 1, "fewer than were said");
    return EXAMPLE_OK();
}
```

### Reference

| API | Intent | Return |
|---|---|---|
| `compress` (both connection configurations) | Offer the extension (a client) or accept it when offered (a server). | - |
| `compress_level`, `compress_window_bits` (both) | As in Chapter 16, section 4: 1 to 9 or -1; 9 to 15. Zero: 6, and 15. | `INVALID_ARG` outside those ranges. |
| `proven_ws_conn_compressed(conn)`, `proven_ws_stream_compressed(ws)` | Whether the other side agreed. | `bool`. |
| `proven_ws_conn_set_compress(conn, on)`, `proven_ws_stream_set_compress(ws, on)` | Whether the messages this side sends from now on are compressed. For the one that must not be - a secret beside text the other side chose, or bytes compressed already: off, send, on. Nothing on a connection that did not agree to compression. | none. |
| `PROVEN_WS_DEFLATE_OFFER` | The `Sec-WebSocket-Extensions` value a client sends. | a string literal. |
| `proven_ws_deflate_accept(request, out, &len, &window_bits)` | A server's choice among the request's offers; writes the answer. | `proven_err_t`: `NOT_FOUND` when no offer can be met - go on without; `OUT_OF_BOUNDS` (`PROVEN_WS_DEFLATE_ANSWER_MAX` bytes are enough). |
| `proven_ws_check_response_deflate(response, key, offered, &protocol, &deflate)` | `proven_ws_check_response` for a client that offered. | the same, and `INVALID_FORMAT` for an answer in a form that was not offered. |
| `proven_ws_decoder_allow_compressed(decoder)` | Let the decoder pass compressed messages through, marked `compressed`. It does not inflate. | none. |
| `rsv1` in `proven_ws_frame_t` | The bit that marks a compressed message, on its first frame. | - |

### Cautions, and what goes wrong

- **Do not compress messages that carry a secret together with text the other side chose.**
  The length of a compressed message tells an observer of the connection how much the two
  have in common, with or without TLS - the leak described in Chapter 16, section 5. Because
  each message is compressed alone, one message cannot leak another; what is inside a single
  message still can. Compression is per connection here: a connection that carries such
  messages should not ask for it.
- **The limit is on the inflated size, and you should set one.** `max_message_bytes` is what
  stops a kilobyte from becoming a gigabyte. Its default is 1 MiB.
- **Raw DEFLATE has no checksum.** A compressed message damaged in transit can inflate to
  different bytes with no error. TLS prevents that; without it, check what matters.
- **A peer that offers only what cannot be met is served uncompressed, not refused.** The
  one such offer is a compression window of 256 bytes (`server_max_window_bits=8`), which
  this compressor does not have. A client's own window size (`client_max_window_bits`) is
  accepted and ignored: any window can be read.
- **A server that accepts in another form is refused by the client.** One that takes the
  offer without `server_no_context_takeover` would compress across messages, which this side
  does not read; the connection fails with `PROVEN_ERR_INVALID_FORMAT`. Servers that follow
  RFC 7692 either accept the offer as made or decline it.
- **Memory while a compressed message is in flight:** about 35 KiB to inflate one (51 KiB in
  the event-driven server), and from 11 KiB to 325 KiB to compress one, by the window and no
  more than the message needs. It is allocated for the message and freed with it - so a busy
  connection with compression allocates per message where one without does not.
- **With the codec alone, text is yours to check.** The decoder cannot check bytes that are
  not text yet; after inflating, check them. And count what comes out: the decoder's limit
  counts compressed bytes.

## 10. What is not here

- **Compression that carries its window from one message to the next** ("context takeover"),
  and any extension other than `permessage-deflate`. Section 9.
- **One call that waits on many connections.** See section 5.
- **Concurrent send and receive on one connection.**
- **Automatic pings, reconnection, backoff.** Policy, and yours.
- **Messages larger than memory through `ws_conn.h`.** A message is assembled whole; to stream
  one, use the decoder.
- **Checking `Origin` for you.** No server here refuses an origin by itself: the list of
  origins is yours, and so is the call (`proven_ws_check_origin`, section 2).
- **HTTP/2 WebSockets (RFC 8441).**
