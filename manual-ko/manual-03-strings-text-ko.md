# 3장: 문자열, 포매팅, 스캐닝

이 장은 `u8str.h`, `u16str.h`, `fmt.h`, `scan.h`를 다룬다.
전체 포맷 문법과 스캐너의 경계 사례는 `manual-08-fmt-scan-ko.md`를 참고하라.

## 목차

1. [U8 문자열과 view](#1-u8-strings-and-views)
2. [U16 문자열과 view](#2-u16-strings-and-views)
3. [포매팅](#3-formatting)
4. [스캐닝](#4-scanning)
5. [예제와 오용 사례](#5-examples-and-misuse-cases)

## 1. U8 문자열과 view

U8 문자열은 owned이며 NUL로 종료되는 바이트 문자열이다. 길이는 Unicode 스칼라 값이 아니라 바이트 단위로 센다. U8 view는 borrowed이며 NUL 종료가 보장되지 않는다.

### 구조체

```text
typedef struct {
    const proven_byte_t *ptr;
    proven_size_t size;
} proven_u8str_view_t;

typedef struct {
    proven_byte_t *ptr;
    proven_size_t size;
} proven_u8str_mut_t;

typedef struct {
    proven_buf_t internal;
    bool         borrowed;
} proven_u8str_t;

typedef struct {
    proven_err_t err;
    proven_u8str_t value;
} proven_result_u8str_t;

typedef struct {
    proven_err_t err;
    const char *value;
} proven_result_cstr_t;
```

의도:

- `proven_u8str_view_t`: borrowed 읽기 전용 바이트 문자열 view.
- `proven_u8str_mut_t`: borrowed 가변 바이트 문자열 view.
- `proven_u8str_t`: NUL 종료를 갖는 owned 문자열.
- `proven_result_u8str_t`: owned 문자열용 result 래퍼.
- `proven_result_cstr_t`: 할당된 NUL 종료 C 문자열용 result 래퍼.

### 내부 레이아웃

view는 겉보기 그대로다 — 포인터 하나와 바이트 개수 하나다:

```text
typedef struct { const proven_byte_t *ptr; proven_size_t size; } proven_u8str_view_t;
typedef struct { proven_byte_t *ptr;       proven_size_t size; } proven_u8str_mut_t;
```

owned 문자열은 (2장의 고정 용량 바이트 버퍼인) `proven_buf_t`에
플래그 하나를 더해 감싼다:

```text
typedef struct {
    proven_buf_t internal;   /* the bytes + length + capacity; always keeps room for a NUL */
    bool         borrowed;   /* false = allocator-owned (default); true = wraps caller memory */
} proven_u8str_t;
```

- `internal.len`은 바이트 길이다(`proven_buf_t`는 `ptr` / `len` / `cap`이며
  — `size` 멤버는 없다). 문자열이 유효한 동안 `internal.len` 위치의 바이트는 항상 NUL
  종료자이므로 `proven_u8str_as_cstr()`은 O(1)이다.
- `borrowed`는 0으로 초기화된 핸들에서 `false`이므로, allocator가 소유하는
  문자열이 안전한 기본값이다. 이 값은 `proven_u8str_borrow`가 여러분이 소유한
  `[buf, buf+cap)`를 감쌀 때에만 `true`로 설정되며, 그럴 경우 크기를 늘리는 연산은
  재할당을 거부하고(`PROVEN_ERR_OUT_OF_BOUNDS`) `proven_u8str_destroy`는 no-op이 된다.
- 문자열을 바꾸기 위해 이 필드들을 직접 읽거나 쓰지 **말라**. 아래 함수들을
  사용하라. 길이를 얻으려고 `internal.len`을 읽는 것은 괜찮지만,
  `proven_u8str_as_view()`를 선호하라.

반례 — borrowed 문자열을 owned 문자열처럼 다루기:

```c
proven_byte_t stack[8];
proven_u8str_t s = proven_u8str_borrow(stack, sizeof stack);   /* borrowed */

/* This would have to reallocate caller memory, so it refuses: the call returns
   PROVEN_ERR_OUT_OF_BOUNDS and `stack` is left exactly as it was. A borrowed
   string never silently escapes to the heap. */
proven_err_t e = proven_u8str_append_grow(alloc, &s, PROVEN_LIT("far too long for eight bytes"));
(void)e;   /* == PROVEN_ERR_OUT_OF_BOUNDS */

/* destroy is a no-op here: `stack` is yours, and the library will not free it.
   Writing it anyway is correct, and it keeps teardown code uniform. */
proven_u8str_destroy(alloc, &s);
```

### 매크로

| 매크로 | 의도 | 비고 |
|---|---|---|
| `PROVEN_LIT(s)` | 문자열 리터럴로부터 `proven_u8str_view_t`를 만든다. | 문자열 리터럴에만 사용하라. |
| `PROVEN_LIT_INIT(s)` | 리터럴 view의 초기화자 형태. | 집합체 초기화에서 사용하라. |
| `PROVEN_INDEX_NOT_FOUND` | find 함수들이 반환하는 센티넬. | `(proven_size_t)-1`과 같다. |

### U8 함수

| API | 의도 | 반환 |
|---|---|---|
| `proven_u8str_create(alloc, limit)` | `limit` 바이트에 NUL을 더한 용량을 가진 빈 owned 문자열을 만든다. | `proven_result_u8str_t`. |
| `proven_u8str_create_from_view(alloc, view)` | view를 새 owned 문자열로 복사한다. | `proven_result_u8str_t`. |
| `proven_u8str_borrow(buf, cap)` | 호출자 소유의 `[buf, buf+cap)`를 고정 용량 문자열로 감싼다(할당 없음). 고정 용량 연산과 `append_fmt`는 동작하고, 크기를 늘리는 연산은 호출자 메모리의 재할당을 거부하며, destroy는 no-op이다. `cap`은 NUL을 포함한다. | `proven_u8str_t`. |
| `proven_u8str_reset(str)` | 버퍼/용량을 재사용을 위해 유지한 채 빈 상태로 잘라낸다(owned든 borrowed든). | `proven_err_t`. |
| `proven_u8str_is_valid(str)` | 공개 문자열 불변식을 검증한다. | `bool`. |
| `proven_u8str_reserve(alloc, str, new_cap)` | 내부 용량이 최소 `new_cap` 바이트가 되도록 보장한다. (borrowed 문자열은 커질 수 없다.) | `proven_err_t`. |
| `proven_u8str_append(str, data)` | 원자적 고정 용량 추가. | `PROVEN_OK` 또는 `PROVEN_ERR_OUT_OF_BOUNDS`. |
| `proven_u8str_append_partial(str, data)` | 잘라내며 추가. | `proven_result_size_t`; `value`는 쓴 바이트 수. |
| `proven_u8str_append_grow(alloc, str, data)` | 원자적 확장 추가. | `proven_err_t`. |
| `proven_u8str_append_byte(alloc, str, b)` | 필요하면 커지며 한 바이트를 추가한다. | `proven_err_t`. |
| `proven_u8str_replace_at(str, index, old_len, data)` | 정확한 바이트 범위를 고정 용량 의미로 교체한다. | `proven_err_t`. |
| `proven_u8str_replace_at_grow(alloc, str, index, old_len, data)` | `replace_at`과 같지만, 편집이 들어가지 않으면 실패하는 대신 버퍼를 (배로) 키운다. | `proven_err_t`; 할당 실패 시 문자열은 그대로. |
| `proven_u8str_insert(str, index, data)` | `index`에 고정 용량 의미로 바이트를 삽입한다. | `proven_err_t`. |
| `proven_u8str_insert_grow(alloc, str, index, data)` | `insert`와 같지만 필요하면 버퍼를 키운다. 수동 `reserve`가 필요 없다. | `proven_err_t`; 할당 실패 시 문자열은 그대로. |
| `proven_u8str_remove(str, index, len)` | 바이트 범위를 제거한다. | `proven_err_t`. |
| `proven_u8str_replace_first(str, start_offset, target, replacement)` | start 이후 처음 일치하는 target을 교체한다. | 못 찾아도 `PROVEN_OK`. |
| `proven_u8str_view_find(haystack, start_offset, needle)` | 바이트 부분문자열의 첫 등장을 찾는다(임의의 바이트 값을 올바르게 처리하며, NUL 종료가 아니다). | index 또는 `PROVEN_INDEX_NOT_FOUND`. |
| `proven_u8str_view_starts_with(str, prefix)` | 접두사 검사. | int 진리값. |
| `proven_u8str_view_ends_with(str, suffix)` | 접미사 검사. | int 진리값. |
| `proven_u8str_view_slice(str, index, len)` | 잘라낸(clamped) 부분 view를 반환한다. | `proven_u8str_view_t`. |
| `proven_u8str_as_cstr(str)` | 내부 NUL 종료 포인터를 반환한다. | `const char *`; 성장/파괴로 무효화됨. |
| `proven_mem_view_from_u8(view)` | U8 view를 바이트 view로 변환한다. | `proven_mem_view_t`. |
| `proven_u8str_view_to_cstr(view, alloc)` | 임의의 view로부터 NUL 종료 C 문자열을 할당한다. | `proven_result_cstr_t`; 호출자가 allocator로 해제한다. |
| `proven_cstr_len(s)` | NUL까지의 바이트를 센다. | `proven_size_t`. |
| `proven_u8str_view_from_cstr(s)` | 신뢰할 수 있는 NUL 종료 문자열로부터 view를 만든다. | null 포인터면 빈 view. |
| `proven_u8str_view_eq(a, b)` | 바이트 동등성 검사. | int 진리값. |
| `proven_u8str_destroy(alloc, str)` | 일치하는 allocator로 owned 저장소를 해제한다. | void. |
| `proven_u8str_as_view(str)` | 현재 내용을 borrow한다. | `proven_u8str_view_t`. |

### 기본 U8 예제

```c
proven_result_u8str_t r = proven_u8str_create_from_view(alloc, PROVEN_LIT("log"));
if (!proven_is_ok(r.err)) {
    return;
}
proven_u8str_t s = r.value;

proven_err_t e = proven_u8str_append_grow(alloc, &s, PROVEN_LIT(": ready"));
if (!proven_is_ok(e)) {
    proven_u8str_destroy(alloc, &s);
    return;
}

/* Valid until the next growing call: as_cstr points into the string's storage. */
const char *cstr = proven_u8str_as_cstr(&s);
(void)cstr;

proven_u8str_destroy(alloc, &s);
```

## 2. U16 문자열과 view

U16 API는 `PROVEN_NO_U16STR`이 정의되면 제외된다. U16 크기는 `proven_u16` 코드 단위로 센다. 내부적으로 `proven_u16str_t`는 바이트를 추적하는 `proven_buf_t`를 사용한다.

### 구조체

```text
typedef struct {
    const proven_u16 *ptr;
    proven_size_t size;
} proven_u16str_view_t;

typedef struct {
    proven_buf_t internal;
} proven_u16str_t;

typedef struct {
    proven_err_t err;
    proven_u16str_t value;
} proven_result_u16str_t;
```

### 매크로

| 매크로 | 의도 |
|---|---|
| `PROVEN_U16_LIT(s)` | UTF-16 리터럴 표현식 `u"..."`로부터 `proven_u16str_view_t`를 만든다. |

### U16 함수

| API | 의도 | 반환 |
|---|---|---|
| `proven_u16str_create(alloc, unit_limit)` | 빈 owned U16 문자열을 만든다. | `proven_result_u16str_t`. |
| `proven_u16str_create_from_view(alloc, view)` | U16 view를 owned 문자열로 복사한다. | `proven_result_u16str_t`. |
| `proven_u16str_destroy(alloc, str)` | owned U16 저장소를 해제한다. | void. |
| `proven_u16str_append(str, data)` | 원자적 고정 용량 추가. | `proven_err_t`. |
| `proven_u16str_append_partial(str, data)` | 잘라내며 추가. | `proven_result_size_t`. |
| `proven_u16str_append_grow(alloc, str, data)` | 원자적 확장 추가. | `proven_err_t`. |
| `proven_u16str_as_ptr(str)` | 내부 `proven_u16` 포인터를 반환한다. | `const proven_u16 *`. |
| `proven_u16str_len(str)` | 코드 단위 길이를 반환한다. | `proven_size_t`. |

예제:

```c
#ifndef PROVEN_NO_U16STR
proven_result_u16str_t r =
    proven_u16str_create_from_view(alloc, PROVEN_U16_LIT("hello"));
if (!proven_is_ok(r.err)) {
    return;
}
proven_u16str_t s = r.value;

(void)proven_u16str_append_grow(alloc, &s, PROVEN_U16_LIT(" world"));

/* Length is in code units, not characters: "hello world" is 11 units. */
proven_size_t units = proven_u16str_len(&s);
(void)units;

proven_u16str_destroy(alloc, &s);
#endif
```

참고: `proven_u16`은 코드 단위이며, 반드시 하나의 완전한 Unicode 문자인 것은 아니다. UTF-16 서로게이트 쌍은 코드 단위 두 개를 사용한다.

## 3. 포매팅

포매터는 `proven_u8str_t` 또는 PAL 기반 스트림에 쓴다. `{}` 자리표시자, `{1}` 같은 명시적 인덱스, 이스케이프된 중괄호 `{{`와 `}}`, 그리고 `{:0>5}`, `{:*^10}`, `{:.<10}` 같은 너비/정렬 지정을 갖춘 작은 구조적 포맷 언어를 사용한다.

### 구조체와 열거형

```text
typedef struct {
    proven_err_t err;
    proven_size_t written;
    proven_size_t required;
} proven_fmt_result_t;
```

필드:

- `err`: 결과 코드.
- `written`: 실제로 쓴 바이트 수.
- `required`: 전체 출력에 필요한 바이트 수.

`proven_arg_type_t` 변형:

- `PROVEN_ARG_NONE`
- `PROVEN_ARG_I32`
- `PROVEN_ARG_U32`
- `PROVEN_ARG_I64`
- `PROVEN_ARG_U64`
- `PROVEN_ARG_F64` — `PROVEN_FMT_NO_FLOAT`이 정의되지 않은 경우
- `PROVEN_ARG_CSTR`
- `PROVEN_ARG_STR_VIEW`
- `PROVEN_ARG_DATETIME`
- `PROVEN_ARG_PTR`
- `PROVEN_ARG_FN`

```text
typedef struct {
    proven_arg_type_t type;
    union {
        proven_i32 i32;
        proven_u32 u32;
        proven_i64 i64;
        proven_u64 u64;
        double f64;
        const char *cstr;
        proven_u8str_view_t str_view;
        proven_datetime_t datetime;
        const void *ptr;
        void (*fn)(void);
    } value;
} proven_arg_t;
```

### 포맷 인자 생성자

| API | 의도 |
|---|---|
| `proven_arg_none()` | 내부 센티넬 값. |
| `proven_arg_i32(v)`, `proven_arg_u32(v)` | 정수 인자. |
| `proven_arg_i64(v)`, `proven_arg_u64(v)` | 넓은 정수 인자. |
| `proven_arg_f64(v)` | 부동소수점 인자(float 포매팅이 비활성화되지 않은 경우). |
| `proven_arg_cstr(v)` | 신뢰할 수 있는 살아 있는 NUL 종료 C 문자열. |
| `proven_arg_cstr_n(v, max_len)` | 경계가 있는 C 문자열 인자; NUL을 `max_len`까지만 스캔한다. |
| `proven_arg_str_view(v)` | borrowed 문자열 view 인자. |
| `proven_arg_datetime(v)` | datetime 인자. |
| `proven_arg_ptr(v)` | 객체 포인터 인자. |
| `proven_arg_fn(v)` | 함수 포인터 인자. |
| `proven_arg_ucstr(v)` | unsigned char C 문자열 헬퍼. |
| `proven_arg_identity(v)` | 기존 `proven_arg_t`에 대한 통과(pass-through). |

### 포맷 매크로

| 매크로 | 의도 |
|---|---|
| `PROVEN_ARG(x)` | 지원되는 인자 타입에 대한 `_Generic` 선택자. |
| `PROVEN_ARG_FN(f)` | 함수 포인터 포매팅 헬퍼. |
| `PROVEN_ARG_CSTR_N(v, max_len)` | 경계가 있는 C 문자열 헬퍼. |
| `proven_u8str_append_fmt(str, fmt, ...)` | 원자적 고정 용량 포매팅. |
| `proven_u8str_append_fmt_trunc(str, fmt, ...)` | 최선 노력(best-effort) 잘라내기 포매팅. |
| `proven_u8str_append_fmt_grow(alloc, str, fmt, ...)` | 원자적 확장 포매팅. |
| `proven_u8str_append_fmt_with_scratch(alloc, str, fmt, scratch, ...)` | 임시 패치 할당을 위한 scratch allocator를 사용하는 확장 포매팅. |
| `PROVEN_FMT_IS_OK(res)` | `proven_fmt_result_t`를 검사한다. |

### 포매팅 엔진

```text
proven_fmt_result_t proven_u8str_fmt_internal(
    proven_allocator_t alloc,
    proven_u8str_t *str,
    bool trunc,
    const char *fmt,
    proven_allocator_t scratch,
    const proven_arg_t *args,
    proven_size_t args_count
);
```

일반적인 사용자 코드는 이 내부 엔진 대신 매크로를 호출해야 한다. 엔진은 인덱스 0에 선두 `proven_arg_none()` 센티넬을 기대한다.

사용되지 않는 추가 포맷 인자는 `PROVEN_ERR_INVALID_ARG`를 반환한다.

예제:

```c
proven_result_u8str_t r = proven_u8str_create(alloc, 8);
if (!proven_is_ok(r.err)) {
    return;
}
proven_u8str_t s = r.value;

/* The target is only 8 bytes; the _grow form reallocates rather than truncate. */
proven_fmt_result_t fr = proven_u8str_append_fmt_grow(
    alloc,
    &s,
    "name={} score={:0>4}",
    PROVEN_ARG(PROVEN_LIT("ada")),
    PROVEN_ARG(42)
);
if (!PROVEN_FMT_IS_OK(fr)) {
    proven_u8str_destroy(alloc, &s);
    return;
}
/* s == "name=ada score=0042" */

proven_u8str_destroy(alloc, &s);
```

## 4. 스캐닝

스캐너는 borrowed `proven_u8str_view_t`로부터 파싱한다. 커서가 진행 상황을 추적한다.

### Result 구조체

```text
typedef struct { proven_err_t err; proven_i64 val; } proven_result_i64_t;
typedef struct { proven_err_t err; proven_u64 val; } proven_result_u64_t;
typedef struct { proven_err_t err; double val; } proven_result_f64_t;
typedef struct { proven_err_t err; proven_u8str_view_t val; } proven_result_u8str_view_t;
```

### `proven_scan_t`

```text
typedef struct {
    proven_u8str_view_t view;
    proven_size_t cursor;
} proven_scan_t;
```

목적: 입력과 현재 파싱 위치를 담는다. `proven_scan_init()`은 유효하지 않은 비어 있지 않은 null view를 빈 view로 정규화한다.

### 스캐너 함수

| API | 의도 | 반환 |
|---|---|---|
| `proven_scan_init(view)` | view로부터 스캐너를 만든다. | `proven_scan_t`. |
| `proven_scan_skip_whitespace(scan)` | 공백을 지나 전진한다. | void. |
| `proven_scan_i64(scan)` | 부호 있는 64비트 정수를 파싱한다. | `proven_result_i64_t`. |
| `proven_scan_u64(scan)` | 부호 없는 64비트 정수를 파싱한다. | `proven_result_u64_t`. |
| `proven_scan_f64(scan)` | 부동소수점 값을 파싱한다. | `proven_result_f64_t`. |
| `proven_parse_double_ascii(view)` | 공백을 건너뛰지 않고 로케일 독립적인 ASCII float 토큰 하나를 파싱한다. | `proven_parse_double_result_t`. |
| `proven_parse_f64_ascii(view)` | 동일한 로케일 독립적 binary64 파서에 대한 호환 별칭. | `proven_parse_f64_result_t`. |
| `proven_strtod(nptr, endptr)` | 공백 건너뛰기와 `endptr` 보고를 갖춘 `strtod` 스타일 토큰 하나를 파싱한다. | `double`. |
| `proven_scan_str(scan)` | 공백으로 구분된 토큰을 입력에 대한 view로 파싱한다. | `proven_result_u8str_view_t`. |
| `proven_scan_skip_until(scan, target)` | target이 있으면 커서를 그리로 옮긴다. | `proven_err_t`. |
| `proven_scan_skip_until_number(scan)` | 다음 숫자로 보이는 위치로 커서를 옮긴다. | void. |

### 스캔 인자 타입

`proven_scan_arg_type_t`는 `proven_scan_arg_t`에 저장된 목적지 종류를 식별한다. `PROVEN_SCAN_ARG(&x)`는 다음에 대한 포인터를 지원한다:

- `short`, `unsigned short`
- `int`, `unsigned int`
- `long`, `unsigned long`
- `long long`, `unsigned long long`
- `double`
- `proven_u8str_view_t`

네이티브 목적지 생성자:

- `proven_scan_arg_short`, `proven_scan_arg_ushort`
- `proven_scan_arg_int`, `proven_scan_arg_uint`
- `proven_scan_arg_long`, `proven_scan_arg_ulong`
- `proven_scan_arg_llong`, `proven_scan_arg_ullong`

명시적 고정 너비 및 유틸리티 헬퍼:

- `proven_scan_arg_i32`, `proven_scan_arg_u32`
- `proven_scan_arg_i64`, `proven_scan_arg_u64`
- `proven_scan_arg_f64`
- `proven_scan_arg_str_view`
- `proven_scan_arg_none`
- `proven_scan_arg_identity`

Long 별칭:

```text
#define PROVEN_SCAN_ARG_LONG(ptr)  proven_scan_arg_long(ptr)
#define PROVEN_SCAN_ARG_ULONG(ptr) proven_scan_arg_ulong(ptr)
```

### 포맷 스캐닝 매크로

| 매크로 | 의도 |
|---|---|
| `PROVEN_SCAN_ARG(x)` | `_Generic` 목적지 선택자. |
| `proven_scan_fmt_cursor(scan_ptr, fmt, ...)` | 기존 커서로부터 스캔한다. |
| `proven_scan_fmt(view, fmt, ...)` | 임시 커서로 view로부터 스캔한다. |

### 스캔 엔진

```text
proven_err_t proven_scan_fmt_internal(
    proven_scan_t *scan,
    const char *fmt,
    const proven_scan_arg_t *args,
    proven_size_t args_count
);
```

| API | 의도 | 반환 |
|---|---|---|
| `proven_scan_fmt_internal(scan, fmt, args, args_count)` | 기존 커서 위에서 동작하는 구조적 스캐너 엔진. | `proven_err_t`. |
| `proven_scan_fmt_internal_view(view, fmt, args, count)` | view 위에 임시 스캐너를 생성하는 편의 래퍼. | `proven_err_t`. |

일반적인 사용자 코드는 매크로를 호출해야 한다. 엔진은 선두 센티넬 인자를 기대한다.

중요한 동작: `proven_scan_fmt_internal()`은 나중 리터럴 불일치가 발생하면 오류를 반환하기 전에 커서를 전진시키고 앞선 목적지들에 쓸 수 있다. 트랜잭션 같은 파싱이 필요하면 커서와 목적지 값을 먼저 저장하라.

예제:

```c
proven_scan_t scan = proven_scan_init(PROVEN_LIT("ID: 402 SCORE: 99.5 ada"));
int id = 0;
double score = 0.0;
proven_u8str_view_t user = {0};

proven_err_t e = proven_scan_fmt_cursor(
    &scan,
    "ID: {} SCORE: {} {}",
    PROVEN_SCAN_ARG(&id),
    PROVEN_SCAN_ARG(&score),
    PROVEN_SCAN_ARG(&user)
);
if (!proven_is_ok(e)) {
    return;
}
/* id == 402, score == 99.5, user borrows "ada" out of the input - it is not a
 * copy, so it is only valid while the scanned bytes are. */
```

### Float 파싱 관련 참고

- `proven_scan_f64()`와 `proven_parse_double_ascii()`는 공유되는
  decimal-to-binary64 백엔드를 거친다.
- 유한 십진 입력은 round-to-nearest, ties-to-even 동작으로 IEEE-754 binary64로
  반올림된다.
- 현재 변환 스택은 `Clinger fast path -> staged
  Eisel-Lemire layer -> exact bigint fallback`이며, 대표 입력이 어느 경로를
  탔는지 테스트가 확인하는 데 쓰는 내부 카운터가 있다.
- staged Eisel-Lemire layer는 현재 생성된 `5^q` 양의 지수 사례와, 십진
  유효숫자가 필요한 `5^q`를 깔끔하게 상쇄하는 정확한 음의 지수 사례를 받아들인다.
- `__uint128_t`가 있는 컴파일러에서는, 같은 layer가 normal-range 사례에서
  보수적으로 반올림된 음의 지수 비율의 부분집합도 받아들이며, 여기에는 원래의
  좁은 프로토타입이 허용했던 것보다 넓은 좌향 시프트 정규화가 포함된다.
- 넓어진 캐시 거듭제곱 곱 경로는 이제 `5e-324`를 포함한 일부 subnormal 사례도
  거치며, true-min에 대한 절반 임계값 아래의 값들은 여전히 exact bigint
  fallback으로 미룬다.
- 체크인된 캐시 `5^q` 상수는 `scripts/generate_float_decimal_tables.py`로 로컬에서
  생성한다.
- `proven_parse_double_ascii()`는 선행 공백을 건너뛰지 않는다.
- `proven_strtod()`는 선행 ASCII 공백을 건너뛰고, `endptr`을 갱신하며, 오버플로 시
  부호 있는 무한대를 반환하고, 언더플로 시 부호 있는 0을 보존한다.

## 5. 예제와 오용 사례

### view는 C 문자열이 아니다

잘못된 예:

```text
proven_u8str_view_t view = get_view();
printf("%s\n", (const char *)view.ptr); /* wrong: view may not be NUL-terminated */
```

올바른 예:

```c
proven_u8str_view_t view = proven_u8str_view_slice(PROVEN_LIT("/etc/hosts"), 5, 5);

/* A view is a pointer and a length into somebody else's bytes. To hand it to a
 * C API that wants a NUL, allocate a real C string from it. */
proven_result_cstr_t c = proven_u8str_view_to_cstr(view, alloc);
if (!proven_is_ok(c.err)) {
    return;
}
/* c.value is "hosts", NUL-terminated. It is yours, so free it. */
alloc.free_fn(alloc.ctx, (void *)c.value);
```

### `PROVEN_LIT`은 리터럴용이다

올바른 예:

```c
proven_u8str_view_t a = PROVEN_LIT("abc");
(void)a;
```

잘못된 예:

```text
const char *runtime = getenv("NAME");
proven_u8str_view_t a = PROVEN_LIT(runtime); /* wrong: macro requires literal syntax */
```

사용법:

```c
const char *runtime = "NAME=value";   /* any trusted NUL-terminated string */
proven_u8str_view_t a = proven_u8str_view_from_cstr(runtime);
(void)a;
```

### 못 찾은 것과 교체된 것을 구별하라

`proven_u8str_replace_first()`는 target을 찾지 못하면 `PROVEN_OK`를 반환한다. 이것이 중요하다면 먼저 검색하라.

```c
proven_result_u8str_t r = proven_u8str_create_from_view(alloc, PROVEN_LIT("the old way"));
if (!proven_is_ok(r.err)) {
    return;
}
proven_u8str_t s = r.value;

proven_size_t at = proven_u8str_view_find(proven_u8str_as_view(&s), 0, PROVEN_LIT("old"));
if (at != PROVEN_INDEX_NOT_FOUND) {
    (void)proven_u8str_replace_first(&s, 0, PROVEN_LIT("old"), PROVEN_LIT("new"));
}

proven_u8str_destroy(alloc, &s);
```

### 경계가 있는 포맷 입력

잘못된 예:

```text
char *untrusted = get_untrusted_pointer();
proven_println("{}", PROVEN_ARG(untrusted));
/* wrong: C-string formatting scans until NUL */
```

올바른 예:

```c
char untrusted[16] = { 'n', 'o', ' ', 'n', 'u', 'l', ' ', 'h', 'e', 'r', 'e', '!', '!', '!', '!', '!' };
proven_size_t max_len = sizeof untrusted;

/* The bounded form stops looking for a NUL after max_len bytes. */
proven_println("{}", PROVEN_ARG_CSTR_N(untrusted, max_len));
```

### borrowed 고정 용량 문자열

`proven_u8str_borrow`를 사용하면 어떤 할당도 없이 스택이나 정적 버퍼에 포매팅할
수 있다 — allocator 없는 코드와 핫패스에서 유용하다. 고정 용량 연산(그리고
`proven_u8str_append_fmt`)을 사용하고, `proven_u8str_reset`으로 재사용하며,
크기를 늘리는 연산이나 `proven_u8str_destroy`를 호출하지 말라(메모리는 호출자가
소유한다).

```c
int cur = 3;
int total = 10;

proven_byte_t line[64];
proven_u8str_t s = proven_u8str_borrow(line, sizeof line);   /* cap includes NUL */

proven_fmt_result_t fr = proven_u8str_append_fmt(&s, "L{}/{}", PROVEN_ARG(cur), PROVEN_ARG(total));
if (PROVEN_FMT_IS_OK(fr)) {
    proven_println("{}", PROVEN_ARG(proven_u8str_as_view(&s)));
}

(void)proven_u8str_reset(&s);   /* reuse next frame, no allocation */
```

오용: borrowed 용량을 초과하게 될 성장 호출은 호출자 메모리를 재할당하는 대신
`PROVEN_ERR_OUT_OF_BOUNDS`를 반환한다.

```c
proven_byte_t small[4];
proven_u8str_t t = proven_u8str_borrow(small, sizeof small);
proven_err_t e = proven_u8str_append_grow(alloc, &t, PROVEN_LIT("toolong"));
(void)e;   /* e == PROVEN_ERR_OUT_OF_BOUNDS; small[] is untouched */
```

### 자기 참조 포매팅

잘못된 예:

```text
const char *inside = proven_u8str_as_cstr(&s);
proven_u8str_append_fmt_grow(alloc, &s, "{}", PROVEN_ARG(inside));
/* wrong: self-aliasing C-string arguments are rejected */
```

올바른 예:

```c
proven_result_u8str_t r = proven_u8str_create_from_view(alloc, PROVEN_LIT("ab"));
if (!proven_is_ok(r.err)) {
    return;
}
proven_u8str_t s = r.value;

/* A view carries a length, so the formatter knows exactly which bytes to snapshot. */
proven_u8str_view_t before = proven_u8str_as_view(&s);
proven_fmt_result_t fr = proven_u8str_append_fmt_grow(alloc, &s, "{}", PROVEN_ARG(before));
(void)fr;   /* s == "abab" */

proven_u8str_destroy(alloc, &s);
```

### 트랜잭션 스캐닝

잘못된 가정:

```text
proven_err_t e = proven_scan_fmt_cursor(&scan, "{} suffix", PROVEN_SCAN_ARG(&x));
/* if suffix mismatches, x and scan.cursor may already have changed */
```

올바른 패턴:

```c
proven_scan_t scan = proven_scan_init(PROVEN_LIT("42 prefix"));
int x = -1;

proven_size_t old_cursor = scan.cursor;
int old_x = x;

proven_err_t e = proven_scan_fmt_cursor(&scan, "{} suffix", PROVEN_SCAN_ARG(&x));
if (!proven_is_ok(e)) {
    /* The literal "suffix" did not match, but 42 was already written into x and
     * the cursor already moved. Put both back yourself. */
    scan.cursor = old_cursor;
    x = old_x;
}
```

### 완성 예제: owned 문자열과 borrowed 문자열

테스트 스위트가 컴파일하고 실행한다. 핵심이 되는 구분: owned 문자열은 재할당할 수 있고 반드시 파괴해야 한다. borrowed 문자열은 호출자 메모리를 감싸고, 결코 재할당하지 않으며, 준 버퍼를 넘어 커지기를 조용히 옮기는 대신 거부한다.

<!-- example: manual/examples/ex_03_u8str.c -->
```c
/*
 * There are two string handles here and the difference is ownership, not size:
 *
 *   proven_u8str_t      - a byte string you can edit. It either owns an
 *                         allocation (create) or borrows one of yours (borrow).
 *   proven_u8str_view_t - a pointer and a length into somebody else's bytes.
 *                         It owns nothing, it is not NUL-terminated, and it is
 *                         only valid while those bytes are.
 *
 * A view is what you pass to a function that reads. A u8str is what you keep.
 */

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    /* --- an OWNED string: the allocator's memory, yours to destroy ---------- */
    /* The capacity argument is content bytes; the NUL is extra, so as_cstr is
     * always O(1) and always safe. */
    proven_result_u8str_t r = proven_u8str_create(alloc, 16);
    EXAMPLE_REQUIRE(proven_is_ok(r.err), "creating a 16-byte string must succeed");
    if (!proven_is_ok(r.err)) {
        return 1;
    }
    proven_u8str_t path = r.value;

    /* append is fixed-capacity: it fits or it fails, and on failure it has not
     * touched the string. It never reallocates, so it needs no allocator. */
    proven_err_t err = proven_u8str_append(&path, PROVEN_LIT("/etc/hosts"));
    EXAMPLE_REQUIRE(proven_is_ok(err), "10 bytes fit in a 16-byte string");

    /* append_grow is the growable twin: give it the allocator the string was
     * created with and it reallocates when needed. Still failure-atomic - if the
     * allocation fails, the string is exactly as it was. */
    err = proven_u8str_append_grow(alloc, &path, PROVEN_LIT(".backup.original"));
    EXAMPLE_REQUIRE(proven_is_ok(err), "append_grow must reallocate rather than fail");

    /* Edits in the middle. insert shifts the tail right; remove shifts it left. */
    err = proven_u8str_insert_grow(alloc, &path, 0, PROVEN_LIT("/mnt"));
    EXAMPLE_REQUIRE(proven_is_ok(err), "inserting a prefix must succeed");

    err = proven_u8str_remove(&path, proven_u8str_as_view(&path).size - 9, 9);  /* drop ".original" */
    EXAMPLE_REQUIRE(proven_is_ok(err), "removing the trailing suffix must succeed");

    /* replace_first returns PROVEN_OK when the target is absent - "nothing to do"
     * is not an error. Search first when the difference matters to you. */
    err = proven_u8str_replace_first(&path, 0, PROVEN_LIT("hosts"), PROVEN_LIT("fstab"));
    EXAMPLE_REQUIRE(proven_is_ok(err), "replacing an existing substring must succeed");

    /* --- reading it: borrow a view, do not copy ----------------------------- */
    /* as_view is free. The view is only good until the next edit: any growing
     * call may reallocate and leave the view (and any cstr) dangling. */
    proven_u8str_view_t v = proven_u8str_as_view(&path);

    EXAMPLE_REQUIRE(proven_u8str_view_eq(v, PROVEN_LIT("/mnt/etc/fstab.backup")),
                    "the edits above should have produced /mnt/etc/fstab.backup");
    EXAMPLE_REQUIRE(proven_u8str_view_starts_with(v, PROVEN_LIT("/mnt")),
                    "the inserted prefix is at the front");

    proven_size_t dot = proven_u8str_view_find(v, 0, PROVEN_LIT(".backup"));
    EXAMPLE_REQUIRE(dot != PROVEN_INDEX_NOT_FOUND, "the suffix must be found");

    /* A slice is a view into the SAME bytes - no allocation, no copy. */
    proven_u8str_view_t stem = proven_u8str_view_slice(v, 0, dot);
    EXAMPLE_REQUIRE(proven_u8str_view_eq(stem, PROVEN_LIT("/mnt/etc/fstab")),
                    "slicing at the suffix leaves the stem");

    /* as_cstr is the escape hatch to C APIs, and it is only valid because the
     * owned string keeps a NUL past its length. Do NOT do this with a view:
     * `stem.ptr` is not NUL-terminated - it just points into `path`. */
    printf("owned:  %s\n", proven_u8str_as_cstr(&path));

    /* --- a BORROWED string: your memory, no allocation at all --------------- */
    /* Same type, same operations - but the bytes are this stack buffer. `cap`
     * includes the NUL, so this holds 31 content bytes. */
    proven_byte_t line[32];
    proven_u8str_t status = proven_u8str_borrow(line, sizeof line);

    err = proven_u8str_append(&status, PROVEN_LIT("mounted "));
    EXAMPLE_REQUIRE(proven_is_ok(err), "appending into a borrowed buffer needs no allocator");
    err = proven_u8str_append(&status, stem);
    EXAMPLE_REQUIRE(proven_is_ok(err), "a view can be appended just like a literal");

    /* The growing calls exist for a borrowed string, but they refuse to
     * reallocate memory they do not own: too much data is OUT_OF_BOUNDS, and
     * `line` is left untouched. A borrowed string cannot silently escape to the
     * heap behind your back. */
    err = proven_u8str_append_grow(alloc, &status,
                                   PROVEN_LIT(" ...and a great deal more text than fits"));
    EXAMPLE_REQUIRE(err == PROVEN_ERR_OUT_OF_BOUNDS,
                    "a borrowed string reports overflow instead of reallocating caller memory");
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_as_view(&status), PROVEN_LIT("mounted /mnt/etc/fstab")),
                    "the failed append must have left the string unchanged");

    printf("borrowed: %s\n", proven_u8str_as_cstr(&status));

    /* reset truncates to empty and keeps the buffer, so the next frame reuses
     * the same 32 bytes with no allocation. */
    err = proven_u8str_reset(&status);
    EXAMPLE_REQUIRE(proven_is_ok(err), "reset must succeed on a borrowed string");
    EXAMPLE_REQUIRE(proven_u8str_as_view(&status).size == 0, "reset empties the string");

    /* --- destroy: the ownership rule, spelled out --------------------------- */
    /* destroy on the borrowed string is a no-op - `line` is not the library's to
     * free. Calling it anyway is correct and costs nothing, and it means the
     * teardown code does not have to know which kind of string it holds. */
    proven_u8str_destroy(alloc, &status);

    /* destroy on the owned string frees the allocation, and it must be given the
     * allocator the string was created with. */
    proven_u8str_destroy(alloc, &path);
    return EXAMPLE_OK();
}
```
