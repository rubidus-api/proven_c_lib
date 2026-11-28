#include "example.h"
#include <string.h>

/*
 * 연결마다 코루틴 하나, 루프가 구동한다.
 *
 * 요청마다 세션 코루틴이 하나 있고 - 메시지를 하나씩 차례로 - 자식인 6장의 reader 를 구동한다.
 * reader 는 2바이트 빅엔디언 길이와 그만큼의 바이트를 오는 대로 가져간다. 세션은 메시지마다
 * 에코로 답한다. 루프는 코루틴을 전혀 모른다. 둘을 잇는 것은 세 가지다.
 *
 *   - 이유. 0을 돌려주는 step 은 그 이유를 적어 둔다: 차례를 다 썼다(yield), 또는 입력을
 *     기다린다(receive). 디스패처는 step 이 끝난 뒤 한 번 읽는다.
 *   - 요청 안의 준비 큐. 실행할 수 있는 요청은 한 번만 연결된다(`queued`). 루프에 post 한
 *     함수 하나가 시작할 때 큐에 있던 것을 차례로 모두 진행한다. yield 한 요청은 맨 뒤로,
 *     다음 라운드로 가므로 바쁜 연결 하나가 루프를 붙잡을 수 없다.
 *   - 깨우기. 입력을 기다리는 요청은 먼저 기다리지 않고 읽어 본 뒤, 0바이트 receive 에
 *     머문다. 그 receive 의 함수가 요청을 다시 큐에 넣는다.
 *
 * 끝에 관한 규칙 하나: 요청의 소켓은 step 만 닫고, 닫히는 중인 요청은 아무것도 큐에 넣지
 * 않는다. 그래서 `closed` 가 올 때는 그 요청의 어떤 것도 큐에 있거나 진행 중이지 않으며,
 * 메모리를 거기서 놓아도 된다.
 */

#define BUDGET 1                        /* step 하나가 차례를 넘기기 전에 답할 수 있는 메시지 수 */
enum { WAIT_NONE, WAIT_YIELD, WAIT_RECV };

typedef struct ex_app ex_app_t;

typedef struct ex_req {
    ex_app_t *app;
    proven_loop_sock_t sock;
    proven_net_conn_t conn;
    proven_coro_t session;              /* 부모: 메시지를 하나씩 차례로 */
    proven_coro_t reader;               /* 자식: 메시지 하나 */
    int high, length, got;              /* reader 의 상태: 지역 변수는 yield 를 넘어 살아남지 못한다 */
    proven_byte_t in[64];
    proven_size_t have, pos;            /* `in` 의 바이트 수, 쓴 바이트 수 */
    proven_byte_t out[2 + 64];
    int wait;                           /* 마지막 step 이 0을 돌려준 이유 */
    int answered;                       /* 이번 step 에서 답한 메시지 수 */
    int steps;
    unsigned ticket;                    /* when it was queued: the queue's order */
    bool input_ended, stopping, queued, closing, used;
    struct ex_req *next;                /* 준비 큐: 요청 안의 링크, 할당 없음 */
    char name;
} ex_req_t;

struct ex_app {
    proven_loop_t *loop;
    ex_req_t reqs[2];
    ex_req_t *head, *tail;              /* 준비 큐 */
    bool drain_posted;
    char last;                          /* 마지막으로 진행한 요청 */
    unsigned tickets;
    int replies, closed, unfair, stray;
};

// ---- 자식과 부모 --------------------------------------------------------

/* reader: r->in 의 바이트를 있는 대로 가져간다. 세션만 이것을 진행한다. */
static proven_i32 reader_step(ex_req_t *r) {
    PROVEN_CORO_BEGIN(&r->reader);
    PROVEN_CORO_AWAIT(&r->reader, r->pos < r->have);
    r->high = r->in[r->pos++];
    PROVEN_CORO_AWAIT(&r->reader, r->pos < r->have);
    r->length = (r->high << 8) | r->in[r->pos++];
    r->got = 0;
    if (r->length > 64) { r->input_ended = true; r->length = 0; }     /* 이 프로그램이 받는 것보다 길다: 세션이 끝난다 */
    while (r->got < r->length) {
        PROVEN_CORO_AWAIT(&r->reader, r->pos < r->have);
        proven_size_t take = r->have - r->pos;
        if (take > (proven_size_t)(r->length - r->got)) take = (proven_size_t)(r->length - r->got);
        memcpy(r->out + 2 + r->got, r->in + r->pos, take);
        r->pos += take;
        r->got += (int)take;
    }
    PROVEN_CORO_END(&r->reader);
}

