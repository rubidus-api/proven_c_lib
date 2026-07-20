# Chapter 1: Foundation API

이 장에서는 `types.h`, `error.h`, `memory.h`, `align.h`, `version.h`, `panic.h`를 다룬다.

## 목차

1. [기본 타입](#1-fundamental-types)
2. [에러 모델](#2-error-model)
3. [검사된 산술 연산](#3-checked-arithmetic)
4. [메모리 view](#4-memory-views)
5. [정렬 헬퍼](#5-alignment-helpers)
6. [버전 매크로](#6-version-macros)
7. [Panic 훅](#7-panic-hook)
8. [예제와 오용 사례](#8-examples-and-misuse-cases)

## 1. 기본 타입

### 고정 폭 별칭

| 이름 | 의미 | 비고 |
|---|---|---|
| `proven_i8`, `proven_i16`, `proven_i32`, `proven_i64` | 부호 있는 고정 폭 정수 | 표준 정수 타입의 별칭. |
| `proven_u8`, `proven_u32`, `proven_u64` | 부호 없는 고정 폭 정수 | `proven_u8`은 수치값용이다. 원시 객체 바이트에는 `proven_byte_t`를 사용하라. |
| `proven_u16` | 16비트 코드 유닛 타입 | `<uchar.h>`가 있으면 `char16_t`를, 없으면 `uint_least16_t`를 사용한다. 현재 U16 문자열 API는 코드 유닛에 이 타입을 사용한다. |
| `proven_byte_t` | 바이트 단위 객체 표현 | `unsigned char`의 별칭. 원시 메모리 검사와 복사에 사용하라. |
| `proven_size_t` | 크기 및 인덱스 타입 | `size_t`의 별칭. |
| `proven_ptrdiff_t` | 포인터 차이 및 부호 있는 오프셋 타입 | `ptrdiff_t`의 별칭. |
| `proven_intptr_t`, `proven_uintptr_t` | 포인터 크기 정수 타입 | arena 범위 검사 같은 명시적 포인터-정수 작업에만 사용한다. |

### `proven_result_size_t`

```text
typedef struct {
    proven_err_t  err;
    proven_size_t value;
} proven_result_size_t;
```

목적: 크기 또는 에러 중 하나를 반환한다. 크기는 `err == PROVEN_OK`일 때만 유효하다.

주요 사용처: 부분 추가(append) 연산, 파일 읽기/쓰기, 파일 크기 조회.

## 2. 에러 모델

### `proven_err_t`

```text
typedef enum {
    PROVEN_OK = 0,
    PROVEN_ERR_NOMEM,
    PROVEN_ERR_OUT_OF_BOUNDS,
    PROVEN_ERR_INVALID_ENCODING,
    PROVEN_ERR_INVALID_ARG,
    PROVEN_ERR_IO,
    PROVEN_ERR_NOT_FOUND,
    PROVEN_ERR_INVALID_STATE,
    PROVEN_ERR_OVERFLOW,
    PROVEN_ERR_UNSUPPORTED,
    PROVEN_ERR_AGAIN,
    PROVEN_ERR_EOF,
    PROVEN_ERR_BUSY,
    PROVEN_ERR_PERMISSION,
    PROVEN_ERR_INVALID_FORMAT
} proven_err_t;
```

| 에러 | 일반적인 의미 |
|---|---|
| `PROVEN_OK` | 성공. |
| `PROVEN_ERR_NOMEM` | allocator가 메모리를 제공하지 못함. |
| `PROVEN_ERR_OUT_OF_BOUNDS` | 인덱스, 크기, 용량, 또는 범위가 유효하지 않음. |
| `PROVEN_ERR_INVALID_ENCODING` | 인코딩된 텍스트가 검증에 실패함. |
| `PROVEN_ERR_INVALID_ARG` | 널 포인터, 유효하지 않은 allocator, 불가능한 모드, 또는 잘못된 인자 개수. |
| `PROVEN_ERR_IO` | OS 또는 장치 I/O 실패. |
| `PROVEN_ERR_NOT_FOUND` | 파일, 키, 부분 문자열, 또는 리소스가 존재하지 않음. |
| `PROVEN_ERR_INVALID_STATE` | 객체 상태가 요청된 연산을 허용하지 않음. |
| `PROVEN_ERR_OVERFLOW` | 정수 변환 또는 크기 산술이 오버플로됨. |
| `PROVEN_ERR_UNSUPPORTED` | 이 플랫폼 또는 빌드 프로파일에서 기능을 사용할 수 없음. |
| `PROVEN_ERR_AGAIN` | 나중에 재시도하라. |
| `PROVEN_ERR_EOF` | 입력의 끝. |
| `PROVEN_ERR_BUSY` | 큐, 락, 또는 리소스가 사용 중. |
| `PROVEN_ERR_PERMISSION` | 접근이 거부됨. |
| `PROVEN_ERR_INVALID_FORMAT` | 포맷 또는 스캔 템플릿이 유효하지 않음. |

### 에러 헬퍼

```text
static inline int proven_is_ok(proven_err_t err);
#define PROVEN_IS_OK(err) proven_is_ok(err)
```

목적: 성공 검사를 명시적이고 읽기 쉽게 만든다.

반환: `err == PROVEN_OK`일 때 0이 아닌 값, 그 외에는 0.

올바른 예:

```c
proven_result_u8str_t s = proven_u8str_create(alloc, 32);
if (!proven_is_ok(s.err)) {
    return;   /* nothing was created, so there is nothing to destroy */
}
(void)proven_u8str_append(&s.value, PROVEN_LIT("ready"));
proven_u8str_destroy(alloc, &s.value);
```

잘못된 예:

```text
proven_result_u8str_t s = proven_u8str_create(alloc, 32);
if (s.err) {
    /* works today because PROVEN_OK is 0, but it hides the API convention,
       and it reads as "if there is an error" only to someone who already
       knows that. Say what you mean. */
    return s.err;
}
```

### 실전 예제: 값으로서의 에러

이 프로그램은 테스트 스위트에서 컴파일되고 실행되므로 시간이 지나도 낡을 수 없다.
이 예제는 실패 가능한 호출이 취하는 두 가지 형태를 보여준다 - 돌려줄 것이 없을 때의
민맨(bare) `proven_err_t`, 그리고 돌려줄 것이 있을 때의 `proven_result_*_t` - 그리고
result 안의 값은 그 옆의 에러를 확인하기 전까지는 아무 의미도 없는 이유를 보여준다.

<!-- example: manual/examples/ex_01_errors.c -->
```c
/*
 * Errors are values in proven: a fallible call hands back either an error or a
 * result, and the compiler makes you look at it. There is nothing to unwind and
 * nothing global to consult.
 */

/* A fallible operation returns proven_err_t when it has no value to give back. */
static proven_err_t write_greeting(proven_u8str_t *out) {
    return proven_u8str_append(out, PROVEN_LIT("hello"));
}

/* When there IS a value, it comes wrapped with the error that guards it. The
 * value is only meaningful once you have checked `err`. */
static proven_result_size_t half(proven_size_t n) {
    proven_result_size_t res = {0};
    if (n % 2 != 0) {
        res.err = PROVEN_ERR_INVALID_ARG;   /* leave res.value at 0: it means nothing */
        return res;
    }
    res.err = PROVEN_OK;
    res.value = n / 2;
    return res;
}

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    /* --- checking a plain proven_err_t ------------------------------------ */
    proven_result_u8str_t s = proven_u8str_create(alloc, 32);
    EXAMPLE_REQUIRE(proven_is_ok(s.err), "creating a 32-byte string should succeed");

    proven_err_t err = write_greeting(&s.value);
    if (!proven_is_ok(err)) {
        /* Nothing was appended, and the string is still valid: proven's
         * grow-style operations are failure-atomic. */
        proven_u8str_destroy(alloc, &s.value);
        return 1;
    }
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_as_view(&s.value), PROVEN_LIT("hello")),
                    "the greeting should have been appended");

    /* --- checking a result struct ----------------------------------------- */
    proven_result_size_t ok = half(10);
    EXAMPLE_REQUIRE(proven_is_ok(ok.err), "10 is even, so halving it must succeed");
    EXAMPLE_REQUIRE(ok.value == 5, "half of 10 is 5");

    proven_result_size_t bad = half(7);
    EXAMPLE_REQUIRE(bad.err == PROVEN_ERR_INVALID_ARG, "7 is odd, so halving it must fail");
    /* bad.value is NOT to be read. It is 0 here, but that is an implementation
     * detail of this function, not a promise of the result type. */

    /* --- the error is impossible to drop by accident ----------------------- */
    /* proven_u8str_append is [[nodiscard]], so this would be a compile error:
     *
     *     proven_u8str_append(&s.value, PROVEN_LIT("!"));
     *
     * If you really do want to ignore a failure, you have to say so: */
    (void)proven_u8str_append(&s.value, PROVEN_LIT("!"));

    printf("greeting: %s\n", proven_u8str_as_cstr(&s.value));

    proven_u8str_destroy(alloc, &s.value);
    return EXAMPLE_OK();
}
```

## 3. 검사된 산술 연산

### `PROVEN_SIZE_MAX`

`proven_size_t`의 최댓값.

### `PROVEN_CKD_ADD`, `PROVEN_CKD_SUB`, `PROVEN_CKD_MUL`

```text
#define PROVEN_CKD_ADD(res, a, b) ...
#define PROVEN_CKD_SUB(res, a, b) ...
#define PROVEN_CKD_MUL(res, a, b) ...
```

목적: 크기와 오프셋 계산을 위한 검사된 정수 산술.

매개변수:

- `res`: 출력 객체에 대한 포인터.
- `a`, `b`: 피연산자.

반환: 오버플로 시 true, 성공 시 false.

올바른 예:

```c
typedef struct { int id; double weight; } item_t;

proven_size_t count = 1024;
proven_size_t total = 0;
proven_err_t err = PROVEN_OK;

if (PROVEN_CKD_MUL(&total, count, sizeof(item_t))) {
    err = PROVEN_ERR_OVERFLOW;   /* refuse to allocate a size that wrapped */
} else {
    proven_result_mem_mut_t block = alloc.alloc_fn(alloc.ctx, total, alignof(item_t));
    err = block.err;
    if (proven_is_ok(err)) {
        alloc.free_fn(alloc.ctx, block.value.ptr);
    }
}
(void)err;
```

잘못된 예:

```text
proven_size_t total = count * sizeof(Item); /* wrong: may wrap */
```

## 4. 메모리 view

### `proven_mem_t`

```text
typedef struct {
    proven_byte_t *ptr;
    proven_size_t size;
} proven_mem_t;
```

목적: owned 메모리 블록을 기술한다. 소유권은 타입에 의해 강제되지 않으므로, 호출자는 여전히 올바른 allocator로 해제해야 한다.

### `proven_mem_view_t`

```text
typedef struct {
    const proven_byte_t *ptr;
    proven_size_t size;
} proven_mem_view_t;
```

목적: borrowed 읽기 전용 바이트 범위. 메모리를 소유하지 않으며 계약상 NUL로 종료되지 않는다.

### `proven_mem_mut_t`

```text
typedef struct {
    proven_byte_t *ptr;
    proven_size_t size;
} proven_mem_mut_t;
```

목적: borrowed 가변 바이트 범위.

### `proven_result_mem_mut_t`

```text
typedef struct {
    proven_err_t err;
    proven_mem_mut_t value;
} proven_result_mem_mut_t;
```

목적: allocator 및 슬라이스를 생성하는 result 타입.

### `proven_result_mem_view_t`

```text
typedef struct {
    proven_err_t err;
    proven_mem_view_t value;
} proven_result_mem_view_t;
```

목적: 검사된 슬라이싱 result 타입.

### 메모리 함수와 인라인 헬퍼

| API | 목적 | 매개변수 | 반환 |
|---|---|---|---|
| `proven_mem_view_from_owned(mem)` | owned 블록으로부터 읽기 전용 view를 생성한다. | `mem`: 메모리 블록. | `proven_mem_view_t`. |
| `proven_mem_mut_from_owned(mem)` | owned 블록으로부터 가변 view를 생성한다. | `mem`: 메모리 블록. | `proven_mem_mut_t`. |
| `proven_mem_view_slice_unchecked(view, offset, size)` | 검증 없이 서브뷰를 만든다. | `view`, `offset`, `size`. | `proven_mem_view_t`. |
| `proven_mem_view_slice_checked(view, offset, size)` | 검사된 서브뷰를 만든다. | `view`, `offset`, `size`. | `proven_result_mem_view_t`. |
| `proven_mem_mut_slice_unchecked(mut, offset, size)` | 검증 없이 가변 서브슬라이스를 만든다. | `mut`, `offset`, `size`. | `proven_mem_mut_t`. |
| `proven_mem_mut_slice_checked(mut, offset, size)` | 검사된 가변 서브슬라이스를 만든다. | `mut`, `offset`, `size`. | `proven_result_mem_mut_t`. |
| `proven_range_contains_ptr(base, cap, ptr, size, out_offset)` | 정수 주소 검사를 사용해 포인터 범위가 기저 할당 안에 있는지 확인한다. | `base`, `cap`, `ptr`, `size`, 선택적 `out_offset`. | `_Bool`. |
| `proven_memcmp(s1, s2, size)` | 원시 메모리 영역을 비교한다. | 두 개의 포인터와 바이트 크기. | 같으면 0, 바이트 순서에 따라 음수 또는 양수. |
| `proven_mem_copy(dst, dst_cap, src)` | 바이트 view를 `dst`로 경계 검사하며 복사한다. | `dst`, 용량, `src` view. | `PROVEN_OK`, 오버플로가 발생할 경우 `PROVEN_ERR_OUT_OF_BOUNDS`(아무것도 기록되지 않음), 또는 크기가 0이 아닌데 널 포인터인 경우 `PROVEN_ERR_INVALID_ARG`. 겹치지 않음. |
| `proven_mem_move(dst, dst_cap, src)` | `proven_mem_copy`와 같지만 소스와 대상이 겹칠 수 있다. | copy와 동일 | copy와 동일. |

검사된 슬라이스 동작:

- `view.size > 0 && view.ptr == NULL`이면 `PROVEN_ERR_INVALID_ARG`를 반환한다.
- 범위가 view 밖에 있으면 `PROVEN_ERR_OUT_OF_BOUNDS`를 반환한다.
- 요청된 `size == 0`이면 널 포인터를 가진 빈 view와 `PROVEN_OK`를 반환한다.

## 5. 정렬 헬퍼

### 매크로

| 매크로 | 의미 |
|---|---|
| `PROVEN_DEFAULT_ALIGNMENT` | 기본 정렬, 현재 8. |
| `PROVEN_MAX_ALIGN` | `alignof(max_align_t)`. |

### 함수

| API | 목적 | 반환 |
|---|---|---|
| `proven_is_pow2(x)` | `x`가 0이 아닌 2의 거듭제곱인지 검사한다. | `bool`. |
| `proven_mem_align_up(addr, align)` | 크기/주소를 위쪽으로 정렬한다. | 정렬된 값, 또는 `align`이 유효하지 않거나 오버플로가 발생하면 0. |
| `proven_uintptr_align_up(addr, align)` | `proven_uintptr_t` 주소를 위쪽으로 정렬한다. | 정렬된 값, 또는 정렬이 유효하지 않거나 오버플로 시 0. |

올바른 예:

```c
typedef struct { int id; double weight; } item_t;

proven_size_t size = 100;
proven_size_t aligned = proven_mem_align_up(size, alignof(item_t));
proven_err_t err = (aligned == 0) ? PROVEN_ERR_OVERFLOW : PROVEN_OK;
(void)err;
```

잘못된 예:

```text
proven_size_t aligned = (size + align - 1) & ~(align - 1); /* wrong: may overflow */
```

## 6. 버전 매크로

```text
#define PROVEN_VERSION_STRING "proven_c_lib-v26.07.20a"
#define PROVEN_VERSION_NUM    260713
#define PROVEN_VERSION_SUFFIX "m"
```

목적: 컴파일 타임 버전 식별.

진단과 빌드 보고서에는 `PROVEN_VERSION_STRING`을 사용하라. `PROVEN_VERSION_NUM`은 수치 비교에만 사용하라.

## 7. Panic 훅

```text
typedef void (*proven_panic_handler_t)(const char *msg);

void proven_panic(const char *msg);
void proven_set_panic_handler(proven_panic_handler_t handler);
```

목적: `proven_arena_alloc_or_panic()` 같은 panic 스타일 API가 사용하는 종단 실패 경로를 처리한다. 라이브러리는 `proven_panic()`을 호출해 panic을 일으키며, 이는 설치된 핸들러로 디스패치한다.

기본 동작: 내장 기본 핸들러가 trap한다(GCC와 Clang에서는 `__builtin_trap()`; 그 외 컴파일러에서는 대신 무한 루프로 스핀한다). 핸들러는 weak symbol이 아니라 함수 포인터를 통해 디스패치되므로, ELF와 PE/COFF(Windows / mingw-w64) 툴체인에서 균일하게 링크된다.

사용자 재정의(핸들러는 파일 스코프 함수이므로, 이는 다른 함수 안에 붙여넣을 수 있는 블록이 아니라 리스팅이다):

```text
static void my_panic(const char *msg) {
    log_critical(msg);            /* your logger */
    for (;;) {
        /* reset, halt, or wait for debugger */
    }
}

proven_set_panic_handler(my_panic);   /* pass NULL to restore the default */
```

계약: 프로덕션 panic 핸들러는 반환하지 않아야 한다. panic 핸들러가 반환하면 `_or_panic` result의 유효성은 보장되지 않는다.

## 8. 예제와 오용 사례

### 안전한 result 처리

```c
proven_byte_t record[16] = {0};
proven_mem_view_t view = { .ptr = record, .size = sizeof record };

proven_result_mem_view_t part = proven_mem_view_slice_checked(view, 4, 8);
if (!proven_is_ok(part.err)) {
    return;   /* the range was not inside the view */
}

proven_byte_t payload[8];
proven_err_t err = proven_mem_copy(payload, sizeof payload, part.value);
(void)err;
```

### 증명 후에만 검사 없는 슬라이싱

호출자가 이미 범위를 증명한 경우 올바른 예:

```c
proven_byte_t record[16] = {0};
proven_mem_view_t view = { .ptr = record, .size = sizeof record };
proven_size_t offset = 4;
proven_size_t size = 8;

if (offset <= view.size && size <= view.size - offset) {
    proven_mem_view_t part = proven_mem_view_slice_unchecked(view, offset, size);
    (void)part;   /* proved in range: safe to read */
}
```

잘못된 예:

```text
proven_mem_view_t part = proven_mem_view_slice_unchecked(view, user_offset, user_size);
/* wrong: user_offset and user_size were not validated */
```

### 빈 view는 허용된다

크기가 0인 view는 널 포인터를 가질 수 있다. 그것을 무턱대고 거부하지 마라.

잘못된 예:

```text
if (!view.ptr) return PROVEN_ERR_INVALID_ARG; /* wrong for empty views */
```

더 나은 예:

```c
proven_mem_view_t view = {0};   /* size 0, ptr NULL: a legal empty view */
proven_err_t err = PROVEN_OK;

if (view.size > 0 && !view.ptr) {
    err = PROVEN_ERR_INVALID_ARG;   /* only a null pointer with bytes behind it is a bug */
}
(void)err;
```
