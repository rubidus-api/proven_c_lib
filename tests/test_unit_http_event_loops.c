#include "proven.h"
#include "proven_test.h"
#include "test_unit_tls_pki.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Several loops behind one listening port.
 *
 * Each loop runs on a thread of its own and has a server of its own with no listener. One
 * more thread accepts, and deals each connection to the servers in turn: it posts a function
 * to that server's loop, and the function - on the loop's thread - calls
 * proven_http_event_server_adopt. No connection is ever touched by two threads.
 *
 * The order of taking it apart matters and is part of what is tested: the acceptor stops
 * first, so that nothing more is posted; then the loops; then the servers.
 */

static proven_mem_view_t mv(const char *s) { return (proven_mem_view_t){ .ptr = (const proven_byte_t *)s, .size = strlen(s) }; }
static proven_u8str_view_t sv(const char *s) { return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)s, .size = strlen(s) }; }

enum { LOOPS = 3 };

typedef struct {
    int index;
    proven_loop_t *loop;
    proven_http_event_server_t *server;
    atomic_int served, adopted, refused, wrong_thread;
    proven_u64 thread_mark;
} worker_t;

static proven_allocator_t g_heap;
static worker_t g_workers[LOOPS];
static proven_net_listener_t g_listener;
static atomic_bool g_stop_accepting;
static atomic_int g_accepted;
static proven_tls_config_t *g_server_tls, *g_client_tls;
static _Thread_local proven_u64 t_mark;

/* What travels from the accepting thread to a loop: the connection, in memory of its own. */
typedef struct { worker_t *to; proven_net_conn_t conn; } handoff_t;

/* On the loop's thread. */
static void adopt_here(void *ctx) {
    handoff_t *h = ctx;
    proven_err_t e = proven_http_event_server_adopt(h->to->server, &h->conn);
    if (e == PROVEN_OK) atomic_fetch_add(&h->to->adopted, 1);
    else { atomic_fetch_add(&h->to->refused, 1); (void)proven_net_close(&h->conn); }   /* still ours: close it */
    free(h);
}

/* The accepting thread. It waits a little at a time so that it can be told to stop. */
static void acceptor(void *arg) {
    (void)arg;
    int next = 0;
    while (!atomic_load(&g_stop_accepting)) {
        handoff_t *h = malloc(sizeof *h);
        if (!h) break;
        if (proven_net_accept(&g_listener, proven_net_deadline_in(20), &h->conn, NULL) != PROVEN_OK) { free(h); continue; }
        atomic_fetch_add(&g_accepted, 1);
        h->to = &g_workers[next];
        next = (next + 1) % LOOPS;
        if (proven_loop_post(h->to->loop, adopt_here, h) != PROVEN_OK) { (void)proven_net_close(&h->conn); free(h); }
    }
}

static void mark_thread(void *ctx) { worker_t *w = ctx; t_mark = (proven_u64)(w->index + 1) * 1000003u; w->thread_mark = t_mark; }

static void on_request(void *ctx, proven_http_stream_t *s, const proven_http_request_t *req) {
    worker_t *w = ctx;
    (void)req;
    if (t_mark != w->thread_mark) atomic_fetch_add(&w->wrong_thread, 1);
    char text[16];
    int n = snprintf(text, sizeof text, "loop %d", w->index);
    atomic_fetch_add(&w->served, 1);
    (void)proven_http_stream_respond(s, 200, NULL, 0, (proven_mem_view_t){ .ptr = (const proven_byte_t *)text, .size = (proven_size_t)n });
}

static void run_loop(void *arg) { (void)proven_loop_run(((worker_t *)arg)->loop); }