/* 도착한 것을 기다리지 않고 읽는다: 1 바이트가 왔다, 0 지금은 없다, -1 입력이 끝났다. */
static int req_fill(ex_req_t *r) {
    if (r->pos > 0) {                                   /* 아직 안 쓴 것만 남긴다 */
        memmove(r->in, r->in + r->pos, r->have - r->pos);
        r->have -= r->pos;
        r->pos = 0;
    }
    if (r->have == sizeof r->in) return 1;
    proven_result_size_t got = proven_loop_read(r->app->loop, &r->sock, (proven_mem_mut_t){ r->in + r->have, sizeof r->in - r->have });
    if (got.err == PROVEN_ERR_TIMEOUT) return 0;
    if (got.err != PROVEN_OK) return -1;                /* PROVEN_ERR_EOF, PROVEN_ERR_RESET */
    r->have += got.value;
    return 1;
}

/* 세션. 0을 돌려줄 때마다 먼저 이유를 적는다. */
static proven_i32 session_step(ex_req_t *r) {
    PROVEN_CORO_BEGIN(&r->session);
    while (!r->stopping && !r->input_ended) {
        PROVEN_CORO_INIT(&r->reader);
        while (!reader_step(r) && !r->input_ended) {
            const int f = req_fill(r);                  /* 머물기 전에 먼저 그냥 읽어 본다 */
            if (f > 0) continue;
            if (f < 0) { r->input_ended = true; break; }
            r->wait = WAIT_RECV;
            PROVEN_CORO_YIELD(&r->session);
            if (r->stopping) break;
        }
        /* 입력이 끝났거나, 너무 긴 메시지거나, 프로그램이 멈추는 중: 답하지 않는다 */
        if (!PROVEN_CORO_IS_DONE(&r->reader) || r->input_ended) break;
        r->out[0] = (proven_byte_t)(r->length >> 8);
        r->out[1] = (proven_byte_t)r->length;
        if (proven_loop_send(r->app->loop, &r->sock, (proven_mem_view_t){ r->out, (proven_size_t)r->length + 2 }).err != PROVEN_OK) break;
        r->app->replies++;
        if (++r->answered >= BUDGET) {
            r->wait = WAIT_YIELD;
            PROVEN_CORO_YIELD(&r->session);
        }
    }
    PROVEN_CORO_END(&r->session);
}

// ---- 디스패처 ------------------------------------------------------------------

static void drain(void *ctx);

/* 요청을 한 번만 큐에 넣는다. 닫히는 중인 요청은 다시 넣지 않는다. */
static void wake(ex_req_t *r) {
    if (r->closing) return;
    ex_app_t *app = r->app;
    if (!r->queued) {
        r->queued = true;
        r->ticket = ++app->tickets;
        r->next = NULL;
        if (app->tail) app->tail->next = r; else app->head = r;
        app->tail = r;
    }
    if (!app->drain_posted) {
        app->drain_posted = true;
        /* PROVEN_ERR_NOMEM: 잃는 것은 없다 - 요청들은 큐에 남고, 다음 깨우기가(이미 큐에 있는
         * 요청의 것이라도) 다시 post 한다. */
        if (proven_loop_post(app->loop, drain, app) != PROVEN_OK) app->drain_posted = false;
    }
}

