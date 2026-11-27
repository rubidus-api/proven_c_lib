#include "example.h"

/*
 * 다른 코루틴을 기다리는 코루틴: 부모가 자식의 step 함수를 PROVEN_CORO_AWAIT 의 조건에
 * 넣는다. AWAIT 는 들를 때마다(처음 들를 때도) 조건을 평가하므로, 부모를 한 번 부를 때마다
 * 자식이 한 번 진행하고, 자식이 끝나는 바로 그 호출에서 부모도 이어 간다.
 *
 * 여기서 자식은 2바이트 빅엔디언 길이를 바이트가 오는 대로 읽는다. 첫 바이트가 오면 곧바로
 * 가져가 자기 구조체에 둔다 - 지역 변수는 멈춤을 넘어 살아남지 못한다 - 그리고 둘째를
 * 기다린다.
 *
 * 규칙 둘:
 *   - 진행시키는 쪽은 하나다. 자식은 부모만 진행시킨다. 부르는 쪽도 자식을 진행시키면
 *     입력 하나에 두 번 나아간다.
 *   - 끝난 상태는 들러붙는다. END 뒤의 상태 -1 은 어떤 case 와도 맞지 않으므로, 그 뒤의
 *     호출은 본문을 건너뛰고 다시 1 을 돌려준다.
 *
 * 그리고 함정 하나: AWAIT(PROVEN_CORO_IS_DONE(&child)) 는 자식을 결코 진행시키지 않는다.
 * 부르는 쪽은 부모만 진행시키기 때문이다. 신호는 자식의 반환값이고, 길이가 0 이어도
 * "끝났다"고 말한다.
 */

typedef struct {
    proven_coro_t coro;
    const unsigned char *in;    /* 지금까지의 입력. 부르는 쪽이 쓴다 */
    int available;              /* `in` 에 있는 바이트 수 */
    int taken;                  /* 읽는 쪽이 쓴 바이트 수 */
    int high;                   /* 첫 바이트. 멈춤을 넘어 간직한다 */
    int length;                 /* 결과 */
    int steps;                  /* 진행된 횟수(검사용) */
} length_reader_t;

static proven_i32 reader_step(length_reader_t *r) {
    r->steps += 1;
    PROVEN_CORO_BEGIN(&r->coro);
    PROVEN_CORO_AWAIT(&r->coro, r->available >= 1);
    r->high = r->in[0];
    r->taken = 1;
    PROVEN_CORO_AWAIT(&r->coro, r->available >= 2);
    r->length = (r->high << 8) | r->in[1];
    r->taken = 2;
    PROVEN_CORO_END(&r->coro);
}

typedef struct {
    proven_coro_t coro;
    length_reader_t reader;
    int length;                 /* 부모가 내놓는다. 그때까지는 -1 */
} request_t;

static proven_i32 request_step(request_t *q) {
    PROVEN_CORO_BEGIN(&q->coro);
    PROVEN_CORO_AWAIT(&q->coro, reader_step(&q->reader));
    q->length = q->reader.length;
    PROVEN_CORO_END(&q->coro);
}

static void request_init(request_t *q, const unsigned char *in, int available) {
    *q = (request_t){ .length = -1 };
    q->reader.in = in;
    q->reader.available = available;
    PROVEN_CORO_INIT(&q->coro);         /* 둘 다, 첫 호출 전에 한 번 */
    PROVEN_CORO_INIT(&q->reader.coro);
}

int main(void) {
    static const unsigned char bytes[] = { 0x01, 0x02 };
    request_t q;
    request_init(&q, bytes, 0);

    /* 한 덩어리가 부모 호출 한 번이다. 부르는 쪽은 입력만 더한다. */
    EXAMPLE_REQUIRE(request_step(&q) == 0 && q.length == -1 && q.reader.taken == 0,
                    "no bytes: the parent waits, the reader has taken nothing");
    q.reader.available = 1;
    EXAMPLE_REQUIRE(request_step(&q) == 0 && q.length == -1 && q.reader.taken == 1,
                    "one byte: the reader took it and waits for the second");
    EXAMPLE_REQUIRE(request_step(&q) == 0 && q.length == -1 && q.reader.taken == 1,
                    "a call with nothing new changes nothing");
    q.reader.available = 2;
    EXAMPLE_REQUIRE(request_step(&q) == 1 && q.length == 258 && q.reader.taken == 2,
                    "both bytes: the child completes and the parent publishes 0x0102 in the same call");
    EXAMPLE_REQUIRE(q.reader.steps == 4, "the reader was stepped exactly once per call of the parent");
    EXAMPLE_REQUIRE(request_step(&q) == 1 && q.length == 258 && q.reader.steps == 4,
                    "done is sticky: a finished parent does not step the reader again");

    /* 첫 호출 전에 입력이 다 와 있으면 호출 한 번으로 끝난다. */
    static const unsigned char zero[] = { 0x00, 0x00 };
    request_init(&q, zero, 2);
    EXAMPLE_REQUIRE(request_step(&q) == 1 && q.length == 0,
                    "a length of zero is a result, not a pending child");
    request_init(&q, bytes, 2);
    EXAMPLE_REQUIRE(request_step(&q) == 1 && q.length == 258, "both bytes at once: 258 on the first call");

    printf("length: %d\n", q.length);
    return EXAMPLE_OK();
}
