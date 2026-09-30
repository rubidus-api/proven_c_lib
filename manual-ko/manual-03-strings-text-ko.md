# 3장: 문자열, 포매팅, 스캐닝

**Part II — 모든 프로그램이 쓰는 어휘. 선행 장:
[1장](manual-01-foundation-ko.md)과 [2장](manual-02-allocation-ko.md).**
**이 장을 마치면** NUL 종료자가 프로그램의 운명을 결정하지 않는 방식으로 텍스트를 담을 수 있고,
오버플로를 거부하는 문자열을 만들 수 있으며, 일상적인 경우의 포매팅과 파싱을 할 수 있다.

이 장은 `u8str.h`, `u16str.h`, `utf.h`, `fmt.h`, `scan.h`를 다룬다. 텍스트 자료의 **튜토리얼 절반**으로서,
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
  `proven_u8str_as_cstr`이 여전히 NUL을 원하는 libc 함수에 바이트를 건네줄 수 있다. 할당자(allocator)로
  만들었으니, 같은 것으로 파괴한다.

**소유 문자열은 그 할당자를 기억하지 않으며, 아무것도 그것을 확인하지 않는다.** 할당하거나 해제할 수
있는 모든 호출 - `_create`, `_reserve`, `_grow` 계열, `_destroy` - 은 할당자를 인자로 받고, 그것은
문자열을 만든 할당자여야 한다. 다른 것을 넘기면 돌려줄 오류가 없다: 그 할당자는 자기가 준 적 없는
메모리를 받아 장부가 망가지고, 충돌은 나중에 엉뚱한 곳에서 일어난다. 모든 문자열에 할당자를 담으면
크기가 두 배가 되고 빌려 쓰는(borrowed) 문자열에는 의미도 없으므로 라이브러리는 그렇게 하지 않는다.
짝은 당신의 코드에서 지켜라 - 소유자마다 할당자 하나, 같은 자리에서 넘긴다.

반례 - 아레나(arena)에서 만든 문자열을 힙(heap)으로 파괴하는 경우:

```text
proven_u8str_t s = proven_u8str_create(arena, 64).value;
proven_u8str_destroy(proven_heap_allocator(), &s);   /* wrong: the heap frees arena memory */
```

둘 다 문자가 아니라 **바이트**를 센다. `"한"`은 UTF-8에서 3바이트이고 1문자인데, 이 라이브러리는
3이라고 말할 것이다. 그것이 라이브러리가 아는 것이기 때문이다. 여기서 텍스트는 바이트의 나열이며,
그 바이트를 문자로 해석하는 일은 이 라이브러리에 없는 Unicode 계층의 몫이다.

뷰(view)는 자기 길이를 지니고 다니므로, NUL 종료자가 어렵게 만들었던 모든 것이 평범해진다: 길이는
필드이고, 텍스트가 0 바이트를 담을 수 있으며, 부분 범위는 복사 없이 같은 메모리를 가리키는 view이고,
들어가지 않을 쓰기는 수행되는 대신 거부된다.

잘못된 예 — view를 C 문자열처럼 다루기:

```text
proven_u8str_view_t v = proven_u8str_view_slice(line, 4, 8);   /* a field inside a line */
printf("%s\n", (const char *)v.ptr);   /* wrong: no terminator, prints until it finds a zero */
```

view는 의도적으로 NUL 종료되지 *않는다*: 보통 다른 누군가의 버퍼 한가운데를 가리키며, 거기에
종료자를 쓰면 다음 필드를 손상시킬 것이다. 크기를 받는 `proven_println("{}", PROVEN_ARG(v))`를
쓰거나, 먼저 소유(owned) 문자열로 복사하라.

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

- `proven_u8str_view_t`: 빌려 쓰는(borrowed) 읽기 전용 바이트 문자열 view.
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

### 뷰 나누기, 다듬기, 끝에서 찾기, 정렬하기

뷰(view)는 그 안에서 찾고, 슬라이스(slice)로 잘라 내고, 같은지 비교할 수 있었고, 그게 전부였다. 프로그램이 텍스트
한 줄로 하는 나머지 일 - 필드로 자르고, 필드를 다듬고, 마지막 점을 찾고, 결과를 정렬하는 일 - 은 모든
호출자가 손으로 썼고, 손으로 쓴 판본들은 같은 곳에서 틀렸다. 나누기가 가장 분명한 경우다. 유능한 사람이
처음 쓰는 루프는 *"구분자가 발견되는 동안 계속"*이다:

```text
proven_size_t off = 0, at;
while ((at = proven_u8str_view_find(s, off, sep)) != PROVEN_INDEX_NOT_FOUND) {
    emit(proven_u8str_view_slice(s, off, at - off));   /* wrong: the tail is never emitted */
    off = at + sep.size;
}
```

이 루프는 마지막 구분자 뒤의 텍스트를 잃고, 그것도 *흔한* 경우에서 틀린다: `"a,b,c"`는 필드 두 개,
`"a"`는 없음, `""`도 없음. 바로잡으려면 마지막 필드를 루프 밖으로 끌어내야 하는데, 마감에 쫓길 때
빠뜨리는 게 바로 그 단계다. 호출자들이 실제로 택한 대안 - 필드마다 소유(owned) 문자열 하나 - 는 3.4배
느리고 필드마다 할당 한 번이었다(RFC-0002 2.2절).

그래서 뷰 어휘에 나머지 낱말이 생겼다. 모두 뷰 위의 순수 함수이고 할당하지 않는다. 두 규칙이 전부에
적용된다. **잘못 만들어진 뷰** - `ptr == NULL`이면서 `size > 0` - **는 빈 뷰로 취급한다**, 함수마다
있는 가드로. 그리고 **빈 결과는 언제나 `{NULL, 0}`**이다. `proven_u8str_view_slice`가 이미 쓰던 표기라서,
빈 결과는 위치를 담지 않는다: 뷰는 포인터가 아니라 크기로 검사하라.

**나누기는 영구적인 계약을 지킨다: 구분자 n개는 필드 n + 1개가 된다.** 왼쪽부터 겹치지 않게 센다.
`"a,"`는 `"a"`와 빈 필드, `",a"`는 빈 필드와 `"a"`, `"a,,b"`는 가운데 빈 필드를 지키며(`strtok`와 다르다),
`""`는 필드 0개가 아니라 빈 필드 하나다. 빈 구분자는 입력 전체를 한 번 내준다 - 빈 필드를 끝없이 내지
않는다. 뻔한 구현은 그렇게 되는데, 빈 needle 검색이 시작한 자리에서 일치하기 때문이다. 반복자는 자기
자신이 아니라 원본을 가리키므로 복사할 수 있고, 복사본은 따로 계속된다.

**다듬기는 정확히 여섯 바이트만 안다**: 공백, `\t`, `\n`, `\v`, `\f`, `\r`. 로케일도 유니코드도 아니다 -
줄 바꿈 없는 공백(no-break space)은 여기서 공백이 아니다. 접두사·접미사 제거는 그것이 없으면 뷰를
그대로 둔다. 오류가 아니며, 적용됐는지 알아야 하면 `proven_u8str_view_starts_with`로 먼저 물어라.

**`proven_u8str_view_find_last`는 위치를 돌려준다**: 마지막 출현의 시작이고, 출현은 겹칠 수 있다.
`find_last("aaa", "aa")`는 1이다. 빈 needle이면 위치는 `size`다 - `_find`와 같은 답 - 그리고 이것이
올바른 바이트 인덱스가 아닌 유일한 결과다. 앞방향 검색과 같은 표본으로
같은 선택을 한다: 보통 텍스트에서는 needle의 가장 드문 바이트를 끝에서부터 찾아 그 주변을 검증하고,
엔트로피가 낮은 haystack - 긴 반복, 작은 알파벳 - 에서는 입력이 무엇이든 선형인 알고리즘으로 바꾼다
(64바이트까지는 역방향 Shift-Or, 그 너머는 역방향 Two-Way).