/* step 하나와, 그 이유에 따라 이어지는 일. */
static void step(ex_req_t *r) {
    ex_app_t *app = r->app;
    if (!r->used) { app->stray++; return; }             /* 놓아 버린 메모리 위의 step 이 될 뻔했다 */
    r->queued = false;
    const ex_req_t *other = &app->reqs[r == &app->reqs[0]];
    /* 먼저 큐에 들어와 기다리던 다른 요청을 두고 두 번 연달아 진행했다 */
    if (app->last == r->name && other->queued && other->ticket < r->ticket) app->unfair++;
    app->last = r->name;
    r->steps++;
    r->answered = 0;
    r->wait = WAIT_NONE;
    if (session_step(r)) {                              /* 끝: 요청의 끝 */
        r->closing = true;
        proven_loop_close(app->loop, &r->sock);         /* 진행 중인 receive 는 CANCELLED 로 답을 받는다 */
        return;
    }
    const int why = r->wait;
    r->wait = WAIT_NONE;
    EXAMPLE_REQUIRE(why != WAIT_NONE, "0을 돌려준 step 은 이유를 적었다");
    if (why == WAIT_YIELD) { wake(r); return; }         /* 큐의 맨 뒤: 다음 라운드 */
    /* 입력을 기다린다: 0바이트 receive 에 머문다. 기다리는 동안 버퍼를 잡지 않는다. */
    if (proven_loop_recv(app->loop, &r->sock, (proven_mem_mut_t){ 0 }) != PROVEN_OK) { r->input_ended = true; wake(r); }
}

/* 이것이 시작할 때 큐에 있던 것을 차례로 진행한다. step 이 다시 넣은 것은 다음 번을 기다린다. */
static void drain(void *ctx) {
    ex_app_t *app = ctx;
    app->drain_posted = false;
    ex_req_t *r = app->head, *last = app->tail;
    app->head = app->tail = NULL;
    while (r) {
        ex_req_t *next = r->next;
        const bool end = r == last;
        step(r);
        if (end) break;
        r = next;
    }
}

// ---- 루프의 함수들 --------------------------------------------------------------

static void req_received(void *ctx, proven_err_t err, proven_size_t n) {
    ex_req_t *r = ctx;
    (void)n;                                            /* 0바이트 receive 는 아무것도 가져가지 않는다 */
    if (err == PROVEN_ERR_CANCELLED) return;            /* 닫히는 중이다 */
    if (err != PROVEN_OK) r->input_ended = true;        /* PROVEN_ERR_EOF, PROVEN_ERR_RESET: 세션이 끝난다 */
    wake(r);
}

static void req_closed(void *ctx) {
    ex_req_t *r = ctx;
    (void)proven_net_close(&r->conn);
    if (r->queued) r->app->stray++;                     /* 있을 수 없다: step 만 닫고, wake 는 거절한다 */
    r->used = false;                                    /* 이제 요청의 메모리를 놓아도 된다 */
    r->app->closed++;
}

static const proven_loop_sock_fns_t req_fns = { .received = req_received, .closed = req_closed };

// ---- 반대쪽 끝들, 이 스레드에서 -------------------------------------------------------

static void say(proven_net_conn_t *c, const char *text) {
    proven_byte_t msg[2 + 64];
    const proven_size_t n = strlen(text);
    msg[0] = (proven_byte_t)(n >> 8);
    msg[1] = (proven_byte_t)n;
    memcpy(msg + 2, text, n);
    proven_result_size_t w = proven_net_write(c, (proven_mem_view_t){ msg, n + 2 }, proven_net_deadline_in(1000));
    EXAMPLE_REQUIRE(w.err == PROVEN_OK && w.value == n + 2, "메시지를 썼다");
}

static bool heard(proven_net_conn_t *c, const char *text) {
    proven_byte_t got[2 + 64];
    const proven_size_t want = strlen(text) + 2;
    for (proven_size_t have = 0; have < want; ) {
        proven_result_size_t r = proven_net_read(c, (proven_mem_mut_t){ got + have, want - have }, proven_net_deadline_in(1000));
        if (r.err != PROVEN_OK) return false;
        have += r.value;
    }
    return got[0] == 0 && got[1] == strlen(text) && memcmp(got + 2, text, strlen(text)) == 0;
}

