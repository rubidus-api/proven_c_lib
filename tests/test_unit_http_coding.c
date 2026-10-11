#include "proven.h"
#include "proven_test.h"
#include "test_unit_tls_pki.h"
#include "test_unit_http_coding_vectors.h"
#include "../src/proven/proven_internal_http_coding.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Content codings in the HTTP drivers: compressed responses from the two servers, decoded
 * bodies in the two clients.
 *
 * Three kinds of evidence, from the inside out:
 *   - the two helpers of http.h, against tables;
 *   - the decoder the clients share, against bodies zlib made (the vector header: gzip, zlib,
 *     raw, several members, and each way of being damaged), fed whole and a byte at a time;
 *   - the drivers over loopback. The blocking server runs on a thread of its own and answers
 *     the blocking client and the event-driven client; the event-driven server runs on the
 *     test's loop and answers the event-driven client, plain and over TLS; and both servers
 *     answer raw sockets, for what a client of this library never sends.
 *
 * Both servers answer the same paths from one table (plan_for), so what differs between two
 * runs is the driver and nothing else. That curl, Python and zlib agree with all of this is
 * checked by a private program outside this suite.
 */

static proven_u8str_view_t sv(const char *s) { return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)s, .size = strlen(s) }; }
static proven_mem_view_t mv(const char *s) { return (proven_mem_view_t){ .ptr = (const proven_byte_t *)s, .size = strlen(s) }; }

#define MIB ((proven_size_t)1024 * 1024)

static proven_allocator_t g_heap;
static proven_loop_t *g_loop;
static proven_byte_t g_text[MIB];          /* the body the generator of the vectors also writes: numbered lines */
static proven_byte_t g_noise[MIB];         /* bytes that do not compress */
static proven_byte_t g_got[3 * MIB];       /* what a client received */

static void fill_bodies(void) {
    proven_size_t n = 0;
    for (unsigned k = 0; n < sizeof g_text; ++k) {
        char line[64];
        int len = snprintf(line, sizeof line, "line %u of the body, which repeats itself\n", k);
        for (int i = 0; i < len && n < sizeof g_text; ++i) g_text[n++] = (proven_byte_t)line[i];
    }
    proven_u32 x = 2463534242u;
    for (proven_size_t i = 0; i < sizeof g_noise; ++i) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        g_noise[i] = (proven_byte_t)(x >> 11);
    }
}

static bool all_zero(const proven_byte_t *p, proven_size_t n) {
    for (proven_size_t i = 0; i < n; ++i) if (p[i] != 0) return false;
    return true;
}

/* Whether `got` is what vector `v` decodes to. */
static bool vector_plain_is(const coding_vector_t *v, const proven_byte_t *got, proven_size_t n) {
    if (v->plain < 0 || n != (proven_size_t)v->plain) return false;
    return v->kind == 'Z' ? all_zero(got, n) : memcmp(got, g_text, n) == 0;
}

#define VECTOR_COUNT (sizeof coding_vectors / sizeof coding_vectors[0])

// ---------------------------------------------------------------------------------------
// What the servers answer: one table for both
// ---------------------------------------------------------------------------------------

typedef struct {
    proven_u16 status;
    proven_http_header_t headers[4];
    proven_size_t header_count;
    bool compress, stream, known, cut, late;
    const proven_byte_t *body;
    proven_size_t total, step;
    char peer[16], accept[80];
} plan_t;

/*
 *   /r/N  compress asked, N bytes of text in one respond       /p/N  the same, not asked
 *   /s/N  compress asked, streamed with its length             /u/N  streamed with no length
 *   /x/N  compress asked, N bytes that do not compress         /n/N  the same, streamed (the megabyte of them over and over)
 *   /v/I  vector I under its Content-Encoding, in one respond  /w/I  streamed a byte a write
 *   /own /204 /range /206 /vary /short /late: see below
 * Every answer says which port the request came from and what Accept-Encoding it carried.
 */
static void plan_for(const proven_http_request_t *req, proven_net_addr_t peer, plan_t *p) {
    char path[64] = { 0 };
    memcpy(path, req->target.ptr, req->target.size < sizeof path - 1 ? req->target.size : sizeof path - 1);
    memset(p, 0, sizeof *p);
    p->status = 200;
    p->step = 1000;
    snprintf(p->peer, sizeof p->peer, "%u", (unsigned)peer.port);
    proven_u8str_view_t accept = { 0 };
    if (!proven_http_header_find(req->headers, req->header_count, PROVEN_LIT("Accept-Encoding"), &accept)) accept = sv("(none)");
    memcpy(p->accept, accept.ptr, accept.size < sizeof p->accept - 1 ? accept.size : sizeof p->accept - 1);
    p->headers[p->header_count++] = (proven_http_header_t){ PROVEN_LIT("X-Peer"), sv(p->peer) };
    p->headers[p->header_count++] = (proven_http_header_t){ PROVEN_LIT("X-Accept"), sv(p->accept) };
    proven_size_t n = strlen(path) > 3 ? (proven_size_t)atol(path + 3) : 0;
    p->body = g_text;
    if (path[1] != '\0' && path[2] == '/' && strchr("rpsuxn", path[1])) {
        p->total = n;
        p->compress = path[1] != 'p';
        p->stream = path[1] == 's' || path[1] == 'u' || path[1] == 'n';
        p->known = path[1] == 's';
        if (path[1] == 'x' || path[1] == 'n') p->body = g_noise;
    } else if ((path[1] == 'v' || path[1] == 'w') && path[2] == '/' && n < VECTOR_COUNT) {
        const coding_vector_t *v = &coding_vectors[n];
        p->body = v->bytes;
        p->total = v->size;
        p->stream = path[1] == 'w';
        p->step = 1;
        p->compress = true;                        /* asked for, and not done: the handler named a coding itself */
        p->headers[p->header_count++] = (proven_http_header_t){ PROVEN_LIT("Content-Encoding"), sv(v->encoding) };
    } else if (strcmp(path, "/own") == 0) {
        p->compress = true;
        p->total = 3000;
        p->headers[p->header_count++] = (proven_http_header_t){ PROVEN_LIT("Content-Encoding"), PROVEN_LIT("br") };
    } else if (strcmp(path, "/204") == 0) {
        p->compress = true;
        p->status = 204;
    } else if (strcmp(path, "/range") == 0) {
        p->compress = true;
        p->status = 206;
        p->total = 1000;
        p->headers[p->header_count++] = (proven_http_header_t){ PROVEN_LIT("Content-Range"), PROVEN_LIT("bytes 0-999/3000") };
    } else if (strcmp(path, "/206") == 0) {
        p->compress = true;
        p->status = 206;
        p->total = 1000;
    } else if (strcmp(path, "/vary") == 0) {
        p->compress = true;
        p->total = 3000;
        p->headers[p->header_count++] = (proven_http_header_t){ PROVEN_LIT("Vary"), PROVEN_LIT("Origin, accept-encoding") };
    } else if (strcmp(path, "/short") == 0) {
        p->compress = p->stream = p->known = p->cut = true;       /* promises 40,000 bytes and writes 4,000 */
        p->total = 4000;
    } else if (strcmp(path, "/late") == 0) {
        p->late = p->stream = p->known = true;                    /* asks only after the response has begun */
        p->total = 3000;
    } else {
        p->status = 404;
    }
}

