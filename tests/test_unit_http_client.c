#include "proven.h"
#include "proven_test.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

/*
 * The HTTP client, against two kinds of peer on the loopback interface:
 *
 *   - this library's own server, running on another thread, for everything a well-behaved
 *     origin does: bodies both ways, redirects, challenges, cookies, ranges, event streams;
 *   - scripted peers - a few lines of socket code on a worker - for what an origin server
 *     cannot play: a proxy, a SOCKS5 relay, and a server that closes a connection the client
 *     believed it could reuse.
 *
 * The client and the server here share one codec. This test shows the client drives it
 * correctly; that the bytes mean the same to curl and to Python's http.server is checked by a
 * private program outside this suite.
 */

static proven_u8str_view_t sv(const char *s) {
    return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)s, .size = strlen(s) };
}
static proven_mem_view_t mv(const char *s) {
    return (proven_mem_view_t){ .ptr = (const proven_byte_t *)s, .size = strlen(s) };
}
static bool starts(proven_u8str_view_t s, const char *prefix) {
    proven_size_t n = strlen(prefix);
    return s.size >= n && memcmp(s.ptr, prefix, n) == 0;
}

// -----------------------------------------------------------------------------
// The origin server
// -----------------------------------------------------------------------------

typedef struct {
    proven_http_server_t *server;
    proven_u16 port;
    proven_u16 port2;       /* a second listener: the same handler under another origin */
    atomic_int requests;
    atomic_int digest_step;
} origin_t;

static const char DIGEST_CHALLENGE_1[] = "Digest realm=\"proven\", qop=\"auth\", algorithm=SHA-256, nonce=\"n-one\", opaque=\"op\"";
static const char DIGEST_CHALLENGE_2[] = "Digest realm=\"proven\", qop=\"auth\", algorithm=SHA-256, nonce=\"n-two\", opaque=\"op\", stale=true";

/* Is `got` the Authorization value a client that knows user/pass would send for `challenge`? */
static bool digest_ok(const char *challenge, proven_u8str_view_t got, const char *method, const char *uri) {
    proven_http_digest_challenge_t dc;
    if (proven_http_digest_challenge_parse(sv(challenge), &dc) != PROVEN_OK) return false;
    static const char key[] = "cnonce=\"";
    proven_u8str_view_t cnonce = {0};
    for (proven_size_t i = 0; i + 8 <= got.size; ++i) {
        if (memcmp(got.ptr + i, key, 8) != 0) continue;
        proven_size_t end = i + 8;
        while (end < got.size && got.ptr[end] != '"') end++;
        cnonce = (proven_u8str_view_t){ .ptr = got.ptr + i + 8, .size = end - i - 8 };
        break;
    }
    if (cnonce.size == 0) return false;
    proven_byte_t want[1024];
    proven_size_t n = 0;
    if (proven_http_digest_auth(&dc, sv("user"), sv("pass"), sv(method), sv(uri), 1, cnonce,
                                (proven_mem_mut_t){ .ptr = want, .size = sizeof want }, &n) != PROVEN_OK) return false;
    return got.size == n && memcmp(got.ptr, want, n) == 0;
}

static void origin(void *ctx, proven_http_exchange_t *x) {
    origin_t *o = ctx;
    const proven_http_request_t *r = proven_http_exchange_request(x);
    proven_u8str_view_t t = r->target;
    atomic_fetch_add(&o->requests, 1);
    static _Thread_local proven_byte_t body[70000];
    static _Thread_local char text[4096];

    if (proven_u8str_view_eq(t, sv("/hello"))) {
        (void)proven_http_exchange_respond(x, 200, NULL, 0, mv("hello"));
    } else if (proven_u8str_view_eq(t, sv("/peer"))) {
        proven_net_addr_t peer = proven_http_exchange_peer(x);
        proven_size_t n = 0;
        if (proven_net_addr_format(&peer, (proven_mem_mut_t){ .ptr = body, .size = 200 }, &n) != PROVEN_OK) return;
        (void)proven_http_exchange_respond(x, 200, NULL, 0, (proven_mem_view_t){ .ptr = body, .size = n });
    } else if (starts(t, "/echo")) {
        proven_size_t n = 0;
        for (;;) {
            proven_result_size_t got = proven_http_exchange_read(x, (proven_mem_mut_t){ .ptr = body + n, .size = sizeof body - n });
            if (got.err != PROVEN_OK) break;
            n += got.value;
        }
        proven_u8str_view_t type = sv("none");
        (void)proven_http_header_find(r->headers, r->header_count, sv("Content-Type"), &type);
        bool chunked = proven_http_header_has_token(r->headers, r->header_count, sv("Transfer-Encoding"), sv("chunked"));
        proven_http_header_t h[3] = {
            { sv("X-Method"), r->method_text }, { sv("X-Type"), type }, { sv("X-Framing"), chunked ? sv("chunked") : sv("length") },
        };
        (void)proven_http_exchange_respond(x, 200, h, 3, (proven_mem_view_t){ .ptr = body, .size = n });
    } else if (proven_u8str_view_eq(t, sv("/headers"))) {
        /* What the client sent, for the fields a client decides about. */
        static const char *names[] = { "Host", "Authorization", "Cookie", "User-Agent", "X-Custom", "Proxy-Authorization" };
        int n = 0;
        for (int i = 0; i < 6; ++i) {
            proven_u8str_view_t v;
            if (!proven_http_header_find(r->headers, r->header_count, sv(names[i]), &v)) continue;
            n += snprintf(text + n, sizeof text - (size_t)n, "%s=%.*s\n", names[i], (int)v.size, (const char *)v.ptr);
        }
        (void)proven_http_exchange_respond(x, 200, NULL, 0, (proven_mem_view_t){ .ptr = (const proven_byte_t *)text, .size = (proven_size_t)n });
    } else if (starts(t, "/hop/")) {
        int left = t.ptr[5] - '0';
        if (left == 0) { (void)proven_http_exchange_respond(x, 200, NULL, 0, mv("landed")); return; }
        int n = snprintf(text, sizeof text, "/hop/%d", left - 1);
        proven_http_header_t h = { sv("Location"), { .ptr = (const proven_byte_t *)text, .size = (proven_size_t)n } };
        (void)proven_http_exchange_respond(x, 302, &h, 1, mv("moved"));
    } else if (starts(t, "/to/")) {
        /* /to/<status>: redirect with that status to ../echo?from=<status>#frag - relative,
         * with dot segments and a fragment, to make the client resolve it. */
        int status = (t.ptr[4] - '0') * 100 + (t.ptr[5] - '0') * 10 + (t.ptr[6] - '0');
        int n = snprintf(text, sizeof text, "../a/../echo?from=%d#frag", status);
        proven_http_header_t h = { sv("Location"), { .ptr = (const proven_byte_t *)text, .size = (proven_size_t)n } };
        (void)proven_http_exchange_respond(x, (proven_u16)status, &h, 1, (proven_mem_view_t){0});
    } else if (proven_u8str_view_eq(t, sv("/away"))) {
        /* To the same server under another port: another origin. */
        int n = snprintf(text, sizeof text, "http://127.0.0.1:%u/headers", (unsigned)o->port2);
        proven_http_header_t h = { sv("Location"), { .ptr = (const proven_byte_t *)text, .size = (proven_size_t)n } };
        (void)proven_http_exchange_respond(x, 302, &h, 1, (proven_mem_view_t){0});
    } else if (proven_u8str_view_eq(t, sv("/away-auth"))) {
        int n = snprintf(text, sizeof text, "http://127.0.0.1:%u/basic", (unsigned)o->port2);
        proven_http_header_t h = { sv("Location"), { .ptr = (const proven_byte_t *)text, .size = (proven_size_t)n } };
        (void)proven_http_exchange_respond(x, 302, &h, 1, (proven_mem_view_t){0});
    } else if (proven_u8str_view_eq(t, sv("/down"))) {
        int n = snprintf(text, sizeof text, "http://127.0.0.1:%u/hello", (unsigned)o->port);
        proven_http_header_t h = { sv("Location"), { .ptr = (const proven_byte_t *)text, .size = (proven_size_t)n } };
        (void)proven_http_exchange_respond(x, 302, &h, 1, mv("down"));
    } else if (proven_u8str_view_eq(t, sv("/ftp"))) {
        proven_http_header_t h = { sv("Location"), sv("ftp://127.0.0.1/file") };
        (void)proven_http_exchange_respond(x, 302, &h, 1, mv("ftp"));
    } else if (proven_u8str_view_eq(t, sv("/basic"))) {
        proven_u8str_view_t a;
        if (proven_http_header_find(r->headers, r->header_count, sv("Authorization"), &a) && proven_u8str_view_eq(a, sv("Basic dXNlcjpwYXNz"))) {
            (void)proven_http_exchange_respond(x, 200, NULL, 0, mv("basic ok"));
        } else {
            proven_http_header_t h = { sv("WWW-Authenticate"), sv("Basic realm=\"proven\"") };
            (void)proven_http_exchange_respond(x, 401, &h, 1, mv("who?"));
        }
    } else if (proven_u8str_view_eq(t, sv("/digest?x=1"))) {
        proven_u8str_view_t a;
        if (proven_http_header_find(r->headers, r->header_count, sv("Authorization"), &a) && digest_ok(DIGEST_CHALLENGE_1, a, "GET", "/digest?x=1")) {
            (void)proven_http_exchange_respond(x, 200, NULL, 0, mv("digest ok"));
        } else {
            /* Basic is offered too; a client that can do Digest must prefer it. */
            proven_http_header_t h[2] = { { sv("WWW-Authenticate"), sv("Basic realm=\"proven\"") }, { sv("WWW-Authenticate"), sv(DIGEST_CHALLENGE_1) } };
            (void)proven_http_exchange_respond(x, 401, h, 2, mv("who?"));
        }
    } else if (proven_u8str_view_eq(t, sv("/stale"))) {
        /* The first good answer is told its nonce has expired; the second, with the new
         * nonce, is let in. */
        proven_u8str_view_t a;
        bool has = proven_http_header_find(r->headers, r->header_count, sv("Authorization"), &a);
        if (has && digest_ok(DIGEST_CHALLENGE_2, a, "GET", "/stale")) {
            atomic_store(&o->digest_step, 3);
            (void)proven_http_exchange_respond(x, 200, NULL, 0, mv("fresh"));
        } else if (has && digest_ok(DIGEST_CHALLENGE_1, a, "GET", "/stale")) {
            atomic_store(&o->digest_step, 2);
            proven_http_header_t h = { sv("WWW-Authenticate"), sv(DIGEST_CHALLENGE_2) };
            (void)proven_http_exchange_respond(x, 401, &h, 1, mv("stale"));
        } else {
            proven_http_header_t h = { sv("WWW-Authenticate"), sv(DIGEST_CHALLENGE_1) };
            (void)proven_http_exchange_respond(x, 401, &h, 1, mv("who?"));
        }
    } else if (proven_u8str_view_eq(t, sv("/setcookie"))) {
        proven_http_header_t h[3] = {
            { sv("Set-Cookie"), sv("sid=abc; Path=/; HttpOnly") },
            { sv("Set-Cookie"), sv("theme=dark") },
            { sv("Set-Cookie"), sv("evil=1; Domain=example.com") },
        };
        (void)proven_http_exchange_respond(x, 200, h, 3, mv("set"));
    } else if (proven_u8str_view_eq(t, sv("/chunked"))) {
        if (proven_http_exchange_begin(x, 200, NULL, 0, PROVEN_HTTP_LENGTH_UNKNOWN) != PROVEN_OK) return;
        for (int i = 0; i < 50; ++i) {
            for (int k = 0; k < 1000; ++k) body[k] = (proven_byte_t)('a' + (i + k) % 26);
            if (proven_http_exchange_write(x, (proven_mem_view_t){ .ptr = body, .size = 1000 }) != PROVEN_OK) return;
        }
    } else if (proven_u8str_view_eq(t, sv("/cut"))) {
        if (proven_http_exchange_begin(x, 200, NULL, 0, 10) != PROVEN_OK) return;
        (void)proven_http_exchange_write(x, mv("abc"));
    } else if (proven_u8str_view_eq(t, sv("/empty"))) {
        (void)proven_http_exchange_respond(x, 204, NULL, 0, (proven_mem_view_t){0});
    } else if (proven_u8str_view_eq(t, sv("/wide"))) {
        memset(text, 'w', 3000);
        proven_http_header_t h = { sv("X-Wide"), { .ptr = (const proven_byte_t *)text, .size = 3000 } };
        (void)proven_http_exchange_respond(x, 200, &h, 1, mv("wide"));
    } else if (proven_u8str_view_eq(t, sv("/events"))) {
        proven_http_header_t h = { sv("Content-Type"), sv("text/event-stream") };
        if (proven_http_exchange_begin(x, 200, &h, 1, PROVEN_HTTP_LENGTH_UNKNOWN) != PROVEN_OK) return;
        (void)proven_http_exchange_write(x, mv(": a comment\n\nid: 1\nda"));
        (void)proven_http_exchange_write(x, mv("ta: first\n\nevent: tick\nid: 2\ndata: line one\ndata: line two\n\n"));
        (void)proven_http_exchange_write(x, mv("retry: 1500\ndata: last\n\n"));
    } else if (proven_u8str_view_eq(t, sv("/letters"))) {
        static const char letters[] = "abcdefghijklmnopqrstuvwxyz";
        proven_u8str_view_t range;
        proven_u64 first = 0, last = 0;
        if (!proven_http_header_find(r->headers, r->header_count, sv("Range"), &range)) {
            (void)proven_http_exchange_respond(x, 200, NULL, 0, mv(letters));
            return;
        }
        proven_err_t e = proven_http_range_parse(range, 26, &first, &last);
        proven_mem_mut_t out = { .ptr = (proven_byte_t *)text, .size = sizeof text };
        proven_size_t len = 0;
        if (e == PROVEN_ERR_OUT_OF_BOUNDS) {
            proven_http_header_t h = { sv("Content-Range"), sv("bytes */26") };
            (void)proven_http_exchange_respond(x, 416, &h, 1, (proven_mem_view_t){0});
        } else if (e != PROVEN_OK || proven_http_write_content_range(out, &len, first, last, 26) != PROVEN_OK) {
            (void)proven_http_exchange_respond(x, 200, NULL, 0, mv(letters));
        } else {
            /* The writer produced a whole header line; the value is between ": " and CRLF. */
            proven_http_header_t h = { sv("Content-Range"), { .ptr = (const proven_byte_t *)text + 15, .size = len - 17 } };
            (void)proven_http_exchange_respond(x, 206, &h, 1, (proven_mem_view_t){ .ptr = (const proven_byte_t *)letters + first, .size = (proven_size_t)(last - first + 1) });
        }
    } else {
        (void)proven_http_exchange_respond(x, 404, NULL, 0, mv("not found"));
    }
}

