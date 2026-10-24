#include "example.h"
#include <string.h>

/*
 * 루프 하나 위의 에코 서버와 그 클라이언트. 루프의 작업만 쓴다.
 *
 * 서버는 자기 함수 안에서 다시 거는 수락으로 연결을 받고, 연결마다 0바이트 수신으로 데이터를
 * 기다린다: 쉬는 연결은 읽기 버퍼를 쥐지 않는다. 데이터가 오면 루프의 scratch 버퍼로 읽어 되돌려
 * 보낸다. 소켓이 바로 받지 않은 것은 루프가 맡아 둔다. 클라이언트도 루프를 통해 연결하고, 타이머
 * 아래에서 자기 버퍼로 받는 수신으로 읽는다.
 */

#define MESSAGE "every operation is answered exactly once"
#define ROUNDS 3

typedef struct ex_app ex_app_t;

typedef struct {                /* 서버의 연결 하나 */
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
    /* 클라이언트 */
    proven_loop_sock_t sock;
    proven_net_conn_t conn;
    proven_loop_timer_t deadline;
    proven_byte_t buf[64];
    proven_size_t have;
    int rounds;
    bool echoed, client_closed, accepting_ended;
};

// ---- 서버의 연결 ---------------------------------------------------------------------

static void peer_end(ex_peer_t *p) {
    /* 클라이언트가 할 말을 다 했다: 보낼 것이 모두 나간 뒤에 닫는다. */
    p->ending = true;
    if (proven_loop_send_wait(p->app->loop, &p->sock, 0) != PROVEN_OK) proven_loop_close(p->app->loop, &p->sock);
}

/* 온 것을 읽어 되돌려 보낸다. 읽는 것보다 빨리 보내는 클라이언트는 루프가 맡아 둘 것을 끝없이
 * 늘리므로, 기다리는 것이 32 KiB를 넘으면 서버는 그것이 따라잡을 때까지 읽기를 멈춘다: 송신 대기를
 * 걸고, 읽기는 `writable`에서 다시 시작한다. */
static void peer_read(ex_peer_t *p) {
    proven_loop_t *loop = p->app->loop;
    proven_mem_mut_t scratch = proven_loop_scratch(loop);
    scratch.size = 16384;
    for (;;) {
        if (proven_loop_send_queued(&p->sock) > 32768) {
            if (proven_loop_send_wait(loop, &p->sock, 0) != PROVEN_OK) proven_loop_close(loop, &p->sock);
            return;
        }
        proven_result_size_t r = proven_net_read(&p->conn, scratch, PROVEN_NET_DONT_WAIT);
        if (r.err == PROVEN_ERR_TIMEOUT) break;                 /* 다 읽었다: 더 오기를 기다린다 */
        if (r.err != PROVEN_OK) { peer_end(p); return; }        /* PROVEN_ERR_EOF: 클라이언트가 끝냈다 */
        proven_result_size_t s = proven_loop_send(loop, &p->sock, (proven_mem_view_t){ scratch.ptr, r.value });
        if (s.err != PROVEN_OK) { proven_loop_close(loop, &p->sock); return; }
    }
    /* 이 연결은 기다리는 동안 아무것도 쥐지 않는다: 0바이트 수신은 버퍼를 받지 않는다. */
    if (proven_loop_recv(loop, &p->sock, (proven_mem_mut_t){ 0 }) != PROVEN_OK) proven_loop_close(loop, &p->sock);
}

static void peer_received(void *ctx, proven_err_t err, proven_size_t n) {
    ex_peer_t *p = ctx;
    (void)n;                                                    /* 0바이트 수신은 아무것도 가져오지 않는다 */
    if (err != PROVEN_OK) return;                               /* PROVEN_ERR_CANCELLED: 닫히는 중이다 */
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
    (void)proven_net_close(&p->conn);                           /* 연결 자체는 여기서, 마지막으로 닫는다 */
    p->used = false;
    /* 이 프로그램의 단 하나뿐인 클라이언트가 갔다: 연결 받기를 멈춘다. */
    if (++p->app->peers_closed == 1) proven_loop_accept_cancel(p->app->loop, &p->app->acc);
}

static const proven_loop_sock_fns_t peer_fns = { .received = peer_received, .writable = peer_writable, .closed = peer_closed };

