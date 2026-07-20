# 3장: 문자열, 포매팅, 스캐닝

**Part II — 모든 프로그램이 쓰는 어휘. 선행 장:
[1장](manual-01-foundation-ko.md)과 [2장](manual-02-allocation-ko.md).**
**이 장을 마치면** NUL 종료자가 프로그램의 운명을 결정하지 않는 방식으로 텍스트를 담을 수 있고,
오버플로를 거부하는 문자열을 만들 수 있으며, 일상적인 경우의 포매팅과 파싱을 할 수 있다.

이 장은 `u8str.h`, `u16str.h`, `fmt.h`, `scan.h`를 다룬다. 텍스트 자료의 **튜토리얼 절반**으로서,
매일 만나는 사례로 포매터와 스캐너를 소개한다. [8장](manual-08-fmt-scan-ko.md)이 레퍼런스 절반이다
— 전체 문법, 모든 인자 생성자, 스캐너의 에러와 복구 규칙을 담는다. 이 장을 먼저 읽으라.

## 목차

1. [U8 문자열과 view](#1-u8-문자열과-view)
2. [U16 문자열과 view](#2-u16-문자열과-view)
3. [포매팅](#3-포매팅)
4. [스캐닝](#4-스캐닝)
5. [예제와 오용 사례](#5-예제와-오용-사례)

## 1. U8 문자열과 view

### 문제: C 문자열은 자기 길이를 모른다

C 문자열은 포인터이고, 어디서 끝나는지는 메모리 어딘가의 0 바이트가 결정한다. 문자열당 1바이트를
아끼려고 1972년에 내린 그 결정 하나가 놀랄 만큼 많은 피해의 배후에 있다:

- **길이 구하기가 검색이다.** `strlen`은 문자열을 훑는다. 반복마다 `strlen(s)`를 검사하는 루프는
  제곱 시간이고, 겉보기에는 평범한 코드처럼 보인다.
- **텍스트가 0 바이트를 담을 수 없다.** 그래서 C 문자열은 UTF-16 버퍼도, 프로토콜 프레임도,
  파일의 일부도, 어떤 이진 데이터도 담을 수 없다 — 문자열 타입과 바이트 타입은 다른 것인데도
  언어는 아닌 척한다.
- **종료자가 없는 것은 탐지할 수 없다.** 자리가 없는 버퍼로 `strcpy`하면 스택 프레임 어딘가에서
  0을 찾을 때까지 써 나간다. 아무것도 그것을 보고하지 않는다. 이는 이 언어 역사상 가장 많이 악용된
  버그 부류다.
- **문자열의 일부를 저렴하게 가리킬 수 없다.** "이 줄의 세 번째 필드"라는 말은 그 바이트를 복사해
  내거나, 원본 위에 0을 써서 원본을 망가뜨리거나 둘 중 하나를 뜻한다. `strtok`은 두 번째를 골랐고,
  그래서 입력을 변형시키며 중첩할 수 없다.

전통적인 땜질인 `strncpy`는 항상 NUL로 종료해 주지는 않는다 — 그러니 그 "안전한" 함수가 문자열이
아닌 문자열을 만들어 낼 수 있다.

### 이 라이브러리가 대신 하는 것

두 개의 타입이 있고, 그 둘의 차이는 소유권이다:

- **`proven_u8str_view_t` — borrowed.** 포인터와 크기가 함께, 다른 누군가가 소유한 바이트를
  가리킨다. 복사는 공짜다. 아무것도 할당하지 않고 아무것도 파괴하지 않으며, 소유자가 사라지면 함께
  유효하지 않게 된다. 함수에 전달하는 것이 이것이다.
- **`proven_u8str_t` — owned.** 자기 저장소와 용량을 가지고, NUL 종료자를 대신 유지해 주므로
  `proven_u8str_as_cstr`이 여전히 NUL을 원하는 libc 함수에 바이트를 건네줄 수 있다. allocator로
  만들었으니, 같은 것으로 파괴한다.

둘 다 문자가 아니라 **바이트**를 센다. `"한"`은 UTF-8에서 3바이트이고 1문자인데, 이 라이브러리는
3이라고 말할 것이다. 그것이 라이브러리가 아는 것이기 때문이다. 여기서 텍스트는 바이트의 나열이며,
그 바이트를 문자로 해석하는 일은 이 라이브러리에 없는 Unicode 계층의 몫이다.

view는 자기 길이를 지니고 다니므로, NUL 종료자가 어렵게 만들었던 모든 것이 평범해진다: 길이는
필드이고, 텍스트가 0 바이트를 담을 수 있으며, 부분 범위는 복사 없이 같은 메모리를 가리키는 view이고,
들어가지 않을 쓰기는 수행되는 대신 거부된다.

잘못된 예 — view를 C 문자열처럼 다루기:

```text
proven_u8str_view_t v = proven_u8str_view_slice(line, 4, 8);   /* a field inside a line */
printf("%s\n", (const char *)v.ptr);   /* wrong: no terminator, prints until it finds a zero */
```

view는 의도적으로 NUL 종료되지 *않는다*: 보통 다른 누군가의 버퍼 한가운데를 가리키며, 거기에
종료자를 쓰면 다음 필드를 손상시킬 것이다. 크기를 받는 `proven_println("{}", PROVEN_ARG(v))`를
쓰거나, 먼저 owned 문자열로 복사하라.

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
- `proven_u8str_t`: NUL로 종료되는 owned 문자열.
- `proven_result_u8str_t`: owned 문자열용 result 래퍼.
- `proven_result_cstr_t`: 할당된 NUL 종료 C 문자열용 result 래퍼.

### 내부 레이아웃

view는 보이는 그대로다 — 포인터 하나와 바이트 개수 하나:

```text
typedef struct { const proven_byte_t *ptr; proven_size_t size; } proven_u8str_view_t;
typedef struct { proven_byte_t *ptr;       proven_size_t size; } proven_u8str_mut_t;
```

owned 문자열은 `proven_buf_t`(2장의 고정 용량 바이트 버퍼)에 플래그 하나를 더해
감싼다:

```text
typedef struct {
    proven_buf_t internal;   /* the bytes + length + capacity; always keeps room for a NUL */
    bool         borrowed;   /* false = allocator-owned (default); true = wraps caller memory */
} proven_u8str_t;
```

- `internal.len`이 바이트 길이다(`proven_buf_t`는 `ptr` / `len` / `cap`이며 —
  `size` 멤버는 없다). 문자열이 유효한 동안 `internal.len` 위치의 바이트는 항상 NUL
  종료자이므로, `proven_u8str_as_cstr()`은 O(1)이다.
- `borrowed`는 0으로 초기화된 핸들에서 `false`이므로, allocator가 소유하는 문자열이
  안전한 기본값이다. 이 값은 여러분이 소유한 `[buf, buf+cap)`을 감싸는
  `proven_u8str_borrow`에 의해서만 `true`가 된다: 그때는 성장 연산이 재할당을
  거부하고(`PROVEN_ERR_OUT_OF_BOUNDS`), `proven_u8str_destroy`는 no-op이 된다.
- 문자열을 바꾸려고 이 필드들을 직접 읽거나 쓰지 **말라**. 아래의 함수를 사용하라.
  길이를 얻으려고 `internal.len`을 읽는 것은 괜찮지만, `proven_u8str_as_view()`를
  선호하라.

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
| `PROVEN_LIT_INIT(s)` | 리터럴 view의 초기화자 형태. | 집합체 초기화에 사용하라. |
| `PROVEN_INDEX_NOT_FOUND` | find 함수가 반환하는 센티널. | `(proven_size_t)-1`과 같다. |

### U8 함수

| API | 의도 | 반환 |
|---|---|---|
| `proven_u8str_create(alloc, limit)` | `limit` 바이트에 NUL을 더한 용량을 가진 빈 owned 문자열을 만든다. | `proven_result_u8str_t`. |
| `proven_u8str_create_from_view(alloc, view)` | view를 새 owned 문자열로 복사한다. | `proven_result_u8str_t`. |
| `proven_u8str_borrow(buf, cap)` | 호출자가 소유한 `[buf, buf+cap)`을 고정 용량 문자열로 감싼다(할당 없음). 고정 용량 연산과 `append_fmt`는 동작하고, 성장 연산은 호출자 메모리 재할당을 거부하며, destroy는 no-op이다. `cap`은 NUL을 포함한다. | `proven_u8str_t`. |
| `proven_u8str_reset(str)` | 버퍼/용량은 재사용을 위해 유지한 채 빈 상태로 자른다(owned든 borrowed든). | `proven_err_t`. |
| `proven_u8str_is_valid(str)` | 공개 문자열 불변식을 검증한다. | `bool`. |
| `proven_u8str_reserve(alloc, str, new_cap)` | 내부 용량이 최소 `new_cap` 바이트가 되도록 보장한다. (borrowed 문자열은 성장할 수 없다.) | `proven_err_t`. |
| `proven_u8str_append(str, data)` | 원자적 고정 용량 append. | `PROVEN_OK` 또는 `PROVEN_ERR_OUT_OF_BOUNDS`. |
| `proven_u8str_append_partial(str, data)` | 잘라내는 append. | `proven_result_size_t`; `value`는 쓰인 바이트 수. |
| `proven_u8str_append_grow(alloc, str, data)` | 원자적 성장 가능 append. | `proven_err_t`. |
| `proven_u8str_append_byte(alloc, str, b)` | 바이트 하나를 append하며, 필요하면 성장한다. | `proven_err_t`. |
| `proven_u8str_replace_at(str, index, old_len, data)` | 정확한 바이트 범위를 고정 용량 의미로 치환한다. | `proven_err_t`. |
| `proven_u8str_replace_at_grow(alloc, str, index, old_len, data)` | `replace_at`과 같지만, 편집이 들어가지 않을 때 실패하는 대신 버퍼를 성장시킨다(2배씩). | `proven_err_t`; 할당 실패 시 문자열은 변경되지 않는다. |
| `proven_u8str_insert(str, index, data)` | 고정 용량 의미로 `index`에 바이트를 삽입한다. | `proven_err_t`. |
| `proven_u8str_insert_grow(alloc, str, index, data)` | `insert`와 같지만 필요할 때 버퍼를 성장시킨다. 수동 `reserve`가 필요 없다. | `proven_err_t`; 할당 실패 시 문자열은 변경되지 않는다. |
| `proven_u8str_remove(str, index, len)` | 바이트 범위를 제거한다. | `proven_err_t`. |
| `proven_u8str_replace_first(str, start_offset, target, replacement)` | start 이후 처음 일치하는 target을 치환한다. | 찾지 못해도 `PROVEN_OK`. |
| `proven_u8str_view_find(haystack, start_offset, needle)` | 바이트 부분 문자열의 첫 등장을 찾는다(어떤 바이트 값이든 올바르게 처리하며, NUL 종료를 요구하지 않는다). | 인덱스 또는 `PROVEN_INDEX_NOT_FOUND`. |
| `proven_u8str_view_starts_with(str, prefix)` | 접두사 검사. | int 진리값. |
| `proven_u8str_view_ends_with(str, suffix)` | 접미사 검사. | int 진리값. |
| `proven_u8str_view_slice(str, index, len)` | 잘라 맞춘(clamped) 부분 view를 반환한다. | `proven_u8str_view_t`. |
| `proven_u8str_as_cstr(str)` | 내부의 NUL 종료 포인터를 반환한다. | `const char *`; 성장/파괴에 의해 무효화된다. |
| `proven_mem_view_from_u8(view)` | U8 view를 바이트 view로 변환한다. | `proven_mem_view_t`. |
| `proven_u8str_view_to_cstr(view, alloc)` | 임의의 view로부터 NUL 종료 C 문자열을 할당한다. | `proven_result_cstr_t`; 호출자가 allocator로 해제한다. |
| `proven_cstr_len(s)` | NUL까지의 바이트를 센다. | `proven_size_t`. |
| `proven_u8str_view_from_cstr(s)` | 신뢰할 수 있는 NUL 종료 문자열로부터 view를 만든다. | 널 포인터에는 빈 view. |
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

### 두 번째 문자열 타입이 아예 존재하는 이유

UTF-8이 옳은 기본값이고 이 라이브러리는 거기에 전념한다. `proven_u16str_t`는 단 하나의 이유로
존재한다: **Windows API가 UTF-16이기 때문이다.** 모든 "와이드" 진입점 — `CreateFileW`,
`GetEnvironmentVariableW`, `W` 계열 전체 — 은 `wchar_t *`를 받고, Windows에서 그것은 16비트다. 그
API들과 대화하는 라이브러리에는 그들의 코드 유닛을 바이트인 척하지 않고 담을 타입이 필요하다.

그러니 이것은 경계 타입이다. UTF-16 API에 닿는 곳에서 이것을 쓰고, 그 밖의 모든 곳에서는
`proven_u8str_t`를 쓴다. 이 타입은 의도적으로 작다 — create, destroy, append, 길이 — 프로그램이
이 타입으로 사고하도록 만든 것이 아니기 때문이다.

붙들어 둘 것이 둘 있다. 모든 UTF-16 버그의 근원이기 때문이다:

- **코드 유닛은 문자가 아니다.** UTF-16은 Basic Multilingual Plane 밖의 것을 *서로게이트 쌍*으로
  인코딩한다 — 한 문자를 뜻하는 두 개의 `proven_u16` 값이다. 이모지는 코드 유닛 두 개다. 그 사이를
  잘라내면 짝 없는 서로게이트가 나오고, 그것은 유효한 UTF-16이 아니다.
- **여기서 `size`는 바이트가 아니라 코드 유닛을 센다.** `proven_u16str_view_t.size`는 `proven_u16`
  값의 개수다. 밑에 깔린 `proven_buf_t`는 바이트를 추적하며, 그래서 `proven_u16str_len`이 나눗셈을
  한다. 이 두 단위를 섞는 것이 이 타입에서 가장 흔한 실수다.

**이 라이브러리에는 UTF-8과 UTF-16 사이의 변환이 없다.** 이는 진짜 빈틈이며, 잊어버린 것이 아니라
의도적이다: 올바른 변환이란 유효하지 않은 입력, 짝 없는 서로게이트, 과장 인코딩을 어떻게 할지
결정하는 일이고, 그것은 Unicode 계층의 몫이다. 오늘날 `proven_u16str_t`는 `u"..."` 리터럴이나 이미
가지고 있는 코드 유닛으로부터 만든다.

`PROVEN_NO_U16STR`이 정의되면 U16 API는 제외되며, 이는 freestanding 빌드의 기본값이다 — 베어메탈
타깃에는 대화할 Windows API가 없다.

잘못된 예 — 코드 유닛 하나가 문자 하나라고 가정하기:

```text
proven_u16str_view_t v = ...;                 /* text containing an emoji */
proven_size_t chars = proven_u16str_len(&s);  /* wrong: that is code units */
```

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
| `proven_u16str_append(str, data)` | 원자적 고정 용량 append. | `proven_err_t`. |
| `proven_u16str_append_partial(str, data)` | 잘라내는 append. | `proven_result_size_t`. |
| `proven_u16str_append_grow(alloc, str, data)` | 원자적 성장 가능 append. | `proven_err_t`. |
| `proven_u16str_as_ptr(str)` | 내부 `proven_u16` 포인터를 반환한다. | `const proven_u16 *`. |
| `proven_u16str_len(str)` | 코드 유닛 단위 길이를 반환한다. | `proven_size_t`. |

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

참고: `proven_u16`은 코드 유닛이지, 반드시 하나의 완전한 Unicode 문자인 것은 아니다. UTF-16 서로게이트 쌍은 코드 유닛 두 개를 쓴다.

## 3. 포매팅

### 문제: `printf`는 타입을 두 번 듣는다

`printf("%d", x)`는 `x`의 타입을 두 번 말한다 — 한 번은 포맷 문자열에서, 한 번은 `x`를 전달하면서
— 그리고 그 둘이 일치하는지는 아무것도 검사하지 않는다. 가변 인자는 타입을 지우므로, 함수는 호출
규약이 남긴 바이트가 무엇이든 포맷이 요구한 모양으로 읽는다:

```text
printf("%d\n", 3.0);      /* wrong: reads a double's bytes as an int */
printf("%s\n", 42);       /* wrong: dereferences 42 as a pointer */
printf("%d %d\n", 1);     /* wrong: reads an argument that was never passed */
```

셋 다 컴파일된다. 현대 컴파일러는 포맷이 리터럴일 때 경고해 주는데, 그건 포맷이 변수가 되기
전까지의 이야기다 — 그리고 그때 여러분에게는 요청만 하면 임의의 스택 메모리를 읽어 주는 함수가
생기며, 이는 자기 이름까지 붙은 취약점 부류다.

두 번째 문제는 출력이 어디로 가느냐다. `sprintf`는 자기가 크기를 모르는 버퍼에 쓴다. `snprintf`는
크기를 받고 나서 **잘라내며**, 썼을 *뻔한* 길이를 반환한다 — 그러니 비교하는 것을 잊은 호출자는
조용히 짧아진 경로나 명령이나 식별자를 얻는다.

### 이 라이브러리가 대신 하는 것

**자리표시자 안에는 타입이 없다.** `{}`는 "여기에 값이 온다"고 말하고, 타입은 인자에서 오며 컴파일
시간에 결정된다:

```text
proven_println("{} scored {}", PROVEN_ARG(name), PROVEN_ARG(score));
```

`PROVEN_ARG`는 `_Generic` 디스패치다 — 컴파일러가 인자의 정적 타입에 맞는 생성자를 고른다. 포맷은
결코 타입을 말하지 않으므로 포맷과 인자가 어긋나는 일은 애초에 불가능하다. `:` 뒤의 스펙이
제어하는 것은 *표현* — 폭, 채움, 정렬, 정밀도, 진법 — 이지 결코 해석이 아니다.

**목적지는 크기를 아는 객체이며**, 고정 용량 형태는 잘라내는 대신 거부한다:
`proven_u8str_append_fmt`은 `PROVEN_ERR_OUT_OF_BOUNDS`로 실패하고 아무것도 쓰지 않으며,
`proven_u8str_append_fmt_grow`는 allocator를 받아 성장한다. 어느 쪽을 호출했는지는 호출 그 자체에
드러난다.

**여러분의 타입도 여기에 참여할 수 있다.** `PROVEN_ARG_OF(&obj, render_fn)`은 여러분이 정의한
타입이 다른 모든 것처럼 `{}`로 출력되게 해 준다 — 확장 지점은 이름 레지스트리가 아니라 컴파일
시간의 타입 있는 지점이다. [8장 §5.1](manual-08-fmt-scan-ko.md)이 방법을 보여준다.

비용을 그대로 말하자면: 인자마다 `PROVEN_ARG`를 두르는 것은 `%d`보다 타이핑이 많고, 이 포맷 언어는
손에 익은 그 언어가 아니다. 그 대가로 사는 것은 이 절 첫머리의 버그 부류를 아예 쓸 수 없게 된다는
것이다.

포매터는 `proven_u8str_t`나 PAL 기반 스트림에 쓴다. `{}` 자리표시자, `{1}` 같은 명시적 인덱스, 이스케이프된 중괄호 `{{`와 `}}`, 그리고 `{:0>5}`, `{:*^10}`, `{:.<10}` 같은 폭/정렬 스펙을 갖춘 작은 구조적 포맷 언어를 사용한다.

잘못된 예 — 스펙이 타입을 고른다고 가정하기:

```text
proven_println("{:d}", PROVEN_ARG(3.5));   /* wrong: the spec formats, it does not convert */
```

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
- `written`: 실제로 쓰인 바이트.
- `required`: 전체 출력에 필요한 바이트.

`proven_arg_type_t` 변종:

- `PROVEN_ARG_NONE`
- `PROVEN_ARG_I32`
- `PROVEN_ARG_U32`
- `PROVEN_ARG_I64`
- `PROVEN_ARG_U64`
- `PROVEN_ARG_F64` — `PROVEN_FMT_NO_FLOAT`가 정의되지 않은 경우
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
| `proven_arg_none()` | 내부 센티널 값. |
| `proven_arg_i32(v)`, `proven_arg_u32(v)` | 정수 인자. |
| `proven_arg_i64(v)`, `proven_arg_u64(v)` | 넓은 정수 인자. |
| `proven_arg_f64(v)` | 부동소수점 인자 — float 포매팅이 비활성화되지 않은 경우. |
| `proven_arg_cstr(v)` | 신뢰할 수 있고 살아 있는 NUL 종료 C 문자열. |
| `proven_arg_cstr_n(v, max_len)` | 경계가 있는 C 문자열 인자; `max_len`까지만 NUL을 탐색한다. |
| `proven_arg_str_view(v)` | borrowed 문자열 view 인자. |
| `proven_arg_datetime(v)` | datetime 인자. |
| `proven_arg_ptr(v)` | 객체 포인터 인자. |
| `proven_arg_fn(v)` | 함수 포인터 인자. |
| `proven_arg_ucstr(v)` | unsigned char C 문자열 헬퍼. |
| `proven_arg_identity(v)` | 기존 `proven_arg_t`를 그대로 통과시킨다. |

### 포맷 매크로

| 매크로 | 의도 |
|---|---|
| `PROVEN_ARG(x)` | 지원되는 인자 타입에 대한 `_Generic` 선택자. |
| `PROVEN_ARG_FN(f)` | 함수 포인터 포매팅 헬퍼. |
| `PROVEN_ARG_CSTR_N(v, max_len)` | 경계가 있는 C 문자열 헬퍼. |
| `proven_u8str_append_fmt(str, fmt, ...)` | 원자적 고정 용량 포매팅. |
| `proven_u8str_append_fmt_trunc(str, fmt, ...)` | 최선을 다하는 잘라내기 포매팅. |
| `proven_u8str_append_fmt_grow(alloc, str, fmt, ...)` | 원자적 성장 가능 포매팅. |
| `proven_u8str_append_fmt_with_scratch(alloc, str, fmt, scratch, ...)` | 임시 패치 할당에 scratch allocator를 쓰는 성장 가능 포매팅. |
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

일반 사용자 코드는 이 내부 엔진 대신 매크로를 호출해야 한다. 엔진은 인덱스 0에 선두 `proven_arg_none()` 센티널이 있을 것을 기대한다.

사용되지 않는 여분의 포맷 인자는 `PROVEN_ERR_INVALID_ARG`를 반환한다.

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

### 문제: `scanf`는 어디서 멈췄는지 말해 주지 않는다

입력을 파싱하는 것은 포매팅의 거울이고, libc의 답은 더 나쁘다. `sscanf`는 *몇 개의 필드를
채웠는지*만 반환하고 그 외에는 아무것도 주지 않는다 — 어느 것이 실패했는지도, 어디까지 갔는지도,
왜인지도:

```text
int n = sscanf(line, "%d %d %d", &a, &b, &c);
if (n != 3) { /* wrong: which field? at what offset? was it malformed or missing? */ }
```

세 번째 필드가 잘못된 형식이라면, 여러분이 아는 것은 둘을 얻었다는 것뿐이다. 사용자에게 위치를
보고할 수 없고, 잘못된 레코드를 건너뛰고 재개할 수 없으며, "입력이 떨어졌다"와 "숫자가 아닌 무언가를
찾았다"를 구별할 수 없다. 그리고 `char *`로의 `%s`는 `strcpy`와 똑같은 무경계 쓰기 문제를 가지며,
이번에는 입력이 프로그램 바깥에서 온다.

그리고 `strtol`이 있는데, 그 계약은 먼저 `errno`를 지우고, 나중에 검사하고, "숫자가 하나도 없음"을
탐지하려고 `endptr`을 입력과 비교할 것을 요구한다 — 변환 하나에 제대로 해내야 할 것이 셋이다.

### 이 라이브러리가 대신 하는 것

**커서는 여러분의 것이다.** `proven_scan_t`는 파싱 중인 view와 그 안의 오프셋을 담는다. 각 스캔은
커서에서 읽고 성공하면 커서를 앞으로 옮긴다. 커서는 여러분이 읽을 수 있는 필드이므로, 파싱이 정확히
어디서 멈췄는지 항상 알 수 있다 — 그것이 사용자에게 보여줄 위치다.

**각 스캔은 result를 반환한다.** `proven_scan_i64`는 `{err, val}`을 돌려주므로, "숫자가 아님",
"범위를 벗어남", "입력의 끝"이 하나의 빠진 필드가 아니라 서로 다른 에러가 된다.

**실패는 커서를 되돌린다** — 원시 스캐너들에 한해서. 실패한 `proven_scan_i64`는 커서를 있던 자리에
남기므로, 같은 위치에서 다른 것을 시도할 수 있다. 그것이 짐작 대신 복구를 가능하게 하는 것이다.

이것에 기대기 전에 알아 둘 것 하나: *구조적* 스캔(`proven_scan_fmt`, `{}` 형태)은 필드 전체에 걸쳐
트랜잭션적이지 **않다**. 세 번째 자리표시자가 실패하면 앞의 두 목적지에는 이미 쓰여 있다.
[8장 §11.1](manual-08-fmt-scan-ko.md)이 에러 코드와 복구 패턴을 온전히 다룬다.

스캐너는 borrow된 `proven_u8str_view_t`로부터 파싱한다. 커서가 진행 상황을 추적한다.

잘못된 예 — 부분적인 구조적 스캔을 아무 일도 없었던 것처럼 다루기:

```text
proven_err_t e = proven_scan_fmt(&sc, "{} {} {}", ...);
if (!proven_is_ok(e)) {
    /* wrong: destinations for the fields that DID parse have already been written */
}
```

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

목적: 입력과 현재 파싱 위치를 담는다. `proven_scan_init()`은 유효하지 않은 비어 있지 않은 널 view를 빈 view로 정규화한다.

### 스캐너 함수

| API | 의도 | 반환 |
|---|---|---|
| `proven_scan_init(view)` | view로부터 스캐너를 만든다. | `proven_scan_t`. |
| `proven_scan_skip_whitespace(scan)` | 공백을 지나 진행한다. | void. |
| `proven_scan_i64(scan)` | 부호 있는 64비트 정수를 파싱한다. | `proven_result_i64_t`. |
| `proven_scan_u64(scan)` | 부호 없는 64비트 정수를 파싱한다. | `proven_result_u64_t`. |
| `proven_scan_f64(scan)` | 부동소수점 값을 파싱한다. | `proven_result_f64_t`. |
| `proven_parse_double_ascii(view)` | 공백을 건너뛰지 않고 로케일 독립적인 ASCII float 토큰 하나를 파싱한다. | `proven_parse_double_result_t`. |
| `proven_parse_f64_ascii(view)` | 같은 로케일 독립 binary64 파서에 대한 호환 별칭. | `proven_parse_f64_result_t`. |
| `proven_strtod(nptr, endptr)` | 공백 건너뛰기와 `endptr` 보고를 갖춘 `strtod` 스타일 토큰 하나를 파싱한다. | `double`. |
| `proven_scan_str(scan)` | 공백으로 구분된 토큰을 입력에 대한 view로 파싱한다. | `proven_result_u8str_view_t`. |
| `proven_scan_skip_until(scan, target)` | target을 찾으면 커서를 그쪽으로 옮긴다. | `proven_err_t`. |
| `proven_scan_skip_until_number(scan)` | 다음으로 숫자처럼 보이는 위치로 커서를 옮긴다. | void. |

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

명시적 고정 폭 및 유틸리티 헬퍼:

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
| `proven_scan_fmt_cursor(scan_ptr, fmt, ...)` | 기존 커서에서 스캔한다. |
| `proven_scan_fmt(view, fmt, ...)` | 임시 커서로 view에서 스캔한다. |

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
| `proven_scan_fmt_internal_view(view, fmt, args, count)` | view 위에 임시 스캐너를 만드는 편의 래퍼. | `proven_err_t`. |

일반 사용자 코드는 매크로를 호출해야 한다. 엔진은 선두 센티널 인자를 기대한다.

중요한 동작: `proven_scan_fmt_internal()`은 뒤쪽에서 리터럴 불일치가 일어나면 에러를 반환하기 전에 커서를 진행시키고 앞선 목적지에 써 넣었을 수 있다. 트랜잭션 같은 파싱이 필요하다면 커서와 목적지 값을 먼저 저장하라.

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

### Float 파싱 관련 사항

- `proven_scan_f64()`와 `proven_parse_double_ascii()`는 공유된
  decimal-to-binary64 백엔드를 거친다.
- 유한한 십진 입력은 round-to-nearest, ties-to-even 동작으로 IEEE-754 binary64로
  반올림된다.
- 현재 변환 스택은 `Clinger fast path -> staged
  Eisel-Lemire layer -> exact bigint fallback`이며, 대표적인 입력이 어느 경로를
  탔는지 테스트가 확인할 수 있도록 내부 카운터를 둔다.
- staged Eisel-Lemire 계층은 현재 생성된 `5^q` 양수 지수 경우와, 십진 유효숫자가
  요구되는 `5^q`를 깔끔하게 상쇄하는 정확한 음수 지수 경우를 받아들인다.
- `__uint128_t`가 있는 컴파일러에서는 같은 계층이 정상 범위 경우에 대해 보수적으로
  반올림된 음수 지수 비율 부분집합도 받아들이며, 원래의 좁은 프로토타입이 허용하던
  것보다 더 넓은 좌시프트 정규화를 포함한다.
- 넓혀진 캐시된 거듭제곱 곱 경로는 이제 `5e-324`를 포함한 일부 비정규(subnormal)
  경우도 단계적으로 처리하지만, true-min에 대한 절반 임계값 아래의 값들은 여전히
  정확한 bigint fallback으로 넘긴다.
- 체크인된 캐시 `5^q` 상수들은 `scripts/generate_float_decimal_tables.py`로
  로컬에서 생성된다.
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

### `PROVEN_LIT`은 리터럴을 위한 것이다

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

이렇게 하라:

```c
const char *runtime = "NAME=value";   /* any trusted NUL-terminated string */
proven_u8str_view_t a = proven_u8str_view_from_cstr(runtime);
(void)a;
```

### 찾지 못한 것과 치환된 것을 구별하라

`proven_u8str_replace_first()`는 target을 찾지 못했을 때 `PROVEN_OK`를 반환한다. 그 차이가 중요하다면 먼저 검색하라.

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

### Borrow된 고정 용량 문자열

`proven_u8str_borrow`를 사용하면 아무 할당 없이 스택이나 정적 버퍼에 포매팅할 수
있다 — allocator가 없는 코드와 뜨거운 경로에서 유용하다. 고정 용량 연산(그리고
`proven_u8str_append_fmt`)을 사용하고, `proven_u8str_reset`으로 재사용하며,
성장 연산이나 `proven_u8str_destroy`는 호출하지 말라(메모리는 호출자가 소유한다).

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

오용: borrow된 용량을 넘길 성장 호출은 호출자 메모리를 재할당하는 대신
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

### 트랜잭션적 스캐닝

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

### 실전 예제: owned 문자열과 borrow된 문자열

테스트 스위트에서 컴파일되고 실행된다. 중요한 구분은 이것이다: owned 문자열은 재할당될 수 있고 반드시 파괴해야 한다; borrow된 문자열은 호출자 메모리를 감싸고, 결코 재할당하지 않으며, 여러분이 준 버퍼를 넘어 성장하는 대신 그것을 조용히 옮기지 않고 거부한다.

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
    err = proven_u8str_insert_grow(alloc, &path, 0, PROVEN_LIT("/srv"));
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

    EXAMPLE_REQUIRE(proven_u8str_view_eq(v, PROVEN_LIT("/srv/etc/fstab.backup")),
                    "the edits above should have produced /srv/etc/fstab.backup");
    EXAMPLE_REQUIRE(proven_u8str_view_starts_with(v, PROVEN_LIT("/srv")),
                    "the inserted prefix is at the front");

    proven_size_t dot = proven_u8str_view_find(v, 0, PROVEN_LIT(".backup"));
    EXAMPLE_REQUIRE(dot != PROVEN_INDEX_NOT_FOUND, "the suffix must be found");

    /* A slice is a view into the SAME bytes - no allocation, no copy. */
    proven_u8str_view_t stem = proven_u8str_view_slice(v, 0, dot);
    EXAMPLE_REQUIRE(proven_u8str_view_eq(stem, PROVEN_LIT("/srv/etc/fstab")),
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
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_as_view(&status), PROVEN_LIT("mounted /srv/etc/fstab")),
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
