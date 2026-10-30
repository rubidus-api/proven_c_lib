#include "example.h"
#include <stdio.h>
#include <string.h>

/*
 * 받아들이는 스레드 없이 포트 하나에 여러 루프(Linux): 포트가 같은 주소와 포트에 루프마다 리스너를
 * 하나씩 열고, 시스템이 연결을 그들 사이에 나눈다.
 *
 * `http3`를 true로 한 같은 포트는 루프마다 UDP 소켓도 열고, 각 QUIC 데이터그램을 그 연결을 가진
 * 루프로 보낸다 - 연결 ID로 고르므로, 주소가 바뀐 클라이언트에게도 제 루프가 답한다. Linux 밖에서는
 * 이 포트를 가질 수 없다: 거기서는 앞 프로그램의 받아들이는 스레드가 방법이고, HTTP/3은 포트마다
 * 루프 하나다.
 */

enum { LOOPS = 2 };

typedef struct {
    int index;
    proven_loop_t *loop;
    proven_http_event_server_t *server;
    int served;                       /* 루프가 멈춘 뒤에만 주 스레드가 읽는다 */
} ex_worker_t;

static ex_worker_t g_workers[LOOPS];

static void on_request(void *ctx, proven_http_stream_t *stream, const proven_http_request_t *head) {
    ex_worker_t *me = ctx;
    (void)head;
    me->served++;
    (void)proven_http_stream_respond(stream, 200, NULL, 0, (proven_mem_view_t){ (const proven_byte_t *)"ok", 2 });
}

static void run(void *arg) { (void)proven_loop_run(((ex_worker_t *)arg)->loop); }

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();

    // ---- 포트: 모든 소켓을 여기서 한꺼번에 연다 ------------------------------------
    proven_http_event_port_t *port = NULL;
    proven_net_addr_t at;
    proven_err_t err = proven_http_event_port_open(heap, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), LOOPS, false, &port, &at);
    if (err == PROVEN_ERR_UNSUPPORTED) {
        printf("이 시스템에는 나눠 쓰는 포트가 없다: 대신 리스너 하나와 받아들이는 스레드\n");
        return EXAMPLE_OK();
    }
    EXAMPLE_REQUIRE(err == PROVEN_OK, "루프 둘을 위한 포트");
    if (err != PROVEN_OK) return EXAMPLE_OK();

    // ---- 루프 둘, 각 서버가 제 몫을 가져간다 ---------------------------------------
    /* listen_port는 listen을 부를 자리에서 부른다: 루프가 돌기 전에, 또는 그 스레드에서. */
    proven_job_sys_t *threads = NULL;
    proven_job_group_t loops;
    proven_job_group_init(&loops);
    EXAMPLE_REQUIRE(proven_job_system_init(heap, LOOPS, 8, &threads) == PROVEN_OK, "스레드 둘");
    for (int i = 0; i < LOOPS; ++i) {
        ex_worker_t *w = &g_workers[i];
        w->index = i;
        proven_http_event_server_config_t config = { .on = { .on_request = on_request }, .ctx = w };
        EXAMPLE_REQUIRE(proven_loop_create(heap, &w->loop) == PROVEN_OK &&
                        proven_http_event_server_create(w->loop, &config, &w->server) == PROVEN_OK &&
                        proven_http_event_server_listen_port(w->server, port, (proven_size_t)i) == PROVEN_OK, "제 몫 위에 서버를 둔 루프");
    }
    /* 서버들이 가져간 것을 쥐고 있다; 포트는 더 필요 없다. */
    proven_http_event_port_close(port);
    for (int i = 0; i < LOOPS; ++i)
        EXAMPLE_REQUIRE(proven_job_group_submit(threads, &loops, run, &g_workers[i]) == PROVEN_OK, "각 루프는 제 스레드에서");

    // ---- 연결 열여섯 -----------------------------------------------------------------
    proven_http_client_config_t client_config = { .alloc = heap, .max_idle_connections = 0 };
    proven_http_client_t *client = NULL;
    EXAMPLE_REQUIRE(proven_http_client_create(&client_config, &client) == PROVEN_OK, "클라이언트");
    char url[64];
    int n = snprintf(url, sizeof url, "http://127.0.0.1:%u/", (unsigned)at.port);
    int answered = 0;
    for (int i = 0; i < 16; ++i) {
        proven_http_client_response_t response;
        if (proven_http_client_get(client, (proven_u8str_view_t){ (const proven_byte_t *)url, (proven_size_t)n }, &response) != PROVEN_OK) continue;
        if (response.status == 200) answered++;
        proven_http_client_finish(&response);
    }
    proven_http_client_destroy(client);
    EXAMPLE_REQUIRE(answered == 16, "모든 요청에 답이 온다");

    // ---- 해체: 루프들을 함께, 그다음 서버들 -----------------------------------------
    for (int i = 0; i < LOOPS; ++i) proven_loop_stop(g_workers[i].loop);
    proven_job_group_wait(threads, &loops);
    int served = 0;
    for (int i = 0; i < LOOPS; ++i) {
        printf("loop %d served %d\n", i, g_workers[i].served);    /* 차례가 아니라 시스템이 고른 것 */
        served += g_workers[i].served;
        proven_http_event_server_destroy(g_workers[i].server);
        proven_loop_destroy(g_workers[i].loop);
    }
    EXAMPLE_REQUIRE(served == 16, "모두 합쳐 요청 열여섯이 처리되었다");
    proven_job_system_close(threads);
    proven_job_system_destroy(threads);
    return EXAMPLE_OK();
}