/* The blocking server's handler. */
static void blocking_handler(void *ctx, proven_http_exchange_t *x) {
    (void)ctx;
    plan_t p;
    plan_for(proven_http_exchange_request(x), proven_http_exchange_peer(x), &p);
    if (p.compress) proven_http_exchange_compress(x);
    if (!p.stream) {
        (void)proven_http_exchange_respond(x, p.status, p.headers, p.header_count, (proven_mem_view_t){ .ptr = p.body, .size = p.total });
        return;
    }
    proven_u64 length = p.known ? (p.cut ? (proven_u64)p.total * 10 : p.total) : PROVEN_HTTP_LENGTH_UNKNOWN;
    if (proven_http_exchange_begin(x, p.status, p.headers, p.header_count, length) != PROVEN_OK) return;
    if (p.late) proven_http_exchange_compress(x);
    for (proven_size_t at = 0; at < p.total; at += p.step) {
        proven_size_t n = p.total - at < p.step ? p.total - at : p.step;
        if (proven_http_exchange_write(x, (proven_mem_view_t){ .ptr = p.body + at % MIB, .size = n }) != PROVEN_OK) return;
    }
    /* Half of them end by returning. */
    if (p.known) (void)proven_http_exchange_end(x);
}

static void blocking_serve(void *arg) { (void)proven_http_server_run(arg); }

/* The event-driven server's callbacks. */
typedef struct {
    proven_http_stream_t *stream;
    const proven_byte_t *body;
    proven_size_t total, step, sent;
} served_t;

static proven_size_t g_most_buffered;
static int g_writable_calls;

static void served_pump(served_t *a) {
    while (a->sent < a->total) {
        proven_size_t n = a->total - a->sent < a->step ? a->total - a->sent : a->step;
        if (n > MIB - a->sent % MIB) n = MIB - a->sent % MIB;
        proven_result_size_t w = proven_http_stream_write(a->stream, (proven_mem_view_t){ .ptr = a->body + a->sent % MIB, .size = n });
        if (w.err != PROVEN_OK) return;
        a->sent += w.value;
        proven_size_t held = proven_http_stream_buffered(a->stream);
        if (held > g_most_buffered) g_most_buffered = held;
        if (w.value < n) return;                   /* refused: on_writable will say when */
    }
    (void)proven_http_stream_end(a->stream);
}

static void event_request(void *ctx, proven_http_stream_t *s, const proven_http_request_t *req) {
    (void)ctx;
    plan_t p;
    plan_for(req, proven_http_stream_peer(s), &p);
    if (p.compress) proven_http_stream_compress(s);
    if (!p.stream) {
        (void)proven_http_stream_respond(s, p.status, p.headers, p.header_count, (proven_mem_view_t){ .ptr = p.body, .size = p.total });
        return;
    }
    served_t *a = calloc(1, sizeof *a);
    if (!a) { proven_http_stream_abort(s); return; }
    *a = (served_t){ .stream = s, .body = p.body, .total = p.total, .step = p.step > 1 ? 50000 : 1 };
    proven_http_stream_set_user(s, a);
    proven_u64 length = p.known ? (p.cut ? (proven_u64)p.total * 10 : p.total) : PROVEN_HTTP_EVENT_LENGTH_UNKNOWN;
    if (proven_http_stream_begin(s, p.status, p.headers, p.header_count, length) != PROVEN_OK) return;
    if (p.late) proven_http_stream_compress(s);
    served_pump(a);
}
static void event_writable(void *ctx, proven_http_stream_t *s) {
    (void)ctx;
    g_writable_calls++;
    served_t *a = proven_http_stream_user(s);
    if (a) served_pump(a);
}
static void event_done(void *ctx, proven_http_stream_t *s, proven_err_t why) {
    (void)ctx; (void)why;
    free(proven_http_stream_user(s));
}

// ---------------------------------------------------------------------------------------
// The clients
// ---------------------------------------------------------------------------------------

/* What came back, whichever client fetched it. `err` is PROVEN_OK for a whole body. */
typedef struct {
    proven_err_t err;
    int status;
    char encoding[32], vary[64], length[24], transfer[24], accept[80], peer[16];
    int vary_fields;
    proven_size_t size;                /* bytes in g_got */
    /* The event-driven client only. */
    int pieces, lasts, dones;
    proven_size_t largest_piece;
    bool done, pause_each, paused_now, while_paused;
    bool noise, noise_intact;          /* the body is the noise, over and over: checked as it comes, not kept */
    proven_u32 pause_once_ms;          /* stop at the first piece for this long */
    proven_http_event_request_t *request;
    proven_loop_timer_t timer;
} got_t;

static void copy_header(const proven_http_header_t *h, proven_size_t count, const char *name, char *out, proven_size_t cap) {
    proven_u8str_view_t v = { 0 };
    out[0] = '\0';
    if (!proven_http_header_find(h, count, sv(name), &v)) return;
    proven_size_t n = v.size < cap - 1 ? v.size : cap - 1;
    memcpy(out, v.ptr, n);
    out[n] = '\0';
}
static void take_head(got_t *g, int status, const proven_http_header_t *h, proven_size_t count) {
    g->status = status;
    copy_header(h, count, "Content-Encoding", g->encoding, sizeof g->encoding);
    copy_header(h, count, "Vary", g->vary, sizeof g->vary);
    copy_header(h, count, "Content-Length", g->length, sizeof g->length);
    copy_header(h, count, "Transfer-Encoding", g->transfer, sizeof g->transfer);
    copy_header(h, count, "X-Accept", g->accept, sizeof g->accept);
    copy_header(h, count, "X-Peer", g->peer, sizeof g->peer);
    g->vary_fields = (int)proven_http_header_count(h, count, PROVEN_LIT("Vary"));
}

static char g_url[128];
static proven_u8str_view_t url_at(bool tls, proven_u16 port, const char *path) {
    int n = snprintf(g_url, sizeof g_url, "%s://127.0.0.1:%u%s", tls ? "https" : "http", (unsigned)port, path);
    return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)g_url, .size = (proven_size_t)n };
}

/* The blocking client: one request, the body read to its end in reads of `read_size`. */
static void bfetch(proven_http_client_t *c, const char *method, proven_u16 port, const char *path,
                   const proven_http_header_t *headers, proven_size_t header_count, proven_size_t read_size, got_t *g) {
    memset(g, 0, sizeof *g);
    proven_http_client_request_t req = { .method = sv(method), .url = url_at(false, port, path), .headers = headers, .header_count = header_count };
    proven_http_client_response_t res;
    g->err = proven_http_client_send(c, &req, &res);
    if (g->err == PROVEN_OK) {
        take_head(g, res.status, res.headers, res.header_count);
        for (;;) {
            proven_size_t room = sizeof g_got - g->size < read_size ? sizeof g_got - g->size : read_size;
            proven_result_size_t r = proven_http_client_read(&res, (proven_mem_mut_t){ .ptr = g_got + g->size, .size = room });
            if (r.err == PROVEN_ERR_EOF) break;
            if (r.err != PROVEN_OK) { g->err = r.err; break; }
            PROVEN_TEST_ASSERT(r.value > 0 && r.value <= room, "a successful read is never zero bytes and never more than asked", "Check cl_read_decoded in src/proven/http_client.c.");
            g->size += r.value;
        }
    }
    proven_http_client_finish(&res);
}
static void bget(proven_http_client_t *c, proven_u16 port, const char *path, got_t *g) { bfetch(c, "GET", port, path, NULL, 0, 4096, g); }

