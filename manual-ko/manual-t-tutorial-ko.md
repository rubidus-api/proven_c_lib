# 따라 하며 익히기: 아홉 개의 짧은 프로그램

**Part I — 여기서 시작합니다.** 입문용 C 책 한 권 외에 사전 지식은 필요 없습니다.
**6과를 마치면** [0장](manual-00-start-here-ko.md)의 인사말 프로그램을 한 줄씩 읽어 낼 수
있고, 레퍼런스 장들이 낯선 낱말의 벽으로 보이지 않게 됩니다. **9과를 마치면** 아레나, 자라는
컨테이너, 파일을 한 번씩 써 본 것이 됩니다 --- 텍스트를 다룰 줄 알게 된 프로그램이 다음으로
찾는 것들입니다.

## 이 글이 있는 이유

0장은 첫 쪽부터 도는 프로그램을 보여 줍니다. 그런데 그 서른 줄에는 새 개념이 다섯 개나 한꺼번에
들어 있습니다 — 인자로 건네는 할당자, 반드시 확인해야 하는 result, 자기 길이를 아는 view, 자르는
대신 거부하는 append, 만든 할당자로 짝지어 부르는 destroy. C 책 한 권을 막 뗐다면 다섯은 넷이
많습니다.

그래서 이 실습은 하나씩만 더합니다. 프로그램 아홉 개, 새로 나오는 것은 매번 딱 하나입니다. 앞의
여섯은 0장의 프로그램까지 이끌고, 뒤의 셋은 그 너머의 아레나·컨테이너·파일로 갑니다. 아홉 모두
`manual/examples/` 아래의 진짜 파일이고 빌드가 컴파일해
**실제로 돌립니다.** 그러니 여기 실린 것은 정말로 돌아간 코드입니다.

빌드는 여느 C 프로그램과 같습니다. 이 라이브러리는 여러분의 코드와 함께 컴파일하는 소스라서
설치할 것이 없습니다.

```text
cc -std=c23 -Iinclude your_program.c src/proven/*.c platform/*.c -o your_program
```