/* One request on a new connection; returns the index of the loop that answered, or -1. */
static int ask(bool tls, proven_u16 port) {
    proven_net_conn_t sock;
    if (proven_net_connect(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, port), proven_net_deadline_in(5000), &sock) != PROVEN_OK) return -1;
    proven_transport_t t = proven_net_conn_transport(&sock);
    if (tls && proven_tls_transport_client(t, g_client_tls, sv("127.0.0.1"), NULL, proven_net_deadline_in(10000), &t) != PROVEN_OK) { (void)proven_net_close(&sock); return -1; }
    char text[512] = { 0 };
    int which = -1;
    if (proven_transport_write_all(t, mv("GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"), proven_net_deadline_in(5000)).err == PROVEN_OK) {
        proven_size_t have = 0;
        for (;;) {
            proven_result_size_t got = proven_transport_read(t, (proven_mem_mut_t){ .ptr = (proven_byte_t *)text + have, .size = sizeof text - 1 - have }, proven_net_deadline_in(10000));
            if (got.err != PROVEN_OK) break;
            have += got.value;
        }
        const char *at = strstr(text, "\r\n\r\nloop ");
        if (strncmp(text, "HTTP/1.1 200", 12) == 0 && at) which = at[9] - '0';
    }
    (void)proven_transport_close(t);
    return which;
}

static void cases(bool tls) {
    const char *how = tls ? "over TLS" : "plain";
    proven_job_sys_t *threads = NULL;
    proven_job_group_t loops, accepting;
    proven_job_group_init(&loops);
    proven_job_group_init(&accepting);
    PROVEN_TEST_ASSERT(proven_job_system_init(g_heap, LOOPS + 1, 8, &threads) == PROVEN_OK, "a thread for each loop and one to accept", how);
    atomic_store(&g_stop_accepting, false);
    atomic_store(&g_accepted, 0);
    for (int i = 0; i < LOOPS; ++i) {
        worker_t *w = &g_workers[i];
        w->index = i; w->loop = NULL; w->server = NULL; w->thread_mark = 0;
        atomic_store(&w->served, 0); atomic_store(&w->adopted, 0); atomic_store(&w->refused, 0); atomic_store(&w->wrong_thread, 0);
        proven_http_event_server_config_t cfg = { .on = { .on_request = on_request }, .ctx = w, .tls = tls ? g_server_tls : NULL };
        PROVEN_TEST_ASSERT(proven_loop_create(g_heap, &w->loop) == PROVEN_OK && proven_http_event_server_create(w->loop, &cfg, &w->server) == PROVEN_OK &&
                           proven_loop_post(w->loop, mark_thread, w) == PROVEN_OK && proven_job_group_submit(threads, &loops, run_loop, w) == PROVEN_OK,
            "a loop on its own thread, with a server that has no listener", how);
    }
    proven_net_addr_t at;
    PROVEN_TEST_ASSERT(proven_net_listen(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), 128, &g_listener, &at) == PROVEN_OK &&
                       proven_job_group_submit(threads, &accepting, acceptor, NULL) == PROVEN_OK, "one listener, and a thread that deals out what it accepts", how);

    enum { REQUESTS = 90 };
    int by_loop[LOOPS] = { 0 }, answered = 0;
    for (int i = 0; i < REQUESTS; ++i) {
        int which = ask(tls, at.port);
        if (which >= 0 && which < LOOPS) { by_loop[which]++; answered++; }
    }
    PROVEN_TEST_ASSERT(answered == REQUESTS, "ninety requests on ninety connections are all answered", how);
    PROVEN_TEST_ASSERT(by_loop[0] == REQUESTS / LOOPS && by_loop[1] == REQUESTS / LOOPS && by_loop[2] == REQUESTS / LOOPS, "thirty by each of the three loops: the connections went where they were dealt", how);

    /* Taking it apart: the acceptor first, then the loops, then the servers. */
    atomic_store(&g_stop_accepting, true);
    proven_job_group_wait(threads, &accepting);
    (void)proven_net_listener_close(&g_listener);
    int served = 0, adopted = 0, refused = 0, wrong = 0;
    for (int i = 0; i < LOOPS; ++i) proven_loop_stop(g_workers[i].loop);
    proven_job_group_wait(threads, &loops);
    for (int i = 0; i < LOOPS; ++i) {
        worker_t *w = &g_workers[i];
        /* Whatever was posted and not yet run is run now, on this thread, before the server goes. */
        PROVEN_TEST_ASSERT(proven_loop_poll(w->loop, PROVEN_NET_DONT_WAIT) == PROVEN_OK, "a last round", how);
        served += atomic_load(&w->served); adopted += atomic_load(&w->adopted); refused += atomic_load(&w->refused); wrong += atomic_load(&w->wrong_thread);
        proven_http_event_server_destroy(w->server);
        proven_loop_destroy(w->loop);
    }
    PROVEN_TEST_ASSERT(adopted == atomic_load(&g_accepted) && refused == 0 && adopted == REQUESTS, "every accepted connection was adopted by a server", how);
    PROVEN_TEST_ASSERT(served == REQUESTS && wrong == 0, "and every request was handled on the thread of the loop it was dealt to", how);
    proven_job_system_close(threads);
    proven_job_system_destroy(threads);
}