static void serve(void *arg) {
    origin_t *o = arg;
    (void)proven_http_server_run(o->server);
}

// -----------------------------------------------------------------------------
// Scripted peers
// -----------------------------------------------------------------------------

typedef struct {
    proven_net_listener_t listener;
    proven_u16 port;
    char first[2048];       /* the first request head it read */
    char second[2048];      /* the second, when the script reads two */
    proven_byte_t socks[600];
    proven_size_t socks_len;
    int accepted;
} peer_t;

static bool peer_accept(peer_t *p, proven_net_conn_t *c) {
    if (proven_net_accept(&p->listener, proven_net_deadline_in(10000), c, NULL) != PROVEN_OK) return false;
    p->accepted++;
    return true;
}

/* Read a request head (to the empty line) into `out` as a C string. */
static bool peer_read_head(proven_net_conn_t *c, char *out, proven_size_t cap) {
    proven_size_t n = 0;
    out[0] = '\0';
    while (n + 1 < cap) {
        proven_result_size_t r = proven_net_read(c, (proven_mem_mut_t){ .ptr = (proven_byte_t *)out + n, .size = 1 }, proven_net_deadline_in(10000));
        if (r.err != PROVEN_OK) return false;
        n += r.value;
        out[n] = '\0';
        if (n >= 4 && memcmp(out + n - 4, "\r\n\r\n", 4) == 0) return true;
    }
    return false;
}

static bool peer_read_exact(proven_net_conn_t *c, proven_byte_t *out, proven_size_t n) {
    proven_size_t have = 0;
    while (have < n) {
        proven_result_size_t r = proven_net_read(c, (proven_mem_mut_t){ .ptr = out + have, .size = n - have }, proven_net_deadline_in(10000));
        if (r.err != PROVEN_OK) return false;
        have += r.value;
    }
    return true;
}

static void peer_say(proven_net_conn_t *c, const char *bytes) {
    (void)proven_net_write_all(c, mv(bytes), proven_net_deadline_in(10000));
}

/* Answers once and closes without saying so; then answers a second connection. */
static void script_forgetful(void *arg) {
    peer_t *p = arg;
    proven_net_conn_t c;
    if (!peer_accept(p, &c)) return;
    if (peer_read_head(&c, p->first, sizeof p->first)) peer_say(&c, "HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\nA");
    (void)proven_net_close(&c);
    if (!peer_accept(p, &c)) return;
    if (peer_read_head(&c, p->second, sizeof p->second)) peer_say(&c, "HTTP/1.1 200 OK\r\nContent-Length: 1\r\nConnection: close\r\n\r\nB");
    (void)proven_net_close(&c);
}

/* An HTTP proxy for a plain request: it is asked for a whole URL. */
static void script_proxy(void *arg) {
    peer_t *p = arg;
    proven_net_conn_t c;
    if (!peer_accept(p, &c)) return;
    if (peer_read_head(&c, p->first, sizeof p->first)) peer_say(&c, "HTTP/1.1 200 OK\r\nContent-Length: 9\r\nConnection: close\r\n\r\nvia proxy");
    (void)proven_net_close(&c);
}

/* An HTTP proxy asked for a tunnel: after its 200 the same bytes are the origin's. */
static void script_tunnel(void *arg) {
    peer_t *p = arg;
    proven_net_conn_t c;
    if (!peer_accept(p, &c)) return;
    if (peer_read_head(&c, p->first, sizeof p->first)) {
        peer_say(&c, "HTTP/1.1 200 Connection established\r\n\r\n");
        if (peer_read_head(&c, p->second, sizeof p->second)) peer_say(&c, "HTTP/1.1 200 OK\r\nContent-Length: 8\r\nConnection: close\r\n\r\ntunneled");
    }
    (void)proven_net_close(&c);
}

static void script_tunnel_denied(void *arg) {
    peer_t *p = arg;
    proven_net_conn_t c;
    if (!peer_accept(p, &c)) return;
    if (peer_read_head(&c, p->first, sizeof p->first)) peer_say(&c, "HTTP/1.1 407 Proxy Authentication Required\r\nContent-Length: 0\r\n\r\n");
    (void)proven_net_close(&c);
}