/* The event-driven client. */
static void e_resume(void *ctx) {
    got_t *g = ctx;
    g->paused_now = false;
    proven_http_event_request_resume(g->request);
}
static void e_response(void *ctx, proven_http_event_request_t *r, const proven_http_response_t *head) {
    (void)r;
    take_head(ctx, head->status, head->headers, head->header_count);
}
static void e_body(void *ctx, proven_http_event_request_t *r, proven_mem_view_t piece, bool last) {
    got_t *g = ctx;
    if (g->paused_now) g->while_paused = true;
    g->pieces++;
    if (last) g->lasts++;
    if (piece.size > g->largest_piece) g->largest_piece = piece.size;
    if (g->noise) {
        for (proven_size_t i = 0; i < piece.size; ++i) if (piece.ptr[i] != g_noise[(g->size + i) % MIB]) g->noise_intact = false;
    } else if (piece.size > 0 && piece.size <= sizeof g_got - g->size) {
        memcpy(g_got + g->size, piece.ptr, piece.size);
    }
    g->size += piece.size;
    if (g->pause_once_ms != 0 && !last) {
        /* Stop for long enough that everything between the two ends fills up. */
        g->paused_now = true;
        proven_http_event_request_pause(r);
        proven_loop_timer_set(g_loop, &g->timer, g->pause_once_ms, e_resume, g);
        g->pause_once_ms = 0;
    } else if (g->pause_each && !last) {
        /* Stop after every piece, and go on from a timer: the rest must wait where it is. */
        g->paused_now = true;
        proven_http_event_request_pause(r);
        proven_loop_timer_set(g_loop, &g->timer, 1, e_resume, g);
    }
}
static void e_done(void *ctx, proven_http_event_request_t *r, proven_err_t why) {
    (void)r;
    got_t *g = ctx;
    proven_loop_timer_cancel(g_loop, &g->timer);
    g->dones++;
    g->err = why;
    g->done = true;
}

static bool spin_until(const bool *flag, proven_u32 ms) {
    proven_net_deadline_t until = proven_net_deadline_in(ms);
    while (!*flag && proven_time_monotonic_now() < until) {
        PROVEN_TEST_ASSERT(proven_loop_poll(g_loop, proven_net_deadline_in(10)) == PROVEN_OK, "a round of the loop", "");
    }
    return *flag;
}

static void efetch(proven_http_event_client_t *c, const char *method, bool tls, proven_u16 port, const char *path,
                   const proven_http_header_t *headers, proven_size_t header_count, bool pause_each, got_t *g) {
    bool noise = g->noise;
    proven_u32 pause_once_ms = g->pause_once_ms;
    memset(g, 0, sizeof *g);
    g->noise = g->noise_intact = noise;
    g->pause_once_ms = pause_once_ms;
    g->pause_each = pause_each;
    proven_http_event_request_options_t o = {
        .method = sv(method), .url = url_at(tls, port, path), .headers = headers, .header_count = header_count,
        .on = { e_response, e_body, NULL, e_done }, .ctx = g,
    };
    proven_err_t e = proven_http_event_client_start(c, &o, &g->request);
    PROVEN_TEST_ASSERT(e == PROVEN_OK, "a request starts", "");
    if (!spin_until(&g->done, 60000)) PROVEN_TEST_INFO("no end after 60 s: {} with {} bytes in {} pieces", PROVEN_ARG(path), PROVEN_ARG((unsigned)g->size), PROVEN_ARG(g->pieces));
    PROVEN_TEST_ASSERT(g->done && g->dones == 1, "and ends, once", "Check cl_decode and cl_process in src/proven/http_event_client.c.");
}
static void eget(proven_http_event_client_t *c, bool tls, proven_u16 port, const char *path, got_t *g) { efetch(c, "GET", tls, port, path, NULL, 0, false, g); }

/* A raw socket: send `request`, and read until the server closes. The loop is turned
 * meanwhile, since the server may be the one that lives on it. */
static char g_raw[2 * 65536];
static proven_size_t g_raw_len;
static const char *g_raw_body;
static proven_size_t g_raw_body_len;

static bool raw(proven_u16 port, const char *request) {
    proven_net_conn_t c;
    g_raw_len = 0;
    g_raw_body = NULL;
    g_raw_body_len = 0;
    if (proven_net_connect(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, port), proven_net_deadline_in(5000), &c) != PROVEN_OK) return false;
    bool ok = proven_net_write_all(&c, mv(request), proven_net_deadline_in(5000)).err == PROVEN_OK;
    proven_net_deadline_t until = proven_net_deadline_in(10000);
    bool closed = false;
    while (ok && !closed && proven_time_monotonic_now() < until) {
        (void)proven_loop_poll(g_loop, proven_net_deadline_in(2));
        for (;;) {
            proven_result_size_t r = proven_net_read(&c, (proven_mem_mut_t){ .ptr = (proven_byte_t *)g_raw + g_raw_len, .size = sizeof g_raw - 1 - g_raw_len }, PROVEN_NET_DONT_WAIT);
            if (r.err == PROVEN_ERR_EOF) { closed = true; break; }
            if (r.err != PROVEN_OK) break;
            g_raw_len += r.value;
            if (g_raw_len == sizeof g_raw - 1) { ok = false; break; }
        }
    }
    (void)proven_net_close(&c);
    g_raw[g_raw_len] = '\0';
    const char *end = strstr(g_raw, "\r\n\r\n");
    if (end) {
        g_raw_body = end + 4;
        g_raw_body_len = g_raw_len - (proven_size_t)(g_raw_body - g_raw);
    }
    return ok && closed && end != NULL;
}
static bool raw_has(const char *line) {
    const char *at = strstr(g_raw, line);
    return at != NULL && g_raw_body != NULL && at < g_raw_body;
}

static bool text_is(const got_t *g, proven_size_t n) { return g->err == PROVEN_OK && g->size == n && memcmp(g_got, g_text, n) == 0; }

typedef void (*fetch_fn)(void *client, bool tls, proven_u16 port, const char *method, const char *path, got_t *g);

// ---------------------------------------------------------------------------------------
// A scripted peer: one answer with no length, ended by closing
// ---------------------------------------------------------------------------------------

typedef struct {
    proven_net_listener_t listener;
    proven_u16 port;
    const coding_vector_t *vector;
} peer_t;

static proven_job_sys_t *g_peer_jobs;
static proven_job_group_t g_peer_group;

static void peer_script(void *arg) {
    peer_t *p = arg;
    proven_net_conn_t c;
    if (proven_net_accept(&p->listener, proven_net_deadline_in(10000), &c, NULL) != PROVEN_OK) return;
    char head[1024];
    proven_size_t n = 0;
    while (n < sizeof head - 1) {
        proven_result_size_t r = proven_net_read(&c, (proven_mem_mut_t){ .ptr = (proven_byte_t *)head + n, .size = 1 }, proven_net_deadline_in(10000));
        if (r.err != PROVEN_OK) break;
        n += r.value;
        if (n >= 4 && memcmp(head + n - 4, "\r\n\r\n", 4) == 0) break;
    }
    (void)proven_net_write_all(&c, mv("HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Encoding: gzip\r\n\r\n"), proven_net_deadline_in(10000));
    (void)proven_net_write_all(&c, (proven_mem_view_t){ .ptr = p->vector->bytes, .size = p->vector->size }, proven_net_deadline_in(10000));
    (void)proven_net_shutdown_write(&c);
    /* Wait for the client to go, so that the close is a close and not a reset. */
    (void)proven_net_read(&c, (proven_mem_mut_t){ .ptr = (proven_byte_t *)head, .size = sizeof head }, proven_net_deadline_in(10000));
    (void)proven_net_close(&c);
}

static void peer_start(peer_t *p, const coding_vector_t *v) {
    proven_net_addr_t at;
    p->vector = v;
    PROVEN_TEST_ASSERT(proven_net_listen(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), 1, &p->listener, &at) == PROVEN_OK, "a scripted peer listens", "");
    p->port = at.port;
    proven_job_group_init(&g_peer_group);
    PROVEN_TEST_ASSERT(proven_job_group_submit(g_peer_jobs, &g_peer_group, peer_script, p) == PROVEN_OK, "its script is started", "");
}
static void peer_finish(peer_t *p) {
    proven_job_group_wait(g_peer_jobs, &g_peer_group);
    (void)proven_net_listener_close(&p->listener);
}