**`proven_u8str_view_cmp`는 바이트를 부호 없이 비교하고 접두사를 먼저 둔다.** 그래서 `"\xFF"`는 `"a"`
뒤에, `"app"`은 `"apple"` 앞에 온다. 뷰 안의 NUL은 데이터다. 답은 부호로 한다 - 반드시 -1, 0, 1은
아니다. `proven_u8str_view_cmp_ptr`는 같은 비교를 `proven_array_sort`에 맞춘 모양으로, 원소를 가리키는
포인터를 받는다.

| API | 의도 | 반환 |
|---|---|---|
| `proven_u8str_view_split(src, sep)` | 나누기 시작; 필드는 `src` 안을 가리키는 뷰. | `proven_u8str_view_split_t` |
| `proven_u8str_view_split_next(&it, &field)` | 다음 필드, 더 없으면 `false`. | `bool` |
| `proven_u8str_view_trim(s)` | 양 끝에서 공백 여섯 바이트를 뗀다. | `proven_u8str_view_t` |
| `proven_u8str_view_trim_start(s)`, `proven_u8str_view_trim_end(s)` | 한쪽 끝만. | `proven_u8str_view_t` |
| `proven_u8str_view_remove_prefix(s, prefix)` | `prefix`를 뗀 `s`, 아니면 그대로의 `s`. | `proven_u8str_view_t` |
| `proven_u8str_view_remove_suffix(s, suffix)` | `suffix`를 뗀 `s`, 아니면 그대로의 `s`. | `proven_u8str_view_t` |
| `proven_u8str_view_find_last(haystack, needle)` | 마지막 출현의 시작, 또는 `PROVEN_INDEX_NOT_FOUND`. | `proven_size_t` |
| `proven_u8str_view_contains(haystack, needle)` | 0부터의 `find`가 `NOT_FOUND`가 아님. | `bool` |
| `proven_u8str_view_cmp(a, b)` | 바이트 단위 부호 없는 순서, 접두사 먼저; 부호만. | `int` |
| `proven_u8str_view_cmp_ptr(pa, pb)` | `proven_array_sort`용 `cmp`: 뷰를 가리키는 포인터. | `int` |
| `proven_u8str_view_is_well_formed(s)` | `{NULL, n > 0}`에서만 거짓. | `bool` |

```text
typedef struct {
    proven_u8str_view_t rest;  /* not yet yielded; points into the caller's source bytes */
    proven_u8str_view_t sep;
    bool                done;  /* set once the final field has been yielded */
} proven_u8str_view_split_t;   /* copyable: a copy continues independently */
```

반례 - 필드나 `_is_well_formed`로 루프를 끝내는 경우. 빈 필드는 `{NULL, 0}`이고, 텍스트 끝을 넘은
슬라이스도 그렇다. 둘 다 well formed다. `_is_well_formed`는 뷰를 읽어도 안전하다는 것만 말한다.
나누기는 `split_next`의 반환값으로, 검색은 `PROVEN_INDEX_NOT_FOUND`로 끝내라:

```text
while (proven_u8str_view_split_next(&it, &f) && f.ptr != NULL) { ... }  /* wrong: stops at the first empty field */
while (proven_u8str_view_is_well_formed(rest)) { ... }                 /* wrong: never false for a slice */
```

반례 - needle을 확인하지 않고 `find_last`의 답 자리의 바이트를 읽는 경우. 빈 needle이면 답은
`size`다:

```text
proven_size_t at = proven_u8str_view_find_last(s, needle);
char c = (char)s.ptr[at];   /* wrong when needle is empty: at == s.size, one past the end */
```

예제는 끝과 가운데에 구분자가 겹친 레코드를 나누고, 필드마다 다듬고, 마지막 점을 찾고, 이중 접미사를
떼고, 이름들을 정렬한다:

<!-- example: manual/examples/ko/ex_03_view_ops.c -->
```c
#include <string.h>

/*
 * 매일 하는 텍스트 일을 뷰 위에서: 한 줄을 필드로 나누고, 다듬고, 알려진 접두사나
 * 접미사를 떼고, 마지막 점을 찾고, 얻은 것을 정렬한다. 정렬에 쓰는 배열 말고는 아무것도
 * 할당하지 않는다. 모든 결과는 처음 받은 텍스트 안을 가리킨다.
 *
 * 두 규칙이 내내 지켜진다. 빈 결과는 {NULL, 0} 이므로, 뷰는 포인터가 아니라 크기로
 * 검사한다. 그리고 분할 루프는 split_next 의 반환값으로 끝낸다 - 구분자 n 개는 빈 필드를
 * 포함해 언제나 n + 1 개의 필드가 된다.
 */

static bool is(proven_u8str_view_t v, const char *want) {
    return v.size == strlen(want) && (v.size == 0 || memcmp(v.ptr, want, v.size) == 0);
}

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    /* --- 나누고, 필드마다 다듬기 -------------------------------------------- */

    /* 끝에 구분자가 있고 빈 필드가 있는 레코드: 구분자 다섯 개, 그래서 필드 여섯 개 -
     * 마지막은 빈 필드다. 사람이 처음 쓰는 루프("구분자가 발견되는 동안")는 다섯 개를
     * 내고 꼬리를 잃는다. */
    proven_u8str_view_t record = PROVEN_LIT(" report.tar.gz , draft.txt,,notes.md ,\tREADME ,");
    proven_u8str_view_t fields[8];
    int n = 0;
    proven_u8str_view_split_t it = proven_u8str_view_split(record, PROVEN_LIT(","));
    proven_u8str_view_t f;
    while (n < 8 && proven_u8str_view_split_next(&it, &f)) {
        fields[n++] = proven_u8str_view_trim(f);   /* ' ', \t, \n, \v, \f, \r - 그 밖에는 없다 */
    }
    EXAMPLE_REQUIRE(n == 6, "five separators give six fields");
    EXAMPLE_REQUIRE(is(fields[0], "report.tar.gz") && is(fields[3], "notes.md") && is(fields[4], "README"),
                    "each field is trimmed on both ends");
    EXAMPLE_REQUIRE(fields[2].size == 0 && fields[5].size == 0, "the empty fields are kept, and tested by size");

    /* 한쪽 끝이 의미 있는 텍스트를 위한 한쪽 다듬기. */
    EXAMPLE_REQUIRE(is(proven_u8str_view_trim_start(PROVEN_LIT("  indented  ")), "indented  "), "trim_start");
    EXAMPLE_REQUIRE(is(proven_u8str_view_trim_end(PROVEN_LIT("  indented  ")), "  indented"), "trim_end");

    /* --- 접두사, 접미사, 그리고 마지막 점 ----------------------------------- */

    proven_u8str_view_t name = fields[0];
    /* 확장자는 마지막 점 뒤에 있다: find 가 아니라 find_last. */
    proven_size_t dot = proven_u8str_view_find_last(name, PROVEN_LIT("."));
    EXAMPLE_REQUIRE(dot == 10, "the last dot in report.tar.gz is at 10");
    EXAMPLE_REQUIRE(is(proven_u8str_view_slice(name, dot + 1, name.size), "gz"), "so the extension is gz");

    /* remove_suffix 는 접미사가 없으면 뷰를 그대로 둔다 - 오류가 아니므로, 알아야 할
     * 때는 starts_with/ends_with 로 먼저 물어라. */
    EXAMPLE_REQUIRE(is(proven_u8str_view_remove_suffix(name, PROVEN_LIT(".tar.gz")), "report"), "the double suffix goes");
    EXAMPLE_REQUIRE(is(proven_u8str_view_remove_suffix(name, PROVEN_LIT(".zip")), "report.tar.gz"),
                    "an absent suffix changes nothing");
    EXAMPLE_REQUIRE(is(proven_u8str_view_remove_prefix(PROVEN_LIT("# heading"), PROVEN_LIT("# ")), "heading"),
                    "a known prefix goes");
    EXAMPLE_REQUIRE(proven_u8str_view_contains(fields[1], PROVEN_LIT("draft")), "contains is find != NOT_FOUND");

    /* --- 비어 있지 않은 필드 정렬 ------------------------------------------ */

    proven_result_array_t ra = proven_array_create(alloc, 8, sizeof(proven_u8str_view_t), alignof(proven_u8str_view_t));
    EXAMPLE_REQUIRE(proven_is_ok(ra.err), "creating the array must succeed");
    if (!proven_is_ok(ra.err)) return EXAMPLE_OK();
    proven_array_t names = ra.value;
    for (int i = 0; i < n; ++i) {
        if (fields[i].size > 0) (void)proven_array_push(&names, &fields[i]);
    }
    /* cmp_ptr 는 정렬용 모양의 cmp 다: 뷰를 가리키는 포인터를 받는다. 바이트는 부호 없이
     * 비교되고, 접두사가 먼저 온다. ASCII 에서 'R' 이 'd' 보다 작으므로 "README" 가
     * 소문자 이름들보다 앞선다. */
    proven_array_sort(&names, proven_u8str_view_cmp_ptr);
    const proven_u8str_view_t *first = proven_array_get(&names, 0);
    const proven_u8str_view_t *last = proven_array_get(&names, 3);
    EXAMPLE_REQUIRE(first && is(*first, "README") && last && is(*last, "report.tar.gz"), "sorted bytewise");
    EXAMPLE_REQUIRE(proven_u8str_view_cmp(PROVEN_LIT("app"), PROVEN_LIT("apple")) < 0,
                    "cmp answers by sign - never compare it with -1");

    /* --- well-formed 는 "찾았다" 도 "비어 있지 않다" 도 아니다 ---------------- */

    /* 끝을 넘은 슬라이스는 {NULL, 0} 이고, 이는 well formed 다 - 그래서 이 술어로는
     * 루프를 끝낼 수도, "비었다" 와 "끝을 넘었다" 를 가를 수도 없다. 뷰를 읽어도
     * 안전하다는 것만 말한다. */
    proven_u8str_view_t past = proven_u8str_view_slice(name, 100, 5);
    EXAMPLE_REQUIRE(proven_u8str_view_is_well_formed(past) && past.size == 0, "past the end: empty and well formed");
    EXAMPLE_REQUIRE(!proven_u8str_view_is_well_formed((proven_u8str_view_t){ NULL, 3 }), "only {NULL, n > 0} is not");

    printf("%d fields, %zu names sorted\n", n, (size_t)names.len);
    proven_array_destroy(&names);
    return EXAMPLE_OK();
}
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

`proven_u16str_t`는 `u"..."` 리터럴이나 이미 가지고 있는 코드 유닛으로부터 만들거나 - 흔한 경우로 -
프로그램이 들고 있는 UTF-8을 `utf.h`로 변환해서 만든다(다음 소절).

`PROVEN_NO_U16STR`이 정의되면 U16 API는 제외되며, 이는 프리스탠딩(freestanding) 빌드의 기본값이다 — 베어메탈
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

### UTF-8과 UTF-16 사이의 변환 (`utf.h`)

프로그램은 UTF-8을 들고 있고, 윈도 와이드 API는 UTF-16을 원하며, 그 API가 돌려주는 이름과 텍스트는
UTF-16인데 프로그램의 나머지는 그것을 UTF-8로 원한다. 이 경계를 넘는 프로그램은 모두 변환기가
필요하고, 변환기는 올바르지 않은 텍스트를 어떻게 할지 정해야 한다: 홀로 남은 연속 바이트, 과장
인코딩(overlong encoding), 인코딩된 서로게이트, UTF-16 쪽의 짝 없는 서로게이트. 고쳐 주고 싶은
유혹이 있다 - U+FFFD로 바꾸고 계속 가는 것 - 그러나 운영체제에 파일 이름을 건네는 라이브러리에게는
틀린 답이다. 고쳐진 이름은 다른 이름이고, 프로그램은 열라고 한 적 없는 파일을 연다.

**그래서 `utf.h`는 어디서나 엄격하다.** 잘못된 입력은 `PROVEN_ERR_INVALID_ENCODING`이고, 내 메모리에
쓰는 형태들은 거부할 때 아무것도 쓰지 않는다. 손실 모드는 없다.

**잘못된 것과 덜 온 것은 다른 답이다.** 조각으로 읽는 텍스트 - 파이프, 버퍼 쓰기 스트림(writer),
콘솔에서 오는 - 는 다음 조각이 완성할 문자의 한가운데서 끝날 수 있다. 부분 변환 형태는 거기서
`PROVEN_ERR_NEED_MORE`로 멈추고 입력을 얼마나 썼는지 알려 주므로, 꼬리를 간직했다가 더 받아 다시
시도하면 된다. 텍스트 전체를 받는 형태에는 기다릴 "다음"이 없으므로, 그쪽에서 문자 중간에 끝나는
텍스트는 잘못된 텍스트다.

세 가지 모양은 라이브러리의 나머지가 쓰는 그대로다: 재고 나서 내 버퍼로 변환(전부 아니면 전무),
소유(owned) 문자열을 늘리며 덧붙이기(전부 아니면 전무), 그리고 입력이나 출력이나 올바른 텍스트가
다할 때까지 온전한 문자만 변환하는 부분 변환. 부분 변환은 서로게이트 쌍의 반쪽이나 UTF-8 시퀀스의
일부를 절대 쓰지 않는다.

| API | 의도 | 반환 |
|---|---|---|
| `proven_utf8_to_utf16_size(src)` | `src`가 변환될 코드 유닛 수; 검증을 겸한다. | `proven_result_size_t` |
| `proven_utf16_to_utf8_size(src, n)` | 코드 유닛 `n`개가 변환될 UTF-8 바이트 수; 검증을 겸한다. | `proven_result_size_t` |
| `proven_utf8_to_utf16(src, out, out_cap, &written)` | `src` 전부를 `out`으로, 아니면 아무것도. 종결자 없음. | `proven_err_t` |
| `proven_utf16_to_utf8(src, n, out, out_cap, &written)` | 반대 방향으로 같은 일. | `proven_err_t` |
| `proven_utf8_to_utf16_partial(src, out, out_cap)` | 무언가가 멈출 때까지 온전한 문자만. | `proven_utf_step_t` |
| `proven_utf16_to_utf8_partial(src, n, out, out_cap)` | 반대 방향; 끝의 상위 서로게이트는 `NEED_MORE`. | `proven_utf_step_t` |
| `proven_utf8_append_to_u16str(alloc, dst, src)` | `src`의 UTF-16을 덧붙이며 `dst`를 늘린다; 전부 아니면 전무. | `proven_err_t` |
| `proven_utf16_append_to_u8str(alloc, dst, src, n)` | 코드 유닛 `n`개의 UTF-8을 덧붙이며 `dst`를 늘린다; 전부 아니면 전무. | `proven_err_t` |

```text
typedef struct {
    proven_err_t  err;       /* OK, OUT_OF_BOUNDS (output full), NEED_MORE (input ends
                                mid-character), INVALID_ENCODING (malformed at `consumed`) */
    proven_size_t consumed;  /* input units used: bytes for UTF-8, code units for UTF-16 */
    proven_size_t written;   /* output units written */
} proven_utf_step_t;