/* A SOCKS5 relay (RFC 1928) that wants a user name and password (RFC 1929), then plays the
 * origin itself. Everything the client sent during the negotiation is kept in p->socks. */
static void script_socks(void *arg) {
    peer_t *p = arg;
    proven_net_conn_t c;
    if (!peer_accept(p, &c)) return;
    proven_byte_t *s = p->socks;
    proven_size_t n = 0;
    do {
        if (!peer_read_exact(&c, s + n, 2)) break;                       /* VER NMETHODS */
        proven_size_t methods = s[n + 1];
        n += 2;
        if (!peer_read_exact(&c, s + n, methods)) break;
        n += methods;
        (void)proven_net_write_all(&c, (proven_mem_view_t){ .ptr = (const proven_byte_t *)"\x05\x02", .size = 2 }, proven_net_deadline_in(10000));
        if (!peer_read_exact(&c, s + n, 2)) break;                       /* VER ULEN */
        proven_size_t ulen = s[n + 1];
        n += 2;
        if (!peer_read_exact(&c, s + n, ulen + 1)) break;                /* UNAME PLEN */
        proven_size_t plen = s[n + ulen];
        n += ulen + 1;
        if (!peer_read_exact(&c, s + n, plen)) break;
        n += plen;
        (void)proven_net_write_all(&c, (proven_mem_view_t){ .ptr = (const proven_byte_t *)"\x01\x00", .size = 2 }, proven_net_deadline_in(10000));
        if (!peer_read_exact(&c, s + n, 5)) break;                       /* VER CMD RSV ATYP LEN */
        proven_size_t hlen = s[n + 4];
        n += 5;
        if (!peer_read_exact(&c, s + n, hlen + 2)) break;                /* the name, the port */
        n += hlen + 2;
        (void)proven_net_write_all(&c, (proven_mem_view_t){ .ptr = (const proven_byte_t *)"\x05\x00\x00\x01\x00\x00\x00\x00\x00\x00", .size = 10 }, proven_net_deadline_in(10000));
        if (peer_read_head(&c, p->first, sizeof p->first)) peer_say(&c, "HTTP/1.1 200 OK\r\nContent-Length: 7\r\nConnection: close\r\n\r\nsocksed");
    } while (0);
    p->socks_len = n;
    (void)proven_net_close(&c);
}

/* A SOCKS5 relay that cannot reach the host. */
static void script_socks_refused(void *arg) {
    peer_t *p = arg;
    proven_net_conn_t c;
    if (!peer_accept(p, &c)) return;
    proven_byte_t s[300];
    do {
        if (!peer_read_exact(&c, s, 2) || !peer_read_exact(&c, s + 2, s[1])) break;
        (void)proven_net_write_all(&c, (proven_mem_view_t){ .ptr = (const proven_byte_t *)"\x05\x00", .size = 2 }, proven_net_deadline_in(10000));
        if (!peer_read_exact(&c, s, 5) || !peer_read_exact(&c, s + 5, (proven_size_t)s[4] + 2)) break;
        (void)proven_net_write_all(&c, (proven_mem_view_t){ .ptr = (const proven_byte_t *)"\x05\x05\x00\x01\x00\x00\x00\x00\x00\x00", .size = 10 }, proven_net_deadline_in(10000));
    } while (0);
    (void)proven_net_close(&c);
}

/* A server that answers the head and then says nothing. */
static void script_silent(void *arg) {
    peer_t *p = arg;
    proven_net_conn_t c;
    if (!peer_accept(p, &c)) return;
    if (peer_read_head(&c, p->first, sizeof p->first)) {
        peer_say(&c, "HTTP/1.1 103 Early Hints\r\nLink: </s.css>\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nab");
        proven_byte_t sink[16];
        (void)proven_net_read(&c, (proven_mem_mut_t){ .ptr = sink, .size = sizeof sink }, proven_net_deadline_in(10000));
    }
    (void)proven_net_close(&c);
}

/* A server that answers with interim responses and never a final one. */
static void script_endless_interim(void *arg) {
    peer_t *p = arg;
    proven_net_conn_t c;
    if (!peer_accept(p, &c)) return;
    if (peer_read_head(&c, p->first, sizeof p->first)) {
        for (int i = 0; i < 12; ++i) peer_say(&c, "HTTP/1.1 102 Processing\r\n\r\n");
        proven_byte_t sink[16];
        (void)proven_net_read(&c, (proven_mem_mut_t){ .ptr = sink, .size = sizeof sink }, proven_net_deadline_in(10000));
    }
    (void)proven_net_close(&c);
}

static proven_job_sys_t *g_jobs;
static proven_job_group_t g_group;

static void peer_start(peer_t *p, void (*script)(void *)) {
    memset(p, 0, sizeof *p);
    proven_net_addr_t at;
    proven_err_t e = proven_net_listen(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), 4, &p->listener, &at);
    PROVEN_TEST_ASSERT(e == PROVEN_OK, "a scripted peer listens", "");
    p->port = at.port;
    proven_job_group_init(&g_group);
    e = proven_job_group_submit(g_jobs, &g_group, script, p);
    PROVEN_TEST_ASSERT(e == PROVEN_OK, "its script is started on a worker", "");
}

static void peer_finish(peer_t *p) {
    proven_job_group_wait(g_jobs, &g_group);
    (void)proven_net_listener_close(&p->listener);
}

// -----------------------------------------------------------------------------
// The client side
// -----------------------------------------------------------------------------

static proven_byte_t g_body[70000];
static proven_size_t g_body_len;
static char g_url[512];

/* Read a response body to its end. Returns the error that ended it: PROVEN_ERR_EOF normally. */
static proven_err_t take_body(proven_http_client_response_t *resp) {
    g_body_len = 0;
    for (;;) {
        proven_result_size_t r = proven_http_client_read(resp, (proven_mem_mut_t){ .ptr = g_body + g_body_len, .size = sizeof g_body - 1 - g_body_len });
        if (r.err != PROVEN_OK) { g_body[g_body_len] = '\0'; return r.err; }
        PROVEN_TEST_ASSERT(r.value > 0, "a successful body read is never zero bytes", "Inspect cl_read_body in src/proven/http_client.c.");
        g_body_len += r.value;
    }
}

static bool body_is(const char *text) {
    return g_body_len == strlen(text) && memcmp(g_body, text, g_body_len) == 0;
}
static bool body_has(const char *text) {
    return strstr((const char *)g_body, text) != NULL;
}

static proven_u8str_view_t url_at(proven_u16 port, const char *path) {
    int n = snprintf(g_url, sizeof g_url, "http://127.0.0.1:%u%s", (unsigned)port, path);
    return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)g_url, .size = (proven_size_t)n };
}

/* GET a URL and read the whole body; returns the status, or 0 with *err set. */
static proven_u16 fetch(proven_http_client_t *c, proven_u8str_view_t url, proven_err_t *err) {
    proven_http_client_response_t resp;
    *err = proven_http_client_get(c, url, &resp);
    proven_u16 status = 0;
    if (*err == PROVEN_OK) {
        status = resp.status;
        proven_err_t e = take_body(&resp);
        if (e != PROVEN_ERR_EOF) *err = e;
    }
    proven_http_client_finish(&resp);
    return status;
}

static proven_u8str_view_t header_of(const proven_http_client_response_t *resp, const char *name) {
    proven_u8str_view_t v = {0};
    (void)proven_http_header_find(resp->headers, resp->header_count, sv(name), &v);
    return v;
}

/* Stands where TLS will: hands the plaintext transport back as the "encrypted" one, and
 * counts, so the tests can see when the client asked for it and with which name. */
static int g_wraps;
static char g_wrap_host[128];
static proven_err_t passthrough_tls(void *ctx, proven_transport_t plain, proven_u8str_view_t host, proven_net_deadline_t until, proven_transport_t *out) {
    (void)until;
    g_wraps++;
    snprintf(g_wrap_host, sizeof g_wrap_host, "%.*s", (int)host.size, (const char *)host.ptr);
    if (ctx) return PROVEN_ERR_UNTRUSTED;
    *out = plain;
    return PROVEN_OK;
}

