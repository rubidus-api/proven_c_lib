# Chapter 2: Allocation, Arenas, Pools, and Buffers

이 장에서는 `allocator.h`, `heap.h`, `arena.h`, `pool.h`, `buffer.h`를 다룬다.

## 목차

1. [Allocator trait](#1-allocator-trait)
2. [Heap allocator](#2-heap-allocator)
3. [Arena allocator](#3-arena-allocator)
4. [Pool allocator](#4-pool-allocator)
5. [원시 바이트 버퍼](#5-raw-byte-buffer)
6. [예제와 오용 사례](#6-examples-and-misuse-cases)

## 1. Allocator trait

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

### `proven_alloc_is_valid`

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

## 2. Heap allocator

```text
[[nodiscard]] proven_allocator_t proven_heap_allocator(void);
```

목적: PAL 기반 heap allocator를 반환한다.

호스팅 동작: 플랫폼 메모리 계층을 통해 구현된 유효한 malloc 스타일 allocator를 반환한다.

프리스탠딩 동작: 제로 allocator 스텁을 반환한다. `proven_alloc_is_valid()`는 false를 반환한다.

예제:

```c
proven_allocator_t heap = proven_heap_allocator();
if (!proven_alloc_is_valid(heap)) {
    return;   /* freestanding build: there is no heap to allocate from */
}

proven_result_mem_mut_t r = heap.alloc_fn(heap.ctx, 256, PROVEN_DEFAULT_ALIGNMENT);
if (!proven_is_ok(r.err)) {
    return;
}

/* r.value.ptr is 256 writable bytes; free it through the same allocator. */
heap.free_fn(heap.ctx, r.value.ptr);
```

## 3. Arena allocator

arena는 호출자가 제공한 backing 저장소에서 선형으로 할당한다. 개별 해제는 no-op이다. reset은 모든 할당을 한 번에 버린다.

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

위험한 예:

```text
proven_result_array_t ar = PROVEN_ARRAY_INIT(a, int, 1);
for (int i = 0; i < 10000; ++i) {
    PROVEN_ARRAY_PUSH(&ar.value, int, i); /* every regrow abandons the old block */
}
```

## 4. Pool allocator

pool은 고정 크기 블록을 재활용한다. 많은 객체가 정확히 같은 크기와 정렬을 가질 때 유용하다.

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

## 5. 원시 바이트 버퍼

`proven_buf_t`는 고정 용량 바이트 버퍼다. 바이트 할당을 소유하지만 그 allocator를 저장하지는 않는다.

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
