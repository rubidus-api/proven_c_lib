#include "example.h"

/*
 * *침입형*(intrusive) 리스트는 여러분의 자료를 담을 노드를 따로 할당하는 대신, 연결
 * 고리를 여러분의 구조체 *안에* 둔다.
 *
 * 배울 때 본 리스트는 이렇게 생겼다.
 *
 *     struct node { struct node *next; void *data; };
 *
 * 넣을 때마다 노드를 할당하므로 항목 천 개짜리 리스트는 포인터를 담자고 존재하는 할당
 * 천 번이 되고, 하나하나가 실패할 기회이자 해제할 거리다. 더 나쁜 것은 `data` 가
 * void* 라는 점이다 - 리스트는 자기가 무엇을 담았는지 모르므로 읽을 때마다 컴파일러가
 * 검사할 수 없는 형변환을 한다.
 *
 * 침입형 리스트는 그것을 뒤집는다. 기억은 *여러분*이 소유하고 고리는 그 안에 산다.
 * 넣는 데 할당이 없으니 실패할 수 없다. 빼는 데도 할당이 없으니 실패할 수 없다. 그리고
 * 고리가 알려진 타입의 멤버이므로, 고리에서 물건으로 돌아오는 것은 형변환이 아니라
 * 컴파일러가 대신 해 주는 산술이다.
 *
 * 대가는 물건 하나가 자기가 가진 고리 멤버 수만큼의 리스트에만 들어갈 수 있다는 것이고,
 * 그것은 구조체를 선언할 때 여러분이 내리는 결정이다.
 */

/* 고리가 멤버다. 이 작업은 한 번에 정확히 한 리스트에만 들어갈 수 있다. */
typedef struct {
    int                 id;
    proven_list_node_t  link;
} task_t;

int main(void) {
    /* 이 프로그램에는 할당자가 아예 없다. 작업들은 스택에 있고, 리스트는 그 안에 사는
     * 포인터들을 다시 이어 줄 뿐이다. */
    task_t a = { .id = 1 };
    task_t b = { .id = 2 };
    task_t c = { .id = 3 };

    proven_list_t queue;
    proven_list_init(&queue);
    EXAMPLE_REQUIRE(proven_list_is_empty(&queue), "a freshly initialised list is empty");

    /* --- 넣기는 실패할 수 없다. 할당하는 것이 없으므로 ----------------- */
    proven_list_push_back(&queue, &a.link);
    proven_list_push_back(&queue, &b.link);
    proven_list_push_back(&queue, &c.link);
    EXAMPLE_REQUIRE(!proven_list_is_empty(&queue), "three tasks are queued");

    /* --- 훑기: PROVEN_LIST_ENTRY 가 고리에서 물건을 되찾아 준다 -------- */
    proven_list_node_t *it = NULL;
    int seen[3] = {0}, n = 0;
    PROVEN_LIST_FOR_EACH(it, &queue) {
        task_t *t = PROVEN_LIST_ENTRY(it, task_t, link);
        if (n < 3) seen[n++] = t->id;
    }
    EXAMPLE_REQUIRE(n == 3 && seen[0] == 1 && seen[1] == 2 && seen[2] == 3,
                    "the walk visits every task, in insertion order");

    /* --- 훑으면서 빼려면 *안전한* 꼴이 필요하다 ------------------------ */
    /* proven_list_remove 는 노드 자신의 next/prev 포인터를 통해 쓴다. 그래서 그냥
     * FOR_EACH 를 쓰면 방금 떼어 낸 노드에서 `it->next` 를 읽게 된다. _SAFE 꼴은 본문이
     * 돌기 *전에* 다음 포인터를 읽어 둔다. */
    proven_list_node_t *safe = NULL;
    PROVEN_LIST_FOR_EACH_SAFE(it, safe, &queue) {
        task_t *t = PROVEN_LIST_ENTRY(it, task_t, link);
        if (t->id == 2) proven_list_remove(it);
    }

    n = 0;
    PROVEN_LIST_FOR_EACH(it, &queue) {
        task_t *t = PROVEN_LIST_ENTRY(it, task_t, link);
        if (n < 3) seen[n++] = t->id;
    }
    EXAMPLE_REQUIRE(n == 2 && seen[0] == 1 && seen[1] == 3, "task 2 was unlinked");

    /* --- 가운데에 끼워 넣기는 포인터 바꿔 끼우기다 --------------------- */
    task_t d = { .id = 4 };
    proven_list_insert_after(&a.link, &d.link);

    n = 0;
    PROVEN_LIST_FOR_EACH(it, &queue) {
        task_t *t = PROVEN_LIST_ENTRY(it, task_t, link);
        if (n < 3) seen[n++] = t->id;
    }
    EXAMPLE_REQUIRE(n == 3 && seen[0] == 1 && seen[1] == 4 && seen[2] == 3,
                    "task 4 sits directly after task 1");

    /* 지울 것이 없다. 리스트는 아무것도 소유한 적이 없다. `a`, `c`, `d` 는 여전히
     * 멀쩡한 지역 변수이고, 그 수명은 리스트가 없었을 때와 똑같이 이 함수의 것이다. */
    return EXAMPLE_OK();
}
