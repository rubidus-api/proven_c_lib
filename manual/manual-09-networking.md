# Chapter 9: Networking

**Part V - Talking to the operating system. Prerequisites: Part II
([1](manual-01-foundation.md), [2](manual-02-allocation.md), [3](manual-03-strings-text.md)) and
[Chapter 5](manual-05-hosted-services.md) section 4 (the two clocks) and its section on streams.**
**After this chapter** you can open a TCP or UDP socket, put a time limit on everything that
waits, tell a refused connection from a lost one, serve many sockets from one thread, and write a
protocol that does not care what carries it.

This chapter covers `net.h`. Like Chapter 5 it needs an operating system and is not part of a
[freestanding](manual-freestanding.md) build; a hosted build can leave it out with `PROVEN_NO_NET`.

## Table of contents

1. [Three rules](#1-three-rules)
2. [Addresses](#2-addresses)
3. [Deadlines](#3-deadlines)
4. [TCP](#4-tcp)
5. [UDP](#5-udp)
6. [Many sockets, one thread](#6-many-sockets-one-thread)
7. [Transports](#7-transports)
8. [What the platforms do differently](#8-what-the-platforms-do-differently)
9. [What is not here](#9-what-is-not-here)

## 1. Three rules

BSD sockets are forty years old and every one of their sharp edges has a name. `net.h` is a thin
layer whose whole job is to take three of them away.

**Every call that can wait takes a deadline.** `recv` on a plain socket waits as long as the peer
keeps it waiting. A program built that way works on a desk and stops in production the first time
a client opens a connection and says nothing - and one such client is enough to stop a server that
handles one connection at a time. Here there is no way to write that program: a read, a write, an
accept and a connect each take a deadline argument, and you must say something.

**A failure says which failure.** "The call failed" is not enough to decide what to do next. Whether
the other side said no, went away, or finished properly are three different situations with three
different responses:

| What happened | Code | Sensible response |
|---|---|---|
| The peer finished sending | `PROVEN_ERR_EOF` | Stop reading; this is the normal end |
| The deadline passed | `PROVEN_ERR_TIMEOUT` | Retry, give up, or close - nothing is broken |
| Nothing listens at that address | `PROVEN_ERR_REFUSED` | Try another address, or report; a retry gets the same answer |
| The peer vanished, or the connection was aborted | `PROVEN_ERR_RESET` | Close; what was in flight may be lost |
| No route to the host or network | `PROVEN_ERR_UNREACHABLE` | Report, or retry later |
| The address to bind is taken, or the system is out of sockets | `PROVEN_ERR_BUSY` | Back off, or choose another port |
| The address may not be bound | `PROVEN_ERR_PERMISSION` | Report; a retry will not help |
| A host name has no address; a Unix-domain path is missing | `PROVEN_ERR_NOT_FOUND` | Report |
| A datagram did not fit the buffer, or is too large to send | `PROVEN_ERR_OUT_OF_BOUNDS` | Use a larger buffer; send less |
| The socket is not open | `PROVEN_ERR_INVALID_STATE` | Fix the sequence of calls |

`PROVEN_ERR_IO` is what is left when none of these fits.

**Nothing is hidden.** No call in this header allocates. A socket is a small value you own and
must close. A reader or writer over a socket uses state you declare. The one piece of process-wide
state - starting Winsock on Windows - is done once, on first use, and is not yours to manage.

## 2. Addresses

A `proven_net_addr_t` says where a socket is or where it should go. It is a plain value: copy it,
compare it, store it in a table.

```text
typedef struct {
    proven_net_family_t family;   /* IPV4, IPV6 or UNIX; NONE when unset */
    proven_u16 port;              /* host byte order: the number you would write down */
    proven_u32 scope_id;          /* IPv6 zone for a link-local address; 0 otherwise */
    proven_byte_t ip[16];         /* network order: 4 bytes for IPv4, 16 for IPv6 */
    proven_u8 path_len;           /* Unix-domain only */
    char path[104];               /* Unix-domain only */
} proven_net_addr_t;
```

### Reference

| API | Intent | Return |
|---|---|---|
| `proven_net_addr_ipv4(a, b, c, d, port)` | An IPv4 address from its four numbers. | `proven_net_addr_t`. |
| `proven_net_addr_ipv6(ip[16], port, scope_id)` | An IPv6 address from sixteen bytes in network order. | `proven_net_addr_t`. |
| `proven_net_addr_loopback(family, port)` | This machine only: `127.0.0.1` or `::1`. | `proven_net_addr_t`; family `NONE` if `family` has no loopback. |
| `proven_net_addr_any(family, port)` | Every interface: `0.0.0.0` or `::`. | `proven_net_addr_t`. |
| `proven_net_addr_unix(path, &out)` | A Unix-domain address from a filesystem path. | `proven_err_t`: `INVALID_ARG` for an empty path or one with a NUL, `OUT_OF_BOUNDS` past `PROVEN_NET_UNIX_PATH_MAX`. |
| `proven_net_addr_parse(text, port, &out)` | Parse a literal: `192.0.2.1`, `2001:db8::1`, `[2001:db8::1]`, `fe80::1%3`. Never touches the network. | `proven_err_t`: `INVALID_FORMAT` when `text` is not an address; `out` is then untouched. |
| `proven_net_addr_format(&addr, out, &written)` | Write `192.0.2.1:80`, `[2001:db8::1]:80`, or the path. No NUL. `PROVEN_NET_ADDR_TEXT_MAX` bytes are always enough. | `proven_err_t`: `OUT_OF_BOUNDS` when `out` is too small. |
| `proven_net_addr_eq(&a, &b)` | Same family, address, port and zone - or the same path. | `bool`. |
| `proven_net_resolve(host, port, out, cap, &count)` | Look a name up through the system resolver; a literal is answered without a query. | `proven_err_t`: `NOT_FOUND`, `TIMEOUT` (the name servers did not answer), `INVALID_ARG` for an empty or over-long name. |

Two decisions in the parser are deliberate. An IPv4 literal is exactly four decimal numbers:
`127.1` and `2130706433` are refused although older parsers accept both, and so is `010.0.0.1`,
which those parsers read as octal *eight*. And IPv6 has many spellings going in but one coming out
- lowercase, no leading zeros, the longest run of zero groups as `::` (RFC 5952) - so two equal
addresses print as equal text and the text can be a map key or a log field you search for.

### Cautions, and what goes wrong

**`proven_net_resolve` blocks and has no deadline.** It is the system resolver, which offers none,
and on a broken network it can take tens of seconds. It is the one call in this header that waits
without a limit, and the header says so rather than pretend.

Wrong - resolving inside the loop that serves every other connection:

```text
for (;;) {
    wait_for_events();
    if (client_wants_upstream)
        proven_net_resolve(name, 443, addrs, 4, &n);   /* wrong: every client waits on DNS */
}
```

Correct - resolve where waiting is acceptable: at startup, or on a job
([Chapter 6](manual-06-execution-and-platform.md)), and hand the address to the loop.

**Binding to "any" publishes the service.** `proven_net_addr_any` listens on every interface the
machine has, including the ones that face a network. A tool meant for your own machine binds
`proven_net_addr_loopback`.

Compiled and run by the test suite:

<!-- example: manual/examples/en/ex_09_net_addr.c -->
```c
/*
 * Addresses are plain values: build one, parse one from text, print it, compare two.
 * Nothing in this program opens a socket or asks the network anything.
 */

static bool prints_as(const proven_net_addr_t *addr, proven_u8str_view_t want) {
    proven_byte_t text[PROVEN_NET_ADDR_TEXT_MAX];
    proven_size_t n = 0;
    if (proven_net_addr_format(addr, (proven_mem_mut_t){ text, sizeof text }, &n) != PROVEN_OK) return false;
    return proven_u8str_view_eq((proven_u8str_view_t){ text, n }, want);
}

int main(void) {
    /* Built from parts. The port is the number you would write down - host byte order. */
    proven_net_addr_t web = proven_net_addr_ipv4(192, 0, 2, 10, 80);
    EXAMPLE_REQUIRE(prints_as(&web, PROVEN_LIT("192.0.2.10:80")), "an IPv4 address prints as address:port");

    /* The two fixed addresses a program needs most: this machine only, and every interface. */
    proven_net_addr_t local = proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 8080);
    proven_net_addr_t everywhere = proven_net_addr_any(PROVEN_NET_FAMILY_IPV6, 8080);
    EXAMPLE_REQUIRE(prints_as(&local, PROVEN_LIT("127.0.0.1:8080")), "loopback is 127.0.0.1");
    EXAMPLE_REQUIRE(prints_as(&everywhere, PROVEN_LIT("[::]:8080")), "any is ::, and IPv6 is bracketed before a port");

    /* Parsed from text. Many spellings of one IPv6 address parse to the same bytes, and there
     * is exactly one spelling on the way out, so the text can be compared and used as a key. */
    proven_net_addr_t a, b;
    EXAMPLE_REQUIRE(proven_net_addr_parse(PROVEN_LIT("2001:DB8:0:0:0:0:0:1"), 443, &a) == PROVEN_OK, "the long form parses");
    EXAMPLE_REQUIRE(proven_net_addr_parse(PROVEN_LIT("[2001:db8::1]"), 443, &b) == PROVEN_OK, "the short, bracketed form parses");
    EXAMPLE_REQUIRE(proven_net_addr_eq(&a, &b), "and they are the same address");
    EXAMPLE_REQUIRE(prints_as(&a, PROVEN_LIT("[2001:db8::1]:443")), "printed in the one canonical form");

    /* What is not an address is refused - including what other parsers quietly accept. */
    proven_net_addr_t bad;
    EXAMPLE_REQUIRE(proven_net_addr_parse(PROVEN_LIT("010.0.0.1"), 80, &bad) == PROVEN_ERR_INVALID_FORMAT,
                    "a leading zero is refused, not read as octal");
    EXAMPLE_REQUIRE(proven_net_addr_parse(PROVEN_LIT("example.com"), 80, &bad) == PROVEN_ERR_INVALID_FORMAT,
                    "a name is not a literal: that is proven_net_resolve's job");

    /* Sixteen raw bytes, in network order, when the address comes from a packet or a file. */
    proven_byte_t raw[16] = { 0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
    proven_net_addr_t link_local = proven_net_addr_ipv6(raw, 22, 3);
    EXAMPLE_REQUIRE(prints_as(&link_local, PROVEN_LIT("[fe80::1%3]:22")), "a link-local address carries its zone");

    /* A Unix-domain address is a filesystem path. */
    proven_net_addr_t sock;
    EXAMPLE_REQUIRE(proven_net_addr_unix(PROVEN_LIT("/run/app.sock"), &sock) == PROVEN_OK, "a path makes an address");
    EXAMPLE_REQUIRE(prints_as(&sock, PROVEN_LIT("/run/app.sock")), "and prints as the path");

    /* Resolving a literal is answered without a query, so this line works with no network.
     * A real name goes to the system resolver, blocks, and has no deadline: call it where
     * that is acceptable. */
    proven_net_addr_t found[4];
    proven_size_t count = 0;
    EXAMPLE_REQUIRE(proven_net_resolve(PROVEN_LIT("192.0.2.10"), 80, found, 4, &count) == PROVEN_OK && count == 1,
                    "a literal resolves to itself");
    EXAMPLE_REQUIRE(proven_net_addr_eq(&found[0], &web), "the same address as the one built by hand");

    return EXAMPLE_OK();
}
```

## 3. Deadlines

A deadline is a moment, not a duration: a reading of the monotonic clock
([Chapter 5 section 4](manual-05-hosted-services.md)) at which a call must give up.

| | Meaning |
|---|---|
| `proven_net_deadline_in(ms)` | The deadline `ms` milliseconds from now. |
| `PROVEN_NET_NO_DEADLINE` | Wait as long as it takes. A silent peer then waits with you. |
| `PROVEN_NET_DONT_WAIT` | Do what can be done right now; anything else is `PROVEN_ERR_TIMEOUT`. |

It is absolute so that one deadline can bound a whole exchange. "This request gets five seconds"
is one value passed to connect, to each write and to each read; with a per-call timeout the same
five seconds would have to be recomputed after every step, and usually is not.

It is on the monotonic clock so that setting the system time does not move it.

And a deadline that passes breaks nothing. `PROVEN_ERR_TIMEOUT` means "not yet"; the socket is
exactly as usable as it was. Whether to try again, give up, or close is yours to decide.

Wrong - a timeout treated as a dead connection:

```text
proven_result_size_t r = proven_net_read(&conn, buf, proven_net_deadline_in(100));
if (r.err != PROVEN_OK) proven_net_close(&conn);   /* wrong: TIMEOUT is not a failure of the connection */
```

Correct - decide what silence means for this protocol:

```text
if (r.err == PROVEN_ERR_TIMEOUT) { send_keepalive(&conn); continue; }
if (r.err == PROVEN_ERR_EOF)     { finish(&conn); break; }
if (r.err != PROVEN_OK)          { proven_net_close(&conn); break; }
```

## 4. TCP

A TCP connection is a stream of bytes in each direction. It has no message boundaries: what one
side sends in a single write the other may receive in three reads, or two writes in one read.

### Reference

| API | Intent | Return |
|---|---|---|
| `proven_net_listen(at, backlog, &listener, &bound)` | Listen at `at`. Port 0 lets the OS choose; `bound` (optional) is the address actually bound. | `proven_err_t`: `BUSY` (taken), `PERMISSION`, `INVALID_ARG` (not this machine's address), `UNSUPPORTED`. |
| `proven_net_accept(&listener, until, &conn, &peer)` | Take the next connection. `peer` is optional. | `proven_err_t`: `TIMEOUT` when none arrived. |
| `proven_net_listener_addr(&listener, &out)` | Where a listener is bound. | `proven_err_t`. |
| `proven_net_listener_close(&listener)` | Stop listening. Accepted connections are unaffected. | `proven_err_t`. |
| `proven_net_connect(to, until, &conn)` | Connect. On failure there is nothing to close. | `proven_err_t`: `REFUSED`, `TIMEOUT`, `UNREACHABLE`, `NOT_FOUND` (Unix-domain path). |
| `proven_net_read(&conn, dest, until)` | Read what has arrived, up to `dest.size`. | `proven_result_size_t`: `EOF` at the end - never zero bytes of success; `TIMEOUT`; `RESET`. |
| `proven_net_write(&conn, src, until)` | Write what fits now. `.value` is how much went, also on failure. | `proven_result_size_t`. |
| `proven_net_write_all(&conn, src, until)` | Write all of `src`, or fail saying how far it got. | `proven_result_size_t`: `.value == src.size` exactly when `.err` is OK. |
| `proven_net_shutdown_write(&conn)` | "Nothing more to send." The peer reads `EOF`; this side can still read. | `proven_err_t`. |
| `proven_net_close(&conn)` | Close. Safe on a connection that is not open. | `proven_err_t`. |
| `proven_net_conn_local_addr(&conn, &out)`, `proven_net_conn_peer_addr(&conn, &out)` | The two ends of a connection. | `proven_err_t`. |
| `proven_net_conn_set_nodelay(&conn, on)` | Send small writes at once instead of gathering them. | `proven_err_t`: `UNSUPPORTED` on a Unix-domain connection. |
| `proven_net_listener_is_open(&l)`, `proven_net_conn_is_open(&c)` | Whether the value holds an open socket. | `bool`. |

`proven_net_listener_t` and `proven_net_conn_t` are small values you own. Close each one you
opened, and do not copy one that is open: two copies are two owners of one socket.

The same calls serve **Unix-domain** stream sockets - pass an address made by
`proven_net_addr_unix`. Listening creates the path; closing the listener does not remove it, so a
server removes a leftover path before it listens.

### Cautions, and what goes wrong

**One read is not one message.** A stream delivers bytes, in order, in whatever pieces the network
made of them.

Wrong - assuming the request arrives in one read:

```text
proven_result_size_t r = proven_net_read(&conn, buf, until);
handle_request(buf, r.value);     /* wrong: this may be half a request, or one and a half */
```

Correct - read until the protocol says the message is complete: a length you were told, a
delimiter you were promised, or `PROVEN_ERR_EOF`. The worked example below reads to the end.

**One write may not send everything.** `proven_net_write` reports how much went. Ignoring that
count silently truncates the message whenever the buffer happens to be full - which is to say,
under load, in production. `proven_net_write_all` is the call that keeps going; and when it fails,
its count is what tells you whether resending would duplicate data.

**A reset is not an end.** `PROVEN_ERR_EOF` means the peer said it was finished and everything it
sent has been read. `PROVEN_ERR_RESET` means it went away without saying so, and some of what was
in flight - in either direction - may be gone. A file transfer that ends in `EOF` is complete; one
that ends in `RESET` is not, however many bytes arrived.

**Closing is not the same as finishing.** To be sure the peer received everything, say you are
done with `proven_net_shutdown_write`, then read until `EOF`, then close. Closing a connection
that still has unread data in it is how the *other* side gets `PROVEN_ERR_RESET`.

Compiled and run by the test suite:

<!-- example: manual/examples/en/ex_09_net_tcp.c -->
```c
/*
 * A TCP exchange, start to finish, with a deadline on everything that can wait.
 *
 * Both ends live in this one program, which works because the kernel completes a connection as
 * soon as the listener's backlog takes it - before accept is called. A real client and server
 * are these same calls in two processes.
 */

int main(void) {
    /* Port 0: the OS picks a free one, and `at` says which. Loopback: this machine only. */
    proven_net_listener_t listener;
    proven_net_addr_t at;
    proven_err_t err = proven_net_listen(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), 16, &listener, &at);
    if (err == PROVEN_ERR_PERMISSION || err == PROVEN_ERR_UNSUPPORTED) {
        printf("no sockets in this environment; nothing to show\n");
        return EXAMPLE_OK();
    }
    EXAMPLE_REQUIRE(err == PROVEN_OK && proven_net_listener_is_open(&listener), "the listener opens");
    EXAMPLE_REQUIRE(at.port != 0, "and reports the port it was given");

    /* One deadline for the whole exchange: five seconds from now, however many calls it takes. */
    proven_net_deadline_t until = proven_net_deadline_in(5000);

    proven_net_conn_t client, server;
    proven_net_addr_t peer;
    EXAMPLE_REQUIRE(proven_net_connect(at, until, &client) == PROVEN_OK, "the client connects");
    EXAMPLE_REQUIRE(proven_net_accept(&listener, until, &server, &peer) == PROVEN_OK, "the server accepts");
    EXAMPLE_REQUIRE(proven_net_conn_is_open(&client) && proven_net_conn_is_open(&server), "two open ends");

    /* Each end knows both addresses. The accepted peer is the client's own local address. */
    proven_net_addr_t client_local, client_peer, listening;
    EXAMPLE_REQUIRE(proven_net_conn_local_addr(&client, &client_local) == PROVEN_OK &&
                    proven_net_conn_peer_addr(&client, &client_peer) == PROVEN_OK, "the client's two addresses");
    EXAMPLE_REQUIRE(proven_net_addr_eq(&client_local, &peer), "the server saw the client's address");
    EXAMPLE_REQUIRE(proven_net_listener_addr(&listener, &listening) == PROVEN_OK &&
                    proven_net_addr_eq(&client_peer, &listening), "the client's peer is where the server listens");

    /* A request written in two small pieces: without this they may wait to be sent together. */
    EXAMPLE_REQUIRE(proven_net_conn_set_nodelay(&client, true) == PROVEN_OK, "small writes go at once");

    /* write_all sends everything or says how far it got. */
    proven_mem_view_t request = proven_mem_view_from_u8(PROVEN_LIT("what time is it?"));
    proven_result_size_t sent = proven_net_write_all(&client, request, until);
    EXAMPLE_REQUIRE(sent.err == PROVEN_OK && sent.value == request.size, "the whole request went");

    /* "I have nothing more to send." The server will read the request, then end of input. */
    EXAMPLE_REQUIRE(proven_net_shutdown_write(&client) == PROVEN_OK, "the client half-closes");

    /* The server reads until the end. A read returns what has arrived - a stream has no message
     * boundaries - so a loop is the only correct way to read "all of it". */
    proven_byte_t buf[64];
    proven_size_t got = 0;
    for (;;) {
        proven_result_size_t r = proven_net_read(&server, (proven_mem_mut_t){ buf + got, sizeof buf - got }, until);
        if (r.err == PROVEN_ERR_EOF) break;                  /* the client finished */
        EXAMPLE_REQUIRE(r.err == PROVEN_OK, "a read that is not the end succeeds");
        if (r.err != PROVEN_OK) break;
        got += r.value;
    }
    EXAMPLE_REQUIRE(got == request.size, "the server received the whole request");

    /* A single write may send only part; the count says how much. Here it all fits. */
    proven_mem_view_t reply = proven_mem_view_from_u8(PROVEN_LIT("time to close"));
    proven_result_size_t w = proven_net_write(&server, reply, until);
    EXAMPLE_REQUIRE(w.err == PROVEN_OK && w.value == reply.size, "a short reply goes in one write");

    proven_result_size_t r = proven_net_read(&client, (proven_mem_mut_t){ buf, sizeof buf }, until);
    EXAMPLE_REQUIRE(r.err == PROVEN_OK && r.value == reply.size, "the client reads the reply after its half-close");

    /* Nothing more is coming, and the client will not wait for ever to find that out. A timeout
     * is not damage: the connection is exactly as usable as before. */
    r = proven_net_read(&client, (proven_mem_mut_t){ buf, sizeof buf }, proven_net_deadline_in(50));
    EXAMPLE_REQUIRE(r.err == PROVEN_ERR_TIMEOUT, "silence is a timeout, not a hang");

    /* Every socket is closed by its owner. */
    EXAMPLE_REQUIRE(proven_net_close(&server) == PROVEN_OK, "the server closes its end");
    r = proven_net_read(&client, (proven_mem_mut_t){ buf, sizeof buf }, until);
    EXAMPLE_REQUIRE(r.err == PROVEN_ERR_EOF, "which the client reads as the end");
    EXAMPLE_REQUIRE(proven_net_close(&client) == PROVEN_OK, "the client closes");
    EXAMPLE_REQUIRE(proven_net_listener_close(&listener) == PROVEN_OK, "the listener closes");

    /* With the listener gone, the same address refuses - a different answer from a timeout. */
    proven_net_conn_t nobody;
    EXAMPLE_REQUIRE(proven_net_connect(at, proven_net_deadline_in(15000), &nobody) == PROVEN_ERR_REFUSED,
                    "nothing listening is PROVEN_ERR_REFUSED");

    return EXAMPLE_OK();
}
```

## 5. UDP

UDP sends messages - datagrams - one at a time, each on its own. A datagram arrives whole or not
at all; datagrams may arrive out of order, twice, or never, and nothing tells the sender which.

### Reference

| API | Intent | Return |
|---|---|---|
| `proven_net_udp_open(at, &udp, &bound)` | Open a socket bound to `at`. A client binds `proven_net_addr_any(family, 0)`. | `proven_err_t`: `BUSY`, `PERMISSION`, `UNSUPPORTED` (a Unix-domain address). |
| `proven_net_udp_send_to(&udp, to, data, until)` | Send one datagram: all of `data` or nothing. | `proven_err_t`: `OUT_OF_BOUNDS` when `data` is larger than a datagram can be. |
| `proven_net_udp_recv_from(&udp, dest, &from, until)` | Receive one datagram; `from` is optional. | `proven_result_size_t`: `OUT_OF_BOUNDS` with `.value == dest.size` when it was cut; `TIMEOUT`. |
| `proven_net_udp_addr(&udp, &out)` | Where the socket is bound. | `proven_err_t`. |
| `proven_net_udp_close(&udp)` | Close. Safe on a socket that is not open. | `proven_err_t`. |
| `proven_net_udp_is_open(&udp)` | Whether the value holds an open socket. | `bool`. |

### Cautions, and what goes wrong

**"Sent" means it left, not that it arrived.** A successful `proven_net_udp_send_to` has handed the
datagram to the network. A protocol that needs an answer waits for one with a deadline and sends
again when it does not come.

**A zero-length datagram is a message.** On a stream, zero bytes would mean the end, which is why
`proven_net_read` never returns it. A datagram can really be empty, so `proven_net_udp_recv_from`
returning success with `.value` 0 is not an end and not an error.

**A buffer that is too small loses the rest.** The part of a datagram that did not fit is not
kept for the next receive - it is gone.

Wrong - a buffer sized by guess, and the error ignored:

```text
proven_byte_t buf[64];
proven_result_size_t r = proven_net_udp_recv_from(&udp, buf_mem, &from, until);
parse(buf, r.value);      /* wrong: on OUT_OF_BOUNDS this is the first 64 bytes of something longer */
```

Correct - size the buffer for the largest message the protocol allows, and treat
`PROVEN_ERR_OUT_OF_BOUNDS` as a malformed packet.

Compiled and run by the test suite:

<!-- example: manual/examples/en/ex_09_net_udp.c -->
```c
/*
 * UDP: messages, not a stream. Each send is one datagram and each receive is one datagram,
 * with the sender's address attached - and no promise that it arrives.
 */

int main(void) {
    proven_net_udp_t server, client;
    proven_net_addr_t server_at, client_at;
    proven_err_t err = proven_net_udp_open(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &server, &server_at);
    if (err == PROVEN_ERR_PERMISSION || err == PROVEN_ERR_UNSUPPORTED) {
        printf("no sockets in this environment; nothing to show\n");
        return EXAMPLE_OK();
    }
    EXAMPLE_REQUIRE(err == PROVEN_OK && proven_net_udp_is_open(&server), "the server's socket opens");
    EXAMPLE_REQUIRE(proven_net_udp_open(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &client, NULL) == PROVEN_OK,
                    "and the client's");
    EXAMPLE_REQUIRE(proven_net_udp_addr(&client, &client_at) == PROVEN_OK && client_at.port != 0,
                    "a socket can be asked where it is bound");

    proven_net_deadline_t until = proven_net_deadline_in(2000);

    /* Two sends are two datagrams. Success means each left this machine. */
    EXAMPLE_REQUIRE(proven_net_udp_send_to(&client, server_at, proven_mem_view_from_u8(PROVEN_LIT("first")), until) == PROVEN_OK &&
                    proven_net_udp_send_to(&client, server_at, proven_mem_view_from_u8(PROVEN_LIT("second message")), until) == PROVEN_OK,
                    "two datagrams leave");

    /* Each receive is one whole datagram and says who sent it. */
    proven_byte_t buf[32];
    proven_net_addr_t from;
    proven_result_size_t r = proven_net_udp_recv_from(&server, (proven_mem_mut_t){ buf, sizeof buf }, &from, until);
    EXAMPLE_REQUIRE(r.err == PROVEN_OK && r.value == 5, "the first datagram, alone: five bytes");
    EXAMPLE_REQUIRE(proven_net_addr_eq(&from, &client_at), "from the client's address");

    /* A buffer that is too small cuts the datagram and says so. The rest of it is gone - the
     * next receive is the next datagram - so size the buffer for the largest message. */
    proven_byte_t small[6];
    r = proven_net_udp_recv_from(&server, (proven_mem_mut_t){ small, sizeof small }, &from, until);
    EXAMPLE_REQUIRE(r.err == PROVEN_ERR_OUT_OF_BOUNDS && r.value == sizeof small,
                    "fourteen bytes into six: PROVEN_ERR_OUT_OF_BOUNDS, six kept");

    /* The reply goes to the address the datagram came from. */
    EXAMPLE_REQUIRE(proven_net_udp_send_to(&server, from, proven_mem_view_from_u8(PROVEN_LIT("ack")), until) == PROVEN_OK,
                    "the server answers the sender");
    r = proven_net_udp_recv_from(&client, (proven_mem_mut_t){ buf, sizeof buf }, NULL, until);
    EXAMPLE_REQUIRE(r.err == PROVEN_OK && r.value == 3, "and the client receives it");

    /* Nothing else is coming. A lost datagram looks exactly like this, which is why every
     * receive has a deadline and every UDP protocol has a retry. */
    r = proven_net_udp_recv_from(&client, (proven_mem_mut_t){ buf, sizeof buf }, NULL, proven_net_deadline_in(50));
    EXAMPLE_REQUIRE(r.err == PROVEN_ERR_TIMEOUT, "silence is a timeout");

    EXAMPLE_REQUIRE(proven_net_udp_close(&client) == PROVEN_OK && proven_net_udp_close(&server) == PROVEN_OK,
                    "both sockets are closed by their owner");
    return EXAMPLE_OK();
}
```

## 6. Many sockets, one thread

A deadline bounds a wait on one socket. A server waits on all of its sockets at once and serves
whichever needs it: that is `proven_net_poll`.

### Reference

| API | Intent | Return |
|---|---|---|
| `proven_net_listener_handle(&l)`, `proven_net_conn_handle(&c)`, `proven_net_udp_handle(&u)` | A socket as the poll sees it, whatever its kind. | `proven_net_handle_t`; not `valid` for a socket that is not open. |
| `proven_net_poll(items, count, until, &ready)` | Wait until at least one item is ready. Up to `PROVEN_NET_POLL_INLINE_MAX` (64) items. | `proven_err_t`: `TIMEOUT` when none was (`ready` is 0); `OUT_OF_BOUNDS` for too many items; `INVALID_ARG` for an invalid handle. |
| `proven_net_poll_with(scratch, items, count, until, &ready)` | The same for any number of items, using memory you supply. | `proven_err_t`, and `OUT_OF_BOUNDS` when `scratch` is too small. |
| `proven_net_poll_scratch_size(count)` | How much scratch `count` items need. | `proven_size_t`; `SIZE_MAX` when it cannot be represented. |

```text
typedef struct {
    proven_net_handle_t handle;
    proven_u8 want;   /* PROVEN_NET_READABLE and/or PROVEN_NET_WRITABLE */
    proven_u8 got;    /* filled in: zero, or what is ready - which may include PROVEN_NET_FAILED */
} proven_net_poll_item_t;
```

**Ready means "will not wait", not "will succeed".** A readable connection may hold data, or the
end, or an error; a readable listener has a connection to accept. So the call that follows a
ready report takes `PROVEN_NET_DONT_WAIT`, and its result is what you act on. An item reported
`PROVEN_NET_FAILED` is in error or hung up; the next read or write on it says which.

**Ask for writability only when you have something to write.** An idle connection is almost always
writable, so a loop that always asks is woken at once, every time, and spins a processor doing
nothing.

Wrong:

```text
items[i].want = PROVEN_NET_READABLE | PROVEN_NET_WRITABLE;   /* wrong when nothing is queued: never sleeps */
```

Correct - `PROVEN_NET_WRITABLE` only for a connection whose last write returned less than you
asked or timed out, and only until that data has gone.

`proven_net_poll_with` exists because the operating system wants an array of its own and this
library does not allocate one behind your back: you give it working memory, sized by
`proven_net_poll_scratch_size`, and reuse it for the next call. Readiness here is `poll` on POSIX
and `WSAPoll` on Windows - the right tool for hundreds of sockets, not for hundreds of thousands.

Compiled and run by the test suite:

<!-- example: manual/examples/en/ex_09_net_poll.c -->
```c
/*
 * One thread, many sockets: wait until any of them needs attention, then serve the ones
 * that do. This is the loop inside every single-threaded server.
 *
 * "Ready" means the next call of that kind will not wait - so each socket reported ready is
 * handled with PROVEN_NET_DONT_WAIT, and the loop never blocks anywhere but in the poll.
 */

int main(void) {
    proven_net_listener_t listener;
    proven_net_addr_t at;
    proven_err_t err = proven_net_listen(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), 16, &listener, &at);
    if (err == PROVEN_ERR_PERMISSION || err == PROVEN_ERR_UNSUPPORTED) {
        printf("no sockets in this environment; nothing to show\n");
        return EXAMPLE_OK();
    }
    EXAMPLE_REQUIRE(err == PROVEN_OK, "the listener opens");

    /* Two clients connect and each sends one message. */
    proven_net_conn_t clients[2];
    for (int i = 0; i < 2; ++i) {
        EXAMPLE_REQUIRE(proven_net_connect(at, proven_net_deadline_in(5000), &clients[i]) == PROVEN_OK, "a client connects");
    }
    EXAMPLE_REQUIRE(proven_net_write_all(&clients[0], proven_mem_view_from_u8(PROVEN_LIT("from the first")), proven_net_deadline_in(5000)).err == PROVEN_OK &&
                    proven_net_write_all(&clients[1], proven_mem_view_from_u8(PROVEN_LIT("from the second")), proven_net_deadline_in(5000)).err == PROVEN_OK,
                    "each sends a message");

    /* The server: a listener and up to two connections, in one list. */
    proven_net_conn_t served[2] = {0};
    proven_size_t served_count = 0;
    proven_size_t bytes_seen = 0;

    proven_net_deadline_t until = proven_net_deadline_in(5000);
    while (bytes_seen < 14 + 15) {
        proven_net_poll_item_t items[3];
        proven_size_t count = 0;
        items[count++] = (proven_net_poll_item_t){ .handle = proven_net_listener_handle(&listener), .want = PROVEN_NET_READABLE };
        for (proven_size_t i = 0; i < served_count; ++i) {
            items[count++] = (proven_net_poll_item_t){ .handle = proven_net_conn_handle(&served[i]), .want = PROVEN_NET_READABLE };
        }

        proven_size_t ready = 0;
        err = proven_net_poll(items, count, until, &ready);
        EXAMPLE_REQUIRE(err == PROVEN_OK && ready > 0, "something becomes ready before the deadline");
        if (err != PROVEN_OK) break;

        /* A readable listener has a connection waiting. */
        if (items[0].got & PROVEN_NET_READABLE) {
            EXAMPLE_REQUIRE(served_count < 2 &&
                            proven_net_accept(&listener, PROVEN_NET_DONT_WAIT, &served[served_count], NULL) == PROVEN_OK,
                            "the accept that poll predicted does not wait");
            served_count++;
        }
        /* A readable connection has data, or its end. `items[1 + i]` was built from `served[i]`. */
        for (proven_size_t i = 1; i < count; ++i) {
            if (!items[i].got) continue;
            proven_byte_t buf[64];
            proven_result_size_t r = proven_net_read(&served[i - 1], (proven_mem_mut_t){ buf, sizeof buf }, PROVEN_NET_DONT_WAIT);
            EXAMPLE_REQUIRE(r.err == PROVEN_OK, "the read that poll predicted does not wait");
            bytes_seen += r.value;
        }
    }
    EXAMPLE_REQUIRE(served_count == 2 && bytes_seen == 29, "both clients were accepted and both messages read, in one thread");

    /* proven_net_poll takes up to PROVEN_NET_POLL_INLINE_MAX sockets. A server with more passes
     * its own working memory: the OS wants an array, and this library does not allocate one. */
    proven_net_poll_item_t one = { .handle = proven_net_conn_handle(&served[0]), .want = PROVEN_NET_WRITABLE };
    proven_byte_t scratch[256];
    proven_size_t ready = 0;
    EXAMPLE_REQUIRE(proven_net_poll_scratch_size(1) <= sizeof scratch, "the scratch for one item is small");
    EXAMPLE_REQUIRE(proven_net_poll_with((proven_mem_mut_t){ scratch, sizeof scratch }, &one, 1, PROVEN_NET_DONT_WAIT, &ready) == PROVEN_OK &&
                    (one.got & PROVEN_NET_WRITABLE), "an idle connection is writable at once");

    /* A UDP socket waits in the same list as everything else. */
    proven_net_udp_t udp;
    EXAMPLE_REQUIRE(proven_net_udp_open(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &udp, NULL) == PROVEN_OK, "a UDP socket");
    proven_net_poll_item_t quiet = { .handle = proven_net_udp_handle(&udp), .want = PROVEN_NET_READABLE };
    EXAMPLE_REQUIRE(proven_net_poll(&quiet, 1, proven_net_deadline_in(30), &ready) == PROVEN_ERR_TIMEOUT && ready == 0,
                    "nothing arrives on it: PROVEN_ERR_TIMEOUT, the idle tick of an event loop");
    (void)proven_net_udp_close(&udp);

    for (int i = 0; i < 2; ++i) {
        (void)proven_net_close(&clients[i]);
        (void)proven_net_close(&served[i]);
    }
    (void)proven_net_listener_close(&listener);
    return EXAMPLE_OK();
}
```

## 7. Transports

A protocol - a line-based command set, an HTTP exchange - should be written once and not know
what carries its bytes. Today that is a TCP connection. Under TLS it is an encrypted session over
one. In a test it is two memory buffers and no network at all. `proven_transport_t` is that seam.

```text
typedef struct {
    void *ctx;
    proven_result_size_t (*read_fn)(void *ctx, proven_mem_mut_t dest, proven_net_deadline_t until);
    proven_result_size_t (*write_fn)(void *ctx, proven_mem_view_t src, proven_net_deadline_t until);
    proven_err_t (*shutdown_fn)(void *ctx);   /* may be NULL */
    proven_err_t (*close_fn)(void *ctx);      /* may be NULL */
} proven_transport_t;
```

It is a small vtable passed by value, the same shape as `proven_allocator_t`
([Chapter 2](manual-02-allocation.md)) and `proven_writer_t`, and for the same reason: the caller
decides, and nothing is hidden. To supply your own, fill in the struct. The contract is that of
the TCP calls: a read never succeeds with zero bytes, and a write reports the bytes that went even
when it then fails.

### Reference

| API | Intent | Return |
|---|---|---|
| `proven_net_conn_transport(&conn)` | A connection as a transport. Closing the transport closes the connection. | `proven_transport_t`. |
| `proven_transport_is_valid(t)` | Whether it has a read and a write function. | `bool`. |
| `proven_transport_read(t, dest, until)`, `proven_transport_write(t, src, until)` | As `proven_net_read` and `proven_net_write`. | `proven_result_size_t`; `INVALID_ARG` for an invalid transport. |
| `proven_transport_write_all(t, src, until)` | Write everything, or fail with the count. A transport that takes nothing and reports no error is `PROVEN_ERR_IO`, not an endless loop. | `proven_result_size_t`. |
| `proven_transport_shutdown(t)`, `proven_transport_close(t)` | End the sending side; close. `PROVEN_OK` when the transport has nothing to do. | `proven_err_t`. |
| `proven_transport_reader(&state, t, timeout_ms)` | A `proven_reader_t` over a transport, for the line reader and anything else that reads a stream. | `proven_reader_t`; not valid if `state` is NULL or `t` is invalid. |
| `proven_transport_writer(&state, t, timeout_ms)` | A `proven_writer_t` over a transport, so `proven_fprint` can send. | `proven_writer_t`. |

The reader and writer adapters connect a socket to everything in
[Chapter 5's stream section](manual-05-hosted-services.md#streams-writers-and-readers). A reader
has nowhere to pass a deadline, so each read or write through an adapter gets its own:
`timeout_ms` from the moment it starts, or no deadline when that is 0. The
`proven_transport_stream_t` state is [caller-owned](manual-00-start-here.md#92-caller-owned-state---no-destroy-do-not-copy):
it must outlive the reader or writer and must not move.

The writer sends each piece as it is given. Wrap it in `proven_writer_buffered` when a message is
formatted in many small pieces, so that it leaves in few packets - and flush before you wait for
the reply.

Compiled and run by the test suite:

<!-- example: manual/examples/en/ex_09_net_transport.c -->
```c
/*
 * A protocol written once, against proven_transport_t, and run over a TCP connection.
 *
 * The function below does not know what carries its bytes. Today it is a socket; under TLS it
 * will be an encrypted session over a socket; in a test it can be two memory buffers. That is
 * what the interface is for.
 */

/* Send one request line and read one reply line, each with a five-second limit per step. */
static proven_err_t ask(proven_transport_t t, const char *question, proven_u8str_view_t expected) {
    if (!proven_transport_is_valid(t)) return PROVEN_ERR_INVALID_ARG;

    /* The formatter writes straight to the transport. */
    proven_transport_stream_t out_state;
    proven_writer_t out = proven_transport_writer(&out_state, t, 5000);
    proven_err_t err = proven_fprintln(out, "ASK {}", PROVEN_ARG(question)).err;
    if (err != PROVEN_OK) return err;

    /* The line reader reads from it, into a buffer this function owns. */
    proven_transport_stream_t in_state;
    proven_byte_t line_buf[128];
    proven_reader_buffered_t lines;
    (void)proven_reader_buffered(&lines, proven_transport_reader(&in_state, t, 5000),
                                 (proven_mem_mut_t){ line_buf, sizeof line_buf });
    proven_result_u8str_view_t reply = proven_reader_read_line(&lines);
    if (reply.err != PROVEN_OK) return reply.err;
    return proven_u8str_view_eq(reply.val, expected) ? PROVEN_OK : PROVEN_ERR_INVALID_FORMAT;
}

int main(void) {
    proven_net_listener_t listener;
    proven_net_addr_t at;
    proven_err_t err = proven_net_listen(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), 4, &listener, &at);
    if (err == PROVEN_ERR_PERMISSION || err == PROVEN_ERR_UNSUPPORTED) {
        printf("no sockets in this environment; nothing to show\n");
        return EXAMPLE_OK();
    }
    EXAMPLE_REQUIRE(err == PROVEN_OK, "the listener opens");

    proven_net_conn_t client_conn, server_conn;
    EXAMPLE_REQUIRE(proven_net_connect(at, proven_net_deadline_in(5000), &client_conn) == PROVEN_OK &&
                    proven_net_accept(&listener, proven_net_deadline_in(5000), &server_conn, NULL) == PROVEN_OK,
                    "a connected pair");

    /* A connection as a transport. From here on, neither side names a socket. */
    proven_transport_t client = proven_net_conn_transport(&client_conn);
    proven_transport_t server = proven_net_conn_transport(&server_conn);
    proven_net_deadline_t until = proven_net_deadline_in(5000);

    /* The server's half, done by hand: the reply is written before the client asks, which is
     * fine in one thread because the bytes simply wait in the connection. */
    proven_result_size_t w = proven_transport_write_all(server, proven_mem_view_from_u8(PROVEN_LIT("ANSWER 42\n")), until);
    EXAMPLE_REQUIRE(w.err == PROVEN_OK && w.value == 10, "the server's reply is on its way");

    EXAMPLE_REQUIRE(ask(client, "the question", PROVEN_LIT("ANSWER 42")) == PROVEN_OK,
                    "the protocol function sends its line and reads the reply");

    /* And the server reads what the client wrote, with the raw calls. */
    proven_byte_t buf[64];
    proven_result_size_t r = proven_transport_read(server, (proven_mem_mut_t){ buf, sizeof buf }, until);
    EXAMPLE_REQUIRE(r.err == PROVEN_OK &&
                    proven_u8str_view_eq((proven_u8str_view_t){ buf, r.value }, PROVEN_LIT("ASK the question\n")),
                    "the request line arrived as written");

    /* A single write reports how much went; here, all of it. */
    w = proven_transport_write(server, proven_mem_view_from_u8(PROVEN_LIT("BYE\n")), until);
    EXAMPLE_REQUIRE(w.err == PROVEN_OK && w.value == 4, "one more line");

    /* Ending and closing go through the same interface. */
    EXAMPLE_REQUIRE(proven_transport_shutdown(server) == PROVEN_OK, "the server ends its sending side");
    r = proven_transport_read(client, (proven_mem_mut_t){ buf, sizeof buf }, until);
    EXAMPLE_REQUIRE(r.err == PROVEN_OK && r.value == 4, "the client reads the last line");
    r = proven_transport_read(client, (proven_mem_mut_t){ buf, sizeof buf }, until);
    EXAMPLE_REQUIRE(r.err == PROVEN_ERR_EOF, "and then the end");

    EXAMPLE_REQUIRE(proven_transport_close(client) == PROVEN_OK && proven_transport_close(server) == PROVEN_OK,
                    "closing a transport closes what carries it");
    EXAMPLE_REQUIRE(!proven_net_conn_is_open(&client_conn), "the connection underneath is closed");
    (void)proven_net_listener_close(&listener);
    return EXAMPLE_OK();
}
```

## 8. What the platforms do differently

The calls behave the same on Linux, the BSDs, macOS and Windows. These are the differences the
library absorbs, written down because they are the ones that bite when sockets are used directly:

| The trap | What `net.h` does |
|---|---|
| A write to a closed connection raises `SIGPIPE` and kills the process (POSIX). | Every send suppresses the signal; the write returns `PROVEN_ERR_RESET`. No signal handler is installed for you. |
| Winsock must be started before any call, name resolution included. | Started once, on first use. |
| On Windows `SO_REUSEADDR` lets another process bind your port and take your connections. | A listener uses `SO_REUSEADDR` on POSIX and `SO_EXCLUSIVEADDRUSE` on Windows: a second listener on one address is `PROVEN_ERR_BUSY` everywhere. |
| An IPv6 listener also accepts IPv4 on some systems and not on others. | IPv6 sockets are IPv6-only everywhere. A dual-stack server opens two listeners. |
| A non-blocking Windows send accepts a buffer of any size and queues it all. | Sends are offered in bounded pieces, so a full buffer pushes back - and a deadline can fire - as it does elsewhere. |
| Windows reports a peer's orderly close to `WSAPoll` as a hang-up, not as readable. | Reported as readable, as on POSIX: the read that follows returns `PROVEN_ERR_EOF`. |
| A UDP send to a closed port makes a *later* receive on the same Windows socket fail. | That report is turned off; a receive answers for the datagram it received. |
| A child process inherits open sockets. | Every socket is created not inheritable (close-on-exec). |
| Winsock answers a Unix-domain connect to a missing path as "refused". | `PROVEN_ERR_NOT_FOUND`, as on POSIX. |

Two things differ and stay different. Windows waits about two seconds before it reports a
refused loopback connection, where POSIX answers at once - allow for it in the deadline. And
Unix-domain sockets need Windows 10 version 1803 or later; where the family is missing,
`proven_net_listen` and `proven_net_connect` return `PROVEN_ERR_UNSUPPORTED`.

## 9. What is not here

- **TLS.** The transport interface is where it will attach; this version has none, and a
  connection made here is not encrypted.
- **A deadline on name resolution.** See section 2.
- **Scale beyond `poll`.** No `epoll`, `kqueue` or completion ports.
- **Unix-domain datagrams, raw sockets, multicast, socket options beyond `TCP_NODELAY`.**
- **HTTP.** The message codec is [Chapter 10](manual-10-http.md); the client and server that drive it over these sockets are being built.
