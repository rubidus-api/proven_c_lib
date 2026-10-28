#include "example.h"
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
