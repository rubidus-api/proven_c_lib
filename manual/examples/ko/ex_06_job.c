#include "example.h"

#include <stdatomic.h>

/*
 * 작업 시스템: 일꾼 스레드와 경계가 있는 원자적 MPMC 큐. 놀고 있는 일꾼은 플랫폼
 * 동기화에 세워 둔다. 이것이 순서를 정해 주는 것은 일의 *넘김*이지, 그 일이 건드리는
 * 자료가 아니다. 아래 계수기가 그냥 int 가 아니라 atomic 인 이유가 그것이다 - 두 작업이
 * 같은 변수를 올리는 것은 부르는 쪽이 달리 말하지 않는 한 자료 경합이다.
 *
 * 수명은 곧은 한 줄이고, 선택 사항이 아니다.
 *
 *     init -> submit... -> close -> destroy
 *
 * destroy 는 submit 과 경합해서는 안 된다. 라이브러리 안의 어느 것도 그것을 강제하지
 * 않는다. 부르는 쪽이 먼저 생산자를 멈춰야 한다. 여기서는 생산자가 이 스레드 하나뿐이라,
 * "생산자를 멈춘다" 는 것은 "닫기 전에 submit 반복문을 끝낸다" 는 뜻이다.
 */

#define JOB_COUNT 64

static void increment(void *arg) {
    atomic_int *counter = arg;
    /* relaxed 로 충분하다. 우리는 합계가 맞기만 하면 되지, 그것에 맞춰 무엇의 순서를
     * 정할 필요가 없다. 결과를 우리에게 공표하는 것은 destroy 안의 join 이다. */
    atomic_fetch_add_explicit(counter, 1, memory_order_relaxed);
}

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    proven_job_sys_t *sys = NULL;
    /* 큐 용량은 2의 거듭제곱이어야 한다 - 고리가 마스크로 순번을 칸에 대응시킨다.
     * JOB_COUNT 보다 크게 잡아, 일꾼이 모두 아직 뜨는 중이어도 submit 이 고리가 가득 찬
     * 것을 만나지 않게 했다. */
    proven_err_t err = proven_job_system_init(alloc, 4, 128, &sys);
    EXAMPLE_REQUIRE(proven_is_ok(err), "starting a job system with 4 workers should succeed");
    if (!proven_is_ok(err)) return 1;

    /* destroy 뒤까지 산다. 작업의 인자는 작업보다 오래 살아야 하고, 작업은 destroy 가
     * 큐를 비울 때까지 돈다. */
    atomic_int counter = 0;

    proven_size_t submitted = 0;
    for (proven_size_t i = 0; i < JOB_COUNT; ++i) {
        /* submit 은 고리가 가득 찼거나 시스템이 닫혔을 때 false 를 돌려준다. 큐에 자리가
         * 나기를 기다리는 일도, 일을 조용히 버리는 일도 없다. 깨우는 경로에서 잠깐 플랫폼
         * 동기화에 들어갈 수는 있다. 그 답을 무시하는 것이 작업을 잃는 방법이고, 그래서
         * [[nodiscard]] 가 붙어 있다. */
        if (!proven_job_submit(sys, increment, &counter)) {
            /* 실제 부르는 쪽이라면 물러났다 다시 시도하거나 proven_job_execute_one 으로
             * 그 자리에서 돌릴 것이다. 여기서 고리가 가득 찼다는 것은 위의 크기 잡기가
             * 틀렸다는 뜻이므로, 덮어 두지 말고 그렇다고 말한다. */
            EXAMPLE_REQUIRE(false, "the queue was sized to hold every job");
            break;
        }
        ++submitted;
    }

    /* 이 스레드가 유일한 생산자이고 submit 을 마쳤다 - 그러니 닫아도 안전하다. close 는
     * 그 뒤의 submit 을 모두 실패시킨다. 이미 줄에 선 작업은 그대로 돈다. */
    proven_job_system_close(sys);

    /* destroy 는 큐가 비고 모든 일꾼이 join 될 때까지 막는다. 그 join 이 동기화 지점이다.
     * destroy 가 돌아온 뒤에는 모든 작업의 모든 기억 효과가 이 스레드에 보인다. 이 줄
     * 앞에서 `counter` 를 읽는 것은 일꾼들이 아직 쓰고 있는 값을 읽는 것이다. */
    proven_job_system_destroy(sys);

    int ran = atomic_load(&counter);
    EXAMPLE_REQUIRE(submitted == JOB_COUNT, "every job should have been accepted");
    EXAMPLE_REQUIRE(ran == (int)submitted, "every submitted job should have run exactly once");

    printf("submitted %zu jobs, %d ran\n", (size_t)submitted, ran);

    return EXAMPLE_OK();
}
