#include "example.h"

/*
 * proven_net_poll에 막혀 있는 루프에게는 소켓만 보인다. 다른 스레드에서 그 루프에게
 * 무언가를 알리려면 - "할 일이 생겼다", "멈춰라" - 여러분이 말할 때 읽을 수 있게 되는
 * 소켓이 필요하다. 그것이 깨우개(waker)다.
 *
 * proven_net_pair는 깨우개를 이루는 재료이고 그 자체로도 쓸모가 있다: 주소 없이 서로
 * 이어진 두 끝으로, 프로그램의 두 부분 사이에 바이트를 건넬 때 쓴다.
 */

static void finish_work(void *arg) {
    proven_net_waker_t *waker = arg;
    proven_time_sleep(20);              /* 어떤 작업을 대신하는 자리 */
    proven_net_waker_wake(waker);       /* 어느 스레드에서 불러도 된다 */
}

int main(void) {
    /* 짝: 한쪽 끝에 넣은 것이 다른 쪽 끝에서 나온다. */
    proven_net_conn_t a, b;
    proven_err_t err = proven_net_pair(&a, &b);
    if (err == PROVEN_ERR_PERMISSION || err == PROVEN_ERR_UNSUPPORTED) {
        printf("no sockets in this environment; nothing to show\n");
        return EXAMPLE_OK();
    }
    EXAMPLE_REQUIRE(err == PROVEN_OK, "a pair opens");
    EXAMPLE_REQUIRE(proven_net_write_all(&a, proven_mem_view_from_u8(PROVEN_LIT("across")), proven_net_deadline_in(1000)).err == PROVEN_OK, "write to one end");
    proven_byte_t buf[16];
    proven_result_size_t got = proven_net_read(&b, (proven_mem_mut_t){ buf, sizeof buf }, proven_net_deadline_in(1000));
    EXAMPLE_REQUIRE(got.err == PROVEN_OK && got.value == 6, "read from the other");
    (void)proven_net_close(&a);
    (void)proven_net_close(&b);

    /* poll 목록에 든 깨우개. 읽을 것이 없으니 poll은 1초를 꼬박 기다릴 터인데... */
    proven_net_waker_t waker;
    EXAMPLE_REQUIRE(proven_net_waker_open(&waker) == PROVEN_OK, "a waker opens");
    proven_job_sys_t *jobs = NULL;
    EXAMPLE_REQUIRE(proven_job_system_init(proven_heap_allocator(), 1, 4, &jobs) == PROVEN_OK, "a worker thread");
    EXAMPLE_REQUIRE(proven_job_submit_ex(jobs, finish_work, &waker) == PROVEN_OK, "work is handed to it");

    proven_net_poll_item_t item = { .handle = proven_net_waker_handle(&waker), .want = PROVEN_NET_READABLE };
    proven_size_t ready = 0;
    proven_time_t start = proven_time_monotonic_now();
    err = proven_net_poll(&item, 1, proven_net_deadline_in(1000), &ready);
    proven_i64 waited_ms = (proven_time_monotonic_now() - start) / 1000000;
    /* ...작업 스레드가 깨웠다. */
    EXAMPLE_REQUIRE(err == PROVEN_OK && ready == 1 && waited_ms < 900, "the poll returns when the worker says so, not at its deadline");

    /* 비워야 한다. 그러지 않으면 계속 읽을 수 있는 상태로 남아 다음 poll이 아무 일 없이 곧장 돌아온다. */
    proven_net_waker_drain(&waker);
    EXAMPLE_REQUIRE(proven_net_poll(&item, 1, proven_net_deadline_in(20), &ready) == PROVEN_ERR_TIMEOUT, "drained: quiet again");

    /* 비우기 전의 여러 번 깨우기는 한 번의 깨우기다. */
    proven_net_waker_wake(&waker);
    proven_net_waker_wake(&waker);
    proven_net_waker_drain(&waker);
    EXAMPLE_REQUIRE(proven_net_poll(&item, 1, proven_net_deadline_in(20), &ready) == PROVEN_ERR_TIMEOUT, "two wakes, one drain: quiet");

    /* 깨우개는 어떤 스레드도 더는 깨울 수 없을 때에만 닫는다: 작업 스레드를 먼저 멈춘다. */
    proven_job_system_close(jobs);
    proven_job_system_destroy(jobs);
    proven_net_waker_close(&waker);
    return EXAMPLE_OK();
}
