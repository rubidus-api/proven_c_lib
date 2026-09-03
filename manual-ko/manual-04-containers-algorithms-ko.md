# 4장: 컨테이너와 알고리즘

**3부 — 자료구조. 선행 조건: 2부
([1](manual-01-foundation-ko.md), [2](manual-02-allocation-ko.md), [3](manual-03-strings-text-ko.md)).**
**이 장을 마치면** 작업에 맞는 컨테이너를 고르고, 보장된 한계 안에서 정렬하고 검색하며,
올바른 이유로 해싱하고, 바이트를 텍스트로 또 그 반대로 바꿀 수 있다.

이 장은 `array.h`, `list.h`, `ring.h`, `map.h`, `algorithm.h`, `hash.h`, `encode.h`를 다룬다.
여기 있는 모든 컨테이너는 할당자(allocator)를 받는다. 2장이 먼저 오는 이유가 그것이다.

## 목차

1. [동적 배열](#1-동적-배열)
2. [침습적(intrusive) 리스트](#2-침습적intrusive-리스트)
3. [링 버퍼](#3-링-버퍼)
4. [해시 맵](#4-해시-맵)
5. [알고리즘](#5-알고리즘)
6. [용도별 해싱](#6-용도별-해싱)
7. [바이트를 텍스트로: hex와 Base64](#7-바이트를-텍스트로-hex와-base64)
8. [예제와 오용 사례](#8-예제와-오용-사례)

## 1. 동적 배열

### 문제: 매번 손으로 다시 쓰는 그 배열

확장 가능한 배열은 모든 C 프로그램이 결국 한 번은 작성하는 자료구조이며, 매번 조금씩 다르게
작성된다:

```text
int *items = malloc(cap * sizeof(int));
/* ... later ... */
if (n == cap) {
    cap *= 2;
    items = realloc(items, cap * sizeof(int));   /* wrong on two counts */
    items[n++] = x;
}
```

한 줄에 버그가 둘 있고, 둘 다 고전이다. `cap * sizeof(int)`는 큰 `cap`에 대해 **랩어라운드**할 수
있고, 그러면 거대한 개수에 대해 작은 할당이 만들어진다 — [1장 §4](manual-01-foundation-ko.md)를
보라. 그리고 `realloc`의 결과를 곧바로 `items`에 대입하면 **NULL을 반환할 때 옛 블록이 누수된다.**
원래 포인터가 사라졌는데 그것이 가리키던 메모리는 여전히 할당된 채로 남기 때문이다.

버그를 빼고 보더라도, 손으로 쓴 버전은 원소 타입마다 다시 써야 하거나, `void *`로 제네릭하게
만들어 타입 검사를 잃어야 한다.

### 이 라이브러리는 대신 무엇을 하는가

`proven_array_t`는 생성될 때의 원소 크기와 정렬을 유지하는 확장 벡터다. 그래서 템플릿 없이도,
호출 지점에서 `void *`를 쓰지 않고도 어떤 타입에나 동작한다 — `PROVEN_ARRAY_*` 매크로가 타입을
받아, 컴파일러가 아직 검사할 수 있는 자리에서 캐스팅을 수행한다.

- 확장은 검사된 산술을 사용하므로, 위의 오버플로는 작은 할당이 아니라 `PROVEN_ERR_OVERFLOW`가
  된다.
- 확장은 **실패-원자적(failure-atomic)**이다: 확장할 수 없으면 기존 원소는 손대지 않은 채로
  여전히 유효하다. 아무것도 누수되지 않고 아무것도 잃지 않는다.
- 문자열 타입과 달리 **allocator를 내부에 저장한다** — `PROVEN_ARRAY_DESTROY`가 배열만 받는
  이유가 그것이다.

`proven_array_t`는 연속된 원소 저장소를 소유하므로, `proven_array_sort`와 §5의 검색들에
넘기는 것도 바로 이것이다.

잘못된 예 — push를 건너 포인터를 붙잡고 있기:

```text
int *first = PROVEN_ARRAY_GET(&arr, int, 0);
(void)PROVEN_ARRAY_PUSH(&arr, int, 42);   /* may reallocate */
*first = 7;                               /* wrong: `first` may point at freed memory */
```

이것은 타입이 제거해 줄 수 없는 유일한 위험이다. 확장은 저장소를 이동시키므로, **배열을 가리키는
모든 포인터나 뷰(view)는 확장을 유발할 수 있는 모든 연산에 의해 무효화된다.** 가리키는 대신 인덱스를
쓰거나, push 이후에 포인터를 다시 가져오라.


### 구조체

```text
typedef struct {
    proven_allocator_t alloc;
    proven_byte_t *data;
    proven_size_t len;
    proven_size_t cap;
    proven_size_t elem_size;
    proven_size_t align;
} proven_array_t;

typedef struct {
    proven_err_t err;
    proven_array_t value;
} proven_result_array_t;
```

필드:

- `alloc`: 성장과 파괴에 쓰이는 allocator.
- `data`: 원소를 위한 바이트 저장소.
- `len`: 현재 원소 개수.
- `cap`: 원소 단위 용량.
- `elem_size`: 각 원소의 크기.
- `align`: 각 원소의 정렬.

### 함수

| API | 의도 | 반환 |
|---|---|---|
| `proven_array_create(alloc, init_cap, elem_size, align)` | 제네릭 배열을 만든다. | `proven_result_array_t`. |
| `proven_array_is_valid(arr)` | 공개 배열 불변식을 검증한다. | `bool`. |
| `proven_array_reserve(arr, new_cap)` | 최소 `new_cap`개의 원소를 보장한다. | `proven_err_t`. |
| `proven_array_push(arr, element)` | 포인터로부터 복사하여 원소 하나를 추가한다. | `proven_err_t`. |
| `proven_array_pop(arr, out_element)` | 마지막 원소를 꺼낸다; `out_element`가 null이면 버린다. | `proven_err_t`. |
| `proven_array_get_mut(arr, index)` | 가변 원소 포인터를 얻는다. | 포인터 또는 null. |
| `proven_array_get(arr, index)` | const 원소 포인터를 얻는다. | 포인터 또는 null. |
| `proven_array_destroy(arr)` | 저장소를 해제하고 상태를 비운다. | void. |

### 매크로

| 매크로 | 의도 |
|---|---|
| `PROVEN_ARRAY_INIT(alloc, type, init_cap)` | `sizeof(type)`와 `alignof(type)`를 사용하는 타입 안전 생성. |
| `PROVEN_ARRAY_PUSH(arr_ptr, type, value)` | 임시 복합 리터럴로 rvalue나 lvalue를 push한다. |
| `PROVEN_ARRAY_POP(arr_ptr, type, out_ptr)` | 출력 포인터로 pop하거나 null로 버린다. |
| `PROVEN_ARRAY_GET(arr_ptr, type, index)` | 타입이 지정된 const 원소 포인터. |
| `PROVEN_ARRAY_GET_MUT(arr_ptr, type, index)` | 타입이 지정된 가변 원소 포인터. |
| `PROVEN_ARRAY_DESTROY(arr_ptr)` | 배열을 파괴한다. |

예제:

```c
proven_result_array_t r = PROVEN_ARRAY_INIT(alloc, int, 4);
if (!proven_is_ok(r.err)) {
    return;
}
proven_array_t nums = r.value;

(void)PROVEN_ARRAY_PUSH(&nums, int, 10);
(void)PROVEN_ARRAY_PUSH(&nums, int, 20);

/* GET points into the array's own storage, and returns NULL out of range. */
const int *first = PROVEN_ARRAY_GET(&nums, int, 0);
if (first) {
    proven_println("first={}", PROVEN_ARG(*first));
}

/* The array stores its allocator, so destroy needs nothing but the array. */
PROVEN_ARRAY_DESTROY(&nums);
```

## 2. 침습적(intrusive) 리스트

### 문제: 배운 대로의 리스트는 원소마다 노드를 할당한다

```text
struct node { struct node *next; void *data; };
```

이것이 교과서의 연결 리스트이며, 교과서가 언급하지 않는 두 가지 비용이 있다. 삽입할 때마다
**할당한다** — 천 개의 항목은 곧 포인터를 담기 위해서만 존재하는 천 번의 할당이고, 각각은 실패할
수 있으며 각각은 짝이 되는 해제를 필요로 한다. 그리고 `data`는 `void *`이므로 리스트는 자신이
무엇을 담고 있는지 전혀 모른다: 다시 읽어낼 때마다 컴파일러가 검사할 수 없는 캐스트가 된다.

### 침습적 리스트는 대신 무엇을 하는가

관계를 뒤집는다: **메모리는 당신이 소유하고, 링크가 그 안에 산다.**

```text
typedef struct {
    int                 id;
    proven_list_node_t  link;   /* the list's hook, inside your struct */
} task_t;
```

이제 삽입은 포인터 두 개를 쓰는 일이다. 아무것도 할당하지 않으므로 **실패할 수 없다** —
`proven_list_push_back`이 `void`를 반환한다는 점에 주목하라. 이 라이브러리에서는 이례적인 일이고,
바로 그것이 요점이다. 제거도 마찬가지다. 객체는 스택에도, 배열 안에도, 아레나 안에도, 어디에나
살 수 있다. 리스트는 이미 그 안에 들어 있는 포인터들을 재배치할 뿐이다.

링크에서 다시 당신의 객체로 돌아가는 것은 `PROVEN_LIST_ENTRY(node, task_t, link)`이며, 노드의
주소에서 멤버의 오프셋을 뺀다. 이것은 맞기를 바라는 캐스트가 아니라, 컴파일러가 타입을 검사하는
산술이다.

포기하는 것: **객체는 자신이 가진 링크 멤버 수만큼의 리스트에만 속할 수 있고**, 그 수는 구조체를
선언할 때 당신이 정한다. 어떤 항목이 큐와 인덱스에 동시에 있어야 한다면, 링크가 둘 필요하다.

`proven_list_t`는 침습적 이중 연결 **원형** 리스트다 — 헤드가 sentinel 노드이므로, 삽입과 제거가
양 끝을 특별히 취급하는 일이 없다. 노드를 할당하지 않는다.

잘못된 예 — 평범한 반복자로 순회하면서 제거하기:

```text
PROVEN_LIST_FOR_EACH(it, &queue) {
    if (should_drop(it)) proven_list_remove(it);   /* wrong: it->next is read after unlink */
}
```

`proven_list_remove`는 노드 자신의 포인터를 통해 쓰기를 하므로, 루프는 방금 떼어낸 노드에서
`next`를 읽게 된다. `PROVEN_LIST_FOR_EACH_SAFE`는 두 번째 변수를 받아 본문이 실행되기 *전에* 다음
포인터를 읽는다. 그것이 바로 이 매크로가 존재하는 이유다.

### 완성 예제: 링크가 호출자의 구조체 안에 사는 큐

테스트 스위트가 컴파일하고 실행한다. 스택에 할당된 task들의 큐를 만들고, 순회하고, 안전한 순회
안에서 하나를 제거하고, 중간에 삽입한다 — 프로그램 어디에도 allocator가 없다.

<!-- example: manual/examples/ko/ex_04_list.c -->
```c
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
```

### 구조체

```text
typedef struct proven_list_node_t {
    struct proven_list_node_t *next;
    struct proven_list_node_t *prev;
} proven_list_node_t;

typedef struct {
    proven_list_node_t head;
} proven_list_t;
```

### 함수와 매크로

| API | 의도 | 반환 |
|---|---|---|
| `proven_list_init(list)` | 원형 센티넬을 초기화한다. | void. |
| `proven_list_insert_after(target, node)` | `target` 뒤에 `node`를 삽입한다. | void. |
| `proven_list_push_back(list, node)` | 꼬리에 삽입한다. | void. |
| `proven_list_remove(node)` | 노드를 떼어내고 링크를 null로 오염(poison)시킨다. | void. |
| `proven_list_is_empty(list)` | 빈 리스트 또는 null 리스트인지 검사한다. | int 진리값. |
| `PROVEN_CONTAINER_OF(ptr, type, member)` | 멤버 포인터를 부모 객체 포인터로 변환한다. | 포인터. |
| `PROVEN_LIST_FOR_EACH(iter, list)` | 노드를 순회한다. | 루프 매크로. |
| `PROVEN_LIST_FOR_EACH_SAFE(iter, safe_next, list)` | 현재 노드를 제거하면서 안전하게 순회한다. | 루프 매크로. |
| `PROVEN_LIST_ENTRY(ptr, type, member)` | 리스트 노드를 담고 있는 객체로 변환한다. | 포인터. |

예제:

```c
typedef struct Item {
    int value;
    proven_list_node_t link;   /* the list lives inside the object; it allocates nothing */
} Item;

proven_list_t list;
proven_list_init(&list);

Item a = { .value = 1 };
Item b = { .value = 2 };
proven_list_push_back(&list, &a.link);
proven_list_push_back(&list, &b.link);

int total = 0;
proven_list_node_t *it = NULL;
PROVEN_LIST_FOR_EACH(it, &list) {
    /* The iterator walks nodes; ENTRY converts a node back to its owner. */
    Item *item = PROVEN_LIST_ENTRY(it, Item, link);
    total += item->value;
}
proven_println("total={}", PROVEN_ARG(total));   /* 3 */
```

## 3. 링 버퍼

### 문제: 커져서는 안 되는 큐

빠른 생산자와 느린 소비자 사이의 큐는 한 가지 질문에 답해야 한다: 소비자가 뒤처지면 무슨 일이
일어나는가? 확장 가능한 큐는 "더 할당한다"고 답하는데, 그러면 일시적인 지연이 무한한 메모리
증가로, 그리고 결국에는 작업을 버리는 것보다 더 나쁜 무언가로 바뀐다.

손으로 쓰는 대안은 배열 하나에 head 인덱스, tail 인덱스, 그리고 모듈로 산술을 더한 것이다 —
그리고 여기에는 유명한 버그가 하나 있다. `head == tail`일 때 버퍼는 비어 있는가, 가득 찼는가?
별도의 개수를 유지하거나 슬롯 하나를 일부러 낭비하지 않는 한 두 상태는 똑같아 보이고, 프로그래머
세대마다 이것을 다시 발견한다.

### 이 라이브러리는 대신 무엇을 하는가

`proven_ring_t`는 개수를 유지하는 고정 용량 FIFO다. 그래서 비어 있음과 가득 참이 구별되며,
커지는 대신 **거부한다**:

- 가득 찬 링에 대한 `proven_ring_push`는 `PROVEN_ERR_OUT_OF_BOUNDS`를 반환한다. 가장 오래된
  항목을 덮어쓰지 않고, 재할당도 하지 않는다. 호출자가 결정한다 — 기다리거나, 새 항목을 버리거나,
  배압(backpressure)을 보고하거나 — 무엇이 옳은지는 호출자만 알기 때문이다.
- 비어 있는 링에 대한 `proven_ring_pop`은 낡은 슬롯을 되돌려주는 대신 실패한다.

용량은 생성 시점에 한 번 정해지며, 그 숫자가 *곧* 정책이다: 생산자가 얼마나 앞서 나가도 되는지를
말한다. 이벤트 큐, 최근 항목의 로그, 오디오나 센서 버퍼 — 한계가 제약이 아니라 설계의 일부인
곳이라면 링을 꺼내라.

이 라이브러리의 대부분의 타입과 달리, `proven_ring_t`는 **allocator를 내부에 저장한다.**
`PROVEN_RING_DESTROY`가 링만 받는 이유가 그것이다.

잘못된 예 — 가득 찬 링을 즉시 재시도할 오류로 취급하기:

```text
while (PROVEN_RING_PUSH(&ring, event_t, e) != PROVEN_OK) { }   /* wrong: spins forever */
```

그 루프 안의 무엇도 소비하지 않으므로 링은 계속 가득 차 있다. 한계가 있는 큐의 거부는 *소비자*에
대한 정보이며, 그것을 무시하는 루프는 곧 멈춤(hang)이다.

### 완성 예제: 가득 찰 때까지 push하기, 그리고 거부가 어떤 모습인지

테스트 스위트가 컴파일하고 실행한다.

<!-- example: manual/examples/ko/ex_04_ring.c -->
```c
/*
 * 고리 버퍼는 내용을 옮기지도, 늘어나지도 않는 고정 크기 큐다. 용량을 한 번 정해 주면
 * push 는 꼬리에 넣고 pop 은 머리에서 빼며, 가득 차면 push 는 *거부한다*.
 *
 * 이것이 없으면 여러분이 쓰게 될 C 는 배열 하나에 인덱스 둘, 그리고 그것을 감아 주는
 * 나머지 연산이고, 버그는 언제나 같다 - "지금 가득 찬 것인가 빈 것인가?" 개수를 따로
 * 세거나 칸 하나를 버리지 않는 한 두 상태 모두 head == tail 이다. 이 고리는 개수를
 * 세므로 그 물음이 생기지 않는다.
 *
 * 만드는 쪽과 쓰는 쪽의 속도가 다르고 만드는 쪽이 얼마나 앞서 나갈지에 단단한 한계를
 * 두고 싶을 때 쓴다 - 이벤트 큐, 로그 고리, 오디오 버퍼. 가득 찼을 때의 거부가 바로
 * 그 기능이다 - 역압(backpressure)이다. 늘어나는 큐는 몰려드는 입력에 기억을 먹어
 * 치우는 것으로 답하다가 더 나쁜 일을 맞는다.
 */

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    /* 이벤트 4개짜리 용량. 5가 되는 일은 없다. */
    proven_result_ring_t r = PROVEN_RING_INIT(alloc, int, 4);
    EXAMPLE_REQUIRE(proven_is_ok(r.err), "creating a 4-slot ring should succeed");
    proven_ring_t ring = r.value;

    /* --- 가득 찰 때까지 push ------------------------------------------- */
    for (int i = 1; i <= 4; ++i) {
        proven_err_t err = PROVEN_RING_PUSH(&ring, int, i);
        EXAMPLE_REQUIRE(proven_is_ok(err), "the first four pushes fit");
    }

    /* 다섯 번째 push 는 거부된다. 가장 오래된 항목을 덮어쓰지도, 늘어나지도 않는다.
     * 가득 찬 고리는 가득 찬 고리이고, 어떻게 할지는 부르는 쪽이 정한다 - 기다리든,
     * 새 이벤트를 버리든, 역압을 알리든. */
    proven_err_t full = PROVEN_RING_PUSH(&ring, int, 5);
    EXAMPLE_REQUIRE(full == PROVEN_ERR_OUT_OF_BOUNDS,
                    "pushing into a full ring must be refused, not silently absorbed");

    /* --- 넣은 차례대로 pop --------------------------------------------- */
    int out = 0;
    proven_err_t err = PROVEN_RING_POP(&ring, int, &out);
    EXAMPLE_REQUIRE(proven_is_ok(err) && out == 1, "pop returns the oldest entry first");

    /* 이제 자리가 다시 생겼고, 고리는 아무것도 옮기지 않은 채 저장소를 감아 돈다. */
    err = PROVEN_RING_PUSH(&ring, int, 5);
    EXAMPLE_REQUIRE(proven_is_ok(err), "after a pop there is room for one more");

    int expected[] = { 2, 3, 4, 5 };
    for (int i = 0; i < 4; ++i) {
        err = PROVEN_RING_POP(&ring, int, &out);
        EXAMPLE_REQUIRE(proven_is_ok(err) && out == expected[i],
                        "entries come out in the order they went in");
    }

    /* --- 빈 상태도 가득 찬 상태처럼 군다: 거부할 뿐 자료를 지어내지 않는다 --- */
    err = PROVEN_RING_POP(&ring, int, &out);
    EXAMPLE_REQUIRE(!proven_is_ok(err), "popping an empty ring must fail rather than return junk");

    PROVEN_RING_DESTROY(&ring);
    return EXAMPLE_OK();
}
```

### 구조체

```text
typedef struct {
    proven_allocator_t alloc;
    proven_mem_mut_t internal;
    proven_size_t head;
    proven_size_t tail;
    proven_size_t len;
    proven_size_t cap;
    proven_size_t elem_size;
    proven_size_t align;
} proven_ring_t;

typedef struct {
    proven_err_t err;
    proven_ring_t value;
} proven_result_ring_t;
```

### 함수와 매크로

| API | 의도 | 반환 |
|---|---|---|
| `proven_ring_create(alloc, cap, elem_size, align)` | 고정 용량 링을 만든다. | `proven_result_ring_t`. |
| `proven_ring_is_valid(ring)` | 링 불변식을 검증한다. | `bool`. |
| `proven_ring_push(ring, element)` | 원소 하나를 push한다; 가득 차면 실패한다. | `proven_err_t`. |
| `proven_ring_pop(ring, out_element)` | 원소 하나를 pop한다; null 출력이면 버린다. | `proven_err_t`. |
| `proven_ring_destroy(ring)` | 저장소를 해제한다. | void. |
| `PROVEN_RING_INIT(alloc, type, cap)` | 타입 안전 생성. | `proven_result_ring_t`. |
| `PROVEN_RING_PUSH(ring_ptr, type, value)` | 타입 안전 push. | `proven_err_t`. |
| `PROVEN_RING_POP(ring_ptr, type, out_ptr)` | 타입 안전 pop. | `proven_err_t`. |
| `PROVEN_RING_DESTROY(ring_ptr)` | 링을 파괴한다. | void. |

예제:

```c
proven_result_ring_t r = PROVEN_RING_INIT(alloc, int, 8);
if (!proven_is_ok(r.err)) {
    return;
}
proven_ring_t q = r.value;

proven_err_t e = PROVEN_RING_PUSH(&q, int, 7);
if (!proven_is_ok(e)) {
    /* The ring is fixed-capacity: a full ring is PROVEN_ERR_OUT_OF_BOUNDS,
     * never a silent grow. */
    PROVEN_RING_DESTROY(&q);
    return;
}

int out = 0;
e = PROVEN_RING_POP(&q, int, &out);   /* FIFO: out == 7. Empty ring is OUT_OF_BOUNDS. */
(void)e;

PROVEN_RING_DESTROY(&q);
```

## 4. 해시 맵

`proven_map_t`는 tombstone을 갖는 open-addressing 해시 맵이다. 정수 키와 빌려 쓰는(borrowed) 또는 소유(owned) U8 문자열 키를 지원한다. allocator를 내부에 저장한다.

### 구조체

```text
typedef enum {
    PROVEN_KEY_TYPE_INT,          /* keys are proven_size_t integers */
    PROVEN_KEY_TYPE_U8_BORROWED,  /* keys are u8 views; caller keeps the bytes alive */
    PROVEN_KEY_TYPE_U8_OWNED      /* keys are u8 views; the map copies and frees the bytes */
} proven_key_type_t;

typedef union {
    proven_size_t id;             /* used when key_type == PROVEN_KEY_TYPE_INT */
    proven_u8str_view_t str;      /* used for the two U8 key modes */
} proven_map_key_t;

typedef struct {
    proven_allocator_t alloc;     /* allocator for the bucket array and owned keys */
    proven_mem_mut_t internal;    /* the single contiguous bucket array (ptr + size) */
    proven_size_t len;            /* live entries */
    proven_size_t used;           /* live entries + tombstones (drives the load factor) */
    proven_size_t cap;            /* number of buckets, always a power of two */
    proven_size_t elem_size;      /* size of one stored value */
    proven_size_t align;          /* alignment requested for the value */
    proven_size_t bucket_stride;  /* bytes per bucket: align_up(header + elem_size) */
    proven_size_t payload_offset; /* bytes from a bucket start to its value payload */
    proven_key_type_t key_type;   /* key mode chosen at create time */
} proven_map_t;

typedef struct {
    proven_err_t err;
    proven_map_t value;
} proven_result_map_t;
```

### 내부 동작 원리

맵은 `cap`개의 버킷으로 이루어진 단일 평면 배열이다 — 값에 대해 항목별 할당이
없다(그리고 INT/BORROWED 모드에서는 키에 대해서도 없다). 그래서 조회가 캐시
친화적으로 유지된다. 각 버킷은 다음과 같이 배치된다:

```
[ header: state + key ][ padding to the value's alignment ][ value payload ]
^ bucket start                                              ^ payload_offset
|<------------------------- bucket_stride ------------------------------------>|
```

헤더의 `state`는 **EMPTY**(한 번도 사용 안 함), **OCCUPIED**(살아 있는 키+값을
보유), **TOMBSTONE**(제거된 항목) 중 하나다. `payload_offset`과 `bucket_stride`는
생성 시점에 `elem_size`/`align`으로부터 한 번 계산되므로, 버킷 `i`의 값 주소는 그냥
`internal.ptr + i*bucket_stride + payload_offset`이다.

- **해싱, 그리고 기본값이 왜 안전한 쪽인가.** 정수 키는 SplitMix/Murmur 스타일의
  비트 혼합 finaliser를 거친다(그래서 순차 id가 버킷 전체에 퍼진다).
  **문자열 키는 OS CSPRNG에서 한 번 뽑은 프로세스별 비밀 아래에서 keyed SipHash-2-4로
  해싱된다** — 왜냐하면 *신뢰할 수 없는* 키를 예측 가능한 함수로 해싱하는 맵은
  터지기를 기다리는 서비스 거부(DoS)이기 때문이다: 키를 통제하는 공격자는 오프라인에서
  충돌을 계산해 그 전부를 한 버킷에 몰아넣고, 모든 조회를 선형 스캔으로 바꾼다. 그들이
  볼 수 없는 비밀로 해시를 키잉하는 것이 그것을 막으며, 이는 Python, Rust, Linux 커널이
  자기네 테이블에 대해 내린 것과 같은 선택이다. 키가 전부 여러분 자신의 프로그램에서
  온다면, `proven_map_create_trusted`가 빠른 unkeyed FNV-1a를 대신 선택한다.
  `proven_map_hash`는 주어진 맵이 실제로 어느 함수를 쓰는지 알려준다. (CSPRNG도
  공격자 모델도 없는 프리스탠딩(freestanding) 타깃에서는 문자열 키가 FNV로 후퇴한다.)
- **탐사(Probing).** 선형 open addressing: 시작 버킷은 `hash & (cap - 1)`이며(`cap`이
  2의 거듭제곱이라 저렴하다), 그다음 검색은 한 번에 한 버킷씩 앞으로 걸어가며 감싸고,
  키(같은 키를 가진 OCCUPIED)나 EMPTY 버킷(키가 없음을 증명함)을 찾을 때까지 계속한다.
  선형 탐사는 걸어가는 버킷들을 메모리 상에서 연속되게 유지한다.
- **제거와 tombstone.** `proven_map_remove`는 버킷을 그냥 비울 수 없다 — 그러면 탐사
  체인이 끊겨 나중 키들이 숨겨진다. 대신 버킷을 TOMBSTONE으로 표시한다. Tombstone은
  조회에서 건너뛰어지지만 여전히 슬롯을 차지하므로, `len`이 아니라 `used`(살아 있는 것 +
  tombstone)가 성장을 견인한다.
- **부하율(load factor)과 리사이즈.** `used >= cap * 3/4`이 되면, 맵은 다음 2의
  거듭제곱 크기의 새 버킷 배열을 할당하고 모든 OCCUPIED 항목을 그리로 **재해싱**하며,
  그 과정에서 모든 tombstone을 버린다. 용량은 커지기만 하고, 결코 줄어들지 않는다.
  `proven_map_reserve`로 미리 예약하라(특히 아레나 allocator에서는, 뒤에 죽은 배열을
  남기지 않기 위해).

### 키 모드 — 하나 고르기

- `PROVEN_KEY_TYPE_INT`: 키는 `proven_size_t`다. 키 저장소 없음.
- `PROVEN_KEY_TYPE_U8_BORROWED`: 버킷이 *view*(포인터 + 길이)를 저장한다.
  **맵은 바이트를 절대 복사하지 않으므로**, 호출자는 항목이 존재하는 동안 내내 그
  정확한 바이트를 살아 있고 옮겨지지 않은 채로 유지해야 한다. 이미 안정된 어딘가에
  사는 키(문자열 리터럴, interned 문자열)에 가장 저렴하다.
- `PROVEN_KEY_TYPE_U8_OWNED`: 삽입 시(`proven_map_set_u8_owned` /
  `PROVEN_MAP_SET_U8_OWNED`) 맵이 키 바이트를 자기 저장소로 **복제**하고
  remove/destroy 시 해제하므로, 호출 직후 원본 버퍼를 해제하거나 재사용해도 된다.

### `set_with_scratch` / 별칭(alias) 사례

`proven_map_set`에 넘기는 `element`가 *맵 자신의 버킷 배열 안*을 가리킨다면(예를 들어
`proven_map_get`이 반환한 포인터를 통해 한 키에서 다른 키로 값을 복사하는 경우), 바로
그 삽입이 유발하는 재해싱이 여러분이 읽고 있는 바이트를 옮기거나 해제할 수 있다.
`proven_map_set_with_scratch`(그리고 `*_WITH_SCRATCH_*` 매크로)는 원본 바이트를 먼저
임시 작업용(scratch) allocator의 임시 버퍼로 담아두므로 삽입이 별칭 안전(alias-safe)해진다. 영구
저장소는 여전히 `map->alloc`을 쓴다. scratch allocator는 그 일시적 복사에만 쓰인다.

### 함수

| API | 의도 | 반환 |
|---|---|---|
| `proven_map_create(alloc, init_cap, key_type, elem_size, align)` | 맵을 만든다. | `proven_result_map_t`. |
| `proven_map_is_valid(map)` | 맵 불변식을 검증한다. | `bool`. |
| `proven_map_reserve(map, new_cap)` | 용량을 보장한다. | `proven_err_t`. |
| `proven_map_set_with_scratch(map, key, element, scratch)` | 임시 별칭 안전 복사에 scratch를 써서 삽입/갱신한다. | `proven_err_t`. |
| `proven_map_set(map, key, element)` | 삽입/갱신한다. | `proven_err_t`. |
| `proven_map_set_u8_owned(map, key, element)` | 맵이 소유하는 U8 키 저장소로 삽입/갱신한다. | `proven_err_t`. |
| `proven_map_get_mut(map, key)` | 가변 값을 조회한다. | 포인터 또는 null. |
| `proven_map_get(map, key)` | const 값을 조회한다. | 포인터 또는 null. |
| `proven_map_remove(map, key)` | 키가 있으면 제거한다. | `proven_err_t`. |
| `proven_map_destroy(map)` | 맵 저장소를 해제한다. | void. |

### 매크로

| 매크로 | 의도 |
|---|---|
| `proven_map_create_with_capacity(...)` | 용량 할당을 강조하는 별칭. |
| `PROVEN_MAP_INIT_INT(alloc, type, init_cap)` | 정수 키 타입 맵을 만든다. |
| `PROVEN_MAP_INIT_U8_BORROWED(alloc, type, init_cap)` | borrowed 문자열 키 타입 맵을 만든다. |
| `PROVEN_MAP_INIT_U8_OWNED(alloc, type, init_cap)` | owned 문자열 키 타입 맵을 만든다. |
| `PROVEN_MAP_SET_INT(map_ptr, int_key, type, value)` | 정수 키를 설정한다. |
| `PROVEN_MAP_SET_WITH_SCRATCH_INT(map_ptr, int_key, type, value, scratch)` | scratch allocator를 써서 정수 키를 설정한다. |
| `PROVEN_MAP_SET_U8_BORROWED(map_ptr, u8_view, type, value)` | borrowed U8 키를 설정한다. |
| `PROVEN_MAP_SET_U8_OWNED(map_ptr, u8_view, type, value)` | owned U8 키를 설정한다. |
| `PROVEN_MAP_SET_WITH_SCRATCH_U8_BORROWED(map_ptr, u8_view, type, value, scratch)` | scratch allocator를 써서 borrowed U8 키를 설정한다. |
| `PROVEN_MAP_GET_INT(map_ptr, type, int_key)` | 정수 키로 const 값을 얻는다. |
| `PROVEN_MAP_GET_U8_BORROWED(map_ptr, type, u8_view)` | borrowed U8 키로 const 값을 얻는다. |
| `PROVEN_MAP_GET_U8_OWNED(map_ptr, type, u8_view)` | owned U8 키로 const 값을 얻는다. |
| `PROVEN_MAP_GET_MUT_INT(map_ptr, type, int_key)` | 정수 키로 가변 값을 얻는다. |
| `PROVEN_MAP_GET_MUT_U8_BORROWED(map_ptr, type, u8_view)` | borrowed U8 키로 가변 값을 얻는다. |
| `PROVEN_MAP_GET_MUT_U8_OWNED(map_ptr, type, u8_view)` | owned U8 키로 가변 값을 얻는다. |
| `PROVEN_MAP_REMOVE_INT(map_ptr, int_key)` | 정수 키를 제거한다. |
| `PROVEN_MAP_REMOVE_U8_BORROWED(map_ptr, u8_view)` | U8 키를 제거한다. |
| `PROVEN_MAP_REMOVE_U8_OWNED(map_ptr, u8_view)` | owned U8 키를 제거한다. |
| `PROVEN_MAP_DESTROY(map_ptr)` | 맵을 파괴한다. |

owned 키 경로:

`PROVEN_KEY_TYPE_U8_OWNED`는 키 바이트의 복제본을 맵 내부에 저장한다. 원본 버퍼가 삽입 후 옮겨지거나 해제될 수 있을 때 사용하라. owned 바이트는 remove와 destroy 시 해제되며, 재해싱 동안 항목과 함께 이동한다.

`PROVEN_HARDENED`와 디버그 검증은 맵 자신의 내부 저장소 안에 들어가는 일부 borrowed 키 포인터를 거부할 수 있다. 그 검사는 방어적일 뿐이며, borrowed 키를 자기 소유(self-owning)로 만들지는 않는다.

예제:

```c
typedef struct UserInfo {
    int level;
    double budget;
} UserInfo;

proven_result_map_t r = PROVEN_MAP_INIT_INT(alloc, UserInfo, 8);
if (!proven_is_ok(r.err)) {
    return;
}
proven_map_t users = r.value;

UserInfo u = { .level = 3, .budget = 99.0 };
(void)PROVEN_MAP_SET_INT(&users, 404, UserInfo, u);

/* GET returns a pointer into the bucket array, or NULL when absent. Any insert
 * that rehashes invalidates it: look it up, use it, drop it. */
const UserInfo *found = PROVEN_MAP_GET_INT(&users, UserInfo, 404);
if (found) {
    proven_println("level={}", PROVEN_ARG(found->level));
}

PROVEN_MAP_DESTROY(&users);
```

## 5. 알고리즘

배열 알고리즘은 `proven_array_t`와 비교 콜백에 대해 동작한다.

### 비교 함수

```text
typedef int (*proven_compare_fn_t)(const void *a, const void *b);
```

`a < b`이면 음수, 같으면 0, `a > b`이면 양수를 반환한다.

### 함수

| API | 의도 | 반환 |
|---|---|---|
| `proven_array_sort(arr, cmp)` | 배열을 제자리(in place) 정렬한다. | void. |
| `proven_array_binary_search(arr, key, cmp)` | 정렬된 배열을 검색한다. | 원소 포인터 또는 null. |
| `proven_array_linear_search(arr, key, cmp)` | 임의의 배열을 스캔하며 검색한다. | 원소 포인터 또는 null. |

`proven_array_sort`는 introsort다: Bentley-McIlroy 3방향 분할, 작은 범위에 대한
insertion-sort 컷오프, 그리고 재귀가 `2*log2(n)` 레벨을 넘으면 쓰이는 heapsort
fallback으로 이루어진다.

명시할 만한 성질이 둘 있다. 실제로 발목을 잡는 것들이기 때문이다:

- **O(n log n)은 전형적인 경우가 아니라 보장이다.** heapsort fallback이 그것을
  보장으로 만든다. 적대적 순서로 최악의 경우에 도달할 수 있는 정렬은, 자기가 작성하지
  않은 데이터를 정렬하는 어떤 프로그램에서도 서비스 거부다.
- **중복 키는 느린 경우가 아니라 빠른 경우다.** 피벗과 같은 원소들은 최종적이며 결코
  재귀되지 않는 런(run)으로 모이므로, 전부-같은 입력은 한 번의 패스로 끝난다. 이것이
  중요한 이유는 저기수성(low-cardinality) 키 — 상태 열, enum, 버킷 id — 가 실제로
  호출자들이 정렬 기준으로 삼는 것이고, 그것들이 바로 순진한 2방향 분할이 퇴화하는
  대상이기 때문이다.

정렬은 안정(stable)하지 않다: 같은 원소들은 재배열될 수 있다.

비교자는 평범한 파일 스코프 함수이므로, 형태는 이렇다:

```text
static int cmp_int(const void *a, const void *b) {
    int x = *(const int *)a;
    int y = *(const int *)b;
    return (x > y) - (x < y);   /* not (x - y): that overflows */
}

proven_array_sort(&nums, cmp_int);
int key = 20;
int *hit = proven_array_binary_search(&nums, &key, cmp_int);
```

이 장 끝의 완성 예제(`manual/examples/ex_04_array.c`)가
바로 이것을 컴파일하고 실행한 버전이다.

## 6. 용도별 해싱

단 하나의 "해시"란 없다. 어느 함수가 올바른지는 그 결과로 무엇을 하려는지에 전적으로
달려 있으며, 잘못된 것을 집으면 쓸데없이 느린 프로그램이나 조용히 안전하지 않은
프로그램을 얻는다. `proven/hash.h`는 작업당 정확히 하나의 primitive를 제공하므로,
작업 이름을 대는 순간 선택이 끝난다:

| 여러분이... | 사용 | 그리고 결정적으로 |
|---|---|---|
| **여러분 자신의** 테이블에 키를 해싱, **신뢰된** 입력 | `proven_hash_bytes` (FNV-1a) | 빠르다; 여기서 crypto 해시는 이득 없이 ~50배 느리다 |
| **신뢰할 수 없는** 입력의 키를 테이블에 해싱 | `proven_hash_keyed` (SipHash-2-4) | FNV는 공격자가 모든 키를 한 버킷에 충돌시켜 O(1) 테이블을 O(n²)로 바꾸게 한다 |
| 데이터가 전송/디스크에서 **손상**되지 않았는지 확인 | `proven_crc32` | 체크섬; gzip/zlib/PNG와 상호운용된다 |
| 콘텐츠 **지문 채취**: 중복 제거, 콘텐츠 주소 지정, "같은 파일?" | `proven_sha256` | *고의로* 위조된 일치에 대해 안전한 유일한 것 |

기억할 한 줄: **CRC-32와 FNV는 사고를 탐지하지 공격을 탐지하지 않는다.** 누군가
여러분을 속여서 이득을 볼 수 있을 때 두 가지가 "같은지"를 결정하는 데 이것들을 쓰지
말라 — 그것이 `proven_sha256`이 있는 이유다. 그리고 keyed 해시는 키가 진짜 무작위성에서
한 번 고른 진짜 비밀일 때에만 안전하다; 고정 키는 키가 아닌 것이나 마찬가지다.

모든 함수는 바이트 단위로 정확하며 엔디안에 독립적이다: 같은 입력은 어떤 타깃에서든
같은 출력을 준다. 왜냐하면 기계에 따라 바뀌는 지문은 콘텐츠의 지문이 아니라 기계의
지문일 것이기 때문이다. 넷 모두 로열티 없는 알고리즘이며(FNV, CRC-32: public domain;
SipHash: CC0; SHA-256: FIPS 180-4, 특허 없음), 각 명세로부터 구현되었고 각각의 공식
알려진 답(known-answer) 벡터에 대해 검증되었다.

### 참조

| API | 의도 | 반환 |
|---|---|---|
| `proven_hash_bytes(view)` | FNV-1a 64. 여러분이 직접 고른 키를 위한 해시 테이블 해시. | `proven_u64`. |
| `proven_hash_keyed(view, key[16])` | 16바이트 비밀 아래의 SipHash-2-4. 같은 작업이되, 공격자가 공급하는 키를 위한 것. | `proven_u64`. |
| `proven_crc32(view)` | 한 번에 하는 CRC-32(IEEE, reflected) — gzip/zlib/PNG가 지니는 그것. | `proven_u32`. |
| `proven_crc32_update(crc, view)` | 청크 스트림에 대한 같은 CRC. `0`에서 시작하라; 호출 사이에 여러분이 쥐고 있는 값이 진짜 CRC이므로, 저장하고, 기록하고, 재개할 수 있다. | `proven_u32`. |
| `proven_sha256(view, out[32])` | 한 번에 하는 SHA-256. | void; `PROVEN_SHA256_SIZE` 바이트를 쓴다. |
| `proven_sha256_init/_update/_final` | 한 번에 메모리에 담을 수 없는 콘텐츠에 대한 같은 다이제스트. 다이제스트는 오직 바이트에만 의존하며, 어떻게 청크되었는지에는 결코 의존하지 않는다. | void. |
| `proven_sha256_to_hex(digest, out[65])` | `sha256sum`과 `git`이 출력하는 64자 소문자 표기. NUL 종료. | void. |

여러분이 쥐는 유일한 구조체:

```text
typedef struct {
    /* Opaque. A running SHA-256: the 8-word chain value, a 64-byte block being
       filled, and the message length. Declare one, init it, update it, finalise it. */
} proven_sha256_t;

#define PROVEN_SHA256_SIZE 32   /* the digest; size your output buffer with this */
```

`proven_sha256_t`는 아무것도 할당하지 않는다 — [호출자 소유 상태](manual-ko.md#42-caller-owned-state--destroy-없음-복사-금지)이므로 파괴할 것이 없다.

### 주의사항, 그리고 무엇이 잘못되는가

**고정 키를 가진 keyed 해시는 keyed 해시가 아니다.** `proven_hash_keyed`의 보안은
전적으로 키가 진짜 무작위성에서 한 번 뽑은 비밀이라는 데 달려 있다. 이것이
`proven_map_create`가 그것을 대신 해주는 이유이자, 테이블을 위해 여러분이 직접
`proven_hash_keyed`를 호출하는 일이 거의 없어야 하는 이유다.

잘못된 예:

```text
proven_byte_t key[16] = { 0 };            /* wrong: a "secret" everyone knows */
proven_u64 h = proven_hash_keyed(user_input, key);
```

```text
proven_byte_t key[16];
memcpy(key, &timestamp, sizeof timestamp); /* wrong: guessable, and mostly zero */
```

올바른 예 — 시작 시점에 OS에서 한 번 뽑아라:

```c
proven_byte_t key[16];
if (proven_random_bytes(key, sizeof key)) {
    proven_u64 h = proven_hash_keyed(
        proven_mem_view_from_u8(PROVEN_LIT("session-token")), key);
    (void)h;
}
```

**두 가지가 "같다"고 결정하는 데 CRC-32나 FNV를 쓰지 말라** — 누군가 여러분을 속여서
이득을 볼 수 있을 때는. 둘 다 사소하게 위조할 수 있다: 같은 CRC-32를 갖는 두 번째
입력을 만드는 것은 교과서 수준의 산수다.

잘못된 예 — 공격자가 오염시킬 수 있는 콘텐츠 주소 지정 저장소:

```text
if (proven_crc32(incoming) == stored_crc) {
    accept_as_identical(incoming);   /* wrong: a forged collision costs seconds */
}
```

올바른 예 — 속일 수 없어야 하는 지문은 `proven_sha256`이며, 32바이트 전체에 걸쳐
비교한다.

**`PROVEN_SHA256_SIZE`가 아닌 다이제스트 버퍼는 버퍼 오버플로다.** `proven_sha256`은
정확히 32바이트를 쓰며, 여러분의 배열이 얼마나 큰지 알지 못한다.

```text
proven_byte_t digest[16];        /* wrong: SHA-256 is 32 bytes */
proven_sha256(data, digest);     /* writes 32 into a 16-byte buffer */
```

테스트 스위트가 컴파일하고 실행한다:

<!-- example: manual/examples/ko/ex_04_hash.c -->
```c
/*
 * 쓰임새로 나눈 해시. 이 모듈은 할 일마다 함수를 정확히 하나씩 준다. 그러니 고를 것은
 * "내가 할 일이 무엇인가" 하나뿐이고, *그것*을 틀리는 것이 위험의 전부다.
 */

int main(void) {
    proven_mem_view_t data = proven_mem_view_from_u8(PROVEN_LIT("the quick brown fox"));

    /* 할 일 1: 믿을 수 있는 입력을 내 표의 자리로. 빠르고, 암호용이 아니다. */
    proven_u64 table_hash = proven_hash_bytes(data);
    EXAMPLE_REQUIRE(table_hash != 0, "FNV-1a produces a spread-out 64-bit value");

    /* 할 일 2: *믿을 수 없는* 입력을 표의 자리로. 목적은 같지만, 입력을 고르는 공격자도
     * 모두를 충돌시킬 수 없다. 키를 모르기 때문이다. 키는 시작할 때 진짜 난수로 한 번
     * 고른다. 고정된 키를 쓰면 이 방법의 뜻이 사라진다. */
    proven_byte_t key[16] = { 0 };   /* 실제 코드에서는 난수원에서 한 번 채운다 */
    proven_u64 safe_hash = proven_hash_keyed(data, key);
    EXAMPLE_REQUIRE(safe_hash != table_hash, "a keyed hash is a different function");

    /* 할 일 3: 이 바이트들이 상했는가? 해시가 아니라 검사합이다. gzip/zlib/PNG 와
     * 그대로 맞물린다 - 셋 다 정확히 이 CRC-32 를 쓴다. */
    proven_u32 checksum = proven_crc32(data);
    /* 널리 쓰이는 CRC-32 확인값이다. 진짜 그 함수라는 것을 눈으로 볼 수 있다. */
    EXAMPLE_REQUIRE(proven_crc32(proven_mem_view_from_u8(PROVEN_LIT("123456789"))) == 0xcbf43926u,
                    "CRC-32 of \"123456789\" is the shared check value");
    (void)checksum;

    /* 할 일 4: 내용의 지문 - 중복 제거, 내용 주소화, "이 둘이 같은 파일인가". 일치를
     * 위조하려는 상대 앞에서도 안전하게 답한다. 답이 속아 넘어가면 안 되는 자리에서
     * 집는 것이 이것이다. */
    proven_byte_t digest[PROVEN_SHA256_SIZE];
    proven_sha256(data, digest);

    char hex[65];
    proven_sha256_to_hex(digest, hex);
    /* sha256sum 과 git 이 찍는 것과 같은 표기라, 그대로 맞물린다. */
    EXAMPLE_REQUIRE(hex[64] == '\0' && proven_cstr_len(hex) == 64,
                    "a SHA-256 fingerprint is 64 lowercase hex characters");

    /* SHA-256 은 흘려 넣을 수도 있다. 한 번에 기억에 담을 수 없는 내용을 위해서다 -
     * 다이제스트는 바이트에만 달렸지, 몇 토막으로 나눠 넣었는지에는 달리지 않는다. */
    proven_sha256_t ctx;
    proven_sha256_init(&ctx);
    proven_sha256_update(&ctx, proven_mem_view_from_u8(PROVEN_LIT("the quick ")));
    proven_sha256_update(&ctx, proven_mem_view_from_u8(PROVEN_LIT("brown fox")));
    proven_byte_t streamed[PROVEN_SHA256_SIZE];
    proven_sha256_final(&ctx, streamed);

    bool same = true;
    for (proven_size_t i = 0; i < PROVEN_SHA256_SIZE; ++i) {
        if (streamed[i] != digest[i]) same = false;
    }
    EXAMPLE_REQUIRE(same, "two updates of the halves equal one hash of the whole");

    return EXAMPLE_OK();
}
```

## 7. 바이트를 텍스트로: hex와 Base64

일단 어떤 것을 해싱할 수 있고(위) 무작위 토큰을 뽑을 수 있으면(`random.h`), 그 바이트를
텍스트만 담는 곳 — URL, HTTP 헤더, 로그 줄, JSON 문자열 — 에 써야 한다. 그것이
`encode.h`다. 암호도, 압축도 없다; 이미 모두가 합의한 두 인코딩을, 숨은 할당 없이,
그리고 보통 잘못되는 두 가지 방식 없이 해낸다.

| 원하는 것 | 사용 | 알파벳 |
|---|---|---|
| 사람이 읽는 다이제스트나 몇 바이트 | `proven_hex_encode` | 소문자 hex, `sha256sum`과 `git`이 출력하는 것 |
| URL, 쿠키, 파일명 안의 바이트 | `proven_base64url_encode` | `-` `_`, 패딩 **없음** — 이스케이프할 것도, 망가뜨릴 `=`도 없음 |
| HTTP 헤더, MIME, JSON 안의 바이트 | `proven_base64_encode` | 표준 `+` `/`, `=` 패딩 |

핵심은 두 가지 거부다:

- **디코더는 한 바이트를 쓰기 전에 입력 전체를 검증한다.** 프로그램 바깥에서 온
  텍스트는 유효하다고 보장되지 않는다; 엉뚱한 문자, 잘못된 길이, 잘못된 패딩, 끼어든
  공백은 아무것도 커밋하지 않은 채 `PROVEN_ERR_INVALID_ENCODING`이다 — 끝을 넘어선
  읽기도 아니고, 호출자가 한 바이트 들어가서 손상을 발견하게 되는 조용히 짧은 결과도
  아니다. `proven_base64_decode`는 알파벳 **둘 다**와 패딩 있음/없음을 받아들인다.
  자기가 방출하는 것만 받는 디코더는 세상의 Base64의 절반을 거부하기 때문이다.
- **출력 크기는 추측이 아니라 호출이다** — `proven_hex_encoded_size`,
  `proven_base64_encoded_size`, 그리고 그들의 디코드 짝. 한 바이트 모자란 버퍼는
  아무것도 쓰지 않은 채 `PROVEN_ERR_OUT_OF_BOUNDS`이며, 결코 잘린 접두사가 아니다.

이것은 순수 계산이다 — 할당도, OS도 없다 — 그리고 freestanding에서 이용 가능하다.

### 참조

모든 호출은 입력, 호출자 소유 출력 버퍼와 그 용량, 그리고 선택적 `written_out`을
받는다. 그 어느 것도 할당하지 않는다.

| API | 의도 | 반환 |
|---|---|---|
| `proven_hex_encoded_size(n)` | `n` 바이트에 대해 `proven_hex_encode`가 쓸 문자 수: `n * 2`, NUL 없음. | `proven_size_t`. |
| `proven_hex_decoded_size(n)` | `n` 문자에 대해 `proven_hex_decode`가 쓸 바이트 수: `n / 2`. | `proven_size_t`. |
| `proven_hex_encode(data, out, cap, &w)` | 소문자 hex. | `PROVEN_OK`; `cap`이 모자라면 `OUT_OF_BOUNDS`(**아무것도 쓰지 않음**); NULL out이나 `{NULL, >0}` view에는 `INVALID_ARG`. |
| `proven_hex_decode(text, out, cap, &w)` | hex를 디코드; 대문자와 소문자 둘 다 허용. | 홀수 길이나 hex가 아닌 바이트에는 `INVALID_ENCODING`(**아무것도 커밋하지 않음**). |
| `proven_base64_encoded_size(n)` | 두 Base64 형식 모두에 대한 상한: `4 * ceil(n/3)`. | `proven_size_t`. |
| `proven_base64_decoded_size(n)` | **패딩 있는 것과 없는** 텍스트에 대한 상한: `3 * ceil(n/4)`. | `proven_size_t`. |
| `proven_base64_encode(data, out, cap, &w)` | 표준 알파벳(`+` `/`), `=` 패딩. | 위와 같음. |
| `proven_base64url_encode(data, out, cap, &w)` | URL-안전 알파벳(`-` `_`), 패딩 **없음**. | 위와 같음. |
| `proven_base64_decode(text, out, cap, &w)` | 디코드. 알파벳 **둘 다**, 패딩 있음/없음을 받아들인다. | 엉뚱한 바이트, 잘못된 길이, 잘못된 패딩에는 `INVALID_ENCODING`. |

`written_out`은 NULL이어도 된다. 진입 시와 모든 오류 경로에서 `0`으로 설정되므로,
결코 낡은 값이 아니다.

### 주의사항, 그리고 무엇이 잘못되는가

**출력 크기는 눈대중이 아니라 크기 함수로 잡아라.** 인코더는 잘라내는 대신 모자란
버퍼를 거부한다 — 즉 손으로 계산한 크기에서 얻는 실패는 조용한 손상이 아니라 여러분이
처리해야 하는 오류다. 그것이 좋은 결과다; 요점은 여러분이 여전히 그것을 처리해야
한다는 것이다.

잘못된 예 — 고전적인 하나 차이(off-by-one) 실수이며, 이제 오버플로 대신 *실패*한다:

```text
proven_byte_t out[16];                       /* wrong: 12 bytes of hex needs 24 chars */
proven_hex_encode(twelve_bytes, out, sizeof out, &w);   /* OUT_OF_BOUNDS, nothing written */
```

올바른 예:

```c
proven_mem_view_t data = proven_mem_view_from_u8(PROVEN_LIT("twelve bytes"));
proven_byte_t out[64];
proven_size_t w = 0;
if (proven_hex_encoded_size(data.size) <= sizeof out &&
    proven_is_ok(proven_hex_encode(data, out, sizeof out, &w))) {
    /* `w` characters of lowercase hex in `out` - not NUL-terminated */
}
```

**Base64URL은 패딩을 방출하지 않으며, 디코더는 그것을 받아들인다.**
`proven_base64_decoded_size`는 *올림*하므로 패딩 없는 텍스트에 대해서도 올바른
상한이다. 그것을 `3 * (n / 4)`로 "개선"하지 말라 — 그것은 패딩 없는 꼬리가 실어
나르는 1~2바이트를 버림하며, 여러분은 이 라이브러리 자신의 URL-안전 출력을 디코드하는
데 실패할 것이다. (실제로 한 번 그런 일이 있었고, 감사(audit)가 그것을 잡아냈다.)

```text
proven_size_t cap = 3 * (text_len / 4);   /* wrong: 0 for "QQ", which decodes to 1 byte */
```

**디코더를 직접 손으로 만들지 말라.** 이것들이 존재하는 이유는 바로, 디코드가 여러분의
프로그램 바깥에서 텍스트를 읽고, 모두가 작성하는 두 줄짜리 루프가 그것을 신뢰하기
때문이다.

잘못된 예 — 홀수 입력에서 끝을 넘어 읽고, 쓰레기를 데이터로 받아들인다:

```text
for (size_t i = 0; i < len; i += 2)               /* wrong: no length check */
    out[i/2] = (hexval(text[i]) << 4) | hexval(text[i+1]);   /* wrong: no validation */
```

`proven_hex_decode`는 한 바이트를 쓰기 전에 입력 **전체**를 검증하므로, 끝 근처의
엉뚱한 문자 하나가 여러분이 완전하다고 믿는 절반만 디코드된 접두사를 쥐게 만들 수 없다.

**공백은 일부러 건너뛰지 않는다.** 붙여넣은, 줄바꿈된 Base64 덩어리는
`INVALID_ENCODING`이지 조용히 다른 결과가 아니다. 줄바꿈된 입력을 *받아들이고* 싶다면,
공백을 직접 제거하라 — 의도적으로, 여러분이 볼 수 있는 곳에서.

**목적지 크기는 손으로 짐작하지 말고 크기 함수로 구한다.** 인코딩마다 짝이 하나씩 있고 —
`proven_hex_encoded_size()` / `proven_hex_decoded_size()`, 그리고
`proven_base64_encoded_size()` / `proven_base64_decoded_size()` — 둘이 약속하는 것이 다르다.

| 방향 | 그 수의 의미 | 쓰는 법 |
|---|---|---|
| 인코딩 | hex와 표준 Base64에서는 **정확한 값**. Base64URL은 패딩을 빼므로 결과가 더 짧을 수 있으나, 이 값은 여전히 안전한 상한이다. | 이만큼 할당한다. 호출이 실제로 쓴 양을 알려 준다. |
| 디코딩 | **상한**이다. 패딩과 쓰인 알파벳은 텍스트를 읽기 전에는 알 수 없다. | 상한만큼 할당한 뒤, 길이로는 상한이 아니라 보고된 개수를 쓴다. |

아래 예제는 그것으로 끝맺는다. `proven_base64_encoded_size()`로 크기를 잡아 목적지를 할당하고,
거기에 인코딩하고, `proven_base64_decoded_size()`로 역방향 버퍼 크기를 잡아 디코딩하고, 같은
바이트를 `proven_hex_decode()`로도 왕복시킨다 — 실제 도구들이 내놓는 대문자 입력과, 온전한 바이트
수가 될 수 없는 홀수 길이 입력까지 포함해서다.

테스트 스위트가 컴파일하고 실행한다:

<!-- example: manual/examples/ko/ex_04_encode.c -->
```c
/*
 * 바이트를 글자로, 쓰임새별로. 규칙은 해시와 같다. 할 일마다 함수 하나, 그리고 위험은
 * 할 일을 잘못 고르는 것. 사람이 읽을 것에는 hex, URL 에 들어갈 것에는 Base64URL,
 * 선로로 나갈 것에는 표준 Base64.
 */

int main(void) {
    proven_mem_view_t data = proven_mem_view_from_u8(PROVEN_LIT("the quick brown fox"));

    /* 할 일 1: 사람이 읽거나 붙여 넣을 다이제스트 - hex, sha256sum 과 git 이 쓰는 표기. */
    proven_byte_t hex[64];   /* proven_hex_encoded_size(19) = 38 */
    proven_size_t hn = 0;
    EXAMPLE_REQUIRE(proven_is_ok(proven_hex_encode(data, hex, sizeof hex, &hn)),
                    "hex encode into a buffer sized by proven_hex_encoded_size");
    EXAMPLE_REQUIRE(hn == proven_hex_encoded_size(data.size), "two hex chars per byte");

    /* 할 일 2: URL 에 들어갈 토큰 - Base64URL. 퍼센트 이스케이프가 필요 없고, 파서가
     * 걸려 넘어질 '=' 채움도 없다. */
    proven_byte_t token_bytes[16] = { 0 };   /* 실제 코드에서는: proven_random_bytes(token_bytes, 16) */
    proven_byte_t url[32];
    proven_size_t un = 0;
    EXAMPLE_REQUIRE(proven_is_ok(proven_base64url_encode(
                        (proven_mem_view_t){ token_bytes, sizeof token_bytes }, url, sizeof url, &un)),
                    "base64url encode a token");
    /* URL 안전 토큰에는 '=' 가 없다. */
    bool has_pad = false;
    for (proven_size_t i = 0; i < un; ++i) if (url[i] == '=') has_pad = true;
    EXAMPLE_REQUIRE(!has_pad, "the URL form emits no padding");

    /* 할 일 3: 선로로 나가는 바이트 - 표준 Base64, HTTP 와 MIME 이 기대하는 +/= 문자표. */
    proven_byte_t b64[64];
    proven_size_t bn = 0;
    EXAMPLE_REQUIRE(proven_is_ok(proven_base64_encode(data, b64, sizeof b64, &bn)),
                    "standard base64 encode");

    /* 그리고 왕복한다. 디코드는 바이트를 그대로 되돌려 준다. 두 문자표와 채움이 있든
     * 없든 받아 주는 디코더는 일부러 그렇게 만든 것이다 - 실제 입력은 온갖 모양으로 온다. */
    proven_byte_t back[32];
    proven_size_t dn = 0;
    EXAMPLE_REQUIRE(proven_is_ok(proven_base64_decode(
                        (proven_mem_view_t){ b64, bn }, back, sizeof back, &dn)),
                    "decode the base64 back");
    EXAMPLE_REQUIRE(dn == data.size && proven_memcmp(back, data.ptr, dn) == 0,
                    "what comes back is exactly what went in");

    /* 검증하는 디코더의 요점: 쓰레기는 추측하지 않고 거부한다. 이것을 두 줄짜리 반복문에
     * 먹였다면 끝을 넘어 읽거나 조용히 짧은 결과를 얻었을 것이다. */
    proven_err_t bad = proven_base64_decode(
        proven_mem_view_from_u8(PROVEN_LIT("not valid base64!!")), back, sizeof back, &dn);
    EXAMPLE_REQUIRE(bad == PROVEN_ERR_INVALID_ENCODING,
                    "a stray character is INVALID_ENCODING, with nothing committed");

    /* 그리고 한 바이트 모자란 버퍼는 거부된다. 잘리는 일은 없다. */
    proven_byte_t tiny[4];
    EXAMPLE_REQUIRE(proven_hex_encode(data, tiny, sizeof tiny, &hn) == PROVEN_ERR_OUT_OF_BOUNDS,
                    "a too-small output buffer is OUT_OF_BOUNDS, not a truncated prefix");

    /* --- 짐작하지 말고 목적지 버퍼의 크기를 셈하기 ------------------------ */

    /* 위의 인코드와 디코드는 모두 넉넉한 것이 뻔한 고정 배열에 썼다. 실제 코드는
     * 목적지를 할당하고, 그러면 크기는 기억해 내는 것이 아니라 셈해야 한다. 크기 함수
     * 넷이 그것을 위한 것이다 - 인코딩마다, 방향마다 하나씩. */
    proven_allocator_t alloc = proven_heap_allocator();

    proven_size_t need = proven_base64_encoded_size(data.size);
    EXAMPLE_REQUIRE(need > 0, "an encoded size must be computed, not guessed");

    proven_result_mem_mut_t out = alloc.alloc_fn(alloc.ctx, need, 1);
    EXAMPLE_REQUIRE(proven_is_ok(out.err), "allocating exactly the encoded size must succeed");

    proven_size_t wrote = 0;
    EXAMPLE_REQUIRE(proven_is_ok(proven_base64_encode(data, out.value.ptr, out.value.size, &wrote)),
                    "encoding into a buffer sized by the size function must fit exactly");
    EXAMPLE_REQUIRE(wrote == need, "and use all of it: this size is exact for standard base64");

    /* 디코드 방향은 정확한 개수가 아니라 *상한*이다. base64 글에는 채움이 있을 수
     * 있으므로, 디코더가 실제로 몇 바이트를 썼는지 알려 준다. 버퍼는 상한으로 잡고,
     * 그다음에는 알려 준 개수를 쓸 것. */
    proven_size_t bound = proven_base64_decoded_size(wrote);
    EXAMPLE_REQUIRE(bound >= data.size, "the decoded bound must cover the real output");

    proven_result_mem_mut_t plain = alloc.alloc_fn(alloc.ctx, bound, 1);
    EXAMPLE_REQUIRE(proven_is_ok(plain.err), "allocating the decode bound must succeed");

    proven_size_t got = 0;
    EXAMPLE_REQUIRE(proven_is_ok(proven_base64_decode(
                        (proven_mem_view_t){ out.value.ptr, wrote }, plain.value.ptr, plain.value.size, &got)),
                    "decoding back must succeed");
    EXAMPLE_REQUIRE(got == data.size && proven_memcmp(plain.value.ptr, data.ptr, got) == 0,
                    "and reproduce the original bytes exactly");

    /* hex 도 같은 짝을 갖는다. 입력 길이가 짝수일 때 디코드 상한은 정확하고, 올바른
     * hex 는 언제나 짝수다 - 홀수 길이는 잘못된 입력이고, 디코더는 마지막 자리를
     * 버리는 대신 그렇다고 말한다. */
    proven_size_t hex_len = 0;
    EXAMPLE_REQUIRE(proven_is_ok(proven_hex_encode(data, hex, sizeof hex, &hex_len)),
                    "re-encode to hex, since the refused call above left its count unusable");

    proven_size_t hex_bound = proven_hex_decoded_size(hex_len);
    EXAMPLE_REQUIRE(hex_bound == data.size, "two hex characters decode to one byte");

    proven_byte_t from_hex[32];
    proven_size_t hex_got = 0;
    EXAMPLE_REQUIRE(proven_is_ok(proven_hex_decode(
                        (proven_mem_view_t){ hex, hex_len }, from_hex, sizeof from_hex, &hex_got)),
                    "hex decodes back to the original bytes");
    EXAMPLE_REQUIRE(hex_got == data.size && proven_memcmp(from_hex, data.ptr, hex_got) == 0,
                    "and the round trip is exact");

    /* hex 는 들어올 때 대소문자를 가리지 않는다. 도구마다 같은 다이제스트를 다른
     * 대소문자로 찍기 때문에 중요하다. */
    EXAMPLE_REQUIRE(proven_is_ok(proven_hex_decode(
                        proven_mem_view_from_u8(PROVEN_LIT("DEADBEEF")), from_hex, sizeof from_hex, &hex_got)),
                    "uppercase hex decodes too");
    EXAMPLE_REQUIRE(hex_got == 4, "and yields one byte per two characters");

    /* 글자 수가 홀수면 온전한 바이트 수가 될 수 없다. */
    EXAMPLE_REQUIRE(proven_hex_decode(proven_mem_view_from_u8(PROVEN_LIT("abc")),
                                      from_hex, sizeof from_hex, &hex_got) == PROVEN_ERR_INVALID_ENCODING,
                    "an odd-length hex string is malformed, not silently truncated");

    alloc.free_fn(alloc.ctx, plain.value.ptr);
    alloc.free_fn(alloc.ctx, out.value.ptr);

    (void)url; (void)un;
    return EXAMPLE_OK();
}
```

## 8. 예제와 오용 사례

### 배열 안을 가리키는 포인터는 낡을 수 있다

잘못된 예:

```text
int *p = PROVEN_ARRAY_GET_MUT(&nums, int, 0);
PROVEN_ARRAY_PUSH(&nums, int, 30);
*p = 99; /* wrong: push may have reallocated the array */
```

올바른 예:

```c
proven_result_array_t r = PROVEN_ARRAY_INIT(alloc, int, 2);
if (!proven_is_ok(r.err)) {
    return;
}
proven_array_t nums = r.value;
(void)PROVEN_ARRAY_PUSH(&nums, int, 10);

/* Push first, then take the pointer. A pointer fetched before a push may point
 * at a block the array has already reallocated away. */
(void)PROVEN_ARRAY_PUSH(&nums, int, 30);
int *p = PROVEN_ARRAY_GET_MUT(&nums, int, 0);
if (p) {
    *p = 99;
}

PROVEN_ARRAY_DESTROY(&nums);
```

### 하나의 침습적 노드는 한 번에 하나의 리스트에 속한다

잘못된 예:

```text
proven_list_push_back(&list_a, &item.link);
proven_list_push_back(&list_b, &item.link); /* wrong */
```

멤버십마다 내장 노드를 하나씩 써라.

### 안전 매크로로 순회 중 제거하기

올바른 예:

```c
typedef struct Item {
    int value;
    proven_list_node_t link;
} Item;

proven_list_t list;
proven_list_init(&list);
Item a = { .value = 1 };
Item b = { .value = 2 };
proven_list_push_back(&list, &a.link);
proven_list_push_back(&list, &b.link);

/* The SAFE form reads `it->next` into `next` before the body runs, so removing
 * `it` (which nulls its links) cannot strand the loop. */
proven_list_node_t *it = NULL;
proven_list_node_t *next = NULL;
PROVEN_LIST_FOR_EACH_SAFE(it, next, &list) {
    Item *item = PROVEN_LIST_ENTRY(it, Item, link);
    if (item->value % 2 == 0) {
        proven_list_remove(it);
    }
}
```

### 링 push는 커지지 않는다

잘못된 예:

```text
PROVEN_RING_PUSH(&q, int, value); /* wrong if you ignore full-ring errors */
```

올바른 예:

```c
proven_result_ring_t r = PROVEN_RING_INIT(alloc, int, 2);
if (!proven_is_ok(r.err)) {
    return;
}
proven_ring_t q = r.value;

int value = 7;
for (int i = 0; i < 3; ++i) {
    proven_err_t e = PROVEN_RING_PUSH(&q, int, value);
    if (e == PROVEN_ERR_OUT_OF_BOUNDS) {
        /* The ring is full. Drop the item, or drain one first - but decide. */
        int drop = 0;
        (void)PROVEN_RING_POP(&q, int, &drop);
        e = PROVEN_RING_PUSH(&q, int, value);
    }
    (void)e;
}

PROVEN_RING_DESTROY(&q);
```

### borrowed 맵 키는 항목보다 오래 살아야 한다

잘못된 예:

```text
proven_u8str_t key = make_key(alloc);
PROVEN_MAP_SET_U8_BORROWED(&m, proven_u8str_as_view(&key), int, 1);
proven_u8str_destroy(alloc, &key);
/* wrong: map still points at freed key bytes */
```

올바른 선택지:

- borrowed 키에는 문자열 리터럴이나 다른 안정된 저장소를 사용하라.
- owned 키 객체를 다른 곳에 저장하고, 맵 항목을 제거한 뒤 그것들을 파괴하라.
- 맵이 키 저장소를 소유해야 한다면 `PROVEN_KEY_TYPE_U8_OWNED`와 `proven_map_set_u8_owned()`를 사용하라.

### 같은 맵에서 borrow한 값을 삽입할 때는 scratch를 써라

`element`가 맵 저장소 안을 가리키고 삽입이 재해싱할 수 있다면, `proven_map_set_with_scratch()`나 scratch 매크로를 사용하라.

```c
proven_result_map_t r = PROVEN_MAP_INIT_INT(alloc, int, 4);
if (!proven_is_ok(r.err)) {
    return;
}
proven_map_t m = r.value;
(void)PROVEN_MAP_SET_INT(&m, 1, int, 42);

/* `src` points into the map's own bucket array. Passing it straight to
 * proven_map_set would be an alias: a rehash triggered by that same insert can
 * free the bytes it is still reading from. The scratch form captures the source
 * bytes into a temporary from `scratch` first, so the insert is alias-safe. */
const int *src = PROVEN_MAP_GET_INT(&m, int, 1);
if (src) {
    (void)proven_map_set_with_scratch(&m, (proven_map_key_t){ .id = 2 }, src, scratch);
}

PROVEN_MAP_DESTROY(&m);
```

### 이진 검색은 정렬된 입력을 요구한다

잘못된 예:

```text
int *hit = proven_array_binary_search(&nums, &key, cmp_int);
/* wrong if nums was not sorted by cmp_int */
```

### 완성 예제: 배열, 정렬, 이진 검색

테스트 스위트가 컴파일하고 실행한다. 중복 키에 대한 정렬의 동작에 주목하라: 그것들은 *빠른* 경우이지, 이차(quadratic)인 경우가 아니다.

<!-- example: manual/examples/ko/ex_04_array.c -->
```c
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
```

### 완성 예제: 정수 키 맵과 owned 문자열 키 맵

테스트 스위트가 컴파일하고 실행한다. owned 키 절반이 잘못 이해하기 쉬운 요점을 증명한다: 맵이 키를 복사하므로, 그것을 만든 버퍼는 즉시 재사용해도 되는 여러분의 것이다.

<!-- example: manual/examples/ko/ex_04_map.c -->
```c
/*
 * proven_map_t 는 납작한 개방 주소법 해시 맵이다. 값 타입은 만들 때 정해지고 버킷
 * 배열 안에 그대로 담긴다 - 값을 위한 항목별 할당이 없고, get 은 그 배열 속을 곧장
 * 가리키는 포인터를 돌려준다.
 *
 * 재미있는 결정은 *키* 쪽이다.
 *
 *   PROVEN_KEY_TYPE_INT          - 키가 proven_size_t 다. 소유할 것이 없다.
 *   PROVEN_KEY_TYPE_U8_BORROWED  - 버킷이 여러분의 포인터와 길이를 담는다. 맵은
 *                                  바이트를 복사하지 않으므로, 항목이 살아 있는
 *                                  동안 *여러분*이 그 바이트를 살아 있고 움직이지
 *                                  않게 지켜야 한다. 문자열 리터럴에 알맞다.
 *   PROVEN_KEY_TYPE_U8_OWNED     - 맵이 키 바이트를 제 저장소로 복사하고 나중에
 *                                  해제한다. 실행 중에 만든 키에 알맞고, 대개의
 *                                  키가 그렇다.
 *
 * 이 예제의 뒷부분이 OWNED 가 존재하는 이유다.
 */

typedef struct {
    int  level;
    long score;
} player_t;

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    /* --- 정수 키 ------------------------------------------------------------ */
    proven_result_map_t r = PROVEN_MAP_INIT_INT(alloc, player_t, 8);
    EXAMPLE_REQUIRE(proven_is_ok(r.err), "creating an int-keyed map must succeed");
    if (!proven_is_ok(r.err)) {
        return 1;
    }
    proven_map_t by_id = r.value;

    proven_err_t err = PROVEN_MAP_SET_INT(&by_id, 404, player_t, ((player_t){ .level = 3, .score = 990 }));
    EXAMPLE_REQUIRE(proven_is_ok(err), "inserting into the map must succeed");
    err = PROVEN_MAP_SET_INT(&by_id, 7, player_t, ((player_t){ .level = 1, .score = 10 }));
    EXAMPLE_REQUIRE(proven_is_ok(err), "inserting a second key must succeed");

    /* 이미 있는 키에 set 하면 값을 바꾼다. 항목을 더하지 않는다. */
    err = PROVEN_MAP_SET_INT(&by_id, 7, player_t, ((player_t){ .level = 2, .score = 40 }));
    EXAMPLE_REQUIRE(proven_is_ok(err), "re-setting an existing key must succeed");
    EXAMPLE_REQUIRE(by_id.len == 2, "re-setting a key replaces its value rather than adding an entry");

    /* get 은 버킷 배열 속을 가리키는 포인터를, 없으면 NULL 을 돌려준다. 다시 해싱하는
     * 삽입이 일어나면 무효가 된다 - 찾고, 쓰고, 버릴 것. */
    const player_t *p = PROVEN_MAP_GET_INT(&by_id, player_t, 7);
    EXAMPLE_REQUIRE(p && p->level == 2 && p->score == 40, "get must see the replaced value");
    EXAMPLE_REQUIRE(PROVEN_MAP_GET_INT(&by_id, player_t, 999) == NULL, "a missing key yields NULL");

    err = PROVEN_MAP_REMOVE_INT(&by_id, 7);
    EXAMPLE_REQUIRE(proven_is_ok(err), "removing a present key must succeed");
    EXAMPLE_REQUIRE(PROVEN_MAP_GET_INT(&by_id, player_t, 7) == NULL, "a removed key is gone");
    EXAMPLE_REQUIRE(by_id.len == 1, "removal decrements the live entry count");

    PROVEN_MAP_DESTROY(&by_id);

    /* --- 소유하는 문자열 키 -------------------------------------------------- */
    /* 같은 맵인데, 실행 중에 지어낸 이름을 키로 쓴다 - 빌린 키였다면 매달린 포인터가
     * 되기를 기다리는 자리다. */
    proven_result_map_t rm = PROVEN_MAP_INIT_U8_OWNED(alloc, player_t, 8);
    EXAMPLE_REQUIRE(proven_is_ok(rm.err), "creating an owned-string-keyed map must succeed");
    if (!proven_is_ok(rm.err)) {
        return 1;
    }
    proven_map_t by_name = rm.value;

    /* 키마다 다시 쓸 작정인 임시 버퍼. BORROWED 맵에서 그 작정은 치명적이다. 모든
     * 항목이 바로 이 같은 바이트를 가리키게 된다. */
    proven_byte_t scratch[32];
    proven_u8str_t name = proven_u8str_borrow(scratch, sizeof scratch);

    err = proven_u8str_append(&name, PROVEN_LIT("ada"));
    EXAMPLE_REQUIRE(proven_is_ok(err), "building the first key must succeed");

    /* set_u8_owned 는 키 바이트를 맵 저장소로 *복사한다*. 돌아온 뒤로 맵의 키는
     * `scratch` 와 아무 상관이 없다. */
    err = PROVEN_MAP_SET_U8_OWNED(&by_name, proven_u8str_as_view(&name), player_t,
                                  ((player_t){ .level = 9, .score = 5000 }));
    EXAMPLE_REQUIRE(proven_is_ok(err), "inserting with an owned key must succeed");

    /* 그래서 버퍼는 곧바로 다음 키에 다시 쓸 수 있고... */
    err = proven_u8str_reset(&name);
    EXAMPLE_REQUIRE(proven_is_ok(err), "the key buffer is ours again the moment set returns");
    err = proven_u8str_append(&name, PROVEN_LIT("grace"));
    EXAMPLE_REQUIRE(proven_is_ok(err), "overwriting the buffer with the next key must succeed");

    err = PROVEN_MAP_SET_U8_OWNED(&by_name, proven_u8str_as_view(&name), player_t,
                                  ((player_t){ .level = 4, .score = 700 }));
    EXAMPLE_REQUIRE(proven_is_ok(err), "inserting the second owned key must succeed");

    /* ...첫 항목은 그 덮어쓰기에 조금도 다치지 않는다. 이것이 요점 전부다. 맵은
     * 지금 "grace" 라고 적힌 버퍼의 뷰가 아니라 "ada" 의 제 사본을 쥐고 있다. BORROWED
     * 맵이었다면 둘 다 "grace" 로 키가 잡힌 항목 둘을 보고했을 것이다 - 아니면 더 나쁘게,
     * 해제된 기억이 키인 항목 하나를. */
    const player_t *ada = PROVEN_MAP_GET_U8_OWNED(&by_name, player_t, PROVEN_LIT("ada"));
    EXAMPLE_REQUIRE(ada && ada->score == 5000, "the copied key survives the caller reusing its buffer");

    const player_t *grace = PROVEN_MAP_GET_U8_OWNED(&by_name, player_t, PROVEN_LIT("grace"));
    EXAMPLE_REQUIRE(grace && grace->score == 700, "the second key is a separate entry");
    EXAMPLE_REQUIRE(by_name.len == 2, "two distinct keys means two entries");

    /* remove 는 맵이 만든 키 사본을 해제한다 - 여러분이 직접 해제하는 일은 없다. */
    err = PROVEN_MAP_REMOVE_U8_OWNED(&by_name, PROVEN_LIT("ada"));
    EXAMPLE_REQUIRE(proven_is_ok(err), "removing an owned key must succeed");
    EXAMPLE_REQUIRE(PROVEN_MAP_GET_U8_OWNED(&by_name, player_t, PROVEN_LIT("ada")) == NULL,
                    "the removed entry is gone");

    printf("map: %zu name(s) left, grace at level %d\n",
           (size_t)by_name.len, grace ? grace->level : -1);

    /* destroy 는 버킷 배열과 그 안에 남은 키 사본을 모두 해제한다(여기서는 "grace").
     * `scratch` 는 우리 것이고 맵보다 오래 살며, 빌린 `name` 손잡이는 해제할 것이 없다. */
    PROVEN_MAP_DESTROY(&by_name);
    proven_u8str_destroy(alloc, &name);
    return EXAMPLE_OK();
}
```

### 실전 예제: 컨테이너 크기 잡기, 정렬되지 않은 데이터 찾기, 흘러가는 데이터 검사합 구하기

앞의 실전 예제들은 컨테이너를 하나씩 다룬다. 이 예제는 그 조각들이 들어가 있는 프로그램 — 작은
이벤트 수집기 — 이고, 컨테이너가 여럿 얽히고 나서야 생기는 질문들에 답한다.

- **채우는 도중에 컨테이너가 재할당하지 않게 한다.** `proven_array_reserve()`와
  `proven_map_reserve()`는 용량을 한 번에 정한다. 힙에서는 복사를 아끼고, 아레나(arena) 뒤에서는 재할당이
  다음 reset까지 남기는 죽은 저장 공간을 아낀다. map에서는 포인터의 정확성 문제이기도 하다.
  리해시(rehash, 버킷 배열을 다시 만드는 일)는 앞서 `get_mut`이 돌려준 모든 포인터를 무효로 만든다.
- **내가 만들지 않은 핸들을 확인한다.** `proven_array_is_valid()`, `proven_ring_is_valid()`,
  `proven_map_is_valid()`는 핸들 자체의 필드가 서로 맞는지 묻는다. 0으로만 초기화된, 혹은 아예 만든
  적 없는 컨테이너를 첫 push가 아니라 내 코드의 경계에서 잡아내는 검사다.
- **정렬되지 않은 데이터를 찾는다.** 이벤트 로그는 도착 순서로 둔다. 그 순서 자체가 기록하려는
  대상이기 때문이다. 그래서 이진 탐색(binary search)은 쓸 수 없다 — 정렬되지 않은 입력에서 그것은
  "없음"을 돌려주지 않고 틀린 답을 돌려준다. 올바른 호출은
  `proven_array_linear_search()`이고, 순서상 **첫 번째** 일치를 돌려준다.
- **map에 이미 있는 값을 한 번의 조회로 고친다.** `proven_map_get_mut()`은 map 자신의 저장 공간을
  가리키는 포인터를 돌려주므로, 카운터는 읽고 고쳐 다시 쓰는 대신 있는 자리에서 증가한다.
- **키에 어떤 해시가 어울리는지 고른다.** `proven_map_create()`는 문자열 키를 키가 있는
  SipHash로 해싱하므로, 키를 고르는 공격자가 모든 키를 한 버킷에 몰아넣을 수 없다.
  `proven_map_create_trusted()`는 빠른 FNV-1a를 쓰며, 모든 키를 내 코드가 고를 때만 옳다.
  `proven_map_hash()`는 map이 실제로 키를 놓는 데 쓴 값을 돌려주므로, 둘의 차이는 주장이 아니라
  관찰할 수 있는 사실이 된다.
- **조각으로 도착하는 데이터의 검사합을 구한다.** `proven_crc32_update()`는 각 조각을 진행 중인
  값에 접어 넣는다. 조각 경계를 어떻게 나누었든 결과는 전체에 대한 `proven_crc32()`와 같다.

<!-- example: manual/examples/ko/ex_04_growth_and_lookup.c -->
```c
/*
 * The container chapters show each structure on its own. A real program uses
 * several at once, and the questions that come up then are not about any single
 * container:
 *
 *   - How do I stop a container reallocating while it fills?  reserve.
 *   - How do I check a container somebody handed me is usable?  is_valid.
 *   - How do I search when the data is NOT sorted?  linear search - and knowing
 *     why binary search would give a wrong answer here.
 *   - How do I update a value already in a map without looking it up twice?
 *     get_mut.
 *   - Do I need the attack-resistant hash, or the fast one?  it depends on who
 *     chooses the keys, and map_hash lets you see the difference.
 *   - How do I checksum data that arrives in pieces?  crc32_update.
 *
 * The program is a small event intake: events arrive, are counted per client,
 * the most recent few are kept for a diagnostic dump, and the batch is
 * checksummed as it streams past.
 */

typedef struct {
    proven_u32 client;
    proven_u32 code;      /* an event code; 0 means "connection closed" */
} event_t;

/* Search by client id: this is the comparison for finding an event, not for
 * sorting them. The array below is in ARRIVAL order and stays that way. */
static int by_client(const void *a, const void *b) {
    const event_t *x = (const event_t *)a;
    const event_t *y = (const event_t *)b;
    return (x->client > y->client) - (x->client < y->client);
}

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    /* --- 1. reserve: decide the capacity once ----------------------------- */

    proven_result_array_t ar = PROVEN_ARRAY_INIT(alloc, event_t, 4);
    EXAMPLE_REQUIRE(proven_is_ok(ar.err), "creating the event log must succeed");
    if (!proven_is_ok(ar.err)) {
        return 1;
    }
    proven_array_t events = ar.value;

    /* We know the batch size before we start, so ask for the room once. Without
     * this the array doubles as it fills, copying its contents each time; behind
     * an arena allocator each of those copies also leaves the old block behind
     * until the arena is reset. */
    proven_err_t err = proven_array_reserve(&events, 16);
    EXAMPLE_REQUIRE(proven_is_ok(err), "reserving room for the whole batch must succeed");
    EXAMPLE_REQUIRE(events.cap >= 16, "the capacity is now at least what was asked for");

    /* is_valid asks whether the handle itself is structurally sound - a pointer,
     * a length and a capacity that agree. Assert it where a container arrives
     * from other code, or after a zero-initialised handle might have escaped;
     * it is not something to repeat after every push. */
    EXAMPLE_REQUIRE(proven_array_is_valid(&events), "a created array must be structurally valid");

    proven_array_t never_created = {0};
    EXAMPLE_REQUIRE(!proven_array_is_valid(&never_created),
                    "a zero-initialised handle is not a usable array");

    static const event_t batch[] = {
        { .client = 7, .code = 200 }, { .client = 3, .code = 200 },
        { .client = 7, .code = 404 }, { .client = 9, .code = 200 },
        { .client = 3, .code = 500 }, { .client = 7, .code = 0   },
    };
    proven_size_t batch_len = sizeof batch / sizeof batch[0];

    proven_size_t cap_before = events.cap;
    for (proven_size_t i = 0; i < batch_len; ++i) {
        err = PROVEN_ARRAY_PUSH(&events, event_t, batch[i]);
        EXAMPLE_REQUIRE(proven_is_ok(err), "pushing an event must succeed");
    }
    EXAMPLE_REQUIRE(events.cap == cap_before, "the reserve was enough: nothing reallocated while filling");

    /* --- 2. searching data that is not sorted ----------------------------- */

    /* The log is in arrival order, which is the order we want to keep: it is the
     * thing being recorded. Binary search would be faster and WRONG here, since
     * it may only be used on a sorted range - on unsorted data it does not
     * return "not found", it returns nonsense. Linear search is the correct
     * tool, and O(n) over a batch this size is nothing. */
    event_t key = { .client = 9, .code = 0 };
    const event_t *found = (const event_t *)proven_array_linear_search(&events, &key, by_client);
    EXAMPLE_REQUIRE(found != NULL, "client 9 appears in the batch");
    EXAMPLE_REQUIRE(found->code == 200, "and linear search returns the FIRST match in order");

    event_t absent = { .client = 42, .code = 0 };
    EXAMPLE_REQUIRE(proven_array_linear_search(&events, &absent, by_client) == NULL,
                    "a client that never appeared is reported as not found");

    /* --- 3. a ring for the most recent events ----------------------------- */

    proven_result_ring_t rr = PROVEN_RING_INIT(alloc, event_t, 4);
    EXAMPLE_REQUIRE(proven_is_ok(rr.err), "creating the recent-events ring must succeed");
    proven_ring_t recent = rr.value;
    EXAMPLE_REQUIRE(proven_ring_is_valid(&recent), "a created ring must be structurally valid");

    proven_ring_t unset = {0};
    EXAMPLE_REQUIRE(!proven_ring_is_valid(&unset), "a zero-initialised ring handle is not usable");

    /* This ring refuses when full rather than overwriting, so "keep the most
     * recent four" means dropping the oldest ourselves before pushing. */
    for (proven_size_t i = 0; i < batch_len; ++i) {
        if (recent.len == recent.cap) {
            event_t dropped;
            err = PROVEN_RING_POP(&recent, event_t, &dropped);
            EXAMPLE_REQUIRE(proven_is_ok(err), "a full ring must yield its oldest element");
        }
        err = PROVEN_RING_PUSH(&recent, event_t, batch[i]);
        EXAMPLE_REQUIRE(proven_is_ok(err), "pushing after making room must succeed");
    }
    EXAMPLE_REQUIRE(recent.len == 4, "the ring holds the four most recent events");

    /* --- 4. counting per client, with one lookup per update --------------- */

    /* create_with_capacity is proven_map_create under a name that says why the
     * capacity argument is there: sizing it now avoids rehashing later, and a
     * rehash both copies every bucket and invalidates every pointer previously
     * returned by get_mut. */
    proven_result_map_t mr = proven_map_create_with_capacity(alloc, 8, PROVEN_KEY_TYPE_INT,
                                                            sizeof(proven_u32), alignof(proven_u32));
    EXAMPLE_REQUIRE(proven_is_ok(mr.err), "creating the counter map must succeed");
    proven_map_t counts = mr.value;
    EXAMPLE_REQUIRE(proven_map_is_valid(&counts), "a created map must be structurally valid");

    err = proven_map_reserve(&counts, 32);
    EXAMPLE_REQUIRE(proven_is_ok(err), "reserving map capacity must succeed");

    for (proven_size_t i = 0; i < batch_len; ++i) {
        proven_map_key_t k = { .id = batch[i].client };

        /* get_mut returns a pointer INTO the map's storage, so the counter is
         * incremented where it lives: one lookup, no copy back. The pointer is
         * good only until the next insert - which is another reason the capacity
         * was reserved above. */
        proven_u32 *seen = (proven_u32 *)proven_map_get_mut(&counts, k);
        if (seen) {
            *seen += 1;
        } else {
            proven_u32 one = 1;
            err = proven_map_set(&counts, k, &one);
            EXAMPLE_REQUIRE(proven_is_ok(err), "inserting a new client must succeed");
        }
    }

    const proven_u32 *seven = PROVEN_MAP_GET_INT(&counts, proven_u32, 7);
    EXAMPLE_REQUIRE(seven != NULL && *seven == 3, "client 7 sent three events");

    /* A closing event (code 0) means the client is gone: drop its counter. */
    for (proven_size_t i = 0; i < batch_len; ++i) {
        if (batch[i].code == 0) {
            err = proven_map_remove(&counts, (proven_map_key_t){ .id = batch[i].client });
            EXAMPLE_REQUIRE(proven_is_ok(err), "removing a present key must succeed");
        }
    }
    EXAMPLE_REQUIRE(PROVEN_MAP_GET_INT(&counts, proven_u32, 7) == NULL, "the closed client is gone");

    /* --- 5. which hash, and how to see the difference --------------------- */

    /* Two string-key maps over the same keys. The default one hashes with keyed
     * SipHash, so an attacker who chooses the keys cannot force them all into
     * one bucket. The trusted one uses fast FNV-1a and is the right choice ONLY
     * when your own code chooses every key. */
    proven_result_map_t untrusted = PROVEN_MAP_INIT_U8_BORROWED(alloc, proven_u32, 8);
    EXAMPLE_REQUIRE(proven_is_ok(untrusted.err), "creating the default string-key map must succeed");
    proven_map_t from_network = untrusted.value;

    proven_result_map_t tr = proven_map_create_trusted(alloc, 8, PROVEN_KEY_TYPE_U8_BORROWED,
                                                       sizeof(proven_u32), alignof(proven_u32));
    EXAMPLE_REQUIRE(proven_is_ok(tr.err), "creating the trusted-key map must succeed");
    proven_map_t internal = tr.value;

    EXAMPLE_REQUIRE(from_network.trusted_keys == false, "the default map defends against chosen keys");
    EXAMPLE_REQUIRE(internal.trusted_keys == true, "the trusted map opts out of that defence");

    /* map_hash exposes the value the map actually places a key by, so the
     * choice is observable rather than something you take on faith. */
    proven_map_key_t name = { .str = PROVEN_LIT("user-agent") };
    proven_u64 keyed = proven_map_hash(&from_network, name);
    proven_u64 fast  = proven_map_hash(&internal, name);
    EXAMPLE_REQUIRE(keyed != fast, "the same key hashes differently under the two functions");
    EXAMPLE_REQUIRE(proven_map_hash(&internal, name) == fast, "and each function is deterministic");

    /* Keys that live in memory the map does not own are BORROWED: the bytes must
     * outlive the map. These are string literals, so they do. When the key comes
     * from a buffer you are about to reuse, use an owned-key map instead
     * (PROVEN_MAP_INIT_U8_OWNED / proven_map_set_u8_owned), which copies. */
    proven_u32 hits = 1;
    err = proven_map_set(&from_network, name, &hits);
    EXAMPLE_REQUIRE(proven_is_ok(err), "inserting a borrowed string key must succeed");
    EXAMPLE_REQUIRE(PROVEN_MAP_GET_U8_BORROWED(&from_network, proven_u32, PROVEN_LIT("user-agent")) != NULL,
                    "and it can be looked up by an equal view, not the same pointer");

    /* --- 6. checksumming a stream in chunks ------------------------------- */

    /* The batch is checksummed as it goes past, which is what a program reading
     * a file or a socket has to do: it never holds the whole thing. Start the
     * running value at 0, feed each chunk in, and the final value is the same
     * one a single call over the concatenation would produce. */
    proven_u32 running = 0;
    for (proven_size_t i = 0; i < batch_len; ++i) {
        proven_mem_view_t chunk = { .ptr = (const proven_byte_t *)&batch[i], .size = sizeof batch[i] };
        running = proven_crc32_update(running, chunk);
    }
    proven_mem_view_t whole = { .ptr = (const proven_byte_t *)batch, .size = sizeof batch };
    EXAMPLE_REQUIRE(running == proven_crc32(whole),
                    "chunked and whole-buffer CRC-32 must agree, whatever the chunking");

    printf("events=%zu recent=%zu crc32=%08x\n",
           (size_t)events.len, (size_t)recent.len, (unsigned)running);

    proven_map_destroy(&internal);
    proven_map_destroy(&from_network);
    proven_map_destroy(&counts);
    PROVEN_RING_DESTROY(&recent);
    PROVEN_ARRAY_DESTROY(&events);
    return EXAMPLE_OK();
}
```

반례 — 도착 순서 로그에 이진 탐색을 쓰는 경우:

```text
const event_t *e = proven_array_binary_search(&events, &key, by_client);   /* wrong */
```

`proven_array_binary_search()`는 같은 비교 함수로 정렬된 구간에서만 쓸 수 있다. 정렬되지 않은
입력에서 이 함수는 실패하지 않는다. 반씩 좁혀 가다 그 자리에 있던 원소를 답이라고 내놓거나, 찾는
원소가 두 칸 옆에 있는데도 없다고 말한다.

반례 — 삽입을 사이에 두고 `get_mut` 포인터를 붙들고 있는 경우:

```text
proven_u32 *seen = proven_map_get_mut(&counts, k);
proven_err_t e = proven_map_set(&counts, other_key, &one);   /* may rehash */
*seen += 1;                                                  /* wrong: may dangle */
```

삽입할 수 있는 무언가를 사이에 두고 붙들 것은 포인터가 아니라 키다.

반례 — 프로그램 밖에서 온 키에 빠른 해시를 쓰는 경우:

```text
proven_result_map_t m = proven_map_create_trusted(alloc, 64, PROVEN_KEY_TYPE_U8_BORROWED,
                                                  sizeof(session_t), alignof(session_t));
/* keys are HTTP header names taken from the request */   /* wrong */
```

그것이 바로 키가 있는 해시가 막으려는 경우다. 키를 나 아닌 누군가가 고른다면
`proven_map_create()`를 쓴다.