/* A gzip body with no length of its own, whole and then cut short, through one client. */
static void until_close(fetch_fn fetch, void *client) {
    peer_t p;
    got_t g = { 0 };
    peer_start(&p, &coding_vectors[0]);
    fetch(client, false, p.port, "GET", "/", &g);
    peer_finish(&p);
    PROVEN_TEST_ASSERT(g.status == 200 && g.err == PROVEN_OK && vector_plain_is(&coding_vectors[0], g_got, g.size), "a gzip body that runs until the server closes is decoded whole", "Check the end-of-stream branch of the client.");
    PROVEN_TEST_ASSERT(strcmp(coding_vectors[10].name, "gzip cut short") == 0, "the vector that is cut short", "");
    peer_start(&p, &coding_vectors[10]);
    fetch(client, false, p.port, "GET", "/", &g);
    peer_finish(&p);
    PROVEN_TEST_ASSERT(g.status == 200 && g.err == PROVEN_ERR_INVALID_FORMAT && g.lasts == 0, "and one that the close cuts short is PROVEN_ERR_INVALID_FORMAT: the close ends the HTTP body, not the gzip stream",
        "Check proven_http_decoder_end_ at the peer's close: cl_read_decoded, and the end-of-stream branch of cl_process.");
}

// ---------------------------------------------------------------------------------------
// The cases that are the same for every pair of drivers
// ---------------------------------------------------------------------------------------


static void fetch_blocking(void *client, bool tls, proven_u16 port, const char *method, const char *path, got_t *g) {
    (void)tls;
    bfetch(client, method, port, path, NULL, 0, 4096, g);
}
static void fetch_event(void *client, bool tls, proven_u16 port, const char *method, const char *path, got_t *g) {
    efetch(client, method, tls, port, path, NULL, 0, false, g);
}

/* A server that compresses, seen through a client that decodes. */
static void compressed_responses(fetch_fn fetch, void *client, bool tls, proven_u16 port) {
    static const proven_size_t sizes[] = { 0, 1, 255, 256, 3000, 65536, MIB };
    got_t g = { 0 };
    char path[32];
    for (proven_size_t i = 0; i < sizeof sizes / sizeof sizes[0]; ++i) {
        proven_size_t n = sizes[i];
        snprintf(path, sizeof path, "/r/%u", (unsigned)n);
        fetch(client, tls, port, "GET", path, &g);
        PROVEN_TEST_ASSERT(g.status == 200 && text_is(&g, n), "a body sent with respond arrives as it was written, at every size", "Check the respond function of the server, and proven_http_compress_all_.");
        PROVEN_TEST_ASSERT((strcmp(g.encoding, "gzip") == 0) == (n >= 256), "compressed from 256 bytes up, and sent as it is below that", "");
        PROVEN_TEST_ASSERT(strcmp(g.vary, "Accept-Encoding") == 0 && strcmp(g.accept, "gzip") == 0, "with Vary: Accept-Encoding either way, in answer to Accept-Encoding: gzip", "");
        if (n >= 256) PROVEN_TEST_ASSERT((proven_size_t)atol(g.length) < n && g.transfer[0] == '\0', "and a Content-Length that is the compressed size", "");

        snprintf(path, sizeof path, "/s/%u", (unsigned)n);
        fetch(client, tls, port, "GET", path, &g);
        PROVEN_TEST_ASSERT(g.status == 200 && text_is(&g, n), "a body streamed with its length arrives whole", "Check the write and end functions of the server.");
        PROVEN_TEST_ASSERT(strcmp(g.encoding, "gzip") == 0 && strcmp(g.transfer, "chunked") == 0 && g.length[0] == '\0', "compressed and chunked: the length given is not the length sent", "");

        snprintf(path, sizeof path, "/u/%u", (unsigned)n);
        fetch(client, tls, port, "GET", path, &g);
        PROVEN_TEST_ASSERT(g.status == 200 && text_is(&g, n) && strcmp(g.encoding, "gzip") == 0, "and so does one streamed with no length", "");
    }
    fetch(client, tls, port, "GET", "/x/3000", &g);
    PROVEN_TEST_ASSERT(g.err == PROVEN_OK && g.size == 3000 && memcmp(g_got, g_noise, 3000) == 0 && g.encoding[0] == '\0' && strcmp(g.length, "3000") == 0 &&
                       strcmp(g.vary, "Accept-Encoding") == 0, "bytes that do not get smaller are sent as they are, still with Vary", "");
    fetch(client, tls, port, "GET", "/x/60000", &g);
    PROVEN_TEST_ASSERT(g.err == PROVEN_OK && g.size == 60000 && memcmp(g_got, g_noise, 60000) == 0 && g.encoding[0] == '\0', "at 60,000 bytes too", "");
    fetch(client, tls, port, "GET", "/p/3000", &g);
    PROVEN_TEST_ASSERT(text_is(&g, 3000) && g.encoding[0] == '\0' && g.vary_fields == 0, "a response that did not ask is not compressed and says nothing of Vary", "");
    fetch(client, tls, port, "GET", "/own", &g);
    PROVEN_TEST_ASSERT(text_is(&g, 3000) && strcmp(g.encoding, "br") == 0, "a handler's own Content-Encoding is left alone, body and header", "");
    fetch(client, tls, port, "GET", "/204", &g);
    PROVEN_TEST_ASSERT(g.err == PROVEN_OK && g.status == 204 && g.size == 0 && g.encoding[0] == '\0', "a 204 is not compressed", "");
    fetch(client, tls, port, "GET", "/range", &g);
    PROVEN_TEST_ASSERT(g.status == 206 && text_is(&g, 1000) && g.encoding[0] == '\0', "nor is a 206 with its Content-Range", "");
    fetch(client, tls, port, "GET", "/206", &g);
    PROVEN_TEST_ASSERT(g.status == 206 && text_is(&g, 1000) && g.encoding[0] == '\0', "or without it: the status is enough", "Check proven_http_compress_applies_.");
    fetch(client, tls, port, "GET", "/vary", &g);
    PROVEN_TEST_ASSERT(text_is(&g, 3000) && strcmp(g.encoding, "gzip") == 0 && g.vary_fields == 1 && strcmp(g.vary, "Origin, accept-encoding") == 0,
        "a Vary that already names Accept-Encoding is not given a second field", "Check proven_http_compress_needs_vary_.");
    fetch(client, tls, port, "GET", "/late", &g);
    PROVEN_TEST_ASSERT(text_is(&g, 3000) && g.encoding[0] == '\0' && strcmp(g.length, "3000") == 0, "asking after the response has begun does nothing", "");
    fetch(client, tls, port, "GET", "/short", &g);
    PROVEN_TEST_ASSERT(g.err != PROVEN_OK && g.lasts == 0, "a compressed response that ends short of its promise does not pass for a whole one", "Check the end function of the server: no last chunk and no end of the gzip stream.");

    got_t h = { 0 };
    fetch(client, tls, port, "GET", "/r/3000", &g);
    fetch(client, tls, port, "HEAD", "/r/3000", &h);
    PROVEN_TEST_ASSERT(h.err == PROVEN_OK && h.size == 0 && strcmp(h.encoding, "gzip") == 0 && strcmp(h.length, g.length) == 0 && h.length[0] != '\0',
        "HEAD announces the encoding and the very length that GET sends", "");
    fetch(client, tls, port, "HEAD", "/s/3000", &h);
    PROVEN_TEST_ASSERT(h.err == PROVEN_OK && h.size == 0 && strcmp(h.encoding, "gzip") == 0 && strcmp(h.transfer, "chunked") == 0, "and for a streamed response, that it is chunked", "");
}

