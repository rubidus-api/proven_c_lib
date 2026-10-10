#include "example.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * 포트 하나 뒤의 여러 루프: 스레드 하나가 받아들이고, 각자의 루프 위에 있는 서버들에게
 * 연결을 나눠 준다.
 *
 * 연결은 루프 하나에 속하고, 그 연결의 어떤 것도 다른 스레드가 건드리지 않는다. 그래서
 * 프로세서에 걸친 확장은 각자 서버를 가진 여러 루프다 - 그리고 들어오는 연결을 그들 사이에
 * 나눠 줄 무언가가 있어야 한다. 여기서는 받아들이고 차례로 나눠 주는 스레드가 그 일을 하며,
 * 이 방식은 라이브러리가 도는 곳이면 어디서나 된다.
 */

enum { LOOPS = 2 };

/* 루프 하나, 그 서버, 그리고 그 핸들러가 세는 것. 그 루프의 스레드만 건드린다. */
typedef struct {
    int index;
    proven_loop_t *loop;
    proven_http_event_server_t *server;
    int served;                       /* 루프가 멈춘 뒤에만 메인 스레드가 읽는다 */
} worker_t;

static worker_t g_workers[LOOPS];
static proven_net_listener_t g_listener;
static atomic_bool g_stop;

static void on_request(void *ctx, proven_http_stream_t *stream, const proven_http_request_t *head) {
    worker_t *me = ctx;
    (void)head;
    char text[16];
    int n = snprintf(text, sizeof text, "loop %d", me->index);
    me->served++;
    (void)proven_http_stream_respond(stream, 200, NULL, 0, (proven_mem_view_t){ (const proven_byte_t *)text, (proven_size_t)n });
}

/* 받아들이는 스레드에서 루프로 건너가는 것. 열린 연결은 복사해서는 안 되므로
 * 자기 메모리에 담겨 간다. */
typedef struct { worker_t *to; proven_net_conn_t conn; } handoff_t;

/* 루프의 스레드에서 - 서버에 연결을 줄 수 있는 유일한 곳. */
static void adopt_here(void *ctx) {
    handoff_t *h = ctx;
    if (proven_http_event_server_adopt(h->to->server, &h->conn) != PROVEN_OK) (void)proven_net_close(&h->conn);   /* 거절됨: 여전히 우리 것이니 닫는다 */
    free(h);
}

/* 받아들이는 스레드. 어떤 서버도 건드리지 않는다: 받아들이고, 고르고, 게시한다. */
static void acceptor(void *arg) {
    (void)arg;
    int next = 0;
    while (!atomic_load(&g_stop)) {
        handoff_t *h = malloc(sizeof *h);
        if (!h) break;
        /* 한 번에 잠깐씩만 기다려서, 멈추라는 말을 스레드가 알아채게 한다. */
        if (proven_net_accept(&g_listener, proven_net_deadline_in(20), &h->conn, NULL) != PROVEN_OK) { free(h); continue; }
        h->to = &g_workers[next];
        next = (next + 1) % LOOPS;
        if (proven_loop_post(h->to->loop, adopt_here, h) != PROVEN_OK) { (void)proven_net_close(&h->conn); free(h); }
    }
}

static void run(void *arg) { (void)proven_loop_run(((worker_t *)arg)->loop); }

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();

    // ---- 루프 둘, 각각 리스너 없는 서버 하나 ---------------------------------------
    /* 리스너 없는 서버는 주어진 것만 서비스한다. */
    proven_job_sys_t *threads = NULL;
    proven_job_group_t loops, accepting;
    proven_job_group_init(&loops);
    proven_job_group_init(&accepting);
    EXAMPLE_REQUIRE(proven_job_system_init(heap, LOOPS + 1, 8, &threads) == PROVEN_OK, "스레드 셋");
    for (int i = 0; i < LOOPS; ++i) {
        worker_t *w = &g_workers[i];
        w->index = i;
        proven_http_event_server_config_t config = { .on = { .on_request = on_request }, .ctx = w };
        EXAMPLE_REQUIRE(proven_loop_create(heap, &w->loop) == PROVEN_OK &&
                        proven_http_event_server_create(w->loop, &config, &w->server) == PROVEN_OK &&
                        proven_job_group_submit(threads, &loops, run, w) == PROVEN_OK, "자기 스레드의 루프와 서버");
    }

    // ---- 리스너 하나, 그리고 나눠 주는 스레드 --------------------------------------
    proven_net_addr_t at;
    EXAMPLE_REQUIRE(proven_net_listen(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), 64, &g_listener, &at) == PROVEN_OK &&
                    proven_job_group_submit(threads, &accepting, acceptor, NULL) == PROVEN_OK, "리스너와 받아들이는 스레드");

    // ---- 연결 여덟 -----------------------------------------------------------------
    /* 연결을 열어 두지 않는 클라이언트: 요청 여덟은 연결 여덟이다. */
    proven_http_client_config_t client_config = { .alloc = heap, .max_idle_connections = 0 };
    proven_http_client_t *client = NULL;
    EXAMPLE_REQUIRE(proven_http_client_create(&client_config, &client) == PROVEN_OK, "클라이언트");
    char url[64];
    int n = snprintf(url, sizeof url, "http://127.0.0.1:%u/", (unsigned)at.port);
    int answers[LOOPS] = { 0 };
    for (int i = 0; i < 8; ++i) {
        proven_http_client_response_t response;
        proven_u8str_t body = { 0 };
        EXAMPLE_REQUIRE(proven_http_client_get(client, (proven_u8str_view_t){ (const proven_byte_t *)url, (proven_size_t)n }, &response) == PROVEN_OK &&
                        proven_http_client_read_all(&response, heap, &body, 64) == PROVEN_OK, "요청에 답이 온다");
        proven_u8str_view_t text = proven_u8str_as_view(&body);
        if (text.size == 6 && memcmp(text.ptr, "loop ", 5) == 0 && text.ptr[5] - '0' < LOOPS) answers[text.ptr[5] - '0']++;
        proven_u8str_destroy(heap, &body);
        proven_http_client_finish(&response);
    }
    proven_http_client_destroy(client);
    EXAMPLE_REQUIRE(answers[0] == 4 && answers[1] == 4, "차례로 나눠 줬다: 루프마다 연결 넷을 서비스했다");

    // ---- 해체, 이 순서로 -----------------------------------------------------------
    /* 받아들이는 쪽이 먼저다. 그래야 더는 게시되지 않는다. 그다음 루프들. 그다음 서버들. */
    atomic_store(&g_stop, true);
    proven_job_group_wait(threads, &accepting);
    (void)proven_net_listener_close(&g_listener);
    for (int i = 0; i < LOOPS; ++i) proven_loop_stop(g_workers[i].loop);
    proven_job_group_wait(threads, &loops);
    int served = 0;
    for (int i = 0; i < LOOPS; ++i) {
        /* 게시되었으나 아직 돌지 않은 것은 여기서, 서버가 사라지기 전에 돈다. */
        EXAMPLE_REQUIRE(proven_loop_poll(g_workers[i].loop, PROVEN_NET_DONT_WAIT) == PROVEN_OK, "마지막 한 바퀴");
        served += g_workers[i].served;
        proven_http_event_server_destroy(g_workers[i].server);
        proven_loop_destroy(g_workers[i].loop);
    }
    EXAMPLE_REQUIRE(served == 8, "모두 여덟 요청이 서비스되었다");
    proven_job_system_close(threads);
    proven_job_system_destroy(threads);
    return EXAMPLE_OK();
}
