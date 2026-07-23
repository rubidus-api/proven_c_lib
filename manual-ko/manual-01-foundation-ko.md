# 1장: 기초 — 에러, 타입, 메모리 view

**2부 — 모든 프로그램이 사용하는 어휘. 선행 조건: [0장](manual-00-start-here-ko.md).**
**이 장을 마치면** 이 라이브러리가 보고하는 모든 실패를 처리할 수 있고, 크기를 잃어버리지 않으면서
메모리 영역을 기술할 수 있으며, 조용히 감기지 않는 크기 산술을 할 수 있다.

이 장은 `types.h`, `error.h`, `memory.h`, `align.h`, `version.h`, `panic.h`를 다룬다 — 다른 모든
장이 그 위에 세워지는 조각들이다. 여기에는 할당하는 것이 아무것도 없고, 운영체제와 이야기하는 것도
아무것도 없다. 그러면서도 진짜로 반드시 읽어야 하는, 가장 짧은 장이다.

## 목차

1. [에러는 값이다](#1-에러는-값이다)
2. [타입, 그리고 왜 표기가 다른가](#2-타입-그리고-왜-표기가-다른가)
3. [메모리 view: 포인터와 길이를 하나로](#3-메모리-view-포인터와-길이를-하나로)
4. [감길 수 없는 크기 산술](#4-감길-수-없는-크기-산술)
5. [정렬](#5-정렬)
6. [Panic: 에러를 돌려줄 상대가 남지 않았을 때](#6-panic-에러를-돌려줄-상대가-남지-않았을-때)
7. [버전 매크로](#7-버전-매크로)
8. [예제와 오용 사례](#8-예제와-오용-사례)

## 1. 에러는 값이다

### 왜 이것이 맨 앞에 오는가

이 매뉴얼의 나머지 모든 페이지는 여기서 실패가 어떻게 보고되는지 여러분이 안다고 가정한다. 그러니
이 절은 건너뛸 수 없는 유일한 절이다.

C는 함수가 실패를 어떻게 보고해야 하는지에 대해 스스로와 합의한 적이 한 번도 없다. `malloc`은
`NULL`을 반환한다. `fopen`도 `NULL`을 반환한다. `read`는 `-1`을 반환한다. `strtol`은 `0`을
반환하고 *어쩌면* `errno`를 설정하는데, 그것도 여러분이 먼저 `errno`를 지웠을 때에만 그렇다.
성공한 호출도 거기에 쓰레기를 남겨 두는 것이 허용되기 때문이다. `printf`는 거의 아무도 확인하지
않는 음수를 반환한다. 네 가지 관례가 있고, 이들이 공유하는 유일한 성질은 **무시해도 조용하고
합법이라는 것**이다:

```text
char *p = malloc(n);
p[0] = 'x';                    /* wrong: malloc returns NULL on failure */

FILE *f = fopen(path, "r");
fread(buf, 1, n, f);           /* wrong: f may be NULL */

long v = strtol(s, NULL, 10);  /* wrong: 0 could be the value or the failure */
```

더 깊은 문제는 이것들이 틀리기 쉽다는 데 있지 않다. **타입이 무언가 일어날 수 있다는 사실을 전혀
말해 주지 않는다는 것**이 문제다. `char *`는 `NULL`이 될 수 있든 없든 똑같은 타입이므로, 그
무엇도 — 컴파일러도, 대충 훑어보는 리뷰어도, 새벽 두 시의 여러분도 — 확인하라는 신호를 받지 못한다.

`errno`는 에러를 *전역이면서 일시적인* 것으로 만들어 상황을 더 나쁘게 한다. 정확히 알맞은 순간에
읽어야 하고, 중간에 끼어든 어떤 라이브러리 호출이든 그것을 덮어쓸 수 있으며, 스레드마다 따로
존재하므로 한 부류의 버그를 고치려다 또 다른 부류를 열어젖혔다.

### 이 라이브러리가 대신 하는 일

에러는 반환값이며, 자기만의 타입을 가진다.

함수의 결과가 성공 아니면 실패뿐일 때에는 `proven_err_t`를 반환한다 — `PROVEN_OK`가 `0`인 평범한
열거형이다:

```text
proven_err_t err = proven_u8str_append(&s, PROVEN_LIT("hello"));
if (!proven_is_ok(err)) { /* handle it */ }
```

함수가 돌려줄 값이 있을 때에는 값과 에러가 하나의 구조체 안에 함께 실려 오며, **에러를 확인하기
전까지 그 값은 아무 의미도 없다**:

```text
proven_result_u8str_t s = proven_u8str_create(alloc, 64);
if (!proven_is_ok(s.err)) return;      /* s.value is not a string; do not touch it */
```

이는 Rust의 `Result`나 C++23의 `std::expected`와 같은 발상을, C가 허용하는 유일한 방식 — 값으로
반환되는 구조체 — 으로 표현한 것이다. 할당도, 간접 참조도, 숨은 제어 흐름도 없다. 여러분이 포기하는
것은 에러 처리를 조용히 건너뛸 수 있는 능력인데, 그것이 바로 요점이다.

라이브러리의 나머지가 이 세 가지 성질에 의존하므로 명시해 둘 가치가 있다:

- **`PROVEN_OK`은 0이다.** 그래서 `{0}`으로 영초기화한 result 구조체는 "성공, 비어 있음"을
  뜻하는 상태로 시작한다.
- **실패 가능한 함수는 `[[nodiscard]]`다** — 반환값을 버리는 코드를 컴파일러가 거부하게 만드는
  C23 속성이다. 여전히 에러를 무시할 수는 있지만, 호출 앞에 `(void)`를 써서 그것이 의도였음을
  밝혀야 한다.
- 함수가 달리 문서화하지 않는 한 **실패는 failure-atomic이다**: 연산이 실패했다면 아무것도 바꾸지
  않았다. 실패한 append는 여러분의 문자열을 있던 그대로 남겨 둔다.

### 레퍼런스

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

| 에러 | 일반적인 의미 | 보통의 대응 |
|---|---|---|
| `PROVEN_OK` | 성공. | 계속 진행한다. |
| `PROVEN_ERR_NOMEM` | allocator가 메모리를 제공하지 못했다. | 이 연산을 포기한다. 객체는 바뀌지 않았다. |
| `PROVEN_ERR_OUT_OF_BOUNDS` | 인덱스, 크기, 용량, 또는 범위가 유효하지 않다 — "들어가지 않는다"도 포함한다. | 대상을 키우거나, 입력을 거부한다. |
| `PROVEN_ERR_INVALID_ENCODING` | 인코딩된 텍스트가 검증에 실패했다(hex, Base64). | 입력을 잘못된 형식으로 보고 거부한다. |
| `PROVEN_ERR_INVALID_ARG` | 널 포인터, 유효하지 않은 allocator, 불가능한 모드, 잘못된 인자 개수. | 호출을 고친다 — 이것은 보통 잘못된 데이터가 아니라 여러분 코드의 버그를 뜻한다. |
| `PROVEN_ERR_IO` | OS 또는 장치가 연산을 실패시켰다. | 재시도하거나, 사용자에게 보고한다. |
| `PROVEN_ERR_NOT_FOUND` | 파일, 키, 부분 문자열, 또는 리소스가 존재하지 않는다. | "없음" 분기를 탄다. 흔히 에러조차 아니다. |
| `PROVEN_ERR_INVALID_STATE` | 객체의 상태가 이 연산을 허용하지 않는다. | 호출 순서를 고친다. |
| `PROVEN_ERR_OVERFLOW` | 정수 변환이나 크기 산술이 오버플로했다. | 그 크기를 거부한다. §4를 보라. |
| `PROVEN_ERR_UNSUPPORTED` | 이 플랫폼이나 빌드 프로파일에서는 사용할 수 없다. | 다른 경로를 택한다(예: 파이프는 seek할 수 없다). |
| `PROVEN_ERR_AGAIN` | 지금은 안 된다. 나중에 다시 시도하라. | 보통 기다린 뒤 재시도한다. |
| `PROVEN_ERR_EOF` | 입력의 끝. | 읽기 루프를 멈춘다 — 예외가 아니라 예상된 일이다. |
| `PROVEN_ERR_BUSY` | 큐, 락, 또는 리소스가 사용 중이다. | 물러나서 기다린다. |
| `PROVEN_ERR_PERMISSION` | 접근이 거부되었다. | 보고한다. 재시도해도 소용없다. |
| `PROVEN_ERR_INVALID_FORMAT` | 포맷이나 스캔 템플릿 자체가 잘못되었다. | 포맷 문자열을 고친다 — 잘못된 데이터가 아니라 버그다. |

```text
static inline int proven_is_ok(proven_err_t err);
#define PROVEN_IS_OK(err) proven_is_ok(err)
```

`proven_is_ok`은 `err == PROVEN_OK`일 때 0이 아닌 값을 반환한다. `PROVEN_IS_OK`은 매크로처럼
보이기를 원하는 자리를 위한 매크로 표기다. 둘은 같은 일을 한다.

올바른 예:

```c
proven_result_u8str_t s = proven_u8str_create(alloc, 32);
if (!proven_is_ok(s.err)) {
    return;   /* nothing was created, so there is nothing to destroy */
}
(void)proven_u8str_append(&s.value, PROVEN_LIT("ready"));
proven_u8str_destroy(alloc, &s.value);
```

반례 — 에러를 맨 진리값처럼 검사하기:

```text
proven_result_u8str_t s = proven_u8str_create(alloc, 32);
if (s.err) {
    /* wrong: works today only because PROVEN_OK is 0. It reads as "if there is
       an error" to someone who already knows that, and as nothing at all to
       everyone else. Say what you mean: !proven_is_ok(s.err). */
    return s.err;
}
```

반례 — 이 설계 전체가 막으려고 존재하는 바로 그 실수:

```text
proven_result_u8str_t s = proven_u8str_create(alloc, 32);
(void)proven_u8str_append(&s.value, text);   /* wrong: s.value may be garbage */
proven_u8str_destroy(alloc, &s.value);       /* wrong: destroying what was never created */
```

`.err`를 확인하기 전에 `.value`를 읽는 것은 이 관례가 여러분 대신 잡아 줄 수 없는 유일한 에러다.
구조체는 어느 쪽이든 존재하므로 컴파일러는 불평하지 않는다 — 그러니 호출 바로 다음 줄에 확인을
적는 습관을 들일 가치가 있다.

### 실전 예제: 값으로서의 에러

이 프로그램은 테스트 스위트가 컴파일하고 실행하므로 낡을 수 없다.
실패 가능한 호출이 취하는 두 가지 형태 — 돌려줄 것이 없을 때의 맨 `proven_err_t`와,
돌려줄 것이 있을 때의 `proven_result_*_t` — 을 보여주고, result 안의 값이 그 옆의
에러를 확인하기 전까지는 아무 의미도 없는 이유를 보여준다.

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

## 2. 타입, 그리고 왜 표기가 다른가

### 그냥 `int`와 `char`를 쓰면 안 되는가?

C의 내장 타입은 모호하기로 유명하다. `int`는 최소 16비트, `long`은 최소 32비트이고, `char`는
컴파일러와 대상 플랫폼에 따라 부호가 있을 수도 없을 수도 있다. 그렇지 않다고 가정한 코드는 여러분의
기계에서는 동작하고 남의 기계에서는 깨지며, 그 깨짐은 대개 조용하다.

C99는 `<stdint.h>`(`int32_t`, `uint64_t` 등)로 폭 문제를 해결했다. 이 라이브러리는 그 아래에서
그것들을 사용하되 자기만의 이름을 붙인다. 이유는 하나다: **라이브러리가 지원하는 모든 대상에서
같은 것을 뜻하는 단 하나의 표기**가 필요하기 때문이다. `<stdint.h>`가 유일하게 쓸 수 있는 헤더일지도
모르는 freestanding 환경까지 포함해서 말이다.

두 번째의, 더 미묘한 이유가 있고 실전에서 중요한 것은 그쪽이다: `proven_u8`과 `proven_byte_t`는
둘 다 8비트지만 서로 다른 것을 뜻한다.

- `proven_u8`은 0에서 255 사이의 **수**다. 더하고, 비교하고, 출력하라.
- `proven_byte_t`는 **어떤 객체 표현의 바이트**다. 이는 `unsigned char`의 별칭이며, 무엇이든
  그 원시 바이트를 미정의 동작 없이 들여다볼 수 있도록 C가 허용하는 유일한 타입이다.

원시 메모리에 `proven_byte_t`를 쓰는 것은 취향의 문제가 아니다. C의 strict-aliasing 규칙은 잘못된
타입의 포인터로 객체를 읽는 것이 미정의라고 말하며, `unsigned char`가 명시적인 예외다. 이
라이브러리의 모든 버퍼, view, 해시 입력이 `proven_byte_t`인 것은 정확히 그 이유 때문이다.

### 레퍼런스

| 이름 | 의미 | 비고 |
|---|---|---|
| `proven_i8`, `proven_i16`, `proven_i32`, `proven_i64` | 부호 있는 고정 폭 정수 | 표준 고정 폭 타입의 별칭. |
| `proven_u8`, `proven_u32`, `proven_u64` | 부호 없는 고정 폭 정수 | `proven_u8`은 **수치값**이다. 원시 객체 바이트에는 `proven_byte_t`를 쓰라. |
| `proven_u16` | 16비트 코드 유닛 타입 | `<uchar.h>`가 있으면 `char16_t`를, 없으면 `uint_least16_t`를 쓴다. U16 문자열 API가 UTF-16 코드 유닛에 이 타입을 쓴다. |
| `proven_byte_t` | 바이트 단위 객체 표현 | `unsigned char`의 별칭. 어떤 객체의 바이트든 합법적으로 들여다볼 수 있는 타입이다. |
| `proven_size_t` | 크기 및 인덱스 타입 | `size_t`의 별칭. 부호 없음 — 둘을 빼기 전에 §4를 보라. |
| `proven_ptrdiff_t` | 포인터 차이, 부호 있는 오프셋 | `ptrdiff_t`의 별칭. |
| `proven_intptr_t`, `proven_uintptr_t` | 포인터 크기 정수 | arena의 범위 검사처럼 명시적인 포인터-정수 작업에만 쓴다. |

```text
typedef struct {
    proven_err_t  err;
    proven_size_t value;
} proven_result_size_t;
```

`proven_result_size_t`는 크기 또는 에러를 반환한다. `value`는 `err == PROVEN_OK`일 때만 유효하다.
부분 append, 파일 읽기와 쓰기, 크기 조회에서 돌려받는 것이 이것이다 — 답이 "몇 개인가"이면서 연산이
실패할 수 있는 모든 자리에서 쓰인다.

반례 — 바이트를 작은 수의 저장소처럼 다루기:

```text
proven_u8 *raw = (proven_u8 *)&some_struct;   /* wrong type for raw inspection */
for (size_t i = 0; i < sizeof some_struct; i++) hash ^= raw[i];
```

주류 컴파일러 전부에서 우연히 동작하지만, 그래도 이것은 잘못 쓴 타입이다. aliasing 보장이 뒤에
있는 쪽은 `proven_byte_t`이고, 그것을 쓰는 것은 여러분이 수가 아니라 표현을 들여다보고 있음을
문서화한다.

## 3. 메모리 view: 포인터와 길이를 하나로

### 문제: 포인터는 잊어버린다

포인터는 무언가가 어디서 시작하는지만 말하고 그 외에는 아무것도 말하지 않는다. "어디서 끝나는가"라는
나머지 절반에 대해 C가 지금까지 해 온 모든 것은 그 위에 얹은 관례였다:

- 문자열을 위한 **NUL 종료자** — 길이를 알고 싶을 때마다 *O(n)* 스캔이 들고, 0 바이트를 포함한
  텍스트를 표현할 수 없으며, 종료자가 없어지는 순간 버퍼 오버런을 낸다.
- 그 밖의 모든 것을 위한 **별도의 길이 매개변수** — 누군가 잘못된 값을 넘기기 전까지만 동작한다.
  두 인자를 서로 묶어 주는 것이 아무것도 없기 때문이다.

```text
void process(const unsigned char *data, size_t len);
process(buf, sizeof buf);        /* fine */
process(buf, sizeof(buf) - 1);   /* also compiles; now the last byte is invisible */
process(other, sizeof buf);      /* also compiles; now it reads past the end */
```

저것 하나하나가 모두 합법적인 호출이다. 포인터와 길이 사이의 관계는 오직 프로그래머의 머릿속에만
존재한다.

### 이 라이브러리가 대신 하는 일

**view**는 포인터와 크기를 한 구조체에 담아 값으로 전달한 것이다. 둘은 하나이기 때문에 떨어질 수
없다.

```text
typedef struct { const proven_byte_t *ptr; proven_size_t size; } proven_mem_view_t;  /* read-only */
typedef struct {       proven_byte_t *ptr; proven_size_t size; } proven_mem_mut_t;   /* writable  */
typedef struct {       proven_byte_t *ptr; proven_size_t size; } proven_mem_t;       /* owned     */
```

이들은 두 워드다. 복사는 공짜이고, 근처 어디에도 할당이 없다. view가 하지 *않는* 일은 소유다.
view는 남의 것인 바이트를 가리키며, 그 바이트가 무효가 되는 순간 함께 무효가 된다. 이 규칙이
[0장](manual-00-start-here-ko.md#5-모든-페이지에서-만나게-될-다섯-가지-계약)의 계약 2다.

세 가지 표기는 의도만 다르며, 그 의도가 핵심이다:

- `proven_mem_view_t` — **borrowed, 읽기 전용.** `const`가 가리켜지는 바이트에 붙어 있으므로
  컴파일러가 강제한다.
- `proven_mem_mut_t` — **borrowed, 쓰기 가능.** 이를 통해 쓸 수 있지만, 여전히 소유하지는 않는다.
- `proven_mem_t` — **owned.** 누군가는 이것을 만들어 낸 allocator로 해제해야 한다. 타입이 그것을
  강제하지는 않는다. 알릴 뿐이다.

그리고 실패할 수 있는 연산을 위한 result 래퍼 두 개가 있다:

```text
typedef struct { proven_err_t err; proven_mem_mut_t  value; } proven_result_mem_mut_t;
typedef struct { proven_err_t err; proven_mem_view_t value; } proven_result_mem_view_t;
```

`proven_result_mem_mut_t`는 allocator가 돌려주는 것이다. `proven_result_mem_view_t`는 *검사된*
슬라이스가 돌려주는 것이다.

### 슬라이싱, 검사된 것과 검사되지 않은 것

view의 부분 범위를 취하는 것은 view로 하게 될 가장 흔한 일이며, 의도적으로 두 가지 형태로
제공된다:

| API | 목적 | 반환 |
|---|---|---|
| `proven_mem_view_from_owned(mem)` | owned 블록에 대한 읽기 전용 view. | `proven_mem_view_t`. |
| `proven_mem_mut_from_owned(mem)` | owned 블록에 대한 쓰기 가능 view. | `proven_mem_mut_t`. |
| `proven_mem_view_slice_checked(view, offset, size)` | 서브뷰, 검증함. | `proven_result_mem_view_t`. |
| `proven_mem_view_slice_unchecked(view, offset, size)` | 서브뷰, 검증하지 **않음**. | `proven_mem_view_t`. |
| `proven_mem_mut_slice_checked(mut, offset, size)` | 쓰기 가능 서브슬라이스, 검증함. | `proven_result_mem_mut_t`. |
| `proven_mem_mut_slice_unchecked(mut, offset, size)` | 쓰기 가능 서브슬라이스, 검증하지 **않음**. | `proven_mem_mut_t`. |
| `proven_range_contains_ptr(base, cap, ptr, size, out_offset)` | 이 포인터 범위가 저 할당 안에 있는가? 정수 주소 비교이며, 무관한 포인터끼리 포인터 산술을 하지 않는다. | `_Bool`. |
| `proven_memcmp(s1, s2, size)` | 원시 메모리를 비교한다. | 같으면 0. 부호는 바이트 순서(부호 없음)에 따른다. |
| `proven_mem_copy(dst, dst_cap, src)` | view를 `dst`로 경계 검사하며 복사한다. | `PROVEN_OK`. 들어가지 않으면 `PROVEN_ERR_OUT_OF_BOUNDS`이며 — **아무것도 기록되지 않는다**. 크기가 0이 아닌데 널 포인터이면 `PROVEN_ERR_INVALID_ARG`. 영역은 겹쳐서는 안 된다. |
| `proven_mem_move(dst, dst_cap, src)` | `proven_mem_copy`와 같지만 영역이 겹쳐도 된다. | 위와 같다. |

검사된 형태의 정확한 동작:

- `view.size > 0 && view.ptr == NULL` → `PROVEN_ERR_INVALID_ARG`(뒤에 메모리가 없는 크기는 빈
  view가 아니라 버그다).
- 요청한 범위가 view 밖으로 벗어남 → `PROVEN_ERR_OUT_OF_BOUNDS`.
- `size == 0` → `PROVEN_OK`와 함께 빈 view(`ptr == NULL`, `size == 0`).

**기본적으로 검사된 형태를 쓰라.** 검사되지 않은 형태는 바로 윗줄들에서 이미 범위를 증명한
경우 — 예컨대 여러분이 직접 계산한 경계를 가진 루프 안 — 를 위해 존재하며, 그때 두 번째 검사는 순수
비용일 뿐이다. 그런 경우는 실제로 있고, 느낌보다 좁다.

반례 — 바깥에서 온 수에 검사 없는 슬라이싱을 쓰기:

```text
proven_mem_view_t part = proven_mem_view_slice_unchecked(view, user_offset, user_size);
/* wrong: user_offset and user_size were never validated. This is the buffer
   overrun from the top of section 3, reintroduced through a safer-looking API. */
```

반례 — 빈 view를 거부하기:

```text
if (!view.ptr) return PROVEN_ERR_INVALID_ARG;   /* wrong: {NULL, 0} is legal */
```

빈 view는 정상적인 값이다. 길이 0인 슬라이스, 내용이 없는 파일, 방금 초기화된 문자열이 그렇다.
*잘못된* view를 가려내는 검사는 크기가 0이 아닌데 포인터가 널인 경우다:

```c
proven_mem_view_t view = {0};   /* size 0, ptr NULL: a legal empty view */
proven_err_t err = PROVEN_OK;

if (view.size > 0 && !view.ptr) {
    err = PROVEN_ERR_INVALID_ARG;   /* only a null pointer with bytes behind it is a bug */
}
(void)err;
```

## 4. 감길 수 없는 크기 산술

### 왜 곱셈 하나에 함수 호출이 필요한가

`proven_size_t`는 부호가 없고, C의 부호 없는 산술은 오버플로하지 않는다 — 조용하고 합법적으로
2^64를 법으로 *감긴다*. 그 규칙 하나가 업계 할당 버그의 상당 부분 뒤에 있다:

```text
proven_size_t total = count * sizeof(item_t);   /* wrong: wraps for large count */
void *p = malloc(total);                        /* succeeds, and is far too small */
items[count - 1] = x;                           /* writes way past the end */
```

`count = 2^61`이고 항목이 16바이트라면 `total`은 `0`이다. `malloc(0)`은 성공한다. 그 뒤의 모든
쓰기는 범위 밖이고, 어디에서도 에러를 보고하지 않았다. 크기가 프로그램 바깥에서 온 데이터로부터
계산될 때마다 — 파일 헤더, 네트워크 패킷, 사용자가 준 개수 — 같은 모양이 나타난다.

뺄셈에는 거울상 문제가 있고, 이쪽이 더 자주 걸린다:

```text
proven_size_t remaining = view.size - offset;   /* wrong when offset > size: a huge number */
```

### 이 라이브러리가 대신 하는 일

산술을 수행하고 그 결과가 들어맞았는지 알려 주는 매크로 세 개다.

```text
#define PROVEN_CKD_ADD(res, a, b)   /* true on overflow */
#define PROVEN_CKD_SUB(res, a, b)
#define PROVEN_CKD_MUL(res, a, b)
```

| 매크로 | 계산 | 반환 | `*res` 기록 |
|---|---|---|---|
| `PROVEN_CKD_ADD(res, a, b)` | `a + b` | **오버플로 시 true**, 성공 시 false | 들어맞을 때만 |
| `PROVEN_CKD_SUB(res, a, b)` | `a - b` | **오버플로 시 true**(부호 없는 언더플로 포함) | 들어맞을 때만 |
| `PROVEN_CKD_MUL(res, a, b)` | `a * b` | **오버플로 시 true** | 들어맞을 때만 |
| `PROVEN_SIZE_MAX` | — | `proven_size_t`가 담을 수 있는 가장 큰 값 | — |

- `res`는 답이 들어갈 곳을 가리키는 **포인터**다.
- 이들은 **오버플로 시 true**, 성공 시 false를 반환한다. 처음에는 거꾸로 읽힌다. 이 관례는
  C23의 `<stdckdint.h>`에서 왔고(컴파일러가 제공하면 이 매크로들이 그것을 사용한다), 그렇지
  않으면 GCC/Clang의 `__builtin_*_overflow` 계열에서 왔다. 호출을 *"이거 오버플로했나?"*로
  읽으면 말이 맞아떨어진다.
- `PROVEN_SIZE_MAX`는 계산한 뒤 검사하는 대신 한계와 직접 비교하고 싶을 때를 위해 있다.

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

반례:

```text
proven_size_t total = count * sizeof(item_t);   /* wrong: may wrap silently */
```

프로그램의 모든 산술 식에 이것들이 필요하지는 않다. 필요한 곳은 크기가 **여러분이 직접 고르지 않은
값으로부터 계산되는** 자리이며, 버그가 있는 곳이 정확히 거기다.

## 5. 정렬

### 정렬이란 무엇이고, 라이브러리가 왜 이를 언급하는가

C의 모든 타입에는 정렬이 있다. 하드웨어가 효율적으로, 또는 아예 로드할 수 있으려면 그 값이 시작해야
하는 주소다. `double`은 보통 8로 나누어떨어지는 주소를 원한다. 어떤 아키텍처는 정렬되지 않은 로드에
폴트를 내고, x86은 그저 느려질 뿐이다. 어느 쪽이든 C는 정렬되지 않은 포인터로 객체에 접근하는 것을
미정의 동작이라고 말한다.

여러분이 이것을 생각할 필요가 없었던 이유는 `malloc`이 무엇에나 알맞게 정렬된 메모리를 반환하기
때문이다. 그 보장은 여러분이 직접 메모리를 나눠 주기 시작하는 순간 정확히 사라진다 — 그리고 직접
메모리를 나눠 주는 일이 바로 [2장](manual-02-allocation-ko.md)의 arena와 pool이 하는 일이다.
포인터를 13바이트 밀고 그 결과를 건네주는 arena는 방금 여러분에게 정렬되지 않은 `double`을 준 것이다.

그래서 여기의 헬퍼들은 allocator를 위해, 그리고 allocator를 직접 쓰는 여러분을 위해 존재한다.

### 레퍼런스

| 매크로 | 의미 |
|---|---|
| `PROVEN_DEFAULT_ALIGNMENT` | 여러분이 달리 말하지 않을 때 라이브러리가 쓰는 기본값. 현재 8이다. |
| `PROVEN_MAX_ALIGN` | `alignof(max_align_t)` — 어떤 내장 타입에도 충분하다. |

| 함수 | 목적 | 반환 |
|---|---|---|
| `proven_is_pow2(x)` | `x`가 0이 아닌 2의 거듭제곱인가? 정렬값은 그래야 한다. | `bool`. |
| `proven_mem_align_up(addr, align)` | 크기나 주소를 `align`의 다음 배수로 올림한다. | 정렬된 값. `align`이 유효하지 않거나 올림이 오버플로하면 **0**. |
| `proven_uintptr_align_up(addr, align)` | 같은 일을 `proven_uintptr_t` 주소에 대해 한다. | 위와 같다. |

에러 관례에 주의하라: 이들은 에러 코드를 담을 자리가 없으므로 **실패 시 0**을 반환한다. 0은 결코
유효한 정렬 주소가 아니므로, 0인지 검사하는 것은 모호하지 않다.

올바른 예:

```c
typedef struct { int id; double weight; } item_t;

proven_size_t size = 100;
proven_size_t aligned = proven_mem_align_up(size, alignof(item_t));
proven_err_t err = (aligned == 0) ? PROVEN_ERR_OVERFLOW : PROVEN_OK;
(void)err;
```

반례 — 고전적인 비트 조작 버전:

```text
proven_size_t aligned = (size + align - 1) & ~(align - 1);   /* wrong: may overflow */
```

저 줄은 수많은 프로덕션 코드에 들어 있고, 표현 범위 꼭대기 근처의 입력을 빼면 모든 입력에 대해
옳다. 그 근처에서는 `size + align - 1`이 작은 수로 감기고, 결과는 시작한 곳보다 *낮은* 주소가
된다.

## 6. Panic: 에러를 돌려줄 상대가 남지 않았을 때

### 에러를 반환하는 라이브러리에 panic이 왜 있는가

이 장의 모든 내용은 실패가 들여다볼 수 있는 값이어야 한다고 주장해 왔다. panic은 그것이 항상
가능하지는 않다는 인정이다.

어떤 API는 구조상 에러 채널이 없다. `proven_arena_alloc_or_panic()`은 result가 아니라 메모리
블록을 반환하는데, 이는 할당 실패가 프로그램이 아예 실행될 수 없다는 뜻이고 돌려줄 만한 것이
없는, 프로그램의 초기 설정 단계를 위해 존재하기 때문이다. freestanding 빌드에서는 `abort()`도,
`stderr`도, 되돌아갈 곳도 없을 수 있다.

그래서 라이브러리는 `proven_panic()`을 호출해 panic을 일으키며, 이는 여러분이 교체할 수 있는
핸들러로 디스패치한다. `exit`를 호출하지도, `stderr`에 출력하지도, 운영체제를 가정하지도 않는다.

```text
typedef void (*proven_panic_handler_t)(const char *msg);

void proven_panic(const char *msg);
void proven_set_panic_handler(proven_panic_handler_t handler);
```

| API | 목적 | 비고 |
|---|---|---|
| `proven_panic(msg)` | panic을 일으킨다. 설치된 핸들러로 디스패치한다. | 라이브러리의 `_or_panic` API가 호출한다. 여러분이 직접 호출해도 된다. |
| `proven_set_panic_handler(handler)` | 핸들러를 설치한다. | 기본값으로 되돌리려면 `NULL`을 넘겨라. 스레드 안전하지 않다 — 다른 스레드가 생기기 전, 시작 단계에서 설치하라. |
| `proven_panic_handler_t` | `void (*)(const char *msg)` | 핸들러는 반환해서는 안 된다. |

기본 핸들러는 trap한다: GCC와 Clang에서는 `__builtin_trap()`이고, 그 밖의 컴파일러에서는 무한
루프다. 디스패치는 weak symbol이 아니라 함수 포인터를 거치므로, ELF와 PE/COFF(Windows,
mingw-w64) 툴체인에서 같은 방식으로 링크된다.

직접 설치하기(파일 스코프 함수이므로, 이는 다른 함수 안에 붙여넣는 것이 아니라 리스팅이다):

```text
static void my_panic(const char *msg) {
    log_critical(msg);            /* your logger */
    for (;;) {
        /* reset, halt, or wait for a debugger */
    }
}

proven_set_panic_handler(my_panic);   /* pass NULL to restore the default */
```

**panic 핸들러는 반환해서는 안 된다.** 반환하면 `_or_panic` 계열은 돌려줄 것이 없고, 그 result의
유효성은 보장되지 않는다.

반례 — 반환하는 핸들러:

```text
static void my_panic(const char *msg) {
    fprintf(stderr, "%s\n", msg);   /* wrong: this returns, and then the caller
                                       proceeds with a block that was never allocated */
}
```

반례 — 평범한 코드에서 `_or_panic`에 손을 뻗기:

```text
proven_mem_mut_t block = proven_arena_alloc_or_panic(&arena, n);
/* wrong for anything that handles input: an arena that is merely full has just
   killed the process. Use proven_arena_alloc and read the error. */
```

## 7. 버전 매크로

진단용으로, 그리고 라이브러리 버전에 맞춰 적응해야 하는 코드를 위한 컴파일 타임 식별자다.

```text
#define PROVEN_VERSION_STRING "proven_c_lib-v26.07.23d"
#define PROVEN_VERSION_NUM    260723
#define PROVEN_VERSION_SUFFIX "d"
```

`PROVEN_VERSION_STRING`은 빌드 보고서나 `--version` 플래그에서 출력하는 것이다.
`PROVEN_VERSION_NUM`은 수치 비교를 위한 정수 형태의 `YYMMDD`다 — `#if PROVEN_VERSION_NUM
>= 260720`처럼 쓴다. `PROVEN_VERSION_SUFFIX`는 같은 날 낸 릴리스를 구분한다.

이 셋은 빌드가 `include/proven/version.h`와 대조해 검사하므로, 매뉴얼이 헤더로부터 어긋날 수 없다.
(한 번 어긋난 적이 있고, 이 문장이 다시는 그러지 않을 이유다: 문자열에는 게이트가 걸려 있었고 나머지
둘에는 없었다.)

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

### 증명한 뒤에만 검사 없는 슬라이싱

호출자가 이미 범위를 증명했을 때 올바른 예:

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

저 검사의 모양에 주목하라. 더 자연스러운 `offset + size <= view.size` 대신
`size <= view.size - offset`으로 쓴 것은 정확히 §4 때문이다. 앞서 나온 `offset <= view.size`
검사가 있으면 합은 감길 수 있지만 차는 감길 수 없다.

반례:

```text
proven_mem_view_t part = proven_mem_view_slice_unchecked(view, user_offset, user_size);
/* wrong: user_offset and user_size were not validated */
```

### 빈 view는 허용된다

크기가 0인 view는 널 포인터를 가질 수 있다. 그것을 무턱대고 거부하지 마라.

반례:

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

### 다음으로 갈 곳

[2장](manual-02-allocation-ko.md)은 이 타입들이 일을 시작하는 곳이다. allocator는
`proven_mem_mut_t`를 만들어 내고, arena는 §5의 정렬 헬퍼를 사용하며, 라이브러리의 모든 확장 가능
컨테이너는 allocator를 매개변수로 받는다. §1이 에러를 반환값으로 받는 이유로 든 것과 같은 이유
때문이다 — 비용은 시그니처에 보여야 한다.
