#include "example.h"
#include <stdio.h>
#include <string.h>

/*
 * Several loops on one port, with no thread that accepts (Linux): the port opens a listener for
 * each loop on the same address and port, and the system spreads connections over them.
 *
 * The same port with `http3` true opens a UDP socket for each loop too, and sends each QUIC
 * datagram to the loop that owns its connection - by connection ID, so a client whose address
 * changes is still answered by its loop. Elsewhere than Linux the port is not to be had: there,
 * the accepting thread of the previous program is the way, and HTTP/3 is one loop per port.
 */

enum { LOOPS = 2 };

typedef struct {
    int index;
    proven_loop_t *loop;
    proven_http_event_server_t *server;
    int served;                       /* read by the main thread only after the loop has stopped */
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

    // ---- the port: every socket opened here, at once -------------------------------
    proven_http_event_port_t *port = NULL;
    proven_net_addr_t at;
    proven_err_t err = proven_http_event_port_open(heap, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), LOOPS, false, &port, &at);
    if (err == PROVEN_ERR_UNSUPPORTED) {
        printf("no shared port on this system: one listener and an accepting thread instead\n");
        return EXAMPLE_OK();
    }
    EXAMPLE_REQUIRE(err == PROVEN_OK, "a port for two loops");
    if (err != PROVEN_OK) return EXAMPLE_OK();

    // ---- two loops, each server taking its share ------------------------------------
    /* listen_port is called where listen would be: before the loop runs, or on its thread. */
    proven_job_sys_t *threads = NULL;
    proven_job_group_t loops;
    proven_job_group_init(&loops);
    EXAMPLE_REQUIRE(proven_job_system_init(heap, LOOPS, 8, &threads) == PROVEN_OK, "two threads");
    for (int i = 0; i < LOOPS; ++i) {
        ex_worker_t *w = &g_workers[i];
        w->index = i;
        proven_http_event_server_config_t config = { .on = { .on_request = on_request }, .ctx = w };
        EXAMPLE_REQUIRE(proven_loop_create(heap, &w->loop) == PROVEN_OK &&
                        proven_http_event_server_create(w->loop, &config, &w->server) == PROVEN_OK &&
                        proven_http_event_server_listen_port(w->server, port, (proven_size_t)i) == PROVEN_OK, "a loop with a server on its share");
    }
    /* The servers hold what they took; the port is no longer needed. */
    proven_http_event_port_close(port);
    for (int i = 0; i < LOOPS; ++i)
        EXAMPLE_REQUIRE(proven_job_group_submit(threads, &loops, run, &g_workers[i]) == PROVEN_OK, "each loop on its own thread");

    // ---- sixteen connections --------------------------------------------------------
    proven_http_client_config_t client_config = { .alloc = heap, .max_idle_connections = 0 };
    proven_http_client_t *client = NULL;
    EXAMPLE_REQUIRE(proven_http_client_create(&client_config, &client) == PROVEN_OK, "a client");
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
    EXAMPLE_REQUIRE(answered == 16, "every request is answered");

    // ---- taking it apart: the loops together, then the servers -----------------------
    for (int i = 0; i < LOOPS; ++i) proven_loop_stop(g_workers[i].loop);
    proven_job_group_wait(threads, &loops);
    int served = 0;
    for (int i = 0; i < LOOPS; ++i) {
        printf("loop %d served %d\n", i, g_workers[i].served);    /* the system's choice, not a turn */
        served += g_workers[i].served;
        proven_http_event_server_destroy(g_workers[i].server);
        proven_loop_destroy(g_workers[i].loop);
    }
    EXAMPLE_REQUIRE(served == 16, "sixteen requests were served in all");
    proven_job_system_close(threads);
    proven_job_system_destroy(threads);
    return EXAMPLE_OK();
}