static void quiet(void *ctx, proven_http_stream_t *s, const proven_http_request_t *req) { (void)ctx; (void)req; (void)proven_http_stream_respond(s, 200, NULL, 0, mv("ok")); }

int main(void) {
    PROVEN_TEST_SUITE("several loops behind one port",
        "Connections accepted on one thread and dealt to event-driven servers on three others with proven_http_event_server_adopt, plain and over TLS; and what adopt refuses.",
        "Inspect proven_http_event_server_adopt and ev_take in src/proven/http_event.c. Run under ThreadSanitizer: the point of the design is that no connection is touched by two threads.");

    g_heap = proven_heap_allocator();
    PROVEN_TEST_ASSERT(tp_build(), "the test's certificates and keys are issued", "");
    proven_cert_store_t *anchors = NULL;
    PROVEN_TEST_ASSERT(proven_cert_store_create(g_heap, &anchors) == PROVEN_OK &&
                       proven_cert_store_add_pem(anchors, (proven_mem_view_t){ (const proven_byte_t *)TP_CA, strlen(TP_CA) }, NULL) == PROVEN_OK, "anchors", "");
    proven_tls_options_t co = { .alloc = g_heap, .anchors = anchors };
    proven_tls_options_t so = { .alloc = g_heap, .certificate_pem = { (const proven_byte_t *)TP_SERVER_ED, strlen(TP_SERVER_ED) }, .private_key_pem = { (const proven_byte_t *)TP_SERVER_ED_KEY, strlen(TP_SERVER_ED_KEY) } };
    PROVEN_TEST_ASSERT(proven_tls_config_create(&co, &g_client_tls) == PROVEN_OK && proven_tls_config_create(&so, &g_server_tls) == PROVEN_OK, "TLS configurations", "");

    PROVEN_TEST_SECTION("what adopt refuses", "One thread, the loop driven by hand.", "Check proven_http_event_server_adopt.");
    {
        proven_loop_t *loop = NULL;
        proven_http_event_server_t *server = NULL;
        proven_http_event_server_config_t cfg = { .on = { .on_request = quiet }, .max_connections = 1 };
        proven_net_listener_t listener;
        proven_net_addr_t at;
        proven_net_conn_t far[3], near[3], closed = { 0 };
        PROVEN_TEST_ASSERT(proven_loop_create(g_heap, &loop) == PROVEN_OK && proven_http_event_server_create(loop, &cfg, &server) == PROVEN_OK &&
                           proven_net_listen(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), 8, &listener, &at) == PROVEN_OK, "a server that takes one connection, and a listener of the test's own", "");
        for (int i = 0; i < 3; ++i) {
            PROVEN_TEST_ASSERT(proven_net_connect(at, proven_net_deadline_in(5000), &far[i]) == PROVEN_OK &&
                               proven_net_accept(&listener, proven_net_deadline_in(5000), &near[i], NULL) == PROVEN_OK, "a connection accepted by the test", "");
        }
        PROVEN_TEST_ASSERT(proven_http_event_server_adopt(NULL, &near[0]) == PROVEN_ERR_INVALID_ARG && proven_http_event_server_adopt(server, NULL) == PROVEN_ERR_INVALID_ARG, "no server, no connection: PROVEN_ERR_INVALID_ARG", "");
        PROVEN_TEST_ASSERT(proven_http_event_server_adopt(server, &closed) == PROVEN_ERR_INVALID_STATE, "a connection that is not open: PROVEN_ERR_INVALID_STATE", "");
        PROVEN_TEST_ASSERT(proven_http_event_server_adopt(server, &near[0]) == PROVEN_OK && !proven_net_conn_is_open(&near[0]) && proven_http_event_server_connections(server) == 1,
            "adopted: the server has one connection, and the caller's value is no longer open", "");
        PROVEN_TEST_ASSERT(proven_http_event_server_adopt(server, &near[1]) == PROVEN_ERR_BUSY && proven_net_conn_is_open(&near[1]) && proven_http_event_server_connections(server) == 1,
            "at max_connections: PROVEN_ERR_BUSY, and the connection is still the caller's", "");
        /* The adopted connection is served like any other. */
        char text[128] = { 0 };
        PROVEN_TEST_ASSERT(proven_net_write_all(&far[0], mv("GET / HTTP/1.1\r\nHost: x\r\n\r\n"), proven_net_deadline_in(5000)).err == PROVEN_OK, "a request on the adopted connection", "");
        proven_net_deadline_t until = proven_net_deadline_in(2000);
        proven_size_t have = 0;
        while (proven_time_monotonic_now() < until && !strstr(text, "\r\n\r\nok")) {
            PROVEN_TEST_ASSERT(proven_loop_poll(loop, proven_net_deadline_in(10)) == PROVEN_OK, "a round", "");
            proven_result_size_t got = proven_net_read(&far[0], (proven_mem_mut_t){ .ptr = (proven_byte_t *)text + have, .size = sizeof text - 1 - have }, PROVEN_NET_DONT_WAIT);
            if (got.err == PROVEN_OK) have += got.value;
        }
        PROVEN_TEST_ASSERT(strncmp(text, "HTTP/1.1 200", 12) == 0 && strstr(text, "\r\n\r\nok"), "is answered", "");
        proven_http_event_server_stop_listening(server);
        PROVEN_TEST_ASSERT(proven_http_event_server_adopt(server, &near[2]) == PROVEN_ERR_INVALID_STATE && proven_net_conn_is_open(&near[2]), "after stop_listening: PROVEN_ERR_INVALID_STATE", "");
        PROVEN_TEST_ASSERT(proven_http_event_server_listen(server, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), NULL) == PROVEN_OK &&
                           proven_http_event_server_adopt(server, &near[2]) == PROVEN_ERR_BUSY, "a server that listens again takes connections again - here it is full: PROVEN_ERR_BUSY", "");
        for (int i = 0; i < 3; ++i) { (void)proven_net_close(&far[i]); (void)proven_net_close(&near[i]); }
        (void)proven_net_listener_close(&listener);
        proven_http_event_server_destroy(server);
        proven_loop_destroy(loop);
    }

    PROVEN_TEST_SECTION("three loops, plain", "Ninety connections dealt in turn.", "");
    cases(false);
    PROVEN_TEST_SECTION("three loops, over TLS", "The same, each connection's handshake done by the loop it was dealt to.", "");
    cases(true);

    proven_tls_config_destroy(g_client_tls); proven_tls_config_destroy(g_server_tls); proven_cert_store_destroy(anchors);
    PROVEN_TEST_PASS("connections dealt to several loops are served by the loop they were dealt to, and by no other thread.");
    return 0;
}
