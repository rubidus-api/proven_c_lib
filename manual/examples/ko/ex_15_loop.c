#include "example.h"
#include <stdatomic.h>

/*
 * 이벤트 루프 그 자체: 타이머, 그리고 다른 스레드에서 하고 돌아오는 일.
 *
 * 루프의 스레드는 "다음은 무엇인가" 말고는 아무것도 기다리지 않는다. 시간이 걸리는 것 - 여기서는
 * 파일이나 데이터베이스를 대신해 잠을 자는 워커 - 은 다른 곳에서 일어나고, 그 결과는 루프에
 * 게시되어 다른 이벤트들 사이에서 실행된다.
 */

typedef struct {
    proven_loop_t *loop;
    proven_loop_timer_t tick, late;
    int ticks;
    int order[8];
    int count;
    atomic_int from_worker;
} app_t;

static void note(app_t *app, int what) { if (app->count < 8) app->order[app->count++] = what; }

/* 타이머는 한 번 울린다. 반복해야 하는 타이머는 스스로 다시 맞춘다. */
static void on_tick(void *ctx) {
    app_t *app = ctx;
    note(app, 1);
    if (++app->ticks < 3) proven_loop_timer_set(app->loop, &app->tick, 20, on_tick, app);
}

static void on_late(void *ctx) { note(ctx, 9); }

/* 워커 스레드가 요청했지만, 이것은 루프의 스레드에서 실행된다. */
static void on_result(void *ctx) {
    app_t *app = ctx;
    note(app, 5);
    proven_loop_timer_cancel(app->loop, &app->late);      /* 오 초 뒤에 울렸을 것이다. 이제는 영영 울리지 않는다 */
    proven_loop_stop(app->loop);
}

/* 이것은 워커 스레드에서 실행된다. 얼마든지 오래 걸려도 된다: 루프는 기다리지 않는다. */
static void slow_work(void *ctx) {
    app_t *app = ctx;
    proven_time_sleep(400);
    atomic_store(&app->from_worker, 1);
    if (proven_loop_post(app->loop, on_result, app) != PROVEN_OK) proven_loop_stop(app->loop);   /* 게시는 할당 실패일 때만 거절된다. 그때는 그냥 실행을 끝낸다 */
}

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();
    app_t app = { 0 };

    // ---- 루프 ---------------------------------------------------------------------
    /* 루프는 셀렉터, 타이머 묶음, 그리고 다른 스레드가 게시할 수 있는 큐다. */
    EXAMPLE_REQUIRE(proven_loop_create(heap, &app.loop) == PROVEN_OK, "루프");

    // ---- 타이머 --------------------------------------------------------------------
    /* 타이머 구조체는 여러분의 것이다: 여러분의 상태 안에 살고, 맞추는 데 아무것도 할당하지
     * 않는다. 울리기를 기다리는 타이머도 비용이 없다 - 때가 될 때까지 들여다보지 않는다. */
    proven_loop_timer_set(app.loop, &app.tick, 20, on_tick, &app);
    proven_loop_timer_set(app.loop, &app.late, 5000, on_late, &app);
    EXAMPLE_REQUIRE(proven_loop_timer_is_set(&app.tick) && proven_loop_timer_is_set(&app.late), "타이머 둘이 맞춰졌다");

    // ---- 다른 곳에서 하는 일 --------------------------------------------------------
    /* job system이 slow_work를 워커 스레드에서 돌린다. 끝나면 on_result를 루프로 게시한다. */
    proven_job_sys_t *workers = NULL;
    EXAMPLE_REQUIRE(proven_job_system_init(heap, 1, 4, &workers) == PROVEN_OK && proven_job_submit(workers, slow_work, &app), "워커가 느린 일을 맡았다");

    // ---- 실행 ----------------------------------------------------------------------
    /* proven_loop_run은 무언가가 proven_loop_stop을 부르면 돌아온다 - 여기서는 on_result가 부른다. */
    proven_time_t began = proven_time_monotonic_now();
    EXAMPLE_REQUIRE(proven_loop_run(app.loop) == PROVEN_OK, "루프가 돌았고 멈춰졌다");
    proven_time_t took = proven_time_monotonic_now() - began;

    EXAMPLE_REQUIRE(app.ticks == 3 && app.count == 4 && app.order[0] == 1 && app.order[1] == 1 && app.order[2] == 1 && app.order[3] == 5,
                    "틱 세 번, 그다음 워커의 결과, 그 순서로");
    EXAMPLE_REQUIRE(atomic_load(&app.from_worker) == 1 && took < 2000000000, "결과는 워커에게서 왔고, 오 초 타이머보다 훨씬 먼저 왔다");
    EXAMPLE_REQUIRE(!proven_loop_timer_is_set(&app.late), "취소된 타이머는 더는 맞춰져 있지 않다");

    proven_job_system_close(workers);
    proven_job_system_destroy(workers);
    proven_loop_destroy(app.loop);
    return EXAMPLE_OK();
}