이 줄이 낯설다면 [0장 §4](manual-00-start-here-ko.md#4-빌드와-include)를 먼저 읽고 오세요.

## 목차

1. [1과 — 출력, 그리고 되는 빌드](#1과--출력-그리고-되는-빌드)
2. [2과 — 자기 길이를 아는 텍스트](#2과--자기-길이를-아는-텍스트)
3. [3과 — 실패할 수 있는 호출은 그렇다고 말한다](#3과--실패할-수-있는-호출은-그렇다고-말한다)
4. [4과 — 값과 오류는 함께 온다](#4과--값과-오류는-함께-온다)
5. [5과 — 기억을 누가 주는가는 인자다](#5과--기억을-누가-주는가는-인자다)
6. [6과 — 0장의 프로그램을 한 줄씩](#6과--0장의-프로그램을-한-줄씩)
7. [7과 — 한꺼번에 풀려나는 기억](#7과--한꺼번에-풀려나는-기억)
8. [8과 — 컨테이너는 자기 할당자를 기억한다](#8과--컨테이너는-자기-할당자를-기억한다)
9. [9과 — 바깥 세상도 실패하고, 같은 방식으로 말한다](#9과--바깥-세상도-실패하고-같은-방식으로-말한다)
10. [다음에 읽을 것](#다음에-읽을-것)

---

## 1과 — 출력, 그리고 되는 빌드

**새로 나오는 것 하나:** `proven_println`, 그리고 그 인자가 검사된다는 사실.

`printf("%d", 3.5)`는 컴파일됩니다. 하지만 틀렸고, 운이 나쁘면 쓰레기를 찍거나 죽습니다. 서식
문자열은 그냥 *문자열*이라, 뒤에 오는 인자와 이어 주는 것이 아무것도 없기 때문입니다.
`proven_println`은 자리표로 `{}`를 쓰고 인자마다 `PROVEN_ARG`로 감쌉니다. 이 매크로가 인자의
타입을 함께 실어 보내므로, 서식과 인자가 어긋날 수가 없습니다.

<!-- example: manual/examples/ko/tut_01_hello.c -->
```c
/*
 * 1과 - 한 줄을 빌드해서 돌려 본다.
 *
 * 아직 이 라이브러리의 생각에 대한 것은 하나도 없다. 이 프로그램이 답하는 물음은
 * "내 빌드 명령이 되는가" 하나뿐이고, 그것만 따로 물어 볼 값어치가 있다. 뒤의
 * 모든 과가 그것을 전제하기 때문이다.
 *
 * printf(fmt, ...) 는 검사되지 않는다. "%d" 자리에 double 을 건넸다고 컴파일러가
 * 알려 주지 못한다. proven_println 은 검사한다. 인자마다 PROVEN_ARG 로 감싸
 * 자기 타입을 함께 들고 가기 때문이다.
 */

int main(void) {
    proven_println("hello from proven");

    /* {} 가 자리표다. 값은 PROVEN_ARG 를 거치면서 어떤 타입인지를 함께 싣는다.
     * 그래서 서식 문자열과 인자가 어긋날 수가 없다. */
    proven_println("one number: {}", PROVEN_ARG(42));
    proven_println("two of them: {} and {}", PROVEN_ARG(1), PROVEN_ARG(2));

    return EXAMPLE_OK();
}
```

돌리면 세 줄이 찍힙니다. 빌드되고 돌았다면 include 경로와 소스 목록이 맞다는 뜻이고, 이다음부터는
빌드가 아니라 라이브러리 이야기입니다.

**처음에 잘 걸리는 것.** `{}`는 `%s`가 아닙니다. 틀릴 글자가 아예 없고, `long`이냐 `int`냐를
기억할 필요도 없습니다 — `PROVEN_ARG`가 압니다.

---

## 2과 — 자기 길이를 아는 텍스트

**새로 나오는 것 하나:** *뷰(view)* --- 포인터와 크기가 함께 다니는 것.

여러분이 아는 C에서 문자열은 포인터 하나이고, 길이는 「처음 0 바이트가 나오는 자리」입니다. 그것을
만지는 함수마다 얼마나 되는지 알아내려고 바이트를 훑어야 하고, 0이 없으면 끝을 넘어 훑습니다. 이
설계 하나가 이 언어의 보안 권고 상당수의 뿌리입니다.

view는 그 빠진 반쪽을 드러냅니다. 바이트를 소유하지 않고 *빌려* 쓰며, 아무것도 해제하지 않습니다.

<!-- example: manual/examples/ko/tut_02_view.c -->
```c
/*
 * 2과 - 자기 길이를 아는 텍스트.
 *
 * 여러분이 아는 C 에서 문자열은 포인터 하나이고, 길이는 "처음 0 바이트가 나오는
 * 자리"다. 함수마다 얼마나 되는지 알아내려고 바이트를 훑어야 하고, 0 이 없으면
 * 끝을 넘어 훑는다.
 *
 * 뷰(view)는 C 가 암묵으로 남겨 둔 그 짝이다. 포인터와 크기가 함께 다닌다.
 * 빌려 쓸 뿐이라 바이트를 소유하지 않고 해제하지도 않는다.
 */

int main(void) {
    /* PROVEN_LIT 은 리터럴에서 뷰를 만든다. 크기는 컴파일 때 정해지므로 여기서
     * 훑는 일은 없다 - strlen 과 다른 점이다. */
    proven_u8str_view_t hello = PROVEN_LIT("hello");

    EXAMPLE_REQUIRE(hello.size == 5, "the view already knows its own length");
    EXAMPLE_REQUIRE(hello.ptr != NULL, "and it points at the literal's bytes");

    /* 길이가 포인터와 함께 다니므로, 비교는 크기 검사에 memcmp 하나다.
     * 훑을 일도 없고 끝을 넘어갈 길도 없다. */
    EXAMPLE_REQUIRE(proven_u8str_view_eq(hello, PROVEN_LIT("hello")), "same text");
    EXAMPLE_REQUIRE(!proven_u8str_view_eq(hello, PROVEN_LIT("hell")), "shorter text differs");

    /* 뷰는 무언가의 *일부*를 복사 없이 가리킬 수 있다. 아래는 리터럴의 가운데다 -
     * 여전히 빌린 것이고, 할당은 없다. */
    proven_u8str_view_t ell = { .ptr = hello.ptr + 1, .size = 3 };
    EXAMPLE_REQUIRE(proven_u8str_view_eq(ell, PROVEN_LIT("ell")), "a window onto the same bytes");

    proven_println("whole: {} (size {})", PROVEN_ARG(hello), PROVEN_ARG(hello.size));
    proven_println("part : {} (size {})", PROVEN_ARG(ell), PROVEN_ARG(ell.size));

    return EXAMPLE_OK();
}
```

**눈여겨볼 것.** `PROVEN_LIT`은 크기를 컴파일 때 정하므로 `strlen`이 아닙니다. 그리고 세 번째
view는 리터럴의 *가운데*를 복사 없이 가리킵니다 --- view가 무언가의 일부를 가리킬 수 있다는 것,
이것이 이 라이브러리의 자르기와 파싱이 할당을 하지 않는 이유입니다.

**처음에 잘 걸리는 것.** view는 빌린 것입니다. 가리키던 바이트가 사라지면 --- 스코프를 벗어난
지역 버퍼, 이미 destroy 한 문자열 --- 그 view는 여느 C 포인터와 똑같이 매달린 포인터가 됩니다.
이 라이브러리는 몰래 수명을 늘려 주지 않습니다.

---

## 3과 — 실패할 수 있는 호출은 그렇다고 말한다

**새로 나오는 것 하나:** `proven_err_t`, 그리고 자르는 대신 거부하기.

C는 실패를 서로 무관한 세 방식으로 알립니다. 특별한 반환값, 널 포인터, 그리고 다음 호출이 덮어
쓰는 전역 `errno`. 셋 다 안 보기 쉽고, 안 봐도 아무도 뭐라 하지 않습니다. 여기서는 실패할 수 있는
호출이 오류를 *값*으로 돌려주고, 그 함수들에는 `[[nodiscard]]`가 붙어 있어 버리면 습관이 아니라
컴파일러 경고가 됩니다.

<!-- example: manual/examples/ko/tut_03_error.c -->
```c
/*
 * 3과 - 실패할 수 있는 호출은 반환값으로 그렇다고 말한다.
 *
 * 여러분이 아는 C 는 실패를 세 가지 방식으로 알린다. 특별한 반환값(-1), 널
 * 포인터, 그리고 다음 호출이 덮어써 버리는 errno 라는 전역. 셋 다 안 보기 쉽고,
 * 안 봐도 아무도 뭐라 하지 않는다.
 *
 * 여기서는 실패할 수 있는 호출이 proven_err_t 를 돌려준다. 그냥 값이라서 담아
 * 둘 수도, 비교할 수도, 위로 넘길 수도 있다. 그리고 이 함수들에 붙은
 * [[nodiscard]] 덕분에 무시하는 것은 습관이 아니라 컴파일러 경고가 된다.
 */

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    /* 딱 8 바이트짜리 자리. (그 자리가 어디서 오는지는 2장의 주제다. 지금은
     * 우리가 "얼마나"를 말했다는 것만 보면 된다.) */
    proven_result_u8str_t s = proven_u8str_create(alloc, 8);
    EXAMPLE_REQUIRE(proven_is_ok(s.err), "8 bytes should be available");

    /* 이건 들어간다. */
    proven_err_t err = proven_u8str_append(&s.value, PROVEN_LIT("12345678"));
    EXAMPLE_REQUIRE(proven_is_ok(err), "eight bytes into eight bytes fits exactly");

    /* 이건 안 들어간다 - 그리고 라이브러리는 *거부*한다. 들어갈 만큼만 붙이지
     * 않는다. 잘린 낱말은 짧아진 낱말이 아니라 다른 낱말이기 때문이다. */
    proven_err_t too_much = proven_u8str_append(&s.value, PROVEN_LIT("9"));
    EXAMPLE_REQUIRE(!proven_is_ok(too_much), "one byte more than capacity must fail");
    EXAMPLE_REQUIRE(too_much == PROVEN_ERR_OUT_OF_BOUNDS, "and it says why: out of bounds");

    /* 거부는 아무것도 바꾸지 않았다. 문자열은 그대로다. 이것이 "실패 원자성"의
     * 뜻이다 - 실패한 호출은 반쯤 된 상태를 남기지 않는다. */
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_as_view(&s.value), PROVEN_LIT("12345678")),
                    "the refused append must not have written anything");

    proven_println("after the refusal, the string is still: {}",
                   PROVEN_ARG(proven_u8str_as_view(&s.value)));

    proven_u8str_destroy(alloc, &s.value);
    return EXAMPLE_OK();
}
```

**눈여겨볼 것.** 들어가지 못한 append는 *아무것도* 바꾸지 않습니다. 들어갈 만큼만 넣지도
않습니다. 이것을 실패 원자성이라고 하는데, 이유는 잘린 문자열이 짧아진 메시지가 아니라 *다른*
메시지이기 때문입니다 --- 잘린 경로는 다른 파일을 가리키고, 잘린 명령은 다른 명령입니다.

**처음에 잘 걸리는 것.** 검사는 `proven_is_ok(err)`이고 `err == PROVEN_OK`도 같은 말입니다.
하면 안 되는 것은 그것을 무시하고 값을 읽는 일입니다.

---

## 4과 — 값과 오류는 함께 온다

**새로 나오는 것 하나:** result 구조체 --- `.err`와 `.value`가 한 번에 돌아온다.

돌려줄 값이 없는 호출에는 오류 코드만으로 충분합니다. 돌려줄 값이 있으면 라이브러리는 둘을 작은
구조체 하나에 담아 줍니다. 규칙은 한 문장입니다. **`err`를 보기 전에는 `value`는 아무 뜻도
없다.**

<!-- example: manual/examples/ko/tut_04_result.c -->
```c
/*
 * 4과 - 돌려줄 값이 있는 호출은, 값과 오류가 함께 온다.
 *
 * 돌려줄 것이 없을 때는 proven_err_t 하나로 충분하다. 돌려줄 것이 *있으면*
 * 필드 둘짜리 작은 구조체를 받는다. `err` 와 `value` 다. 규칙은 한 문장이다.
 * `err` 를 보기 전에는 `value` 는 아무 뜻도 없다.
 *
 * malloc 이 NULL 인지 확인하던 것과 같은 규율인데, 확인해야 한다는 사실이
 * 기억해야 할 관습이 아니라 타입의 일부라는 점이 다르다.
 */

/* 여러분의 함수도 이것을 돌려줄 수 있다. 라이브러리에 마법은 없다. */
static proven_result_size_t safe_div(proven_size_t a, proven_size_t b) {
    proven_result_size_t res = {0};
    if (b == 0) {
        res.err = PROVEN_ERR_INVALID_ARG;    /* value 는 0 그대로 - 아무 뜻도 없다 */
        return res;
    }
    res.err = PROVEN_OK;
    res.value = a / b;
    return res;
}

int main(void) {
    proven_result_size_t ok = safe_div(10, 2);
    EXAMPLE_REQUIRE(proven_is_ok(ok.err), "dividing by 2 is fine");
    EXAMPLE_REQUIRE(ok.value == 5, "and only now may we read the value");

    proven_result_size_t bad = safe_div(10, 0);
    EXAMPLE_REQUIRE(!proven_is_ok(bad.err), "dividing by zero must fail");
    EXAMPLE_REQUIRE(bad.err == PROVEN_ERR_INVALID_ARG, "and say which rule was broken");
    /* bad.value 는 0 이지만 그것은 답이 아니다. 답이 없다는 뜻이다. */

    /* 라이브러리 자신의 호출도 같은 모양이다. proven_u8str_create 는 만든
     * 문자열과 그것을 지키는 오류를 함께 돌려준다. */
    proven_allocator_t alloc = proven_heap_allocator();
    proven_result_u8str_t s = proven_u8str_create(alloc, 16);
    if (!proven_is_ok(s.err)) return 1;      /* 만들어진 것이 없으니 지울 것도 없다 */

    proven_println("10 / 2 = {}", PROVEN_ARG(ok.value));

    proven_u8str_destroy(alloc, &s.value);
    return EXAMPLE_OK();
}
```

**눈여겨볼 것.** `safe_div`는 라이브러리 함수가 아니라 *여러분의* 함수입니다. 이 방식은 그냥
구조체를 돌려주는 것일 뿐, 밑에 깔린 장치가 없습니다. 그래서 이 쪽을 읽은 순간부터 여러분 코드에
그대로 쓸 수 있습니다.

**처음에 잘 걸리는 것.** 실패했을 때 `value` 자리에는 보통 0이 들어 있습니다. 그 0은 답이
아니라 *답이 없다는 것*입니다. 둘을 가르는 것이 `err` 검사입니다.

---

## 5과 — 기억을 누가 주는가는 인자다

**새로 나오는 것 하나:** `proven_allocator_t` --- 가정하지 않고 건네받는다.

`malloc`은 여러분 대신 내려진 결정입니다. 힙 하나, 전략 하나, 그리고 호출 자리에는 그런 티가 나지
않습니다. 이 라이브러리에서는 기억이 필요한 것이면 무엇이든 할당자를 인자로 받고 그것만 씁니다.
따라 나오는 둘이 요점입니다 --- 「이건 누가 할당했지?」에 호출을 읽어 답할 수 있고, 같은 코드에
다른 할당자를 건네도 코드는 그대로입니다.

<!-- example: manual/examples/ko/tut_05_allocator.c -->
```c
/*
 * 5과 - 기억을 누가 주는가는 전역이 아니라 인자다.
 *
 * malloc 은 여러분 대신 내려진 전역 결정이다. 힙 하나, 전략 하나, 그리고 호출
 * 자리에는 보이지 않는다. 여기서는 기억이 필요한 것이면 무엇이든
 * proven_allocator_t 를 받아 그것만 쓴다. 따라 나오는 둘이 요점이다.
 *
 *   - "이건 누가 할당했지?" 에 호출을 보고 언제나 답할 수 있다.
 *   - 같은 코드에 다른 할당자를 건네도 코드는 그대로다.
 */

/* 이 함수는 기억이 어디서 오는지 알지도, 신경 쓰지도 않는다. */
static proven_result_u8str_t make_greeting(proven_allocator_t alloc,
                                           proven_u8str_view_t name) {
    proven_result_u8str_t out = proven_u8str_create(alloc, 64);
    if (!proven_is_ok(out.err)) return out;

    proven_err_t err = proven_u8str_append(&out.value, PROVEN_LIT("hello, "));
    if (proven_is_ok(err)) err = proven_u8str_append(&out.value, name);
    if (!proven_is_ok(err)) {
        proven_u8str_destroy(alloc, &out.value);   /* 만든 것을 되돌린다 */
        out.err = err;
        out.value = (proven_u8str_t){0};
    }
    return out;
}

int main(void) {
    /* (a) 보통의 힙 - 밑에서 malloc 과 free 가 돈다 */
    proven_allocator_t heap = proven_heap_allocator();

    proven_result_u8str_t a = make_greeting(heap, PROVEN_LIT("world"));
    EXAMPLE_REQUIRE(proven_is_ok(a.err), "the heap should be able to give 64 bytes");
    proven_println("from the heap : {}", PROVEN_ARG(proven_u8str_as_view(&a.value)));
    /* 만든 것과 *같은* 할당자로 지운다. 그 짝이 이 라이브러리 소유 규칙의
     * 전부다. */
    proven_u8str_destroy(heap, &a.value);

    /* (b) 아레나 - 기억 덩이 하나를 차례로 나눠 주고 한꺼번에 되돌린다.
     *     위의 make_greeting 은 조금도 바뀌지 않았다는 데 주목할 것. */
    alignas(PROVEN_MAX_ALIGN) proven_byte_t backing[512];
    proven_arena_t arena = proven_arena_create((proven_mem_mut_t){
        .ptr = backing, .size = sizeof backing });
    proven_allocator_t from_arena = proven_arena_as_allocator(&arena);

    proven_result_u8str_t b = make_greeting(from_arena, PROVEN_LIT("arena"));
    EXAMPLE_REQUIRE(proven_is_ok(b.err), "the arena has room for this too");
    proven_println("from an arena : {}", PROVEN_ARG(proven_u8str_as_view(&b.value)));

    /* 여기엔 지우는 반복문이 없다. 아레나는 reset 으로 전부 되돌린다. 그 이야기는
     * 2장의 몫이고, 지금의 요점은 *부르는 쪽이 골랐다*는 것뿐이다. */
    proven_arena_reset(&arena);

    return EXAMPLE_OK();
}
```

**눈여겨볼 것.** `make_greeting`은 한 번 쓰였고 힙에서도, 아레나에서도 돕니다. 아레나는 여러분이
가진 기억 덩이 하나를 포인터를 밀어 가며 나눠 주고 한꺼번에 되돌리는 방식입니다. 함수는 바뀌지
않았고, 전역으로 설정한 것도 없습니다.

**소유 규칙 한 줄:** 만든 쪽이 지운다, 그것도 *같은* 할당자로.

**처음에 잘 걸리는 것.** 아레나에서는 `destroy`가 아무것도 되돌리지 않습니다 --- 아레나는 reset
으로 해제합니다. 그래도 부르세요. 그 짝이 있어야 나중에 할당자를 바꿔도 코드가 옳습니다.

---

## 6과 — 0장의 프로그램을 한 줄씩

**새로 나오는 것 하나:** 없습니다. 그것이 이 과제입니다.

<!-- example: manual/examples/ko/tut_06_hello_again.c -->
```c
/*
 * 6과 - 0장의 프로그램을 한 줄씩 읽는다.
 *
 * 새로 나오는 것은 없다. 매뉴얼이 첫 쪽에 싣는 그 인사말 프로그램 그대로이고,
 * 이 과의 요점은 이제 그 모든 부분에 이름을 댈 수 있다는 것이다. 건네받는
 * 할당자(5과), 반드시 확인해야 하는 result(4과), 거부가 돌려주는 오류(3과),
 * 자기 길이를 들고 다니는 뷰(2과), 그리고 인자를 검사하는 출력(1과).
 *
 * 이것이 이제 평범하게 읽힌다면 이 실습은 할 일을 다한 것이고, 레퍼런스 장들이
 * 여러분에게 열린 것이다.
 */

int main(void) {
    /* 5과 - 기억이 어디서 오는지는 부르는 쪽이 정한다 */
    proven_allocator_t alloc = proven_heap_allocator();

    /* 4과 - 문자열과 그것을 지키는 오류가 함께 온다 */
    proven_result_u8str_t greeting = proven_u8str_create(alloc, 64);
    if (!proven_is_ok(greeting.err)) return 1;

    /* 2과 - 자기 크기를 아는, 빌려 쓰는 텍스트 */
    proven_u8str_view_t name = PROVEN_LIT("world");

    /* 3과 - 각 append 는 들어가거나 거부한다. 자르는 것은 하나도 없다 */
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

    /* 1과 - 서식 문자열과 인자는 어긋날 수 없다 */
    proven_println("{}", PROVEN_ARG(proven_u8str_as_view(&greeting.value)));

    /* 다시 5과 - 만든 할당자로 지운다 */
    proven_u8str_destroy(alloc, &greeting.value);
    return EXAMPLE_OK();
}
```

저 주석들이 새 정보가 아니라 *아는 것에 붙은 이름표*로 읽힌다면, 이제 레퍼런스 장을 펼칠 준비가
된 것입니다. 다음 세 과는 그 장들로 들어가는 첫걸음 --- 아레나, 컨테이너, 파일 --- 이고, 모양은
같습니다. 과마다 새 생각 하나.

---

## 7과 — 한꺼번에 풀려나는 기억

**새로 나오는 것 하나:** `proven_arena_reset` --- 아레나(arena)가 나눠 준 것을 호출 하나로 전부
돌려받는다.

5과는 `make_greeting`에 아레나를 건네고 끝에서 reset 했지만, 그 reset 이 무엇을 하는지는 말하지
않았습니다. 생각은 이것이 전부입니다. 아레나는 여러분이 가진 기억 덩어리를 포인터를 앞으로 밀며
조각조각 나눠 줍니다. 조각 하나를 돌려받는 방법은 없습니다 --- 아레나에 `destroy`를 불러도 돌아오는
것이 없습니다. reset 하면 *전부*가 한꺼번에 돌아옵니다.

이것은 대부분의 프로그램이 가진 모양에 맞습니다. 바퀴마다 임시 글을 좀 만들어 쓰고, 쓰고 나면
끝나는 루프입니다. 바퀴 안에서는 마음껏 할당하고 끝에서 reset 하면, 따라다닐 것도 샐 것도
없습니다.

<!-- example: manual/examples/ko/tut_07_arena.c -->
```c
/*
 * 7과 - 한꺼번에 풀려나는 기억.
 *
 * 5과는 make_greeting 에 아레나를 건넨 뒤 reset 했지만, 그 reset 이 무엇인지는
 * 말하지 않았다. 이번 과는 그 호출 하나에 대한 것이다.
 *
 * 아레나는 여러분이 가진 기억 덩어리 위에서 포인터를 앞으로 밀어 가며 나눠 준다.
 * 객체 하나하나를 푸는 free 는 없다. 아레나에 destroy 를 불러도 돌아오는 것은
 * 없다. 모든 것이 proven_arena_reset 한 번에 한꺼번에 돌아온다. 제약처럼 들리지만
 * 그것이 아레나를 쓰는 이유다. 일감마다 임시 글을 만드는 루프는 문자열을 몇 개를
 * 만들었든 문장 하나로 전부 버릴 수 있다.
 */

int main(void) {
    /* 이 배열의 주인은 아레나가 아니라 우리다. 아레나는 조각을 나눠 줄 뿐이다. */
    alignas(PROVEN_MAX_ALIGN) proven_byte_t backing[256];
    proven_arena_t arena = proven_arena_create((proven_mem_mut_t){
        .ptr = backing, .size = sizeof backing });
    proven_allocator_t scratch = proven_arena_as_allocator(&arena);

    static const proven_u8str_view_t names[] = {
        PROVEN_LIT("ada"), PROVEN_LIT("grace"), PROVEN_LIT("barbara"),
    };

    for (proven_size_t i = 0; i < sizeof names / sizeof names[0]; ++i) {
        /* 한 바퀴에 문자열 둘, 둘 다 아레나에서 나온다. */
        proven_result_u8str_t line = proven_u8str_create(scratch, 32);
        proven_result_u8str_t note = proven_u8str_create(scratch, 32);
        EXAMPLE_REQUIRE(proven_is_ok(line.err) && proven_is_ok(note.err),
                        "each round fits easily in 256 bytes");
        if (!proven_is_ok(line.err) || !proven_is_ok(note.err)) return 1;

        proven_err_t err = proven_u8str_append(&line.value, PROVEN_LIT("hello, "));
        if (proven_is_ok(err)) err = proven_u8str_append(&line.value, names[i]);
        if (proven_is_ok(err)) err = proven_u8str_append(&note.value, PROVEN_LIT("(scratch)"));
        EXAMPLE_REQUIRE(proven_is_ok(err), "the appends fit their capacity");

        /* 바퀴마다 첫 할당은 `backing` 의 맨 앞에 떨어진다. 앞 바퀴 끝의
         * reset 이 전부 돌려주었기 때문이다. */
        EXAMPLE_REQUIRE((const void *)proven_u8str_as_view(&line.value).ptr == (const void *)backing,
                        "each round starts again at the beginning of the block");

        proven_println("round {}: {} {} ({} bytes in use)",
                       PROVEN_ARG(i), PROVEN_ARG(proven_u8str_as_view(&line.value)),
                       PROVEN_ARG(proven_u8str_as_view(&note.value)),
                       PROVEN_ARG(arena.offset));

        /* 문장 하나로 두 문자열이 사라진다. destroy 루프도, 잊을 것도 없다.
         * 여기서부터 `line` 과 `note` 는 회수된 기억을 가리킨다. */
        proven_arena_reset(&arena);
        EXAMPLE_REQUIRE(arena.offset == 0, "reset reclaims everything the round allocated");
    }

    /* 아레나는 자라지 않는다. 덩어리보다 많이 달라고 하면 3과에서 본 것과 같은
     * 오류 값이 온다. 죽지도 않고, 몰래 malloc 으로 넘어가지도 않는다. */
    proven_result_u8str_t too_big = proven_u8str_create(scratch, 1024);
    EXAMPLE_REQUIRE(too_big.err == PROVEN_ERR_NOMEM, "a 256-byte arena refuses 1 KiB");

    proven_arena_destroy(&arena);
    return EXAMPLE_OK();
}
```

**눈여겨볼 것.** 바퀴마다 쓰는 바이트 수가 같고, 바퀴마다 첫 문자열이 `backing`의 맨 앞에
떨어집니다 --- 프로그램이 그것을 확인합니다. reset 은 문자열 두 개를 푼 것이 아니라 숫자 하나를
0으로 되돌렸을 뿐이고, 그래서 한 바퀴에 문자열을 둘 만들든 이천 개 만들든 값이 같습니다. 그리고
아레나가 너무 작으면 답은 `PROVEN_ERR_NOMEM`, 3과와 같은 오류 값이지 죽는 것이 아닙니다.

**처음에 잘 걸리는 것.** reset 뒤에도 `line`과 `note`는 옛 포인터를 그대로 들고 있고, 그 포인터는
이제 다음 바퀴가 덮어쓸 기억을 가리킵니다. reset 은 아레나가 내준 모든 포인터를 한꺼번에 허공을
가리키게 만듭니다. 아레나 밖으로 복사해 두지 않은 것은 바퀴 너머로 들고 가지 마세요.

---

## 8과 — 컨테이너는 자기 할당자를 기억한다

**새로 나오는 것 하나:** 컨테이너는 할당자(allocator)를 만들 때 한 번 받아서 간직한다.

3과의 문자열은 자리가 정해져 있었고, 할당자를 `create`에도 `destroy`에도 건넸습니다. 자라는
배열은 그렇게 할 수 없습니다. 자리를 넘치는 push 는 호출자가 계획하지 않은 순간에 다시 할당해야
하기 때문입니다. 그래서 배열은 만들 때 받은 할당자를 간직합니다. 한 번 건네고 나면 push, get,
destroy 는 배열만 받습니다.

<!-- example: manual/examples/ko/tut_08_containers.c -->
```c
/*
 * 8과 - 컨테이너는 자기 할당자를 기억한다.
 *
 * 문자열은 할당자로 만들고 같은 할당자로 없앤다. 두 번 다 여러분이 건넨다.
 * 자라는 배열은 push 가 저장 공간을 넘칠 때마다 나중에 다시 할당해야 하므로,
 * 만들 때 받은 할당자를 간직한다. 새로 나오는 것은 그 하나다. 할당자는 만들 때
 * 한 번 건네고, 그 뒤의 push, get, destroy 는 배열 말고는 아무것도 받지 않는다.
 *
 * PROVEN_ARRAY_* 매크로는 원소 타입을 인자로 받아 값을 그 타입으로 만든다.
 * 그래서 그 타입으로 바뀔 수 없는 값 - int 배열에 push 하는 구조체 - 은
 * 컴파일되지 않는다. 적어 준 타입이 배열을 만들 때의 타입과 같은지는 알지
 * 못한다. 매번 같은 타입을 적는 것은 여러분 몫이다.
 */

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    /* 2 는 처음 용량이지 한도가 아니다. */
    proven_result_array_t made = PROVEN_ARRAY_INIT(alloc, int, 2);
    EXAMPLE_REQUIRE(proven_is_ok(made.err), "creating an empty array must succeed");
    if (!proven_is_ok(made.err)) return 1;
    proven_array_t squares = made.value;

    /* 둘 들어갈 자리에 열 번 push 한다. 들어가지 않는 push 마다 배열이 기억한
     * 할당자로 저장 공간을 늘린다. 여기에는 할당자가 없다. */
    for (int i = 1; i <= 10; ++i) {
        proven_err_t err = PROVEN_ARRAY_PUSH(&squares, int, i * i);
        EXAMPLE_REQUIRE(proven_is_ok(err), "the heap can grow a ten-int array");
        if (!proven_is_ok(err)) {
            PROVEN_ARRAY_DESTROY(&squares);
            return 1;
        }
    }
    EXAMPLE_REQUIRE(squares.len == 10, "ten pushes, ten elements");

    /* get 은 원소를 가리키는 포인터를 돌려주고, 인덱스가 끝을 넘으면 NULL 을
     * 돌려준다. 범위를 벗어난 인덱스는 엉뚱한 곳 읽기가 아니라 검사할 수 있는 답이다. */
    const int *third = PROVEN_ARRAY_GET(&squares, int, 2);
    EXAMPLE_REQUIRE(third && *third == 9, "element 2 is 3 * 3");
    EXAMPLE_REQUIRE(PROVEN_ARRAY_GET(&squares, int, 10) == NULL, "index 10 is past the end");

    int sum = 0;
    for (proven_size_t i = 0; i < squares.len; ++i) {
        sum += *PROVEN_ARRAY_GET(&squares, int, i);
    }
    EXAMPLE_REQUIRE(sum == 385, "1 + 4 + 9 + ... + 100");
    proven_println("{} squares, sum {}", PROVEN_ARG(squares.len), PROVEN_ARG(sum));

    /* destroy 는 배열만 받는다. 간직해 둔 할당자로 풀어 준다. */
    PROVEN_ARRAY_DESTROY(&squares);
    return EXAMPLE_OK();
}
```

**눈여겨볼 것.** 배열은 둘 들어갈 자리로 만들어져 push 를 열 번 받았는데, 그 어디에도 할당자가
나오지 않습니다. 끝을 넘는 `PROVEN_ARRAY_GET`은 `NULL`입니다 --- 저장 공간 너머에 무엇이 있든
읽어 버리는 것이 아니라, 확인할 수 있는 답입니다. 매크로가 원소 타입(`int`)을 인자로 받으므로
그 타입으로 바뀔 수 없는 값(구조체 같은)은 컴파일되지 않습니다. 배열을 만들 때와 같은 타입을 적는
것은 여전히 여러분 몫입니다.

**처음에 잘 걸리는 것.** `PROVEN_ARRAY_GET`이 돌려주는 포인터는 배열의 저장 공간 *안*을 가리킵니다.
다음 push 가 저장 공간을 더 큰 덩어리로 옮길 수 있고, 그러면 옛 포인터는 허공을 가리킵니다. 꺼내서
쓰고, push 너머로 들고 가지 마세요. [4장](manual-04-containers-algorithms-ko.md)에 이 실수가 통째로
나옵니다. 7과의 걸림돌이 같은 이유로 다시 나온 것입니다 --- 그 기억이 언제 옮겨질지는 여러분의
포인터가 아닌 다른 것이 정합니다.

---

## 9과 — 바깥 세상도 실패하고, 같은 방식으로 말한다

**새로 나오는 것 하나:** 여러분 프로그램 탓이 아닌 실패도 같은 오류 값으로 온다.

지금까지의 실패는 모두 프로그램 안에 원인이 있었습니다 --- 자리가 모자라거나 인자가 틀렸거나.
파일은 바깥의 실패를 데려옵니다. 없는 이름, 쓸 권한이 없는 디렉터리, 가득 찬 디스크. 어느 것도
버그가 아니고, 모두 멀쩡한 프로그램에게 일어납니다. 라이브러리는 이것들도 앞에서와 같은
`proven_err_t` 값으로 알리므로, 새로 배울 두 번째 오류 체계가 없습니다.

쓰는 호출은 통파일 호출입니다. `proven_fs_write_file_atomic`은 호출 하나로 파일을 쓰고,
`proven_fs_read_all_u8str`은 파일을 소유하는 문자열로 되읽습니다. open 도, 읽기 루프도, 잊어버릴
close 도 없습니다.

<!-- example: manual/examples/ko/tut_09_files.c -->
```c
/*
 * 9과 - 바깥 세상도 실패하고, 같은 방식으로 그렇다고 말한다.
 *
 * 지금까지는 프로그램 안의 이유로만 실패할 수 있었다. 자리가 모자라거나 인자가
 * 틀렸거나. 파일은 없을 수도, 읽을 수 없을 수도, 디스크가 가득 찼을 수도 있고,
 * 그 어느 것도 여러분 코드의 버그가 아니다. 라이브러리는 그런 실패도 앞에서와
 * 같은 오류 값으로 알린다. 그러니 이미 배운 검사가 검사의 전부다.
 *
 * 여기 쓰는 것은 통파일 호출이다. 호출 하나로 파일을 쓰고, 하나로 되읽는다.
 * open 도, 읽기 루프도, 잊어버릴 close 도 없다.
 */

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    /* 현재 디렉터리의 이름 하나. 프로그램은 끝나기 전에 이것을 지운다. */
    proven_u8str_view_t path = PROVEN_LIT("proven_tutorial_notes.tmp");
    proven_u8str_view_t text = PROVEN_LIT("buy milk\ncall home\n");

    /* 원자적이다. 읽는 쪽은 옛 파일이나 새 파일을 보지, 반쪽을 보지 않는다.
     * 할당자는 호출이 경로를 다루며 쓸 수 있는 임시 작업 공간이다. */
    proven_err_t err = proven_fs_write_file_atomic(alloc, path, proven_mem_view_from_u8(text));
    EXAMPLE_REQUIRE(proven_is_ok(err), "writing a small file in the current directory should work");
    if (!proven_is_ok(err)) return 1;

    /* 읽기는 소유하는 문자열을 돌려준다. 4과의 모양, 5과의 규칙 그대로다.
     * `alloc` 으로 만들어졌으니 `alloc` 으로 없앤다. */
    proven_result_u8str_t back = proven_fs_read_all_u8str(alloc, path);
    EXAMPLE_REQUIRE(proven_is_ok(back.err), "the file we just wrote can be read");
    if (proven_is_ok(back.err)) {
        EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_as_view(&back.value), text),
                        "the bytes come back exactly as written");
        proven_println("read {} bytes back", PROVEN_ARG(proven_u8str_as_view(&back.value).size));
        proven_u8str_destroy(alloc, &back.value);
    }

    err = proven_fs_remove(alloc, path);
    EXAMPLE_REQUIRE(proven_is_ok(err), "removing the file we created should work");

    /* 이제 파일은 없고, 그것을 읽는 것은 프로그램 바깥에서 온 실패다.
     * 그 실패도 보통 값으로 오고, 어떤 실패인지 말해 준다. */
    proven_result_u8str_t missing = proven_fs_read_all_u8str(alloc, path);
    EXAMPLE_REQUIRE(missing.err == PROVEN_ERR_NOT_FOUND, "a removed file reads as NOT_FOUND");
    if (proven_is_ok(missing.err)) proven_u8str_destroy(alloc, &missing.value);
    proven_println("reading it again: not found, as expected");

    return EXAMPLE_OK();
}
```

**눈여겨볼 것.** `proven_fs_read_all_u8str`이 돌려주는 문자열은 소유하는 것입니다. `alloc`으로
만들고 `alloc`으로 없앱니다 --- 5과의 규칙 그대로입니다. 지운 파일을 다시 읽으면
`PROVEN_ERR_NOT_FOUND`가 옵니다. 3과의 거부와 같은 종류의 값이고, 무엇이 잘못됐는지 이름을
댑니다. 쓰기 이름의 *atomic(원자적)*은 읽는 쪽이 옛 파일이나 새 파일을 보지 둘이 섞인 것은 보지
않는다는 뜻입니다. 무엇을 약속하지 *않는지*(예컨대 정전을 견디는 것)는
[5장](manual-05-hosted-services-ko.md)에 있습니다.

**처음에 잘 걸리는 것.** 쓰기와 `proven_fs_remove`에 건네는 할당자는 경로를 다루는 데 쓰는 임시
작업 공간이지 무언가의 주인이 아닙니다. 그 호출들은 가져간 것을 돌아오기 전에 다 돌려줍니다.
destroy 할 것이 생기는 것은 문자열을 건네주는 읽기뿐입니다.

---

## 다음에 읽을 것

| 알고 싶은 것 | 읽을 곳 |
|---|---|
| 위 내용의 완전한 판 | [0장](manual-00-start-here-ko.md) |
| 타입·오류·계약의 자세한 것 | [1장](manual-01-foundation-ko.md) |
| 아레나·풀·버퍼 제대로 | [2장](manual-02-allocation-ko.md) |
| 소유하는 문자열, 자르기, 인코딩 | [3장](manual-03-strings-text-ko.md) |
| 배열·맵·리스트·해시 | [4장](manual-04-containers-algorithms-ko.md) |
| 파일·스트림·시간·난수 | [5장](manual-05-hosted-services-ko.md) |
| 모르는 낱말 | [용어집](manual-00-start-here-ko.md#13-부록-b-용어집) |