typedef enum {
    PROVEN_TEXT_UTF8, PROVEN_TEXT_UTF16LE, PROVEN_TEXT_UTF16BE,
    PROVEN_TEXT_AUTO         /* readers only: decided by a byte order mark */
} proven_text_encoding_t;    /* for the u16 writers and readers in stream.h (chapter 5) */
```

반례 - 바이트 하나를 코드 유닛 하나로 넓히는 경우. ASCII에서는 맞기 때문에 테스트를 통과해
살아남고, 다른 모든 문자는 의미 없는 유닛 두세 개가 된다. 라이브러리 자신의 `proven_time_u16_fmt`가
`utf.h`가 생기기 전까지 호출자의 비 ASCII 요일 이름에 정확히 이렇게 했다:

```text
for (size_t i = 0; i < text.size; ++i) {
    proven_u16 unit = (proven_u16)text.ptr[i];        /* wrong: a byte is not a code unit */
    (void)proven_u16str_append_grow(alloc, &wide, (proven_u16str_view_t){ &unit, 1 });
}
```

반례 - 조각을 전체 텍스트 형태로 변환하는 경우. 문자 한가운데서 끝나는 조각은 텍스트가 멀쩡한데도
잘못된 것으로 거부된다. 부분 변환 형태를 쓰고 꼬리를 이어 가져가라:

```text
proven_utf8_to_utf16(piece, out, cap, &n);   /* wrong for pieces: a split character is INVALID */
```

예제는 한국어 파일 이름을 와이드 API용으로 변환했다가 되돌리고, 아무것도 쓰지 않는 거부를 보이고,
소유 문자열을 양방향으로 늘리며, 두 조각 사이에서 잘린 문자를 이어 붙인다:

<!-- example: manual/examples/ko/ex_03_utf.c -->
```c
#include <string.h>

