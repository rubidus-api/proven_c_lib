# 2장: 할당 — 힙, 아레나, 풀, 버퍼

**Part II — 모든 프로그램이 쓰는 어휘. 선행 장: [1장](manual-01-foundation-ko.md).**
**이 장을 마치면** 반사적으로 `malloc`을 집어드는 대신 할당 전략을 의도적으로 고를 수 있고,
세 가지 비용 중 어느 것을 치르고 있는지 알게 된다.

이 장은 `heap.h`, `arena.h`, `pool.h`, `allocator.h`, `buffer.h`를 다룬다. 예전에 이 장의 7절이었던
스레드 안전성과 포인터 provenance 자료는 이제 나머지 동시성 주제와 함께
[6장](manual-06-execution-and-platform-ko.md)에 있다 — 책에서 가장 어려운 자료가 맨 앞쪽 장 중
하나에 들어앉아 있었기 때문이다.

## 목차

1. [할당이 매개변수인 이유, 그리고 heap allocator](#1-할당이-매개변수인-이유-그리고-heap-allocator)
2. [Arena: 여러 객체, 하나의 수명](#2-arena-여러-객체-하나의-수명)
3. [Pool: 여러 객체, 하나의 크기](#3-pool-여러-객체-하나의-크기)
4. [Allocator trait](#4-allocator-trait)
5. [원시 바이트 버퍼](#5-원시-바이트-버퍼)
6. [예제와 오용 사례](#6-예제와-오용-사례)

## 1. 할당이 매개변수인 이유, 그리고 heap allocator

### `malloc`의 문제

`malloc`은 전역이다. 어떤 함수든 호출할 수 있고, 시그니처의 그 무엇도 함수가 호출하는지 아닌지를
말해 주지 않으며, 메모리가 무엇에 쓰이든 모든 호출은 똑같은 범용 allocator로 간다.

여기서 따라 나오는 결과가 넷이고, 아마 이미 겪어 봤을 것이다:

- **시그니처만 보고는 함수가 할당하는지 알 수 없다.** `char *build_message(int n)`은 할당할 수도,
  정적 버퍼 안의 포인터를 반환할 수도, 리터럴을 반환할 수도 있다. 세 경우 모두 타입이 같으므로
  호출자는 해제해야 하는지 알 수 없고, 답은 틀렸을지도 모르는 주석 속에 있다.
- **프로그램의 한 부분만 전략을 바꿀 수 없다.** 파서가 파싱이 끝날 때 전부 죽는 작은 할당을 만
  번 한다면, `malloc`과 `free`는 만 번의 범용 할당과 만 번의 해제를 한다. bump allocator라면 한
  번이면 된다. 모든 호출 지점을 다시 쓰지 않고는 그렇게 하겠다고 말할 방법이 없다.
- **실패 경로를 테스트할 수 없다.** `malloc`을 원할 때 실패시키려면 전역으로 가로채야 한다 —
  `LD_PRELOAD`, 링커 트릭, 라이브러리 내부 호출까지 잡아채는 `#define malloc my_malloc`. 그런데
  정작 가장 테스트하고 싶은 분기는 메모리가 바닥났을 때 실행되는 그 분기다.
- **힙이 없는 곳에서는 아예 쓸 수 없다.** 펌웨어, 커널, 부트로더: `malloc`이 없고, 그것을
  전제하는 모든 라이브러리는 쓸 수 없다.

`malloc`은 잘못 설계된 것이 아니다. 그것은 고정된 정책이며, 다르게 하라고 말할 매개변수를 받지
않는다는 사실 때문에 모든 호출 지점에 하드코딩되어 있는 것이다.

### 이 라이브러리가 대신 하는 것

**allocator는 값이고, 매개변수다.** 할당할 수 있는 함수는 `proven_allocator_t`를 받고, 할당할 수
없는 함수는 받지 않는다. 발상은 그게 전부이고, 모든 결과가 여기서 따라 나온다:

```text
proven_err_t proven_u8str_append(proven_u8str_t *str, proven_u8str_view_t data);
proven_err_t proven_u8str_append_grow(proven_allocator_t alloc, proven_u8str_t *str, proven_u8str_view_t data);
```

시그니처를 읽어 보라. 첫 번째는 문자열을 키울 수 없으므로 텍스트가 들어가지 않으면 실패한다. 두
번째는 키울 수 있고, allocator를 받음으로써 그렇다고 말한다. 어느 쪽을 호출했는지 궁금해할 일이
결코 없다.

이제 같은 코드가 호출자가 고르는 세 가지 서로 다른 전략과 함께 동작한다:

| Allocator | 얻는 방법 | 개별 해제? | 쓸 때 |
|---|---|---|---|
| **Heap** | `proven_heap_allocator()` | 예 | 일반적인 경우. 서로 무관한 수명을 가진 객체들. |
| **Arena** | `proven_arena_create(backing)` 후 `proven_arena_as_allocator(&a)` | **아니오** — free는 no-op이다; 전체를 reset하거나 destroy한다 | 전부 같은 순간에 죽는 많은 할당: 요청 하나, 프레임 하나, 파싱 하나. |
| **Pool** | `proven_pool_init(&p, base, size, align, bin_cap)` 후 `proven_pool_as_allocator(&p)` | 예, free list로 들어간다 | **하나의 고정 크기**인 객체를 반복해서 할당하고 해제할 때. |

heap에서 시작하라. 이유가 있을 때 나머지 둘로 손을 뻗되, 그 이유는 대개 측정이다.

### Heap allocator

```text
proven_allocator_t proven_heap_allocator(void);
```

이것은 라이브러리의 인터페이스를 입은 `malloc`, `realloc`, `free`다. 그러지 않을 구체적인 이유가
없다면 이것을 써야 하며, 여기에 영리한 구석은 전혀 없다 — 그게 요점이다. 이 매뉴얼에서 달리 할
이유가 없는 모든 예제는 이것을 쓴다:

```c
proven_allocator_t heap = proven_heap_allocator();

proven_result_u8str_t s = proven_u8str_create(heap, 64);
if (!proven_is_ok(s.err)) {
    return;   /* nothing was created, so there is nothing to destroy */
}
(void)proven_u8str_append(&s.value, PROVEN_LIT("ready"));
proven_u8str_destroy(heap, &s.value);   /* the SAME allocator */
```

값은 네 워드 — 컨텍스트 포인터 하나와 함수 포인터 셋 — 이고 값으로 전달된다. 복사는 공짜이고,
자신의 struct에 저장해도 되며, 파괴할 필요도 없다.

**freestanding 빌드에서는 `proven_heap_allocator`가 존재하지 않는다.** 감쌀 `malloc`이 없기
때문이다. 이는 우회해야 할 제약이 아니라, 라이브러리 전체가 allocator를 매개변수로 받는 바로 그
이유이며, 정적 배열 위의 arena가 같은 코드를 마이크로컨트롤러에서 돌아가게 하는 이유다.
[freestanding 모드](manual-freestanding-ko.md)를 참고하라.

잘못된 예 — 생성할 때와 다른 allocator로 파괴하기:

```text
proven_result_u8str_t s = proven_u8str_create(arena_alloc, 64);
proven_u8str_destroy(heap_alloc, &s.value);   /* wrong: heap free on arena memory */
```

객체는 자신을 만든 allocator를 기억하지 않는다 — 그것이 객체를 작게 유지하는 요인이다. 오늘날
이것을 검사하는 것은 없으며, 실패는 다른 어딘가에서 나중에 드러나는 힙 손상이다. 구조적으로 짝을
지어라: allocator를 객체 옆에 두거나, 둘을 함께 전달하라.

## 2. Arena: 여러 객체, 하나의 수명

### 문제: 사망 시점이 같은 수많은 작은 할당

설정 파일을 파싱한다고 하자. 키마다 문자열 하나, 값마다 문자열 하나, 섹션마다 노드 하나를
할당한다 — 작은 할당 수천 개다. 그리고 파싱이 끝나면 결과를 만들고, 그 할당 하나하나는 모두 같은
순간에 쓰레기가 된다.

`malloc`으로는 그 대가를 두 번 치른다. 할당마다 free list를 검색하고, 장부를 갱신하고, 헤더가
붙은 메모리를 반환한다; `free`마다 메모리를 돌려주고 어쩌면 이웃과 병합한다. 그리고 하나도 빠뜨려선
안 된다. 잊어버린 `free` 하나면 누수이기 때문이다.

arena가 하는 관찰은 **그 객체들이 전부 같은 수명을 가진다**는 것이며, 따라서 개별 해제는 무의미한
작업이라는 것이다. 함께 죽는다면, 함께 해제될 수 있다.

### Arena가 동작하는 방식

arena는 메모리 블록 하나와 오프셋 하나다. 할당한다는 것은 요청받은 정렬에 맞게 오프셋을 올림하고,
그 주소를 반환하고, 오프셋을 앞으로 옮기는 것이다. 알고리즘 전체가 이게 전부다:

```text
[####used####|                    free                    ]
              ^ offset
```

- **할당은 명령어 몇 개다.** 검색도, free list도, 할당마다 붙는 헤더도 없다.
- **`free`는 아무 일도 하지 않는다.** 의도적으로 no-op이다.
- **회수는 arena 전체를 reset하거나 destroy해서 한다.** 오프셋을 0으로 되돌리는 것이다. 연산
  하나가 객체 만 개를 해제한다.

메모리는 여러분이 준다. `proven_arena_create`는 `proven_mem_mut_t`를 받는다 — 힙에서 받아 온
블록이든, `static` 배열이든, 스택 위의 영역이든 상관없다. arena는 스스로 할당하지 않으며, 그것이
밑에 힙이 없어도 동작하게 하는 요인이다.

### 포기하는 것

arena는 범용 allocator가 아니며, 그 맞바꿈은 실제적이다:

- **객체 하나를 해제할 수 없다.** 수명이 실제로 공유되지 않는다면, arena는 설계상 누수한다 —
  메모리는 reset에서만 회수된다.
- **바닥나는 것은 단단한 한계다.** backing 블록은 고정되어 있다. 여기서의 `PROVEN_ERR_NOMEM`은
  기계의 메모리가 바닥났다는 뜻이 아니라, 이 arena가 바닥났다는 뜻이다.
- **arena를 가리키는 모든 포인터는 reset에서 죽는다.** 한꺼번에, 아무 경고도 없이. reset보다 오래
  사는 view는 dangling view다. [0장](manual-00-start-here-ko.md#5-모든-페이지에서-만나게-될-다섯-가지-계약)의
  계약 2를 보라.

수명 공유가 프로그램에 대한 사실일 때 arena를 쓰라. 희망 사항일 때가 아니라.

### `proven_arena_t`

```text
typedef struct {
    proven_mem_mut_t backing;
    proven_size_t offset;
} proven_arena_t;
```

필드:

- `backing`: 호출자가 소유하는 가변 메모리 범위.
- `offset`: `backing` 내에서 다음 할당 위치.

### Arena 함수와 헬퍼

| API | 의도 | 매개변수 | 반환 |
|---|---|---|---|
| `proven_arena_create(backing)` | backing 슬라이스 위에 arena를 초기화한다. | `backing`: 호출자 소유 메모리. | `proven_arena_t`. |
| `proven_arena_reset(arena)` | arena의 모든 할당을 버린다. | `arena`. | void. |
| `proven_arena_destroy(arena)` | 형식적 정리. | `arena`. | void; 호출자 backing arena에는 no-op. |
| `proven_arena_alloc_aligned(arena, size, align)` | 명시적 정렬로 할당한다. | `arena`, 바이트 `size`, 2의 거듭제곱 `align`. | `proven_result_mem_mut_t`. |
| `proven_arena_realloc_aligned(arena, old_ptr, old_size, new_size, align)` | 재할당; 꼬리(tail) 할당을 제자리에서 확장/축소할 수 있다. | 이전 할당 세부 정보와 새 크기. | `proven_result_mem_mut_t`. |
| `proven_arena_alloc(arena, size)` | `PROVEN_DEFAULT_ALIGNMENT`로 할당한다. | `arena`, 바이트 `size`. | `proven_result_mem_mut_t`. |
| `proven_arena_alloc_aligned_or_panic(arena, size, align)` | 할당하거나 panic 훅을 호출한다. | `arena`, `size`, `align`. | `proven_mem_mut_t`. |
| `proven_arena_alloc_or_panic(arena, size)` | 기본 정렬 panic 할당. | `arena`, `size`. | `proven_mem_mut_t`. |
| `proven_arena_as_allocator(arena)` | arena를 `proven_allocator_t`로 노출한다. | `arena`. | allocator trait, 또는 널 arena에 대해 제로 allocator. |

Trait 어댑터 헬퍼:

- `proven_arena_alloc_trait`
- `proven_arena_realloc_trait`
- `proven_arena_free_trait`

이들은 allocator trait가 함수 포인터를 필요로 하기 때문에 노출된다. 애플리케이션 코드는 보통 대신 `proven_arena_as_allocator()`를 호출한다.

예제:

```c
alignas(max_align_t) proven_byte_t storage[4096];
proven_arena_t arena = proven_arena_create((proven_mem_mut_t){
    .ptr = storage,
    .size = sizeof storage,
});

proven_result_mem_mut_t r = proven_arena_alloc(&arena, 64);
if (!proven_is_ok(r.err)) {
    return;   /* the arena cannot grow: it reports NOMEM instead */
}

proven_arena_reset(&arena);   /* reclaims r and everything else at once */
proven_arena_destroy(&arena);
```

### 성장 가능한 컨테이너와 함께 쓰는 arena

성장 가능한 컨테이너는 arena allocator를 사용할 수 있지만, arena의 `free`가 no-op이므로 성장이 이전 블록을 버릴 수 있다. 가능하면 용량을 미리 확보(reserve)하라.

올바른 예:

```c
alignas(max_align_t) proven_byte_t storage[4096];
proven_arena_t arena = proven_arena_create((proven_mem_mut_t){
    .ptr = storage,
    .size = sizeof storage,
});
proven_allocator_t a = proven_arena_as_allocator(&arena);

/* Ask for the capacity up front, so growth never has to abandon a block. */
proven_result_array_t ar = PROVEN_ARRAY_INIT(a, int, 128);
if (!proven_is_ok(ar.err)) {
    return;
}
(void)PROVEN_ARRAY_PUSH(&ar.value, int, 10);
PROVEN_ARRAY_DESTROY(&ar.value);   /* correct, but arena free reclaims nothing */
proven_arena_reset(&arena);        /* this is what gives the bytes back */
```

잘못된 예 — 아주 작은 초기 용량에서 arena 안으로 성장시키기:

```text
proven_result_array_t ar = PROVEN_ARRAY_INIT(a, int, 1);
for (int i = 0; i < 10000; ++i) {
    PROVEN_ARRAY_PUSH(&ar.value, int, i); /* wrong: every regrow abandons the old block */
}
```

재성장할 때마다 arena에 더 큰 블록을 요청하고 이전 것을 "해제"하는데 — arena에서 그것은 아무 일도
하지 않는다. 배열 자체는 올바르게 끝나지만, arena는 지금까지 할당한 모든 중간 크기를 그대로 쥔 채
끝난다: 원하던 버퍼 하나 위에 1, 2, 4, 8 … 8192개 원소어치의 버려진 공간이 얹힌다. 위의 올바른
예처럼 용량을 미리 확보하라.

잘못된 예 — reset보다 오래 사는 view:

```text
proven_u8str_view_t name = /* ... built in the arena ... */;
proven_arena_reset(&arena);
use(name);                 /* wrong: those bytes are now free space */
```

이것이 arena의 가장 날카로운 모서리다. reset은 회수하는 메모리를 건드리지 않으므로 바이트는 보통
아직 그대로 있고, 버그는 보통 테스트에서 드러나지 않는다 — 다음 할당이 그 위에 덮어쓰기 직전까지는.

## 3. Pool: 여러 객체, 하나의 크기

### arena가 풀지 못하는 문제

arena는 수명이 공유된다고 가정한다. 정반대 모양을 한 작업 부하도 많다: 한 타입의 객체가 특별한
순서 없이 계속 만들어지고 파괴되는 경우다. 이벤트의 연결 리스트. 커졌다 작아졌다 하는 트리의
노드들. 생겼다 사라지는 연결 레코드들.

arena는 이것을 할 수 없다 — 객체 하나를 결코 회수하지 않으므로, 오래 도는 프로그램은 한없이 커질
것이다. 힙은 할 수 있고, 그것이 정확히 그 비용이다: 할당마다 검색하고, 해제마다 장부를 갱신하며,
범용 allocator는 이미 천 번이나 처리한 요청에 대해 범용적인 일을 한다.

pool이 하는 관찰은 **이 객체들이 전부 같은 크기**라는 것이다. 크기가 같다면, 해제된 것 하나가
앞으로의 요청에 정확히 들어맞으므로, 아무런 검색 없이 곧바로 다시 건네줄 수 있다.

### Pool이 동작하는 방식

pool은 해제된 블록들의 작은 스택 — *bin* — 을 유지하고, 뻔한 일을 한다:

- **할당**: bin에 뭔가 있으면 하나를 pop해서 반환한다. 포인터 읽기 하나와 감소 하나다. bin이 비어
  있을 때만 하위 allocator로 간다.
- **해제**: bin에 자리가 있으면 재사용을 위해 블록을 push한다. bin이 꽉 찼으면 pool이 메모리를
  영원히 세워 두지 않도록 하위 allocator로 되돌려준다.

따라서 `bin_cap`은 다이얼이다: 이 pool이 예비로 쥐고 있어도 되는 메모리의 양이다.

### 포기하는 것

- **하나의 pool은 정확히 하나의 크기와 정렬만 처리한다.** 그 밖의 요청은 다른 데서 처리되는 것이
  아니라 `PROVEN_ERR_INVALID_ARG`로 거부된다. pool이 만들어질 때보다 더 엄격한 정렬도 거부되고,
  더 느슨한 것은 블록이 이미 그것을 충족하므로 괜찮다.
- **pool은 살아 있는 객체를 추적하지 않는다.** `proven_pool_destroy`는 bin에 있는 것을 해제하지,
  여러분이 아직 쥐고 있는 것을 해제하지 않는다. 할당한 것을 먼저 전부 해제하라. 그러지 않으면
  누수인 *데다가* 이제 dangling이다.

arena 대 pool을 한 줄씩으로: arena는 *많이 할당하고 한 번에 전부 해제*를 위한 것이고, pool은
*같은 크기를 몇 번이고 저렴하게 할당하고 해제*하기 위한 것이다.

### `proven_pool_t`

```text
typedef struct {
    proven_allocator_t base_alloc;
    proven_size_t item_size;
    proven_size_t item_align;
    void **bin;
    proven_size_t bin_cap;
    proven_size_t bin_len;
} proven_pool_t;
```

필드:

- `base_alloc`: 새 블록과 재활용 bin에 사용되는 allocator.
- `item_size`: pool이 받아들이는 정확한 객체 크기. 그 외 크기는 `PROVEN_ERR_INVALID_ARG`로 거부된다.
- `item_align`: 모든 블록이 할당되는 정렬. *더 엄격한* 정렬 요청은 `PROVEN_ERR_INVALID_ARG`로 거부되고, 더 느슨한 요청은 블록이 이미 그것을 충족하므로 처리된다.
- `bin`: 캐시된 free-list 배열.
- `bin_cap`: 최대 캐시 블록 개수.
- `bin_len`: 현재 캐시 블록 개수.

### 재활용이 작동하는 방식

pool은 하나의 큰 슬랩(slab)을 소유하지 않는다. 각 항목을 `base_alloc`에서 개별적으로
할당하고, 해제된 블록들의 작은 **스택**(`bin`, 최대 `bin_cap`개의 포인터로 이루어진
배열)을 유지한다:

- **할당.** `bin_len > 0`이면 맨 위 포인터를 pop한다(O(1), `base_alloc` 호출 없음) —
  이것이 pool의 전체 요점이다. 그렇지 않으면 `base_alloc`로 넘어간다.
- **해제.** `bin_len < bin_cap`이면 재사용을 위해 포인터를 bin에 push한다;
  그렇지 않으면(bin이 꽉 참) `base_alloc`로 곧바로 되돌려 해제한다. 따라서 bin은
  pool이 재사용을 위해 세워둘 수 있는 메모리의 양을 제한한다.
- **파괴.** `proven_pool_destroy`는 bin에 아직 남아 있는 모든 포인터와 bin 배열
  자체를 해제한다 — 하지만 살아 있는(넘겨준) 항목은 추적하지 **않으므로**, pool을
  파괴하기 전에 할당한 모든 것을 해제해야 한다.

이는 pool을 지역(region) allocator가 아니라 수명이 짧은 동일 타입 객체(노드, 이벤트)를
위한 churn 최적화기(optimizer)로 만든다. "많이 할당하고 한 번에 전부 해제"하고 싶으면
`arena`를 사용하라; "같은 크기를 몇 번이고 저렴하게 할당/해제"하고 싶으면 `pool`을
사용하라.

#### 반례

```text
/* WRONG: the pool only handles its configured size/alignment. */
proven_allocator_t a = proven_pool_as_allocator(&pool);   /* item_size == sizeof(Node) */
a.alloc_fn(a.ctx, sizeof(BigThing), alignof(BigThing));   /* not the pool's item -> rejected */

/* WRONG: destroying while items are still live leaks (and dangles) them. */
void *p = a.alloc_fn(a.ctx, sizeof(Node), alignof(Node)).value.ptr;
proven_pool_destroy(&pool);   /* `p` is NOT freed and is now dangling */
/* RIGHT: free every handed-out item first, then destroy. */
```

### Pool 함수

| API | 의도 | 반환 |
|---|---|---|
| `proven_pool_init(pool, base_alloc, item_size, item_align, bin_cap)` | 고정 크기 pool을 초기화한다. | `PROVEN_OK` 또는 에러. |
| `proven_pool_as_allocator(pool)` | pool이 뒷받침하는 allocator trait를 반환한다. | `proven_allocator_t`. |
| `proven_pool_destroy(pool)` | 캐시된 블록과 bin 저장소를 해제한다. | void. |

예제:

```c
typedef struct Node { int value; } Node;

proven_pool_t pool = {0};
proven_err_t e = proven_pool_init(&pool, alloc, sizeof(Node), alignof(Node), 64);
if (!proven_is_ok(e)) {
    return;
}

proven_allocator_t node_alloc = proven_pool_as_allocator(&pool);
proven_result_mem_mut_t n = node_alloc.alloc_fn(node_alloc.ctx, sizeof(Node), alignof(Node));
if (proven_is_ok(n.err)) {
    /* Hand it back before destroy: the pool does not track live items. */
    node_alloc.free_fn(node_alloc.ctx, n.value.ptr);
}

proven_pool_destroy(&pool);
```

잘못된 예:

```text
node_alloc.alloc_fn(node_alloc.ctx, sizeof(LargerObject), alignof(LargerObject));
/* wrong: one pool is for one fixed object size and alignment */
```

## 4. Allocator trait

### 이 절이 첫 번째가 아니라 네 번째인 이유

여러분은 세 가지 allocator를 그들이 공유하는 인터페이스를 보지 않고 이미 써 보았고, 그것은
의도적이었다. 인터페이스는 이 발상에서 가장 재미없는 부분이고 아무 맥락 없이 읽기에 가장 어려운
것이다: 함수 포인터 typedef 셋과 정렬 계약은, 그 뒤에 있는 heap과 arena와 pool을 보기 전까지는
거의 아무 의미도 없다.

이 절이 필요한 이유는 둘이다: 자신만의 allocator를 작성하는 것, 그리고 이 라이브러리의 모든
allocator가 지키기로 약속한 규칙을 이해하는 것. 위의 셋을 *쓰기만* 할 생각이라면
`proven_heap_allocator()`, `proven_arena_as_allocator()`, `proven_pool_as_allocator()`가 API
전부이니 건너뛰어도 된다.

여기서 trait란 인터페이스로 쓰이는 함수 포인터 struct를 말한다 — 손으로 써 낸, C 버전의 가상
테이블이다. `proven_allocator_t`는 라이브러리에서 가장 중요한 trait이고, `stream.h`와 `random.h`도
같은 모양을 쓴다.

### 함수 포인터 타입

```text
typedef proven_result_mem_mut_t (*proven_alloc_fn_t)(
    void *ctx,
    proven_size_t size,
    proven_size_t align
);

typedef proven_result_mem_mut_t (*proven_realloc_fn_t)(
    void *ctx,
    void *old_ptr,
    proven_size_t old_size,
    proven_size_t new_size,
    proven_size_t align
);

typedef void (*proven_free_fn_t)(void *ctx, void *ptr);
```

의도:

- `alloc_fn`은 새로운 바이트 슬라이스를 할당한다.
- `realloc_fn`은 할당 크기를 변경하며 failure-atomic이어야 한다: 실패 시
  이전 블록은 여전히 유효하고 변경되지 않으므로, 호출자는 아무것도 잃지 않는다.
- 블록은 할당될 때와 **같은 `align`**으로 재할당되고 해제되어야 한다. allocator는
  과도하게 정렬된(over-aligned) 요청에 대해 일반 요청과는 다른 하위 메커니즘을
  선택할 수 있으며, heap allocator가 실제로 그렇게 한다: `align <=
  alignof(max_align_t)` — 이 라이브러리의 모든 문자열, 버퍼, 바이트 배열이 여기에
  해당한다 — 는 제자리(in place)에서 성장이 일어날 수 있도록 `malloc`/`realloc`을
  거치고, 그보다 더 엄격하게 정렬된 것은 정렬된 allocator를 거친다. 블록을 다른
  정렬 클래스로 되돌려주는 것은 정의되지 않은 동작이다.
- `free_fn`은 메모리를 해제한다. allocator가 크기 메타데이터를 필요로 한다면, 내부적으로 그것을 추적해야 한다.

### `proven_allocator_t`

```text
typedef struct {
    void *ctx;
    proven_alloc_fn_t alloc_fn;
    proven_realloc_fn_t realloc_fn;
    proven_free_fn_t free_fn;
} proven_allocator_t;
```

필드:

- `ctx`: allocator별 상태.
- `alloc_fn`: 할당 함수.
- `realloc_fn`: 재할당 함수.
- `free_fn`: 해제 함수.

### 레퍼런스

| 멤버 / API | 형태 | 계약 |
|---|---|---|
| `ctx` | `void *` | allocator 자신의 상태. 호출자에게는 불투명하며, 첫 번째 인자로 되돌아온다. |
| `alloc_fn` | `proven_alloc_fn_t` | `align` 정렬로 `size` 바이트를 할당한다. `proven_result_mem_mut_t`를 반환한다. |
| `realloc_fn` | `proven_realloc_fn_t` | 크기를 바꾼다. **failure-atomic**: 실패 시 이전 블록은 손대지 않은 채 여전히 유효하다. |
| `free_fn` | `proven_free_fn_t` | 해제한다. 블록이 할당될 때와 같은 `align` 클래스를 주어야 한다. |
| `proven_alloc_is_valid(alloc)` | `static inline bool` | 모든 함수 포인터가 널이 아닐 때, 즉 trait를 호출할 수 있을 때 true. |

이 라이브러리의 모든 allocator가 지키며, 여러분의 것도 지켜야 하는 세 가지 규칙:

1. **같은 allocator, 전체 수명 동안.** 한 블록의 할당, 재할당, 해제는 하나의 allocator로 하라.
2. **같은 정렬 클래스.** 어떤 정렬로 할당된 블록은 그 정렬로 재할당되고 해제되어야 한다.
   allocator는 과도하게 정렬된 요청에 다른 메커니즘을 쓸 수 있다.
3. **실패는 아무것도 바꾸지 않는다.** 실패한 `realloc_fn`은 이전 블록을 유효하고 변경되지 않은
   채로 남긴다.

```text
static inline bool proven_alloc_is_valid(proven_allocator_t alloc);
```

목적: 모든 함수 포인터가 널이 아닌지 확인한다.

반환: trait를 호출할 수 있으면 true.

올바른 예:

```c
proven_allocator_t heap = proven_heap_allocator();
proven_err_t err = proven_alloc_is_valid(heap) ? PROVEN_OK : PROVEN_ERR_UNSUPPORTED;
(void)err;   /* a freestanding build hands back a zero allocator, not a valid one */
```

잘못된 예:

```text
proven_allocator_t alloc = {0};
proven_result_mem_mut_t r = alloc.alloc_fn(alloc.ctx, 64, 8); /* wrong: null call */
```

## 5. 원시 바이트 버퍼

### 무엇을 위한 것인가

`proven_buf_t`는 이 장에서 가장 단순한 것이다: 포인터 하나, 길이 하나, 용량 하나. 자기 바이트를
소유하고, 결코 성장하지 않으며, `proven_u8str_t`와 `proven_u16str_t`가 이것으로 만들어져 있다.

*텍스트*가 아니라 *바이트*를 원할 때 — 조립 중인 레코드, 소켓에 곧 써 넣을 프레임 — 그리고 최대
크기를 미리 알 때 직접 손을 뻗어라. 내용이 텍스트라면 대신 [3장](manual-03-strings-text-ko.md)의
문자열 타입을 쓰라. 같은 저장소에 더해 NUL 종료, view, 검색, 포매팅까지 준다.

여기 있는 다른 모든 것과 마찬가지로 allocator를 저장하지 **않으므로**, `proven_buf_destroy`는
`proven_buf_create`에 주었던 것과 같은 것을 받는다.

### `proven_buf_t`

```text
typedef struct {
    proven_byte_t *ptr;
    proven_size_t len;
    proven_size_t cap;
} proven_buf_t;
```

필드:

- `ptr`: 저장소 포인터.
- `len`: 현재 사용 중인 바이트.
- `cap`: 할당된 바이트.

### `proven_result_buf_t`

```text
typedef struct {
    proven_err_t err;
    proven_buf_t value;
} proven_result_buf_t;
```

### 버퍼 함수

| API | 의도 | 반환 |
|---|---|---|
| `proven_buf_create(alloc, cap)` | 용량이 0이 아닌 버퍼를 할당한다. | `proven_result_buf_t`; 유효하지 않은 allocator 또는 용량 0은 `PROVEN_ERR_INVALID_ARG`를 반환한다. |
| `proven_buf_append(buf, data)` | 원시 바이트가 들어맞으면 추가한다. | `PROVEN_OK` 또는 에러. |
| `proven_buf_destroy(alloc, buf)` | 일치하는 allocator로 버퍼 저장소를 해제한다. | void. |

예제:

```c
proven_result_buf_t r = proven_buf_create(alloc, 64);
if (!proven_is_ok(r.err)) {
    return;
}
proven_buf_t buf = r.value;

proven_err_t e = proven_buf_append(&buf, (proven_mem_view_t){
    .ptr = (const proven_byte_t *)"abc",
    .size = 3,
});
(void)e;   /* PROVEN_ERR_OUT_OF_BOUNDS if it would not fit: the buffer never grows */

proven_buf_destroy(alloc, &buf);
```

## 6. 예제와 오용 사례

### 파괴 시 allocator 일치시키기

올바른 예:

```c
proven_allocator_t heap = proven_heap_allocator();

proven_result_buf_t r = proven_buf_create(heap, 128);
if (!proven_is_ok(r.err)) {
    return;
}
proven_buf_destroy(heap, &r.value);   /* the same allocator that created it */
```

잘못된 예:

```text
proven_result_buf_t r = proven_buf_create(heap, 128);
proven_buf_destroy(other_alloc, &r.value); /* wrong: allocator mismatch */
```

### Arena의 free는 no-op이다

```c
alignas(max_align_t) proven_byte_t storage[1024];
proven_arena_t arena = proven_arena_create((proven_mem_mut_t){
    .ptr = storage,
    .size = sizeof storage,
});
proven_allocator_t a = proven_arena_as_allocator(&arena);

proven_result_mem_mut_t r = proven_arena_alloc(&arena, 32);
if (proven_is_ok(r.err)) {
    /* Legal, and correct to write - but it intentionally reclaims nothing. */
    a.free_fn(a.ctx, r.value.ptr);
}
proven_arena_reset(&arena);   /* only this gives the 32 bytes back */
```

### Pool 크기 계약

올바른 예:

```c
typedef struct Node { int value; } Node;

proven_pool_t pool = {0};
if (!proven_is_ok(proven_pool_init(&pool, alloc, sizeof(Node), alignof(Node), 8))) {
    return;
}
proven_allocator_t node_alloc = proven_pool_as_allocator(&pool);

/* The pool serves exactly the size and alignment it was configured with. */
proven_result_mem_mut_t n = node_alloc.alloc_fn(node_alloc.ctx, sizeof(Node), alignof(Node));
if (proven_is_ok(n.err)) {
    node_alloc.free_fn(node_alloc.ctx, n.value.ptr);
}

/* Anything else is refused with PROVEN_ERR_INVALID_ARG rather than served. */
proven_result_mem_mut_t wrong = node_alloc.alloc_fn(node_alloc.ctx, sizeof(Node) * 2, alignof(Node));
(void)wrong;

proven_pool_destroy(&pool);
```

잘못된 예:

```text
node_alloc.alloc_fn(node_alloc.ctx, sizeof(Other), alignof(Other));
/* wrong: PROVEN_ERR_INVALID_ARG - a pool is for one size and one alignment */
```

### 버퍼 append는 성장하지 않는다

잘못된 예:

```text
proven_buf_append(&buf, huge_data); /* returns out-of-bounds; no automatic growth */
```

성장이 필요하면 `proven_u8str_append_grow()`나 배열을 사용하라.

### 버퍼 append는 겹침을 허용한다

올바른 예:

```c
proven_result_buf_t r = proven_buf_create(alloc, 64);
if (!proven_is_ok(r.err)) {
    return;
}
proven_buf_t buf = r.value;

proven_err_t e = proven_buf_append(&buf, (proven_mem_view_t){
    .ptr = (const proven_byte_t *)"abcdefgh",
    .size = 8,
});
if (proven_is_ok(e)) {
    /* The source view points back into the buffer's own bytes. That is allowed:
     * append moves rather than copies, so the overlap is well defined. */
    e = proven_buf_append(&buf, (proven_mem_view_t){
        .ptr = buf.ptr + 2,
        .size = 4,
    });
}
(void)e;

proven_buf_destroy(alloc, &buf);
```

잘못된 예:

```text
proven_sys_mem_copy(buf.ptr + buf.len, buf.ptr + 2, 4); /* copy is not the public contract here */
```

### 실전 예제: 호출자가 제공한 메모리 위의 arena

테스트 스위트에서 컴파일되고 실행된다. arena를 손 뻗어 쓸 가치가 있게 만드는 bump-and-drop 수명을 보여준다: 할당은 거의 공짜이고, 개별적으로 해제되는 것은 없으며, `proven_arena_reset`이 전체 영역을 한 번에 회수한다.

<!-- example: manual/examples/ex_02_arena.c -->
```c
/*
 * An arena does not own memory: it bumps a pointer through memory YOU own. That
 * is the whole trade. Allocation is an add, individual frees do not exist, and
 * you get everything back at once with a reset.
 *
 * The shape that makes it worth using is bump-then-drop: a phase allocates
 * freely, the phase ends, one reset reclaims the lot. No per-object bookkeeping
 * to get wrong, and nothing to leak - the backing storage below is a plain
 * array with automatic storage duration.
 */

int main(void) {
    /* The backing store is the caller's. Over-align it so the arena can satisfy
     * any alignment a caller asks for out of the first byte. */
    alignas(max_align_t) static proven_byte_t storage[4096];

    proven_arena_t arena = proven_arena_create((proven_mem_mut_t){
        .ptr = storage,
        .size = sizeof storage,
    });

    /* --- bump ------------------------------------------------------------- */
    proven_result_mem_mut_t a = proven_arena_alloc(&arena, 64);
    EXAMPLE_REQUIRE(proven_is_ok(a.err), "64 bytes must fit in a 4 KiB arena");
    EXAMPLE_REQUIRE(a.value.ptr == storage, "the first allocation starts at the backing store");

    /* Explicit alignment when the type demands more than PROVEN_DEFAULT_ALIGNMENT.
     * The arena pads to reach it, so the bytes it skips are simply gone until reset. */
    proven_result_mem_mut_t b = proven_arena_alloc_aligned(&arena, 32, 64);
    EXAMPLE_REQUIRE(proven_is_ok(b.err), "an over-aligned block must still fit");
    EXAMPLE_REQUIRE(((uintptr_t)b.value.ptr % 64) == 0, "the block must honour the requested alignment");

    /* --- the arena as an allocator for another API ------------------------- */
    /* Anything in proven that takes a proven_allocator_t can be driven by the
     * arena. The string below therefore lives inside `storage`. */
    proven_allocator_t arena_alloc = proven_arena_as_allocator(&arena);
    EXAMPLE_REQUIRE(proven_alloc_is_valid(arena_alloc), "the arena must expose a usable allocator");

    proven_result_u8str_t s = proven_u8str_create(arena_alloc, 32);
    EXAMPLE_REQUIRE(proven_is_ok(s.err), "the arena should be able to back a 32-byte string");

    proven_err_t err = proven_u8str_append_grow(arena_alloc, &s.value, PROVEN_LIT("scratch line"));
    EXAMPLE_REQUIRE(proven_is_ok(err), "appending into an arena-backed string must succeed");

    /* Destroying it is still correct and still required by the ownership rules -
     * but arena free is a no-op, so it reclaims nothing. That is not a leak: the
     * bytes belong to `storage`, and the reset below is what returns them. */
    proven_u8str_destroy(arena_alloc, &s.value);

    proven_size_t used = arena.offset;
    EXAMPLE_REQUIRE(used > 64, "every allocation above came out of the same backing store");

    /* --- drop -------------------------------------------------------------- */
    /* One statement frees the 64-byte block, the aligned block and the string.
     * Reset costs the same whether ten objects were allocated or ten thousand. */
    proven_arena_reset(&arena);
    EXAMPLE_REQUIRE(arena.offset == 0, "reset must reclaim every allocation at once");

    /* Proof that the storage really is reusable: the next allocation lands back
     * at the start. Every pointer handed out before the reset is dangling now -
     * that is the price of the reset being free. */
    proven_result_mem_mut_t c = proven_arena_alloc(&arena, 64);
    EXAMPLE_REQUIRE(proven_is_ok(c.err), "allocation after reset must succeed");
    EXAMPLE_REQUIRE(c.value.ptr == storage, "after reset the arena bumps from the beginning again");

    /* --- exhaustion is an error, not a crash -------------------------------- */
    proven_result_mem_mut_t too_big = proven_arena_alloc(&arena, sizeof storage);
    EXAMPLE_REQUIRE(too_big.err == PROVEN_ERR_NOMEM, "an arena cannot grow: it reports NOMEM instead");

    printf("arena: %zu bytes used before reset, %zu in use now\n",
           (size_t)used, (size_t)arena.offset);

    /* Formal cleanup. A no-op for a caller-backed arena, but writing it keeps
     * the lifetime obvious if the backing store later becomes heap memory. */
    proven_arena_destroy(&arena);
    return EXAMPLE_OK();
}
```

### 실전 예제: 고정 크기 블록을 재활용하는 pool

테스트 스위트에서 컴파일되고 실행된다. 해제된 블록이 재활용 bin에서 곧장 다시 나오는 것과, pool이 무엇을 거부하는지를 보여준다.
<!-- example: manual/examples/ex_02_pool.c -->
```c
/*
 * A pool is a churn optimizer, not a region. It is for one type: allocate and
 * free the same fixed-size block over and over - list nodes, events, particles -
 * without paying malloc every time.
 *
 * It keeps a small stack of freed blocks (the "bin"). Freeing pushes a block
 * onto the bin instead of returning it to the base allocator; allocating pops
 * one back off. Both are O(1) and neither touches the heap. That recycling is
 * the entire point, and the check below proves it happens.
 *
 * Ownership: the pool caches freed blocks, but it does NOT track the blocks it
 * has handed out. Every block you take, you must give back before destroy - the
 * pool cannot free what it does not know about.
 */

typedef struct {
    int id;
    int score;
} node_t;

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();
    EXAMPLE_REQUIRE(proven_alloc_is_valid(heap), "hosted builds have a heap allocator");

    /* The pool takes a base allocator for the blocks it cannot serve from the
     * bin, plus the exact size and alignment of the one type it manages. The
     * last argument caps how many freed blocks are parked for reuse. */
    proven_pool_t pool = {0};
    proven_err_t err = proven_pool_init(&pool, heap, sizeof(node_t), alignof(node_t), 4);
    EXAMPLE_REQUIRE(proven_is_ok(err), "initializing a pool of node_t must succeed");
    if (!proven_is_ok(err)) {
        return 1;
    }

    proven_allocator_t nodes = proven_pool_as_allocator(&pool);

    /* --- first block: nothing in the bin, so it comes from the heap --------- */
    proven_result_mem_mut_t first = nodes.alloc_fn(nodes.ctx, sizeof(node_t), alignof(node_t));
    EXAMPLE_REQUIRE(proven_is_ok(first.err), "the pool must be able to serve its own item type");
    if (!proven_is_ok(first.err)) {
        proven_pool_destroy(&pool);
        return 1;
    }

    node_t *n = (node_t *)first.value.ptr;
    *n = (node_t){ .id = 1, .score = 100 };
    void *first_addr = n;

    /* --- hand it back: it lands in the bin, not back on the heap ------------ */
    nodes.free_fn(nodes.ctx, n);
    EXAMPLE_REQUIRE(pool.bin_len == 1, "a freed block is cached for reuse, not returned to the heap");
    /* `n` is dangling from here on. The pool owns those bytes again. */

    /* --- second block: the freed one is handed straight back ---------------- */
    proven_result_mem_mut_t second = nodes.alloc_fn(nodes.ctx, sizeof(node_t), alignof(node_t));
    EXAMPLE_REQUIRE(proven_is_ok(second.err), "allocating from a non-empty bin must succeed");
    EXAMPLE_REQUIRE(second.value.ptr == first_addr, "the recycled block is the one that was freed");
    EXAMPLE_REQUIRE(pool.bin_len == 0, "taking it back out empties the bin");

    /* Recycled memory is NOT zeroed for you - it is whatever the pool left there.
     * Initialize every field, exactly as you would for a fresh malloc. */
    node_t *m = (node_t *)second.value.ptr;
    *m = (node_t){ .id = 2, .score = 50 };
    EXAMPLE_REQUIRE(m->id == 2, "the recycled block is ours to overwrite");

    /* --- one pool serves one size and one alignment ------------------------- */
    /* A request for anything else is refused: this is not a general allocator, and it will
     * not silently hand you a block of the wrong size. The code is PROVEN_ERR_UNSUPPORTED -
     * "not my job" - and not INVALID_ARG, which would read as "you passed me garbage" and
     * send you hunting for a bug in your own code. */
    proven_result_mem_mut_t wrong = nodes.alloc_fn(nodes.ctx, sizeof(node_t) * 2, alignof(node_t));
    EXAMPLE_REQUIRE(wrong.err == PROVEN_ERR_UNSUPPORTED, "the pool only serves its configured item size");

    /* --- return every live block before destroying -------------------------- */
    /* proven_pool_destroy frees what is in the bin and the bin itself. `m` is
     * still handed out, so if we skipped this free it would leak. */
    nodes.free_fn(nodes.ctx, m);

    printf("pool: %zu block(s) cached for reuse at teardown\n", (size_t)pool.bin_len);

    proven_pool_destroy(&pool);
    return EXAMPLE_OK();
}
```