/* A client that decodes, shown every vector by the blocking server. */
static void decoded_vectors(fetch_fn fetch, void *client, proven_u16 port) {
    char path[32];
    got_t g = { 0 };
    for (proven_size_t i = 0; i < VECTOR_COUNT; ++i) {
        const coding_vector_t *v = &coding_vectors[i];
        for (int streamed = 0; streamed < 2; ++streamed) {
            if (streamed && v->size > 4096) continue;
            snprintf(path, sizeof path, "/%c/%u", streamed ? 'w' : 'v', (unsigned)i);
            fetch(client, false, port, "GET", path, &g);
            PROVEN_TEST_ASSERT(g.status == 200, "the response arrives", v->name);
            PROVEN_TEST_ASSERT(g.vary_fields == 1 && strcmp(g.encoding, v->encoding) == 0, "with the header as the server wrote it", v->name);
            if (v->plain >= 0) {
                PROVEN_TEST_ASSERT(g.err == PROVEN_OK && vector_plain_is(v, g_got, g.size), "a body zlib made decodes to what zlib was given", v->name);
                if (!streamed) PROVEN_TEST_ASSERT((unsigned long)atol(g.length) == v->size, "and Content-Length still counts what was sent", v->name);
            } else if (v->plain == CODING_PLAIN_REFUSED) {
                PROVEN_TEST_ASSERT(g.err == PROVEN_ERR_INVALID_FORMAT && g.lasts == 0, "a body zlib refuses is PROVEN_ERR_INVALID_FORMAT, never a whole body", v->name);
            } else {
                PROVEN_TEST_ASSERT(g.err == PROVEN_OK && g.size == v->size && memcmp(g_got, v->bytes, v->size) == 0, "a coding that is not decoded arrives as it was sent", v->name);
            }
        }
    }
}

// ---------------------------------------------------------------------------------------