static void run_until(ex_app_t *app, bool (*done)(const ex_app_t *)) {
    for (int i = 0; i < 200 && !done(app); ++i) {
        EXAMPLE_REQUIRE(proven_loop_poll(app->loop, proven_net_deadline_in(10)) == PROVEN_OK, "한 바퀴");
    }
    EXAMPLE_REQUIRE(done(app), "루프가 거기까지 갔다");
}
static bool six_replies(const ex_app_t *app) { return app->replies == 6 && !app->head; }
static bool seven_replies(const ex_app_t *app) { return app->replies == 7 && !app->head; }
static bool one_closed(const ex_app_t *app) { return app->closed == 1; }
static bool both_closed(const ex_app_t *app) { return app->closed == 2; }
static bool both_parked(const ex_app_t *app) { return app->reqs[0].steps > 0 && app->reqs[1].steps > 0 && !app->head; }

int main(void) {
    static ex_app_t app;
    proven_net_conn_t peer[2];
    EXAMPLE_REQUIRE(proven_loop_create(proven_heap_allocator(), &app.loop) == PROVEN_OK, "루프");
    for (int i = 0; i < 2; ++i) {
        ex_req_t *r = &app.reqs[i];
        *r = (ex_req_t){ .app = &app, .used = true, .name = (char)('A' + i) };
        EXAMPLE_REQUIRE(proven_net_pair(&r->conn, &peer[i]) == PROVEN_OK, "연결된 한 쌍");
        PROVEN_CORO_INIT(&r->session);
        EXAMPLE_REQUIRE(proven_loop_attach(app.loop, &r->sock, &r->conn, &req_fns, r) == PROVEN_OK, "요청 쪽 끝을 루프에 넘겼다");
        wake(r);                                        /* 새 요청은 step 하나로 시작한다 */
    }
    run_until(&app, both_parked);

    // ---- 각각에 메시지 셋: step 마다 하나씩, 번갈아 ----------------------
    say(&peer[0], "one"); say(&peer[0], "two"); say(&peer[0], "three");
    say(&peer[1], "uno"); say(&peer[1], "dos"); say(&peer[1], "tres");
    run_until(&app, six_replies);
    EXAMPLE_REQUIRE(heard(&peer[0], "one") && heard(&peer[0], "two") && heard(&peer[0], "three"), "A 의 에코 셋, 순서대로");
    EXAMPLE_REQUIRE(heard(&peer[1], "uno") && heard(&peer[1], "dos") && heard(&peer[1], "tres"), "B 의 에코 셋, 순서대로");
    EXAMPLE_REQUIRE(app.unfair == 0, "다른 요청이 큐에 있는 동안 두 번 연달아 진행된 요청은 없다");

    // ---- 입력을 기다리는 요청은 진행되지 않는다 --------------------------------------
    const int b_steps = app.reqs[1].steps;
    say(&peer[0], "four");
    run_until(&app, seven_replies);
    EXAMPLE_REQUIRE(heard(&peer[0], "four"), "A 의 넷째 에코");
    EXAMPLE_REQUIRE(app.reqs[1].steps == b_steps, "입력을 기다리는 B 는 A 가 바쁜 동안 진행되지 않았다");

    // ---- 입력의 끝이 세션을 끝낸다 -----------------------------------------------------
    (void)proven_net_close(&peer[0]);
    run_until(&app, one_closed);
    EXAMPLE_REQUIRE(!app.reqs[0].used && app.reqs[1].used, "A 는 EOF 에서 끝나 자기 step 이 닫았다. B 는 계속된다");

    // ---- 진행 중인 receive 에서 기다리는 요청 멈추기 ---------------------------------
    app.reqs[1].stopping = true;
    wake(&app.reqs[1]);                                 /* 세션이 `stopping` 을 보고 끝난다 */
    run_until(&app, both_closed);
    EXAMPLE_REQUIRE(!app.reqs[1].used, "B 는 멈췄다: 진행 중인 receive 가 취소된 뒤 닫혔다");
    EXAMPLE_REQUIRE(app.stray == 0, "`closed` 뒤에 요청에 닿은 것은 없다");

    (void)proven_net_close(&peer[1]);
    proven_loop_destroy(app.loop);
    return EXAMPLE_OK();
}
