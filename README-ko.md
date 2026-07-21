# Proven C library

[English](README.md) · **한국어**

> 하나의 생각 위에 세운 C23 시스템 라이브러리: **메모리는 자기가 어디서 왔는지 알아야 합니다.**

C 책을 한 권 뗐다고 해 봅시다. 포인터도, `malloc`도, `printf`도, `char *`도 압니다. 그리고 처음으로 진짜
프로그램을 쓰다가 책이 다루지 않은 부분을 만납니다 — `strcpy`는 당신의 버퍼가 얼마나 큰지 전혀 모르고,
`malloc`이 `NULL`을 돌려주는 것은 그냥 기억해야 하는 일이며, `printf("%d", 3.0)`은 컴파일된다는 것.

`proven`은 그 문제들에 하나씩 답을 낸 결과입니다. 순수한 C이고, 프레임워크가 아닙니다. C 프로젝트가
결국 매번 다시 짜게 되는 그 계층입니다 — 넘겨받는 allocator, 자기 길이를 들고 다니는 문자열, 컨테이너,
포맷팅과 스캐닝, 파일, 해시, 난수 — 소유권과 실패가 모든 시그니처에 드러난 채로.

**처음이라면 [0장](manual-ko/manual-00-start-here-ko.md)부터.** 입문서 한 권 외에는 아무것도
전제하지 않으며, 이 저장소에서 찾아보는 문서가 아니라 읽으라고 쓴 유일한 문서입니다.

- 버전: proven_c_lib-v26.07.20f · 표준: C23 · 라이선스: MIT
- 저장소: https://github.com/rubidus-api/proven_c_lib

---

## 이름의 유래: provenance

**Provenance**(프로버넌스)는 미술계 용어입니다. 어떤 작품의 내력 — 어디서 왔고, 누가 소장했고, 어떻게
여기까지 왔는지를 문서로 남긴 것입니다. provenance가 없는 그림은 진품일 수는 있어도, 아무도 그것을
증명할 수 없습니다.

C의 메모리 모델은 거의 정확히 같은 뜻으로 이 단어를 쓰며, 정의하기보다 *직접 보는* 편이 쉽습니다.
완전한 프로그램 하나가 여기 있습니다. `int` 포인터 둘, 유일한 차이는 각자가 어디서 왔는가입니다:

```c
#include <stdio.h>
#include <string.h>
int y = 2, x = 1;                       /* 컴파일러가 둘을 인접 배치할 가능성이 높다 */
int main(void) {
    int *p = &x + 1;                    /* x에서 유래 — 주소는 x 바로 다음 */
    int *q = &y;                        /* y에서 유래 — 다른 객체 */
    if (memcmp(&p, &q, sizeof p) != 0)  /* 두 포인터가 비트까지 동일한 주소를 담을 */
        return 0;                       /* 때에만 진행한다 */
    *p = 11;                            /* p를 통해 11을 씀 */
    printf("*p = %d, *q = %d\n", *p, *q);
}
```

```text
gcc -O1 :  *p = 11, *q = 11     # 같은 주소; 둘 다 11로 읽힘
gcc -O2 :  *p = 11, *q = 2      # 같은 주소 — 그런데 *p는 11, *q는 2
```

그 `-O2` 줄을 다시 보세요. `p`와 `q`는 **비트까지 동일한 주소**를 담고 있습니다 — `memcmp`가 두
포인터의 원시 바이트를 비교해, 일치할 때에만 프로그램을 진행시켰습니다. `p`를 통해 `11`을 씁니다.
그런데 `p`를 역참조하면 `11`, `q`를 역참조하면 `2`입니다. **한 주소, 두 값.** 경쟁 상태도, 초기화
안 된 메모리도, 미정의 *출력*도 아닙니다 — 프로그램은 결정적이고 매번 이렇게 찍습니다. 컴파일러는
`p`가 `x`에서 유래했음을 알기에, 그것을 통한 쓰기가 `y`에 닿을 수 없다고 가정하고 `y`를 레지스터에
붙들어 둡니다. 그래서 주소가 마지막 비트까지 같아도 `*p`와 `*q`는 서로 다른 것을 가리킵니다.

여기서 **무엇이 문제가 아닌지**에 주목하세요. `&x + 1`을 형성하는 것은 완벽히 합법입니다 — 표준은
객체 끝 바로 다음을 가리키는 포인터를 만드는 것을 명시적으로 허용합니다. 코드를 읽어서 잡아낼 만한
"범위 밖" 접근은 아무것도 없습니다. 놀라운 부분은 전적으로 마지막 단계에 있습니다. 두 포인터가
비트까지 같은 주소이면서도 같은 포인터가 아닐 수 있다는 것, 각자가 자기가 온 객체의 정체성을 지니기
때문입니다. 그 정체성이 그것의 **provenance**이고, 컴파일러는 주소가 둘을 구별하지 못하는 자리에서도
그것을 실재하는 것으로 다룹니다. (GCC는 여기서 `&x + 1` 쓰기에 대해 경고까지 합니다 — 그러고 나서
그대로 오컴파일합니다.)