int main(void) {
    PROVEN_TEST_SUITE("http: content codings",
        "Responses compressed when a handler asks and the client accepts, and compressed bodies decoded by the clients that ask - in all four drivers, against bodies zlib made.",
        "Inspect src/proven/http_coding.c for the rules, and the respond, begin, write and end functions of the two servers and the body paths of the two clients for their use.");

    g_heap = proven_heap_allocator();
    fill_bodies();

    PROVEN_TEST_SECTION("Accept-Encoding and Content-Encoding", "The two helpers of http.h, against tables.", "Check proven_http_accepts_coding and proven_http_content_coding in src/proven/http.c.");
    {
        static const struct { const char *value; bool gzip, deflate, identity; } rows[] = {
            { NULL, false, false, true },                          /* no field: nothing but identity */
            { "", false, false, true },
            { "gzip", true, false, true },
            { "GZip", true, false, true },
            { "x-gzip", true, false, true },
            { "gzip, deflate, br", true, true, true },
            { "deflate", false, true, true },
            { "br", false, false, true },
            { "gzip;q=0", false, false, true },
            { "gzip;q=0.0", false, false, true },
            { "gzip;q=0.000", false, false, true },
            { "gzip;q=0.001", true, false, true },
            { "gzip; q=1", true, false, true },
            { "gzip ; q=1.000", true, false, true },
            { "gzip;Q=0.5", true, false, true },
            { "*", true, true, true },
            { "*;q=0", false, false, false },
            { "*;q=0, gzip", true, false, false },
            { "*;q=0, identity", false, false, true },
            { "gzip;q=0, *", false, true, true },
            { "identity;q=0", false, false, false },
            { "identity;q=0, gzip", true, false, false },
            { "identity", false, false, true },
            { "deflate, gzip;q=0.5", true, true, true },
            { " gzip ,deflate ", true, true, true },
            { "gzip,,deflate", true, true, true },
            { "gzip;q=2", false, false, true },                    /* malformed: identity only */
            { "gzip;q=1.001", false, false, true },
            { "gzip;q=0.0000", false, false, true },
            { "gzip;q=", false, false, true },
            { "gzip;q=.5", false, false, true },
            { "gzip;x=1", false, false, true },
            { "gzip;q=0.5x", false, false, true },
            { "deflate, gzip;q=one", false, false, true },
        };
        for (proven_size_t i = 0; i < sizeof rows / sizeof rows[0]; ++i) {
            proven_http_header_t h[2] = { { PROVEN_LIT("Host"), PROVEN_LIT("x") }, { PROVEN_LIT("accept-ENCODING"), sv(rows[i].value ? rows[i].value : "") } };
            proven_size_t n = rows[i].value ? 2 : 1;
            PROVEN_TEST_ASSERT(proven_http_accepts_coding(h, n, PROVEN_HTTP_CODING_GZIP) == rows[i].gzip, "gzip is accepted exactly when the field says so", rows[i].value ? rows[i].value : "(no field)");
            PROVEN_TEST_ASSERT(proven_http_accepts_coding(h, n, PROVEN_HTTP_CODING_DEFLATE) == rows[i].deflate, "and deflate", rows[i].value ? rows[i].value : "(no field)");
            PROVEN_TEST_ASSERT(proven_http_accepts_coding(h, n, PROVEN_HTTP_CODING_IDENTITY) == rows[i].identity, "and identity", rows[i].value ? rows[i].value : "(no field)");
            PROVEN_TEST_ASSERT(!proven_http_accepts_coding(h, n, PROVEN_HTTP_CODING_OTHER), "PROVEN_HTTP_CODING_OTHER is never accepted", "");
        }
        proven_http_header_t two[2] = { { PROVEN_LIT("Accept-Encoding"), PROVEN_LIT("br") }, { PROVEN_LIT("Accept-Encoding"), PROVEN_LIT("gzip;q=0.2") } };
        PROVEN_TEST_ASSERT(proven_http_accepts_coding(two, 2, PROVEN_HTTP_CODING_GZIP) && !proven_http_accepts_coding(two, 2, PROVEN_HTTP_CODING_DEFLATE), "two fields are read as one list", "");
        PROVEN_TEST_ASSERT(proven_http_accepts_coding(NULL, 0, PROVEN_HTTP_CODING_IDENTITY) && !proven_http_accepts_coding(NULL, 0, PROVEN_HTTP_CODING_GZIP), "no headers at all: identity", "");

        static const struct { const char *value; proven_http_coding_t coding; } codings[] = {
            { NULL, PROVEN_HTTP_CODING_IDENTITY }, { "", PROVEN_HTTP_CODING_IDENTITY }, { "identity", PROVEN_HTTP_CODING_IDENTITY },
            { "gzip", PROVEN_HTTP_CODING_GZIP }, { "GZIP", PROVEN_HTTP_CODING_GZIP }, { "x-gzip", PROVEN_HTTP_CODING_GZIP }, { " gzip ", PROVEN_HTTP_CODING_GZIP },
            { "deflate", PROVEN_HTTP_CODING_DEFLATE }, { "br", PROVEN_HTTP_CODING_OTHER }, { "zstd", PROVEN_HTTP_CODING_OTHER },
            { "gzip, gzip", PROVEN_HTTP_CODING_OTHER }, { "deflate, gzip", PROVEN_HTTP_CODING_OTHER }, { "gzip;q=1", PROVEN_HTTP_CODING_OTHER },
        };
        for (proven_size_t i = 0; i < sizeof codings / sizeof codings[0]; ++i) {
            proven_http_header_t h = { PROVEN_LIT("content-encoding"), sv(codings[i].value ? codings[i].value : "") };
            PROVEN_TEST_ASSERT(proven_http_content_coding(&h, codings[i].value ? 1 : 0) == codings[i].coding, "Content-Encoding names the coding", codings[i].value ? codings[i].value : "(no field)");
        }
        proven_http_header_t twice[2] = { { PROVEN_LIT("Content-Encoding"), PROVEN_LIT("gzip") }, { PROVEN_LIT("Content-Encoding"), PROVEN_LIT("gzip") } };
        PROVEN_TEST_ASSERT(proven_http_content_coding(twice, 2) == PROVEN_HTTP_CODING_OTHER, "two fields are more than one coding", "");
    }

    PROVEN_TEST_SECTION("the decoder", "What the two clients share, against bodies zlib made: whole, and a byte in and a byte out at a time.", "Check proven_http_decoder_step_ and proven_http_decoder_end_ in src/proven/http_coding.c.");
    {
        for (proven_size_t i = 0; i < VECTOR_COUNT; ++i) {
            const coding_vector_t *v = &coding_vectors[i];
            proven_http_header_t h = { PROVEN_LIT("Content-Encoding"), sv(v->encoding) };
            proven_http_coding_t coding = PROVEN_HTTP_CODING_IDENTITY;
            bool wanted = proven_http_decoder_wanted_(200, &h, 1, &coding);
            PROVEN_TEST_ASSERT(wanted == (v->plain != CODING_PLAIN_AS_SENT), "a body is decoded exactly when its coding is gzip or deflate alone", v->name);
            PROVEN_TEST_ASSERT(!proven_http_decoder_wanted_(206, &h, 1, &coding), "and never in a 206", v->name);
            if (!wanted) continue;
            for (int bytewise = 0; bytewise < 2; ++bytewise) {
                proven_http_decoder_t_ d;
                proven_http_decoder_begin_(&d, g_heap, coding);
                proven_size_t in = 0, out = 0;
                proven_err_t e = PROVEN_OK;
                for (;;) {
                    proven_size_t give = bytewise ? (v->size - in > 0 ? 1 : 0) : v->size - in;
                    proven_size_t room = bytewise ? 1 : sizeof g_got - out;
                    proven_size_t used = 0, made = 0;
                    e = proven_http_decoder_step_(&d, (proven_mem_view_t){ .ptr = v->bytes + in, .size = give }, &used, (proven_mem_mut_t){ .ptr = g_got + out, .size = room }, &made);
                    if (e != PROVEN_OK) break;
                    in += used;
                    out += made;
                    if (used == 0 && made == 0 && in == v->size) break;
                    PROVEN_TEST_ASSERT(used > 0 || made > 0, "a step with input to take or room to fill does one or the other", v->name);
                }
                if (e == PROVEN_OK) e = proven_http_decoder_end_(&d);
                proven_http_decoder_free_(&d);
                if (v->plain >= 0) PROVEN_TEST_ASSERT(e == PROVEN_OK && vector_plain_is(v, g_got, out), "what zlib made decodes to what zlib was given", v->name);
                else PROVEN_TEST_ASSERT(e == PROVEN_ERR_INVALID_FORMAT, "what zlib refuses is PROVEN_ERR_INVALID_FORMAT", v->name);
            }
        }
        proven_http_decoder_t_ d;
        proven_http_decoder_begin_(&d, g_heap, PROVEN_HTTP_CODING_GZIP);
        PROVEN_TEST_ASSERT(proven_http_decoder_end_(&d) == PROVEN_OK, "a body of no bytes at all is an empty body, not a damaged one", "");
        proven_http_decoder_free_(&d);

        proven_byte_t *packed = NULL;
        proven_size_t cap = 0, size = 0;
        PROVEN_TEST_ASSERT(proven_http_compress_all_(g_heap, 0, 0, (proven_mem_view_t){ g_text, 255 }, &packed, &cap, &size) == PROVEN_OK && packed == NULL, "a body under 256 bytes is not compressed", "");
        PROVEN_TEST_ASSERT(proven_http_compress_all_(g_heap, 0, 0, (proven_mem_view_t){ g_noise, 3000 }, &packed, &cap, &size) == PROVEN_OK && packed == NULL, "nor one that would not get smaller", "");
        for (int bits = 0; bits <= 15; bits = bits == 0 ? 9 : bits + 3) {
            PROVEN_TEST_ASSERT(proven_http_compress_all_(g_heap, 0, bits, (proven_mem_view_t){ g_text, 100000 }, &packed, &cap, &size) == PROVEN_OK && packed != NULL && size < 100000 / 4,
                "text is, to less than a quarter, at any window", "");
            proven_size_t written = 0, consumed = 0;
            PROVEN_TEST_ASSERT(proven_inflate_all(g_heap, PROVEN_DEFLATE_GZIP, (proven_mem_view_t){ packed, size }, (proven_mem_mut_t){ g_got, sizeof g_got }, &written, &consumed) == PROVEN_OK &&
                               written == 100000 && consumed == size && memcmp(g_got, g_text, written) == 0, "and inflates to itself", "");
            g_heap.free_fn(g_heap.ctx, packed);
        }
    }

    {
        proven_net_listener_t probe;
        proven_err_t err = proven_net_listen(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), 1, &probe, NULL);
        if (err == PROVEN_ERR_PERMISSION || err == PROVEN_ERR_UNSUPPORTED) {
            PROVEN_TEST_INFO("SKIP: this environment refuses to open a listening socket (error {}).", PROVEN_ARG((int)err));
            PROVEN_TEST_PASS("the helpers and the decoder hold; the drivers were skipped: no sockets here.");
            return 0;
        }
        PROVEN_TEST_ASSERT(err == PROVEN_OK, "a loopback listener on a free port opens", "");
        (void)proven_net_listener_close(&probe);
    }

    PROVEN_TEST_ASSERT(tp_build(), "the test's certificates and keys are issued", "");
    proven_cert_store_t *anchors = NULL;
    PROVEN_TEST_ASSERT(proven_cert_store_create(g_heap, &anchors) == PROVEN_OK &&
                       proven_cert_store_add_pem(anchors, (proven_mem_view_t){ (const proven_byte_t *)TP_CA, strlen(TP_CA) }, NULL) == PROVEN_OK, "anchors", "");
    proven_tls_options_t co = { .alloc = g_heap, .anchors = anchors };
    proven_tls_options_t so = { .alloc = g_heap, .certificate_pem = { (const proven_byte_t *)TP_SERVER_ED, strlen(TP_SERVER_ED) }, .private_key_pem = { (const proven_byte_t *)TP_SERVER_ED_KEY, strlen(TP_SERVER_ED_KEY) } };
    proven_tls_config_t *client_tls = NULL, *server_tls = NULL;
    PROVEN_TEST_ASSERT(proven_tls_config_create(&co, &client_tls) == PROVEN_OK && proven_tls_config_create(&so, &server_tls) == PROVEN_OK, "TLS configurations", "");
    PROVEN_TEST_ASSERT(proven_loop_create(g_heap, &g_loop) == PROVEN_OK, "a loop", "");

    /* The blocking server, on a thread of its own. */
    proven_job_sys_t *server_jobs = NULL;
    PROVEN_TEST_ASSERT(proven_job_system_init(g_heap, 1, 4, &server_jobs) == PROVEN_OK, "a thread for the blocking server", "");
    PROVEN_TEST_ASSERT(proven_job_system_init(g_heap, 1, 4, &g_peer_jobs) == PROVEN_OK, "and one for a scripted peer", "");
    proven_http_server_t *bserver = NULL;
    proven_http_server_config_t bcfg = { .alloc = g_heap, .handler = blocking_handler };
    proven_net_addr_t bat;
    PROVEN_TEST_ASSERT(proven_http_server_create(&bcfg, &bserver) == PROVEN_OK &&
                       proven_http_server_listen(bserver, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &bat) == PROVEN_OK, "the blocking server listens", "");
    proven_job_group_t server_group;
    proven_job_group_init(&server_group);
    PROVEN_TEST_ASSERT(proven_job_group_submit(server_jobs, &server_group, blocking_serve, bserver) == PROVEN_OK, "its loop is started", "");

    proven_http_client_config_t plain_cfg = { .alloc = g_heap, .max_idle_connections = 4 };
    proven_http_client_config_t decoding_cfg = { .alloc = g_heap, .max_idle_connections = 4, .decompress = true };
    proven_http_client_t *bplain = NULL, *bdecoding = NULL;
    PROVEN_TEST_ASSERT(proven_http_client_create(&plain_cfg, &bplain) == PROVEN_OK && proven_http_client_create(&decoding_cfg, &bdecoding) == PROVEN_OK, "a blocking client that decodes, and one that does not", "");
    proven_http_event_client_config_t edecoding_cfg = { .decompress = true, .tls = client_tls };
    proven_http_event_client_t *eplain = NULL, *edecoding = NULL;
    PROVEN_TEST_ASSERT(proven_http_event_client_create(g_loop, NULL, &eplain) == PROVEN_OK && proven_http_event_client_create(g_loop, &edecoding_cfg, &edecoding) == PROVEN_OK, "and the same pair of event-driven clients", "");

    got_t g = { 0 };

    PROVEN_TEST_SECTION("settings", "What is refused when a server is made, and what the calls do with nothing.", "");
    {
        proven_http_server_t *bad = NULL;
        proven_http_event_server_t *ebad = NULL;
        static const int levels[] = { -2, 10 }, windows[] = { 8, 16, -1, 1 };
        for (int i = 0; i < 2; ++i) {
            proven_http_server_config_t b = { .alloc = g_heap, .handler = blocking_handler, .compress_level = levels[i] };
            proven_http_event_server_config_t ev = { .on = { .on_request = event_request }, .compress_level = levels[i] };
            PROVEN_TEST_ASSERT(proven_http_server_create(&b, &bad) == PROVEN_ERR_INVALID_ARG && bad == NULL &&
                               proven_http_event_server_create(g_loop, &ev, &ebad) == PROVEN_ERR_INVALID_ARG && ebad == NULL, "a compression level outside -1..9 is PROVEN_ERR_INVALID_ARG", "");
        }
        for (int i = 0; i < 4; ++i) {
            proven_http_server_config_t b = { .alloc = g_heap, .handler = blocking_handler, .compress_window_bits = windows[i] };
            proven_http_event_server_config_t ev = { .on = { .on_request = event_request }, .compress_window_bits = windows[i] };
            PROVEN_TEST_ASSERT(proven_http_server_create(&b, &bad) == PROVEN_ERR_INVALID_ARG && bad == NULL &&
                               proven_http_event_server_create(g_loop, &ev, &ebad) == PROVEN_ERR_INVALID_ARG && ebad == NULL, "a window outside 9..15 is PROVEN_ERR_INVALID_ARG", "");
        }
        proven_http_exchange_compress(NULL);
        proven_http_stream_compress(NULL);
        PROVEN_TEST_ASSERT(true, "the two calls accept null", "");
    }

    PROVEN_TEST_SECTION("the blocking client and the blocking server", "A server that compresses and a client that decodes, each also seen without the other.", "Check src/proven/http_client.c (cl_read_decoded) and src/proven/http_server.c (sv_zip, the respond function).");
    {
        compressed_responses(fetch_blocking, bdecoding, false, bat.port);
        decoded_vectors(fetch_blocking, bdecoding, bat.port);

        until_close(fetch_blocking, bdecoding);

        /* What each side does alone. */
        bget(bplain, bat.port, "/r/3000", &g);
        PROVEN_TEST_ASSERT(text_is(&g, 3000) && g.encoding[0] == '\0' && strcmp(g.vary, "Accept-Encoding") == 0 && strcmp(g.accept, "(none)") == 0,
            "a client that sets nothing sends no Accept-Encoding and gets the body as it is, with Vary", "");
        bget(bplain, bat.port, "/v/0", &g);
        PROVEN_TEST_ASSERT(g.err == PROVEN_OK && g.size == coding_vectors[0].size && memcmp(g_got, coding_vectors[0].bytes, g.size) == 0, "and a gzip body reaches it as the gzip it is", "");

        proven_http_header_t own = { PROVEN_LIT("Accept-Encoding"), PROVEN_LIT("identity") };
        bfetch(bdecoding, "GET", bat.port, "/r/3000", &own, 1, 4096, &g);
        PROVEN_TEST_ASSERT(text_is(&g, 3000) && strcmp(g.accept, "identity") == 0 && g.encoding[0] == '\0', "a request's own Accept-Encoding is sent in place of the client's", "");
        proven_http_header_t range = { PROVEN_LIT("Range"), PROVEN_LIT("bytes=0-9") };
        bfetch(bdecoding, "GET", bat.port, "/r/3000", &range, 1, 4096, &g);
        PROVEN_TEST_ASSERT(text_is(&g, 3000) && strcmp(g.accept, "(none)") == 0, "and with a Range, none is sent", "");

        /* The caller's buffer is the bound. */
        for (proven_size_t read_size = 1; read_size <= 100000; read_size = read_size * 7 + 1) {
            bfetch(bdecoding, "GET", bat.port, "/u/65536", NULL, 0, read_size, &g);
            PROVEN_TEST_ASSERT(text_is(&g, 65536), "the body is the same whatever the size of the reads", "");
        }
        proven_http_client_response_t res;
        proven_u8str_t all = { 0 };
        PROVEN_TEST_ASSERT(proven_http_client_get(bdecoding, url_at(false, bat.port, "/v/9"), &res) == PROVEN_OK, "two megabytes of zeros in two kilobytes of gzip", "");
        proven_err_t e = proven_http_client_read_all(&res, g_heap, &all, 100000);
        PROVEN_TEST_ASSERT(e == PROVEN_ERR_OUT_OF_BOUNDS && proven_u8str_as_view(&all).size == 100000 && all_zero(proven_u8str_as_view(&all).ptr, 100000), "read_all stops at its limit of decoded bytes: PROVEN_ERR_OUT_OF_BOUNDS", "Check proven_http_client_read_all.");
        proven_http_client_finish(&res);
        proven_u8str_destroy(g_heap, &all);

        /* The connection is reusable when the HTTP body is over, decoded or not. */
        char first[16];
        bget(bdecoding, bat.port, "/r/3000", &g);
        memcpy(first, g.peer, sizeof first);
        bget(bdecoding, bat.port, "/s/65536", &g);
        PROVEN_TEST_ASSERT(text_is(&g, 65536) && strcmp(first, g.peer) == 0, "after a decoded body the connection carries the next request", "Check cl_read_decoded: EOF is the end of the HTTP body.");
        bget(bdecoding, bat.port, "/v/3", &g);
        PROVEN_TEST_ASSERT(g.err == PROVEN_OK && strcmp(first, g.peer) == 0, "and the next, after two gzip members", "");
    }

    PROVEN_TEST_SECTION("the event-driven client and the blocking server", "The same server, and the client that is given pieces.", "Check cl_decode in src/proven/http_event_client.c.");
    {
        compressed_responses(fetch_event, edecoding, false, bat.port);
        decoded_vectors(fetch_event, edecoding, bat.port);

        until_close(fetch_event, edecoding);

        eget(eplain, false, bat.port, "/v/0", &g);
        PROVEN_TEST_ASSERT(g.err == PROVEN_OK && strcmp(g.accept, "(none)") == 0 && g.size == coding_vectors[0].size && memcmp(g_got, coding_vectors[0].bytes, g.size) == 0,
            "a client that sets nothing sends no Accept-Encoding and is given the gzip as it is", "");
        proven_http_header_t range = { PROVEN_LIT("Range"), PROVEN_LIT("bytes=0-9") };
        efetch(edecoding, "GET", false, bat.port, "/r/3000", &range, 1, false, &g);
        PROVEN_TEST_ASSERT(text_is(&g, 3000) && strcmp(g.accept, "(none)") == 0, "with a Range, no Accept-Encoding is sent", "");

        eget(edecoding, false, bat.port, "/v/9", &g);
        PROVEN_TEST_ASSERT(g.err == PROVEN_OK && g.size == 2 * MIB && all_zero(g_got, g.size) && g.lasts == 1, "two megabytes of zeros arrive from two kilobytes", "");
        PROVEN_TEST_ASSERT(g.largest_piece <= 16384 && g.pieces >= 128, "in pieces of at most 16 KiB", "");

        efetch(edecoding, "GET", false, bat.port, "/v/9", NULL, 0, true, &g);
        PROVEN_TEST_ASSERT(g.err == PROVEN_OK && g.size == 2 * MIB && all_zero(g_got, g.size) && g.lasts == 1 && !g.while_paused,
            "paused after every piece, nothing arrives while paused and nothing is lost", "Check cl_decode: the payload stays in the stash until it is all decoded.");
        efetch(edecoding, "GET", false, bat.port, "/w/3", NULL, 0, true, &g);
        PROVEN_TEST_ASSERT(g.err == PROVEN_OK && vector_plain_is(&coding_vectors[3], g_got, g.size) && !g.while_paused, "the same for two members that arrive a byte a chunk", "");

        proven_http_event_client_config_t bounded_cfg = { .decompress = true, .max_body_bytes = 100000 };
        proven_http_event_client_t *bounded = NULL;
        PROVEN_TEST_ASSERT(proven_http_event_client_create(g_loop, &bounded_cfg, &bounded) == PROVEN_OK, "a client with max_body_bytes of 100,000", "");
        eget(bounded, false, bat.port, "/v/9", &g);
        PROVEN_TEST_ASSERT(g.err == PROVEN_ERR_OUT_OF_BOUNDS && g.size <= 100000 && g.lasts == 0, "the limit counts decoded bytes: PROVEN_ERR_OUT_OF_BOUNDS, and no more than the limit delivered", "");
        eget(bounded, false, bat.port, "/r/65536", &g);
        PROVEN_TEST_ASSERT(text_is(&g, 65536), "a body within it passes", "");
        proven_http_event_client_destroy(bounded);
    }

    for (int tls = 0; tls < 2; ++tls) {
        PROVEN_TEST_SECTION(tls ? "the event-driven server and client, over TLS" : "the event-driven server and client",
            "The server on the loop, compressing into its bounded output queue.", "Check ev_zip and the write function in src/proven/http_event.c.");
        proven_http_event_server_t *eserver = NULL;
        proven_http_event_server_config_t ecfg = { .on = { event_request, NULL, event_writable, event_done }, .tls = tls ? server_tls : NULL };
        proven_net_addr_t eat;
        PROVEN_TEST_ASSERT(proven_http_event_server_create(g_loop, &ecfg, &eserver) == PROVEN_OK &&
                           proven_http_event_server_listen(eserver, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &eat) == PROVEN_OK, "the server listens", "");
        g_most_buffered = 0;
        g_writable_calls = 0;
        compressed_responses(fetch_event, edecoding, tls != 0, eat.port);

        /* Bytes that do not compress, to a client that stops reading for a while: the queue
         * fills, writes are refused, and on_writable brings the rest. */
        g.noise = true;
        g.pause_once_ms = 200;
        efetch(edecoding, "GET", tls != 0, eat.port, "/n/16777216", NULL, 0, false, &g);
        PROVEN_TEST_ASSERT(g.err == PROVEN_OK && g.size == 16 * MIB && g.noise_intact && !g.while_paused && strcmp(g.encoding, "gzip") == 0,
            "sixteen megabytes that do not compress, to a client that stops reading for a while, arrive whole", "");
        g.noise = false;
        PROVEN_TEST_ASSERT(g_writable_calls > 0, "the server refused writes on the way and said when to go on", "Check the zip branch of proven_http_stream_write and ev_progress.");
        PROVEN_TEST_ASSERT(g_most_buffered > 32 * 1024 && g_most_buffered <= 64 * 1024 + 16384 + 1024, "and its output queue passed its limit by no more than what one piece compresses to", "Check the zip branch of proven_http_stream_write.");
        efetch(edecoding, "GET", tls != 0, eat.port, "/u/1048576", NULL, 0, true, &g);
        /* Compressed, this is a few dozen kilobytes: the server has sent it all and closed
         * while the client is still paused over the second piece. */
        PROVEN_TEST_ASSERT(text_is(&g, MIB) && !g.while_paused, "and so does a megabyte of text, which the server has finished sending long before the client has finished reading",
            "Check cl_readable in src/proven/http_event_client.c: a hang-up reported after the end was seen is not a cut connection.");

        if (!tls) {
            PROVEN_TEST_SECTION("raw sockets", "What a client of this library never sends, to both servers.", "");
            for (int which = 0; which < 2; ++which) {
                proven_u16 port = which ? eat.port : bat.port;
                const char *server = which ? "the event-driven server" : "the blocking server";
                PROVEN_TEST_ASSERT(raw(port, "GET /s/3000 HTTP/1.0\r\nAccept-Encoding: gzip\r\n\r\n"), "an HTTP/1.0 request is answered and the connection closed", server);
                proven_size_t written = 0, consumed = 0;
                PROVEN_TEST_ASSERT(raw_has("Content-Encoding: gzip\r\n") && !raw_has("Transfer-Encoding") && !raw_has("Content-Length") && raw_has("Connection: close\r\n"),
                    "compressed, with no chunks and no length: the body runs to the close", server);
                PROVEN_TEST_ASSERT(proven_inflate_all(g_heap, PROVEN_DEFLATE_GZIP, (proven_mem_view_t){ (const proven_byte_t *)g_raw_body, g_raw_body_len }, (proven_mem_mut_t){ g_got, sizeof g_got }, &written, &consumed) == PROVEN_OK &&
                                   written == 3000 && consumed == g_raw_body_len && memcmp(g_got, g_text, 3000) == 0, "and is one whole gzip stream", server);

                static const struct { const char *accept; bool gzip; } rows[] = {
                    { "Accept-Encoding: gzip;q=0\r\n", false }, { "Accept-Encoding: identity\r\n", false }, { "Accept-Encoding: deflate, br\r\n", false }, { "", false },
                    { "Accept-Encoding: *\r\n", true }, { "Accept-Encoding: br;q=1.0, gzip;q=0.8\r\n", true }, { "Accept-Encoding: x-gzip\r\n", true },
                };
                for (proven_size_t i = 0; i < sizeof rows / sizeof rows[0]; ++i) {
                    char request[256];
                    for (int streamed = 0; streamed < 2; ++streamed) {
                        snprintf(request, sizeof request, "GET /%c/3000 HTTP/1.1\r\nHost: x\r\nConnection: close\r\n%s\r\n", streamed ? 's' : 'r', rows[i].accept);
                        PROVEN_TEST_ASSERT(raw(port, request) && raw_has("HTTP/1.1 200 OK\r\n") && raw_has("Vary: Accept-Encoding\r\n"), "answered, with Vary", rows[i].accept);
                        PROVEN_TEST_ASSERT(raw_has("Content-Encoding: gzip\r\n") == rows[i].gzip, "compressed exactly when Accept-Encoding allows gzip", rows[i].accept);
                        if (!rows[i].gzip) PROVEN_TEST_ASSERT(raw_has("Content-Length: 3000\r\n") && g_raw_body_len == 3000 && memcmp(g_raw_body, g_text, 3000) == 0, "and otherwise sent as it is, with its length", rows[i].accept);
                    }
                }
                PROVEN_TEST_ASSERT(raw(port, "HEAD /u/3000 HTTP/1.1\r\nHost: x\r\nConnection: close\r\nAccept-Encoding: gzip\r\n\r\n") && raw_has("Content-Encoding: gzip\r\n") && g_raw_body_len == 0,
                    "HEAD for a streamed response has the headers and not a byte of body", server);
            }
        }
        proven_http_event_server_destroy(eserver);
    }

    proven_http_event_client_destroy(eplain);
    proven_http_event_client_destroy(edecoding);
    proven_http_client_destroy(bplain);
    proven_http_client_destroy(bdecoding);
    proven_http_server_stop(bserver);
    proven_job_group_wait(server_jobs, &server_group);
    proven_http_server_destroy(bserver);
    proven_job_system_close(server_jobs);
    proven_job_system_destroy(server_jobs);
    proven_job_system_close(g_peer_jobs);
    proven_job_system_destroy(g_peer_jobs);
    proven_loop_destroy(g_loop);
    proven_tls_config_destroy(client_tls);
    proven_tls_config_destroy(server_tls);
    proven_cert_store_destroy(anchors);
    PROVEN_TEST_PASS("responses are compressed when asked for and accepted, and compressed bodies are decoded by the clients that ask.");
    return 0;
}
