# Chapter 0: 여기서부터 시작

**Part I — 여기서 시작합니다.** 입문용 C 책 한 권 외에 사전 지식은 필요 없습니다.
**이 장을 마치면** `proven`을 링크한 프로그램을 빌드할 수 있고, 다른 어떤 장이든 읽을 수 있으며,
모르는 용어를 찾아볼 수 있습니다.

> **문체에 대해.** 이 장만 경어체(합니다체)로 쓰였고, 1장부터의 레퍼런스 장들은 평서체(해라체)를
> 씁니다. 의도한 것입니다 — 0장은 독자에게 직접 말을 거는 입문 장이고, 나머지는 찾아보는 문서이기
> 때문입니다.

## 목차

1. [이 매뉴얼은 누구를 위한 것인가](#1-이-매뉴얼은-누구를-위한-것인가)
2. [이 라이브러리가 존재하는 이유](#2-이-라이브러리가-존재하는-이유)
3. [첫 프로그램](#3-첫-프로그램)
4. [빌드와 include](#4-빌드와-include)
5. [모든 페이지에서 만나게 될 다섯 가지 계약](#5-모든-페이지에서-만나게-될-다섯-가지-계약)
6. [부록 B: 용어집](#6-부록-b-용어집)
7. [부록 D: libc 대응표](#7-부록-d-libc-대응표)
8. [다음에 읽을 것](#8-다음에-읽을-것)

---

## 1. 이 매뉴얼은 누구를 위한 것인가

이 매뉴얼은 여러분이 입문용 C 책을 한 권 끝냈다고 가정합니다. 구체적으로는 변수와 제어 흐름,
함수, 배열, `struct`, 포인터와 `*`/`&`, `malloc`과 `free`, `printf`, `strlen`을 쓰는 `char *`
문자열, 그리고 `gcc main.c -o main`으로 프로그램을 컴파일하는 것에 익숙하다고 봅니다. 이것들이
모두 익숙하다면 충분합니다.

반대로 다음은 **가정하지 않습니다**: 규율로서의 ownership(소유), 빌려 쓰는(borrowed) 데이터와 소유(owned) 데이터의
차이, 아레나(arena)나 풀(pool), 인터페이스로 쓰이는 함수 포인터 테이블, C23 어트리뷰트, 컴파일러가 적극적으로
이용해 먹는 대상으로서의 미정의 동작(undefined behaviour), "그냥 되던데" 수준을 넘어선 정렬
(alignment), atomic, 그리고 라이브러리가 최선을 다하는 대신 연산을 *거부*할 수도 있다는 발상.
이 하나하나는 처음 등장하는 자리에서 설명하며, 전부 §6의 용어집에도 있습니다.

이 문서는 C 튜토리얼이 아닙니다. 포인터가 무엇인지는 설명하지 않습니다. 대신 이 라이브러리가 왜
`errno`를 설정하는 대신 에러가 담긴 `struct`를 돌려주는지는 길게 설명합니다. 그것은 여러분이
동의하지 않을 권리가 있는 설계 결정이고, 아무도 설명해 주지 않은 결정에는 반대할 수조차 없기
때문입니다.

---

## 2. 이 라이브러리가 존재하는 이유

C는 여러분에게 거의 아무것도 주지 않으면서 여러분을 완전히 믿습니다. 그것이 C의 큰 장점입니다 —
런타임도 없고, 숨은 할당도 없고, 여러분이 쓰지 않은 비용도 없습니다 — 그리고 그래서 C는 여전히
운영체제와 임베디드 장치, 작고 예측 가능해야 하는 모든 것의 언어입니다.

그리고 같은 다섯 가지 버그가 50년째 출하되고 있는 이유이기도 합니다. 이 라이브러리는 그 다섯
가지 버그에 대한 답의 모음입니다. 각 답에는 대가가 따르고, 이 절은 그 대가가 무엇인지 말합니다.

### 문자열 함수는 무엇이 얼마나 큰지 모릅니다

```text
char buf[64];
strcpy(buf, name);            /* wrong: how long is name? strcpy never asks */
strcat(buf, ", welcome!");    /* wrong: and how much room is left now? */
```

`strcpy`는 목적지 포인터와 원본 포인터를 받습니다. 그 시그니처 어디에도 목적지의 크기가 실려
있지 않으므로, 아무것도 그것을 검사할 수 없습니다. 이 함수는 64바이트 버퍼의 200번째 바이트에도
기꺼이 쓰고, 그렇게 망가뜨리는 것은 컴파일러가 마침 그 뒤에 배치한 무엇이든입니다 — 흔히는
반환 주소입니다. 이것은 부주의한 사람들이 가끔 저지르는 실수가 아닙니다. 이 언어의 역사에서 가장
많이 악용된 버그 유형이며, API가 그것을 *기본* 동작으로 만들어 두었습니다.

`strncpy`는 전통적인 해답이지만 그 자체가 또 하나의 함정입니다. 항상 NUL로 종단하지는 않기
때문에, "안전한" 버전이 조용히 문자열이 아닌 문자열을 만들어 냅니다.

**이 라이브러리는 대신 이렇게 합니다.** 문자열이 자기 길이를 지니고 다닙니다.
`proven_u8str_view_t`는 포인터 *와* 크기가 항상 함께 있는 것입니다. append는 목적지의 용량을
알기 때문에 그 용량을 검사하고, 텍스트가 들어가지 않으면 **에러를 반환하고 아무것도 쓰지
않습니다** — 자르지 않습니다. 잘린 경로는 틀린 경로이고, 잘린 명령은 다른 명령이기 때문입니다.
[챕터 3](manual-03-strings-text-ko.md)을 참조하세요.

**대가.** 모든 문자열이 한 워드가 아니라 두 워드이고, NUL 종단 형태를 따로 요청하지 않으면
`proven` 문자열을 `printf("%s")`에 바로 넘길 수 없습니다.

### 실패를 확인하게 만드는 장치가 없습니다

```text
char *p = malloc(n);
p[0] = 'x';                   /* wrong: malloc returns NULL when it fails */
```

`malloc`은 `NULL`을 반환해서 실패를 알리는데, 여러분이 끝내 들여다보지 않아도 C는 한마디도 하지
않습니다. `fopen`도, `realloc`도, 센티널 값을 반환하는 모든 함수가 마찬가지입니다. 에러는
*제공되어* 있고, 알아차리는 것은 선택 사항입니다.

`errno`는 더 나쁩니다. 호출 이후까지 살아남는 전역이기 때문입니다. 다른 라이브러리 호출이
덮어쓸 기회를 갖기 전에, 정확히 알맞은 순간에 확인해야 하고, 성공한 호출도 거기에 쓰레기 값을
넣을 수 있다는 사실을 기억해야 합니다.

**이 라이브러리는 대신 이렇게 합니다.** 실패할 수 있는 함수는 에러를 *값으로* 반환하고, 돌려줄
결과까지 있을 때는 둘이 하나의 `struct`에 담겨 함께 돌아옵니다:

```text
proven_result_u8str_t s = proven_u8str_create(alloc, 64);
if (!proven_is_ok(s.err)) return 1;      /* s.value means nothing until you check */
```

하는 일 자체가 실패할 수 있는 함수에는 `[[nodiscard]]`가 붙어 있어서, 에러를 버리는 코드는
컴파일러가 빌드를 거부합니다. 물론 의도적으로 무시할 수는 있습니다 — 호출 앞에 `(void)`를 붙이면
됩니다 — 그리고 그것을 타이핑해야 한다는 점이 바로 핵심입니다.
[챕터 1](manual-01-foundation-ko.md)을 참조하세요.

**대가.** `if`가 늘어납니다. 깊은 곳의 실패에서 한 번에 빠져나오게 해 주는 예외 메커니즘이
없으므로, 에러 경로가 코드의 모양에 그대로 드러납니다. 그 드러남이 바로 기능입니다.

### `printf`는 여러분이 말하는 것을 그대로 믿습니다

```text
printf("%d\n", 3.0);          /* wrong: %d with a double. This compiles. */
printf("%s\n", 42);           /* wrong: and this one crashes */
```

포맷 문자열은 런타임에 아무도 검사하지 않습니다. 요즘 컴파일러는 리터럴 포맷에 대해 경고해 주는데,
포맷이 변수가 되는 순간까지만 도움이 됩니다. 그다음부터는 varargs 스택에 마침 들어 있는 바이트를
포맷 문자열이 요구한 모양대로 읽어 대는 함수로 되돌아갑니다.

**이 라이브러리는 대신 이렇게 합니다.** `{}`는 타입이 들어 있지 않은 자리표시자이고, 타입은
인자에서 오며 `_Generic`으로 컴파일 타임에 결정됩니다:

```text
proven_println("{} is {}", PROVEN_ARG(name), PROVEN_ARG(count));
```

타입을 두 번 쓰지 않았으므로 `%d`와 `double`이 어긋날 여지 자체가 없습니다. 입문용 설명은
[챕터 3 §3](manual-03-strings-text-ko.md)을, 전체 문법은 [챕터 8](manual-08-fmt-scan-ko.md)을
보세요.

**대가.** 인자마다 `PROVEN_ARG`로 감싸야 하고, 손에 익은 것과는 다른 포맷 언어를 써야 합니다.

### 이건 누가 free하나요?

```text
char *s = build_message();    /* do I free this? the type does not say */
```

함수가 반환한 `char *`는 갓 할당된 것일 수도, 호출자의 버퍼를 가리킬 수도, 읽기 전용 메모리에 있는
문자열 리터럴일 수도, 다음 호출이 덮어쓸 정적 버퍼를 가리킬 수도 있습니다. 네 경우 모두 타입은
똑같습니다. 답은 문서에 있고, 문서는 코드와 어긋나기 마련입니다.

**이 라이브러리는 대신 이렇게 합니다.** ownership이 타입 이름과 시그니처에 드러나 있습니다.
`proven_u8str_t`는 **owned**입니다 — `_create`에서 받았으니 같은 할당자(allocator)로 `_destroy`해야
합니다. `proven_u8str_view_t`는 **borrowed**입니다 — 다른 누군가가 소유한 바이트를 가리키므로
절대 destroy하지 않고, 소유자가 사라지는 순간 유효하지 않게 됩니다. 할당할 수 있는 함수는 모두
allocator를 파라미터로 받으므로, allocator가 없는 시그니처는 할당할 수 없습니다.

**대가.** C에 하나뿐이던 타입이 둘이 되고, 모든 경계에서 "이건 누가 소유하지?"를 물어야 하는
규율이 생깁니다 — 어차피 치르던 비용을, 더 늦게 디버거 안에서 치르는 대신 지금 치르는 것입니다.

### 아무도 타입 검사를 해 줄 수 없는 비교 함수

```text
qsort(a, n, sizeof *a, cmp);  /* cmp takes const void*; get it wrong and it is UB */
```

`qsort`는 비교자를 `void *` 인터페이스로 받으므로, 파라미터 타입이 틀린 비교자도 그대로
컴파일됩니다. 이 버그의 고전적인 형태는 가리키는 대상 대신 포인터 자체를 비교하는 것인데, 그러면
잘 실행되고 아무것도 제대로 정렬하지 않으면서 절대 죽지 않는 프로그램이 나옵니다.

**이 라이브러리는 대신 이렇게 합니다.** 모양은 똑같은 `void *`입니다 — 여기는 C이고, 다른 방법은
없습니다 — 대신 라이브러리가 계약을 정확히 문서화하고, 복사해 쓸 수 있는 동작하는 비교자를
제공하며, `proven_array_sort`는 공격자가 고른 입력에서 *O(n²)*으로 퇴화하는 퀵소트가 아니라
*O(n log n)*을 보장하는 introsort입니다.
[챕터 4](manual-04-containers-algorithms-ko.md)를 참조하세요.

### 바이트에는, 여러분이 정해 주지 않아도, 타입이 있습니다

여러분은 메모리를 바이트로 생각합니다. C의 추상 기계는 그렇지 않습니다. 메모리를 *타입이 있는* 것으로
다루며, 같은 바이트를 서로 다른 두 타입의 포인터로 읽는 것은 미정의 동작입니다 — 그리고 컴파일러는
그것을, 최적화가 켜졌을 때만, 조용히 이용해도 됩니다. 이것을 **strict aliasing**이라 부르며, 바이트
버퍼를 폭이 다른 포인터로 읽는 손수 짠 모든 파서 아래에 깔린 함정입니다:

```text
void *buf = malloc(8);
uint32_t *w = buf;      /* 같은 메모리를 32비트로 봄 — 캐스트도, 경고도 없음 */
uint16_t *h = buf;      /* 같은 메모리를 16비트로 봄 */
*w = 0xAAAAAAAAu;
*h = 0x1234;            /* 하위 절반을 바꿈 */
printf("%08x\n", *w);   /* aaaa1234를 기대하면 틀림: -O2에서는 aaaaaaaa가 찍힘 */
```

`-O0`으로 컴파일하면 `aaaa1234`가, `-O2`에서는 `aaaaaaaa`가 찍힙니다. 컴파일러가 `uint16_t` 쓰기와
`uint32_t` 읽기는 같은 메모리를 건드릴 수 없다고 가정하고 그 쓰기를 지워 버렸기 때문입니다. 경고는
없고, 프로그램은 디버그 빌드에서 돌린 모든 테스트를 통과했습니다. 이것이 리눅스 커널이
`-fno-strict-aliasing`으로 컴파일해 피하는 부류의 버그입니다 — 규칙 하나 때문에 플래그 하나를 통째로.

**이 라이브러리는 대신 이렇게 합니다.** 원시 메모리는 `unsigned char`의 별칭인 `proven_byte_t`입니다
— 이 규칙이 명시적으로 면제하는 유일한 타입입니다. 표준이 어떤 객체의 바이트든 그 타입으로 들여다보는
것을 허용하기 때문입니다. 평범한 API는 여러분의 바이트를 몰래 더 넓은 타입으로 재해석하지 않으므로, 위
버그는 그것을 통해서는 쓸 수가 없습니다. (strict aliasing에는 더 미묘한 형제 *provenance*가 있고,
라이브러리 이름이 거기서 왔습니다. [챕터 6 §3](manual-06-execution-and-platform-ko.md)과 프로젝트
README가 그것을 다룹니다.)

### 이 라이브러리가 아닌 것

이것은 프레임워크가 아니며, 여러분의 `main`을 차지할 생각이 없습니다. 초기화해야 할 전역 상태가
없고, 스레드를 띄우지 않으며, `atexit` 핸들러를 등록하지 않고, 여러분이 allocator를 건네주지 않은
것은 아무것도 할당하지 않습니다. 모든 모듈은 단독으로 쓸 수 있습니다. 대부분은 운영체제가 전혀
없어도 돌아갑니다 — [프리스탠딩(freestanding) 모드](manual-freestanding-ko.md)를 보세요.

| 문제 | C가 주는 것 | `proven`이 주는 것 | 대가 |
|---|---|---|---|
| 버퍼 오버런 | `strcpy`, `strcat` — 어디에도 크기가 없음 | view가 길이를 지님; 쓰기는 자르는 대신 거부 | 문자열당 두 워드 |
| 확인되지 않은 실패 | `NULL` 반환과 `errno` | 에러를 값으로 반환, 버려서는 안 되는 것에는 `[[nodiscard]]` | `if`가 늘어남 |
| 포맷 불일치 | `printf`는 포맷 문자열을 믿음 | 타입을 인자에서 가져오는 `{}` | 호출마다 `PROVEN_ARG` |
| 불분명한 ownership | `char *`가 네 가지 서로 다른 것을 의미 | owned와 borrowed가 서로 다른 타입 | 타입이 하나에서 둘로 |
| 숨은 할당 | 무엇이든 `malloc`을 부를 수 있음 | allocator를 받는 함수만 할당 가능 (한정된 예외 하나: 과도하게 긴 줄에서의 `proven_println`) | 시그니처에 파라미터가 하나 더 |
| 숨은 타입을 가진 바이트 | 메모리를 더 넓은 포인터로 재해석하면 최적화기가 이용하는 UB | 원시 바이트는 규칙이 면제하는 타입 `proven_byte_t`를 거침 | — |

---

## 3. 첫 프로그램

이것이 전부입니다. 모든 줄이 §5의 계약 중 하나이고, 빌드가 바로 이 파일을 컴파일하고 실행하므로
조용히 사실이 아니게 될 수 없습니다.

<!-- example: manual/examples/ex_00_hello.c -->
```c
/*
 * The first program. It is deliberately small, and every line of it is one of
 * the five contracts you will meet on every page of this manual.
 *
 * Compare it with the C you already know:
 *
 *     char buf[64];
 *     strcpy(buf, name);          <- how big is name? strcpy does not ask.
 *     strcat(buf, ", welcome!");  <- and now? strcat does not ask either.
 *     printf("%s\n", buf);
 *
 * That program is correct until the day `name` is longer than you assumed, and
 * then it is a security advisory. The version below cannot do that: every write
 * knows the size of its destination, and every operation that could fail hands
 * you back an error you are not allowed to ignore silently.
 */

int main(void) {
    /* (1) You pass the allocator in. The library never reaches for a global
     *     malloc behind your back, so you always know who allocated what. */
    proven_allocator_t alloc = proven_heap_allocator();

    /* (2) Anything that can fail returns its error WITH its value. There is no
     *     errno to remember to check, and `greeting.value` means nothing until
     *     you have looked at `greeting.err`. */
    proven_result_u8str_t greeting = proven_u8str_create(alloc, 64);
    if (!proven_is_ok(greeting.err)) return 1;

    /* (3) A view is borrowed text that knows its own length. PROVEN_LIT builds
     *     one from a literal at compile time - no strlen scan happens here. */
    proven_u8str_view_t name = PROVEN_LIT("world");

    /* (4) The append refuses rather than truncates. If "hello, " and the name
     *     did not fit in the 64 bytes asked for above, this returns
     *     PROVEN_ERR_OUT_OF_BOUNDS and writes nothing - it never quietly stores
     *     half a word and lets you carry on. */
    proven_err_t err = proven_u8str_append(&greeting.value, PROVEN_LIT("hello, "));
    if (proven_is_ok(err)) err = proven_u8str_append(&greeting.value, name);
    if (proven_is_ok(err)) err = proven_u8str_append(&greeting.value, PROVEN_LIT("!"));

    if (!proven_is_ok(err)) {
        proven_u8str_destroy(alloc, &greeting.value);
        return 1;
    }

    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_as_view(&greeting.value),
                                         PROVEN_LIT("hello, world!")),
                    "the three appends should have built the whole greeting");

    proven_println("{}", PROVEN_ARG(proven_u8str_as_view(&greeting.value)));

    /* (5) You created it with `alloc`, so you destroy it with the SAME `alloc`.
     *     Owning things are destroyed exactly once; borrowed things - like
     *     `name` above - are never destroyed at all. */
    proven_u8str_destroy(alloc, &greeting.value);

    return EXAMPLE_OK();
}
```

`EXAMPLE_REQUIRE`와 `EXAMPLE_OK`는 라이브러리의 일부가 아닙니다 — `manual/examples/example.h`에서
오며, 이 매뉴얼의 모든 예제가 본문이 말하는 대로 여전히 동작하는지 빌드가 확인할 수 있게 하려고
존재합니다. 여러분의 프로그램에서는 둘 다 쓰지 않습니다.

여기저기서 계속 나오므로, 잠시 멈춰서 볼 만한 세부 사항 셋:

- **`proven_u8str_create(alloc, 64)`는 64바이트의 용량을 요청합니다.** 그리고 그 용량에는 NUL
  종단 문자가 포함됩니다. 이 문자열은 스스로 커지지 않습니다. `proven_u8str_append`는 고정 용량
  형태라서, 텍스트가 들어가지 않으면 실패합니다. 성장을 원할 때는 `proven_u8str_append_grow`를
  부르면서 allocator를 넘기며, 어느 쪽을 쓰고 있는지는 시그니처만 봐도 한눈에 드러납니다.
- **`PROVEN_LIT("hello, ")`는 런타임 비용이 0입니다.** 문자열 리터럴에 대한 컴파일 타임
  `sizeof`일 뿐입니다. 텍스트가 다른 곳에서 오는 경우를 위해 `proven_u8str_view_from_cstr(s)`가
  있고, 그쪽은 실제로 NUL을 찾아 스캔합니다 — 하나는 공짜이고 다른 하나는 *O(n)*이기 때문에 이름을
  다르게 지은 것입니다.
- **`destroy`는 allocator를 다시 받습니다.** 문자열은 자신을 만든 allocator를 기억하지 않으므로,
  같은 것을 넘겨야 합니다. 덕분에 문자열이 작게 유지되고 의존 관계가 눈에 보이지만, 실제 위험
  요소이기도 합니다 — §5를 보세요.

---

## 4. 빌드와 include

전부를 위한 헤더는 하나입니다:

```text
#include "proven.h"
```

이것이 공개 API 전체를 끌어옵니다. 쓰는 것만 include하고 싶다면 모든 모듈이 `include/proven/`
아래에 자기 헤더를 가지고 있고 — `#include "proven/u8str.h"`, `#include "proven/fs.h"` 하는
식입니다 — `manual-ko.md` §7이 모든 헤더를 그것을 문서화한 챕터로 연결해 줍니다.

이 라이브러리에 필요한 전부인, 손으로 컴파일하기:

```text
gcc -std=c2x -Iinclude -Iplatform your_program.c src/proven/*.c platform/*.c -o your_program
```

`src/proven/`은 이식 가능한 라이브러리 본체입니다. `platform/`은 운영체제와 이야기하는 얇은
계층으로, **PAL**(platform abstraction layer, 플랫폼 추상화 계층)입니다. 시스템 호출을 하는 것은
전부 거기에 있고 다른 어디에도 없으며, 그래서 나머지 라이브러리를 운영체제가 전혀 없는 대상용으로
컴파일할 수 있습니다.

이 저장소는 빌드 시스템 대신 C 프로그램으로 스스로를 빌드합니다:

```text
cc -std=c2x -o nob nob.c     # build the build driver, once
./nob build                  # compile everything and run the whole test suite
./nob release                # the same, optimised
```

인자 없이 `./nob`을 실행하면 나머지 모드를 보여줍니다 — 새니타이저, freestanding, 크로스 컴파일,
벤치마크입니다. `make`도 CMake도 없고, 설치할 것도 없습니다.

**C23 컴파일러가 필요합니다.** 이 라이브러리는 C23 기능을 의도적으로 사용하며, 가장 눈에 띄는
것이 `[[nodiscard]]`입니다. GCC 13+, Clang 16+, 최근 MSVC 모두 동작합니다. 빌드 드라이버는
`-std=c23`을 먼저 시험해 보고, 조금 오래된 컴파일러를 위해 `-std=c2x`로 물러섭니다.

---

## 5. 모든 페이지에서 만나게 될 다섯 가지 계약

이 다섯 규칙이 이 라이브러리의 생김새 대부분을 설명합니다. 각각은 여기서 한 번 진술되고, 다른
모든 곳에서는 전제로 깔립니다. 형식적인 버전은 `manual-ko.md` §3에 있고, 여기 있는 것은 일상
언어판입니다.

| # | 계약 | 한 줄 요약 | 챕터 |
|---|---|---|---|
| 1 | **에러는 값이다** | 실패 가능한 호출은 `proven_err_t`나 `{ err, value }` struct를 반환한다 | [1](manual-01-foundation-ko.md) |
| 2 | **view는 borrowed다** | view는 다른 누군가가 소유한 메모리를 가리키는 포인터 + 길이다 | [3](manual-03-strings-text-ko.md) |
| 3 | **할당은 파라미터다** | 할당할 수 있는 함수는 allocator를 받고, 할 수 없는 함수는 받지 않는다 | [2](manual-02-allocation-ko.md) |
| 4 | **호출자 소유 상태는 복사하면 안 된다** | 어떤 struct는 자기 자신을 가리킨다; 복사하면 dangling 포인터가 생긴다 | [5](manual-05-hosted-services-ko.md) |
| 5 | **거부하되 절대 자르지 않는다** | 들어가지 않는 연산은 실패하고 아무것도 쓰지 않는다 | [3](manual-03-strings-text-ko.md) |

### 1. 에러는 값이다

실패 가능한 함수는 모두 에러를 돌려줍니다. 그 외에 돌려줄 것이 없으면 맨(bare) `proven_err_t`이고,
값이 있으면 값과 에러가 함께 이동하며 에러를 확인하기 전까지 값은 무의미합니다. `PROVEN_OK`은
0이며, `proven_is_ok(err)`가 `err == 0`보다 잘 읽힙니다.

잘못된 예 — 에러보다 값을 먼저 읽기:

```text
proven_result_u8str_t s = proven_u8str_create(alloc, 64);
proven_u8str_append(&s.value, text);   /* wrong: s.value is garbage if create failed */
```

둘을 짝지어 놓은 이유가 바로 값이 성공 경로에서만 유효하다는 데 있습니다. 매번, 먼저
확인하세요.

### 2. view는 borrowed다

`proven_u8str_view_t`는 포인터와 크기입니다. 아무것도 소유하지 않고, 아무것도 할당하지 않으며,
마음대로 복사해도 됩니다 — 하지만 가리키는 대상이 파괴되거나 이동하는 순간 유효하지 않게 됩니다.

잘못된 예 — 가리키는 대상보다 오래 사는 view:

```text
proven_u8str_view_t name;
{
    proven_result_u8str_t owned = proven_u8str_create(alloc, 16);
    (void)proven_u8str_append(&owned.value, PROVEN_LIT("temp"));
    name = proven_u8str_as_view(&owned.value);
    proven_u8str_destroy(alloc, &owned.value);   /* the bytes are gone */
}
use(name);                                       /* wrong: dangling view */
```

이것은 dangling 포인터와 똑같은 수명 버그이지만, view가 값*처럼 보이기* 때문에 따로 말해 둘
가치가 있습니다. view는 값이 아닙니다. struct를 뒤집어쓴 포인터입니다.

### 3. 할당은 파라미터다

시그니처를 읽으세요. `proven_u8str_append(str, data)`는 할당할 수 없으므로 텍스트가 들어가지
않으면 실패합니다. `proven_u8str_append_grow(alloc, str, data)`는 allocator를 받으므로 성장할 수
있습니다. 이 라이브러리의 어떤 것도 여러분 몰래 `malloc`을 부르지 않으며, 그래서 arena에서,
pool에서, 또는 힙이 없는 장치에서도 쓸 수 있습니다.

여기서 따라 나오는 것이 바로 위험 요소입니다: **생성할 때 쓴 allocator로 파괴해야 합니다.**
객체는 그것을 기억하지 않습니다.

잘못된 예 — 짝이 맞지 않는 allocator:

```text
proven_result_u8str_t s = proven_u8str_create(arena_alloc, 64);
proven_u8str_destroy(heap_alloc, &s.value);   /* wrong: heap free on arena memory */
```

현재로서는 아무것도 이것을 검사하지 않습니다. 나중에, 다른 어딘가에서 드러나는 힙 손상입니다.

### 4. 호출자 소유 상태는 복사하면 안 된다

어떤 객체는 여러분이 소유하고 포인터로 넘기는 struct입니다 — 버퍼드 쓰기 스트림(writer), line 읽기 스트림(reader), 디렉터리
이터레이터 같은 것들입니다. 그중 여럿은 자기 자신의 필드를 가리키는 포인터를 품고 있어서, struct를
복사하면 여전히 *원본*을 가리키는 포인터가 함께 복사됩니다.

잘못된 예 — state struct 복사:

```text
proven_writer_buf_t a = ...;
proven_writer_buf_t b = a;          /* wrong: b's internals still point into a */
```

`manual-ko.md` §4.2에 이런 타입 열여섯 개가 모두 나열되어 있습니다. 규칙은 간단합니다. 살아 있을
자리에서 만들고, `&it`을 넘기고, 대입하지 마세요.

### 5. 거부하되 절대 자르지 않는다

결과가 들어가지 않을 때, 이 라이브러리는 연산을 실패시키고 목적지를 그대로 둡니다. "들어가는
만큼"을 쓰지 않습니다. 잘린 경로는 엉뚱한 파일을 열고, 잘린 명령은 엉뚱한 명령을 실행하며, 잘린
숫자는 다른 숫자입니다 — 그리고 그 하나하나가 눈에 보이는 에러보다 나쁩니다.

정말로 자르는 것이 원하는 동작인 경우를 위해서는, 얼마나 썼는지 알려 주는 별도의, 이름이 다른
함수가 있습니다:

```text
proven_result_size_t r = proven_u8str_append_partial(&s, huge);
/* r.value is how many bytes were actually appended. Reading it is the point. */
```

잘못된 예 — 둘이 똑같이 동작한다고 가정하기:

```text
(void)proven_u8str_append_partial(&s, huge);   /* wrong: the count WAS the result */
```

---

## 6. 부록 B: 용어집

이 매뉴얼이 평범한 단어인 양 쓰는 용어들입니다. 평범한 C 단어가 아니며, 하나하나가 무게를
지고 있습니다.

본문이 지키는 규칙이 셋 있습니다. 덕분에 여기 나오는 어떤 말도 짐작할 필요가 없습니다.

- **저희끼리만 쓰는 은어를 만들지 않습니다.** 어떤 생각에 널리 쓰이는 말이 이미 있으면 그 말을
  씁니다. 이 라이브러리가 새로 만든 것만 여기에서 정의합니다.
- **영어 용어는 각 장에서 처음 나올 때 괄호로 원문을 함께 적습니다** — 뷰(view)처럼. 영어로 개념을
  익힌 분은 원래 이름을 찾을 수 있고, 그렇지 않은 분은 번역되지 않은 낱말을 마주치지 않습니다.
- **줄임말은 처음 나올 때 풀어 씁니다.** 예를 들어 PAL은 플랫폼 추상화 계층(Platform Abstraction
  Layer, PAL)으로 적습니다. 모든 줄임말은 아래 표에도 들어 있습니다.

| 용어 | 여기서의 뜻 |
|---|---|
| **owned** | 파괴할 책임이 여러분에게 있습니다. `_create`에서 와서 `_destroy`로 가며, 정확히 한 번입니다. |
| **borrowed** | 다른 누군가가 소유한 메모리를 가리킵니다. 절대 파괴하지 않습니다. 소유자가 살아 있는 동안에만 유효합니다. |
| **view** | borrowed 포인터 + 길이 쌍, 예를 들어 `proven_u8str_view_t`. 복사 가능하고, 소유하지 않으며, 할당하지 않습니다. |
| **allocator** | 네 가지를 담은 값: 컨텍스트 포인터 하나와 함수 포인터 셋(alloc, realloc, free). 할당할 수 있는 모든 것에 값으로 전달됩니다. |
| **arena** | 하나의 블록 안에서 포인터를 밀어 가며 메모리를 나눠 주는 allocator. 개별 free는 아무 일도 하지 않고, arena 전체를 한 번에 reset하거나 파괴합니다. 빠르며, "수명이 같은 작은 것 여럿"에 딱 맞습니다. |
| **pool** | 고정 크기 하나짜리 객체를 여럿 다루는 allocator로, free 리스트를 두어 해제가 실제로 슬롯을 재활용합니다. |
| **trait(트레잇)** | 인터페이스로 쓰이는 함수 포인터 struct — 가상 함수 테이블에 대한 C의 답입니다. `proven_allocator_t`, `proven_writer_t`, `proven_rng_t`가 트레잇입니다. C 키워드가 아니라 빌려 온 용어입니다. |
| **PAL** | 플랫폼 추상화 계층(platform abstraction layer): 실제 시스템 호출을 하는 `platform/` 아래의 코드. OS에 의존하는 유일한 부분입니다. |
| **freestanding** | 운영체제도 libc도 없는 빌드 — 베어메탈입니다. `PROVEN_FREESTANDING`으로 선택합니다. |
| **failure atomicity(실패 원자성)** | 연산이 실패하면 아무것도 바꾸지 않습니다. 실패한 grow는 기존 데이터를 온전하고 유효하게 남겨 둡니다. |
| **provenance** | 포인터가 어느 할당에서 왔는가. C의 최적화기는 서로 다른 할당에서 온 포인터가 절대 겹치지 않는다고 가정하며, 이를 어기는 것은 단지 놀라운 일이 아니라 미정의 동작입니다. 챕터 6에서 다룹니다. |
| **UB**(undefined behaviour, 미정의 동작) | "예측할 수 없는 출력"이 아닙니다 — 표준이 아무 요구도 하지 않으며, 최적화기는 그런 일이 결코 일어나지 않는다고 가정해도 됩니다. UB가 여러분의 `if`를 지워 버릴 수 있는 이유입니다. |
| **`[[nodiscard]]`** | C23 어트리뷰트. 반환값을 버리면 컴파일러가 에러를 냅니다. 에러를 놓쳐서는 안 되는 모든 함수에 붙어 있습니다. |
| **fixed-capacity(고정 용량)** | 성장하지 않습니다. 가득 차면 실패합니다. allocator를 받지 않습니다. |
| **growable(성장 가능)** | 가득 차면 재할당합니다. allocator를 받습니다. 이름에 항상 `_grow`가 붙습니다. |
| **CSPRNG** | 암호학적으로 안전한 의사난수 생성기: 앞선 출력을 본 뒤에도 공격자가 예측할 수 없는 출력. |
| **intrusive(침입형)** | 리스트의 링크가 따로 할당된 노드가 아니라 여러분의 struct *안에* 들어 있습니다. 원소마다 할당이 발생하지 않습니다. |
| **code unit(코드 유닛)** | 인코딩의 원소 하나: UTF-8에서는 한 바이트, UTF-16에서는 16비트 값. 문자가 아닙니다 — 한 문자가 여러 개를 차지할 수 있습니다. |
| **code point(코드 포인트)** | 유니코드에서 한 글자에 매겨진 번호입니다. 한 코드 포인트는 UTF-8에서 1~4바이트, UTF-16에서 코드 유닛 1~2개를 차지합니다. 둘 중 무엇을 세어도 글자 수가 되지 않는 이유입니다. |
| **API**(Application Programming Interface, 응용 프로그래밍 인터페이스) | 라이브러리가 다른 프로그램이 부르라고 내놓은 함수와 타입의 집합입니다. 이 라이브러리에서는 `include/proven/`에 선언된 모든 것입니다. |
| **hosted(호스티드)** | 밑에 운영체제와 C 표준 라이브러리가 있는 빌드입니다. *freestanding*의 반대입니다. |
| **heap(힙)** | `malloc`이 메모리를 나눠 주는 범용 메모리 영역입니다. `proven_heap_allocator()`가 이것을 쓰는 allocator입니다. |
| **slice(슬라이스)** | **쓸 수 있는** borrowed 포인터 + 길이 쌍입니다. 예를 들어 `proven_mem_mut_t`. 같은 생각의 읽기 전용 형태가 *view*입니다. |
| **result(결과 구조체)** | 에러 코드와 값을 함께 담은 작은 struct입니다. 예를 들어 `proven_result_u8str_t`. 옆에 있는 에러를 확인하기 전까지 값은 아무 뜻도 없습니다. |
| **panic(패닉)** | 실패를 돌려줄 호출자가 없을 때 택하는 의도적인 정지입니다. `proven_set_panic_handler()`로 그때 할 일을 고르며, 기본값은 프로그램을 멈춥니다. |
| **reserve(예약)** | 컨테이너의 용량을 **지금** 올려서 이후의 증가가 재할당하지 않게 하는 일입니다. 힙에서는 복사를, arena에서는 죽은 저장 공간을 아낍니다. |
| **dangling(허공을 가리키는)** | 이미 해제되었거나 옮겨진 메모리를 가리키는 포인터입니다. 그것을 쓰는 것은 미정의 동작이며, 여기서 흔한 원인은 재할당할 수 있는 호출을 사이에 두고 포인터를 붙들고 있는 것입니다. |
| **use-after-free(해제 후 사용)** | dangling 포인터로 읽거나 쓰는 일입니다. 아래의 새니타이저가 잡아냅니다. |
| **sanitizer(새니타이저)** | 실행 시 검사를 덧붙이는 컴파일러 모드입니다. **ASan**(AddressSanitizer)은 메모리 오류를, **UBSan**(UndefinedBehaviorSanitizer)은 미정의 동작을, **TSan**(ThreadSanitizer)은 데이터 경쟁을 찾습니다. `./nob asan`, `./nob ubsan`, `./nob tsan`. |
| **partial write / short read(부분 쓰기 / 짧은 읽기)** | 요청한 것보다 *적은* 바이트를 옮긴 한 번의 쓰기나 읽기입니다. 에러가 아니라 정상이며, 짧은 읽기를 입력의 끝으로 착각하는 것이 파일 뒤끝을 잃는 고전적인 방법입니다. |
| **EOF**(End Of File, 파일의 끝) | 더 읽을 입력이 없다는 뜻입니다. 0바이트 성공이 아니라 `PROVEN_ERR_EOF`로 보고되므로 "아직 아무것도 안 왔다"와 혼동될 수 없습니다. |
| **flush(비우기)** | 버퍼드 writer에 쌓인 바이트를 다음 단계로 밀어 보내는 일입니다. 이 라이브러리의 어떤 것도 종료 시에 대신 flush해 주지 않습니다. |
| **back-pressure(배압)** | 소비자가 따라오지 못해 생산자를 늦추는 일입니다. `proven_writer_write_partial()`이 그것을 알아채고 대응하게 해 주는 호출입니다. |
| **durability(지속성)** | 정전을 겪고도 데이터가 남는다는 보장입니다. 운영체제에 도달한 것만으로는 모자랍니다. `proven_fs_sync()`가 파일의 바이트를 저장 장치에 올리고, `proven_fs_sync_dir()`이 이름 바꾸기에 대해 같은 일을 합니다. |
| **atomic rename(원자적 이름 바꾸기)** | 완성된 임시 파일을 원래 이름 위로 rename해 파일을 교체하는 방식입니다. 읽는 쪽은 옛 파일 전체나 새 파일 전체를 보며, 반쯤 쓰인 혼합물은 보지 않습니다. |
| **advisory lock(권고 잠금)** | 마찬가지로 잠금을 요청하는 프로세스만 막는 잠금입니다(`proven_fs_lock()`). 접근 권한 통제가 아니라 협력하는 프로그램들 사이의 약속입니다. |
| **hard link(하드 링크)** | 같은 파일에 대한 두 번째 **이름**입니다. 원본이라는 것이 없고, 마지막 이름이 지워질 때까지 데이터가 삽니다. 같은 파일 시스템 안에서만 됩니다. |
| **symbolic link(심볼릭 링크)** | 경로를 담은 작은 파일입니다. 파일 시스템을 건너뛸 수 있고, 아무것도 가리키지 않을 수도 있습니다. 그때는 따라가기가 실패합니다. |
| **memory mapping(메모리 매핑)** | 파일 내용을 어떤 주소에 나타나게 해서 프로세서가 메모리처럼 읽게 하는 일입니다(`mmap.h`). **SHARED** 매핑의 쓰기는 파일로 돌아가고, **PRIVATE** 매핑은 *기록 시 복사*라서 쓰기가 이 프로세스에만 남습니다. |
| **copy-on-write(기록 시 복사)** | 누군가 쓰기 전까지 메모리를 공유하다가, 쓰는 쪽에게만 사적인 사본을 주는 방식입니다. `PROVEN_MMAP_PRIVATE`가 하는 일입니다. |
| **cursor(커서)** | 스캐너가 읽고 있는 텍스트에서의 현재 위치입니다(`proven_scan_t.cursor`). 스캔은 이것을 앞으로 밀고, `proven_scan_skip_*`은 의도적으로 옮깁니다. |
| **locale(로케일)** | 소수점이 `.`인지 `,`인지를 포함한, 지역 관습에 대한 시스템의 설정입니다. 이 라이브러리의 수 파싱은 **로케일에서 자유롭습니다**. 어떤 기계에서도 쉼표는 소수점이 아닙니다. |
| **entropy(엔트로피)** | 운영체제나 하드웨어에서 오는, 진짜로 예측할 수 없는 비트입니다. 생성기는 엔트로피로 *씨앗을 받고*, 시계나 계수기는 아무리 무작위처럼 보여도 엔트로피가 아닙니다. |
| **seed(씨앗)** | 생성기의 시작값입니다. 같은 씨앗은 같은 수열을 다시 냅니다. 재현 가능한 시험에는 꼭 필요하고, 키에는 치명적입니다. |
| **PRNG / CSPRNG** | 의사난수 생성기(Pseudo-Random Number Generator)는 씨앗에서 수열을 계산합니다. **암호학적으로 안전한**(Cryptographically Secure) 것은 앞선 출력을 본 공격자에게도 예측 불가능하다는 성질이 더 있습니다. |
| **HashDoS(해시 서비스 거부 공격)** | 충돌하도록 고른 키를 해시 테이블에 먹여 *O(1)* 조회를 *O(n²)*으로 만드는 공격입니다. `proven_map_create()`는 키가 있는 해시로 이를 막고, `proven_map_create_trusted()`는 내가 고른 키에 한해 그 방어를 포기합니다. |
| **open addressing(개방 주소법)** | map의 배치 방식입니다. 항목들이 평평한 버킷 배열 하나에 살고, 충돌하면 따로 할당된 노드 사슬을 따라가는 대신 다음 칸으로 옮겨 갑니다. |
| **tombstone(묘비)** | map에서 항목이 지워진 자리에 남기는 표시입니다. 조회가 그 자리를 지나 계속 탐색하게 해 줍니다. 묘비도 적재율에 셈되므로, 삭제가 잦으면 여전히 리해시가 일어납니다. |
| **rehash(리해시)** | 버킷 배열을 새 크기로 다시 만드는 일입니다. 앞서 `get_mut`이 돌려준 모든 포인터를 무효로 만들며, 그래서 포인터가 아니라 키를 붙들어야 합니다. |
| **checksum(검사합)** | 우연한 손상을 알아내는 짧은 값입니다 — `proven_crc32()`. 사고는 잡아내지만 변조는 결코 잡지 못합니다. |
| **digest / hash(다이제스트 / 해시)** | 데이터에서 계산한 고정 크기 값입니다. **SHA-256**은 변조를 드러내는 암호학적 다이제스트이고, **FNV-1a**와 **SipHash-2-4**는 테이블용 해시이며 이 중 키를 쓰는 것은 SipHash뿐입니다. |
| **hex / Base64 / Base64URL** | 바이트를 텍스트로 적는 방법들입니다. hex는 바이트당 두 글자, Base64는 3바이트를 `+ / =`를 쓰는 네 글자로 묶고, Base64URL은 `- _`를 쓰고 패딩이 없어 URL이나 파일 이름에 안전합니다. |
| **padding(패딩)** | 출력 길이를 4의 배수로 맞추려고 Base64가 붙이는 `=` 문자입니다. Base64URL은 붙이지 않습니다. |
| **BMP**(Basic Multilingual Plane, 기본 다국어 평면) | 유니코드의 첫 65,536개 코드 포인트입니다. 그 밖의 글자 — 이모지, 드문 한중일 한자 다수 — 는 UTF-16 코드 유닛 두 개가 필요합니다. |
| **NUL terminator(NUL 종결자)** | C 문자열의 끝을 표시하는 0 바이트입니다. *view*에는 이것이 없으며, 그래서 대신 길이를 지니고 다닙니다. |
| **shortest round trip(최단 왕복 표기)** | 다시 읽었을 때 정확히 같은 부동소수점 값이 되는 가장 짧은 자릿수로 찍는 것입니다. `PROVEN_FLOAT_FORMAT_MODE_SHORTEST`가 이것을 요청하며, 직렬화기가 원하는 성질입니다. |
| **dispatch macro(디스패치 매크로)** | 인자의 타입에서 함수를 골라 주는, C11 `_Generic` 위에 만든 매크로입니다 — `PROVEN_ARG(x)`와 `PROVEN_SCAN_ARG(&x)`. 이름 있는 생성자들 중에서 고를 뿐, 그 자신이 생성자는 아닙니다. |
| **identity constructor(항등 생성자)** | `proven_arg_identity()` / `proven_scan_arg_identity()`. 이미 만들어진 인자를 받아 그대로 통과시켜, 매크로로 굴러가는 코드가 그것을 받아들일 수 있게 합니다. |
| **scratch(임시 작업용) allocator** | 결과를 소유하는 allocator와 따로, 임시 작업 메모리만을 위해 넘기는 allocator입니다. 예를 들어 `proven_map_set_with_scratch()`의 `scratch` 매개변수. |
| **stackless coroutine(스택 없는 코루틴)** | 자기 스택 없이 멈췄다 이어 갈 수 있는 함수입니다. 상태가 여러분이 들고 있는 struct 안에 삽니다. `coro.h`는 이것을 switch로 구현하므로 스레드도 할당도 들지 않습니다. |
| **bounded queue(상한 있는 큐)** | 용량이 정해져 있어 가득 차면 늘어나는 대신 거절하는 큐입니다. job 시스템이 이것을 쓰므로, 일꾼보다 빠른 생산자는 메모리를 소진하는 대신 거절을 듣습니다. |
| **monotonic clock(단조 시계)** | 앞으로만 가는 시계로, 무언가에 걸린 시간을 재는 데 씁니다. 시스템 시각이 교정될 때 뛸 수 있는 벽시계(wall clock)와 다릅니다. 벽시계로 소요 시간을 재면 음수가 나오는 일이 그래서 생깁니다. |

---

## 7. 부록 D: libc 대응표

이미 C를 쓰고 있다면 이것이 가장 빠른 입구입니다. 각 행은 그 맞바꿈을 설명하는 챕터로
연결됩니다.

| 원래 쓰던 것 | 대신 쓸 것 | 무엇이 다른가 |
|---|---|---|
| `malloc` / `free` | `proven_heap_allocator()` + `_create` / `_destroy` | allocator가 파라미터라서, 같은 코드가 arena나 pool 위에서도 돌아갑니다. [챕터 2](manual-02-allocation-ko.md) |
| `strcpy`, `strcat` | `proven_u8str_append`, `_append_grow` | 크기를 알고 있으므로, 오버런을 저지르는 대신 거부합니다. [챕터 3](manual-03-strings-text-ko.md) |
| `strlen` | `view.size` | 길이가 이미 거기 있습니다. 아무것도 스캔하지 않습니다. [챕터 3](manual-03-strings-text-ko.md) |
| 동등 비교용 `strcmp` | `proven_u8str_view_eq` | 중간에 NUL이 박힌 텍스트에서도 동작하고, 끝을 넘어가지 않습니다. [챕터 3](manual-03-strings-text-ko.md) |
| `strstr` | `proven_u8str_view_find` | 인덱스 또는 `PROVEN_INDEX_NOT_FOUND`를 반환하며, 탐색이 단순 무식하지 않습니다. [챕터 3](manual-03-strings-text-ko.md) |
| `strtok` | *(아직 대응물 없음)* | `strtok`은 입력을 변경하고 중첩해서 쓸 수 없습니다. view 기반 분할기가 `docs/RFC-0002`에 설계되어 있습니다. |
| `printf` | `proven_println("{}", PROVEN_ARG(x))` | 타입이 포맷 문자열이 아니라 인자에서 옵니다. [챕터 8](manual-08-fmt-scan-ko.md) |
| `sprintf` | `proven_u8str_append_fmt` | 크기가 정해진 목적지에 쓰고, 그것을 넘기기를 거부합니다. [챕터 8](manual-08-fmt-scan-ko.md) |
| `sscanf` | `proven_scan_*`, `proven_scan_fmt` | 어느 필드가 실패했고 커서가 어디서 멈췄는지 알려 줍니다. [챕터 8](manual-08-fmt-scan-ko.md) |
| `strtod` | `proven_parse_f64_ascii` | 올바르게 반올림하고, 로케일에 영향받지 않으며, `errno`를 쓰지 않습니다. [챕터 8](manual-08-fmt-scan-ko.md) |
| `fopen` / `fread` / `fclose` | `proven_fs_open`, `_read`, `_close`, 또는 `proven_fs_read_all_u8str` | 명시적인 에러, 숨은 버퍼링 없음, 파일 전체를 한 번의 호출로. [챕터 5](manual-05-hosted-services-ko.md) |
| `fgets` | `proven_sysio_read_line`, `proven_reader_read_line` | 버퍼를 정확히 꽉 채우는 줄도 잃어버리지 않고 반환합니다. [챕터 5](manual-05-hosted-services-ko.md) |
| `qsort` | `proven_array_sort` | introsort입니다: 퀵소트의 최악 경우가 아니라 *O(n log n)* 보장. [챕터 4](manual-04-containers-algorithms-ko.md) |
| `bsearch` | `proven_array_binary_search` | 같은 모양, 같은 비교자 계약. [챕터 4](manual-04-containers-algorithms-ko.md) |
| `rand` | `proven_xoshiro256ss_*` 또는 `proven_random_bytes` | 재현 가능하고 빠르거나, 예측 불가능하고 안전하거나 — 여러분이 의도적으로 고릅니다. [챕터 5](manual-05-hosted-services-ko.md) |
| `time` / `clock` | `proven_time_now`, `proven_time_breakdown` | 나노초 단위이고, 벽시계와 monotonic을 명시적으로 구분합니다. [챕터 5](manual-05-hosted-services-ko.md) |
| `assert` | `proven_panic` + panic 훅 | freestanding 빌드에서도 동작하고, 교체할 수 있습니다. [챕터 1](manual-01-foundation-ko.md) |

---

## 8. 다음에 읽을 것

각 장이 그 앞의 장들만 필요로 하도록 순서를 잡았습니다.

| 파트 | 읽을 것 | 무엇을 위해 |
|---|---|---|
| **I** | 이 장 | 계약과 어휘 |
| **II** | [1](manual-01-foundation-ko.md) → [2](manual-02-allocation-ko.md) → [3](manual-03-strings-text-ko.md) | 에러, 메모리, 텍스트: 모든 프로그램이 쓰는 것 |
| **III** | [4](manual-04-containers-algorithms-ko.md) | 배열, 맵, 리스트, 링, 정렬, 해싱, 인코딩 |
| **IV** | [8](manual-08-fmt-scan-ko.md) | 챕터 3이 소개한 뒤의, 형식화와 파싱 전체 |
| **V** | [5](manual-05-hosted-services-ko.md) | 파일, 스트림, 표준 I/O, 시간, 난수, 매핑 |
| **VI** | [6](manual-06-execution-and-platform-ko.md) → [freestanding](manual-freestanding-ko.md) | 코루틴, job, 스레드 안전성, 베어메탈, 크로스 빌드 |
| **부록** | [A: alias 인덱스](manual-07-alias-xcv-index-ko.md), 위의 B와 D | 찾아보기 |

숙련된 C 프로그래머이고 시간이 없다면, 위의 §7을 읽고, 그다음
[챕터 1](manual-01-foundation-ko.md)을 읽고, 그다음 필요한 것을 다루는 챕터를 읽으세요. 더
초심자라면 파트 I과 II를 순서대로 읽으세요 — 짧고, 뒤의 모든 것이 그것을 전제로 합니다.