/*
 * 프로그램은 UTF-8 로 생각하고, 윈도의 "와이드" 호출은 UTF-16 을 원한다. utf.h 가
 * 양방향의 건널목이고, 엄격하다 - 올바른 UTF-8 이나 UTF-16 이 아닌 텍스트는 고쳐지지
 * 않고 거부된다. 잘못된 바이트를 슬쩍 물음표로 바꾸는 변환기는, 막 열려던 파일 이름을
 * 바꿔 버린 것이다.
 *
 * 이 라이브러리 어디서나처럼 세 가지 모양이 있다: 재고 나서 내 버퍼로 변환(전부 아니면
 * 전무), 소유 문자열을 늘리며 덧붙이기(전부 아니면 전무), 조각으로 도착하는 텍스트를
 * 위한 부분 변환.
 */

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    /* "보고서.txt" - 프로그램의 나머지가 들고 있는 모양 그대로의 파일 이름. */
    proven_u8str_view_t name = PROVEN_LIT("\xEB\xB3\xB4\xEA\xB3\xA0\xEC\x84\x9C.txt");

    /* --- 재고 나서, 내 버퍼로 변환 ------------------------------------------ */

    /* 먼저 묻는다: 답은 코드 단위이고, 검증도 겸한다. 한글 아홉 바이트는 여기서
     * 코드 단위 아홉 개가 아니라 세 개다. */
    proven_result_size_t need = proven_utf8_to_utf16_size(name);
    EXAMPLE_REQUIRE(proven_is_ok(need.err) && need.value == 7, "3 syllables + \".txt\" are 7 code units");

    /* 와이드 API 가 기대하는 NUL 자리로 하나 더; 변환기는 NUL 을 쓰지 않는다. */
    proven_u16 wide[16];
    proven_size_t units = 0;
    proven_err_t err = proven_utf8_to_utf16(name, wide, 15, &units);
    EXAMPLE_REQUIRE(proven_is_ok(err) && units == 7, "the name converts whole");
    wide[units] = 0;
    EXAMPLE_REQUIRE(wide[0] == 0xBCF4, "the first code unit is the syllable, not its first byte");
    /* 윈도라면: CreateFileW((LPCWSTR)wide, ...). 어디서나 돌도록 여기서는 부르지 않는다. */

    /* 그리고 되돌리기: 와이드 API 가 돌려준 이름을, 나머지 모두를 위해 UTF-8 로. */
    proven_result_size_t back = proven_utf16_to_utf8_size(wide, units);
    EXAMPLE_REQUIRE(proven_is_ok(back.err) && back.value == name.size, "the reverse size is the original byte count");
    proven_byte_t bytes[32];
    proven_size_t nbytes = 0;
    err = proven_utf16_to_utf8(wide, units, bytes, sizeof bytes, &nbytes);
    EXAMPLE_REQUIRE(proven_is_ok(err) && nbytes == name.size && memcmp(bytes, name.ptr, nbytes) == 0,
                    "the round trip gives the same bytes");

    /* --- 엄격: 잘못된 텍스트는 거부되고, 아무것도 쓰이지 않는다 -------- */

    /* 잘렸거나 잘못 해독된 이름에 있을 법한, 홀로 남은 연속 바이트. */
    proven_u8str_view_t broken = PROVEN_LIT("bad\x80.txt");
    wide[0] = 0x1234;
    err = proven_utf8_to_utf16(broken, wide, 15, &units);
    EXAMPLE_REQUIRE(err == PROVEN_ERR_INVALID_ENCODING, "a stray continuation byte is refused");
    EXAMPLE_REQUIRE(units == 0 && wide[0] == 0x1234, "and the output is untouched");

    /* --- 소유 문자열 늘리기 ------------------------------------------------ */

    proven_result_u16str_t r = proven_u16str_create(alloc, 4);
    EXAMPLE_REQUIRE(proven_is_ok(r.err), "creating the wide path must succeed");
    proven_u16str_t path = r.value;
    err = proven_utf8_append_to_u16str(alloc, &path, PROVEN_LIT("C:\\reports\\"));
    EXAMPLE_REQUIRE(proven_is_ok(err), "the directory appends");
    err = proven_utf8_append_to_u16str(alloc, &path, name);
    EXAMPLE_REQUIRE(proven_is_ok(err) && proven_u16str_len(&path) == 11 + 7, "and the name, growing the string");
    EXAMPLE_REQUIRE(proven_u16str_as_ptr(&path)[18] == 0, "still NUL-terminated for the system call");

    /* 실패한 덧붙이기는 아무것도 바꾸지 않는다: 반쯤 변환된 꼬리가 없다. */
    err = proven_utf8_append_to_u16str(alloc, &path, broken);
    EXAMPLE_REQUIRE(err == PROVEN_ERR_INVALID_ENCODING && proven_u16str_len(&path) == 18, "a refused append leaves the path as it was");

    proven_result_u8str_t r8 = proven_u8str_create(alloc, 8);
    EXAMPLE_REQUIRE(proven_is_ok(r8.err), "creating the UTF-8 copy must succeed");
    proven_u8str_t copy = r8.value;
    err = proven_utf16_append_to_u8str(alloc, &copy, proven_u16str_as_ptr(&path), proven_u16str_len(&path));
    EXAMPLE_REQUIRE(proven_is_ok(err) && copy.internal.len == 11 + name.size, "the whole path comes back as UTF-8");

    /* --- 부분 변환: 조각으로 도착하는 텍스트 ------------------------------- */

    /* 읽기 스트림이 4 바이트씩 넘겨 주고, "고" 는 첫 바이트 뒤에서 잘린다. 부분 변환은
     * 온전한 것만 변환하고 NEED_MORE 로 멈추며, 얼마나 썼는지 알려 준다. 호출자는
     * 나머지를 간직했다가 다음 조각을 붙인다. */
    proven_u16 out[8];
    proven_utf_step_t st = proven_utf8_to_utf16_partial((proven_u8str_view_t){ name.ptr, 4 }, out, 8);
    EXAMPLE_REQUIRE(st.err == PROVEN_ERR_NEED_MORE && st.consumed == 3 && st.written == 1,
                    "one whole syllable, and the start of the next kept back");
    st = proven_utf8_to_utf16_partial((proven_u8str_view_t){ name.ptr + st.consumed, 6 }, out + 1, 7);
    EXAMPLE_REQUIRE(proven_is_ok(st.err) && st.written == 2 && out[1] == 0xACE0, "with the next piece it completes");

    /* 반대 방향에서도 같은 구분이다: 끝의 상위 서로게이트는, 나머지 반쪽이 다음
     * 조각에 있는 쌍의 반쪽일 수 있다. */
    const proven_u16 emoji_half[] = { 'o', 'k', 0xD83D };
    proven_byte_t u8out[16];
    st = proven_utf16_to_utf8_partial(emoji_half, 3, u8out, sizeof u8out);
    EXAMPLE_REQUIRE(st.err == PROVEN_ERR_NEED_MORE && st.consumed == 2, "a trailing high surrogate waits");

    printf("converted a %zu-byte name to %zu code units and back\n", (size_t)name.size, (size_t)need.value);

    proven_u8str_destroy(alloc, &copy);
    proven_u16str_destroy(alloc, &path);
    return EXAMPLE_OK();
}
```

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
| `proven_arg_u16(v)` | borrowed UTF-16 view, UTF-8로 렌더링된다. 엄격: 짝 없는 서로게이트는 포맷 전체를 실패시킨다. 너비는 u8 view와 같이 UTF-8 바이트로 센다. `PROVEN_ARG`는 `proven_u16str_view_t`에 이것을 고른다. |
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
| `proven_u8str_append_fmt_with_scratch(alloc, str, fmt, scratch, ...)` | 임시 패치 할당에 임시 작업용(scratch) allocator를 쓰는 성장 가능 포매팅. |
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
- 현재 변환 스택은 `Clinger fast path -> direct Eisel-Lemire -> staged
  Eisel-Lemire layer -> exact bigint fallback`이며, 대표적인 입력이 어느 경로를
  탔는지 테스트가 확인할 수 있도록 내부 카운터를 둔다.
- direct Eisel-Lemire 계층은 유효숫자에 128비트로 자른 5의 거듭제곱을 곱하고, 곱의
  하위 비트로 반올림을 증명한다. 증명할 수 없는 것(동률일 수 있는 값, 비정규 또는
  오버플로 결과)은 넘긴다. 19자리보다 긴 유효숫자는 앞 19자리와 거기에 1을 더한 값으로
  위아래를 묶고, 두 경계가 같게 반올림될 때 그 값을 쓴다.
- staged Eisel-Lemire 계층은 현재 생성된 `5^q` 양수 지수 경우와, 십진 유효숫자가
  요구되는 `5^q`를 깔끔하게 상쇄하는 정확한 음수 지수 경우를 받아들인다.
- 모든 대상에서(128비트 정수 타입이 필요 없다) 같은 계층이 정상 범위 경우에 대해 보수적으로
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

<!-- example: manual/examples/ko/ex_03_u8str.c -->
```c
/*
 * 여기 문자열 손잡이가 둘 있고, 둘을 가르는 것은 크기가 아니라 소유다.
 *
 *   proven_u8str_t      - 고칠 수 있는 바이트 문자열. 할당을 소유하거나(create)
 *                         여러분의 것을 빌린다(borrow).
 *   proven_u8str_view_t - 남의 바이트를 가리키는 포인터와 길이. 아무것도 소유하지
 *                         않고, NUL 로 끝나지 않으며, 그 바이트가 살아 있는 동안에만
 *                         쓸 수 있다.
 *
 * 읽기만 하는 함수에 건네는 것이 뷰이고, 손에 쥐고 있는 것이 u8str 다.
 */

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    /* --- *소유하는* 문자열: 할당자의 기억, 여러분이 지울 것 ---------------- */
    /* 용량 인자는 내용 바이트 수다. NUL 은 그 밖에 따로 있어서, as_cstr 은 언제나
     * O(1) 이고 언제나 안전하다. */
    proven_result_u8str_t r = proven_u8str_create(alloc, 16);
    EXAMPLE_REQUIRE(proven_is_ok(r.err), "creating a 16-byte string must succeed");
    if (!proven_is_ok(r.err)) {
        return 1;
    }
    proven_u8str_t path = r.value;

    /* append 는 용량이 고정이다. 들어가거나 실패하고, 실패했다면 문자열에 손도 대지
     * 않았다. 재할당하지 않으므로 할당자도 필요 없다. */
    proven_err_t err = proven_u8str_append(&path, PROVEN_LIT("/etc/hosts"));
    EXAMPLE_REQUIRE(proven_is_ok(err), "10 bytes fit in a 16-byte string");

    /* append_grow 는 늘어나는 쌍둥이다. 문자열을 만들 때 쓴 할당자를 주면 필요할 때
     * 재할당한다. 여전히 실패 원자적이다 - 할당이 실패하면 문자열은 그대로다. */
    err = proven_u8str_append_grow(alloc, &path, PROVEN_LIT(".backup.original"));
    EXAMPLE_REQUIRE(proven_is_ok(err), "append_grow must reallocate rather than fail");

    /* 가운데를 고치기. insert 는 꼬리를 오른쪽으로, remove 는 왼쪽으로 민다. */
    err = proven_u8str_insert_grow(alloc, &path, 0, PROVEN_LIT("/srv"));
    EXAMPLE_REQUIRE(proven_is_ok(err), "inserting a prefix must succeed");

    err = proven_u8str_remove(&path, proven_u8str_as_view(&path).size - 9, 9);  /* ".original" 을 떼어 낸다 */
    EXAMPLE_REQUIRE(proven_is_ok(err), "removing the trailing suffix must succeed");

    /* 찾는 것이 없으면 replace_first 는 PROVEN_OK 를 돌려준다 - "할 일이 없다" 는
     * 오류가 아니다. 그 차이가 중요하면 먼저 찾아볼 것. */
    err = proven_u8str_replace_first(&path, 0, PROVEN_LIT("hosts"), PROVEN_LIT("fstab"));
    EXAMPLE_REQUIRE(proven_is_ok(err), "replacing an existing substring must succeed");

    /* --- 읽기: 복사하지 말고 뷰를 빌릴 것 ---------------------------------- */
    /* as_view 는 공짜다. 그 뷰는 다음 편집 전까지만 쓸 수 있다. 늘리는 호출은 재할당할
     * 수 있고, 그러면 뷰(그리고 cstr)는 매달린 포인터가 된다. */
    proven_u8str_view_t v = proven_u8str_as_view(&path);

    EXAMPLE_REQUIRE(proven_u8str_view_eq(v, PROVEN_LIT("/srv/etc/fstab.backup")),
                    "the edits above should have produced /srv/etc/fstab.backup");
    EXAMPLE_REQUIRE(proven_u8str_view_starts_with(v, PROVEN_LIT("/srv")),
                    "the inserted prefix is at the front");

    proven_size_t dot = proven_u8str_view_find(v, 0, PROVEN_LIT(".backup"));
    EXAMPLE_REQUIRE(dot != PROVEN_INDEX_NOT_FOUND, "the suffix must be found");

    /* 슬라이스는 *같은* 바이트를 가리키는 뷰다 - 할당도 복사도 없다. */
    proven_u8str_view_t stem = proven_u8str_view_slice(v, 0, dot);
    EXAMPLE_REQUIRE(proven_u8str_view_eq(stem, PROVEN_LIT("/srv/etc/fstab")),
                    "slicing at the suffix leaves the stem");

    /* as_cstr 은 C API 로 나가는 비상구이고, 소유한 문자열이 길이 뒤에 NUL 을 지키기
     * 때문에만 옳다. 뷰로는 이렇게 하지 *말 것*. `stem.ptr` 은 NUL 로 끝나지 않는다 -
     * 그냥 `path` 안을 가리킬 뿐이다. */
    printf("owned:  %s\n", proven_u8str_as_cstr(&path));

    /* --- *빌린* 문자열: 여러분의 기억, 할당은 전혀 없다 -------------------- */
    /* 타입도 연산도 같다 - 다만 바이트가 이 스택 버퍼다. `cap` 은 NUL 을 포함하므로
     * 내용은 31 바이트까지 담긴다. */
    proven_byte_t line[32];
    proven_u8str_t status = proven_u8str_borrow(line, sizeof line);

    err = proven_u8str_append(&status, PROVEN_LIT("mounted "));
    EXAMPLE_REQUIRE(proven_is_ok(err), "appending into a borrowed buffer needs no allocator");
    err = proven_u8str_append(&status, stem);
    EXAMPLE_REQUIRE(proven_is_ok(err), "a view can be appended just like a literal");

    /* 빌린 문자열에도 늘리는 호출은 있지만, 자기 것이 아닌 기억을 재할당하지는
     * 않는다. 자료가 넘치면 OUT_OF_BOUNDS 이고 `line` 은 손대지 않은 채로 남는다.
     * 빌린 문자열이 등 뒤에서 조용히 힙으로 달아나는 일은 없다. */
    err = proven_u8str_append_grow(alloc, &status,
                                   PROVEN_LIT(" ...and a great deal more text than fits"));
    EXAMPLE_REQUIRE(err == PROVEN_ERR_OUT_OF_BOUNDS,
                    "a borrowed string reports overflow instead of reallocating caller memory");
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_as_view(&status), PROVEN_LIT("mounted /srv/etc/fstab")),
                    "the failed append must have left the string unchanged");

    printf("borrowed: %s\n", proven_u8str_as_cstr(&status));

    /* reset 은 비어 있는 상태로 자르고 버퍼는 그대로 둔다. 그래서 다음 프레임이 같은
     * 32 바이트를 할당 없이 다시 쓴다. */
    err = proven_u8str_reset(&status);
    EXAMPLE_REQUIRE(proven_is_ok(err), "reset must succeed on a borrowed string");
    EXAMPLE_REQUIRE(proven_u8str_as_view(&status).size == 0, "reset empties the string");

    /* --- destroy: 소유 규칙을 그대로 적어 보면 ------------------------------ */
    /* 빌린 문자열에 대한 destroy 는 아무 일도 하지 않는다 - `line` 은 라이브러리가
     * 해제할 것이 아니다. 그래도 부르는 것이 옳고 값도 들지 않으며, 덕분에 뒷정리 코드는
     * 자기가 쥔 문자열이 어느 쪽인지 몰라도 된다. */
    proven_u8str_destroy(alloc, &status);

    /* 소유한 문자열에 대한 destroy 는 할당을 해제한다. 그리고 반드시 그 문자열을 만들
     * 때 쓴 할당자를 주어야 한다. */
    proven_u8str_destroy(alloc, &path);
    return EXAMPLE_OK();
}
```

### 실전 예제: 버퍼는 고정인데 데이터는 그렇지 않을 때

앞의 예제는 자리가 모자라면 문자열을 늘린다. 실제 코드의 상당 부분은 그럴 수 없다. 레코드에는 고정
폭 필드가 있고, 로그 한 줄에는 상한이 있으며, 아레나(arena) 위에 놓인 문자열은 재할당할 때마다 옛 사본을
다음 reset까지 남긴다. 그런 호출자는 데이터가 들어가지 않을 때 호출이 주는 세 가지 답 중 어느
것인지를 알아야 한다.

| 호출의 종류 | 데이터가 들어가지 않을 때 하는 일 | 해당 호출 |
|---|---|---|
| **원자적(atomic), 고정 용량** | `PROVEN_ERR_OUT_OF_BOUNDS`로 거절하고 아무것도 바꾸지 않는다. | `proven_u8str_append`, `proven_u8str_insert`, `proven_u8str_replace_at` |
| **최선 노력(best-effort, 잘라 쓰기)** | 들어가는 만큼 쓰고, `PROVEN_ERR_OUT_OF_BOUNDS`와 **함께** 실제로 쓴 바이트 수를 돌려준다. | `proven_u8str_append_partial` |
| **원자적, 확장 가능** | allocator를 통해 재할당한다. 할당이 실패하면 아무것도 바꾸지 않는다. | `proven_u8str_append_grow`, `proven_u8str_replace_at_grow`, `proven_u8str_append_byte` |

보조 호출 둘도 여기 나온다. `proven_u8str_reserve()`는 용량을 미리 한 번에 올려 두어 이후의 증가가
재할당을 부르지 않게 한다. 힙(heap)에서는 복사를 아끼고, arena에서는 재할당이 남기는 죽은 저장
공간을 아낀다. `proven_u8str_is_valid()`는 문자열 핸들 자체의 필드가 서로 모순되지 않는지 본다.
문자열이 다른 코드에서 넘어오는 경계에서 한 번 확인할 값어치가 있고, 편집할 때마다 되풀이할 검사는
아니다.

아래 프로그램은 상한이 있는 로그 한 줄을 쓰고, 넘치는 append를 거절당하고, 최선 노력 호출로 일부러
잘라 내고, `proven_u8str_replace_at()`으로 경로 가운데를 고친다 — 먼저 짧아지는 방향(언제나
들어간다), 다음에 길어지는 방향(들어가지 않아 거절된다), 그리고 같은 편집을
`proven_u8str_replace_at_grow()`로 다시 한다. 마지막은 `proven_u8str_view_ends_with()`로 파일
확장자를 확인하는 것이다.

<!-- example: manual/examples/ko/ex_03_fixed_edits.c -->
```c
/*
 * 앞의 예제는 자리가 모자라면 문자열을 늘렸다. 이번 것은 늘리는 것이 허용되지 않는
 * 경우 - 크기가 고정된 레코드, 길이 상한이 단단한 로그 줄, 재할당하면 안 되는 아레나
 * 속 버퍼 - 그리고 자료가 들어가지 않을 때 호출이 줄 수 있는 두 가지 정직한 답에 대한
 * 것이다.
 *
 *   "안 됩니다, 그리고 아무것도 바꾸지 않았습니다" - 원자적 호출들: append, insert,
 *                                      replace_at. 먼저 용량을 검사하므로, 거부는
 *                                      문자열을 그대로 남긴다.
 *   "일부만 했고, 얼마나 했는지 알려 드립니다" - 최선 노력 호출: append_partial.
 *                                      담을 수 있는 만큼 담고 몇 바이트를 썼는지
 *                                      알려 준다.
 *
 * 둘 다 쓸모가 있다. 잘못 고르면 레코드를 조용히 자르거나 조용히 버린다. `_grow` 짝은
 * 세 번째 답 - "네, 자리를 더 찾았습니다" - 이고, 견주어 보라고 끝에 실었다.
 */