int main(void) {
    PROVEN_TEST_SUITE("http: the client",
        "Requests out, responses in, against this library's server and against scripted peers on loopback.",
        "Inspect src/proven/http_client.c.");

    {
        proven_net_listener_t probe;
        proven_err_t err = proven_net_listen(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), 1, &probe, NULL);
        if (err == PROVEN_ERR_PERMISSION || err == PROVEN_ERR_UNSUPPORTED) {
            PROVEN_TEST_INFO("SKIP: this environment refuses to open a listening socket (error {}).", PROVEN_ARG((int)err));
            PROVEN_TEST_PASS("skipped: no sockets here.");
            return 0;
        }
        PROVEN_TEST_ASSERT(err == PROVEN_OK, "a loopback listener on a free port opens", "");
        (void)proven_net_listener_close(&probe);
    }

    proven_allocator_t heap = proven_heap_allocator();
    proven_err_t e;

    /* The origin runs on its own one-thread job system, so that waiting for a scripted peer -
     * which runs queued jobs on the waiting thread - can never pick up the server's loop. */
    proven_job_sys_t *server_jobs = NULL;
    e = proven_job_system_init(heap, 1, 4, &server_jobs);
    PROVEN_TEST_ASSERT(e == PROVEN_OK, "a thread for the origin server", "");
    e = proven_job_system_init(heap, 2, 8, &g_jobs);
    PROVEN_TEST_ASSERT(e == PROVEN_OK, "threads for scripted peers", "");

    static origin_t o;
    proven_http_server_config_t scfg = {0};
    scfg.alloc = heap;
    scfg.handler = origin;
    scfg.handler_ctx = &o;
    scfg.max_body_bytes = 65536;
    e = proven_http_server_create(&scfg, &o.server);
    PROVEN_TEST_ASSERT(e == PROVEN_OK, "the origin server is created", "");
    proven_net_addr_t at, at2;
    e = proven_http_server_listen(o.server, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &at);
    PROVEN_TEST_ASSERT(e == PROVEN_OK, "it listens", "");
    e = proven_http_server_listen(o.server, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &at2);
    PROVEN_TEST_ASSERT(e == PROVEN_OK, "and on a second port", "");
    o.port = at.port;
    o.port2 = at2.port;
    proven_job_group_t server_group;
    proven_job_group_init(&server_group);
    e = proven_job_group_submit(server_jobs, &server_group, serve, &o);
    PROVEN_TEST_ASSERT(e == PROVEN_OK, "its loop is started", "");

    proven_http_client_config_t cfg = {0};
    cfg.alloc = heap;
    cfg.max_idle_connections = 4;
    cfg.max_redirects = 5;
    cfg.user_agent = sv("proven-test/1");
    proven_http_client_t *client = NULL;
    proven_http_client_response_t resp;

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("configuration is checked when the client is made",
        "An invalid allocator, a proxy that is not http or socks5, and credentials with control bytes are refused.",
        "Inspect proven_http_client_create.");
    // ---------------------------------------------------------------
    {
        proven_http_client_config_t bad = {0};
        PROVEN_TEST_ASSERT(proven_http_client_create(&bad, &client) == PROVEN_ERR_INVALID_ARG && client == NULL, "no allocator: PROVEN_ERR_INVALID_ARG", "");
        bad = cfg;
        bad.proxy = sv("ftp://127.0.0.1:1");
        PROVEN_TEST_ASSERT(proven_http_client_create(&bad, &client) == PROVEN_ERR_INVALID_ARG, "an ftp proxy: PROVEN_ERR_INVALID_ARG", "");
        bad = cfg;
        bad.username = sv("us\ner");
        PROVEN_TEST_ASSERT(proven_http_client_create(&bad, &client) == PROVEN_ERR_INVALID_ARG, "a user name with a line break: PROVEN_ERR_INVALID_ARG", "");
        proven_http_client_destroy(NULL);
        proven_http_client_response_t zero = {0};
        proven_http_client_finish(&zero);
        proven_http_client_finish(NULL);
    }

    e = proven_http_client_create(&cfg, &client);
    PROVEN_TEST_ASSERT(e == PROVEN_OK && client != NULL, "a client is created", "");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a GET, and what a request is refused for before anything is sent",
        "Status, headers and body arrive; error statuses are responses, not errors; malformed requests never reach the network.",
        "Inspect proven_http_client_send.");
    // ---------------------------------------------------------------
    {
        e = proven_http_client_get(client, url_at(o.port, "/hello"), &resp);
        PROVEN_TEST_ASSERT(e == PROVEN_OK && resp.status == 200, "a GET returns once the head is read", "");
        PROVEN_TEST_ASSERT(proven_u8str_view_eq(header_of(&resp, "content-length"), sv("5")), "response headers are there, by any case", "");
        PROVEN_TEST_ASSERT(header_of(&resp, "Date").size == PROVEN_HTTP_DATE_SIZE, "including the server's Date", "");
        PROVEN_TEST_ASSERT(resp.redirects == 0 && proven_u8str_view_eq(resp.url, url_at(o.port, "/hello")), "the response names the URL it came from", "");
        proven_byte_t two[2];
        proven_result_size_t r = proven_http_client_read(&resp, (proven_mem_mut_t){ .ptr = two, .size = 2 });
        PROVEN_TEST_ASSERT(r.err == PROVEN_OK && r.value == 2 && memcmp(two, "he", 2) == 0, "the body is read at the caller's pace, here two bytes", "");
        PROVEN_TEST_ASSERT(take_body(&resp) == PROVEN_ERR_EOF && body_is("llo"), "then the rest, then PROVEN_ERR_EOF", "");
        r = proven_http_client_read(&resp, (proven_mem_mut_t){ .ptr = two, .size = 2 });
        PROVEN_TEST_ASSERT(r.err == PROVEN_ERR_EOF, "and EOF again on asking again", "");
        proven_http_client_finish(&resp);
        PROVEN_TEST_ASSERT(resp.internal == NULL && resp.status == 0, "finish zeroes the response", "");

        PROVEN_TEST_ASSERT(fetch(client, url_at(o.port, "/nowhere"), &e) == 404 && e == PROVEN_OK && body_is("not found"), "a 404 is a response, with its body", "");
        PROVEN_TEST_ASSERT(fetch(client, url_at(o.port, "/empty"), &e) == 204 && e == PROVEN_OK && g_body_len == 0, "a 204 has no body: EOF at once", "");
        PROVEN_TEST_ASSERT(fetch(client, url_at(o.port, ""), &e) == 404 && e == PROVEN_OK, "a URL with no path asks for /", "");

        int before = atomic_load(&o.requests);
        PROVEN_TEST_ASSERT(fetch(client, sv("/hello"), &e) == 0 && e == PROVEN_ERR_INVALID_FORMAT, "a relative URL: PROVEN_ERR_INVALID_FORMAT", "");
        PROVEN_TEST_ASSERT(fetch(client, sv("ftp://127.0.0.1/x"), &e) == 0 && e == PROVEN_ERR_UNSUPPORTED, "another scheme: PROVEN_ERR_UNSUPPORTED", "");
        PROVEN_TEST_ASSERT(fetch(client, sv("https://127.0.0.1/x"), &e) == 0 && e == PROVEN_ERR_UNSUPPORTED, "https without a tls_wrap: PROVEN_ERR_UNSUPPORTED, not a silent downgrade", "");
        static const char *reserved[] = { "Host", "content-length", "Transfer-Encoding", "Connection" };
        for (int i = 0; i < 4; ++i) {
            proven_http_header_t h = { sv(reserved[i]), sv("x") };
            proven_http_client_request_t req = { .url = url_at(o.port, "/hello"), .headers = &h, .header_count = 1 };
            PROVEN_TEST_ASSERT(proven_http_client_send(client, &req, &resp) == PROVEN_ERR_INVALID_ARG, "a header the client writes itself: PROVEN_ERR_INVALID_ARG", "");
            proven_http_client_finish(&resp);
        }
        proven_http_header_t inj = { sv("X-Custom"), sv("a\r\nX-Evil: 1") };
        proven_http_client_request_t req = { .url = url_at(o.port, "/hello"), .headers = &inj, .header_count = 1 };
        PROVEN_TEST_ASSERT(proven_http_client_send(client, &req, &resp) != PROVEN_OK, "a header value with a line break is refused", "A request-splitting hole otherwise.");
        proven_http_client_finish(&resp);
        req = (proven_http_client_request_t){ .url = sv("http://127.0.0.1/a b") };
        PROVEN_TEST_ASSERT(proven_http_client_send(client, &req, &resp) != PROVEN_OK, "a URL with a space is refused", "");
        proven_http_client_finish(&resp);
        PROVEN_TEST_ASSERT(atomic_load(&o.requests) == before, "none of those reached the server", "");

        /* Nothing is listening where a listener was just closed. */
        proven_net_listener_t gone;
        proven_net_addr_t gone_at;
        PROVEN_TEST_ASSERT(proven_net_listen(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), 1, &gone, &gone_at) == PROVEN_OK, "a port is found", "");
        (void)proven_net_listener_close(&gone);
        PROVEN_TEST_ASSERT(fetch(client, url_at(gone_at.port, "/"), &e) == 0 && e == PROVEN_ERR_REFUSED, "nothing listening: PROVEN_ERR_REFUSED", "");
        PROVEN_TEST_ASSERT(fetch(client, sv("http://no-such-host.invalid/"), &e) == 0 && e == PROVEN_ERR_NOT_FOUND, "a name that does not resolve: PROVEN_ERR_NOT_FOUND", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("connections are reused, and a dead one is noticed",
        "Two requests travel on one connection; a connection the server closed while idle costs a retry, not an error.",
        "Inspect cl_pool_take, cl_pool_put and the replay loop in proven_http_client_send.");
    // ---------------------------------------------------------------
    {
        char first[200];
        PROVEN_TEST_ASSERT(fetch(client, url_at(o.port, "/peer"), &e) == 200, "the server says which port the client called from", "");
        snprintf(first, sizeof first, "%.*s", (int)(g_body_len < 199 ? g_body_len : 199), (const char *)g_body);
        PROVEN_TEST_ASSERT(fetch(client, url_at(o.port, "/peer"), &e) == 200 && body_is(first), "a second request comes from the same port: the same connection", "");

        /* A response that is finished unread is closed, not reused: its body is still on the wire. */
        e = proven_http_client_get(client, url_at(o.port, "/chunked"), &resp);
        PROVEN_TEST_ASSERT(e == PROVEN_OK, "a large response is begun", "");
        proven_http_client_finish(&resp);
        PROVEN_TEST_ASSERT(fetch(client, url_at(o.port, "/peer"), &e) == 200 && !body_is(first), "after finishing a response unread, the next request uses a new connection", "");

        proven_http_client_config_t once = cfg;
        once.max_idle_connections = 0;
        proven_http_client_t *c2 = NULL;
        PROVEN_TEST_ASSERT(proven_http_client_create(&once, &c2) == PROVEN_OK, "a client that keeps no connections", "");
        PROVEN_TEST_ASSERT(fetch(c2, url_at(o.port, "/peer"), &e) == 200, "is served", "");
        snprintf(first, sizeof first, "%.*s", (int)(g_body_len < 199 ? g_body_len : 199), (const char *)g_body);
        PROVEN_TEST_ASSERT(fetch(c2, url_at(o.port, "/peer"), &e) == 200 && !body_is(first), "from a different port each time", "");
        proven_http_client_destroy(c2);

        static peer_t p;
        peer_start(&p, script_forgetful);
        PROVEN_TEST_ASSERT(fetch(client, url_at(p.port, "/one"), &e) == 200 && e == PROVEN_OK && body_is("A"), "a server answers and, unannounced, closes", "");
        PROVEN_TEST_ASSERT(fetch(client, url_at(p.port, "/two"), &e) == 200 && e == PROVEN_OK && body_is("B"),
            "the next request finds the kept connection dead and is sent again on a new one", "");
        peer_finish(&p);
        PROVEN_TEST_ASSERT(p.accepted == 2 && strstr(p.second, "GET /two HTTP/1.1\r\n") != NULL, "the server saw two connections and the second request once", "");
        PROVEN_TEST_ASSERT(strstr(p.first, "\r\nUser-Agent: proven-test/1\r\n") != NULL, "the configured User-Agent is sent", "");
        PROVEN_TEST_ASSERT(strstr(p.first, "\r\nConnection:") == NULL, "a client that keeps connections does not ask for a close", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("request bodies: from memory and from a stream",
        "A body in memory goes with Content-Length; a stream goes chunked; the server reads the same bytes.",
        "Inspect the body section of cl_exchange.");
    // ---------------------------------------------------------------
    {
        proven_http_header_t h = { sv("Content-Type"), sv("application/x-www-form-urlencoded") };
        proven_byte_t form[128];
        proven_size_t fl = 0;
        proven_mem_mut_t fm = { .ptr = form, .size = sizeof form };
        PROVEN_TEST_ASSERT(proven_url_form_append(fm, &fl, mv("name"), mv("J. Doe & co")) == PROVEN_OK &&
                           proven_url_form_append(fm, &fl, mv("n"), mv("1+1=2")) == PROVEN_OK, "a form body is built", "");
        proven_http_client_request_t req = { .method = sv("POST"), .url = url_at(o.port, "/echo"), .headers = &h, .header_count = 1,
                                             .body = { .ptr = form, .size = fl } };
        e = proven_http_client_send(client, &req, &resp);
        PROVEN_TEST_ASSERT(e == PROVEN_OK && resp.status == 200, "a POST with a body in memory", "");
        PROVEN_TEST_ASSERT(proven_u8str_view_eq(header_of(&resp, "X-Method"), sv("POST")) && proven_u8str_view_eq(header_of(&resp, "X-Framing"), sv("length")) &&
                           proven_u8str_view_eq(header_of(&resp, "X-Type"), sv("application/x-www-form-urlencoded")), "arrives as POST, with a length and its type", "");
        PROVEN_TEST_ASSERT(take_body(&resp) == PROVEN_ERR_EOF && body_is("name=J.+Doe+%26+co&n=1%2B1%3D2"), "and the form bytes", "");
        proven_http_client_finish(&resp);

        static proven_byte_t big[40000];
        for (proven_size_t i = 0; i < sizeof big; ++i) big[i] = (proven_byte_t)('A' + i % 23);
        proven_reader_view_t rv;
        req = (proven_http_client_request_t){ .method = sv("PUT"), .url = url_at(o.port, "/echo"),
              .body_stream = proven_reader_from_view(&rv, (proven_u8str_view_t){ .ptr = big, .size = sizeof big }) };
        e = proven_http_client_send(client, &req, &resp);
        PROVEN_TEST_ASSERT(e == PROVEN_OK && resp.status == 200 && proven_u8str_view_eq(header_of(&resp, "X-Framing"), sv("chunked")), "a PUT from a stream is sent chunked", "");
        PROVEN_TEST_ASSERT(take_body(&resp) == PROVEN_ERR_EOF && g_body_len == sizeof big && memcmp(g_body, big, sizeof big) == 0, "and arrives whole", "");
        proven_http_client_finish(&resp);

        req = (proven_http_client_request_t){ .method = sv("POST"), .url = url_at(o.port, "/echo"), .body = mv("x"), .body_stream = proven_reader_from_view(&rv, sv("y")) };
        PROVEN_TEST_ASSERT(proven_http_client_send(client, &req, &resp) == PROVEN_ERR_INVALID_ARG, "both a body and a stream: PROVEN_ERR_INVALID_ARG", "");
        proven_http_client_finish(&resp);

        req = (proven_http_client_request_t){ .method = sv("POST"), .url = url_at(o.port, "/echo") };
        e = proven_http_client_send(client, &req, &resp);
        PROVEN_TEST_ASSERT(e == PROVEN_OK && take_body(&resp) == PROVEN_ERR_EOF && g_body_len == 0, "a POST without a body says Content-Length: 0 and is answered", "");
        proven_http_client_finish(&resp);

        /* More than the server will take: it answers 413 without reading the body, and that
         * answer must reach a client that is still in the middle of sending. */
        static proven_byte_t huge[300000];
        memset(huge, 'h', sizeof huge);
        req = (proven_http_client_request_t){ .method = sv("POST"), .url = url_at(o.port, "/echo"), .body = { .ptr = huge, .size = sizeof huge } };
        e = proven_http_client_send(client, &req, &resp);
        PROVEN_TEST_ASSERT(e == PROVEN_OK && resp.status == 413, "a body the server refuses by its length is answered 413, and the client receives it",
            "A PROVEN_ERR_RESET here means the server closed with the upload unread and the reset destroyed its own response: inspect sv_linger in src/proven/http_server.c.");
        proven_http_client_finish(&resp);

        req = (proven_http_client_request_t){ .method = sv("HEAD"), .url = url_at(o.port, "/hello") };
        e = proven_http_client_send(client, &req, &resp);
        PROVEN_TEST_ASSERT(e == PROVEN_OK && resp.status == 200 && proven_u8str_view_eq(header_of(&resp, "Content-Length"), sv("5")), "HEAD: the headers", "");
        PROVEN_TEST_ASSERT(take_body(&resp) == PROVEN_ERR_EOF && g_body_len == 0, "and no body, whatever Content-Length says", "");
        proven_http_client_finish(&resp);
        PROVEN_TEST_ASSERT(fetch(client, url_at(o.port, "/hello"), &e) == 200 && body_is("hello"), "the connection is in step afterwards", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("response bodies: chunked, cut short, too large a head, read_all",
        "A chunked body arrives as its payload; half a body is PROVEN_ERR_RESET, never a short success.",
        "Inspect cl_read_body and proven_http_client_read_all.");
    // ---------------------------------------------------------------
    {
        PROVEN_TEST_ASSERT(fetch(client, url_at(o.port, "/chunked"), &e) == 200 && e == PROVEN_OK && g_body_len == 50000, "fifty chunks of a thousand bytes", "");
        bool same = true;
        for (int i = 0; i < 50 && same; ++i) for (int k = 0; k < 1000; ++k) if (g_body[i * 1000 + k] != (proven_byte_t)('a' + (i + k) % 26)) { same = false; break; }
        PROVEN_TEST_ASSERT(same, "are the bytes the handler wrote", "");

        PROVEN_TEST_ASSERT(fetch(client, url_at(o.port, "/cut"), &e) == 200 && e == PROVEN_ERR_RESET && body_is("abc"),
            "a body that ends before its announced length: the bytes that came, then PROVEN_ERR_RESET", "EOF here would let half a download pass for a whole one.");

        proven_u8str_t all = {0};
        e = proven_http_client_get(client, url_at(o.port, "/chunked"), &resp);
        PROVEN_TEST_ASSERT(e == PROVEN_OK, "the large response again", "");
        e = proven_http_client_read_all(&resp, heap, &all, 100000);
        PROVEN_TEST_ASSERT(e == PROVEN_OK && proven_u8str_as_view(&all).size == 50000, "read_all gathers it", "");
        proven_http_client_finish(&resp);
        PROVEN_TEST_ASSERT(proven_u8str_reset(&all) == PROVEN_OK, "the string is emptied", "");
        e = proven_http_client_get(client, url_at(o.port, "/chunked"), &resp);
        PROVEN_TEST_ASSERT(e == PROVEN_OK, "and again", "");
        e = proven_http_client_read_all(&resp, heap, &all, 12345);
        PROVEN_TEST_ASSERT(e == PROVEN_ERR_OUT_OF_BOUNDS && proven_u8str_as_view(&all).size == 12345, "with a limit below its size: PROVEN_ERR_OUT_OF_BOUNDS, holding exactly the limit", "");
        proven_http_client_finish(&resp);
        proven_u8str_destroy(heap, &all);

        proven_http_client_config_t tight = cfg;
        tight.max_head_bytes = 2048;
        proven_http_client_t *c2 = NULL;
        PROVEN_TEST_ASSERT(proven_http_client_create(&tight, &c2) == PROVEN_OK, "a client with a 2 KiB head limit", "");
        PROVEN_TEST_ASSERT(fetch(c2, url_at(o.port, "/wide"), &e) == 0 && e == PROVEN_ERR_OUT_OF_BOUNDS, "a response head larger than that: PROVEN_ERR_OUT_OF_BOUNDS", "");
        PROVEN_TEST_ASSERT(fetch(c2, url_at(o.port, "/hello"), &e) == 200 && body_is("hello"), "and the client is still usable", "");
        proven_http_client_destroy(c2);
        PROVEN_TEST_ASSERT(fetch(client, url_at(o.port, "/wide"), &e) == 200 && body_is("wide"), "the default limit takes it", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("timeouts, and interim responses",
        "A server that stops mid-body costs io_timeout_ms; 103 Early Hints is passed over.",
        "Inspect the head loop in cl_exchange.");
    // ---------------------------------------------------------------
    {
        proven_http_client_config_t quick = cfg;
        quick.io_timeout_ms = 200;
        proven_http_client_t *c2 = NULL;
        PROVEN_TEST_ASSERT(proven_http_client_create(&quick, &c2) == PROVEN_OK, "a client with a 200 ms limit", "");
        static peer_t p;
        peer_start(&p, script_silent);
        proven_time_t start = proven_time_monotonic_now();
        e = proven_http_client_get(c2, url_at(p.port, "/"), &resp);
        PROVEN_TEST_ASSERT(e == PROVEN_OK && resp.status == 200, "the 103 is skipped and the 200 is the response", "");
        PROVEN_TEST_ASSERT(take_body(&resp) == PROVEN_ERR_TIMEOUT && body_is("ab"), "two of five bytes come, then PROVEN_ERR_TIMEOUT", "");
        proven_i64 took = (proven_time_monotonic_now() - start) / 1000000;
        PROVEN_TEST_ASSERT(took >= 150 && took < 5000, "after about io_timeout_ms", "");
        proven_http_client_finish(&resp);
        peer_finish(&p);

        peer_start(&p, script_endless_interim);
        start = proven_time_monotonic_now();
        e = proven_http_client_get(c2, url_at(p.port, "/"), &resp);
        PROVEN_TEST_ASSERT(e == PROVEN_ERR_INVALID_FORMAT, "a server that sends interim responses without end is PROVEN_ERR_INVALID_FORMAT after eight", "Inspect CL_MAX_INTERIM in cl_exchange.");
        PROVEN_TEST_ASSERT((proven_time_monotonic_now() - start) / 1000000 < 5000, "decided by the count, without waiting for a timeout", "");
        proven_http_client_finish(&resp);
        proven_http_client_destroy(c2);
        peer_finish(&p);
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("redirects",
        "Followed up to the limit, resolved against the current URL; the method rules of RFC 9110; credentials stay with their origin.",
        "Inspect the redirect block in proven_http_client_send. A failure in the cross-origin rows is a credential leak.");
    // ---------------------------------------------------------------
    {
        e = proven_http_client_get(client, url_at(o.port, "/hop/3"), &resp);
        PROVEN_TEST_ASSERT(e == PROVEN_OK && resp.status == 200 && resp.redirects == 3, "three hops are followed", "");
        proven_byte_t want[100];
        int wn = snprintf((char *)want, sizeof want, "http://127.0.0.1:%u/hop/0", (unsigned)o.port);
        PROVEN_TEST_ASSERT(proven_u8str_view_eq(resp.url, (proven_u8str_view_t){ .ptr = want, .size = (proven_size_t)wn }), "the response names where it ended up", "");
        PROVEN_TEST_ASSERT(take_body(&resp) == PROVEN_ERR_EOF && body_is("landed"), "and carries that page", "");
        proven_http_client_finish(&resp);

        e = proven_http_client_get(client, url_at(o.port, "/hop/6"), &resp);
        PROVEN_TEST_ASSERT(e == PROVEN_OK && resp.status == 302 && resp.redirects == 5, "past max_redirects the 302 itself is returned", "");
        PROVEN_TEST_ASSERT(proven_u8str_view_eq(header_of(&resp, "Location"), sv("/hop/0")), "with its Location for the caller to judge", "");
        proven_http_client_finish(&resp);

        proven_http_client_config_t none = cfg;
        none.max_redirects = 0;
        proven_http_client_t *c2 = NULL;
        PROVEN_TEST_ASSERT(proven_http_client_create(&none, &c2) == PROVEN_OK, "a client that follows none", "");
        PROVEN_TEST_ASSERT(fetch(c2, url_at(o.port, "/hop/1"), &e) == 302 && body_is("moved"), "gets the 302 and its body", "");
        proven_http_client_destroy(c2);

        static const struct { const char *path; const char *method; const char *arrives; bool body_kept; } rows[] = {
            { "/to/301", "POST", "GET", false }, { "/to/302", "POST", "GET", false }, { "/to/303", "POST", "GET", false },
            { "/to/303", "PUT", "GET", false },  { "/to/307", "POST", "POST", true }, { "/to/308", "POST", "POST", true },
            { "/to/301", "PUT", "PUT", true },   { "/to/302", "DELETE", "DELETE", true },
        };
        for (proven_size_t i = 0; i < sizeof rows / sizeof rows[0]; ++i) {
            proven_http_header_t h = { sv("Content-Type"), sv("text/plain") };
            proven_http_client_request_t req = { .method = sv(rows[i].method), .url = url_at(o.port, rows[i].path), .headers = &h, .header_count = 1, .body = mv("payload") };
            e = proven_http_client_send(client, &req, &resp);
            PROVEN_TEST_ASSERT(e == PROVEN_OK && resp.status == 200 && resp.redirects == 1, "a redirect with a body is followed", "");
            if (!proven_u8str_view_eq(header_of(&resp, "X-Method"), sv(rows[i].arrives))) PROVEN_TEST_INFO("row: {} {}", PROVEN_ARG(rows[i].method), PROVEN_ARG(rows[i].path));
            PROVEN_TEST_ASSERT(proven_u8str_view_eq(header_of(&resp, "X-Method"), sv(rows[i].arrives)), "with the method the status calls for", "301/302 turn only POST into GET; 303 turns everything; 307/308 nothing.");
            PROVEN_TEST_ASSERT(take_body(&resp) == PROVEN_ERR_EOF && body_is(rows[i].body_kept ? "payload" : ""), "and the body kept or dropped with it", "");
            PROVEN_TEST_ASSERT(proven_u8str_view_eq(header_of(&resp, "X-Type"), sv(rows[i].body_kept ? "text/plain" : "none")), "Content-Type goes only where the body goes", "");
            /* "../a/../echo?from=NNN#frag" against "/to/NNN": dot segments resolved, fragment not sent. */
            PROVEN_TEST_ASSERT(resp.url.size > 15 && memcmp(resp.url.ptr + resp.url.size - 14, "/echo?from=30", 13) == 0, "the relative Location was resolved, and its fragment dropped", "");
            proven_http_client_finish(&resp);
        }

        /* A stream cannot be sent twice: a 307 for one is handed back. */
        proven_reader_view_t rv;
        proven_http_client_request_t req = { .method = sv("POST"), .url = url_at(o.port, "/to/307"), .body_stream = proven_reader_from_view(&rv, sv("once")) };
        e = proven_http_client_send(client, &req, &resp);
        PROVEN_TEST_ASSERT(e == PROVEN_OK && resp.status == 307 && resp.redirects == 0, "a 307 for a stream body is returned, not followed", "");
        proven_http_client_finish(&resp);
        req = (proven_http_client_request_t){ .method = sv("POST"), .url = url_at(o.port, "/to/303"), .body_stream = proven_reader_from_view(&rv, sv("once")) };
        e = proven_http_client_send(client, &req, &resp);
        PROVEN_TEST_ASSERT(e == PROVEN_OK && resp.status == 200 && proven_u8str_view_eq(header_of(&resp, "X-Method"), sv("GET")), "a 303 for one is followed: the GET needs no body", "");
        proven_http_client_finish(&resp);

        PROVEN_TEST_ASSERT(fetch(client, url_at(o.port, "/ftp"), &e) == 302 && body_is("ftp"), "a redirect to another scheme is returned, not followed", "");

        /* Leaving the origin: what the caller attached for the first host stays behind. */
        proven_http_header_t hs[3] = { { sv("Authorization"), sv("Bearer secret") }, { sv("Cookie"), sv("session=secret") }, { sv("X-Custom"), sv("kept") } };
        req = (proven_http_client_request_t){ .url = url_at(o.port, "/away"), .headers = hs, .header_count = 3 };
        e = proven_http_client_send(client, &req, &resp);
        PROVEN_TEST_ASSERT(e == PROVEN_OK && resp.status == 200 && take_body(&resp) == PROVEN_ERR_EOF, "a redirect to another port is followed", "");
        PROVEN_TEST_ASSERT(!body_has("secret"), "Authorization and Cookie are not sent to the other origin", "A credential leak: inspect cl_is_credential_header and at.cross_origin.");
        PROVEN_TEST_ASSERT(body_has("X-Custom=kept"), "other headers are", "");
        proven_http_client_finish(&resp);
        req = (proven_http_client_request_t){ .url = url_at(o.port, "/headers"), .headers = hs, .header_count = 3 };
        e = proven_http_client_send(client, &req, &resp);
        PROVEN_TEST_ASSERT(e == PROVEN_OK && take_body(&resp) == PROVEN_ERR_EOF && body_has("Authorization=Bearer secret") && body_has("Cookie=session=secret"),
            "to the origin they were meant for, they are sent", "");
        proven_http_client_finish(&resp);
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("answering a challenge",
        "Basic and Digest, only after being asked, Digest preferred, a stale nonce retried once, a wrong password not retried, and never across origins.",
        "Inspect the 401 block in proven_http_client_send.");
    // ---------------------------------------------------------------
    {
        PROVEN_TEST_ASSERT(fetch(client, url_at(o.port, "/basic"), &e) == 401 && body_is("who?"), "without credentials a 401 is the response", "");

        proven_http_client_config_t known = cfg;
        known.username = sv("user");
        known.password = sv("pass");
        proven_http_client_t *c2 = NULL;
        PROVEN_TEST_ASSERT(proven_http_client_create(&known, &c2) == PROVEN_OK, "a client with a user name and password", "");
        int before = atomic_load(&o.requests);
        PROVEN_TEST_ASSERT(fetch(c2, url_at(o.port, "/headers"), &e) == 200 && !body_has("Authorization"), "credentials are not volunteered", "");
        before = atomic_load(&o.requests);
        PROVEN_TEST_ASSERT(fetch(c2, url_at(o.port, "/basic"), &e) == 200 && body_is("basic ok"), "a Basic challenge is answered", "");
        PROVEN_TEST_ASSERT(atomic_load(&o.requests) == before + 2, "in two requests: the challenge and the answer", "");
        PROVEN_TEST_ASSERT(fetch(c2, url_at(o.port, "/digest?x=1"), &e) == 200 && body_is("digest ok"),
            "offered Basic and Digest, the client answers Digest, and the server's own computation agrees", "Inspect proven_http_digest_auth; the uri must be the target as sent, query included.");
        atomic_store(&o.digest_step, 0);
        before = atomic_load(&o.requests);
        PROVEN_TEST_ASSERT(fetch(c2, url_at(o.port, "/stale"), &e) == 200 && body_is("fresh") && atomic_load(&o.digest_step) == 3, "a stale nonce is answered again with the new one", "");
        PROVEN_TEST_ASSERT(atomic_load(&o.requests) == before + 3, "in three requests", "");
        e = proven_http_client_get(c2, url_at(o.port, "/away-auth"), &resp);
        PROVEN_TEST_ASSERT(e == PROVEN_OK && resp.status == 401 && resp.redirects == 1, "a challenge from an origin a redirect led to is not answered", "A credential leak otherwise.");
        proven_http_client_finish(&resp);
        proven_http_client_destroy(c2);

        known.password = sv("wrong");
        PROVEN_TEST_ASSERT(proven_http_client_create(&known, &c2) == PROVEN_OK, "a client with the wrong password", "");
        before = atomic_load(&o.requests);
        PROVEN_TEST_ASSERT(fetch(c2, url_at(o.port, "/basic"), &e) == 401, "gets the 401 back", "");
        PROVEN_TEST_ASSERT(atomic_load(&o.requests) == before + 2, "after one answer, not a loop", "");
        before = atomic_load(&o.requests);
        PROVEN_TEST_ASSERT(fetch(c2, url_at(o.port, "/digest?x=1"), &e) == 401 && atomic_load(&o.requests) == before + 2, "and the same for Digest", "");
        proven_http_client_destroy(c2);
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("cookies, ranges and event streams",
        "A jar is filled from Set-Cookie and read for the next request; a range request gets its part; an event stream parses as it arrives.",
        "Inspect the cookie handling in cl_exchange; src/proven/http_cookie.c; src/proven/sse.c.");
    // ---------------------------------------------------------------
    {
        proven_http_cookie_jar_t jar;
        PROVEN_TEST_ASSERT(proven_http_cookie_jar_init(&jar, heap, 16) == PROVEN_OK, "a cookie jar", "");
        proven_http_client_config_t with = cfg;
        with.cookies = &jar;
        proven_http_client_t *c2 = NULL;
        PROVEN_TEST_ASSERT(proven_http_client_create(&with, &c2) == PROVEN_OK, "a client with the jar", "");
        PROVEN_TEST_ASSERT(fetch(c2, url_at(o.port, "/headers"), &e) == 200 && !body_has("Cookie="), "an empty jar sends no Cookie header", "");
        PROVEN_TEST_ASSERT(fetch(c2, url_at(o.port, "/setcookie"), &e) == 200, "a response sets three cookies", "");
        PROVEN_TEST_ASSERT(proven_http_cookie_jar_count(&jar) == 2, "two are kept: the one for a foreign Domain is not", "");
        PROVEN_TEST_ASSERT(fetch(c2, url_at(o.port, "/headers"), &e) == 200 && body_has("sid=abc") && body_has("theme=dark") && !body_has("evil"), "the next request carries them", "");
        PROVEN_TEST_ASSERT(fetch(c2, url_at(o.port, "/away"), &e) == 200 && body_has("sid=abc"),
            "cookies are for a host, whatever the port (RFC 6265 section 8.5)", "");
        proven_http_header_t own = { sv("Cookie"), sv("mine=1") };
        proven_http_client_request_t req = { .url = url_at(o.port, "/headers"), .headers = &own, .header_count = 1 };
        e = proven_http_client_send(c2, &req, &resp);
        PROVEN_TEST_ASSERT(e == PROVEN_OK && take_body(&resp) == PROVEN_ERR_EOF && body_has("Cookie=mine=1") && !body_has("sid"), "a Cookie header of the caller's own replaces the jar's", "");
        proven_http_client_finish(&resp);
        proven_http_client_destroy(c2);
        proven_http_cookie_jar_destroy(&jar);

        proven_http_header_t range = { sv("Range"), sv("bytes=5-9") };
        req = (proven_http_client_request_t){ .url = url_at(o.port, "/letters"), .headers = &range, .header_count = 1 };
        e = proven_http_client_send(client, &req, &resp);
        PROVEN_TEST_ASSERT(e == PROVEN_OK && resp.status == 206, "a range request is answered 206", "");
        proven_u64 first = 0, last = 0, total = 0;
        bool has_total = false;
        PROVEN_TEST_ASSERT(proven_http_content_range_parse(header_of(&resp, "Content-Range"), &first, &last, &total, &has_total) == PROVEN_OK && has_total && first == 5 && last == 9 && total == 26,
            "Content-Range says which part of how much", "");
        PROVEN_TEST_ASSERT(take_body(&resp) == PROVEN_ERR_EOF && body_is("fghij"), "and the body is that part", "");
        proven_http_client_finish(&resp);
        range.value = sv("bytes=40-");
        e = proven_http_client_send(client, &req, &resp);
        PROVEN_TEST_ASSERT(e == PROVEN_OK && resp.status == 416, "a range past the end is 416", "");
        proven_http_client_finish(&resp);

        e = proven_http_client_get(client, url_at(o.port, "/events"), &resp);
        PROVEN_TEST_ASSERT(e == PROVEN_OK && proven_u8str_view_eq(header_of(&resp, "Content-Type"), sv("text/event-stream")), "an event stream is opened", "");
        proven_byte_t work[512];
        proven_sse_t sse;
        PROVEN_TEST_ASSERT(proven_sse_init(&sse, (proven_mem_mut_t){ .ptr = work, .size = sizeof work }) == PROVEN_OK, "a parser for it", "");
        int events = 0;
        bool right = true;
        for (;;) {
            proven_byte_t piece[7];                    /* small on purpose: events straddle reads */
            proven_result_size_t r = proven_http_client_read(&resp, (proven_mem_mut_t){ .ptr = piece, .size = sizeof piece });
            if (r.err != PROVEN_OK) { PROVEN_TEST_ASSERT(r.err == PROVEN_ERR_EOF, "the stream ends cleanly", ""); break; }
            proven_size_t pos = 0;
            while (pos < r.value) {
                proven_size_t used = 0;
                proven_sse_event_t ev;
                bool have = false;
                PROVEN_TEST_ASSERT(proven_sse_feed(&sse, (proven_mem_view_t){ .ptr = piece + pos, .size = r.value - pos }, &used, &ev, &have) == PROVEN_OK, "the stream parses", "");
                pos += used;
                if (!have) continue;
                events++;
                if (events == 1) right = right && proven_u8str_view_eq(ev.data, sv("first")) && proven_u8str_view_eq(ev.id, sv("1")) && ev.event.size == 0;
                if (events == 2) right = right && proven_u8str_view_eq(ev.data, sv("line one\nline two")) && proven_u8str_view_eq(ev.event, sv("tick")) && proven_u8str_view_eq(ev.id, sv("2"));
                if (events == 3) right = right && proven_u8str_view_eq(ev.data, sv("last")) && ev.has_retry && ev.retry_ms == 1500 && proven_u8str_view_eq(ev.id, sv("2"));
            }
        }
        PROVEN_TEST_ASSERT(events == 3 && right, "three events, each with its data, type, id and retry", "");
        PROVEN_TEST_ASSERT(proven_u8str_view_eq(proven_sse_last_id(&sse), sv("2")), "and the id to resume from", "");
        proven_http_client_finish(&resp);
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("the TLS seam",
        "With a tls_wrap, an https URL is connected, wrapped for the host named, and used; a wrap that refuses fails the request; https never falls to http.",
        "Inspect the tls_wrap call in cl_connect and the downgrade rule in the redirect block.");
    // ---------------------------------------------------------------
    {
        proven_http_client_config_t tls = cfg;
        tls.tls_wrap = passthrough_tls;
        proven_http_client_t *c2 = NULL;
        PROVEN_TEST_ASSERT(proven_http_client_create(&tls, &c2) == PROVEN_OK, "a client with a stand-in for TLS", "");
        int n = snprintf(g_url, sizeof g_url, "https://127.0.0.1:%u/headers", (unsigned)o.port);
        g_wraps = 0;
        PROVEN_TEST_ASSERT(fetch(c2, (proven_u8str_view_t){ .ptr = (const proven_byte_t *)g_url, .size = (proven_size_t)n }, &e) == 200 && e == PROVEN_OK, "an https URL is fetched through it", "");
        PROVEN_TEST_ASSERT(g_wraps == 1 && strcmp(g_wrap_host, "127.0.0.1") == 0, "the wrap was called once, with the host to verify", "");
        n = snprintf(g_url, sizeof g_url, "https://127.0.0.1:%u/hello", (unsigned)o.port);
        PROVEN_TEST_ASSERT(fetch(c2, (proven_u8str_view_t){ .ptr = (const proven_byte_t *)g_url, .size = (proven_size_t)n }, &e) == 200 && g_wraps == 1, "a reused connection is not wrapped again", "");
        PROVEN_TEST_ASSERT(fetch(c2, url_at(o.port, "/peer"), &e) == 200 && g_wraps == 1, "an http URL to the same host and port does not use the https connection, nor the wrap", "");
        n = snprintf(g_url, sizeof g_url, "https://127.0.0.1:%u/down", (unsigned)o.port);
        e = proven_http_client_get(c2, (proven_u8str_view_t){ .ptr = (const proven_byte_t *)g_url, .size = (proven_size_t)n }, &resp);
        PROVEN_TEST_ASSERT(e == PROVEN_OK && resp.status == 302 && resp.redirects == 0, "a redirect from https to http is returned, not followed", "A downgrade otherwise.");
        proven_http_client_finish(&resp);
        proven_http_client_destroy(c2);

        tls.tls_ctx = &tls;        /* makes the stand-in refuse */
        PROVEN_TEST_ASSERT(proven_http_client_create(&tls, &c2) == PROVEN_OK, "a client whose wrap refuses", "");
        n = snprintf(g_url, sizeof g_url, "https://127.0.0.1:%u/hello", (unsigned)o.port);
        int before = atomic_load(&o.requests);
        PROVEN_TEST_ASSERT(fetch(c2, (proven_u8str_view_t){ .ptr = (const proven_byte_t *)g_url, .size = (proven_size_t)n }, &e) == 0 && e == PROVEN_ERR_UNTRUSTED, "its error is the request's: PROVEN_ERR_UNTRUSTED", "");
        PROVEN_TEST_ASSERT(atomic_load(&o.requests) == before, "and nothing was sent", "");
        proven_http_client_destroy(c2);
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("proxies",
        "An HTTP proxy is asked for the whole URL, or for a tunnel when the URL is https; a SOCKS5 relay is given the host name. The target name is never resolved here.",
        "Inspect cl_connect, cl_http_tunnel and cl_socks5. The target hosts are under .invalid: a PROVEN_ERR_NOT_FOUND means the client tried to resolve them itself.");
    // ---------------------------------------------------------------
    {
        static peer_t p;
        char proxy[100];
        proven_http_client_config_t via = cfg;
        proven_http_client_t *c2 = NULL;
        via.tls_wrap = passthrough_tls;

        peer_start(&p, script_proxy);
        snprintf(proxy, sizeof proxy, "http://puser:ppass@127.0.0.1:%u", (unsigned)p.port);
        via.proxy = sv(proxy);
        PROVEN_TEST_ASSERT(proven_http_client_create(&via, &c2) == PROVEN_OK, "a client with an HTTP proxy", "");
        memset(proxy, 0, sizeof proxy);        /* the client copied it */
        PROVEN_TEST_ASSERT(fetch(c2, sv("http://origin.invalid:8080/a/b?c=d#frag"), &e) == 200 && e == PROVEN_OK && body_is("via proxy"), "a plain URL is fetched through it", "");
        peer_finish(&p);
        PROVEN_TEST_ASSERT(strstr(p.first, "GET http://origin.invalid:8080/a/b?c=d HTTP/1.1\r\n") == p.first, "the proxy was asked for the absolute URL, without the fragment", "");
        PROVEN_TEST_ASSERT(strstr(p.first, "\r\nHost: origin.invalid:8080\r\n") != NULL, "with the origin as Host", "");
        PROVEN_TEST_ASSERT(strstr(p.first, "\r\nProxy-Authorization: Basic cHVzZXI6cHBhc3M=\r\n") != NULL, "and the proxy's credentials from its URL", "");

        peer_start(&p, script_tunnel);
        proven_http_client_destroy(c2);
        snprintf(proxy, sizeof proxy, "http://puser:ppass@127.0.0.1:%u", (unsigned)p.port);
        via.proxy = sv(proxy);
        PROVEN_TEST_ASSERT(proven_http_client_create(&via, &c2) == PROVEN_OK, "the same, at the next scripted proxy", "");
        g_wraps = 0;
        PROVEN_TEST_ASSERT(fetch(c2, sv("https://secure.invalid/s?t=1"), &e) == 200 && e == PROVEN_OK && body_is("tunneled"), "an https URL is fetched through a tunnel", "");
        peer_finish(&p);
        PROVEN_TEST_ASSERT(strstr(p.first, "CONNECT secure.invalid:443 HTTP/1.1\r\nHost: secure.invalid:443\r\n") == p.first, "the proxy was asked to CONNECT to host:port", "");
        PROVEN_TEST_ASSERT(strstr(p.first, "\r\nProxy-Authorization: Basic cHVzZXI6cHBhc3M=\r\n") != NULL, "with its credentials", "");
        PROVEN_TEST_ASSERT(strstr(p.second, "GET /s?t=1 HTTP/1.1\r\nHost: secure.invalid\r\n") == p.second, "inside the tunnel the request is an ordinary one", "");
        PROVEN_TEST_ASSERT(strstr(p.second, "Proxy-Authorization") == NULL, "and the proxy's credentials do not travel to the origin", "A credential leak otherwise.");
        PROVEN_TEST_ASSERT(g_wraps == 1 && strcmp(g_wrap_host, "secure.invalid") == 0, "TLS is begun inside the tunnel, for the origin's name", "");

        peer_start(&p, script_tunnel_denied);
        proven_http_client_destroy(c2);
        snprintf(proxy, sizeof proxy, "http://127.0.0.1:%u", (unsigned)p.port);
        via.proxy = sv(proxy);
        PROVEN_TEST_ASSERT(proven_http_client_create(&via, &c2) == PROVEN_OK, "a proxy that will refuse", "");
        g_wraps = 0;
        PROVEN_TEST_ASSERT(fetch(c2, sv("https://secure.invalid/"), &e) == 0 && e == PROVEN_ERR_PERMISSION && g_wraps == 0, "a 407 to CONNECT: PROVEN_ERR_PERMISSION, and TLS never begun", "");
        peer_finish(&p);
        PROVEN_TEST_ASSERT(strstr(p.first, "Proxy-Authorization") == NULL, "no credentials in the URL, none sent", "");

        peer_start(&p, script_socks);
        proven_http_client_destroy(c2);
        snprintf(proxy, sizeof proxy, "socks5://suser:spass@127.0.0.1:%u", (unsigned)p.port);
        via.proxy = sv(proxy);
        PROVEN_TEST_ASSERT(proven_http_client_create(&via, &c2) == PROVEN_OK, "a client with a SOCKS5 relay", "");
        PROVEN_TEST_ASSERT(fetch(c2, sv("http://far.invalid:8081/x"), &e) == 200 && e == PROVEN_OK && body_is("socksed"), "a URL is fetched through it", "");
        peer_finish(&p);
        static const proven_byte_t negotiation[] = {
            5, 2, 0, 2,                                             /* version 5; two methods: none, user/password */
            1, 5, 's','u','s','e','r', 5, 's','p','a','s','s',      /* RFC 1929 */
            5, 1, 0, 3, 11, 'f','a','r','.','i','n','v','a','l','i','d', 0x1f, 0x91,   /* CONNECT, a name, port 8081 */
        };
        PROVEN_TEST_ASSERT(p.socks_len == sizeof negotiation && memcmp(p.socks, negotiation, sizeof negotiation) == 0,
            "the negotiation is byte for byte what RFC 1928 and RFC 1929 lay out, with the host as a name", "");
        PROVEN_TEST_ASSERT(strstr(p.first, "GET /x HTTP/1.1\r\nHost: far.invalid:8081\r\n") == p.first, "then an ordinary request", "");

        peer_start(&p, script_socks_refused);
        proven_http_client_destroy(c2);
        snprintf(proxy, sizeof proxy, "socks5://127.0.0.1:%u", (unsigned)p.port);
        via.proxy = sv(proxy);
        PROVEN_TEST_ASSERT(proven_http_client_create(&via, &c2) == PROVEN_OK, "a relay that cannot reach the host", "");
        PROVEN_TEST_ASSERT(fetch(c2, sv("http://far.invalid/x"), &e) == 0 && e == PROVEN_ERR_REFUSED, "its reply 5 is PROVEN_ERR_REFUSED", "");
        peer_finish(&p);
        proven_http_client_destroy(c2);
    }

    proven_http_client_destroy(client);
    proven_http_server_stop(o.server);
    proven_job_group_wait(server_jobs, &server_group);
    proven_http_server_destroy(o.server);
    proven_job_system_close(g_jobs);
    proven_job_system_destroy(g_jobs);
    proven_job_system_close(server_jobs);
    proven_job_system_destroy(server_jobs);

    PROVEN_TEST_PASS("the client keeps its contract.");
    return 0;
}
