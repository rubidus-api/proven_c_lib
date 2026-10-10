# Chapter 14: TLS

**Part V - Talking to the operating system. Prerequisites: [Chapter 13](manual-13-certificates.md)
(certificates and trust anchors); [Chapter 11](manual-11-http-client-server.md) for HTTPS,
[Chapter 12](manual-12-websocket.md) for `wss`.**
**After this chapter** you can serve and fetch over HTTPS, open a `wss` connection, wrap any
connection of your own in TLS, drive the protocol yourself where there is no socket - and say
what your program believes about its peer, and why.

This chapter covers `tls.h`. The engine in it calls no operating system and is available in a
[freestanding](manual-freestanding.md) build; the transport wrapper and everything that touches
a socket are hosted-only, and `PROVEN_NO_NET` leaves them out.

**Read this first.** This is a new implementation of TLS with no external audit. It is tested
against the protocol's published example handshakes, against two other implementations in both
roles, and against collections of adversarial inputs (section 9 says exactly what, and what was
not possible). That is evidence, not proof. A program whose users depend on TLS against a
capable adversary should terminate it in something audited and use this library behind it.

## Table of contents

1. [What TLS does, in one page](#1-what-tls-does-in-one-page)
2. [A configuration](#2-a-configuration)
3. [HTTPS and wss: the two lines](#3-https-and-wss-the-two-lines)
4. [Any connection: the transport wrapper](#4-any-connection-the-transport-wrapper)
5. [The engine](#5-the-engine)
6. [Resumption](#6-resumption)
7. [Client certificates and pins](#7-client-certificates-and-pins)
8. [When it fails](#8-when-it-fails)
9. [What is negotiated, what is held, what is not here](#9-what-is-negotiated-what-is-held-what-is-not-here)
10. [The legacy set: what each setting exposes you to](#10-the-legacy-set-what-each-setting-exposes-you-to)

## 1. What TLS does, in one page

A TCP connection gives you a pipe. Everyone between the two ends - the cafe's access point, an
internet provider, whoever compromised a router - can read what goes through it and change it,
and neither end can tell. TLS (Transport Layer Security; this library speaks version 1.3,
RFC 8446, and a cut-down version 1.2, RFC 5246 - section 9 says which part; older parts exist
only for a configuration that asks for them by name, section 10) puts three properties on top
of the pipe:

| Property | Meaning | What it is made from |
|---|---|---|
| **Authentication** | The other end is who you meant to reach | Its certificate, checked as Chapter 13 describes, and a signature proving it holds the certificate's key |
| **Confidentiality** | Nobody in between can read the data | Keys agreed by a key exchange that those in between can watch and still not compute |
| **Integrity** | Nobody in between can change, drop or reorder the data unnoticed | Every record carries a tag only a key holder could have made; a clean end is itself a message |

It happens in two phases. The **handshake** is a few messages at the start: the two sides
agree on algorithms, exchange key shares, the server (and optionally the client) presents its
certificate and proves it owns the key, and both derive the same secret keys. After that
**records** carry your data, encrypted and tagged.

Three consequences are worth knowing before you write a line:

- **A name is half of it.** Encryption to an unverified peer is encryption to whoever answered.
  The client must know which name it meant to reach, and the certificate must be for that name.
- **A clean close matters.** If the connection just stops, TLS cannot tell "the peer finished"
  from "someone cut the wire to hide the rest". So closing is a message, and a connection that
  ends without it is reported as an error, not as an end of data.
- **It costs.** A handshake is public-key arithmetic. Section 9 has this implementation's
  numbers, and they are not small.

## 2. A configuration

```c
proven_err_t proven_tls_config_create(const proven_tls_options_t *options, proven_tls_config_t **out);
void proven_tls_config_destroy(proven_tls_config_t *config);
```

A `proven_tls_config_t` holds everything that is the same for every connection: whom to
believe, who you are, which application protocols you speak. **It is built once and never
changes**, so one config is shared by every connection and every thread.
`proven_tls_config_create` copies what it is given - except the anchor store, which must
outlive the config - and `proven_tls_config_destroy` wipes the private key as it frees it.

Zero-initialise a `proven_tls_options_t`, set `alloc`, and set what applies:

| Field | For | Meaning |
|---|---|---|
| `alloc` | both | Required. Connections made from this config allocate through it too |
| `anchors` | whoever verifies | The trust anchors (Chapter 13). A client needs them; a server needs them only to check client certificates |
| `verify` | whoever verifies | `PROVEN_TLS_VERIFY_CHAIN` (the default): a chain to the anchors, valid now, for the expected name. `PROVEN_TLS_VERIFY_PIN_ONLY`: section 7 |
| `pins`, `pin_count` | whoever verifies | Public-key pins (section 7) |
| `certificate_pem`, `private_key_pem` | a server; a client with a certificate | This side's certificates, its own first, and its key. The key is P-256, Ed25519, or RSA of 2048 to 4096 bits, as a PKCS #8 block (labelled `PRIVATE KEY`), an `EC PRIVATE KEY` block or an `RSA PRIVATE KEY` block, not encrypted |
| `client_auth` | a server | Whether to ask clients for a certificate (section 7) |
| `alpn`, `alpn_count` | both | Application protocol names, most preferred first (`http/1.1`). The server picks its first choice among what the client offers; if both sides have a list and nothing is common, the handshake fails |
| `random`, `now` | both | Where unpredictable bytes and the time come from. Leave them null on a hosted system: the operating system's source and the wall clock are used. A freestanding build must supply both |
| `no_resumption`, `ticket_lifetime_s` | both, a server | Section 6 |
| `keep_peer_certificate` | whoever verifies | Keep the peer's certificate after the handshake, for `proven_tls_peer_certificate` |
| `max_handshake_bytes` | both | The largest handshake message accepted. Default 65,536 |
| `min_version`, `max_version` | both | The oldest and newest protocol version this side agrees to: `PROVEN_TLS_VERSION_1_2` or `PROVEN_TLS_VERSION_1_3`. Zero is the default - 1.2 for the minimum, 1.3 for the maximum. **`min_version = PROVEN_TLS_VERSION_1_3` is how to refuse TLS 1.2** (section 9). `PROVEN_TLS_VERSION_1_1` and `PROVEN_TLS_VERSION_1_0` are legacy (section 10) |
| `legacy` | both | `PROVEN_TLS_LEGACY_*` bits: parts of TLS that are left out unless named here. Zero, the default, is none of them. **Read section 10 before setting one** |

**`anchors`, `verify` and `pins` describe how the peer is verified, in either role.** There is
no setting that verifies nothing. A client that would check a chain and is given no anchors is
refused when the config is made; a test that wants to skip the public authorities makes its own
root, or pins a key.

**Everything that can be wrong with the options is an error here**, not at the first
handshake, which is at three in the morning on somebody else's machine:

| Returns | When |
|---|---|
| `PROVEN_ERR_INVALID_ARG` | No allocator; anchors missing where this side verifies a chain; `PIN_ONLY` with no pins; a certificate without a key, or a key without a certificate; more than 8 certificates; an empty ALPN name; a version that does not exist, or a minimum above the maximum; a minimum below TLS 1.2 without `PROVEN_TLS_LEGACY_CBC`; a `legacy` bit that does not exist |
| `PROVEN_ERR_INVALID_FORMAT` | PEM that does not parse, or holds no certificate, or no key; an RSA key outside 2048 to 4096 bits, or whose parts do not belong together, or that does not sign |
| `PROVEN_ERR_UNSUPPORTED` | A key of another kind - P-384, for one - or an encrypted key file |
| `PROVEN_ERR_INVALID_STATE` | The key is not the one in the certificate |
| `PROVEN_ERR_NOMEM` | The allocator refused |

**Which key to choose.** All three kinds work on either side of a connection. They do not
cost the same: with an RSA key, every full handshake this side signs takes several times
longer than with P-256 or Ed25519 (section 9 has the figures), and on a server that is the
largest part of a handshake. Use RSA where a certificate you already have requires it; for a
new one, P-256 or Ed25519. An RSA key is tried when the configuration is made - one signature,
checked - so a key file that is damaged is refused there and not at the first connection.

**An identity without an authority.**

```c
proven_err_t proven_tls_self_signed(const proven_u8str_view_t *names, proven_size_t name_count,
                                    proven_i64 not_before, proven_i64 not_after,
                                    proven_tls_random_fn random, void *random_ctx,
                                    proven_mem_mut_t certificate_pem, proven_size_t *certificate_len,
                                    proven_mem_mut_t private_key_pem, proven_size_t *private_key_len);
```

`proven_tls_self_signed` makes an Ed25519 key and a certificate signed with that same key, both
as PEM: the certificate names `names` (DNS names or IP literals), is valid between the two
times, and may be used by a server or a client. 1,024 and 256 bytes hold them for a few names;
`PROVEN_ERR_OUT_OF_BOUNDS` says a buffer was too small.

**Nobody vouches for a self-signed certificate.** A peer believes it only by already having it:
put the certificate in the peer's anchor store, or give the peer the key's pin and
`PROVEN_TLS_VERIFY_PIN_ONLY` (section 7). That is the right arrangement between machines you
run yourself, and for development and tests; it is not a way to be reached by the public, whose
browsers trust authorities and nothing else. The key is yours to store safely and to wipe.

## 3. HTTPS and wss: the two lines

For HTTP nothing changes but the configuration.

**A client** gets `https` URLs by being given the wrap function this library provides and a
config as its context:

```c
proven_err_t proven_tls_http_wrap(void *config, proven_transport_t plain, proven_u8str_view_t host,
                                  proven_net_deadline_t until, proven_transport_t *out);
```

Set `tls_wrap = proven_tls_http_wrap` and `tls_ctx` to your `proven_tls_config_t *` in the
`proven_http_client_config_t` of Chapter 11. From then on a request for an `https` URL
connects, runs the handshake for the URL's host - through a proxy tunnel if there is a proxy -
and proceeds; a `wss` URL given to `proven_ws_conn_connect` does the same. For the public
internet the config's anchors are the system's (`proven_cert_store_add_system`). A server that
cannot be verified is `PROVEN_ERR_UNTRUSTED`, `PROVEN_ERR_EXPIRED`, `PROVEN_ERR_NOT_YET_VALID`
or `PROVEN_ERR_NAME_MISMATCH` from the request, **and nothing of the request was sent**.

**A server** speaks TLS when its `proven_http_server_config_t` has `tls` set to a config with a
certificate. Every connection it accepts is then TLS; a client that sends plain HTTP to that
port is dropped. Handlers do not change: by the time one runs, the request is plain text, and
what it writes is encrypted on the way out. A WebSocket accepted from such a handler is `wss`.

The handshake is driven by the server's own loop as the client's bytes arrive. It never waits
on one connection, so a client that connects and says nothing costs a slot until
`head_timeout_ms` - which a new connection's handshake shares with its first request's head -
and holds up nobody.

To serve both `http` and `https`, run two servers; a server is one or the other.

Compiled and run by the test suite:

<!-- example: manual/examples/en/ex_14_tls_https.c -->
```c
#include <stdio.h>
#include <string.h>

/*
 * HTTPS, both ends, in one program: the HTTP server of chapter 11 with a TLS config, and the
 * HTTP client of chapter 11 with the TLS wrap. Nothing else about either changes.
 *
 * The server is on another thread on the loopback interface, so the example needs no network.
 */

/* There is no certificate in this file. The server's identity is made when the program runs:
 * a self-signed certificate for 127.0.0.1 and its key, valid for a day around now. The client
 * is given that certificate as the one thing it believes. */

typedef struct {
    proven_http_server_t *server;
} app_t;

static void handle(void *ctx, proven_http_exchange_t *x) {
    app_t *app = ctx;
    const proven_http_request_t *req = proven_http_exchange_request(x);
    /* The handler is the same as without TLS: by the time it runs, the request is plain text. */
    if (proven_u8str_view_eq(req->target, PROVEN_LIT("/quit"))) {
        (void)proven_http_exchange_respond(x, 200, NULL, 0, proven_mem_view_from_u8(PROVEN_LIT("bye\n")));
        proven_http_server_stop(app->server);
        return;
    }
    (void)proven_http_exchange_respond(x, 200, NULL, 0, proven_mem_view_from_u8(PROVEN_LIT("hello over TLS\n")));
}

static void serve(void *arg) {
    (void)proven_http_server_run(((app_t *)arg)->server);
}

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();

    // ---- an identity ---------------------------------------------------------------
    /* A self-signed certificate for the address this example serves on. A real server would be
     * given a certificate from an authority; a pair of machines you run yourself can use this
     * and a pin (chapter 14, section 7). */
    proven_byte_t cert_pem[1024], key_pem[256];
    proven_size_t cert_len = 0, key_len = 0;
    proven_u8str_view_t names[1] = { PROVEN_LIT("127.0.0.1") };
    proven_i64 now = proven_time_now() / 1000000000;
    EXAMPLE_REQUIRE(proven_tls_self_signed(names, 1, now - 3600, now + 86400, NULL, NULL,
                                           (proven_mem_mut_t){ cert_pem, sizeof cert_pem }, &cert_len,
                                           (proven_mem_mut_t){ key_pem, sizeof key_pem }, &key_len) == PROVEN_OK, "a self-signed certificate and its key");

    // ---- the server ----------------------------------------------------------------
    /* One field makes a server speak TLS: its config's `tls`. The TLS config holds the
     * certificate the server presents and the key that proves it is the server's. */
    proven_tls_options_t server_options = {
        .alloc = heap,
        .certificate_pem = { cert_pem, cert_len },
        .private_key_pem = { key_pem, key_len },
    };
    proven_tls_config_t *server_tls = NULL;
    EXAMPLE_REQUIRE(proven_tls_config_create(&server_options, &server_tls) == PROVEN_OK, "the server's TLS configuration");

    app_t app = { 0 };
    proven_http_server_config_t server_config = { .alloc = heap, .handler = handle, .handler_ctx = &app, .tls = server_tls };
    proven_net_addr_t at;
    EXAMPLE_REQUIRE(proven_http_server_create(&server_config, &app.server) == PROVEN_OK &&
                    proven_http_server_listen(app.server, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &at) == PROVEN_OK, "an HTTPS server on a free port");
    proven_job_sys_t *threads = NULL;
    proven_job_group_t running;
    proven_job_group_init(&running);
    EXAMPLE_REQUIRE(proven_job_system_init(heap, 1, 4, &threads) == PROVEN_OK &&
                    proven_job_group_submit(threads, &running, serve, &app) == PROVEN_OK, "its loop runs on another thread");

    // ---- the client ----------------------------------------------------------------
    /* The client's TLS config says whom to believe: here, the certificate just made. For the
     * public internet it would be proven_cert_store_add_system instead. */
    proven_cert_store_t *anchors = NULL;
    EXAMPLE_REQUIRE(proven_cert_store_create(heap, &anchors) == PROVEN_OK &&
                    proven_cert_store_add_pem(anchors, (proven_mem_view_t){ cert_pem, cert_len }, NULL) == PROVEN_OK,
                    "the anchors");
    proven_tls_options_t client_options = { .alloc = heap, .anchors = anchors };
    proven_tls_config_t *client_tls = NULL;
    EXAMPLE_REQUIRE(proven_tls_config_create(&client_options, &client_tls) == PROVEN_OK, "the client's TLS configuration");

    /* Two fields make a client speak HTTPS: the wrap function this library provides, and the
     * TLS config as its context. */
    proven_http_client_config_t client_config = { .alloc = heap, .max_idle_connections = 1, .tls_wrap = proven_tls_http_wrap, .tls_ctx = client_tls };
    proven_http_client_t *client = NULL;
    EXAMPLE_REQUIRE(proven_http_client_create(&client_config, &client) == PROVEN_OK, "an HTTP client");

    char url_text[64];
    int url_len = snprintf(url_text, sizeof url_text, "https://127.0.0.1:%u/", (unsigned)at.port);
    proven_u8str_view_t url = { (const proven_byte_t *)url_text, (proven_size_t)url_len };

    proven_http_client_response_t response;
    proven_u8str_t body = { 0 };
    EXAMPLE_REQUIRE(proven_http_client_get(client, url, &response) == PROVEN_OK && response.status == 200, "GET over https: 200");
    EXAMPLE_REQUIRE(proven_http_client_read_all(&response, heap, &body, 4096) == PROVEN_OK &&
                    proven_u8str_view_eq(proven_u8str_as_view(&body), PROVEN_LIT("hello over TLS\n")), "and the body, which crossed the wire encrypted");
    proven_u8str_destroy(heap, &body);
    proven_http_client_finish(&response);

    // ---- a client with nothing to believe ------------------------------------------
    /* There is no setting that verifies nothing. A client that would check a chain must be given
     * anchors, and one given an empty store believes no server at all. */
    proven_tls_options_t stranger_options = { .alloc = heap, .anchors = NULL };
    proven_tls_config_t *no_anchors = NULL;
    EXAMPLE_REQUIRE(proven_tls_config_create(&stranger_options, &no_anchors) == PROVEN_ERR_INVALID_ARG, "no anchors at all is refused when the config is made");

    proven_cert_store_t *empty = NULL;
    EXAMPLE_REQUIRE(proven_cert_store_create(heap, &empty) == PROVEN_OK, "an empty store");
    stranger_options.anchors = empty;
    EXAMPLE_REQUIRE(proven_tls_config_create(&stranger_options, &no_anchors) == PROVEN_OK, "a config that trusts nobody");
    proven_http_client_config_t wary_config = { .alloc = heap, .tls_wrap = proven_tls_http_wrap, .tls_ctx = no_anchors };
    proven_http_client_t *wary = NULL;
    EXAMPLE_REQUIRE(proven_http_client_create(&wary_config, &wary) == PROVEN_OK, "an HTTP client");
    EXAMPLE_REQUIRE(proven_http_client_get(wary, url, &response) == PROVEN_ERR_UNTRUSTED, "the same server is PROVEN_ERR_UNTRUSTED, and no request was sent to it");
    proven_http_client_finish(&response);
    proven_http_client_destroy(wary);

    // ---- done ----------------------------------------------------------------------
    url_len = snprintf(url_text, sizeof url_text, "https://127.0.0.1:%u/quit", (unsigned)at.port);
    EXAMPLE_REQUIRE(proven_http_client_get(client, (proven_u8str_view_t){ (const proven_byte_t *)url_text, (proven_size_t)url_len }, &response) == PROVEN_OK, "the server is asked to stop");
    proven_http_client_finish(&response);
    proven_job_group_wait(threads, &running);

    proven_http_client_destroy(client);
    proven_http_server_destroy(app.server);
    proven_job_system_close(threads);
    proven_job_system_destroy(threads);
    proven_tls_config_destroy(client_tls);
    proven_tls_config_destroy(no_anchors);
    proven_tls_config_destroy(server_tls);
    proven_cert_store_destroy(anchors);
    proven_cert_store_destroy(empty);
    proven_mem_wipe((proven_mem_mut_t){ key_pem, sizeof key_pem });
    return EXAMPLE_OK();
}
```

## 4. Any connection: the transport wrapper

```c
proven_err_t proven_tls_transport_client(proven_transport_t plain, const proven_tls_config_t *config,
                                         proven_u8str_view_t server_name, proven_tls_session_t *session,
                                         proven_net_deadline_t until, proven_transport_t *out);
proven_err_t proven_tls_transport_server(proven_transport_t plain, const proven_tls_config_t *config,
                                         proven_net_deadline_t until, proven_transport_t *out);
proven_tls_conn_t *proven_tls_transport_conn(proven_transport_t tls);
```

A `proven_transport_t` (Chapter 9) is a connection as an interface: read, write, shut down,
close. `proven_tls_transport_client` and `proven_tls_transport_server` take one, run the
handshake over it within `until`, and return another - which is what the HTTP wrap in section 3
is made of, and what you use for a protocol of your own.

- **On success the new transport owns the old one.** Closing it says a TLS goodbye and closes
  the connection underneath. Do not use `plain` again.
- **On failure `plain` is untouched and still yours** to close.
- `server_name` is what the client meant to reach: it is sent to the server and checked against
  its certificate.
- A read that returns `PROVEN_ERR_EOF` means the peer closed properly. A connection that ends
  any other way is `PROVEN_ERR_RESET`: do not treat what you have as complete.
- `proven_tls_transport_conn` gives the engine inside, for the questions of section 5 (which
  suite, which protocol, whose key). It returns null for a transport that is not one of these.

Reads and writes take deadlines as every transport's do. A read may have to write (the protocol
occasionally owes the peer a message) and a write never has to read.

## 5. The engine

Under the wrapper is a state machine with no I/O in it. You give it the bytes that arrived; it
gives you the bytes to send. Use it directly when the transport is yours - an event loop, a
serial line, a memory buffer in a test - or when there is no operating system at all.

```c
proven_err_t proven_tls_client_create(const proven_tls_config_t *config, proven_u8str_view_t server_name,
                                      proven_tls_session_t *session, proven_tls_conn_t **out);
proven_err_t proven_tls_server_create(const proven_tls_config_t *config, proven_tls_conn_t **out);
void proven_tls_conn_destroy(proven_tls_conn_t *conn);

proven_err_t proven_tls_feed(proven_tls_conn_t *conn, proven_mem_view_t in, proven_size_t *consumed);
proven_mem_view_t proven_tls_pending_output(proven_tls_conn_t *conn);
void proven_tls_output_sent(proven_tls_conn_t *conn, proven_size_t n);

bool proven_tls_is_established(const proven_tls_conn_t *conn);
proven_result_size_t proven_tls_read(proven_tls_conn_t *conn, proven_mem_mut_t dest);
proven_result_size_t proven_tls_write(proven_tls_conn_t *conn, proven_mem_view_t src);
proven_err_t proven_tls_close(proven_tls_conn_t *conn);
```

The whole of using it is one loop:

1. **Whatever `proven_tls_pending_output` shows, send**, and tell it how much went with
   `proven_tls_output_sent`. A client has output the moment it is created.
2. **Whatever arrives, give to `proven_tls_feed`.** It need not be a whole record or only one;
   incomplete records are kept inside.
3. **After every feed, call `proven_tls_read` until it says `PROVEN_ERR_NEED_MORE`.**
4. Once `proven_tls_is_established`, `proven_tls_write` encrypts your data into the pending
   output. Go to 1.

Two rules in that loop are easy to get wrong:

- **`proven_tls_feed` may consume less than you gave it.** When it has decrypted application
  data that you have not read yet, it stops and sets `*consumed` to what it took. Read, then
  feed the rest. A caller that discards the unconsumed part loses data.
- **The view from `proven_tls_pending_output` is good only until the next call** on that
  connection. Send from it or copy it; do not hold it across a feed.

`proven_tls_read` returns `PROVEN_ERR_EOF` once the peer has closed properly and everything
before the close was read. `proven_tls_close` puts this side's goodbye in the pending output;
after it nothing more can be written, and reading continues until the peer closes too.
`proven_tls_write` takes everything unless more than 64 KiB of output is already waiting to be
sent, in which case it takes what fits - possibly nothing - and you send some first.

A connection is used by one thread at a time. A config, and the anchor store under it, may be
used by any number.

What was agreed, once established:

```c
proven_u16 proven_tls_version(const proven_tls_conn_t *conn);
proven_u16 proven_tls_cipher_suite(const proven_tls_conn_t *conn);
proven_u8str_view_t proven_tls_alpn(const proven_tls_conn_t *conn);
bool proven_tls_resumed(const proven_tls_conn_t *conn);
bool proven_tls_peer_key_sha256(const proven_tls_conn_t *conn, proven_byte_t out[32]);
proven_mem_view_t proven_tls_peer_certificate(const proven_tls_conn_t *conn);
proven_u8str_view_t proven_tls_server_name(const proven_tls_conn_t *conn);
proven_err_t proven_tls_key_update(proven_tls_conn_t *conn);
```

- `proven_tls_version` is `PROVEN_TLS_VERSION_1_3` or `PROVEN_TLS_VERSION_1_2` (or one of the
  two legacy versions, for a configuration that allowed them), and zero until
  the peer's hello has settled it. A program that must not carry something over TLS 1.2 checks
  here, or - better - sets `min_version` and never gets that far.
- `proven_tls_cipher_suite` is the suite's number (section 9); `proven_tls_alpn` the agreed
  application protocol, empty when none was.
- `proven_tls_peer_key_sha256` gives the hash of the peer's public key - the value a pin holds
  - and returns `false` when the peer presented no certificate. The certificate itself is not
  kept after the handshake unless the config set `keep_peer_certificate`, in which case
  `proven_tls_peer_certificate` returns it for `proven_cert_parse`.
- `proven_tls_server_name` is, in the server role and during the handshake only, the name the
  client asked for.
- `proven_tls_key_update` replaces this side's sending key with the next one. The engine does
  this by itself long before a key has protected too much; call it when policy wants it sooner.
  TLS 1.2 has no such message: there it returns `PROVEN_ERR_UNSUPPORTED`, and a connection
  that has sent 2^31 records refuses to write more (`PROVEN_ERR_OVERFLOW`) - open another.

Compiled and run by the test suite:

<!-- example: manual/examples/en/ex_14_tls_engine.c -->
```c
#include <string.h>

/*
 * The TLS engine with no network in sight: a client and a server connection in one program,
 * with this file carrying the bytes from one to the other. That is all a network does for them.
 *
 * It is the layer under the transport wrapper. Use it directly when the transport is yours:
 * an event loop, a serial line, a test.
 */

/* There is no certificate in this file. The server's identity is made when the program runs:
 * a self-signed certificate for example.test and its key. A certificate nobody vouches for is
 * believed only by a peer that already holds it, so the client is given it as its anchor. */

/* The engine reads no clock. A program gives it one; this one always says 2027-06-01, so the
 * example behaves the same on any day. */
static proven_i64 fixed_now(void *ctx) { (void)ctx; return 1811808000; }

/* Hand everything one side wants sent to the other. The receiver may take less than it was
 * given when it has application data waiting to be read; what it did not take stays pending. */
static proven_err_t carry(proven_tls_conn_t *from, proven_tls_conn_t *to) {
    proven_mem_view_t out = proven_tls_pending_output(from);
    proven_size_t at = 0;
    while (at < out.size) {
        proven_size_t used = 0;
        proven_err_t e = proven_tls_feed(to, (proven_mem_view_t){ out.ptr + at, out.size - at }, &used);
        if (e != PROVEN_OK) return e;
        if (used == 0) break;                 /* it wants its application data read first */
        at += used;
    }
    proven_tls_output_sent(from, at);
    return PROVEN_OK;
}

/* True when the bytes of `text` appear, in order, somewhere in `data`. */
static bool appears(proven_mem_view_t data, const char *text) {
    proven_size_t n = strlen(text);
    for (proven_size_t i = 0; i + n <= data.size; ++i) {
        if (memcmp(data.ptr + i, text, n) == 0) return true;
    }
    return false;
}

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();

    // ---- an identity ---------------------------------------------------------------
    /* proven_tls_self_signed makes an Ed25519 key and a certificate that names what you ask,
     * valid between two times. Here: from the day before this example's fixed clock to a year
     * after. A real server would be given a certificate from an authority instead. */
    proven_byte_t cert_pem[1024], key_pem[256];
    proven_size_t cert_len = 0, key_len = 0;
    proven_u8str_view_t names[1] = { PROVEN_LIT("example.test") };
    EXAMPLE_REQUIRE(proven_tls_self_signed(names, 1, 1811808000 - 86400, 1811808000 + 365 * 86400, NULL, NULL,
                                           (proven_mem_mut_t){ cert_pem, sizeof cert_pem }, &cert_len,
                                           (proven_mem_mut_t){ key_pem, sizeof key_pem }, &key_len) == PROVEN_OK, "a self-signed certificate and its key");

    // ---- two configurations ------------------------------------------------------
    /* A config says whom to believe and who you are. It is built once and shared by every
     * connection. The client believes the certificate just made; the server is that
     * certificate with its key. */
    proven_cert_store_t *anchors = NULL;
    EXAMPLE_REQUIRE(proven_cert_store_create(heap, &anchors) == PROVEN_OK &&
                    proven_cert_store_add_pem(anchors, (proven_mem_view_t){ cert_pem, cert_len }, NULL) == PROVEN_OK,
                    "the anchors");

    proven_u8str_view_t protocols[1] = { PROVEN_LIT("example/1") };
    proven_tls_options_t client_options = { .alloc = heap, .anchors = anchors, .now = fixed_now, .alpn = protocols, .alpn_count = 1 };
    proven_tls_options_t server_options = {
        .alloc = heap, .now = fixed_now, .alpn = protocols, .alpn_count = 1,
        .certificate_pem = { cert_pem, cert_len },
        .private_key_pem = { key_pem, key_len },
    };
    proven_tls_config_t *client_config = NULL, *server_config = NULL;
    EXAMPLE_REQUIRE(proven_tls_config_create(&client_options, &client_config) == PROVEN_OK, "a client configuration");
    EXAMPLE_REQUIRE(proven_tls_config_create(&server_options, &server_config) == PROVEN_OK, "a server configuration: its key matches its certificate");

    // ---- the handshake ------------------------------------------------------------
    /* A client connection has its first message ready the moment it exists. A server
     * connection waits. The session is where a ticket for next time will be put. */
    proven_tls_session_t session = { 0 };
    proven_tls_conn_t *client = NULL, *server = NULL;
    EXAMPLE_REQUIRE(proven_tls_client_create(client_config, PROVEN_LIT("example.test"), &session, &client) == PROVEN_OK, "a client");
    EXAMPLE_REQUIRE(proven_tls_server_create(server_config, &server) == PROVEN_OK, "a server");
    EXAMPLE_REQUIRE(proven_tls_pending_output(client).size > 0 && proven_tls_pending_output(server).size == 0, "the client speaks first");

    /* Carry bytes back and forth until both are done. Two round trips do it. */
    for (int round = 0; round < 10 && !(proven_tls_is_established(client) && proven_tls_is_established(server)); ++round) {
        EXAMPLE_REQUIRE(carry(client, server) == PROVEN_OK, "client to server");
        EXAMPLE_REQUIRE(carry(server, client) == PROVEN_OK, "server to client");
    }
    EXAMPLE_REQUIRE(proven_tls_is_established(client) && proven_tls_is_established(server), "both sides are established");
    EXAMPLE_REQUIRE(proven_tls_version(client) == PROVEN_TLS_VERSION_1_3 && proven_tls_version(server) == PROVEN_TLS_VERSION_1_3, "both spoke TLS 1.3, the newest version both sides have");
    EXAMPLE_REQUIRE(proven_tls_cipher_suite(client) == proven_tls_cipher_suite(server) && proven_tls_cipher_suite(client) != 0, "they agreed on a cipher suite");
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_tls_alpn(client), PROVEN_LIT("example/1")), "and on the application protocol");
    EXAMPLE_REQUIRE(!proven_tls_resumed(client), "this was a full handshake");

    // ---- application data ----------------------------------------------------------
    /* Writing encrypts into the pending output; reading gives what feeding decrypted. */
    proven_result_size_t wrote = proven_tls_write(client, proven_mem_view_from_u8(PROVEN_LIT("hello, server")));
    EXAMPLE_REQUIRE(wrote.err == PROVEN_OK && wrote.value == 13, "thirteen bytes taken");
    proven_mem_view_t wire = proven_tls_pending_output(client);
    EXAMPLE_REQUIRE(wire.size > 13 && !appears(wire, "hello, server"), "what goes on the wire is a record, and not the text");
    EXAMPLE_REQUIRE(carry(client, server) == PROVEN_OK, "client to server");
    proven_byte_t buf[64];
    proven_result_size_t got = proven_tls_read(server, (proven_mem_mut_t){ buf, sizeof buf });
    EXAMPLE_REQUIRE(got.err == PROVEN_OK && got.value == 13 && memcmp(buf, "hello, server", 13) == 0, "the server reads what the client wrote");
    EXAMPLE_REQUIRE(proven_tls_read(server, (proven_mem_mut_t){ buf, sizeof buf }).err == PROVEN_ERR_NEED_MORE, "and then there is nothing more yet");

    // ---- closing --------------------------------------------------------------------
    /* A close is a message too: it tells the peer that the end is the end and not a cut wire. */
    EXAMPLE_REQUIRE(proven_tls_close(client) == PROVEN_OK && carry(client, server) == PROVEN_OK, "the close travels");
    EXAMPLE_REQUIRE(proven_tls_read(server, (proven_mem_mut_t){ buf, sizeof buf }).err == PROVEN_ERR_EOF, "and the server's read reports a proper end");
    proven_tls_conn_destroy(client);
    proven_tls_conn_destroy(server);

    // ---- resumption -----------------------------------------------------------------
    /* The first connection left a ticket in the session. A second connection that is given the
     * same session offers it, and the handshake skips the certificates and one signature. */
    EXAMPLE_REQUIRE(proven_tls_client_create(client_config, PROVEN_LIT("example.test"), &session, &client) == PROVEN_OK &&
                    proven_tls_server_create(server_config, &server) == PROVEN_OK, "a second pair");
    for (int round = 0; round < 10 && !(proven_tls_is_established(client) && proven_tls_is_established(server)); ++round) {
        EXAMPLE_REQUIRE(carry(client, server) == PROVEN_OK && carry(server, client) == PROVEN_OK, "client to server");
    }
    EXAMPLE_REQUIRE(proven_tls_resumed(client) && proven_tls_resumed(server), "both sides resumed");
    proven_tls_conn_destroy(client);
    proven_tls_conn_destroy(server);

    // ---- a server that is not the one asked for -------------------------------------
    /* The same server, reached under a name its certificate does not carry. The client refuses,
     * and says precisely why. */
    proven_tls_conn_t *wrong = NULL;
    EXAMPLE_REQUIRE(proven_tls_client_create(client_config, PROVEN_LIT("elsewhere.test"), NULL, &wrong) == PROVEN_OK &&
                    proven_tls_server_create(server_config, &server) == PROVEN_OK, "a second pair");
    EXAMPLE_REQUIRE(carry(wrong, server) == PROVEN_OK, "client to server");
    EXAMPLE_REQUIRE(carry(server, wrong) == PROVEN_ERR_NAME_MISMATCH, "the client's feed returns PROVEN_ERR_NAME_MISMATCH");
    EXAMPLE_REQUIRE(proven_tls_peer_fault(wrong) == PROVEN_CERT_FAULT_NAME_MISMATCH && !proven_tls_is_established(wrong), "with the precise fault, and no connection");
    /* A failure leaves an alert in the pending output. Send it: it is how the peer learns. */
    EXAMPLE_REQUIRE(proven_tls_pending_output(wrong).size > 0 && carry(wrong, server) == PROVEN_ERR_PROTOCOL, "the server is told");
    EXAMPLE_REQUIRE(proven_tls_alert_received(server) == 42, "bad_certificate, by its number");
    proven_tls_conn_destroy(wrong);
    proven_tls_conn_destroy(server);

    proven_mem_wipe((proven_mem_mut_t){ session.opaque, sizeof session.opaque });
    proven_mem_wipe((proven_mem_mut_t){ key_pem, sizeof key_pem });
    proven_tls_config_destroy(client_config);
    proven_tls_config_destroy(server_config);
    proven_cert_store_destroy(anchors);
    return EXAMPLE_OK();
}
```

## 6. Resumption

A full handshake costs two signatures and a certificate chain. A client that has talked to a
server before can skip those: the server gave it a **ticket** at the end of the first
handshake, and a second connection that presents the ticket proves with it that both sides
already share a secret. A fresh key exchange still happens, so the new connection's keys are
its own.

On the client it is one struct:

```c
typedef struct { proven_byte_t opaque[PROVEN_TLS_SESSION_SIZE]; } proven_tls_session_t;
```

Zero-initialise a `proven_tls_session_t`, and pass it to `proven_tls_client_create` (or to
`proven_tls_transport_client`) for each connection to the same server name. The connection
writes a ticket into it when the server sends one, and the next connection offers it;
`proven_tls_resumed` says whether the server accepted. It is a plain block of bytes with no
pointers in it - keep it wherever you like, for as long as you like - and **it holds a
secret**: wipe it with `proven_mem_wipe` when you are done. It is offered only to the name it
was made for, and never past the lifetime the server gave it. One session serves one connection
at a time.

On the server there is nothing to do. Tickets are issued unless `no_resumption` is set, and
honoured for `ticket_lifetime_s` (two hours by default, a week at most). **The server stores
nothing per client**: a ticket is the session's state sealed under a key that only this
process holds. That key is made when the config is, replaced every ticket lifetime, kept one
lifetime longer so that outstanding tickets still open, and never written anywhere - so
tickets do not survive a restart, and two processes do not accept each other's.

What resumption does not do here:

- **No early data** (0-RTT). Data sent before the handshake completes can be replayed by an
  attacker; using it safely requires an application written for it, and this library does not
  offer it.
- **Not through `proven_tls_http_wrap`.** The HTTP client's connections do not resume; it
  reuses open connections instead, which is cheaper still.
- A resumed connection's peer is the one verified when the session was made: the certificate is
  not sent again, so it is not checked again, and an expiry that has passed since is not
  noticed. The ticket lifetime bounds how long that can matter.

**Under TLS 1.2 a resumed connection is weaker than a new one, and you should know how.**
There is no fresh key exchange: the new connection's keys come from the first connection's
master secret and two new random values. So whoever later obtains the server's ticket key can
read every connection resumed from a ticket it sealed, and the first one's secret with it -
which a full handshake, and TLS 1.3's resumption, do not allow. The ticket key here lives only
in the process's memory and is replaced every ticket lifetime, which bounds the exposure to
that; `no_resumption` removes it. A session made under one version is never offered to, or
accepted by, the other: one `proven_tls_session_t` holds whichever kind the last connection
left, and a handshake that ends up in the other version is simply a full one.

## 7. Client certificates and pins

**Client certificates.** A server can require that the client proves who it is, too - mutual
TLS, the usual choice between services. On the server set `client_auth` and give `anchors`:

| `client_auth` | A client with no certificate | A client with one that does not verify |
|---|---|---|
| `PROVEN_TLS_CLIENT_AUTH_NONE` | (not asked) | (not asked) |
| `PROVEN_TLS_CLIENT_AUTH_REQUEST` | Let in. `proven_tls_peer_key_sha256` returns `false` | Refused |
| `PROVEN_TLS_CLIENT_AUTH_REQUIRE` | Refused | Refused |

On the client set `certificate_pem` and `private_key_pem`; it presents them when asked and not
otherwise. The server learns *that* the client holds a key some authority vouched for. *Which*
client it is is yours to decide - from `proven_tls_peer_key_sha256`, or from the certificate
(`keep_peer_certificate`) and its names - and `PROVEN_TLS_CLIENT_AUTH_REQUEST` leaves even the
"whether" to you: check before acting on a request.

With an HTTP server the engine is inside the server, and a handler has no call to reach it in
this version: use `REQUIRE` there, so that every request a handler sees came from a verified
client.

**Pins.** A pin is the SHA-256 of a public key (Chapter 13, section 7). Give `pins` and the
peer's key must be one of them:

- With `PROVEN_TLS_VERIFY_CHAIN`, **in addition** to the chain. A valid certificate from any
  authority in the world is then not enough; it has to be for the key you expect.
- With `PROVEN_TLS_VERIFY_PIN_ONLY`, **instead** of it. No chain, no dates, no name - the key
  is the identity, and `server_name` may be empty. This is the mode for a peer you run
  yourself with a self-signed certificate, and it means the pin is all of your security.

Ship two pins - the key in use and a spare - or a lost key becomes a fleet that cannot connect.

## 8. When it fails

A connection fails once, with one error, and stays failed.

| Error | What happened | Usual cause |
|---|---|---|
| `PROVEN_ERR_UNTRUSTED` | The peer's certificate does not chain to an anchor, or is not pinned, or a required client certificate was not presented (the peer is told `certificate_required` in TLS 1.3 and `handshake_failure` in 1.2, which has no closer word) | A private or self-signed authority; a missing intermediate; on Windows, a root the machine has not fetched (Chapter 13) |
| `PROVEN_ERR_EXPIRED`, `PROVEN_ERR_NOT_YET_VALID` | A certificate on the path is outside its validity period | An unrenewed certificate - or this machine's clock |
| `PROVEN_ERR_NAME_MISMATCH` | The certificate is valid and is for another name | A wrong URL, a misconfigured server, or an interception |
| `PROVEN_ERR_PROTOCOL` | Everything else the peer did: a version or algorithm this side does not speak, a malformed or unexpected message, a record that does not authenticate - or the peer ended the handshake with an alert of its own | A peer that speaks only TLS 1.1 or older, or only the parts of TLS 1.2 this library leaves out (section 9); a TLS 1.2 peer when `min_version` is 1.3; no cipher suite or group in common; a peer that refused *this* side's certificate; corruption |
| `PROVEN_ERR_RESET` (the wrapper) | The connection ended without a TLS close | The peer crashed or was cut off; do not trust what was received as complete |
| `PROVEN_ERR_TIMEOUT` (the wrapper) | The deadline passed | A peer that stopped answering |

```c
proven_cert_fault_t proven_tls_peer_fault(const proven_tls_conn_t *conn);
int proven_tls_alert_received(const proven_tls_conn_t *conn);
int proven_tls_alert_sent(const proven_tls_conn_t *conn);
```

For the log line: `proven_tls_peer_fault` is the precise reason a certificate was refused
(Chapter 13's list, plus `PROVEN_CERT_FAULT_PIN_MISMATCH`). `proven_tls_alert_received` is the
alert the peer sent, as its number in RFC 8446 section 6 - when the answer is
`PROVEN_ERR_PROTOCOL` and you want to know what the *other* side objected to, this is where it
is (48 `unknown_ca` and 42 `bad_certificate` mean it did not accept your certificate; 40
`handshake_failure` that nothing was in common; 70 `protocol_version` that it wants another
version). `proven_tls_alert_sent` is what this side told the peer. Both are -1 when there was
none.

With the engine, the error is returned once by `proven_tls_feed`; the alert that tells the peer
is then in the pending output - send it - and after that every call but the output pair and the
questions returns `PROVEN_ERR_INVALID_STATE`.

**The errors are deliberately few.** A peer that fails authentication learns only that it
failed. Do not relay `proven_tls_peer_fault` or the details of a `PROVEN_ERR_PROTOCOL` to the
other side of the connection; they are for your log.

## 9. What is negotiated, what is held, what is not here

**Negotiated.** TLS 1.3, and TLS 1.2 cut down to the part of it that is still defensible. When
both sides can speak 1.3, that is what they speak.

In TLS 1.3:

| What | Which |
|---|---|
| Cipher suites | `TLS_AES_128_GCM_SHA256` (0x1301), `TLS_AES_256_GCM_SHA384` (0x1302), `TLS_CHACHA20_POLY1305_SHA256` (0x1303) |
| Their order | AES-128, AES-256, ChaCha20 on a processor with AES instructions (x86-64 with AES-NI, AArch64 with the cryptography extension). **ChaCha20 first everywhere else**: without those instructions this library's AES is constant-time and some thirty times slower than its ChaCha20 |
| Key exchange | X25519; P-256 when the peer insists (one extra round trip) |
| This side's key | ECDSA with P-256, Ed25519, or RSA of 2048 to 4096 bits (signing with RSA-PSS and SHA-256) |
| A peer's key | Those, ECDSA with P-384, and RSA (2048 to 8192 bits, RSA-PSS) |
| Also | ALPN, server name indication, session tickets, key update, `record_size_limit`, the middlebox-compatibility messages |

In TLS 1.2:

| What | Which |
|---|---|
| Cipher suites | `ECDHE-ECDSA` and `ECDHE-RSA`, each with `AES128-GCM-SHA256`, `AES256-GCM-SHA384` and `CHACHA20-POLY1305`: 0xC02B, 0xC02C, 0xCCA9, 0xC02F, 0xC030, 0xCCA8. In the same order of ciphers as above |
| Key exchange | X25519 or P-256, a fresh key pair for every connection - so there is always forward secrecy |
| This side's key | ECDSA with P-256 or Ed25519 for the `ECDHE-ECDSA` suites; RSA for `ECDHE-RSA`, signing with RSA-PSS when the peer offers it and PKCS #1 v1.5 with SHA-256 when it does not |
| A peer's key | The same kinds as in 1.3; its signature must use SHA-256, SHA-384 or SHA-512 |
| **Required of the peer** | The extended master secret (RFC 7627). A peer without it is refused with `handshake_failure` |
| Also | ALPN, server name indication, session tickets (RFC 5077; section 6), client certificates, the downgrade mark of RFC 8446 section 4.1.3 |

**What that leaves out is the point.** Every attack on TLS 1.2 that has a name went through
something this list does not have: the CBC cipher suites (padding oracles), RSA key exchange
(Bleichenbacher's attack, and no forward secrecy), finite-field Diffie-Hellman with weak or
chosen groups, renegotiation, compression, a master secret not bound to the handshake, SHA-1
and MD5 in handshake signatures. A peer that offers only those gets `handshake_failure`, and
your program gets `PROVEN_ERR_PROTOCOL`. Some of them can be switched on, one by one, for a
peer that has nothing else: section 10 says which, and what each one costs.

- **Renegotiation is declined, not answered.** A peer that asks for a new handshake on an
  established connection is sent the warning `no_renegotiation` and the connection carries on
  as it was. What the peer does next is its choice: some go on, some close. One that keeps
  asking is cut off after sixteen requests in a row.
- **A client is not fooled into 1.2.** A server that could have spoken 1.3 and answers 1.2
  marks its random value; a client here that offered 1.3 and sees the mark ends the handshake
  with `illegal_parameter`, because somebody in between removed the better offer.
- **ChangeCipherSpec is accepted in one place per direction** - where the keys change - and
  nowhere else.
- **An Ed25519 key authenticates a client in TLS 1.3 only.** A client whose key is Ed25519
  presents no certificate to a 1.2 server (as if it had none). A server's Ed25519 key works in
  both.

**To speak TLS 1.3 only**, set `min_version = PROVEN_TLS_VERSION_1_3`: that is exactly what
versions of this library before 0.22.0 did. Consider it for a server whose clients you control,
and for anything where the weaker resumption of section 6 is not acceptable. The default
accepts 1.2 because a client that cannot reach a server that speaks nothing newer is not
safer, only disconnected - and a great many servers and devices still speak nothing newer.

**Held per connection.** During the handshake, a few kilobytes that are freed when it
completes. After it, **about one kilobyte** per connection when nothing is in flight: the keys
and the counters. Record buffers exist only while a record is being received, is waiting to be
read, or is waiting to be sent, and are freed when it is done - so ten thousand idle TLS
connections hold about ten megabytes, not the three hundred that two 16 KiB buffers each would
cost. That is this layer's share: the transport wrapper adds a few dozen bytes, and an HTTP
server's own per-connection request buffer is Chapter 11's and is separate. (A TLS 1.2
connection holds the same.)

**What a handshake costs.** On one x86-64 core of the development machine, a full handshake
with X25519 and a P-256 certificate is about 2 ms of processor time on the server and about 5 ms
on a client that verifies the chain: for a server, some 500 new connections a second per core.
The public-key arithmetic here is written to be constant-time and to be read, and is ten to fifty
times slower than a tuned library's. Resumption (section 6) removes the signatures; keeping
connections open removes the handshake.

**With an RSA key the signature dominates.** On the same core one signature takes about 9 ms
with a 2048-bit key, 27 ms with 3072 bits and 55 ms with 4096: a server with a 2048-bit RSA key
completes about a hundred new connections a second per core, against five hundred with P-256.
On the event-driven server of [Chapter 15](manual-15-event-loop.md) that time is spent on the
loop's thread, where nothing else is served meanwhile. The signing is done modulo the two
primes with a constant-time exponentiation, its input blinded, and its result checked with
the public key before it is released; it keeps its working numbers on the stack - about
20 KiB by the compiler's own accounting at `-O2` on x86-64 - which a small target must allow
for. A peer must accept `rsa_pss_rsae_sha256`, as TLS 1.3 requires of every implementation:
it is the one scheme this side signs with there.

**Bounds on a peer.** A handshake message is at most `max_handshake_bytes`; a record at most what the protocol allows, refused on its header. Records that carry nothing - empty ones, repeated compatibility messages - are tolerated sixteen in a row. Key-update requests are answered until 64 KiB of answers are waiting unsent. Past any of these the connection ends with `PROVEN_ERR_PROTOCOL`.

**Not here:**

- **Unless a configuration asks (section 10):** TLS 1.1 and 1.0, CBC cipher suites, RSA and
  finite-field Diffie-Hellman key exchange, a peer without the extended master secret.
- **Not at all:** SSL 3.0 and earlier; RC4, 3DES, export, anonymous and DSS suites; CBC with
  SHA-384; renegotiation; compression; session IDs kept on the server; `record_size_limit` in
  TLS 1.2. A peer that needs one of those fails with `PROVEN_ERR_PROTOCOL`.
- **RSA keys above 4096 bits for this side**, and RSA-PSS keys (a certificate whose key is marked for PSS only).
- **Early data (0-RTT).**
- **Revocation.** No CRL, no OCSP, no stapling. See Chapter 13.
- **Choosing a certificate by the name the client asked for.** One config, one certificate.
- **Encrypted ClientHello, post-handshake client authentication, external pre-shared keys.**
- **Padding of records.** Record lengths reveal data lengths, as with most TLS in use.
- **Sharing tickets between processes**, or keeping them across a restart.

**How this was tested.** The registered tests replay the five example handshakes of RFC 8448
through the key schedule and the record layer, value by value, and two of them through the
client, record by record; run a client and a server against each other in memory for every
suite and key type, every refusal in sections 7 and 8, resumption, and a sweep of single-bit
changes to the handshake; and run HTTPS and `wss` over real sockets. Outside the registered
tests, both roles were run against OpenSSL and the server against GnuTLS, for every suite and
group, with retries, resumption and client certificates; and the client fetched pages from
public servers. For RSA keys on this side: the same two implementations accepted handshakes
signed with keys of 2048, 3072 and 4096 bits that OpenSSL had made; the library's PKCS #1 v1.5
signatures were the same bytes as OpenSSL's for those keys and its PSS signatures were accepted
by it. For TLS 1.2: its key derivation is checked against the two published test vectors
for the PRF and against values computed outside this library (there is no published trace of a
whole 1.2 handshake as there is for 1.3, so for the handshake the other implementations are
the evidence); both roles were run against OpenSSL for all six suites and all three kinds of
key, with tickets, client certificates, PKCS #1 and PSS signatures, a peer without the extended
master secret (refused), a peer offering only what is left out (refused), and a peer asking to
renegotiate (declined); and GnuTLS's client was run against the server. For the legacy set:
its pieces are checked against values computed outside the library and against the published
ones that exist (section 10 lists them); both roles were run against OpenSSL, and the server
against GnuTLS's client, in TLS 1.0, 1.1 and 1.2 with each added key exchange and cipher, with
and without encrypt_then_mac, with client certificates, and against peers without the extended
master secret; and the two places where a wrong step gives a key away - opening a CBC record
and recovering an RSA premaster - were run under the same checker as below, with what is
decrypted marked secret, and reported nothing. The signing path, and
the key derivation and record protection of both versions, were run under a checker that reports any branch or memory index
that depends on a secret - the primes, the private exponents, the blinding values, the shared
key and everything derived from it - and reported none,
at two optimisation levels on one compiler and one processor family. **Not done:** no coverage-guided fuzzing, no protocol-level fuzzing suite
(tlsfuzzer, BoGo), no timing measurement on hardware, no external review. The primitives
underneath are Chapter 13's, with the limits stated there.

## 10. The legacy set: what each setting exposes you to

Everything in this section is **off unless a configuration names it**, and none of it is what
you want. It exists because a program sometimes has to talk to a machine that has nothing
better - an appliance nobody can update, a server on somebody else's network - and the
alternative is no connection or a second TLS library. RFC 8996 retired TLS 1.0 and 1.1 and
RFC 9325 says not to negotiate the rest. If you control both ends, change the other end.

**Each bit is one decision.** Set `legacy` in the options to the ones you mean:

| Bit | What becomes possible | What you give up |
|---|---|---|
| `PROVEN_TLS_LEGACY_CBC` | Cipher suites that encrypt with AES in CBC mode and authenticate with HMAC-SHA1 or HMAC-SHA256. Needed for any version below 1.2, which has nothing else | A construction with a twenty-year record of padding attacks; here it is slow to receive |
| `PROVEN_TLS_LEGACY_RSA_KEY_EXCHANGE` | The client encrypts the session's secret under the server's RSA key instead of both sides agreeing on one | **Forward secrecy**: whoever obtains the server's key later reads every recorded connection. And the key itself is put within reach of a server-side mistake (below) |
| `PROVEN_TLS_LEGACY_DHE` | Key exchange by Diffie-Hellman in a finite field, with an RSA certificate | Nothing in principle; in practice time - it is several times slower than the elliptic-curve exchange - and it exists mostly on peers that are old in other ways too |
| `PROVEN_TLS_LEGACY_NO_EXTENDED_MASTER_SECRET` | A connection to a peer that does not have RFC 7627 goes ahead | The binding of the session's keys to the handshake that made them |

and `min_version` to `PROVEN_TLS_VERSION_1_1` or `PROVEN_TLS_VERSION_1_0` to allow those
versions - which also needs the CBC bit, and is refused without it. Nothing changes for a
configuration that sets none of this: it sends the same hello, gives the same answers and
holds the same memory as before these existed.

**They are a last resort in the handshake as well.** A legacy suite is offered after, and
chosen after, every suite of section 9; an older version is agreed only when the peer has no
newer one. Two sides that both allow everything still end up on TLS 1.3. And the downgrade
checks of section 9 cover the old versions: a server that could have spoken 1.2 and is made to
answer 1.0 says so in a way the client checks.

### CBC suites

`ECDHE-ECDSA-` and `ECDHE-RSA-` with `AES128-SHA256`, `AES128-SHA` and `AES256-SHA`; and, with
the bits below, the same ciphers after `DHE-RSA-` and after plain RSA (there also `AES256-SHA256`).
The suites whose MAC is SHA-1 are the only ones TLS 1.0 and 1.1 have.

The weakness is the order of operations. The sender authenticates the plaintext, pads it,
then encrypts; the receiver must decrypt, strip the padding and only then check the MAC. A
receiver that is a little faster or a little different for one kind of bad record than for
another tells an attacker, one byte at a time, what the plaintext was (Vaudenay 2002, POODLE,
Lucky Thirteen). What this library does about it:

- **It asks for the fix first.** `encrypt_then_mac` (RFC 7366) authenticates the ciphertext
  instead, so nothing is decrypted that was not sent by the peer. It is offered whenever CBC is
  and used whenever the peer agrees. Most peers that still need CBC do not have it.
- **Without it, opening a record does the same work whatever is in it.** Everything after
  decryption - finding the padding, checking it, finding the MAC, computing it over a length
  that depends on the padding, comparing - is done with masks, no branch and no memory index
  depending on a decrypted byte, and the number of hash blocks computed depends only on the
  record's length. Every failure is the one alert, `bad_record_mac`.
- **Decryption is the portable, constant-time AES** in every configuration - there is no
  hardware path for it. A CBC connection receives at about 6 MiB/s on the development machine
  (it sends at about 75). That is the price of this design, and a reason not to use it.
- **In TLS 1.0** each record's IV is the previous record's last block, so an attacker who can
  make you send chosen data knows the IV in advance (BEAST). Application data is therefore
  sent as one byte, then the rest - two records for every write.

### Key exchange by RSA

`AES128-GCM-SHA256` and `AES256-GCM-SHA384`, and with the CBC bit `AES128-SHA256`,
`AES256-SHA256`, `AES128-SHA` and `AES256-SHA`. The server's key must be RSA.

Two things, and the second is worse than it looks:

- **No forward secrecy.** The secret every key is derived from travels encrypted under the
  server's long-term key. A recording of the connection is as safe as that key, for ever.
- **The server becomes a decryption oracle if it makes one mistake.** It decrypts a value the
  client chose. If anything it does afterwards - an alert, a delay - depends on whether that
  value decrypted to something well-formed, an attacker can use it, a few thousand connections
  at a time, to decrypt or *sign* anything with the server's key (Bleichenbacher 1998; ROBOT,
  2017). That is not limited to this key exchange: a signature forged this way is as good in
  TLS 1.3. **Enabling this bit on a server puts its key at that risk for every protocol the
  key is used in.** What is done here: 48 random bytes are drawn before the decryption; the
  decryption is the blinded, checked operation of section 9; the result is examined with
  masks - every byte, in fixed order - and either it or the random bytes become the secret, by
  mask; the handshake then continues identically and fails, if it fails, at the Finished
  message, like any handshake with a wrong key. This path was run under a checker that reports
  any branch or memory index that depends on the decrypted bytes, and it reported none. It has
  not been timed on hardware.

### Finite-field Diffie-Hellman

`DHE-RSA-` with `AES128-GCM-SHA256`, `AES256-GCM-SHA384` and `CHACHA20-POLY1305`, and with the
CBC bit `AES128-SHA256`, `AES256-SHA256`, `AES128-SHA` and `AES256-SHA`.

What went wrong with DHE was the group, which the server chooses and the client cannot
examine in the time a handshake takes: too small (Logjam), not a prime, or built to leak. So
no group is examined here at all:

- **A client accepts a prime only if it is, byte for byte, one of six** - the 2048-, 3072- and
  4096-bit groups of RFC 7919 and of RFC 3526 - with generator 2. Anything else ends the
  handshake with `insufficient_security`: a server with a group of its own making cannot be
  reached, however good the group is.
- **A server offers one**: the 2048-bit group of RFC 7919.
- A value from the peer of 0, 1 or the prime less one is refused. Every connection uses a
  fresh exponent, which is what keeps a known weakness of the key derivation (Raccoon, 2020)
  out of reach.
- It costs about 13 ms of processor time a side on the development machine, on top of the
  signature: a server with a 2048-bit RSA key completes some forty-five such handshakes a
  second per core. And it keeps its working numbers on the stack: about 20 KiB at the deepest
  point of a handshake, by the compiler's own accounting at `-O2` on x86-64 - the same as
  signing with RSA.

### TLS 1.0 and 1.1

`min_version = PROVEN_TLS_VERSION_1_1` or `PROVEN_TLS_VERSION_1_0`, with the CBC bit.

- The handshake's integrity rests on MD5 and SHA-1 together, and signatures are made over
  them: an RSA key signs both hashes, an ECDSA key signs the SHA-1. Neither hash would be
  accepted for this anywhere else in the library.
- There is no AEAD cipher: every suite is CBC with HMAC-SHA1, with everything said above.
- **An Ed25519 key cannot serve these versions** - they have no way to carry its signature. A
  server whose key is Ed25519 refuses a client that speaks nothing newer.
- **No session is kept** from a TLS 1.0 or 1.1 connection: every one is a full handshake.
- Client certificates work, with P-256 and RSA keys.

### A peer without the extended master secret

RFC 7627 (2015) binds a session's keys to the handshake that made them. Without it, somebody
in the middle can arrange for two different connections to share a master secret, and then
pass a client's certificate proof from one to the other (the triple handshake attack). A
default configuration refuses a peer that lacks it. Most peers that speak only TLS 1.0 or 1.1
lack it.

With `PROVEN_TLS_LEGACY_NO_EXTENDED_MASTER_SECRET` such a peer is talked to, and on that
connection the things the attack needs are not done: **no ticket is issued or accepted**, a
client **shows no certificate**, and a server **does not ask for one** - a server that
*requires* client certificates refuses such a client outright. A peer that does have the
extension still uses it; the bit only allows its absence.

### What this costs a program that uses none of it

Eight bytes per connection (a pointer that stays null). An idle TLS 1.0 CBC connection holds
about 1.15 KiB instead of about 1 KiB.

### How this was checked, and what was not

The registered tests check every piece against values computed outside the library - AES-CBC
against SP 800-38A, HMAC against RFC 2202 and RFC 4231, and the rest (the old PRF, CBC records
in every version and both MAC orders, Diffie-Hellman) against an independent implementation -
and run every suite in every version it exists in, both roles, with each refusal described
above. The six Diffie-Hellman primes were compared with two other implementations when they
were added. Outside the registered tests, both roles were run against OpenSSL and the server
against GnuTLS; and opening a CBC record and recovering an RSA premaster were each run under a
checker with the decrypted bytes marked secret, together with a deliberately wrong version of
each that the checker must catch (it did).

**Not done:** a timing measurement on hardware fine enough to see Lucky Thirteen or a
Bleichenbacher oracle - the checker shows that the instructions executed do not depend on the
secret, not what a processor does with them; no fuzzing of these paths; no external review.