#define FIELD_CAP 32u

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    proven_result_u8str_t r = proven_u8str_create(alloc, FIELD_CAP);
    EXAMPLE_REQUIRE(proven_is_ok(r.err), "creating the field buffer must succeed");
    if (!proven_is_ok(r.err)) {
        return 1;
    }
    proven_u8str_t field = r.value;

    /* is_valid 는 손잡이 자신의 구조를 검사한다 - 포인터와, 서로 어긋나지 않는 용량과
     * 길이. 문자열이 다른 데서 넘어올 때 여러분 코드의 경계에서 한 번 단언해 둘 값어치가
     * 있다. 편집할 때마다 할 검사는 아니다. 모든 편집이 그 성질을 지키기 때문이다. */
    EXAMPLE_REQUIRE(proven_u8str_is_valid(&field), "a freshly created string must be structurally valid");

    /* --- 미리 자리를 잡아 두기 -------------------------------------------- */

    /* reserve 는 용량을 지금 올려 두어 나중의 성장이 재할당하지 않게 한다. 힙에서는
     * 복사를 아끼고, 아레나에서는 더 나쁜 것을 아낀다. 거기서는 재할당마다 옛 블록이
     * 다음 reset 까지 새기 때문이다. 필요할 만큼을, 한 번에 청할 것. */
    proven_err_t err = proven_u8str_reserve(alloc, &field, 64);
    EXAMPLE_REQUIRE(proven_is_ok(err), "reserving 64 bytes must succeed");
    EXAMPLE_REQUIRE(field.internal.cap >= 64, "the capacity must actually be at least what was asked for");

    /* --- 원자적 호출: 들어가거나, 아무것도 바꾸지 않거나 -------------------- */

    err = proven_u8str_append(&field, PROVEN_LIT("2026-01-01 level=info "));
    EXAMPLE_REQUIRE(proven_is_ok(err), "the prefix fits in the reserved capacity");

    /* append_byte 는 한 바이트를 더한다. 구분자, 종결자, 이스케이프 문자가 그런
     * 것들이다. 늘리는 호출이라 할당자를 받는다 - 한 바이트야말로 생각지 못한 경계에서
     * 용량 검사가 걸리는 바로 그 경우다. */
    err = proven_u8str_append_byte(alloc, &field, (proven_u8)'[');
    EXAMPLE_REQUIRE(proven_is_ok(err), "appending a single separator byte must succeed");

    err = proven_u8str_append(&field, PROVEN_LIT("disk full"));
    EXAMPLE_REQUIRE(proven_is_ok(err), "the message fits");

    err = proven_u8str_append_byte(alloc, &field, (proven_u8)']');
    EXAMPLE_REQUIRE(proven_is_ok(err), "closing the bracket must succeed");

    /* 이제 용량이 담을 수 있는 것보다 많이 청해 본다. 원자적 append 는 거부하고 -
     * 이것이 믿고 기댈 값어치가 있는 성질이다 - 문자열은 호출 전에 담고 있던 것을
     * 그대로 담고 있다. */
    proven_size_t before = proven_u8str_as_view(&field).size;
    err = proven_u8str_append(&field, PROVEN_LIT(" and a very long trailing explanation that certainly does not fit"));
    EXAMPLE_REQUIRE(err == PROVEN_ERR_OUT_OF_BOUNDS, "an oversized atomic append must be refused");
    EXAMPLE_REQUIRE(proven_u8str_as_view(&field).size == before, "and must leave the string untouched");

    /* --- 최선 노력 호출: 들어가는 만큼, 그리고 그 개수 --------------------- */

    /* 보고서의 폭 고정 열이 이것을 쓰는 경우다. 들어가는 만큼 쓰고, 얼마나 썼는지 알아
     * 두어 부르는 쪽이 그 값을 온전한 척하는 대신 잘렸다고 표시할 수 있게 한다. */
    proven_result_size_t part = proven_u8str_append_partial(&field, PROVEN_LIT(" ...more text than there is room for"));
    EXAMPLE_REQUIRE(part.err == PROVEN_ERR_OUT_OF_BOUNDS, "a partial append that truncates still reports the truncation");
    EXAMPLE_REQUIRE(part.value > 0, "but it wrote what it could");
    EXAMPLE_REQUIRE(proven_u8str_as_view(&field).size == before + part.value,
                    "and the string grew by exactly the number of bytes it reports");
    printf("partial append wrote %zu byte(s) before the buffer was full\n", (size_t)part.value);

    /* --- 늘리지 않고 가운데를 고치기 --------------------------------------- */

    proven_result_u8str_t r2 = proven_u8str_create(alloc, FIELD_CAP);
    EXAMPLE_REQUIRE(proven_is_ok(r2.err), "creating the second buffer must succeed");
    proven_u8str_t path = r2.value;
    err = proven_u8str_append(&path, PROVEN_LIT("var/log/service.log"));
    EXAMPLE_REQUIRE(proven_is_ok(err), "the path fits");

    /* insert 는 꼬리를 오른쪽으로 민다. 용량 고정이다 - 들어가거나 거부한다. */
    err = proven_u8str_insert(&path, 0, PROVEN_LIT("/"));
    EXAMPLE_REQUIRE(proven_is_ok(err), "inserting a leading slash must succeed");
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_as_view(&path), PROVEN_LIT("/var/log/service.log")),
                    "the insert lands at index 0");

    /* replace_at 은 어떤 자리의 old_len 바이트를 길이에 상관없는 자료로 바꾼다.
     * 결과가 여전히 들어가기만 하면 된다. "service"(7)를 "daemon"(6)으로 바꾸면 문자열이
     * 짧아지므로 용량 때문에 실패할 수 없다. */
    proven_size_t at = proven_u8str_view_find(proven_u8str_as_view(&path), 0, PROVEN_LIT("service"));
    EXAMPLE_REQUIRE(at != PROVEN_SIZE_MAX, "the substring must be found before it can be replaced");
    err = proven_u8str_replace_at(&path, at, 7, PROVEN_LIT("daemon"));
    EXAMPLE_REQUIRE(proven_is_ok(err), "a shortening replacement must succeed");
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_as_view(&path), PROVEN_LIT("/var/log/daemon.log")),
                    "and produce the expected path");

    /* 같은 편집을 거꾸로 하면 32바이트 버퍼를 넘치고, 용량 고정 호출은 경로를 자르는
     * 대신 거부한다 - 자르는 쪽이 바로 엉뚱한 파일에 조용히 쓰게 되는 그 실패다. */
    before = proven_u8str_as_view(&path).size;
    err = proven_u8str_replace_at(&path, at, 6, PROVEN_LIT("a-replacement-name-far-too-long-for-this-buffer"));
    EXAMPLE_REQUIRE(err == PROVEN_ERR_OUT_OF_BOUNDS, "a replacement that does not fit must be refused");
    EXAMPLE_REQUIRE(proven_u8str_as_view(&path).size == before, "and must leave the path unchanged");

    /* replace_at_grow 는 재할당을 허락받은 같은 편집이다. 버퍼가 힙에 있고 길이가 정말
     * 한정되지 않았을 때 쓸 것. 한계가 형식의 일부라면 용량 고정 호출을 고를 것. */
    err = proven_u8str_replace_at_grow(alloc, &path, at, 6, PROVEN_LIT("a-replacement-name-far-too-long-for-this-buffer"));
    EXAMPLE_REQUIRE(proven_is_ok(err), "the growing variant makes room instead of refusing");
    EXAMPLE_REQUIRE(proven_u8str_view_ends_with(proven_u8str_as_view(&path), PROVEN_LIT(".log")),
                    "the extension is still at the end after the edit");

    /* ends_with 는 확장자 검사가 실제로 던지는 물음에 답한다. 손으로 셈한 인덱스로 하면
     * off-by-one 이 사는 자리가 되고, 포인터에 strcmp 로 하면 뷰에는 없는 NUL 이
     * 필요해진다. */
    EXAMPLE_REQUIRE(!proven_u8str_view_ends_with(proven_u8str_as_view(&path), PROVEN_LIT(".txt")),
                    "and it is not a .txt file");

    /* 빈 접미사는 무엇의 접미사이기도 하다. 접미사 목록을 도는 반복문이 특별한 경우를
     * 두지 않아도 되게 해 주는 답이다. */
    EXAMPLE_REQUIRE(proven_u8str_view_ends_with(proven_u8str_as_view(&path), PROVEN_LIT("")),
                    "every string ends with the empty suffix");

    printf("log line: %s\n", proven_u8str_as_cstr(&field));
    printf("path:     %s\n", proven_u8str_as_cstr(&path));

    proven_u8str_destroy(alloc, &path);
    proven_u8str_destroy(alloc, &field);
    return EXAMPLE_OK();
}
```

반례 — 최선 노력 호출을 원자적 호출처럼 다루는 경우:

```text
(void)proven_u8str_append_partial(&line, field);   /* wrong: ignores the count */
```

잘렸다는 사실이 보고되는 곳은 반환값뿐이다. 그것을 버리면 "레코드가 잘렸다"가 "레코드가 멀쩡해
보인다"로 바뀐다.

반례 — 바이트 수를 글자 수로 읽는 경우:

```text
if (part.value == field.size) { /* all of it was written */ }
```

이 비교는 옳고, **바이트** 단위로 옳다. UTF-8 한 글자는 최대 4바이트이므로 최선 노력 append는 글자
중간에서 멈출 수 있다. 뒤끝이 유효한 UTF-8이어야 한다면 자를 지점을 용량이 아니라 여러분이 정한다.

### 실전 예제: 시스템 호출에 넘길 UTF-16 문자열 조립하기

`proven_u16str_t`가 제 몫을 하는 곳은 운영체제 호출이 UTF-16을 요구하는 경계뿐이다. 보통은 윈도의
와이드(wide) API가 그 이유다. 방식은 언제나 같다. 코드 단위(code unit)를 모은 다음
`proven_u16str_as_ptr()`를 그 호출에 넘긴다.

이 코드가 옳은지는 세 가지가 정한다.

- **단위는 코드 단위이지 바이트도 글자도 아니다.** 용량 32는 코드 단위 32개, 곧 64바이트다. 기본
  다국어 평면(Basic Multilingual Plane, BMP) 밖의 글자 — 이모지, 드문 한중일 한자 다수 — 는 두
  개를 차지한다.
- **`proven_u16str_as_ptr()`는 복사하지 않는다.** 그 포인터는 문자열을 늘리는 다음 append 전까지만
  유효하다.
- **결과는 NUL로 끝난다.** 일부러 잘라 낸 뒤에도 그렇기 때문에, 종결자를 기대하는 시스템 호출에
  그대로 넘겨도 안전하다.

<!-- example: manual/examples/ko/ex_03_u16str.c -->
```c
/*
 * UTF-16 이 이 라이브러리에 있는 이유는 하나다. 어떤 운영체제 호출은 그것만 받는다.
 * 윈도의 "와이드" API 가 늘 그 경우다 - CreateFileW 에 건네는 파일 이름은 바이트가
 * 아니라 NUL 로 끝나는 16비트 코드 단위의 줄이다.
 *
 * 그래서 이 타입이 하는 일은 좁다. 코드 단위를 모으고, 개수를 맞게 지키고, 시스템
 * 호출이 원하는 포인터를 내주는 것. 프로그램의 나머지는 UTF-8 로 두어야 한다.
 *
 * 하나만 헷갈리지 말 것 - 단위다. 여기서 용량 32 는 코드 단위 32개, 곧 64 바이트이고,
 * 기본 다국어 평면 밖의 문자는 - 이모지, 드문 한자 대부분 - 두 개를 쓴다. 코드 단위의
 * 개수는 문자의 개수가 아니고, 한 번도 그랬던 적이 없다.
 */

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    /* 인자는 바이트 한계가 아니라 코드 단위 한계다. */
    proven_result_u16str_t r = proven_u16str_create(alloc, 32);
    EXAMPLE_REQUIRE(proven_is_ok(r.err), "creating a 32-code-unit string must succeed");
    if (!proven_is_ok(r.err)) {
        return 1;
    }
    proven_u16str_t name = r.value;
    EXAMPLE_REQUIRE(proven_u16str_len(&name) == 0, "a new string is empty");

    /* PROVEN_U16_LIT 은 u"..." 리터럴에서 뷰를 만들고 단위 개수를 리터럴 자체에서
     * 계산한다. 그래서 개수가 글과 어긋날 수 없다. */
    proven_err_t err = proven_u16str_append(&name, PROVEN_U16_LIT("C:\\logs\\"));
    EXAMPLE_REQUIRE(proven_is_ok(err), "the directory prefix fits in 32 code units");

    err = proven_u16str_append(&name, PROVEN_U16_LIT("service.log"));
    EXAMPLE_REQUIRE(proven_is_ok(err), "the file name fits too");
    EXAMPLE_REQUIRE(proven_u16str_len(&name) == 8 + 11, "the length is a count of code units");

    /* 바이트 문자열 쌍둥이처럼 원자적이다. 자료가 넘치면 거부되고 문자열은 그대로
     * 남는다. 그래서 경로가 반쯤 적히는 일이 없다. */
    proven_size_t before = proven_u16str_len(&name);
    err = proven_u16str_append(&name, PROVEN_U16_LIT(".a-suffix-long-enough-to-overflow-the-capacity"));
    EXAMPLE_REQUIRE(err == PROVEN_ERR_OUT_OF_BOUNDS, "an oversized append must be refused");
    EXAMPLE_REQUIRE(proven_u16str_len(&name) == before, "and must not truncate the path");

    /* --- 시스템 호출이 원하는 포인터 -------------------------------------- */

    /* as_ptr 은 내부의 코드 단위를 NUL 로 끝난 채로, 복사 없이 돌려준다. 호출 직전의
     * 마지막 걸음이고, 그 포인터는 다음 append 전까지만 쓸 수 있다. 늘리는 append 는
     * 저장소를 옮길 수 있다. */
    const proven_u16 *wide = proven_u16str_as_ptr(&name);
    EXAMPLE_REQUIRE(wide != NULL, "an assembled string must yield a pointer");
    EXAMPLE_REQUIRE(wide[0] == (proven_u16)'C', "the first code unit is the drive letter");
    EXAMPLE_REQUIRE(wide[proven_u16str_len(&name)] == 0, "the sequence is NUL-terminated for the system call");
    /* 윈도에서는 이것이 이 타입의 존재 이유 전부다.
     *     HANDLE h = CreateFileW((LPCWSTR)wide, ...);
     * 여기서는 부르지 않는다. 이 예제는 다른 모든 곳에서도 돌아야 하기 때문이다. */

    /* --- 자르는 것이 옳은 답일 때 ----------------------------------------- */

    /* 어떤 시스템 구조체에는 폭이 고정된 필드가 있다 - 이를테면 16 단위짜리 이름표 -
     * 거기서는 들어가지 않는 이름을 거부하는 것이 아니라 자르는 것이 뜻이다. 부분
     * append 가 그것을 위한 것이다. 담을 수 있는 만큼 담고 몇 단위를 썼는지 알려 주어,
     * 부르는 쪽이 그 값을 잘렸다고 표시할 수 있게 한다. */
    proven_result_u16str_t r2 = proven_u16str_create(alloc, 16);
    EXAMPLE_REQUIRE(proven_is_ok(r2.err), "creating the fixed-width label must succeed");
    proven_u16str_t label = r2.value;

    proven_result_size_t wrote = proven_u16str_append_partial(&label, PROVEN_U16_LIT("a-label-that-is-longer-than-the-field"));
    EXAMPLE_REQUIRE(wrote.err == PROVEN_ERR_OUT_OF_BOUNDS, "the truncation is reported, not hidden");
    EXAMPLE_REQUIRE(wrote.value == 16, "it filled the field exactly");
    EXAMPLE_REQUIRE(proven_u16str_len(&label) == wrote.value, "and the length matches what it says it wrote");
    EXAMPLE_REQUIRE(proven_u16str_as_ptr(&label)[wrote.value] == 0,
                    "a truncated string is still NUL-terminated, so it is still safe to pass on");

    printf("assembled %zu code unit(s); label truncated to %zu\n",
           (size_t)proven_u16str_len(&name), (size_t)wrote.value);

    proven_u16str_destroy(alloc, &label);
    proven_u16str_destroy(alloc, &name);
    return EXAMPLE_OK();
}
```

반례 — UTF-16 버퍼 크기를 바이트로 잡는 경우:

```text
proven_result_u16str_t r = proven_u16str_create(alloc, sizeof(buf));   /* wrong */
```

인자는 코드 단위의 개수다. 바이트 수를 넘기면 의도한 것의 두 배를 요구하게 되고, 그 바이트 수가
UTF-8 문자열에서 왔다면 변환 결과를 담지도 못하는 버퍼를 요구하게 된다.

반례 — append를 사이에 두고 포인터를 붙들고 있는 경우:

```text
const proven_u16 *w = proven_u16str_as_ptr(&name);
proven_err_t e = proven_u16str_append_grow(alloc, &name, more);   /* may reallocate */
use_wide(w);   /* wrong: w may dangle */
```

포인터는 그것을 소비하는 호출 바로 앞에서 얻는다.
