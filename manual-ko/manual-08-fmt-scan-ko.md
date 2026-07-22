# Chapter 8: Formatting and Scanning (v26.07.23a)

이 장은 `fmt.h`와 `scan.h`에 대한 상세 레퍼런스다.
Chapter 3은 더 짧은 개요와 일상적인 예제를 제공한다.
이 장은 정확한 문법, 파라미터 형태, 반환값, 그리고 호출자가 흔히 실수하는 지점에 초점을 맞춘다.

## Table of contents

1. [Design model](#1-design-model)
2. [Formatter data model](#2-formatter-data-model)
3. [Formatter constructors and selectors](#3-formatter-constructors-and-selectors)
4. [Format string grammar](#4-format-string-grammar)
5. [Formatting APIs](#5-formatting-apis)
5.1. [Formatting a type of your own](#51-formatting-a-type-of-your-own)
6. [Console print helpers](#6-console-print-helpers)
7. [Scanner data model](#7-scanner-data-model)
8. [Scanner primitive APIs](#8-scanner-primitive-apis)
9. [Scan argument model](#9-scan-argument-model)
10. [Structural scan grammar](#10-structural-scan-grammar)
11. [Scan formatting APIs](#11-scan-formatting-apis)
11.1. [Scan error code guide and recovery](#111-scan-error-code-guide-and-recovery)
12. [Examples and misuse cases](#12-examples-and-misuse-cases)
13. [Freestanding and build-mode notes](#13-freestanding-and-build-mode-notes)

## 1. Design model

포매팅 측면과 스캐닝 측면은 정반대의 문제를 해결한다.

- 포매팅은 타입이 있는 값을 받아 텍스트로 렌더링한다.
- 스캐닝은 텍스트를 받아 타입이 있는 값을 기록한다.

이 프로젝트는 양쪽 모두 의도적으로 작게 유지한다.

- 포매팅은 간결한 플레이스홀더 언어, positional 재사용, 단순한 정렬(alignment), width, 그리고 숫자 값에 대한 hex 렌더링을 지원한다.
- 스캐닝은 타입이 있는 목적지 포인터, 엄격한 플레이스홀더 개수 검사, 그리고 공백 축약(whitespace collapsing)을 포함한 리터럴 매칭을 지원한다.
- 어느 쪽도 완전한 `printf`나 `scanf` 복제를 목표로 하지 않는다.

실용적인 결과로, 이 API들은 대규모 범용 포맷 엔진보다 추론하기 쉬우면서도, 문법은 일반적인 시스템 코드 작업에 충분히 표현력이 있다.

## 2. Formatter data model

### `proven_fmt_result_t`

```text
typedef struct {
proven_err_t  err;
proven_size_t written;
proven_size_t required;
} proven_fmt_result_t;
```

의미:

- `err`: 연산의 상태 코드.
- `written`: 목적지에 실제로 기록된 바이트 수.
- `required`: 전체 포맷 출력에 필요한 총 바이트 수.

먼저 `err`를 사용하라. 나머지 필드는 잘림(truncating)이나 부분적으로 성공한 연산에서 가장 유용하다.

성공한 result는 다음과 같은 모습이다.

```c
proven_result_u8str_t rs = proven_u8str_create(alloc, 64);
if (proven_is_ok(rs.err)) {
    proven_u8str_t s = rs.value;

    proven_fmt_result_t r = proven_u8str_append_fmt_trunc(
        &s,
        "hello {}",
        PROVEN_ARG("world")
    );
    if (proven_is_ok(r.err)) {
        proven_println("{}", PROVEN_ARG(proven_u8str_as_view(&s)));
    }

    proven_u8str_destroy(alloc, &s);
}
```

잘림이 발생한 result도 얼마나 더 많은 공간이 필요했는지는 여전히 알려줄 수 있다.
여기서는 목적지를 일부러 너무 작게 만들었으므로 `written`이 `required`에
못 미치게 되며, 그럼에도 문자열은 여전히 유효하다.

```c
proven_result_u8str_t rs = proven_u8str_create(alloc, 8);   /* on purpose: too small */
if (proven_is_ok(rs.err)) {
    proven_u8str_t s = rs.value;

    proven_fmt_result_t r = proven_u8str_append_fmt_trunc(
        &s,
        "name={} score={}",
        PROVEN_ARG("ada"),
        PROVEN_ARG(42)
    );
    /* r.written is what fit; r.required is what the whole output needed. */
    proven_println("wrote {} of {} bytes",
                   PROVEN_ARG(r.written), PROVEN_ARG(r.required));

    proven_u8str_destroy(alloc, &s);
}
```

### `proven_arg_type_t`

```text
typedef enum {
PROVEN_ARG_NONE,
PROVEN_ARG_I32,
PROVEN_ARG_U32,
PROVEN_ARG_I64,
PROVEN_ARG_U64,
#ifndef PROVEN_FMT_NO_FLOAT
PROVEN_ARG_F64,
#endif
PROVEN_ARG_CSTR,
PROVEN_ARG_STR_VIEW,
PROVEN_ARG_DATETIME,
PROVEN_ARG_PTR,
PROVEN_ARG_FN,
} proven_arg_type_t;
```

포매터는 현재 다음 값 클래스들을 인식한다.

- signed 32비트 정수
- unsigned 32비트 정수
- signed 64비트 정수
- unsigned 64비트 정수
- 부동소수점 값 (`PROVEN_FMT_NO_FLOAT`가 정의되지 않은 경우)
- 신뢰할 수 있는 C 문자열
- 빌려온(borrowed) U8 문자열 뷰
- datetime
- 객체 포인터
- 함수 포인터

### `proven_arg_t`

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

union 필드는 선택된 `type`과 일치해야 한다.
union 필드를 잘못 채워 놓고 포매터가 알아서 추측해 주기를 바라며 `proven_arg_t`를 억지로 만들지 마라.

잘못된 예:

```text
proven_arg_t arg = {0};
arg.type = PROVEN_ARG_I64;
arg.value.u64 = 123; /* wrong: type and union field do not match */
```

올바른 예:

```c
proven_arg_t arg = proven_arg_i64(123);
(void)arg;   /* pass it to a formatting macro; PROVEN_ARG(arg) accepts it as-is */
```

## 3. Formatter constructors and selectors

### Constructor summary

| API | 파라미터 | 반환 | 의도 |
|---|---|---|---|
| `proven_arg_none(void)` | 없음 | `proven_arg_t` | 내부 sentinel 값. |
| `proven_arg_i32(int v)` | signed 정수 | `proven_arg_t` | 32비트 signed 정수로 렌더링. |
| `proven_arg_u32(unsigned int v)` | unsigned 정수 | `proven_arg_t` | 32비트 unsigned 정수로 렌더링. |
| `proven_arg_i64(long long v)` | 넓은 signed 정수 | `proven_arg_t` | 64비트 signed 정수로 렌더링. |
| `proven_arg_u64(unsigned long long v)` | 넓은 unsigned 정수 | `proven_arg_t` | 64비트 unsigned 정수로 렌더링. |
| `proven_arg_f64(double v)` | 부동소수점 값 | `proven_arg_t` | float 포매팅이 비활성화되지 않은 한, 부동소수점 텍스트로 렌더링. |
| `proven_arg_cstr(const char *v)` | 신뢰할 수 있는 live C 문자열 | `proven_arg_t` | NUL로 종료되는 C 문자열을 렌더링. |
| `proven_arg_cstr_n(const char *v, proven_size_t max_len)` | 경계가 있을 수 있는 C 문자열 | `proven_arg_t` | NUL을 찾되 `max_len`까지만 렌더링. |
| `proven_arg_str_view(proven_u8str_view_t v)` | 빌려온 U8 뷰 | `proven_arg_t` | NUL 종료를 가정하지 않고 빌려온 뷰를 렌더링. |
| `proven_arg_datetime(proven_datetime_t v)` | datetime 값 | `proven_arg_t` | 포매터의 datetime 규칙으로 datetime을 렌더링. |
| `proven_arg_ptr(const void *v)` | 객체 포인터 | `proven_arg_t` | 포인터 값을 렌더링. |
| `proven_arg_fn(void (*v)(void))` | 함수 포인터 | `proven_arg_t` | 원시 함수 포인터 표현을 렌더링. |
| `proven_arg_ucstr(const unsigned char *v)` | unsigned-char 문자열 | `proven_arg_t` | `proven_arg_cstr`에 대한 편의 래퍼. |
| `proven_arg_identity(proven_arg_t v)` | 기존 argument 객체 | `proven_arg_t` | 통과(pass-through) 헬퍼. |
| `proven_arg_bool(bool v)` | boolean | `proven_arg_t` | `1` / `0`이 아니라 `true` / `false`를 단어로 렌더링. |
| `proven_arg_char(char v)` | 문자 하나 | `proven_arg_t` | **문자**를 렌더링. 이것이 바로 `char` 변수는 문자로 렌더링되는데 리터럴 `PROVEN_ARG('Z')`는 여전히 `90`으로 렌더링되는 이유다. C에서 `'Z'`는 `int` 타입이며, 어떤 `_Generic`으로도 이것을 숫자 90과 구별할 수 없다. |
| `proven_arg_custom(const void *v, proven_fmt_custom_fn fn)` | 어떤 타입이든 | `proven_arg_t` | 라이브러리가 전혀 알지 못하는 타입을, 당신이 제공하는 함수를 통해 렌더링. [Formatting a user-defined type](#51-formatting-a-type-of-your-own) 참조. |

### `PROVEN_ARG(x)`

`PROVEN_ARG(x)`는 통상적인 진입점이다.
`_Generic`을 사용하여 컴파일러가 `x`의 타입으로부터 생성자를 고른다.

현재 매핑은 다음과 같다.

- `_Bool`, `char`, `signed char`, `short`, `int` -> `proven_arg_i32`
- `unsigned char`, `unsigned short`, `unsigned int` -> `proven_arg_u32`
- `long`, `long long` -> `proven_arg_i64`
- `unsigned long`, `unsigned long long` -> `proven_arg_u64`
- `double`, `float` -> `proven_arg_f64` (`PROVEN_FMT_NO_FLOAT`가 정의되지 않은 경우)
- `const char *`, `char *` -> `proven_arg_cstr`
- `unsigned char *`, `const unsigned char *` -> `proven_arg_ucstr`
- `void *`, `const void *` -> `proven_arg_ptr`
- `proven_u8str_view_t` -> `proven_arg_str_view`
- `proven_datetime_t` -> `proven_arg_datetime`
- `proven_arg_t` -> `proven_arg_identity`

중요한 결과:

- `PROVEN_ARG`는 함수 포인터를 선택하지 않는다.
- 함수 포인터에는 `PROVEN_ARG_FN(f)`를 사용하라.

잘못된 예:

```text
void helper(void) {}
proven_u8str_append_fmt_grow(alloc, &s, "{}", PROVEN_ARG(helper)); /* wrong */
```

올바른 예:

```c
void helper(void);   /* whatever function you want to print the address of */

proven_result_u8str_t rs = proven_u8str_create(alloc, 64);
if (proven_is_ok(rs.err)) {
    proven_fmt_result_t r = proven_u8str_append_fmt_grow(alloc, &rs.value, "{}",
                                                         PROVEN_ARG_FN(helper));
    (void)r;
    proven_u8str_destroy(alloc, &rs.value);
}
```

### `PROVEN_ARG_FN(f)`

이 매크로는 호출자가 함수 포인터를 `void *`로 캐스팅하지 않고도 넘길 수 있도록 존재한다.
`proven_arg_fn`을 감싼 작은 안전 래퍼다.

예:

```c
void helper(void);

proven_result_u8str_t rs = proven_u8str_create(alloc, 64);
if (proven_is_ok(rs.err)) {
    proven_u8str_t s = rs.value;

    proven_fmt_result_t r = proven_u8str_append_fmt_grow(
        alloc,
        &s,
        "callback = {}",
        PROVEN_ARG_FN(helper)
    );
    if (!PROVEN_FMT_IS_OK(r)) {
        proven_eprintln("formatting the callback failed");
    }

    proven_u8str_destroy(alloc, &s);
}
```

### `PROVEN_ARG_CSTR_N(v, max_len)`

이 매크로는 경계가 있는 문자열 헬퍼다.
소스가 완전히 신뢰할 수 있는 C 문자열이 아닐 수 있지만 여전히 C 문자열과 유사한 입력 처리를 원할 때 사용하라.

이 매크로는 `max_len`까지만 NUL을 탐색한 뒤 경계 지어진 접두부를 뷰로서 포매팅한다.

좋은 사용 사례 - `buf`가 통신선(wire)에서 들어왔으므로 NUL 종료가 전혀 보장되지 않는 경우:

```c
char buf[128];                       /* filled from an untrusted source */
(void)proven_mem_copy(buf, sizeof buf, proven_mem_view_from_u8(PROVEN_LIT("payload")));

proven_result_u8str_t rs = proven_u8str_create(alloc, 64);
if (proven_is_ok(rs.err)) {
    proven_fmt_result_t r = proven_u8str_append_fmt_grow(
        alloc,
        &rs.value,
        "payload={}",
        PROVEN_ARG_CSTR_N(buf, sizeof buf)   /* looks for NUL only within 128 bytes */
    );
    (void)r;
    proven_u8str_destroy(alloc, &rs.value);
}
```

나쁜 사용 사례:

```text
const char *buf = get_network_buffer();
proven_u8str_append_fmt_grow(alloc, &s, "{}", PROVEN_ARG(buf)); /* wrong if buf is not trusted */
```

### Float formatting note

`PROVEN_FMT_NO_FLOAT`가 정의되면 float 지원은 제네릭 셀렉터에서 제거되고 float 생성자는 사용할 수 없다.
이는 런타임 토글이 아니라 컴파일 시점의 구성 선택이다.

`double`의 기본 `{}` 렌더링은 유한 값에 대해 소수점 이하 6자리 고정 형식을
만들며, 크기가 간결한 형식에 비해 너무 크거나 너무 작으면 과학적 표기(scientific)로
전환된다. 이전 버전과 달리, 이 출력은 이제 **정확하고 정수만 사용하는** 엔진으로
계산된다(`double`/`long double` 근사 없음). 자릿수는 정확히 반올림되므로(round-half-to-even),
`{}`는 같은 값에 대해 `printf("%.6f")` / `%.6e`가 출력하는 것과 일치한다. 최단(shortest)
라운드트립 형식이나 6이 아닌 다른 precision이 필요하면 아래에 설명하는
`proven_float_format_*` 정책 API를 사용하라.

### Accuracy and limits

- 기본 `{}` float 출력은 소수점 이하 6자리를 사용하며, 가장 가까운 값으로
  정확히 반올림하되 동점(tie)은 짝수로 처리한다(glibc `%.6f`/`%.6e`와 일치). 어떤
  크기에서도 정확하며, 구성 가능한 큰 정수(big-integer) 용량을 넘어서는
  precision/크기 한계는 없다.
- 라운드트립 직렬화에는 **최단(shortest)** 정책
  (`proven_float_format_options_shortest()`)을 사용하라. 이는 다시 파싱하면 정확히
  같은 값이 되는 가장 짧은 십진 문자열을 방출한다.
- 십진수-double 스캐닝은 IEEE-754 binary64로, 가장 가까운 값으로 반올림하되
  동점은 짝수로 처리하도록 정확히 반올림되며, 호스트의 `strtod`와 비트 단위로 일치한다.
  가장 작은 subnormal에 대한 중간 임계값 아래의 값들은 입력 부호를 보존한 채
  부호 있는 0으로 반올림된다.
- 파서는 `Clinger fast path -> Eisel-Lemire -> 정확한 big-integer 폴백`으로
  계층화되어 있다. 모든 계층이 정확하고 폴백이 최종 판정자이므로, 결과는 모든
  입력에 대해 정확히 반올림된다. 캐시된 거듭제곱 테이블은 생성된 소스다
  (`scripts/generate_float_decimal_tables.py`).
- 정확 폴백 big-integer 용량은 임베디드 타겟을 위해
  `PROVEN_FLOAT_BIGINT_LIMBS`(`include/proven/float_config.h` 참조)로 조정할 수 있다.
  fast path는 big integer를 절대 건드리지 않는다.
- 검증: 포매터와 파서는 40.8억 개의 모든 유한 `binary32` 값과 25.6억 개의
  무작위 `binary64` 값에 대해 호스트 C 라이브러리와 남김없이 대조 검사되었으며,
  불일치가 하나도 없었다. 알고리즘, 방법론, glibc와의 벤치마크는
  `docs/float-correctness-and-performance.md`를 참조하라.

### Inside the engine (conceptual)

API를 사용하는 데 이 내용이 전혀 필요하지 않다. 이는 출력이 왜 신뢰할 수 있는지
알고 싶은 독자를 위한 것이다. 전체 내용은
`docs/float-correctness-and-performance.md`에 있다.

**파싱(십진수 → binary64), 세 계층, 빠른 것 우선.** 결과는 항상 정확히
반올림된다. 계층은 순전히 속도의 계단일 뿐이며, 각 계층은 정확한 답을 보장할 수
있을 때만 선택된다.

1. *Clinger fast path.* 값의 유효 자릿수가 적고 지수가 작을 때, 유효숫자(significand)와
   `10^exp` 모두 `double`로 정확히 표현 가능하므로, 단 한 번의 반올림된
   곱셈/나눗셈이 증명 가능하게 올바르다. 일상적인 대부분의 숫자를 커버한다.
2. *Eisel-Lemire.* 캐시된 10의 거듭제곱과의 64×128비트 고정소수점 곱셈이며,
   결과가 반올림 경계로부터 충분히 멀어 확실한지를 검사한다. 검사가 불확실하면(값이
   중간의 동점 지점에 놓이면) 다음 계층으로 넘어간다.
3. *정확한 big-integer 폴백.* 값을 큰 정수들의 비(`significand`와 `5^q`/`2^q`)로
   구성하고, 후보 `double` 및 그 이웃과 정확히 비교한다. 이것이 동점과 subnormal을
   올바르게 만드는 최종 판정자다. 시드된 ±16-ULP 윈도우가 탐색을 몇 번의 비교로
   유지한다. big-integer 용량은 `PROVEN_FLOAT_BIGINT_LIMBS`로 제한된다. 이 계층만이
   limb를 (스택에) 할당하며, fast path는 여기에 절대 도달하지 않는다.

**포매팅(binary64/32 → 십진수).** 두 엔진, 그 어디에도 `long double` 없음:

- *최단(shortest)* (`proven_float_format_options_shortest()`): **Grisu3** fast path가
  거의 모든 입력에 대해 최소한의 라운드트립 자릿수를 ~90 ns에 만든다. Grisu3가
  최소성을 증명할 수 없는 드문 경우에는 정확한 **Dragon4**(Burger–Dybvig,
  round-to-even) 코어로 폴백한다. 결과는 같은 비트로 다시 파싱되는 유일한 최단
  십진수다.
- *고정 `%f` / 과학적 `%e`* (기본 `{}`와 고정 옵션): 정확한 정수 엔진이 값을
  big-integer `mul_pow5`/시프트로 `10^precision`만큼 스케일링하고, 정수 `divmod`를
  수행한 뒤, round-half-to-even으로 반올림한다. 따라서 어떤 precision과 크기에서도
  glibc와 일치하며, `2^64`/precision 한계가 없다. 극단적인 지수는 실제
  임의정밀도(arbitrary-precision) 연산을 하며 그만큼 느리다(실무에서는 드물다).

### Public float parsing APIs

세 진입점이 하나의 정확히 반올림되는 백엔드를 공유한다.

- `proven_scan_f64(scan)` — `proven_scan_t` 커서로부터 파싱하며, 실패 시 커서를
  복원한다. 네이티브하고 길이로 경계 지어진 경로(NUL 종료 불필요).
- `proven_parse_double_ascii(view)` — `proven_u8str_view_t`로부터 로케일 독립적인
  ASCII 토큰 하나를 파싱하고 소비한 길이를 보고한다.
- `proven_strtod(nptr, endptr)` — C 문자열 위의 `strtod` 스타일 편의 래퍼로, 선행
  ASCII 공백을 건너뛰고 `endptr`을 보고한다.

#### Worked example: parsing

```c
#include "proven/scan.h"
#include "proven/float_parse.h"

/* (1) Native, view-based parsing through a scanner cursor. */
proven_scan_t sc = proven_scan_init(proven_u8str_view_from_cstr("3.14159e2 rest"));
proven_result_f64_t r = proven_scan_f64(&sc);
if (r.err == PROVEN_OK) {
    /* r.val == 314.159; the cursor now sits at " rest". */
    proven_println("parsed {}", PROVEN_ARG(r.val));
}

/* (2) strtod-style wrapper for C strings. endptr reports where parsing stopped. */
char *end = NULL;
double v = proven_strtod("  -0.5\t", &end);   /* v == -0.5, *end == '\t' */
(void)v;

/* A trailing exponent marker with no digits stops like strtod: "1e" parses 1,
   leaving endptr at 'e'. Inputs with hundreds of significant digits and extreme
   exponents are still rounded correctly via the exact fallback. */
```

#### Worked example: formatting with the policy API

`proven_float_format_f64_policy` / `_f32_policy`는 호출자 버퍼에 직접 기록하고 기록한
바이트 수를 보고한다. 이들은 절대 할당하지 않는다.

```c
#include "proven/float_format.h"

char buf[64];
proven_size_t n = 0;

/* Shortest round-trippable form: 0.1 -> "0.1" (not "0.10000000000000001"). */
(void)proven_float_format_f64_policy(buf, sizeof buf, 0.1,
    PROVEN_FLOAT_FORMAT_POLICY_RYU,
    proven_float_format_options_shortest(), &n);
/* buf == "0.1", n == 3 */

/* Fixed precision (correctly rounded, round-half-to-even). */
proven_float_format_options_t opt = proven_float_format_options_fixed_default();
opt.precision = 2;
(void)proven_float_format_f64_policy(buf, sizeof buf, 3.14159,
    PROVEN_FLOAT_FORMAT_POLICY_DEFAULT, opt, &n);
/* buf == "3.14" */

/* Always scientific - this is what {:e} selects. Six fractional digits by default,
   a signed two-digit-minimum exponent: exactly what printf's %e prints. */
proven_float_format_options_t sci = proven_float_format_options_scientific();
sci.precision = 2;
(void)proven_float_format_f64_policy(buf, sizeof buf, 42.0,
    PROVEN_FLOAT_FORMAT_POLICY_DEFAULT, sci, &n);
/* buf == "4.20e+01" - where fixed would give "42.00" and shortest "42" */
```

- `PROVEN_FLOAT_FORMAT_POLICY_RYU`는 최단 출력을 선택한다. `DEFAULT`/`SIMPLE`은
  정확한 고정 precision 경로(`%f`, 아주 크거나 작은 크기에서는 `%e`로 전환)를
  선택한다.
- 버퍼가 너무 작으면 `PROVEN_ERR_OUT_OF_BOUNDS`를 반환하며(값은 절대 소리 없이
  잘리지 않는다), 지원되지 않는 정책에는 `PROVEN_ERR_INVALID_ARG`를 반환한다.
- 제네릭 `{}` 포매터(`proven_u8str_append_fmt*`)는 내부적으로 `DEFAULT` 정책을
  사용하므로, 일상적인 로깅에서는 이 API를 직접 호출할 필요가 없다.

## 4. Format string grammar

포매터는 의도적으로 작은 문법을 받아들인다.

### Replacement fields

지원되는 형식:

- `{}`: 다음 positional argument
- `{0}`: 첫 번째 사용자 argument
- `{1}`: 두 번째 사용자 argument
- `{2}`: 세 번째 사용자 argument
- 이하 계속

번호 매김은 사용자 대상이며 0부터 시작한다.
구현은 인덱스 0에 숨겨진 sentinel을 저장하고, 사용자 인덱스 `0`을 내부 argument 슬롯 `1`로 매핑한다.

### Escaped braces

- `{{`는 리터럴 `{`가 된다
- `}}`는 리터럴 `}`가 된다

### The layout spec

```text
{:[[fill]align][sign][#][0][width][.precision][type]}
```

모든 부분은 선택적이며, 순서는 나머지 세계가 사용하는 것과 같다. 따라서 Python이나
Rust에서 복사한 스펙은 여기서도 그곳과 같은 의미를 가진다.

| 부분 | 값 | 하는 일 |
|---|---|---|
| `align` | `<` `>` `^` | 왼쪽, 오른쪽, 가운데. 기본값 `>`. |
| `fill` | align 앞에 오는 임의의 문자 | 패딩 문자. 기본값은 공백. |
| `sign` | `+` 또는 공백 | 음수가 아닌 수에 부호를 강제하거나, 부호 자리를 하나 예약한다. |
| `#` | | 대체 형식(alternate form): `0x`, `0X`, `0o`, `0b` 접두어. 정수 전용. |
| `0` | | 0 채움. `42`에 대한 `{:08}`은 `00000042`. |
| `width` | 숫자, 최대 10000 | 최소 필드 width. |
| `.precision` | `.N`, 최대 60 | 소수 자릿수. **float 전용.** |
| `type` | `x X o b d`(int), `f g e`(float) | 기수(base)와 대소문자. `f` 고정, `g` 최단 라운드트립, `e` 과학적 표기(printf `%e`). |

과거에 거짓이었던 만큼 알아둘 가치가 있는 두 가지가 있다.

- **선행 `0`은 0 채움이지 width의 첫 자리가 아니다.** `{:08}`은 v26.07.12f 전까지
  `"      42"`를 만들었다(공백 패딩, 오류 없음). 명시적 fill은 여전히 우선한다:
  `{:*>08}`은 `*`로 채운다.
- **0 채움은 부호와 자릿수 사이에 들어간다.** `42`에 대한 `{:+08}`은
  `+0000042`이며, `0000+42`가 아니다. 패딩은 숫자의 일부이고, 숫자의 부호가 먼저
  온다.

### Floats

`{}`는 정확히 반올림된 소수점 이하 6자리를 준다. `{:.3}`은 3자리, `{:.0}`은 0자리를
주고, `{:e}`는 과학적 표기를 강제한다 - 가수(mantissa), `precision` 소수 자릿수(기본
6자리), 부호 있는 최소 두 자리 지수, 정확히 반올림, 정확히 printf의 `%e`와 같다 -
이는 `{:f}`와 `{:g}`가 도달하지 못하는 형식이다: `{:f}`는 지수를 절대 보여주지 않고,
`{:g}`는 더 짧을 때만 지수를 사용한다. `{:f}`는 고정 형식을 강제하고, `{:g}`는
라운드트립되는 가장 짧은 표현을 준다.

v26.07.12i 전까지 **이 중 어느 것도 존재하지 않았다**: 모든 float은 영원히 정확히
6자리 소수로만 나왔다. 정확 엔진은 언제나 이 모든 것을 할 수 있었으나, `{}` 문법이
그것에 도달하지 못했을 뿐이다. 눈에 보이는 대가는 float 컬럼을 정렬할 수 없다는
것이었다. `12.5`는 9자 폭으로, `100.0`은 10자 폭으로 렌더링되었기 때문이다.

```c
proven_byte_t buf[64];
proven_u8str_t line = proven_u8str_borrow(buf, sizeof buf);
(void)proven_u8str_append_fmt(&line, "{:>9.2}", PROVEN_ARG(12.5));    /* "    12.50" */
```

### A spec the argument cannot honour is an error

double에 대한 `{:x}`, 정수에 대한 `{:.2}`, 정수에 대한 `{:f}`, 문자열에 대한 `{:#}`:
모두 `PROVEN_ERR_INVALID_FORMAT`.

이들은 과거에 *무시*되었다 - double에 대한 `{:x}`는 `3.500000`을 출력하고 성공을
보고했다. 호출자는 무언가를 요청하고, 다른 것을 받았으며, 그것이 제대로 되었다는
말을 들었다. 그것은 있을 수 있는 최악의 결과이며, 거부하는 것보다 나쁘다.

### `char` and `bool`

`PROVEN_ARG('Z')`는 `Z`로 렌더링되고, `bool`은 `true` 또는 `false`로 렌더링된다.
둘 다 과거에는 정수 경로를 거쳤으므로, 문자가 `90`으로 출력되었고 문자 하나를
방출할 방법이 아예 없었다 - hex 덤프의 ASCII 컬럼은 별도 버퍼에서 손으로 만들어
문자열로 넘겨야 했다. 이제 대문자 hex 덤프는 한 개의 루프다.

```c
proven_byte_t hexbuf[64];
proven_u8str_t hexline = proven_u8str_borrow(hexbuf, sizeof hexbuf);
unsigned char byte = 0xde;
(void)proven_u8str_append_fmt(&hexline, "{:02X} ", PROVEN_ARG((unsigned)byte));
(void)proven_u8str_append_fmt(&hexline, "{}", PROVEN_ARG((char)'.'));
```

## 5. Formatting APIs

### `proven_u8str_fmt_internal(...)`

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

이것은 내부 포매팅 엔진이다.
사용자 코드는 보통 대신 공개 매크로를 호출해야 한다.

파라미터:

- `alloc`: 문자열이 커져야 할 때 사용되는 할당자
- `str`: 목적지 U8 문자열
- `trunc`: true이면 best-effort 잘림을 허용하고, false이면 atomic(원자적) 동작을 유지
- `fmt`: 포맷 텍스트
- `scratch`: 필요할 때 임시 alias 패칭에 사용되는 할당자
- `args`: 인덱스 0의 숨겨진 sentinel을 포함한 포맷 argument 배열
- `args_count`: sentinel을 포함한 `args`의 총 길이

반환값:

- `proven_fmt_result_t`

중요한 규칙:

- `args_count`는 플레이스홀더 개수에 숨겨진 sentinel을 더한 값과 일치해야 한다
- 사용되지 않는 여분의 argument는 오류다
- 누락된 argument는 오류다
- 엔진이 목적지 문자열과 빌려온 뷰 argument 사이의 aliasing을 감지하면, 실패 atomicity를 보존하기 위해 scratch 할당자를 사용할 수 있다

### `proven_u8str_append_fmt(str, fmt, ...)`

고정 용량 문자열로의 atomic(원자적) 포매팅.
결과가 들어맞지 않으면, 함수는 실패를 보고하고 목적지를 변경하지 않는다.

전부 아니면 전무(all-or-nothing) 동작을 원할 때 사용하라.

### `proven_u8str_append_fmt_trunc(str, fmt, ...)`

Best-effort 포매팅.
들어맞는 만큼 기록하고, 얼마나 기록되었는지와 얼마나 필요했는지를 보고한다.

부분 출력이 허용될 때 사용하라.

### `proven_u8str_append_fmt_grow(alloc, str, fmt, ...)`

성장 가능한(growable) 포매팅.
제공된 할당자를 통해 목적지 문자열을 재할당할 수 있다.
할당 실패 시, 기존 문자열은 유효하게 유지된다.

수동 용량 계획 없이 출력을 들어맞게 하고 싶을 때 사용하라.

### `proven_u8str_append_fmt_with_scratch(alloc, str, fmt, scratch, ...)`

별도의 scratch 할당자를 사용하는 성장 가능한 포매팅.
argument 목록에 목적지 버퍼와 alias할 수 있는 문자열 뷰가 있어 임시 패칭이 필요할 때 유용하다.

`alloc`과 `scratch` 모두에 실제 할당자를 사용하라.
호출에 충분할 만큼 수명이 길지 않은 한, 죽은 arena나 일회용 임시 버퍼를 넘기지 마라.

### Float format policy seam

공개 float 정책 헤더는 float 포매팅을 위한 명시적 정책 레이어를 제공한다.
의도적으로 작으며, 정확한 고정 precision 포매터를 기본 경로로 유지한다.

주요 진입점은 다음과 같다.

- `proven_float_format_f64_policy(...)`
- `proven_float_format_f32_policy(...)`
- `proven_float_format_options_fixed_default()`
- `proven_float_format_options_shortest()`

정책 참고 사항:

- `PROVEN_FLOAT_FORMAT_POLICY_DEFAULT`와 `PROVEN_FLOAT_FORMAT_POLICY_SIMPLE`은 정확한 고정 precision 출력(정확히 반올림, round-half-to-even)을 선택한다.
- `PROVEN_FLOAT_FORMAT_POLICY_RYU`는 최단 출력 정책 분기다.
- 정책 API는 지원되지 않는 enum 값에 대해 `PROVEN_ERR_INVALID_ARG`를 반환한다.
- 정책 API는 호출자가 제공한 버퍼가 너무 작을 때 `PROVEN_ERR_OUT_OF_BOUNDS`를 반환한다.

예:

```c
char buf[128];
proven_size_t written = 0;
proven_err_t err = proven_float_format_f64_policy(
    buf,
    sizeof buf,
    0.1,
    PROVEN_FLOAT_FORMAT_POLICY_RYU,
    proven_float_format_options_shortest(),
    &written
);
if (proven_is_ok(err)) {
    /* buf holds the shortest form that parses back to exactly 0.1 */
    proven_println("{}", PROVEN_ARG_CSTR_N(buf, written));
}
```

### `PROVEN_FMT_IS_OK(res)`

`proven_fmt_result_t`를 검사하는 작은 헬퍼 매크로.
의도를 간결하게 유지하고 싶을 때 사용하라.

예:

```c
proven_result_u8str_t rs = proven_u8str_create(alloc, 32);
if (proven_is_ok(rs.err)) {
    proven_u8str_t s = rs.value;

    proven_fmt_result_t r = proven_u8str_append_fmt_grow(
        alloc,
        &s,
        "name={} score={:0>4}",
        PROVEN_ARG("ada"),
        PROVEN_ARG(42)
    );
    if (!PROVEN_FMT_IS_OK(r)) {
        /* the string is untouched: grow-mode formatting is failure-atomic */
        proven_eprintln("formatting failed");
    }

    proven_u8str_destroy(alloc, &s);
}
```

### Console-style helpers

`sysio` 레이어는 같은 포매터 기계를 사용하는 print 헬퍼들을 제공한다.

- `proven_print(fmt, ...)`
- `proven_println(fmt, ...)`
- `proven_eprint(fmt, ...)`
- `proven_eprintln(fmt, ...)`

이들은 stdout이나 stderr로 직접 포맷 출력을 원할 때 편리하다.
여전히 `proven_err_t`를 반환하므로, 출력이 중요할 때는 결과를 검사하라.

예:

```c
if (!proven_is_ok(proven_println("hello {}", PROVEN_ARG("world")))) {
    /* the write to stdout failed - a closed pipe, a full disk */
    proven_eprintln("stdout is not writable");
}
```

### 5.1. Formatting a type of your own

`PROVEN_ARG`는 `_Generic` 위에 세워졌고, `_Generic`은 컴파일 시점에 알려준 타입에
대해서만 디스패치할 수 있다. 당신의 타입은 알려줄 수 없다. 그래서 포매터의 argument
집합 — 정수, float, 문자열, 포인터, datetime — 은 **닫힌** 집합이었다: `rect_t`,
`uuid_t`, `vec3_t`는 `{}`에 전혀 넘길 수 없었다.

이를 우회하는 두 방법은 모두 나빴다. 값을 scratch 문자열로 미리 포매팅해서 *그것*을
넘기는 방법: 값마다 할당과 복사가 로깅 경로에서 발생하는데, 로깅 경로야말로 할당이
바로 실패한 상황에서도 계속 동작해야 하는 유일한 경로다. 아니면 필드를 하나씩
출력하고 컬럼 정렬은 포기하는 방법.

`PROVEN_ARG_OF(&obj, render)`가 그 문이다.

```c
proven_err_t render(proven_fmt_sink_t out, const void *obj);
```

그 시그니처의 형태로부터 세 가지가 따라 나온다.

- **렌더러는 버퍼가 아니라 sink를 받는다.** 렌더러는 공간이 얼마나 있는지 알 필요가
  없고, 무엇도 오버플로할 수 없다. `proven_fmt_put`으로 방출한다.
- **합성(compose)된다.** 렌더러는 포매터를 다시 호출할 수 있으며 — 스택 버퍼로,
  할당자 없이 — 그 결과를 sink에 넘길 수 있다. 필드 자체가 사용자 타입인 타입도
  자연스럽게 중첩된다.
- **Width, fill, alignment가 동작한다.** 포매터는 렌더러를 **두 번** 실행한다:
  먼저 counting sink에 대해 출력 폭을 알아내고, 그다음 실제로, 그 주위에 패딩을
  적용하여 실행한다. 이것이 `{:>10}`이 당신의 타입 컬럼을 int 컬럼을 정렬하는 것과
  똑같이 정렬하는 이유이며, 렌더러가 **결정적(deterministic)**이어야 하고 `obj`를
  변경해서는 안 되는 이유다. 두 패스가 불일치하면, 포매터는 정렬된 컬럼에 잘못된
  폭의 필드를 방출하고 나중에 알아차리게 하는 대신 `PROVEN_ERR_INVALID_ARG`를
  반환한다.

포매터가 **하지 않을** 일은 추측이다. 사각형에 대한 `{:x}`, UUID에 대한 `{:.2}`,
행렬에 대한 `{:+}` — 라이브러리는 그것들이 당신의 타입에 무엇을 의미하는지 전혀
모르므로, `PROVEN_ERR_INVALID_FORMAT`으로 거부한다. 그럴듯한 답을 지어내고 성공을
보고하는 것이야말로 포매터가 거짓말을 시작하는 방식이다. 당신이 요청하지 않은 타입
글자는 오류보다 낫지 않다.

테스트 스위트가 컴파일하고 실행함:

<!-- example: manual/examples/ex_08_fmt_custom.c -->
```c
/*
 * Formatting a type the library has never heard of.
 *
 * `PROVEN_ARG` is built on `_Generic`, which can only dispatch on types it was told
 * about - and it cannot be told about yours. So before `PROVEN_ARG_OF` existed, a
 * `rect_t` simply could not be printed. You either pre-formatted it into a scratch
 * string and passed that (an allocation and a copy per value, in the logging path,
 * which is the one path that must not allocate), or you printed the fields one at a
 * time and gave up on ever aligning the column.
 *
 * A renderer receives a *sink*, not a buffer. That is what makes it compose: it can
 * call the formatter again, and its output is just bytes going somewhere. And because
 * the formatter measures the renderer's output before emitting it - by running it once
 * against a counting sink - width, fill and alignment work on a user type exactly as
 * they do on an int.
 */

typedef struct { int w, h; } rect_t;

static proven_err_t render_rect(proven_fmt_sink_t out, const void *obj) {
    const rect_t *r = (const rect_t *)obj;

    /* Compose: the formatter, into a stack buffer, no allocator anywhere. */
    proven_byte_t tmp[64];
    proven_u8str_t s = proven_u8str_borrow(tmp, sizeof tmp);
    proven_fmt_result_t f = proven_u8str_append_fmt(&s, "{}x{}",
                                                    PROVEN_ARG(r->w), PROVEN_ARG(r->h));
    if (!PROVEN_FMT_IS_OK(f)) return f.err;

    return proven_fmt_put(out, proven_u8str_as_view(&s));
}

int main(void) {
    rect_t a = { .w = 1920, .h = 1080 };
    rect_t b = { .w = 640,  .h = 480  };

    proven_byte_t buf[128];
    proven_u8str_t line = proven_u8str_borrow(buf, sizeof buf);

    /* Just like any other argument. */
    proven_fmt_result_t r = proven_u8str_append_fmt(&line, "mode={}", PROVEN_ARG_OF(&a, render_rect));
    EXAMPLE_REQUIRE(PROVEN_FMT_IS_OK(r), "a user type should format");
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_as_view(&line), PROVEN_LIT("mode=1920x1080")),
                    "the renderer's bytes should be what came out");

    /* And it aligns, which is the whole reason the formatter measures it first: a
     * column of user-defined values lines up like a column of anything else. */
    (void)proven_u8str_reset(&line);
    r = proven_u8str_append_fmt(&line, "[{:>10}]\n[{:>10}]",
                                PROVEN_ARG_OF(&a, render_rect),
                                PROVEN_ARG_OF(&b, render_rect));
    EXAMPLE_REQUIRE(PROVEN_FMT_IS_OK(r), "two user types should format");
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_as_view(&line),
                                         PROVEN_LIT("[ 1920x1080]\n[   640x480]")),
                    "both rows should be right-aligned to the same width");

    /* A spec the library cannot interpret for your type is refused, not guessed at.
     * `{:x}` on a rectangle has no meaning, and answering it with something plausible
     * while reporting success is how a formatter starts lying to you. */
    (void)proven_u8str_reset(&line);
    r = proven_u8str_append_fmt(&line, "{:x}", PROVEN_ARG_OF(&a, render_rect));
    EXAMPLE_REQUIRE(r.err == PROVEN_ERR_INVALID_FORMAT,
                    "a type letter on a user type should be an error");

    return EXAMPLE_OK();
}
```

## 6. Console print helpers

이 섹션은 상세 I/O API가 Chapter 5에 있기 때문에 의도적으로 짧다.
포매터 사용자에게 중요한 점은 콘솔 헬퍼들이 문자열 append API와 같은 argument 규칙을 공유한다는 것이다.

흔한 실수:

- `proven_println`에 `PROVEN_SCAN_ARG`를 사용하기
- 모든 포맷 문자열에 `PROVEN_LIT`이 필요하다고 가정하기
- 출력 함수도 여전히 실패할 수 있다는 것을 잊기

## 7. Scanner data model

스캐너는 당신이 이미 가지고 있는 바이트 위의 커서다. 텍스트를 소유하지 않고,
복사하지 않으며, 어디에서도 읽지 않는다 - `proven_scan_t`는 뷰에 오프셋을 더한
것이며, 그것이 전부다.

```text
typedef struct {
    proven_u8str_view_t view;    /* the bytes being read; not owned */
    proven_size_t       cursor;  /* how far in we are */
} proven_scan_t;
```

이것이 `scanf`와 다른 이유이므로 언급할 가치가 있는 두 가지 결과:

- **스캐너는 절대 할당하지 않고 당신의 입력에 절대 기록하지 않는다.** 스캔된 단어는
  원본 바이트 *안*을 가리키는 `proven_u8str_view_t`로 반환된다. 그것은 정확히 그
  바이트가 유효한 동안만 유효하며, 그 이상은 아니다. 그것들보다 오래 살아야 한다면,
  `proven_u8str_create_from_view()`로 복사하라.
- **커서는 당신의 것이다.** 그것은 평범한 필드다. 저장하거나, 복원하거나, 손으로
  진행시킬 수 있다(§12는 `proven_scan_skip_until` 이후 정확히 그렇게 한다). 스캐너의
  어떤 것도 당신에게 숨겨져 있지 않으므로, 당신을 위해 되돌려야 할 것도 없다.

`proven_scan_init()`은 잘못된 형식의 뷰(`size > 0`인데 null 포인터)를 신뢰하는 대신
빈 뷰로 정규화한다. 따라서 쓰레기로부터 만들어진 스캐너는 역참조 대신 입력 끝(EOF)으로
읽힌다.

각 primitive는 값을 그것을 지키는 오류와 짝지은 result 구조체를 반환한다:
`proven_result_i64_t`, `proven_result_u64_t`, `proven_result_f64_t`,
`proven_result_u8str_view_t`. **오류가 `PROVEN_OK`가 아닌 한 값은 무의미하다** -
스캐너는 실패를 뜻하는 sentinel 값을 사용하지 않는데, 모든 sentinel은 동시에
정당한 입력이기도 하기 때문이다.

## 8. Scanner primitive APIs

```text
void                       proven_scan_skip_whitespace(proven_scan_t *scan);
proven_result_i64_t        proven_scan_i64(proven_scan_t *scan);
proven_result_u64_t        proven_scan_u64(proven_scan_t *scan);
proven_result_f64_t        proven_scan_f64(proven_scan_t *scan);
proven_result_u8str_view_t proven_scan_str(proven_scan_t *scan);
proven_err_t               proven_scan_skip_until(proven_scan_t *scan, proven_u8str_view_t target);
void                       proven_scan_skip_until_number(proven_scan_t *scan);
```

값을 반환하는 것들은 `[[nodiscard]]`다: 결과를 버리는 스캔은 애초에 할 필요가 없던 스캔이다.

### Shared behaviour

- **선행 공백은 건너뛴다** — 모든 값 스캐너가 그렇게 한다. 먼저
  `proven_scan_skip_whitespace()`를 호출할 필요가 없다. 그 함수는 커서를 직접
  위치시키고 싶을 때를 위해 존재한다.
- **스캐닝은 값에 속할 수 없는 첫 바이트에서 멈춘다.** `"12abc"`는 `12`를 내고
  커서를 `a`에 남긴다. 그것은 오류가 아니다 - 스캐너는 당신이 물은 질문에 답했고
  나머지는 다음에 묻는 이를 위해 남겼다.
- **실패 시 커서는 복원된다.** 따라서 실패한 스캔은 비사건(non-event)이다: 돌아서서
  같은 위치를 다른 것으로 파싱할 수 있다. §12가 이렇게 한다.

### The integer scanners

| 입력 | `proven_scan_i64` | 이유 |
|---|---|---|
| `"42"`, `"+42"`, `"-42"` | `OK` - 42, 42, -42 | 부호는 숫자의 일부다 |
| `"9223372036854775808"` | `PROVEN_ERR_OVERFLOW` | `INT64_MAX`보다 하나 큼. **랩(wrap)하지 않는다** |
| `"abc"`, `""` | `PROVEN_ERR_INVALID_ARG` | 여기에 숫자가 없다 |
| `"0x10"` | `OK` - **0**, 커서는 1에 | **십진수만**: 0, 그 뒤에 텍스트 |

마지막 행이 사람들을 놀라게 하는 것이다. `proven_scan_i64`와 `proven_scan_u64`는
십진수를 읽는다. hex도, 8진수도, 기수 접두어도 없다. `0x10`은 정수 0이고, `x10`은
여전히 입력에 남아 있다.

`proven_scan_u64`는 unsigned를 뜻한다: `"-1"`은 `18446744073709551615`로 랩하는 것이
아니라 `PROVEN_ERR_INVALID_ARG`다. 음수를 조용히 거대한 양수로 바꾸는 스캐너는
경계 검사가 무력화되는 방식이다.

### The float scanner

`proven_scan_f64`는 라이브러리의 나머지와 같은 정확히 반올림되는 십진 엔진을 거친다:
가장 가까운 값으로 반올림, 동점은 짝수로, 그 어디에도 `long double` 없음. `nan`과
`inf`를 받아들인다.

두 경계 동작은 **의도적으로 비대칭**이며, 그 비대칭이 요점이다.

- `"1e309"`는 `PROVEN_ERR_OVERFLOW`를 준다. 올바른 유한 답이 없으므로, 요청하지
  않은 무한대를 건네는 대신 거부한다.
- `"1e-400"`은 `PROVEN_OK`와 `0.0`을 주며, 부호는 보존된다. 0으로의 언더플로는
  정확히 반올림된 답 *그 자체*다. 그것을 오류로 보고하는 것은 올바른 산술을 실패로
  보고하는 것이 될 것이다.

### Words and navigation

`proven_scan_str`은 다음 공백으로 구분된 구간을 입력 안을 가리키는 뷰로 반환한다.
공백 외에 아무것도 남지 않은 경우는 `PROVEN_ERR_INVALID_ARG`다.

**완전한 뷰**에서는 "입력이 떨어졌다"와 "입력이 틀렸다"가 같은 사실이다 - 더 이상
입력이 없으므로, 끝에서 잘린 숫자는 정말로 잘못된 형식이다. **스트림** 위에서는 이
둘이 반대되는 사실이며, 그 차이가 기다릴지 오류를 보고할지를 결정한다. 파싱이 가진
것의 끝을 넘어 달렸을 때 스캐너는 `proven_scan_t::needs_more`를 설정하며, 버퍼드
스캐너는 정확히 그것을 사용해 다시 채우고 재시도한다: `-`를 전달한 뒤 잠시 후 `12`를
전달하는 파이프는 `-12`로 스캔된다. 이전에는 잘못된 형식의 숫자였다. 실제로 *존재하는*
잘못된 바이트 - 숫자가 와야 할 자리의 글자 - 는 여전히 오류이며, 이후 어떤 입력도
그것을 바꾸지 못한다. 스캐너는 그것을 기다리지 않는다.

`proven_scan_skip_until(scan, target)`은 커서를 target을 **지나서**가 아니라
target으로 옮긴다 - 그것을 얼마나 소비할지는 당신이 결정한다. target이 거기 없으면
결과는 `PROVEN_ERR_NOT_FOUND`이고 **커서는 움직이지 않는다**: 스캐너는 탐색에 실패한
입력을 소비하지 않는다.

`proven_scan_skip_until_number`는 첫 숫자에서, 또는 바로 뒤에 숫자가 따라오는
부호에서 멈춘다. 숫자가 없으면 커서를 입력의 끝까지 몰아간다 - 따라서 읽을 것이
있다고 가정하기 전에 `scan.cursor < scan.view.size`를 검사하라.

## 9. Scan argument model

스캔 argument는 컴파일러가 선택하는 **당신의 목적지를 가리키는 타입 있는 포인터**다.

```text
PROVEN_SCAN_ARG(&x)     /* _Generic on the pointer type */
```

이것이 스캐너가 `scanf`와 가장 첨예하게 다른 지점이다. 잘못 쓸 포맷 글자가 없는데,
포맷 글자가 아예 없기 때문이다. `long`에 대한 `%d`나, 너무 작은 버퍼에 대한 `%s`는
여기서 가능한 실수가 아니다: 목적지의 타입 *자체*가 명세이며, 불일치는 손상된
스택이 아니라 컴파일 오류다.

지원되는 목적지: `short`, `unsigned short`, `int`, `unsigned int`, `long`,
`unsigned long`, `long long`, `unsigned long long`, `double`, 그리고
`proven_u8str_view_t`.

`PROVEN_SCAN_ARG_LONG(&x)`와 `PROVEN_SCAN_ARG_ULONG(&x)`는 호출 지점에서 명시적이길
원하는 호출자를 위해 존재한다. `PROVEN_SCAN_ARG`는 이미 `long*`와 `unsigned long*`를
처리한다.

**좁은 목적지는 범위 검사된다.** `"70000"`을 `short`로 스캔하는 것은 잘린 `4464`가
아니라 `PROVEN_ERR_OVERFLOW`다. 값은 64비트로 파싱되어, 무언가를 저장하기 전에
목적지의 범위와 대조 검사된다.

`proven_scan_arg_*` 생성자들은 argument 배열을 손으로 만들어야 할 때를 위해
공개되어 있지만, 호출자가 사용하는 것은 매크로다.

## 10. Structural scan grammar

스캔 포맷 문자열은 포매터의 것을 거꾸로 읽은 것이다.

- 플레이스홀더는 argument 하나를 순서대로 소비한다.
- 그 외의 모든 것은 **입력과 정확히 일치해야 하는 리터럴**이다.

스캐닝 측에서는 플레이스홀더 안에 스펙이 없다. Width, fill, alignment는 포매팅의
관심사이며, 스캐너는 거기 있는 것을 읽는다.

포맷 안의 공백은 특별하지 않다. 값 스캐너들이 스스로 선행 공백을 건너뛰므로, 두
플레이스홀더 사이에 공백이 있는 포맷과 없는 포맷은 `"7 8"`을 동일하게 파싱한다 -
포맷의 공백은 입력의 공백과 일치하고, 그것이 없었더라도 두 번째 스캐너가 어차피
건너뛰었을 것이다.

플레이스홀더의 수는 argument의 수와 같아야 한다. 입력의 값이 너무 적은 것은 오류다.
**너무 많은 것은 오류가 아니다**(§11.1).

## 11. Scan formatting APIs

```text
proven_scan_fmt(view, fmt, ...)            /* scan a view from the beginning */
proven_scan_fmt_cursor(&scan, fmt, ...)    /* continue from an existing cursor */
proven_err_t proven_scan_fmt_internal(...) /* what the macros expand to */
```

자기완결적인 한 줄에는 `proven_scan_fmt`를 사용하라. 스캔이 같은 입력 위를 걷는 더
긴 여정의 한 단계일 때는 `proven_scan_fmt_cursor`를 사용하라: 이것은 당신이 소유한
커서를 진행시키므로, §8의 primitive들과 자유롭게 섞인다.

### 11.1. Scan error code guide and recovery

| 코드 | 실제로 일어난 일 | 할 일 |
|---|---|---|
| `PROVEN_OK` | 모든 플레이스홀더가 채워졌고 모든 리터럴이 일치했다. | 값들을 발행(publish)하라. |
| `PROVEN_ERR_INVALID_ARG` | 입력이 당신이 요청한 형태가 아니다 - 플레이스홀더가 읽을 값이 없었거나, 입력이 떨어졌다. | 그 줄은 일치하지 않는다. 보고하되, 같은 형태로 재시도하지 마라. |
| `PROVEN_ERR_NEED_MORE` | **버퍼드 스캐너 전용.** 토큰이 읽기 경계에서 반으로 잘렸다: 그 나머지가 아직 도착하지 않았다. 보통은 이것을 보지 않는다 - `proven_sysio_scanner_scan`이 당신을 위해 다시 채우고 재시도한다 - 이는 스캐너가 스스로에게 하는 말이다. | 아무것도. 이미 처리되었다. |
| `PROVEN_ERR_NOT_FOUND` | 포맷 안의 **리터럴**이 일치하지 않았다. | 그 줄은 예상과 다른 형태다. 저장된 커서에서 다른 포맷을 시도하라. |
| `PROVEN_ERR_OVERFLOW` | 숫자는 제대로 된 형식이었지만 목적지에 들어맞지 않는다. | 입력이 유효한데 목적지가 너무 좁은 것일 수도, 입력이 적대적인 것일 수도 있다. 그 둘은 매우 다른 상황이다 - 타입을 넓히기 전에 구별하라. |
| `PROVEN_ERR_INVALID_FORMAT` | 포맷 문자열 자체가 잘못된 형식이다. | 입력이 아니라 당신 코드의 버그다. |

**구조적 스캐너는 트랜잭션이 아니며, 이것이 물어뜯는 그 지점이다.**

리터럴이 일치에 실패했을 때, 불일치 *이전*의 플레이스홀더들은 이미 기록되어 있다.
호출은 `PROVEN_ERR_NOT_FOUND`를 반환하는데 당신의 목적지는 그래도 값을 담고 있다 -
아래의 `id`는 실패한 호출에서 온 7이다.

```text
int id = -1;
double ratio = -1.0;
proven_err_t err = proven_scan_fmt(line, "id={} XXX={}",
                                   PROVEN_SCAN_ARG(&id), PROVEN_SCAN_ARG(&ratio));
/* err == PROVEN_ERR_NOT_FOUND, and id == 7: it was written before the failure. */
```

그러므로: **실패 시 모든 목적지를 오염된(clobbered) 것으로 취급하라.** 전부 아니면
전무가 필요하면, 지역 변수로 스캔하고 호출이 성공한 뒤에만 발행하라 - §12의 예제가
그 형태를 보여준다. 대안으로, 호출 전에 `scan.cursor`를 저장하고 이후에 복원하라.
커서는 평범한 필드이며, 그것은 의도적이다.

**후행 입력은 오류가 아니다.** `"7 8"`에 대해 플레이스홀더 하나를 스캔하면 값 7로
성공하고 `8`은 소비되지 않은 채 남는다. 스캐너는 당신이 요청한 것을 일치시키고
멈췄다. 당신이 묻지 않은 것을 단속하지 않는다. 줄 전체가 소비되어야 한다면, 커서를
직접 검사하라.

```text
if (scan.cursor != scan.view.size) { /* there is unparsed input left */ }
```

## 12. Examples and misuse cases

### Worked example: formatting a line and scanning it back

테스트 스위트가 컴파일하고 실행함. 스택에서 빌린 문자열(오버플로 시 atomic)과
할당자 기반 문자열(성장함)로 포매팅하고, 신뢰할 수 없는 바이트에 경계 argument를
사용한 뒤, 그 줄을 다시 파싱한다 - float은 정확히 라운드트립된다.

<!-- example: manual/examples/ex_08_fmt_scan.c -->
```c
/*
 * Formatting and scanning are the two halves of the same idea: `{}` renders typed
 * values into text, and the scanner reads text back into typed destinations. Both
 * are type-checked at the call site (_Generic picks the constructor), so there is
 * no format-string/argument mismatch to get wrong at runtime.
 *
 * The choice that matters is *where the bytes go*:
 *
 *   append_fmt       - fixed capacity, atomic. Too long? Nothing is written and
 *                      you get PROVEN_ERR_OUT_OF_BOUNDS. No allocator involved,
 *                      so it works on a stack buffer.
 *   append_fmt_grow  - allocator-backed. Grows to fit; on allocation failure the
 *                      string is left exactly as it was.
 */

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    /* --- fixed capacity: no allocator, no allocation ------------------------ */
    /* borrow wraps caller memory, so this string lives entirely on the stack. `cap`
     * includes the NUL, so 32 bytes hold 31 of content. Nothing to destroy. */
    proven_byte_t stack_buf[32];
    proven_u8str_t fixed = proven_u8str_borrow(stack_buf, sizeof stack_buf);

    proven_fmt_result_t r = proven_u8str_append_fmt(&fixed, "port={}", PROVEN_ARG(8080));
    EXAMPLE_REQUIRE(PROVEN_FMT_IS_OK(r), "a short line should fit in 32 bytes");
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_as_view(&fixed), PROVEN_LIT("port=8080")),
                    "the fixed-capacity append should have rendered the port");

    /* Atomic means atomic: an append that does not fit changes nothing. The string
     * is still valid and still holds what it held before - no truncated tail to
     * clean up. (Use append_fmt_trunc if a truncated tail is what you want.) */
    proven_fmt_result_t too_long = proven_u8str_append_fmt(
        &fixed, " and a great deal more text than will ever fit here {}", PROVEN_ARG(1));
    EXAMPLE_REQUIRE(too_long.err == PROVEN_ERR_OUT_OF_BOUNDS, "the overlong append must fail");
    EXAMPLE_REQUIRE(too_long.required > too_long.written, "it reports what it would have needed");
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_as_view(&fixed), PROVEN_LIT("port=8080")),
                    "a failed atomic append must leave the string untouched");

    /* --- specs: fill, align, width, hex ------------------------------------- */
    proven_result_u8str_t created = proven_u8str_create(alloc, 8);   /* deliberately small */
    EXAMPLE_REQUIRE(proven_is_ok(created.err), "creating the output string should succeed");
    if (!proven_is_ok(created.err)) return 1;
    proven_u8str_t out = created.value;

    /* grow reallocates as needed, so the initial capacity is a hint, not a limit.
     * `{:0>4}` = fill '0', align right, width 4. `{:x}` = lowercase hex, no 0x. */
    r = proven_u8str_append_fmt_grow(alloc, &out, "id={:0>4} tag={:*^9} addr=0x{:x}",
                                     PROVEN_ARG(7),
                                     PROVEN_ARG(PROVEN_LIT("ok")),
                                     PROVEN_ARG(48879));
    EXAMPLE_REQUIRE(PROVEN_FMT_IS_OK(r), "the growing append should succeed");
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_as_view(&out),
                                         PROVEN_LIT("id=0007 tag=***ok**** addr=0xbeef")),
                    "fill/align/width/hex should render exactly this");
    printf("%s\n", proven_u8str_as_cstr(&out));

    /* --- untrusted text is bounded, never trusted to be NUL-terminated ------ */
    /* PROVEN_ARG on a char* means "walk it until a NUL turns up" - fine for a
     * literal, a buffer-overread for anything that came off a socket. This buffer
     * has no NUL at all; PROVEN_ARG_CSTR_N stops at the length instead, so it reads
     * only what actually exists. Use it for anything you did not create yourself. */
    const char untrusted[4] = {'a', 'b', 'c', 'd'};   /* no terminator, on purpose */
    EXAMPLE_REQUIRE(proven_is_ok(proven_u8str_reset(&out)), "reset should keep the buffer");
    r = proven_u8str_append_fmt_grow(alloc, &out, "payload={}",
                                     PROVEN_ARG_CSTR_N(untrusted, sizeof untrusted));
    EXAMPLE_REQUIRE(PROVEN_FMT_IS_OK(r), "the bounded append should succeed");
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_as_view(&out), PROVEN_LIT("payload=abcd")),
                    "the bounded argument should render its whole 4 bytes and stop");

    /* --- format a record, then scan it back --------------------------------- */
    proven_i64 sensor_id = 42;
    double reading = 3.14159;

    EXAMPLE_REQUIRE(proven_is_ok(proven_u8str_reset(&out)), "reset should keep the buffer");
    r = proven_u8str_append_fmt_grow(alloc, &out, "{} {} {}",
                                     PROVEN_ARG(sensor_id),
                                     PROVEN_ARG(PROVEN_LIT("boiler")),
                                     PROVEN_ARG(reading));
    EXAMPLE_REQUIRE(PROVEN_FMT_IS_OK(r), "formatting the record should succeed");
    printf("record: %s\n", proven_u8str_as_cstr(&out));

    /* One scanner over one view. Each call advances the cursor past what it
     * consumed, so the calls compose left to right - and each one can fail
     * independently, which is the difference between a parser and a guess. */
    proven_scan_t sc = proven_scan_init(proven_u8str_as_view(&out));

    proven_result_i64_t id = proven_scan_i64(&sc);
    EXAMPLE_REQUIRE(proven_is_ok(id.err), "the first field should parse as an integer");
    EXAMPLE_REQUIRE(id.val == sensor_id, "the integer should round-trip");

    /* scan_str returns a view *into the scanned string* - it copies nothing and
     * owns nothing, so it is only valid while `out` is. */
    proven_result_u8str_view_t name = proven_scan_str(&sc);
    EXAMPLE_REQUIRE(proven_is_ok(name.err), "the second field should parse as a word");
    EXAMPLE_REQUIRE(proven_u8str_view_eq(name.val, PROVEN_LIT("boiler")), "the name should round-trip");

    proven_result_f64_t temp = proven_scan_f64(&sc);
    EXAMPLE_REQUIRE(proven_is_ok(temp.err), "the third field should parse as a float");

    /* Exactly equal, not approximately: the scanner is correctly rounded, so it
     * returns the nearest double to the text - and the text the formatter produced
     * (six fractional digits) names this value unambiguously. Bit-for-bit, this is
     * the same double we started with. For a value that needs more than six
     * fractional digits, format it with the shortest policy
     * (proven_float_format_options_shortest) and the same round-trip holds. */
    EXAMPLE_REQUIRE(temp.val == reading, "the float must round-trip exactly, not approximately");

    /* The input is fully consumed: nothing was silently left on the table. */
    proven_result_i64_t extra = proven_scan_i64(&sc);
    EXAMPLE_REQUIRE(!proven_is_ok(extra.err), "there should be nothing left to scan");

    proven_u8str_destroy(alloc, &out);
    return EXAMPLE_OK();
}
```

### Worked example: the scanner's error codes, and recovering from them

테스트 스위트가 컴파일하고 실행함. 위 표의 모든 코드를 여기서 일부러 유발한다.
비-트랜잭션 실패까지 포함하는데 - 읽기만 한 계약은 배우지 못한 계약이기 때문이다.

<!-- example: manual/examples/ex_08_scan_recovery.c -->
```c
/*
 * The scanner's error codes, and how to recover from them.
 *
 * The scanner is not scanf. It never writes through a pointer it was not given,
 * it never guesses a width, and it tells you which of several different things
 * went wrong. That last part only helps if you know what the codes mean - so
 * this program provokes each one on purpose.
 */

static proven_u8str_view_t v(const char *s) {
    return proven_u8str_view_from_cstr(s);
}

int main(void) {
    /* --- the primitives restore the cursor when they fail ----------------- */
    /* A failed scan is a non-event: the cursor is where it was, so you can try
     * to parse the same position as something else. */
    {
        proven_scan_t sc = proven_scan_init(v("abc"));
        proven_result_i64_t n = proven_scan_i64(&sc);
        EXAMPLE_REQUIRE(n.err == PROVEN_ERR_INVALID_ARG, "'abc' is not an integer");
        EXAMPLE_REQUIRE(sc.cursor == 0, "a failed integer scan leaves the cursor alone");

        /* So the same position can be read as a word instead. */
        proven_result_u8str_view_t w = proven_scan_str(&sc);
        EXAMPLE_REQUIRE(proven_is_ok(w.err) && proven_u8str_view_eq(w.val, PROVEN_LIT("abc")),
                        "the same bytes parse fine as a word");
    }

    /* --- a number that does not fit is OVERFLOW, not a wrapped value ------ */
    {
        proven_scan_t sc = proven_scan_init(v("9223372036854775808"));   /* INT64_MAX + 1 */
        proven_result_i64_t n = proven_scan_i64(&sc);
        EXAMPLE_REQUIRE(n.err == PROVEN_ERR_OVERFLOW, "one past INT64_MAX must not wrap");
        EXAMPLE_REQUIRE(sc.cursor == 0, "the cursor is restored on overflow too");
    }

    /* --- but a float that underflows is NOT an error ---------------------- */
    /* Too large is OVERFLOW; too small is zero, with the sign kept. That
     * asymmetry is deliberate - underflow to zero is the correctly rounded
     * answer, while overflow has no correct finite answer at all. */
    {
        proven_scan_t big = proven_scan_init(v("1e309"));
        proven_result_f64_t b = proven_scan_f64(&big);
        EXAMPLE_REQUIRE(b.err == PROVEN_ERR_OVERFLOW, "1e309 does not fit a double");

        proven_scan_t tiny = proven_scan_init(v("-1e-400"));
        proven_result_f64_t t = proven_scan_f64(&tiny);
        EXAMPLE_REQUIRE(proven_is_ok(t.err), "1e-400 underflows, which is not an error");
        EXAMPLE_REQUIRE(t.val == 0.0, "it rounds to zero");
    }

    /* --- the integer scanners are decimal only ---------------------------- */
    /* "0x10" is not sixteen. It is a zero, followed by text the scanner has not
     * been asked to look at. This surprises people, so it is worth knowing. */
    {
        proven_scan_t sc = proven_scan_init(v("0x10"));
        proven_result_i64_t n = proven_scan_i64(&sc);
        EXAMPLE_REQUIRE(proven_is_ok(n.err) && n.val == 0, "0x10 scans as the integer 0");
        EXAMPLE_REQUIRE(sc.cursor == 1, "and the cursor stops before the 'x'");
    }

    /* --- scanning stops at the first byte that cannot belong to the value -- */
    {
        proven_scan_t sc = proven_scan_init(v("12abc"));
        proven_result_i64_t n = proven_scan_i64(&sc);
        EXAMPLE_REQUIRE(proven_is_ok(n.err) && n.val == 12, "12abc yields 12");
        EXAMPLE_REQUIRE(sc.cursor == 2, "and leaves 'abc' for whoever asks next");
    }

    /* --- unsigned means unsigned ------------------------------------------ */
    {
        proven_scan_t sc = proven_scan_init(v("-1"));
        proven_result_u64_t n = proven_scan_u64(&sc);
        EXAMPLE_REQUIRE(n.err == PROVEN_ERR_INVALID_ARG,
                        "-1 is rejected rather than wrapping to a huge unsigned value");
    }

    /* --- navigating to a value: skip_until ------------------------------- */
    /* skip_until leaves the cursor ON the target, not past it, so you decide
     * how much of it to consume. */
    {
        proven_scan_t sc = proven_scan_init(v("port=8080"));
        proven_err_t err = proven_scan_skip_until(&sc, PROVEN_LIT("="));
        EXAMPLE_REQUIRE(proven_is_ok(err), "the '=' is there");
        EXAMPLE_REQUIRE(sc.cursor == 4, "the cursor sits on the '=' itself");

        ++sc.cursor;                                  /* step over it */
        proven_result_i64_t port = proven_scan_i64(&sc);
        EXAMPLE_REQUIRE(proven_is_ok(port.err) && port.val == 8080, "the port parses");

        /* Not finding it is NOT_FOUND, and the cursor does not move - the
         * scanner does not consume the input it failed to navigate. */
        proven_scan_t sc2 = proven_scan_init(v("port=8080"));
        proven_err_t missing = proven_scan_skip_until(&sc2, PROVEN_LIT("#"));
        EXAMPLE_REQUIRE(missing == PROVEN_ERR_NOT_FOUND, "there is no '#'");
        EXAMPLE_REQUIRE(sc2.cursor == 0, "and the cursor stayed put");
    }

    /* --- the structural scanner ------------------------------------------- */
    {
        int id = 0;
        double ratio = 0.0;
        proven_u8str_view_t name = {0};

        proven_err_t err = proven_scan_fmt(v("id=7 ratio=0.5 name=ada"),
                                           "id={} ratio={} name={}",
                                           PROVEN_SCAN_ARG(&id),
                                           PROVEN_SCAN_ARG(&ratio),
                                           PROVEN_SCAN_ARG(&name));
        EXAMPLE_REQUIRE(proven_is_ok(err), "the line matches the shape");
        EXAMPLE_REQUIRE(id == 7 && ratio == 0.5, "the values land in the right places");
        EXAMPLE_REQUIRE(proven_u8str_view_eq(name, PROVEN_LIT("ada")), "including the word");
    }

    /* --- the structural scanner is NOT transactional ---------------------- */
    /*
     * This is the one that bites. When a literal fails to match, the scan
     * returns an error - but the placeholders BEFORE the mismatch have already
     * been written through. `id` is 7 even though the call failed.
     *
     * So: on failure, treat every destination as clobbered. If you need
     * all-or-nothing, scan into locals and only publish them once the call
     * succeeded, which is what the code below does.
     */
    {
        int id = -1;
        double ratio = -1.0;
        proven_err_t err = proven_scan_fmt(v("id=7 ratio=0.5"),
                                           "id={} XXX={}",       /* the literal is wrong */
                                           PROVEN_SCAN_ARG(&id),
                                           PROVEN_SCAN_ARG(&ratio));
        EXAMPLE_REQUIRE(err == PROVEN_ERR_NOT_FOUND, "the literal 'XXX=' is not in the input");
        EXAMPLE_REQUIRE(id == 7, "and yet id was already written: the scan is not atomic");

        /* The safe shape: scan into locals, publish on success. */
        int good_id = 0;
        double good_ratio = 0.0;
        int published_id = -1;
        proven_err_t ok = proven_scan_fmt(v("id=7 ratio=0.5"), "id={} ratio={}",
                                          PROVEN_SCAN_ARG(&good_id), PROVEN_SCAN_ARG(&good_ratio));
        if (proven_is_ok(ok)) published_id = good_id;
        EXAMPLE_REQUIRE(published_id == 7, "publish only what a successful scan produced");
    }

    /* --- running out of input, and having input left over ------------------ */
    {
        int a = 0, b = 0;
        proven_err_t short_input = proven_scan_fmt(v("5"), "{} {}",
                                                   PROVEN_SCAN_ARG(&a), PROVEN_SCAN_ARG(&b));
        EXAMPLE_REQUIRE(!proven_is_ok(short_input), "two placeholders, one value: that fails");

        /* Trailing input is NOT an error. The scanner matched what you asked for
         * and stopped; it does not police what you did not ask about. If the
         * whole line must be consumed, check that yourself. */
        int only = 0;
        proven_scan_t sc = proven_scan_init(v("7 8"));
        proven_err_t err = proven_scan_fmt_cursor(&sc, "{}", PROVEN_SCAN_ARG(&only));
        EXAMPLE_REQUIRE(proven_is_ok(err) && only == 7, "the first value scans");
        EXAMPLE_REQUIRE(sc.cursor < sc.view.size, "and '8' is still sitting there, unconsumed");
    }

    /* --- narrow destinations are range-checked ---------------------------- */
    {
        short small = 0;
        proven_err_t err = proven_scan_fmt(v("70000"), "{}", PROVEN_SCAN_ARG(&small));
        EXAMPLE_REQUIRE(err == PROVEN_ERR_OVERFLOW,
                        "70000 does not fit a short, and the scanner says so rather than truncating");
    }

    return EXAMPLE_OK();
}
```

### Misuse: assuming `0x10` is sixteen

그것은 0이다. 정수 스캐너들은 십진수 전용이며, `x10`은 여전히 입력에 남아 있다.
hex가 필요하면, 그 자릿수 루프는 당신이 직접 작성하는 것이다.

### Misuse: treating trailing input as an error

그것은 오류가 아니다. `"7 8"`에 대한 플레이스홀더 하나는 성공한다. 신경 쓰인다면
커서를 검사하라.

### Misuse: trusting destinations after a failed scan

그것들은 오염되었다. §11.1을 참조하라.

### Misuse: keeping a scanned word after its input is gone

`proven_scan_str`은 복사본이 아니라 **입력 안을 가리키는 뷰**를 반환한다. 버퍼가
사라지면, 그 단어도 사라진다. 그것이 온 바이트보다 오래 살아야 한다면
`proven_u8str_create_from_view()`로 복사하라.

## 13. Freestanding and build-mode notes

스캐너는 코어다: I/O를 하지 않고, 아무것도 할당하지 않으며, 어떤 플랫폼 레이어도
건드리지 않는다. 따라서 freestanding 빌드에서도 hosted 빌드에서와 정확히 똑같이
사용할 수 있다.

유일한 빌드 모드 의존성은 float다. Freestanding 빌드는 `PROVEN_FMT_NO_FLOAT`(float
*포매터*를 컴파일에서 제외)와 `PROVEN_NO_U16STR`를 설정한다. `proven_scan_f64`는
십진 파싱 엔진(`float_parse.c`와 `float_decimal.c`)을 끌어오는데, 이는 정수 전용이며
- `long double` 없음, libm 없음, 소프트 float 헬퍼 호출 없음 - 코드 크기 면에서
공짜는 아니다. 그것이 중요한 타겟이면서 float을 읽을 필요가 없다면, 그저 호출하지
마라: 스캐너의 다른 어떤 것도 float 경로를 참조하지 않는다.

`proven_sysio_scanner_*`(Chapter 5)는 **다른** 것이다: 파일 위의 버퍼드 스캐너로,
I/O를 하기 때문에 hosted 전용이다. 이 장에서 설명한 스캐너는 당신이 이미 가지고
있는 바이트를 읽는다.