static void on_accept(void *ctx, proven_err_t err, proven_net_conn_t *conn) {
    ex_app_t *app = ctx;
    if (err != PROVEN_OK) {                                     /* PROVEN_ERR_CANCELLED: 이제 리스너를 닫아도 된다 */
        (void)proven_net_listener_close(&app->listener);
        app->accepting_ended = true;
        return;
    }
    ex_peer_t *p = NULL;
    for (int i = 0; i < 4 && !p; ++i) if (!app->peers[i].used) p = &app->peers[i];
    if (!p) { (void)proven_net_close(conn); }
    else {
        *p = (ex_peer_t){ .app = app, .conn = *conn, .used = true };    /* 이제 이 연결은 우리 것이다 */
        if (proven_loop_attach(app->loop, &p->sock, &p->conn, &peer_fns, p) != PROVEN_OK ||
            proven_loop_recv(app->loop, &p->sock, (proven_mem_mut_t){ 0 }) != PROVEN_OK) { (void)proven_net_close(&p->conn); p->used = false; }
    }
    /* 안에서 다시 건다: 이미 기다리는 연결은 한 바퀴를 더 돌지 않고 받는다. */
    if (proven_loop_accept(app->loop, &app->acc, &app->listener, on_accept, app) != PROVEN_OK) app->accepting_ended = true;
}

// ---- 클라이언트 ----------------------------------------------------------------------

static void client_deadline(void *ctx) {
    ex_app_t *app = ctx;
    proven_loop_recv_cancel(app->loop, &app->sock);             /* 그 `received`가 PROVEN_ERR_CANCELLED로 온다 */
}

static void client_send(ex_app_t *app) {
    app->have = 0;
    proven_result_size_t s = proven_loop_send(app->loop, &app->sock, (proven_mem_view_t){ (const proven_byte_t *)MESSAGE, strlen(MESSAGE) });
    /* 클라이언트 자신의 버퍼로 받는 수신. 버퍼는 `received`까지 루프의 것이다. */
    if (s.err != PROVEN_OK || proven_loop_recv(app->loop, &app->sock, (proven_mem_mut_t){ app->buf, sizeof app->buf }) != PROVEN_OK) {
        proven_loop_close(app->loop, &app->sock);
        return;
    }
    proven_loop_timer_set(app->loop, &app->deadline, 5000, client_deadline, app);
}

static void client_received(void *ctx, proven_err_t err, proven_size_t n) {
    ex_app_t *app = ctx;
    if (err != PROVEN_OK) { proven_loop_close(app->loop, &app->sock); return; }    /* 기한이 지났거나 서버가 갔다 */
    app->have += n;
    if (app->have < strlen(MESSAGE)) {                          /* 스트림이다: 메아리가 조각으로 올 수 있다 */
        if (proven_loop_recv(app->loop, &app->sock, (proven_mem_mut_t){ app->buf + app->have, sizeof app->buf - app->have }) != PROVEN_OK)
            proven_loop_close(app->loop, &app->sock);
        return;
    }
    proven_loop_timer_cancel(app->loop, &app->deadline);
    if (memcmp(app->buf, MESSAGE, strlen(MESSAGE)) != 0) { proven_loop_close(app->loop, &app->sock); return; }
    if (++app->rounds < ROUNDS) { client_send(app); return; }
    app->echoed = true;
    proven_loop_close(app->loop, &app->sock);                   /* 서버가 끝을 읽고 자기 쪽을 닫는다 */
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
    EXAMPLE_REQUIRE(proven_loop_create(heap, &app.loop) == PROVEN_OK, "루프");

    // ---- 서버 ----------------------------------------------------------------------
    proven_net_addr_t bound;
    EXAMPLE_REQUIRE(proven_net_listen(proven_net_addr_ipv4(127, 0, 0, 1, 0), 16, &app.listener, &bound) == PROVEN_OK, "빈 포트의 리스너");
    EXAMPLE_REQUIRE(proven_loop_accept(app.loop, &app.acc, &app.listener, on_accept, &app) == PROVEN_OK, "수락 시작");

    // ---- 클라이언트 ------------------------------------------------------------------
    EXAMPLE_REQUIRE(proven_loop_connect(app.loop, &app.sock, bound, &app.conn, &client_fns, &app) == PROVEN_OK, "연결 시작");

    // ---- 모든 것이 끝날 때까지 돈다 ----------------------------------------------------
    proven_net_deadline_t end = proven_net_deadline_in(10000);
    while (!(app.client_closed && app.accepting_ended) && proven_time_monotonic_now() < end) {
        EXAMPLE_REQUIRE(proven_loop_poll(app.loop, proven_net_deadline_in(100)) == PROVEN_OK, "한 바퀴");
    }
    EXAMPLE_REQUIRE(app.echoed && app.rounds == ROUNDS, "메시지가 세 번 되돌아왔다");
    EXAMPLE_REQUIRE(app.peers_closed == 1 && app.accepting_ended && !proven_net_listener_is_open(&app.listener),
                    "서버는 클라이언트 다음에 자기 쪽을 닫고 수락을 멈췄다");
    proven_loop_destroy(app.loop);
    return EXAMPLE_OK();
}
