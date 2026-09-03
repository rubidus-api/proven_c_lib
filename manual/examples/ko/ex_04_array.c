#include "example.h"

/*
 * proven_array_t 는 원소 타입 하나짜리 늘어나는 벡터다. 만들 때 쓴 할당자를 스스로
 * 담아 두므로 push 는 늘릴 수 있고 destroy 는 할당자를 다시 받지 않고도 해제할 수
 * 있다 - 뒤집어 말하면 배열은 *자기 자신만으로* 지워져야 한다는 뜻이다.
 *
 * PROVEN_ARRAY_* 매크로는 void* 로 된 속을 타입 안전하게 감싼 얼굴이다. 타입에서
 * sizeof/alignof 를 뽑고 원소를 포인터로 건네므로, 레코드 배열에 int 를 밀어 넣는
 * 코드는 컴파일되지 않는다.
 */

/* 우선순위를 키로 하는 작업 큐. 실제 자료는 값의 가짓수가 적다 - 우선순위 셋에 작업은
 * 여럿. 그 사실이 정렬에서 중요하다. 아래를 볼 것. */
typedef struct {
    int priority;
    int id;
} task_t;

/* 비교자가 곧 배열의 순서다. 정렬과 이진 탐색에는 *반드시* 같은 것을 써야 한다 -
 * 다른 순서로 찾는 것은 컴파일러가 볼 수 없는 버그다.
 *
 * (x > y) - (x < y) 꼴은 일부러 쓴 것이다. 두 int 를 빼면 큰 값에서 넘쳐 엉뚱한
 * 부호를 돌려준다. */
static int cmp_priority(const void *a, const void *b) {
    int x = ((const task_t *)a)->priority;
    int y = ((const task_t *)b)->priority;
    return (x > y) - (x < y);
}

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    /* --- push / get / pop --------------------------------------------------- */
    /* init_cap 은 한계가 아니라 힌트다. push 는 그것을 넘어 늘어난다. 잘 잡아 두면
     * 재할당을 피할 뿐이다. */
    proven_result_array_t r = PROVEN_ARRAY_INIT(alloc, task_t, 4);
    EXAMPLE_REQUIRE(proven_is_ok(r.err), "creating an array of task_t must succeed");
    if (!proven_is_ok(r.err)) {
        return 1;
    }
    proven_array_t tasks = r.value;

    /* 일부러 중복이 많게 했다. 실제 키가 그렇게 생겼기 때문이다 - 상태 열, enum,
     * 버킷 번호. */
    static const task_t seed[] = {
        { .priority = 2, .id = 10 }, { .priority = 0, .id = 11 },
        { .priority = 2, .id = 12 }, { .priority = 1, .id = 13 },
        { .priority = 2, .id = 14 }, { .priority = 0, .id = 15 },
        { .priority = 1, .id = 16 }, { .priority = 2, .id = 17 },
    };

    for (proven_size_t i = 0; i < sizeof seed / sizeof seed[0]; ++i) {
        proven_err_t err = PROVEN_ARRAY_PUSH(&tasks, task_t, seed[i]);
        EXAMPLE_REQUIRE(proven_is_ok(err), "pushing into a growable array must succeed");
        if (!proven_is_ok(err)) {
            PROVEN_ARRAY_DESTROY(&tasks);
            return 1;
        }
    }
    EXAMPLE_REQUIRE(tasks.len == 8, "eight pushes give eight elements");

    /* get 은 배열 저장소 *안*을 가리키는 포인터를 돌려준다 - 사본이 아니고, 다음
     * push 가 재할당하면 매달린 포인터가 된다. push 를 다 한 뒤에 얻어 쓰고, 담아
     * 두지는 말 것. */
    const task_t *front = PROVEN_ARRAY_GET(&tasks, task_t, 0);
    EXAMPLE_REQUIRE(front && front->id == 10, "element 0 is the first one pushed");

    /* 범위를 벗어나면 널 포인터다. 죽는 것도, 미정의 동작도 아니다. */
    EXAMPLE_REQUIRE(PROVEN_ARRAY_GET(&tasks, task_t, 99) == NULL, "an out-of-range index yields NULL");

    /* pop 은 마지막 원소를 복사해 내보낸다(그냥 버리려면 NULL 을 건넨다). */
    task_t last = {0};
    proven_err_t err = PROVEN_ARRAY_POP(&tasks, task_t, &last);
    EXAMPLE_REQUIRE(proven_is_ok(err), "popping a non-empty array must succeed");
    EXAMPLE_REQUIRE(last.id == 17 && tasks.len == 7, "pop returns the last element and shrinks the array");

    /* 도로 넣는다. 이 예제의 나머지가 여덟 개를 모두 필요로 한다. */
    err = PROVEN_ARRAY_PUSH(&tasks, task_t, last);
    EXAMPLE_REQUIRE(proven_is_ok(err), "pushing the popped element back must succeed");

    /* --- 정렬 -------------------------------------------------------------- */
    /* 인트로소트다. O(n log n) 은 평균이 아니라 보장이다 - 힙소트로 물러나는 길이
     * 있어, 상대가 유도할 수 있는 제곱 시간 경우를 배제한다. 그리고 여기서는 같은 키가
     * *빠른* 쪽이다. 피벗과 같은 것은 모두 한 구간으로 갈라 두고 다시 들어가지 않으므로,
     * 위의 중복된 우선순위는 서로 다른 값보다 값이 덜 든다.
     *
     * 안정 정렬은 아니다. 작업 10 과 12 는 어느 쪽이 먼저 나올지 정해져 있지 않다. */
    proven_array_sort(&tasks, cmp_priority);

    for (proven_size_t i = 1; i < tasks.len; ++i) {
        const task_t *prev = PROVEN_ARRAY_GET(&tasks, task_t, i - 1);
        const task_t *cur  = PROVEN_ARRAY_GET(&tasks, task_t, i);
        EXAMPLE_REQUIRE(prev && cur && prev->priority <= cur->priority,
                        "after sorting, priorities must be non-decreasing");
    }

    /* --- 이진 탐색 --------------------------------------------------------- */
    /* 바로 이 비교자로 정렬해 두었기 때문에만 옳다. 키는 원소 하나 통째지만, 비교자가
     * 읽는 필드만 뜻이 있다. */
    task_t key = { .priority = 1, .id = 0 };
    const task_t *hit = (const task_t *)proven_array_binary_search(&tasks, &key, cmp_priority);
    EXAMPLE_REQUIRE(hit != NULL, "priority 1 is present, so the search must find it");
    EXAMPLE_REQUIRE(hit && hit->priority == 1, "the hit must be an element with the searched key");
    /* 키가 중복되면 그 키를 가진 *어떤* 원소를 찾지, 첫 번째를 찾지는 않는다. 첫
     * 번째가 필요하면 찾은 자리에서 뒤로 훑을 것. */

    task_t absent = { .priority = 9, .id = 0 };
    EXAMPLE_REQUIRE(proven_array_binary_search(&tasks, &absent, cmp_priority) == NULL,
                    "a key that is not there must return NULL");

    printf("tasks: %zu sorted, found priority %d (id %d)\n",
           (size_t)tasks.len, hit ? hit->priority : -1, hit ? hit->id : -1);

    /* 배열은 원소 저장소를 소유한다. destroy 는 배열을 만들 때 쓴 할당자로 그것을
     * 돌려준다. 여기 원소는 그냥 자료다 - 원소가 무언가를 소유했다면 하나씩 먼저
     * 지워야 했을 것이다. */
    PROVEN_ARRAY_DESTROY(&tasks);
    return EXAMPLE_OK();
}