이것은 표준 문헌 애호가의 호기심거리가 아닙니다. 최적화의 상당 부분이 여기서 나오며, 디버거로는
보여줄 수 없는 부류의 버그입니다. 오컴파일은 디버거가 무언가를 보기도 전에 일어나기 때문입니다. 이
모델은 진행 중인 실제 표준화 작업입니다. ISO WG14의 provenance 기술 명세
([N2577](https://www.open-std.org/jtc1/sc22/wg14/www/docs/n2577.pdf) →
[N3005](https://www.open-std.org/jtc1/sc22/wg14/www/docs/n3005.pdf))이며, 위원회가 선택한 모델은
**PNVI-ae-udi**입니다. C23 본문에 들어간 것이 아니라 출판을 앞둔 TS이지만, 방향은 정해졌습니다.

### 규칙은 하나가 아니라 둘: strict aliasing과 provenance

provenance를 그 더 오래되고 유명한 형제 — **strict aliasing** — 과 뭉뚱그려 "컴파일러가 영리하게
구는 것" 정도로 묶고 싶어지기 쉽습니다. 둘은 뿌리를 공유합니다. C의 추상 기계가 하드웨어보다 메모리를
더 엄격하게 모형화한다는 것입니다. 하지만 둘은 서로 다른 질문에 답하는 *별개의 규칙*이며, 곧 보게
되듯이 한쪽을 끄는 컴파일러 플래그가 다른 쪽은 그대로 두고 갑니다.

| | strict aliasing | provenance |
|---|---|---|
| 던지는 질문 | 이 주소에는 무슨 **타입**이 저장돼 있나? | 이 포인터는 어느 **객체**에 닿아도 되나? |
| 나이 | 오래됨 — C89/C99의 "실효 타입" | 새로움 — WG14 TS(N3005), 출판 대기 |
| 컴파일러의 가정 | 호환되지 않는 타입의 두 포인터는 결코 같은 객체를 가리키지 않는다 | 한 객체에서 유래한 포인터는 다른 객체에 접근할 수 없다 |
| 걸려드는 방식 | 메모리를 틀린 타입으로 읽음(타입 퍼닝) | 포인터를 객체 밖으로 밀거나 세탁한 뒤, 다른 객체가 있는 자리에서 씀 |
| 축복받은 탈출구 | 원시 바이트를 `unsigned char`로 접근 — 이것이 바로 `proven_byte_t` | 포인터 산술을 한 객체 안에 머무르게, 포인터를 정수에서 되짓지 말 것 |

둘 다 걸려들기 쉽고, 둘 다 `-O0`에서는 옳고 `-O2`에서는 틀린 코드를 만듭니다 — 디버그 빌드에서 돌린
모든 테스트를 통과하고 살아남으므로, 가능한 최악의 실패 방식입니다. provenance 쪽은 이미 보았습니다.
이 절 첫머리의 `*p = 11, *q = 2` 프로그램이 바로 그것입니다 — `int *` 포인터 둘, 타입 퍼닝 없음,
최적화에서만 틀림. 그 strict-aliasing 쌍둥이는 **경고 한 줄 없이** 걸려들며, 손으로 짠 모든 파서와
직렬화기의 모습입니다: 바이트 버퍼를 폭이 다른 두 포인터로 읽는 것.

```c
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
int main(void) {
    void *buf = malloc(8);
    uint32_t *w = buf;      /* 같은 메모리를 32비트로 봄 */
    uint16_t *h = buf;      /* 같은 메모리를 16비트로 봄 — 캐스트도, 경고도 없음 */
    *w = 0xAAAAAAAAu;
    *h = 0x1234;            /* 하위 절반을 바꿈 */
    printf("%08x\n", *w);   /* 그 쓰기가 반영됐나? */
    free(buf);
}
```

```text
gcc -O0 :  aaaa1234     # 16비트 쓰기가 반영됨
gcc -O2 :  aaaaaaaa     # 쓰기가 사라짐 — 컴파일러가 h와 w는 겹칠 수 없다고 가정
```

그리고 이 둘이 하나가 아니라 두 규칙임을 증명하는 세부:

```text
                     -O2      -O2 -fno-strict-aliasing
strict-aliasing 버그  깨짐     고쳐짐
provenance 버그       깨짐     여전히 깨짐   <- 애초에 aliasing이 아니었음
```

`-fno-strict-aliasing`은 큰 C 프로젝트 — 리눅스 커널을 비롯해 — 이 첫 번째 부류의 버그를 없애려고
집는 플래그입니다. 두 번째에는 아무 효과가 없습니다. provenance는 그런 스위치가 없는 별개의 규칙이기
때문입니다. 여기서 빠져나갈 수는 없고, 걸려들지 않도록 피할 수 있을 뿐입니다.

이것이 라이브러리 전체의 논거를 축소해 보여줍니다. 보이지 않는 두 규칙, 경고 없음, 최적화에서만 틀림,
그리고 그중 하나에만 탈출 플래그가 있음. 라이브러리의 답은 올바른 쪽을 기본으로 만드는 것입니다. 원시
바이트는 `proven_byte_t`를 거칩니다. strict aliasing이 면제하는 유일한 타입이라, 위 첫 번째 버그는
평범한 API로는 쓸 수가 없습니다. 길이는 포인터와 함께 다니므로, 산술이 객체 끝 밖으로 벗어날 이유가
없습니다. 무엇도 정수로 세탁되지 않습니다.

**정직한 예외 하나, 그리고 그것이 이 라이브러리가 지금 자리를 겨냥하는 이유입니다.** 인트루시브
리스트는 어떤 멤버를 가리키는 포인터에서 그것을 감싼 객체를 되찾습니다 — 고전적인 `container_of`,
여기서는 `(type *)((proven_byte_t *)ptr - offsetof(type, member))`입니다. 이 관용구는 실제 C가 사는
곳 어디에나, 무엇보다 리눅스 커널에 있으며, 객체 모델의 가장 엄격한 해석이 한 번도 편히 승인한 적
없는 바로 그 경우입니다. 유래가 *멤버*인 포인터를 *구조체 전체*에 닿는 데 쓰는 것이죠. 그래서
`proven`은 엄격한 provenance 순수성을 **주장하지 않습니다** — 인트루시브 컨테이너를 제공하면서 동시에
그럴 수는 없습니다. 이 라이브러리는 모든 C 프로그래머가 이미 존중하고 새니타이저가 실제로 검사하는,
정착되고 보편적으로 합의된 미정의 동작을 방어합니다. 그리고 아직 정착되지 않은 최전선 — 지배적이고
숱하게 검증된 기법이 엄격한 모형과 부딪히는 그곳 — 은 정직하게, 정착되지 않은 것으로 다룹니다.
provenance는 이 라이브러리가 기우는 방향이지, 이미 완성했다고 내세우는 보증이 아닙니다. 그 경계를
제대로 잡는 것, 그리고 특정 API가 어느 쪽에 서 있는지를 분명히 하는 것 자체가 이 프로젝트가 풀어 가고
있는 일의 일부입니다.

### 이 라이브러리가 그 이름을 갖게 된 경위

**`proven`이라는 이름은 *prove*가 아니라 *provenance*에서 왔습니다.**

저는 이 개념들을 늦게 만났습니다. 한동안은 평범한 심상 모형으로 C를 써 왔습니다 — 메모리는 바이트고,
포인터는 주소라는. 그러다 strict aliasing을 읽었고, 이어서 포인터 provenance를 읽었고, 발밑이
움직였습니다. 제가 그동안 미묘하게 틀린 코드를 쓰고도 무사했다는 이야기가 아닙니다 — 아마 그랬을 겁니다.
핵심은 **C가 메모리에 대해 두고 있는 규칙이 제가 머릿속에 지니고 다니던 모형보다 훨씬 엄격하다**는
깨달음이었고, 그 둘 사이의 간극이야말로 아무도 재현하지 못하는 버그가 나오는 자리라는 것이었습니다.

당연한 대응은 규칙을 제대로 익혀서 손으로 지키는 것입니다. 그것으로 충분하지 않았던 이유를 정직하게
적어 두고 싶습니다. **저는 이 규칙들을 잘 알지 못하고, 평범한 코드를 쓰는 동안 그것들을 머릿속에
안정적으로 붙들고 있지 못합니다.** 실효 타입, 어떤 캐스트가 provenance를 세탁하는 순간, 어떤 탈출구가
표준이 축복한 것이고 어떤 것이 그저 오늘 우연히 동작하는 것인지 — 이건 정말로 어렵고, "조심하라"는
말은 전략이 아닙니다.

그래서 대응은 반대 방향이 되었습니다. 규칙을 제가 안정적으로 붙들 수 없다면, 규칙은 제 주의력이 아닌
다른 곳에 살아야 합니다. **올바른 쪽이 쉬운 쪽이 되도록 만드는 라이브러리, 그리고 모든 시그니처에
드러나는 관습 안에.** 원시 바이트는 `proven_byte_t`를 거칩니다. 표현을 들여다보라고 표준이 실제로
축복한 타입이 그것이기 때문입니다. 길이는 포인터와 함께 다닙니다. 그래야 틀릴 산술이 없습니다. 크기는
검사 매크로를 거칩니다. 조용한 감김은 제가 반드시 잊어버릴 규칙이기 때문입니다. 이 중 어느 것도 그
순간의 제가 조심할 것을 요구하지 않습니다. 한 번, 여기서 조심할 것을 요구할 뿐입니다.

이름이 기록하는 것이 그것입니다. 코드가 증명되었다는 뜻이 아니라, *provenance*를 중심에 놓고 지었다는
뜻입니다 — 메모리는 자기가 어디서 왔는지의 내력을 지녀야 하고, 라이브러리가 그것을 손으로 추적하게
만들어서는 안 된다는 생각입니다.

`proven_c_lib`는 AI를 협업자로 삼아 만들었고, 그것도 같은 생각의 일부입니다. 메모리 모델 전체를 머리에
담아 두지 못하는 사람에게 도움이 되는 명시성은, 언어 모델이 "호출부에 드러난 이유로 옳은" 코드를
내놓게 하는 명시성과 같은 것이기 때문입니다. 우연히 옳은 코드가 아니라.

**그리고 우연이 하나 있습니다.** *proven*은 "증명된, 검증된, 사실로 드러난"이라는 뜻이기도 합니다 — 이
저장소가 결국 어떤 모습이 되었는지를 생각하면, 제가 계획한 그 무엇보다도 잘 들어맞습니다. 등록된 테스트
170개, 매뉴얼의 모든 예제를 빌드가 컴파일하고 실행하며, 문서에도 게이트가 걸려 있어 존재하지 않는
함수를 주장할 수 없습니다. 두 단어는 어원이 다릅니다. *provenance*는 라틴어 *provenire*("나오다"),
*proven*은 *probare*("검증하다")에서 왔습니다. 뿌리가 다른 두 단어가 공교롭게 같은 일곱 글자에 도착한
것이고, 그 우연이 의도보다 이 프로젝트를 더 잘 설명합니다.

---

## C는 포터블 어셈블리가 아니다

C를 기계 위에 얇게 덮인 정직한 한 겹으로 보는 화법이 있습니다. 바이트는 바이트고, 포인터는 주소이며,
캐스팅은 공짜이고, 표준은 하드웨어를 아는 사람의 앞길을 막는 형식일 뿐이라는 관점입니다. 이 관점은
영리한 코드를 낳습니다 — 정수와 포인터를 자유롭게 섞고, aliasing 기교를 부리고, union을 재해석 캐스트로
쓰는. 그리고 컴파일러가 거의 곧이곧대로 번역하던 1980년에는 그럴듯했습니다.

지금은 틀렸고, **왜** 틀렸는지를 분명히 해 둘 값어치가 있습니다. 이유는 컴파일러가 적대적으로 변했기
때문이 아닙니다.

**C의 추상 기계는 언제나 하드웨어보다 엄격했습니다.** 누구의 의견이 아니라 표준에서 나온 예 셋입니다:

- **실효 타입(effective type)과 strict aliasing.** 객체에 저장된 값은 호환되는 타입의 lvalue를 통해서만
  접근할 수 있습니다 — 문자 타입에는 의도적인 예외가 있습니다. C의 모델에서 메모리는
  *타입을 가집니다*. 어셈블리에는 그런 개념 자체가 없습니다.
- **provenance**, 위에서 본 것. 하드웨어는 주소를 보고, C는 주소 *그리고 그것이 어디서 왔는지*를 봅니다.
- **미정의 동작은 "기계가 하는 대로"가 아닙니다.** 표준이 아무런 요구도 하지 않는다는 뜻이며, 최적화기는
  그런 일이 결코 일어나지 않는다고 가정해도 됩니다 — UB가, 바로 그것을 막으려고 쓴 `if`를 지워 버릴 수
  있는 이유가 이것입니다.

그러니 옛 프로그램들이 어느 날 옳지 않게 된 것이 아닙니다. 애초에 옳았던 적이 없고, 다만 **동작했을**
뿐입니다. 표준이 처음부터 내어 준 자유를 아무도 아직 써먹지 않았기 때문입니다.

정직하게 요약하면 이렇습니다. **C는 쓸 수 있는 것에 대해서는 관대하고, 약속하는 것에 대해서는 엄격합니다.**
"포터블 어셈블리" 관점은 그 둘을 뒤섞습니다. C에 탈출구가 없는 것은 아닙니다 — `unsigned char`로 바이트를
들여다보기, `memcpy`를 통한 타입 퍼닝, `uintptr_t` 왕복 — 그러나 그것들은 *좁고 명시된* 통로이지
일반 면허가 아닙니다.

이 라이브러리는 그 사실을 우회하지 않고 받아들입니다. 원시 바이트는 `proven_byte_t`(어떤 객체의 바이트든
합법적으로 들여다볼 수 있는 유일한 타입인 `unsigned char`의 별칭)입니다. view는 포인터와 희망이 아니라
포인터와 길이를 지닙니다. 크기 산술은 검사 매크로를 거치는데, 부호 없는 오버플로가 조용히 그리고
합법적으로 감기기 때문입니다. 이것은 방어적 프로그래밍이 아니라, 실제로 명세된 그 언어를 쓰는 것입니다.

---

## 시스템 언어들은 어디로 가고 있나

지난 10년 동안 문자열과 메모리에 대해 대략의 합의가 만들어졌습니다. 여러 언어가 각자 도달한 결론이라
볼 만합니다. `proven`은 같은 답의 C판이기 때문입니다.

### 문자열: 길이를 포인터 옆에 둔다

NUL로 끝나는 문자열은 문자열 하나당 1바이트를 아끼려던 1970년대의 결정이었습니다. 청구서는 막대했습니다.
길이를 아는 데 *O(n)* 탐색이 들고, 텍스트가 0 바이트를 담을 수 없으며, 종료 문자가 없으면 아무도
탐지하지 못하는 버퍼 오버런이 됩니다.

거의 모든 대안이 같은 해법을 다시 발견합니다 — **길이를 포인터 옆에 두는 것**:

| | 소유(owning) | 차용(borrowed) |
|---|---|---|
| **Pascal** (1970년대) | 길이 접두 문자열 | — |
| **C++17** | `std::string` | `std::string_view` |
| **Rust** | `String` | `&str` |
| **Zig** | `std.ArrayList(u8)` | `[]const u8` (슬라이스: 포인터 + 길이) |
| **Go** | — | `string`, `[]byte` |
| **`proven`** | `proven_u8str_t` | `proven_u8str_view_t` |

Pascal이 길이 접두로 먼저 도달했습니다. C++17의 `string_view`는 *차용* 쪽 절반을 널리 퍼뜨렸습니다 — 대부분의
함수는 텍스트를 소유하려는 게 아니라 *읽으려* 할 뿐이고, 넘기려고 문자열을 복사하는 것이 프로그램에서 가장
흔한 불필요한 할당이라는 관찰입니다. Rust와 Zig는 그 구분을 처음부터 타입 시스템에 넣었습니다.

뒤쪽 절반이 앞쪽만큼 중요합니다. **소유와 차용은 서로 다른 타입이어야 합니다.** `char *`는 네 가지를
뜻합니다 — 방금 할당된 것, 호출자 버퍼 안을 가리키는 것, 읽기 전용 메모리의 문자열 리터럴, 다음 호출이
덮어쓸 정적 버퍼 — 그리고 타입은 그중 무엇인지 알려 주지 못합니다. 둘을 쪼개면 그 답이 시그니처에 적힙니다.

### 메모리: allocator는 매개변수다

`malloc`은 전역입니다. 어떤 함수가 할당하는지 시그니처가 말해 주지 않고, 프로그램의 한 부분만 전략을
바꿀 수 없으며, 전역으로 가로채지 않고는 실패 경로를 시험할 수 없고, 힙이 없는 곳에서는 아예 쓸 수 없습니다.

Zig의 답 — 할당하는 모든 것에 `Allocator` 인터페이스를 명시적으로 넘기는 것 — 이 오늘날 그 대안을 가장
분명하게 말해 주며, `proven`이 따르는 것도 그것입니다. 할당이 매개변수가 되는 순간, 세 전략이 호출
지점에서 서로 교체 가능해집니다:

| | 동작 방식 | 개별 해제 | 언제 쓰나 |
|---|---|---|---|
| **Heap** | 인터페이스 뒤의 `malloc`/`free` | 가능 | 일반적인 경우. |
| **Arena** | 블록 하나에서 포인터를 밀어 나감 | **불가** — no-op이고, 전체를 reset합니다 | 함께 죽는 다수의 할당: 요청 하나, 프레임 하나, 파싱 한 번. |
| **Pool** | 고정 크기 슬롯 + 프리 리스트 | 가능, 리스트로 반환 | 한 가지 크기의 객체를 계속 만들고 없앨 때. |

arena는 `free` 만 번을 `reset` 한 번으로 바꿉니다. 그리고 리눅스에서 도는 코드가 힙 없는
마이크로컨트롤러에서도 그대로 도는 이유이기도 합니다: `static` 배열 위에 arena를 얹으면 나머지는 바뀌지
않습니다.

### 모던 C에는 이미 도구가 있다

필요한 조각들은 이미 언어 안에 있고, 대부분의 C 코드가 아직 따라잡지 못했을 뿐입니다:

- **C99** — 지정 초기화자, 복합 리터럴: 매개변수 열 개짜리 함수 대신 옵션 구조체.
- **C11** — `_Generic`. `{}`가 포맷 문자열이 아니라 인자에서 타입을 얻어 오는 방법이 이것입니다.
- **C23** — `[[nodiscard]]`(이 저장소에서 172번 쓰였고, 그래서 컴파일러가 에러를 버리는 코드를 거부합니다),
  검사 산술을 위한 `<stdckdint.h>`, `constexpr`, `typeof`, `nullptr`.

Andre Weissflog의 [*Modern C for C++ Peeps*](https://floooh.github.io/2019/09/27/modern-c-for-cpp-peeps.html)와
Luca Sas의 ACCU 2021 발표 **Modern C and What We Can Learn From It**
([영상](https://www.youtube.com/watch?v=QpAhX-gsHMs) ·
[슬라이드](https://accu.org/conf-docs/PDFs_2021/luca_sass_modern_c_and_what_we_can_learn_from_it.pdf))이
이 스타일에 대한 가장 좋은 짧은 입문입니다. 이 라이브러리를 그 발표에 비추어 읽은 결과가
[`docs/RFC-0002`](docs/RFC-0002-view-vocabulary-and-splitting.md)입니다 — 해 볼 만한 작업이었습니다. 결과가
대체로 "이미 하고 있는 것들의 목록"과 진짜 빈틈 하나였기 때문입니다.

---

## 이 라이브러리는 무엇을 위한 것인가

**표준 라이브러리의 낡은 부분을 밑바닥부터 대체하되, 배제하지 않기 위해서.** `strcpy`, `strtok`,
`sprintf`, `errno`, `qsort`, `rand`, `atoi` — 각각에 대해 크기를 알고, 검사하고, 명시적인 답이 여기
있습니다. 그러나 `proven`은 libc 대체물이 아니며 당신의 `main`을 차지하려 하지 않습니다. 전역 상태가
없고, 스레드를 띄우지 않으며, `atexit` 핸들러를 등록하지 않고, 당신이 allocator를 건네지 않은 것은
아무것도 할당하지 않습니다. 모듈 하나만 써도 되고, 이미 링크하고 있는 무엇과든 나란히 쓰면 됩니다.
[부록 D](manual-ko/manual-00-start-here-ko.md#7-부록-d-libc-대응표)가 당신이 아는 libc 호출을 무엇으로
바꿔 쓸지, 그리고 왜 다른지를 대응시켜 줍니다.

**사람이 쓰기에 편하고, 사람이 AI와 함께 쓸 때 안전하도록.** 명시적인 쪽이 더 장황하며, 그것이
거래입니다. 그 대가로 얻는 것은 중요한 사실이 전부 **국소적**이라는 점입니다 — 어떤 호출이 할당하는지는
매개변수 목록에, 실패할 수 있는지는 반환 타입에, 커지는지는 이름에 적혀 있습니다. 이것은 지친 사람도,
언어 모델도 주변 코드에서 안정적으로 복원해 내지 못하는 종류의 맥락이며, 문서를 성실함이 아니라 빌드
게이트로 지키는 이유이기도 합니다.

**모던 C가 실제로 버티는지 시험하기 위해서.** 정직하게 말하면 이것이 세 번째 이유입니다. C23을 프레임워크
없이 의도적으로 썼을 때 진짜 지원 계층을 감당할 수 있는지에 대한 살아 있는 실험이고, 그 실험은 공개로
진행됩니다. 모든 설계 결정이 [`docs/`](docs/)의 RFC로 남아 있습니다 — 틀렸다가 정정된 것들까지 포함해서.

---

## 사람들이 이 라이브러리를 찾는 이유

- 소유권이 명시적이라, 누가 할당하고 누가 해제하는지 보기 쉽습니다.
- 실패할 수 있는 API는 결과를 반환하며 실패를 조용히 숨기지 않습니다. 중요한 것들은 `[[nodiscard]]`라
  컴파일러가 에러를 버리도록 두지 않습니다.
- 연산은 **자르지 않고 거부합니다**. 들어가지 않는 경로는 에러이지, 다른 파일을 여는 짧아진 경로가 아닙니다.
- 재할당 계열 연산은 문서화된 범위에서 실패 원자적입니다 — 실패한 grow는 기존 데이터를 그대로 둡니다.
- 차용 view는 소유 객체와 다른 타입입니다.
- 십진/부동소수 변환은 올바르게 반올림되고(호스트 `strtod`/`snprintf`와 비트 단위로 동일),
  `binary32`에 대해 전수 검사했으며, 일반적인 데이터에서는 glibc보다 빠릅니다.
- 호스티드 OS 접근은 `platform/`의 PAL 뒤에 격리되어 있어, freestanding 빌드가 깔끔하게 떼어 냅니다.
- 빌드 시스템은 C 파일 하나, `nob.c`입니다. CMake도, Meson도, npm도, 외부 테스트 프레임워크도 없습니다.

그래서 명령줄 도구, 임베디드에 가까운 코드, 나중에 더 엄격한 플랫폼 경계가 필요해질 수 있는 실험
코드, 그리고 제어를 포기하지 않으면서 간결한 지원 계층을 원하는 C 프로젝트에 쓸 만합니다.

## 빠른 시작

```sh
cc nob.c -o nob     # 빌드 드라이버를 한 번 빌드
./nob build         # 전체 컴파일 + 전체 테스트 실행
```

다른 컴파일러를 쓰려면:

```sh
./nob strict-error -cc clang
```

자주 쓰는 검사들:

```sh
./nob release
./nob asan
./nob ubsan
./nob tsan
./nob regression
./nob bench-float
./nob freestanding
./nob cross -build-root /home/user/work/build/proven_c_lib
```

인자 없이 `./nob`을 실행하면 전체 명령 목록이 나옵니다.

## 작은 예제

이 예제는 owned UTF-8 문자열을 만들고, formatter로 뒤에 내용을 붙인 다음, 출력하고, 같은 allocator 계열로 해제합니다.

```c
#include "proven.h"

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    proven_result_u8str_t r =
        proven_u8str_create_from_view(alloc, PROVEN_LIT("hello"));
    if (!proven_is_ok(r.err)) {
        return 1;
    }

    proven_u8str_t s = r.value;

    proven_fmt_result_t fr =
        proven_u8str_append_fmt_grow(alloc, &s, ", {}", PROVEN_ARG("world"));
    if (!proven_is_ok(fr.err)) {
        proven_u8str_destroy(alloc, &s);
        return 1;
    }

    proven_println("{}", PROVEN_ARG(proven_u8str_as_view(&s)));
    proven_u8str_destroy(alloc, &s);
    return 0;
}
```

hosted 소스에 대해 빌드하려면 다음처럼 하면 됩니다.

```sh
cc -std=c23 -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L \
  -Iinclude -Iplatform \
  app.c src/proven/*.c platform/proven_sys_*.c \
  -pthread -o app
```

## 숨겨진 ownership이 없는 컨테이너

`proven_array_t`는 커질 수 있는 연속 배열입니다. 성장과 해제에 사용할 allocator trait를 저장하므로, 호출부가 memory ownership을 분명히 드러내게 됩니다.

```c
#include "proven.h"

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    proven_result_array_t r = PROVEN_ARRAY_INIT(alloc, int, 4);
    if (!proven_is_ok(r.err)) {
        return 1;
    }

    proven_array_t numbers = r.value;

    if (!proven_is_ok(PROVEN_ARRAY_PUSH(&numbers, int, 10))) goto fail;
    if (!proven_is_ok(PROVEN_ARRAY_PUSH(&numbers, int, 20))) goto fail;

    const int *first = PROVEN_ARRAY_GET(&numbers, int, 0);
    if (!first) goto fail;

    proven_println("first = {}", PROVEN_ARG(*first));
    PROVEN_ARRAY_DESTROY(&numbers);
    return 0;

fail:
    PROVEN_ARRAY_DESTROY(&numbers);
    return 1;
}
```

같은 패턴은 문자열, map, buffer, arena, pool, filesystem helper 전반에 반복됩니다. allocator로 만들고, result를 확인하고, borrowed pointer는 짧게 쓰고, owned 객체는 의도적으로 해제합니다.

## bounded input을 쓰는 텍스트 포맷팅

`PROVEN_ARG("literal")`는 NUL 종료 문자열이 신뢰할 수 있을 때 편리합니다. 하지만 프로그램 바깥에서 들어온 텍스트는, formatter가 노출하려는 바이트보다 더 멀리 검색하지 않도록 bounded view 또는 bounded C-string 인자를 사용하는 편이 좋습니다.

```c
const char packet_text[5] = { 'o', 'k', '!', '!', 'x' };
proven_println("rx: {}", PROVEN_ARG_CSTR_N(packet_text, 4));
```

owned 문자열에 대해 formatter가 자주 쓰이는 모드는 세 가지입니다.

- `proven_u8str_append_fmt`: 고정 용량, atomic.
- `proven_u8str_append_fmt_trunc`: 고정 용량, truncating.
- `proven_u8str_append_fmt_grow`: allocator 기반, atomic.

## 파일 통째로 읽기, 그리고 2차로 만들 수 없는 정렬

대부분의 프로그램이 실제로 원하는 동작인 "파일 통째로 읽기"는 한 번의 호출입니다:

```c
proven_result_u8str_t src = proven_fs_read_all_u8str(alloc, PROVEN_LIT("main.c"));
if (!proven_is_ok(src.err)) return src.err;
proven_u8str_view_t text = proven_u8str_as_view(&src.value);   /* NUL 종단, 추가 복사 없음 */
proven_u8str_destroy(alloc, &src.value);
```

`proven_fs_read_all` / `proven_fs_read_all_u8str`는 미리 잰 크기까지가 아니라 **EOF까지** 읽습니다. 파일이 보고한 크기는 초기 용량을 정하는 데만 쓰이므로 정규 파일은 여전히 할당 1회·패스 1회로 끝나지만, 크기를 미리 알 수 없는 소스(파이프, FIFO, `/proc` 항목)가 빈 결과로 돌아오지 않고, 읽는 도중 자라는 파일도 조용히 잘리지 않습니다.

쓰기도 대칭입니다. `proven_fs_write_file`은 생성하거나 잘라내고, `proven_fs_write_file_atomic`은 형제 임시 파일에 쓴 뒤 rename으로 덮어써서 동시 독자가 옛 파일 전체 또는 새 파일 전체만 보게 합니다. 대상의 권한도 보존됩니다(`0600` 파일이 `0644`로 다시 공개되지 않습니다). 독자에 대해 원자적이며, 전원 손실에 대한 내구성은 별도의 명시적 요청입니다 - `proven_fs_sync`(fsync)와 `proven_fs_write_file_durable`이 있고, 후자는 유일하게 올바른 순서로 세 단계를 수행합니다: 임시 파일 sync → rename → 디렉터리 sync.

`proven_array_sort`는 introsort이고, 실제로 발목을 잡는 두 성질은 명시할 가치가 있습니다:

- **O(n log n)은 보장이지 평균적 기대치가 아닙니다.** 재귀 깊이를 넘어서면 heapsort로 탈출하는 것이 그 보장을 만듭니다. 적대적 입력으로 최악의 경우에 도달할 수 있는 정렬은, 자기가 만들지 않은 데이터를 정렬하는 모든 프로그램에서 서비스 거부(DoS)입니다.
- **중복 키가 빠른 경우입니다.** 피벗과 같은 원소들은 확정된 구간으로 모여 다시 재귀되지 않으므로, 전부 같은 입력은 한 번의 패스로 끝납니다. 저카디널리티 키(상태 컬럼, enum, 버킷 id)야말로 호출자가 실제로 정렬하는 키이고, 순진한 분할이 정확히 그 지점에서 무너집니다.

## 해시, 토큰, 그리고 URL에 넣을 수 있는 텍스트

"해시"도 "난수"도 하나가 아닙니다. 결과를 무엇에 쓰느냐에 따라 정답이 달라지고, 잘못 고르면 쓸데없이 느리거나 **조용히 안전하지 않은** 프로그램이 됩니다. 두 모듈 모두 "무슨 일을 하는가"를 말하는 순간 선택이 정해지도록 구성했고, 잘못된 선택을 실수로 하기 어렵게 이름을 지었습니다.

| 하려는 일 | 쓸 것 |
|---|---|
| **내가 만든** 키를 내 테이블에 해싱 (신뢰된 입력) | `proven_hash_bytes` — FNV-1a, 빠름 |
| **신뢰할 수 없는** 입력의 키를 해싱 | `proven_hash_keyed` — SipHash. (`proven_map`이 이미 이걸 씁니다: 문자열 키 맵은 **기본이 HashDoS 저항**) |
| 디스크·전송 중 **손상** 검출 | `proven_crc32` — gzip/zlib/PNG와 상호운용 |
| 콘텐츠 **지문** — 중복제거, "같은 파일인가?" | `proven_sha256` — **고의로 위조된** 일치까지 막는 유일한 것 |
| 키, 토큰, 논스 | `proven_random_bytes` (OS CSPRNG), 또는 그걸로 시드한 `proven_chacha_rng_t` |
| **재현 가능한** 실행 — 시뮬레이션, 테스트 | `proven_xoshiro256ss_t`. 빠르고 시드로 정확히 재생. **비밀에는 절대 금지** |

```c
/* URL에 안전한 세션 토큰: 강한 바이트 → 이스케이프가 필요 없는 텍스트. */
proven_byte_t raw[16];
proven_byte_t token[32];
proven_size_t n = 0;

if (proven_random_bytes(raw, sizeof raw) &&
    proven_is_ok(proven_base64url_encode(
        (proven_mem_view_t){ raw, sizeof raw }, token, sizeof token, &n))) {
    proven_println("token: {}", PROVEN_ARG_CSTR_N((const char *)token, n));
}
```

`encode.h`가 나머지 절반입니다: `hex`, `base64`, `base64url`(패딩 없음, 이스케이프 불필요). 디코더는 **한 바이트를 쓰기 전에 입력 전체를 검증**합니다 — 이상한 문자는 `PROVEN_ERR_INVALID_ENCODING`이지, 끝을 넘어 읽거나 조용히 짧은 결과가 되지 않습니다. 출력 크기는 외우는 숫자가 아니라 **호출하는 함수**입니다.

생성기와 해시는 순수 연산이라 베어메탈에서도 동작합니다. OS가 없는 보드라면 하드웨어 엔트로피를 한 번 넘겨주면(`proven_random_set_source`) 암호학적 생성기가 운영체제 없이 그대로 작동합니다.

## 스트림: stdin에서 한 줄, 그리고 줄마다 syscall 하지 않는 출력

writer는 바이트 싱크, reader는 바이트 소스입니다. 둘 다 allocator처럼 값으로 넘기는 작은 vtable이라, `serialize(writer, value)` 하나가 메모리·파일·표준 스트림 위에서 모두 동작하고, 포매터를 그중 아무 데나 겨눌 수 있습니다.

```c
/* stdin을 한 줄씩. 버퍼 하나, 줄마다 할당 없음. */
proven_byte_t buf[4096];
proven_sysio_lines_t lines;
if (proven_is_ok(proven_sysio_stdin_lines(&lines,
        (proven_mem_mut_t){ .ptr = buf, .size = sizeof buf }))) {
    for (;;) {
        proven_result_u8str_view_t line = proven_sysio_read_line(&lines);
        if (line.err == PROVEN_ERR_EOF) break;
        if (!proven_is_ok(line.err)) break;   /* 버퍼보다 긴 줄은 잘리지 않고 거부됩니다 */
        proven_println("{}", PROVEN_ARG(line.val));
    }
}
```

반환된 view는 **여러분의 버퍼를 가리키며** 다음 호출까지만 유효합니다 — 이것이 백만 줄을 백만 번의 할당이 아니라 버퍼 하나로 처리하게 만드는 이유입니다. 보관하려면 복사하세요.

출력 쪽을 버퍼링하면 작은 출력 1000번이 syscall 1000번이 아니라 **1번**이 됩니다. 다만 **반드시 flush해야 합니다**: 숨겨진 전역 버퍼가 없으니 대신 flush해 줄 소멸자도, `atexit` 핸들러도 없습니다. 직접 호출(`proven_println`, `proven_eprintln`)이 무버퍼로 남아 있는 이유가 바로 이것입니다 — 반환 전에 이미 나가 있습니다.

## 정확하고 빠른 숫자 변환

십진수 → `double` 파싱과 `double`/`float` → 십진수 포매팅은 정확히 반올림되며
(round-to-nearest, ties to even), `long double` 없이 정수 연산만으로 계산됩니다.
파서는 호스트 `strtod`와 비트 단위로 동일하고, 고정 `%f`/`%e` 출력은 호스트
`snprintf`와 일치하며, shortest 모드는 round-trip 가능한 최소 길이 문자열을 냅니다.

```c
#include "proven/scan.h"
#include "proven/float_format.h"

/* 파싱: 정확히 반올림, NUL 종단 불필요(길이 기반 view). */
proven_scan_t sc = proven_scan_init(proven_u8str_view_from_cstr("3.14159e2"));
double v = proven_scan_f64(&sc).val;            /* 314.159 */

/* shortest 포매팅: 같은 값으로 되돌아가는 최소 문자열. */
char buf[64];
proven_size_t n = 0;
proven_float_format_f64_policy(buf, sizeof buf, 0.1,
    PROVEN_FLOAT_FORMAT_POLICY_RYU,
    proven_float_format_options_shortest(), &n);  /* buf == "0.1" */
```

검증 범위를 과장 없이 그대로 적으면:

- 전수: 유한 `binary32` 값 4,278,190,080개 전체, 호스트 C 라이브러리 대비 0 불일치
  (shortest round-trip + 최소성, 파서 vs `strtod`).
- 대규모: 무작위 `binary64` 값 2,560,000,000개, 0 불일치(이 검증이 실제 포매팅
  결함 1건을 찾아 고쳤습니다 — 문서 참고).
- glibc 2.41 대비 속도(이 머신, x86-64): 일반 숫자 파싱과 shortest 포매팅에서 더 빠름
  (~4-5배). `%f`/`%e`는 정상 크기에서 더 빠르고, 극단 크기에서는 정확한 임의정밀
  연산을 하므로 더 느립니다.

방법론·알고리즘·전체 벤치마크는
[`docs/float-correctness-and-performance.md`](docs/float-correctness-and-performance.md)에 있습니다.

## 플랫폼 경계

portable implementation 파일은 `src/proven/`에 있습니다. OS와 C runtime 호출은 `platform/` 아래에 격리되어 있습니다.

- heap 할당
- filesystem 연산
- time
- environment 접근
- console I/O
- threads
- memory mapping
- 필요한 경우의 math helper

이 분리는 core library를 감사하기 쉽게 만들고, 포팅 작업이 들어갈 위치도 분명하게 해 줍니다. 현재 주요 런타임 대상은 hosted Linux입니다. 빌드는 toolchain이 설치되어 있을 때 선택적 대상에 대해서도 compile-only 점검을 제공합니다. 대상은 Linux AArch64, Linux ARM hard-float, Linux i686, MinGW Windows x86_64/i686, ARM Cortex-M freestanding, RISC-V ELF freestanding입니다.

Cross compilation은 header, source visibility, ABI assumption, target별 compile-time branch가 함께 맞는지 확인하는 용도입니다. 대상 머신에서의 runtime test를 대신하지는 않습니다.

## 주요 모듈

- Foundation: `types`, `error`, `memory`, `align`, `version`, `config`.
- Allocation: `allocator`, `heap`, `arena`, `pool`.
- Buffers and strings: `buffer`, `u8str`, `u16str`.
- Containers: `array`, `list`, `ring`, `map`.
- Algorithms: `algorithm`.
- Text: `fmt`, `scan`.
- Numbers: `float_parse`, `float_format`.
- 해싱과 인코딩: `hash` (FNV-1a, SipHash-2-4, CRC-32, SHA-256), `encode` (hex, Base64, Base64URL).
- 난수: `random` (xoshiro256** 재현 가능, ChaCha20 암호학적, 무편향 범위/셔플 헬퍼, 교체 가능한 엔트로피 소스 — 기본은 OS CSPRNG, 베어메탈은 보드의 하드웨어 TRNG).
- Hosted services: `fs`, `stream`, `time`, `mmap`, `sysio`.
- Execution: `coro`, `job`.
- Diagnostics: `panic`.
- Optional short aliases: `alias_xcv`.

## 이 라이브러리가 아닌 것

`proven`은 libc 대체품도 아니고, garbage collector도 아니고, 프레임워크도 아닙니다. 프로세스, build graph, error policy를 대신 소유하려고 하지도 않습니다. 읽기 쉽고, 테스트하기 쉽고, 한 경계씩 포팅할 수 있도록 만든 C 컴포넌트 모음입니다.

플랫폼 경계가 **어디서 끝나는지**도 적어 둘 가치가 있습니다. 적어 두지 않으면 부딪혀 봐야 알게 되기 때문입니다. PAL이 덮는 범위는 메모리, 파일시스템, 시간, 메모리 매핑, 환경 변수, 콘솔 I/O, 스레드입니다. 프로세스 제어(`fork` / `exec` / 파이프), 터미널 제어(raw 모드, job control), 네트워킹은 **덮지 않습니다**. 프로그램의 본질이 그 중 하나라면 POSIX나 Win32를 직접 부르게 되고, "플랫폼 `#ifdef` 없음"이라는 성질은 거기까지 확장되지 않습니다.

`hash` 모듈은 암호학적·비암호학적 해시(FNV·SipHash·CRC-32와 함께 SHA-256)를, `random`은 OS 강도 바이트를 실제로 제공합니다. 다만 `proven`은 암호 라이브러리가 아닙니다. 의도적인 비목표라서 찾아 헤매지 않으셔도 되는 것은 서명, 키 교환, 비밀번호 해싱/KDF, 인증 암호화(AEAD), TLS이며, 여기에 더해 경로 조작, 인자 파싱, 로깅 프레임워크도 제공하지 않습니다.

## 실제로 프로젝트에 적용했을 때의 효용성

아래는 `proven` 위에서 작은 터미널 텍스트 에디터(`prov`)를 만들며 관찰한 기록입니다. 하나의 사례일 뿐이며, 과장 없이 사실대로 적습니다.

이런 효능이 있었습니다:

- **테스트 용이성.** `proven_allocator_t`를 모든 모듈에 꿰니 에디터 코어 전체를 ASan/UBSan + leak 검출 아래에서 돌릴 수 있었고, 무작위 편집과 모델을 대조하는 검증까지 가능했습니다. 실무적으로 가장 큰 이득이었습니다.
- **에러 누락·문자열 버그 감소.** `[[nodiscard]]`가 붙은 `proven_err_t`는 무시된 에러를 컴파일 단계 신호로 만들고, bounded/owning `u8str`와 타입 기반 포매터는 `snprintf` 포맷/오버플로 버그 클래스를 없앴습니다. 에디터 코어는 `main` 진입점을 빼면 libc-free로 유지됐습니다.
- **이식성.** `fs` / `time` / `mmap` / 터미널 PAL 덕에 한 코드베이스로 Linux x86_64/arm64/armhf와 Windows x64를 빌드했습니다. 파일 브라우저가 에디터 쪽에는 플랫폼 `#ifdef` 하나 없이 `proven_fs_list` / `proven_fs_stat` / `proven_time_breakdown`만으로 양쪽에서 동작했습니다.

이런 점은 감수해야 하고, 기대해서는 안 됩니다:

- **성능 향상은 아닙니다.** libc `memmove`를 `proven_mem_move`로 바꾼 것은 벤치마크상 중립이었습니다. 에디터의 5~50× 편집 속도 향상은 전적으로 자체 자료구조(증분 라인 인덱스, piece 코얼레싱)에서 나왔지 라이브러리에서 나온 것이 아닙니다.
- **벤더링 규율.** 복사만 하고 직접 패치하지 않는 통합 방식이라, 라이브러리 공백은 그 자리에서 고치지 않고 업스트림에 상신해야 합니다. 초기 개발에서 그런 공백 3건(Windows panic 심볼 링크 실패, 고정용량 문자열 생성자 부재, `fs` stat의 owner/group 부재)을 만났고, 다운스트림에서 패치하는 대신 업스트림에서 해결하거나 보류했습니다.
- **전면적 결합.** 할당자와 Result 타입을 도처에 넘기는 것은 의도된 약속입니다. 오래 유지되는 멀티플랫폼 코드베이스에서 값을 하지만, 일회성 코드에는 과합니다.
- **젊은 라이브러리에는 공백이 있습니다.** 필요한 primitive가 없어서 직접 메우거나 상신해야 하는 경우를 종종 만나게 됩니다. 가치는 안전성·테스트 용이성·이식성에 집중돼 있고, 편의성이나 순수 속도에 있지 않습니다.

## 문서

- 사용자 매뉴얼: `manual/manual.md` (`manual/` 아래 챕터들)
- 한국어 매뉴얼: `manual-ko/manual-ko.md`
- freestanding 가이드: `manual/manual-freestanding.md`
- 부동소수점 정확성과 성능: `docs/float-correctness-and-performance.md`
- 사례 연구, 언어 툴체인: `docs/case-study-lowent.md`
- 기본 연산 처리량(해시/인코딩/난수): `docs/primitives-benchmark.md`
- 테스트 매트릭스: `TEST.md`
- 변경 이력: `CHANGELOG.md`
- 기여자 체크리스트: `CHECKLIST.md`

## 상태

주요 검증 대상은 C23 모드의 Linux x86_64 + GCC 또는 Clang입니다. sanitizer, regression, freestanding, cross compile 점검은 `nob.c`가 수행합니다. 선택적 cross target은 해당 toolchain이 설치되어 있을 때 점검합니다. 빌드 드라이버는 먼저 `-std=c23`를 시도하고, 필요하면 `-std=c2x`로 내려갑니다.

Cross compilation은 compile-time coverage만 제공합니다. header, PAL 분리, target-specific branch가 함께 컴파일되는지 확인할 뿐, 각 대상에서의 runtime validation을 대체하지는 않습니다.

Borrowed view는 호출자가 lifetime을 관리해야 합니다. Public struct는 C 사용을 위해 layout을 노출하지만, 호출자가 직접 손상시키면 안 됩니다. 방어적 검사용 validation helper가 따로 있습니다.

Strict pointer provenance는 여기서 완전히 주장하지 않습니다. 이 라이브러리는 문서화된 계약 아래에서 일반적인 hosted-systems C 구현 위의 흔한 undefined-behavior 위험을 피하도록 설계되었습니다.

## 작성자와 라이선스

개발: rubidus-api.
이메일: rubidus@gmail.com
라이선스: MIT License. `LICENSE`를 참조하세요.
