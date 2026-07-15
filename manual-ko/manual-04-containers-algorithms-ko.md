# 4장: 컨테이너와 알고리즘

이 장은 `array.h`, `list.h`, `ring.h`, `map.h`, `algorithm.h`를 다룬다.

## 목차

1. [동적 배열](#1-dynamic-array)
2. [침습적(intrusive) 리스트](#2-intrusive-list)
3. [링 버퍼](#3-ring-buffer)
4. [해시 맵](#4-hash-map)
5. [알고리즘](#5-algorithms)
6. [용도별 해싱](#6-hashing-by-use-case)
7. [바이트를 텍스트로: hex와 Base64](#7-bytes-to-text-hex-and-base64)
8. [예제와 오용 사례](#8-examples-and-misuse-cases)

## 1. 동적 배열

`proven_array_t`는 제네릭 확장 벡터다. allocator를 내부에 저장하며 연속된 원소 저장소를 소유한다.

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

`proven_list_t`는 침습적 이중 연결 원형 리스트다. 노드를 할당하지 않는다. 각 사용자 객체가 `proven_list_node_t`를 내장한다.

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

`proven_ring_t`는 고정 용량 FIFO다. 가득 차면 push가 실패한다. allocator를 내부에 저장한다.

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

`proven_map_t`는 tombstone을 갖는 open-addressing 해시 맵이다. 정수 키와 borrowed 또는 owned U8 문자열 키를 지원한다. allocator를 내부에 저장한다.

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
  공격자 모델도 없는 freestanding 타깃에서는 문자열 키가 FNV로 후퇴한다.)
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
scratch allocator의 임시 버퍼로 담아두므로 삽입이 별칭 안전(alias-safe)해진다. 영구
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

`proven_sha256_t`는 아무것도 할당하지 않는다 — [호출자 소유 상태](manual-ko.md#42-caller-owned-state-no-destroy-do-not-copy)이므로 파괴할 것이 없다.

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

<!-- example: manual/examples/ex_04_hash.c -->
```c
/*
 * Hashing, by use case. The module gives you exactly one function per job, so the only
 * decision is which job you have - and getting THAT wrong is the whole danger.
 */

int main(void) {
    proven_mem_view_t data = proven_mem_view_from_u8(PROVEN_LIT("the quick brown fox"));

    /* Job 1: hash a key into your own table, trusted input. Fast, non-cryptographic. */
    proven_u64 table_hash = proven_hash_bytes(data);
    EXAMPLE_REQUIRE(table_hash != 0, "FNV-1a produces a spread-out 64-bit value");

    /* Job 2: hash a key from UNTRUSTED input. Same purpose, but an attacker who picks the
     * input still cannot make everything collide, because they do not have the key. Pick
     * the key once at startup from real randomness; a fixed key defeats the point. */
    proven_byte_t key[16] = { 0 };   /* in real code: fill from a random source, once */
    proven_u64 safe_hash = proven_hash_keyed(data, key);
    EXAMPLE_REQUIRE(safe_hash != table_hash, "a keyed hash is a different function");

    /* Job 3: did these bytes get corrupted? A checksum, not a hash. Interoperates with
     * gzip/zlib/PNG, which all carry this exact CRC-32. */
    proven_u32 checksum = proven_crc32(data);
    /* The canonical CRC-32 sanity value, so you can see it is the real one: */
    EXAMPLE_REQUIRE(proven_crc32(proven_mem_view_from_u8(PROVEN_LIT("123456789"))) == 0xcbf43926u,
                    "CRC-32 of \"123456789\" is the shared check value");
    (void)checksum;

    /* Job 4: fingerprint content - dedup, content-addressing, "are these the same file",
     * answered safely even against someone trying to forge a match. This is the one you
     * reach for when the answer must not be foolable. */
    proven_byte_t digest[PROVEN_SHA256_SIZE];
    proven_sha256(data, digest);

    char hex[65];
    proven_sha256_to_hex(digest, hex);
    /* The same spelling sha256sum and git print, so it interoperates: */
    EXAMPLE_REQUIRE(hex[64] == '\0' && proven_cstr_len(hex) == 64,
                    "a SHA-256 fingerprint is 64 lowercase hex characters");

    /* SHA-256 streams, for content you cannot hold in memory at once - the digest depends
     * only on the bytes, never on how they were chunked. */
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

테스트 스위트가 컴파일하고 실행한다:

<!-- example: manual/examples/ex_04_encode.c -->
```c
/*
 * Bytes to text, by use case. The rule is the same one hashing follows: one function per job,
 * and the danger is picking the wrong job. Hex for something a human reads; Base64URL for
 * something that goes in a URL; standard Base64 for something that goes on the wire.
 */

int main(void) {
    proven_mem_view_t data = proven_mem_view_from_u8(PROVEN_LIT("the quick brown fox"));

    /* Job 1: a digest a human will read or paste - hex, the spelling sha256sum and git use. */
    proven_byte_t hex[64];   /* proven_hex_encoded_size(19) = 38 */
    proven_size_t hn = 0;
    EXAMPLE_REQUIRE(proven_is_ok(proven_hex_encode(data, hex, sizeof hex, &hn)),
                    "hex encode into a buffer sized by proven_hex_encoded_size");
    EXAMPLE_REQUIRE(hn == proven_hex_encoded_size(data.size), "two hex chars per byte");

    /* Job 2: a token that goes in a URL - Base64URL, so nothing needs percent-escaping and
     * there is no '=' padding for a parser to trip over. */
    proven_byte_t token_bytes[16] = { 0 };   /* in real code: proven_random_bytes(token_bytes, 16) */
    proven_byte_t url[32];
    proven_size_t un = 0;
    EXAMPLE_REQUIRE(proven_is_ok(proven_base64url_encode(
                        (proven_mem_view_t){ token_bytes, sizeof token_bytes }, url, sizeof url, &un)),
                    "base64url encode a token");
    /* No '=' in a URL-safe token. */
    bool has_pad = false;
    for (proven_size_t i = 0; i < un; ++i) if (url[i] == '=') has_pad = true;
    EXAMPLE_REQUIRE(!has_pad, "the URL form emits no padding");

    /* Job 3: bytes on the wire - standard Base64, the +/= alphabet HTTP and MIME expect. */
    proven_byte_t b64[64];
    proven_size_t bn = 0;
    EXAMPLE_REQUIRE(proven_is_ok(proven_base64_encode(data, b64, sizeof b64, &bn)),
                    "standard base64 encode");

    /* And it round-trips: decode gives back exactly the bytes. A decoder that accepts both
     * alphabets and padded-or-not is deliberate - real input comes in every shape. */
    proven_byte_t back[32];
    proven_size_t dn = 0;
    EXAMPLE_REQUIRE(proven_is_ok(proven_base64_decode(
                        (proven_mem_view_t){ b64, bn }, back, sizeof back, &dn)),
                    "decode the base64 back");
    EXAMPLE_REQUIRE(dn == data.size && proven_memcmp(back, data.ptr, dn) == 0,
                    "what comes back is exactly what went in");

    /* The point of a validating decoder: junk is refused, not guessed. A caller who fed this
     * to a two-line loop would read past the end or get a silently short result. */
    proven_err_t bad = proven_base64_decode(
        proven_mem_view_from_u8(PROVEN_LIT("not valid base64!!")), back, sizeof back, &dn);
    EXAMPLE_REQUIRE(bad == PROVEN_ERR_INVALID_ENCODING,
                    "a stray character is INVALID_ENCODING, with nothing committed");

    /* And a buffer one byte too small is refused, never truncated. */
    proven_byte_t tiny[4];
    EXAMPLE_REQUIRE(proven_hex_encode(data, tiny, sizeof tiny, &hn) == PROVEN_ERR_OUT_OF_BOUNDS,
                    "a too-small output buffer is OUT_OF_BOUNDS, not a truncated prefix");

    (void)hex; (void)url; (void)un;
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

<!-- example: manual/examples/ex_04_array.c -->
```c
/*
 * proven_array_t is a growable vector of one element type. It stores the
 * allocator you created it with, so push can grow and destroy can free without
 * you passing the allocator back in - which also means the array must be
 * destroyed with nothing but itself.
 *
 * The PROVEN_ARRAY_* macros are the type-safe face of the void* core: they
 * derive sizeof/alignof from the type and hand the element to the array by
 * pointer, so pushing an int into an array of records will not compile.
 */

/* A task queue keyed by priority. Real data is low-cardinality: three
 * priorities, many tasks. That matters for the sort - see below. */
typedef struct {
    int priority;
    int id;
} task_t;

/* The comparator is the array's ordering. The same one MUST be used for the
 * sort and for the binary search - a search under a different order is a bug
 * the compiler cannot see.
 *
 * The (x > y) - (x < y) form is deliberate: subtracting the two ints would
 * overflow for large values and hand back a nonsense sign. */
static int cmp_priority(const void *a, const void *b) {
    int x = ((const task_t *)a)->priority;
    int y = ((const task_t *)b)->priority;
    return (x > y) - (x < y);
}

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    /* --- push / get / pop --------------------------------------------------- */
    /* init_cap is a hint, not a limit: push grows past it. Sizing it right just
     * avoids the reallocations. */
    proven_result_array_t r = PROVEN_ARRAY_INIT(alloc, task_t, 4);
    EXAMPLE_REQUIRE(proven_is_ok(r.err), "creating an array of task_t must succeed");
    if (!proven_is_ok(r.err)) {
        return 1;
    }
    proven_array_t tasks = r.value;

    /* Deliberately duplicate-heavy, because that is what real keys look like:
     * a status column, an enum, a bucket id. */
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

    /* get returns a pointer INTO the array's storage - it is not a copy, and the
     * next push may reallocate and leave it dangling. Fetch it after the pushes,
     * use it, do not store it. */
    const task_t *front = PROVEN_ARRAY_GET(&tasks, task_t, 0);
    EXAMPLE_REQUIRE(front && front->id == 10, "element 0 is the first one pushed");

    /* Out of range is a null pointer, not a crash and not UB. */
    EXAMPLE_REQUIRE(PROVEN_ARRAY_GET(&tasks, task_t, 99) == NULL, "an out-of-range index yields NULL");

    /* pop copies the last element out (pass NULL to just discard it). */
    task_t last = {0};
    proven_err_t err = PROVEN_ARRAY_POP(&tasks, task_t, &last);
    EXAMPLE_REQUIRE(proven_is_ok(err), "popping a non-empty array must succeed");
    EXAMPLE_REQUIRE(last.id == 17 && tasks.len == 7, "pop returns the last element and shrinks the array");

    /* Put it back: the rest of the example wants all eight. */
    err = PROVEN_ARRAY_PUSH(&tasks, task_t, last);
    EXAMPLE_REQUIRE(proven_is_ok(err), "pushing the popped element back must succeed");

    /* --- sort --------------------------------------------------------------- */
    /* An introsort: O(n log n) is a guarantee, not an average - the heapsort
     * fallback rules out the quadratic case an adversary could otherwise steer
     * you into. And equal keys are the FAST path here: everything equal to the
     * pivot is partitioned into a run that is never recursed into, so the
     * duplicate priorities above cost less than distinct ones would, not more.
     *
     * It is not stable: task 10 and task 12 may come out in either order. */
    proven_array_sort(&tasks, cmp_priority);

    for (proven_size_t i = 1; i < tasks.len; ++i) {
        const task_t *prev = PROVEN_ARRAY_GET(&tasks, task_t, i - 1);
        const task_t *cur  = PROVEN_ARRAY_GET(&tasks, task_t, i);
        EXAMPLE_REQUIRE(prev && cur && prev->priority <= cur->priority,
                        "after sorting, priorities must be non-decreasing");
    }

    /* --- binary search ------------------------------------------------------ */
    /* Only legal because the array is sorted by this exact comparator. The key
     * is a whole element, but only the fields the comparator reads matter. */
    task_t key = { .priority = 1, .id = 0 };
    const task_t *hit = (const task_t *)proven_array_binary_search(&tasks, &key, cmp_priority);
    EXAMPLE_REQUIRE(hit != NULL, "priority 1 is present, so the search must find it");
    EXAMPLE_REQUIRE(hit && hit->priority == 1, "the hit must be an element with the searched key");
    /* With duplicate keys it finds SOME element with that key, not the first one.
     * If you need the first, scan backwards from the hit. */

    task_t absent = { .priority = 9, .id = 0 };
    EXAMPLE_REQUIRE(proven_array_binary_search(&tasks, &absent, cmp_priority) == NULL,
                    "a key that is not there must return NULL");

    printf("tasks: %zu sorted, found priority %d (id %d)\n",
           (size_t)tasks.len, hit ? hit->priority : -1, hit ? hit->id : -1);

    /* The array owns its element storage; destroy returns it through the
     * allocator the array was created with. Elements are plain data here - if
     * they owned anything, you would have to destroy each one first. */
    PROVEN_ARRAY_DESTROY(&tasks);
    return EXAMPLE_OK();
}
```

### 완성 예제: 정수 키 맵과 owned 문자열 키 맵

테스트 스위트가 컴파일하고 실행한다. owned 키 절반이 잘못 이해하기 쉬운 요점을 증명한다: 맵이 키를 복사하므로, 그것을 만든 버퍼는 즉시 재사용해도 되는 여러분의 것이다.

<!-- example: manual/examples/ex_04_map.c -->
```c
/*
 * proven_map_t is a flat open-addressing hash map. The value type is fixed at
 * create time and stored inline in the bucket array - there is no per-entry
 * allocation for values, and get hands back a pointer straight into that array.
 *
 * The interesting decision is the KEY:
 *
 *   PROVEN_KEY_TYPE_INT          - the key is a proven_size_t. Nothing to own.
 *   PROVEN_KEY_TYPE_U8_BORROWED  - the bucket stores your pointer and length.
 *                                  The map never copies the bytes, so YOU must
 *                                  keep them alive and unmoved for as long as
 *                                  the entry exists. Right for string literals.
 *   PROVEN_KEY_TYPE_U8_OWNED     - the map copies the key bytes into its own
 *                                  storage and frees them again. Right for keys
 *                                  built at runtime, which is most of them.
 *
 * The second half of this example is the reason OWNED exists.
 */

typedef struct {
    int  level;
    long score;
} player_t;

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    /* --- integer keys ------------------------------------------------------- */
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

    /* set on an existing key replaces the value; it does not add an entry. */
    err = PROVEN_MAP_SET_INT(&by_id, 7, player_t, ((player_t){ .level = 2, .score = 40 }));
    EXAMPLE_REQUIRE(proven_is_ok(err), "re-setting an existing key must succeed");
    EXAMPLE_REQUIRE(by_id.len == 2, "re-setting a key replaces its value rather than adding an entry");

    /* get returns a pointer into the bucket array, or NULL when absent. It is
     * invalidated by any insert that rehashes - look it up, use it, drop it. */
    const player_t *p = PROVEN_MAP_GET_INT(&by_id, player_t, 7);
    EXAMPLE_REQUIRE(p && p->level == 2 && p->score == 40, "get must see the replaced value");
    EXAMPLE_REQUIRE(PROVEN_MAP_GET_INT(&by_id, player_t, 999) == NULL, "a missing key yields NULL");

    err = PROVEN_MAP_REMOVE_INT(&by_id, 7);
    EXAMPLE_REQUIRE(proven_is_ok(err), "removing a present key must succeed");
    EXAMPLE_REQUIRE(PROVEN_MAP_GET_INT(&by_id, player_t, 7) == NULL, "a removed key is gone");
    EXAMPLE_REQUIRE(by_id.len == 1, "removal decrements the live entry count");

    PROVEN_MAP_DESTROY(&by_id);

    /* --- owned string keys --------------------------------------------------- */
    /* Same map, keyed by a name that we build at runtime - the case where a
     * borrowed key would be a dangling pointer waiting to happen. */
    proven_result_map_t rm = PROVEN_MAP_INIT_U8_OWNED(alloc, player_t, 8);
    EXAMPLE_REQUIRE(proven_is_ok(rm.err), "creating an owned-string-keyed map must succeed");
    if (!proven_is_ok(rm.err)) {
        return 1;
    }
    proven_map_t by_name = rm.value;

    /* A scratch buffer we intend to reuse for every key. With a BORROWED map
     * that plan is fatal: every entry would point at these same bytes. */
    proven_byte_t scratch[32];
    proven_u8str_t name = proven_u8str_borrow(scratch, sizeof scratch);

    err = proven_u8str_append(&name, PROVEN_LIT("ada"));
    EXAMPLE_REQUIRE(proven_is_ok(err), "building the first key must succeed");

    /* set_u8_owned COPIES the key bytes into map storage. After it returns, the
     * map's key no longer has anything to do with `scratch`. */
    err = PROVEN_MAP_SET_U8_OWNED(&by_name, proven_u8str_as_view(&name), player_t,
                                  ((player_t){ .level = 9, .score = 5000 }));
    EXAMPLE_REQUIRE(proven_is_ok(err), "inserting with an owned key must succeed");

    /* So the buffer is immediately free to be reused for the next key... */
    err = proven_u8str_reset(&name);
    EXAMPLE_REQUIRE(proven_is_ok(err), "the key buffer is ours again the moment set returns");
    err = proven_u8str_append(&name, PROVEN_LIT("grace"));
    EXAMPLE_REQUIRE(proven_is_ok(err), "overwriting the buffer with the next key must succeed");

    err = PROVEN_MAP_SET_U8_OWNED(&by_name, proven_u8str_as_view(&name), player_t,
                                  ((player_t){ .level = 4, .score = 700 }));
    EXAMPLE_REQUIRE(proven_is_ok(err), "inserting the second owned key must succeed");

    /* ...and the first entry is untouched by that overwrite. This is the whole
     * point: the map holds its own copy of "ada", not a view of a buffer that
     * now says "grace". A BORROWED map would report two entries both keyed
     * "grace" - or worse, one keyed by freed memory. */
    const player_t *ada = PROVEN_MAP_GET_U8_OWNED(&by_name, player_t, PROVEN_LIT("ada"));
    EXAMPLE_REQUIRE(ada && ada->score == 5000, "the copied key survives the caller reusing its buffer");

    const player_t *grace = PROVEN_MAP_GET_U8_OWNED(&by_name, player_t, PROVEN_LIT("grace"));
    EXAMPLE_REQUIRE(grace && grace->score == 700, "the second key is a separate entry");
    EXAMPLE_REQUIRE(by_name.len == 2, "two distinct keys means two entries");

    /* Remove frees the key copy the map made - you never free it yourself. */
    err = PROVEN_MAP_REMOVE_U8_OWNED(&by_name, PROVEN_LIT("ada"));
    EXAMPLE_REQUIRE(proven_is_ok(err), "removing an owned key must succeed");
    EXAMPLE_REQUIRE(PROVEN_MAP_GET_U8_OWNED(&by_name, player_t, PROVEN_LIT("ada")) == NULL,
                    "the removed entry is gone");

    printf("map: %zu name(s) left, grace at level %d\n",
           (size_t)by_name.len, grace ? grace->level : -1);

    /* destroy frees the bucket array AND every key copy still in it ("grace"
     * here). `scratch` is ours and outlives the map; the borrowed `name` handle
     * has nothing to free. */
    PROVEN_MAP_DESTROY(&by_name);
    proven_u8str_destroy(alloc, &name);
    return EXAMPLE_OK();
}
```
