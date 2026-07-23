# Proven Freestanding 모드 (v26.07.23d)

**6부 — 더 나아가기. 선행 조건: 2–5부, 그리고
[6장](manual-06-execution-and-platform-ko.md).**
**이 가이드를 마치면** 운영체제도 libc도 없는 대상용으로 이 라이브러리를 빌드할 수 있고,
그 상황에서 어떤 모듈이 살아남고 어떤 모듈이 살아남지 못하는지 정확히 알게 된다.

이 가이드는 `nob.c`와 공개 헤더가 구현한 현재의 `PROVEN_FREESTANDING` 구성을 설명한다.

**이 가이드의 생김새에 대한 한마디.** 다른 장들과 달리 이 가이드의 대략 절반은 절차다 — 플래그
목록, 파일 목록, 그리고 컴파일 명령 두 개. 그 절들은 의도적으로 짧다: 길게 설명된 컴파일 명령은
아무도 읽지 않는 컴파일 명령이다. *결정*을 담은 절들 — 이 모드가 존재하는 이유(§0), "제외됨"이
정말 무슨 뜻인지(§3), 여러분의 panic 핸들러가 무엇을 해야 하는지(§5), 부동소수점을 뺌으로써
무엇을 잃는지(§8), 호스티드 호출이 왜 링크 타임에 실패하는지(§9), 수명 규칙이 왜 여기서 더
중요한지(§10), 그리고 이 가이드의 주장들이 어떻게 검증되는지(§11) — 은 온전히 풀어 썼다.

## 0. 이 모드가 존재하는 이유, 그리고 라이브러리 전체가 이 제약에 의해 빚어진 이유

Freestanding 모드는 펌웨어, 커널, 부트로더, 하이퍼바이저, 그리고 그 밑에 운영체제가 없는
그 밖의 모든 곳을 위한 것이다 — `malloc`도, `open`도, `printf`도 없고, 흔히 `libc` 자체가 없다.

C 표준에는 이를 가리키는 이름이 있다. **호스티드(hosted)** 구현은 표준 라이브러리 전체를 주고
프로그램을 `main`에서 시작한다. **Freestanding** 구현은 소수의 헤더만을 보장한다 —
`<stddef.h>`, `<stdint.h>`, `<limits.h>`와 몇 가지 더 — 그리고 이것이 베어메탈을 대상으로 하는
컴파일러가 주는 것이다. `-ffreestanding`은 컴파일러에게 정확히 그렇게 가정하라고 알린다.

대부분의 C 라이브러리는 여기서 아예 쓸 수 없으며, 그 이유가 거창한 경우는 드물다. 세 단계 아래
호출에 묻혀 있는 `malloc` 하나, 오류 경로의 `printf` 하나, 또는 결코 실행되지 않는 무언가에
의해 초기화되는 전역 하나다. 호스트에 대한 숨은 의존성 하나면 마이크로컨트롤러에서 라이브러리를
쓸 수 없게 만들기에 충분하다.

**이것이 이 라이브러리의 설계 결정 대부분을 낳은 제약이며**, 그 연결을 보아 둘 가치가 있다.
그렇지 않으면 취향처럼 보이는 선택들을 설명해 주기 때문이다:

| 설계 결정 | 앞선 장에서는 이렇게 읽힌다 | 그리고 진짜 이유는 |
|---|---|---|
| Allocator는 매개변수다 ([2장](manual-02-allocation-ko.md)) | "전략을 갈아 끼울 수 있도록" | 여기에는 `malloc`이 없다. `static` 배열 위의 arena가 유일한 allocator이며, allocator를 매개변수로 받던 코드는 이미 그대로 동작한다. |
| 오류는 반환되는 값이다 ([1장](manual-01-foundation-ko.md)) | "무시할 수 없도록" | 여기에는 `errno`가 없고 풀어낼 스택도 없다. 반환값이 존재하는 유일한 메커니즘이다. |
| Panic은 훅을 거친다 ([1장 §6](manual-01-foundation-ko.md)) | "오버라이드할 수 있도록" | 여기에는 `abort()`도 `stderr`도 없다. 기본 동작이 trap인 것은 이식성 있게 할 수 있는 전부가 그것이기 때문이다. |
| Syscall은 `platform/`에만 산다 | "관심사의 분리" | 새 대상을 위해 교체해야 하는 유일한 디렉터리다. `src/proven/` 안의 모든 것은 구성상 이식 가능하다. |
| C 문자열 대신 뷰 ([3장](manual-03-strings-text-ko.md)) | "길이를 잃어버리지 않도록" | `strlen`은 libc다. 뷰는 자기 길이를 함께 가지고 다니며 아무것도 필요로 하지 않는다. |

달리 말하면: freestanding 지원은 옆에 덧붙인 기능이 아니다. 이 매뉴얼의 나머지는 여기서
살아남도록 지어진 라이브러리를 계속 설명해 온 것이고, 이 가이드는 무엇이 살아남고 무엇이
살아남지 못하는지 알아내는 곳이다.

### 무엇을 포기하는가

운영체제를 필요로 하는 모든 것, 그리고 그 밖에는 아무것도 아니다:

- **힙 없음.** `proven_heap_allocator`는 존재하지 않는다. 여러분이 메모리를 공급하고 —
  보통은 `static` 배열이다 — 그 위에 arena나 pool을 얹는다.
- **파일시스템 없음, 표준 스트림 없음, 클럭 없음, OS 난수 없음.** `fs.h`, `sysio.h`, `mmap.h`,
  `time.h`, 그리고 OS 엔트로피 소스는 모두 호스티드 서비스다. `proven_chacha_rng_t`는 여러분이
  직접 시드를 주면 여전히 동작하며, `proven_random_set_source`가 그것을 위한 것이다.
- **기본적으로 부동소수점 포매팅 없음.** `PROVEN_FMT_NO_FLOAT`가 그것을 뺀다. 부동소수점
  경로는 포매터에서 가장 큰 코드 덩어리이고 대부분의 펌웨어는 `double`을 출력할 일이 없기
  때문이다. 다시 켜는 방법은 8절이 설명한다.
- **UTF-16 문자열 없음.** `PROVEN_NO_U16STR`가 켜져 있다. 여기에는 대화할 Windows API가 없다.

여러분이 유지하는 것은 이식 가능한 코어 전체다: 오류, 뷰, 검사된 산술, 정렬, arena, pool,
버퍼, 문자열, 배열, 맵, 리스트, 링, 정렬, 검색, 해싱, 인코딩, 코루틴, 그리고 정수 포매팅과
스캐닝.

### 여전히 물어뜯는 것

대상이 작아졌다고 해서 수명 규칙이 느슨해지지는 않는다 — 오히려 더 중요해진다. 실수를 알아챌
운영체제도 없고, 여러분이 아직 들고 있는 메모리를 재사용하지 않으려 해 줄 allocator도 없기
때문이다. 10절은 짧고, 사람들이 건너뛰는 절이다.

## 1. 빌드 프로파일

빌드 드라이버는 다음 freestanding 플래그를 사용한다:

```sh
-std=c23
-ffreestanding
-DPROVEN_FREESTANDING
-DPROVEN_FMT_NO_FLOAT
-DPROVEN_NO_U16STR
```

드라이버는 여전히 `-std=c23`을 먼저 탐지하고, 컴파일러가 과도기 표기를 사용하는 경우 `-std=c2x`로 폴백한다. 빌드 래퍼가 두 플래그 표기 중 어느 쪽도 받아들이더라도 freestanding 소스 계약은 C23 우선으로 유지된다.

호스티드 `./nob freestanding` 명령은 또한 로컬 freestanding 테스트 실행 파일을 빌드 호스트에서 정적으로 링크한다. `./nob cross`의 크로스 freestanding 대상은 컴파일 전용 오브젝트 검사를 사용한다.

프로젝트 freestanding 검사를 실행하라:

```sh
cc nob.c -o nob
./nob freestanding -build-root build-out/proven_c_lib
```

크로스 빌드 매트릭스를 실행하라:

```sh
./nob cross -build-root build-out/proven_c_lib
```

## 2. 사용 가능한 소스 파일

현재 freestanding 빌드에서 `nob.c`는 호스티드 전용 모듈을 제외한 이식 가능한 소스 파일을 컴파일한다.

포함된 핵심 모듈:

```text
src/proven/memory.c
src/proven/arena.c
src/proven/pool.c
src/proven/buffer.c
src/proven/heap.c
src/proven/u8str.c
src/proven/array.c
src/proven/ring.c
src/proven/map.c
src/proven/algorithm.c
src/proven/hash.c
src/proven/encode.c
src/proven/random.c
src/proven/float_decimal.c
src/proven/float_parse.c
src/proven/float_format.c
src/proven/time.c
src/proven/fmt.c
src/proven/scan.c
src/proven/panic.c
platform/proven_sys_math.c
```

제외된 호스티드 모듈:

```text
src/proven/u16str.c
src/proven/fs.c
src/proven/stream.c
src/proven/sysio.c
src/proven/mmap.c
src/proven/job.c
platform/proven_sys_fs.c
platform/proven_sys_thread.c
platform/proven_sys_io.c
platform/proven_sys_env.c
platform/proven_sys_time.c
platform/proven_sys_random.c
platform/proven_sys_mem.c
```

대상에 대한 실제 플랫폼 백엔드를 함께 제공하지 않는 한, 제외된 호스티드 모듈을 베어메탈 빌드에 추가하지 말라.

## 3. 모듈 사용 가능성

### 이 표를 읽는 법, 그리고 "제외됨"의 의미

여기서 모듈이 제외되는 이유는 정확히 하나다: 운영체제만이 제공할 수 있는 무언가를 필요로 하기
때문이다. 코드가 작은 대상에서 검증되지 않았다거나 누군가 아직 손을 못 댔다는 뜻이 아니다 —
`fs.h`는 파일시스템이, `sysio.h`는 표준 스트림이, `mmap.h`는 가상 메모리가, `job.h`는
스레드가 필요하다. 이식할 것이 없다.

흥미로운 행은 사용 가능하지도 제외되지도 않은 두 개다:

- **`heap.h`는 스텁이다.** `proven_heap_allocator()`는 여전히 존재하고 여전히 컴파일되며,
  함수 포인터가 널인 allocator를 반환한다 — `proven_alloc_is_valid`가 유효하지 않다고
  보고하는 그것이다. 조용히 다른 어딘가에서 할당하지도 않고 링크에 실패하지도 않는다. 이 선택은
  중요하다: allocator 트레이트를 대상으로 작성된 코드는 베어메탈 대상에서도 계속 컴파일되고,
  깨지는 지점은 힙을 요구한 바로 그 한 줄이며, 런타임에, 여러분이 직접 쓸 수 있는 검사와 함께
  드러난다. 대신 무엇을 넘길지는 4절이 보여 준다.
- **`time.h`는 제한적이다.** datetime 포매팅은 여러분이 공급한 숫자에 대한 순수 산술이므로
  컴파일된다. 없는 것은 그 숫자다: 클럭을 읽는 것은 syscall이므로 여기서 `proven_time_now`에는
  백엔드가 없다. 여러분 자신의 하드웨어 타이머에서 얻은 타임스탬프를 포매팅하라.

"사용 가능"으로 표시된 모든 것은 이식 가능한 코어이며, "사용 가능"이란 호스티드 빌드에서 도는
바로 그 테스트들이 여기에 대해서도 돈다는 뜻이다 — freestanding 프로파일은 이 표에서 주장되는
것이 아니라 릴리스마다 `./nob freestanding`이 빌드하고 검사한다.

| 모듈 | 현재 freestanding 프로파일에서의 상태 | 비고 |
|---|---|---|
| `types.h`, `error.h`, `align.h`, `memory.h` | 사용 가능 | 고정 폭 정수와 `uintptr_t` 지원이 필요하다. |
| `allocator.h` | 사용 가능 | 트레이트만 제공. 호출자가 뒷받침 allocator를 제공한다. |
| `arena.h` | 사용 가능 | 정적 메모리 영역을 위한 주 allocator. |
| `pool.h` | 사용 가능 | 호출자가 제공하는 기반 allocator(흔히 arena)를 사용한다. |
| `buffer.h` | 사용 가능 | 고정 용량 바이트 버퍼. |
| `u8str.h` | 사용 가능 | U8 문자열과 뷰. |
| `u16str.h` | 제외됨 | 현재 프로파일은 `PROVEN_NO_U16STR`를 정의한다. |
| `array.h`, `list.h`, `ring.h`, `map.h` | 사용 가능 | 숨겨진 OS 의존성 없음. |
| `algorithm.h` | 사용 가능 | 배열용 정렬/검색 헬퍼. |
| `hash.h` | 사용 가능 | FNV-1a, SipHash-2-4, CRC-32, SHA-256 — 바이트 단위로 정확, OS 의존성 없음. |
| `encode.h` | 사용 가능 | Hex와 Base64 — 순수 계산, OS 없음. |
| `fmt.h` | 부동소수점 없이 사용 가능 | 현재 프로파일은 `PROVEN_FMT_NO_FLOAT`를 정의한다. |
| `scan.h` | 사용 가능 | 메모리 뷰용 스캐너. |
| `float_parse.h` | 사용 가능 | `proven_strtod`, `proven_parse_double_ascii`, `proven_parse_f64_ascii`가 모두 여기서 컴파일된다. 십진수→binary64 엔진은 정수 연산만 쓰므로 libc가 필요 없다. 유일한 차이는 freestanding 빌드가 오버플로/언더플로에서 `errno`를 설정하지 않는다는 점이며, 대신 반환된 `proven_err_t`가 그 정보를 담는다 — `errno` 자체가 없는 타깃에서 확인해야 할 값이다. 이는 이 프로파일이 실제로 컴파일에서 제외하는 `fmt.h`의 부동소수점 **출력**과는 별개다. |
| `float_format.h` | `fmt.h` 연동 없이 사용 가능 | binary64 포매터는 정수 연산만 쓰므로 컴파일되지만, 현재 프로파일이 `PROVEN_FMT_NO_FLOAT`를 정의하므로 `{}`는 부동소수점을 렌더링하지 않는다. 코드 크기를 감수할 가치가 있다고 판단한 타깃에서 자릿수 출력이 필요하다면 `float_format.h`의 진입점을 직접 호출할 것. |
| `time.h` | 제한적 | 핵심 datetime 포매팅은 컴파일 가능하지만, 실제 PAL 시간은 제외된다. |
| `heap.h` | 스텁 | `proven_heap_allocator()`는 유효하지 않은 allocator를 반환한다. |
| `fs.h`, `stream.h`, `mmap.h`, `sysio.h`, `job.h` | 제외됨 | 호스티드 PAL 서비스가 필요하다. |
| `random.h` | 사용 가능 | 생성기와 헬퍼는 순수 산술이다. `proven_random_bytes`도 여기서 동작하지만, `proven_random_set_source`로 엔트로피 소스 — 보드의 TRNG, 링 오실레이터, 또는 ADC 잡음 바닥 — 를 설치한 후에만 가능하다. 아무것도 설치되지 않았을 때는 클럭으로 시드된 PRNG로 폴백하지 않고 **false**를 반환한다. 폴백은 성공처럼 보이면서 아무것도 보고하지 않는 보안 구멍이 될 것이기 때문이다. 그런 다음 `proven_chacha_rng_seed_from_entropy`가 보드의 엔트로피를 끝없는 암호학적 스트림으로 바꾼다. |
| `coro.h` | 사용 가능 | 매크로 전용 스택리스 코루틴 지원. |
| `panic.h` | 사용 가능 | 대상별 trap/reset 동작을 위한 오버라이드. |

## 4. 최소 정적 arena 설정

힙이 없으므로 메모리는 정적 블록에서 나오고 그 위에 arena를 얹는다. arena는
그 블록을 소유하지 않는다: `alloc`은 오프셋을 증가시키고, `free`는 no-op이며,
`reset`은 전체를 한 번에 되감는다.

이것이 라이브러리 전체가 가능하게 하려고 빚어진 패턴이며, 왜 그것이 통하는지 볼 가치가 있다.
할당할 수 있는 모든 함수는 `proven_allocator_t`를 매개변수로 받으므로, 호스티드 빌드에서
`proven_heap_allocator()`를 대상으로 작성된 코드는 arena를 대신 건네주기만 하면 여기서 그대로
돈다. `u8str.h`, `array.h`, `map.h`의 어떤 것도 메모리가 `malloc`이 아니라 `.bss`의 `static`
배열에서 왔다는 것을 알지도, 상관하지도 않는다.

그 블록의 크기를 정할 때 내려야 할 세 가지 결정이 있고, 어느 것도 라이브러리가 대신 내려 줄 수
없다:

- **얼마나 크게.** arena는 성장할 수 없으므로 그 크기는 여러분이 컴파일 타임에 고르는 단단한
  한계다. 너무 작으면 할당이 `PROVEN_ERR_NOMEM`을 반환하기 시작하고, 너무 크면 펌웨어의 나머지가
  필요로 하던 RAM을 써 버린 것이다. 이것은 몇 달 뒤 힙 단편화로 발견하는 대신 명시적으로 하라고
  요구되는 거래다.
- **언제 reset할지.** arena의 이점 전부는 한 번에 모든 것을 해제하는 데 있다. 자연스러운 지점은
  루프 한 번, 수신한 패킷 하나, 명령 하나 — 작업 묶음에 분명한 끝이 있는 곳이면 어디든이다.
  그 묶음 동안 할당된 모든 것은 reset에서 죽으므로, **그것보다 오래 살아남아도 되는 것은 없다**.
- **뒷받침 배열의 정렬.** 아래처럼 선언에 `alignas(max_align_t)`를 붙인다. 그것이 없으면 배열이
  `double`을 담을 수 없는 주소에서 시작할 수 있고, arena에는 그것을 고칠 수단이 없다.

잘못됨 — reset보다 오래 사는, arena 안에 만들어진 뷰:

```text
proven_u8str_view_t label = build_label(arena_alloc);   /* lives in the arena */
proven_arena_reset(&arena);
send(label);   /* wrong: those bytes are free space now, and the next allocation takes them */
```

호스티드 빌드에서는 이것이 테스트에서 괜찮아 보일 만큼은 자주 살아남는다. 여기서는 다음 할당이
곧바로 같은 바이트를 가져가므로 그렇지 않다.

```c
#include "proven/types.h"
#include "proven/memory.h"
#include "proven/arena.h"
#include "proven/array.h"
#include "proven/panic.h"

/* static storage: no OS, no heap, known at link time */
static alignas(PROVEN_MAX_ALIGN) proven_byte_t storage[4096];

proven_arena_t arena = proven_arena_create((proven_mem_mut_t){
    .ptr = storage,
    .size = sizeof storage,
});
proven_allocator_t arena_alloc = proven_arena_as_allocator(&arena);

proven_result_array_t r = PROVEN_ARRAY_INIT(arena_alloc, proven_i32, 16);
if (proven_is_ok(r.err)) {
    proven_array_t values = r.value;
    proven_err_t e = PROVEN_ARRAY_PUSH(&values, proven_i32, 42);
    (void)e;
    PROVEN_ARRAY_DESTROY(&values); /* arena free is a no-op */
}

proven_arena_destroy(&arena);      /* also a no-op: the caller owns `storage` */
```

흔한 실수:

```text
proven_allocator_t heap = proven_heap_allocator();
heap.alloc_fn(heap.ctx, 64, 8); /* wrong: heap is invalid in PROVEN_FREESTANDING */
```

올바름:

```c
proven_allocator_t heap = proven_heap_allocator();
if (!proven_alloc_is_valid(heap)) {
    /* use an arena, pool, or target-provided allocator instead */
    proven_panic("no heap on this target");
}
```

## 5. Panic 핸들러 오버라이드

### 여기서는 왜 핸들러 설치가 선택 사항이 아닌가

호스티드 시스템에서 trap하는 panic은 견딜 만하다: 프로세스가 죽고, 운영체제가 뒷정리를 하고,
무언가가 그것을 다시 띄운다. 베어메탈에는 그 밑에 아무것도 없다. trap은 코어를 멈추고, 장치가
하던 일이 무엇이든 — 모터를 어떤 속도로 유지하는 것, 무선 링크를 유지하는 것, 히터를 구동하는
것 — CPU가 멈춘 뒤에도 여전히 그러고 있다.

그래서 기본 핸들러는 여러분이 반드시 작성해야 할 핸들러의 자리 표시자이며, 여러분의 핸들러가
무엇을 하는지는 프로그래밍 결정이라기보다 제품 결정이다. 흔한 형태들:

- **하드웨어를 안전 상태로 만든 다음 멈춘다.** 모터 끄기, 출력을 알려진 수준으로, 그다음 회전
  대기하거나 디버거를 기다린다. 물리적 액추에이터가 있는 것이라면 무엇에든 올바르다.
- **기록하고 리셋한다.** 리셋을 넘어 살아남는 레지스터나 예약된 RAM 영역에 사유 코드를 쓴 다음,
  워치독을 발동시킨다. 다음 부팅이 지난 부팅이 왜 끝났는지 보고한다.
- **요란하게 멈춘다.** 알아볼 수 있는 패턴으로 LED를 깜빡인다. 콘솔이 없는 보드에서는 이것이
  진단 채널의 전부이며, 들리는 것보다 값어치가 크다.

[1장 §6](manual-01-foundation-ko.md)의 규칙이 여기서는 더 강하게 적용된다: **핸들러는 반환해서는
안 된다.** 반환하면 `proven_arena_alloc_or_panic`은 애초에 할당되지 않은 블록을 가지고 진행하고,
실패는 "장치가 멈췄다"에서 "장치가 널 포인터를 통해 쓰고 있다"로 옮겨 간다.

잘못됨 — 로그를 남기고 반환하는 핸들러:

```text
static void my_panic(const char *msg) {
    uart_write(msg);   /* wrong: this returns, and the caller uses a block it never got */
}
```

`proven_arena_alloc_or_panic()`과 관련 panic 경로는 `proven_panic()`을 호출하며, 이는 `proven_set_panic_handler()`로 설치된 핸들러로 디스패치한다. 기본 핸들러는 trap한다.

핸들러는 함수 전체이므로, 이것은 조각이 아니라 리스팅이다:

```text
#include "proven/panic.h"

static void my_panic(const char *msg) {
(void)msg;
/* optional: write msg to UART or crash log */
for (;;) {
/* or reset the MCU */
}
}

/* install once during startup; pass NULL to restore the default */
proven_set_panic_handler(my_panic);
```

프로덕션 panic 핸들러는 반환해서는 안 된다. 반환하면 panic을 유발한 할당 결과가 유효하다고 보장되지 않는다.

## 6. Cortex-M 컴파일 명령 예제

```sh
arm-none-eabi-gcc \
-std=c23 \
-mcpu=cortex-m4 -mthumb \
-ffreestanding -nostdlib \
-DPROVEN_FREESTANDING \
-DPROVEN_FMT_NO_FLOAT \
-DPROVEN_NO_U16STR \
-Iinclude -Iplatform \
-c src/proven/memory.c -o memory.o
```

실제 펌웨어 빌드는 위에 나열된 포함 freestanding 모듈을 컴파일하고, 대상 startup 코드를 컴파일하고, 여러분의 linker script로 링크해야 한다.

## 7. RISC-V ELF 컴파일 명령 예제

```sh
riscv64-elf-gcc \
-std=c23 \
-ffreestanding -nostdlib \
-DPROVEN_FREESTANDING \
-DPROVEN_FMT_NO_FLOAT \
-DPROVEN_NO_U16STR \
-Iinclude -Iplatform \
-c src/proven/arena.c -o arena.o
```

여러분의 툴체인이 `riscv64-unknown-elf-gcc` 이름을 사용하면 그 컴파일러를 대신 사용하라. 프로젝트 크로스 매트릭스는 사용 가능한 경우 두 이름을 모두 검사한다.

## 8. Freestanding 모드에서의 포매팅

### 무엇이 남는가, 그리고 왜 대개 그것으로 충분한가

포매터는 이식 가능한 계산이다 — 여러분이 공급한 목적지에 바이트를 만들어 넣는다 — 그래서 거의
전부가 여기서 살아남는다. 사라지는 것은 자리 표시자 타입 하나: `double`이다.

큰 손실처럼 들리지만 대개 그렇지 않다. 올바른 부동소수점 포매팅이란 서브노멀을 포함한 모든
입력에 대해 같은 값으로 되읽히는 가장 짧은 십진수를 내보내는 것을 뜻하고, 그러려면 큰 정수 산술과
룩업 테이블이 필요하다. 그것이 포매터에서 단일로 가장 큰 코드 덩어리다. 대부분의 펌웨어는 정수,
문자열 뷰, 문자, 포인터를 포매팅한다 — 센서 읽은 값은 스케일된 정수이고, 상태 줄은 텍스트다 —
그래서 이 프로파일은 기본적으로 부동소수점 경로를 빼고, 그 덕분에 바이너리가 더 작다.

남는 것: 모든 폭의 정수, 뷰, 문자, 불리언, 포인터에 대한 `{}`. 스펙 문법 전체(폭, 채움, 정렬,
진법, 부호). 여러분 자신의 타입을 위한 `PROVEN_ARG_OF`. 그리고 스캐너 전체 — 같은 무게를 지지
않는 부동소수점 *파싱*까지 포함해서.

작은 대상에서 정말로 부동소수점이 필요하다면 스위치는 `PROVEN_FMT_NO_FLOAT`이고, 그와 함께 가는
용량 조절 손잡이는 §8a가 다룬다. 결정하기 전에 크기 변화를 측정하라 — 플래시가 32 KB인 부품에서
그것은 반올림 오차가 아니다.

다른 귀결에도 주의하라: **여기에는 `proven_println`이 없다.** 포매팅은 `proven_u8str_t`나 버퍼에
덧붙이며, 그 바이트를 UART나 RTT 채널로 보내는 것은 여러분의 플랫폼 코드다. freestanding 빌드에는
가정할 수 있는 표준 출력이 없기 때문이다.

부동소수점 포매팅은 `PROVEN_FMT_NO_FLOAT`에 의해 비활성화된다. 정수와 문자열 뷰 포매팅은 계속 사용 가능하다.

올바름(여기서 `alloc`은 4절에서처럼 arena allocator이다 — 힙이 없다):

```c
proven_result_u8str_t r = proven_u8str_create(alloc, 32);
if (proven_is_ok(r.err)) {
    proven_fmt_result_t f = proven_u8str_append_fmt_grow(alloc, &r.value,
                                                         "value={}", PROVEN_ARG(123));
    (void)f;
    proven_u8str_destroy(alloc, &r.value);
}
```

잘못됨:

```text
proven_u8str_append_fmt_grow(alloc, &s, "{}", PROVEN_ARG(3.14));
/* wrong in the current freestanding profile: float args are excluded, so
   PROVEN_ARG has no _Generic association for double and this fails to compile */
```

## 8a. Float 큰 정수 용량 조정

정확한 십진수-to-binary64 폴백과 큰 정수 나눗셈 헬퍼는 용량이
`PROVEN_FLOAT_BIGINT_LIMBS`(기본값 160, `proven/float_config.h`에 있음)로 설정되는
고정 크기 큰 정수를 사용한다. 이것이 그들의 스택 사용량을 지배하는 요인이다: 각 큰
정수는 `8 * PROVEN_FLOAT_BIGINT_LIMBS` 바이트이며, 나눗셈은 여러 개에 더해 32비트
스크래치를 사용한다.

스택이 빠듯한 대상은 이를 낮출 수 있다. 예를 들면:

```sh
-DPROVEN_FLOAT_BIGINT_LIMBS=48
```

유지되는 가수 상한(`PROVEN_FLOAT_MAX_SIGNIFICAND_DIGITS`)은 이 용량에서 파생되므로,
더 작은 값이 절대 오버플로하지 않는다: 파서는 파생된 유효 자릿수까지의 입력에 대해
여전히 올바르게 반올림하고, 더 긴 입력에 대해서는 1 ULP 이내에 머문다. Clinger와
Eisel-Lemire 빠른 경로는 큰 정수를 사용하지 않으므로, 일반적인 짧은 입력은 영향을
받지 않는다. binary64 반올림 경계는 최대 767개의 유효 자릿수를 필요로 할 수 있으므로,
병적으로 긴 십진수의 정확한 반올림이 중요할 때는 기본값(160)을 유지하라.

## 9. 호스티드 API 피하기

### 왜 이것이 런타임의 뜻밖이 아니라 링크 오류인가

호스티드 모듈은 freestanding 빌드에 아예 컴파일되어 들어가지 않는다. 따라서 그중 하나를
호출하면 **링크 오류**가 된다 — 빌드 타임에, 쓰지 말았어야 할 함수의 이름을 대는 미정의 심벌로.

그것이 설계가 작동하는 모습이다. 많은 임베디드 라이브러리가 택하는 대안은 런타임에 오류를
반환하는 스텁을 제공하는 것이다. 그러면 마이크로컨트롤러에서 `proven_fs_open` 호출이
컴파일되고, 링크되고, 출하되고, 아무도 밟아 보지 않은 경로에서 현장에서 실패한다. 여기서는
빌드 밖으로 나갈 수가 없다.

여러분 자신의 코드에 대한 실질적인 귀결: 어떤 모듈이 호스티드 도구와 펌웨어 양쪽에 공유된다면,
호스티드 전용 호출은 `#ifndef PROVEN_FREESTANDING` 뒤에 두어야 하고, 링크 오류가 어떤 것을
놓쳤는지 정확히 알려 준다.

현재 freestanding 프로파일에서 다음을 호출하지 말라:

```text
proven_fs_open(...);
proven_println(...);
proven_env_get(...);
proven_mmap_create(...);
proven_job_system_init(...);
```

이들은 의도적으로 제외된 호스티드 PAL 파일을 필요로 한다.

## 10. 수명 규칙은 여전히 적용된다

### 사람들이 건너뛰는 절, 그리고 여기서 그 대가가 더 큰 이유

Freestanding는
[0장 §5](manual-00-start-here-ko.md#5-모든-페이지에서-만나게-될-다섯-가지-계약)의 소유권
규칙을 하나도 완화하지 않는다. 오히려 날을 세운다. 모두 같은 방향을 가리키는 세 가지 이유가
있다:

- **여러분과 메모리 사이에 allocator가 없다.** 힙 블록을 가리키는 댕글링 포인터는 allocator가
  그 블록을 아직 다시 내주지 않았다는 이유로 한동안 살아남는 일이 흔하다. arena는 reset 직후
  바로 다음 할당에서 같은 바이트를 내주므로, 낡은 뷰는 즉시, 그리고 결정론적으로 덮어써진다.
- **여러분을 붙잡아 줄 것이 없다.** 작은 대상에는 MMU 트랩도 없고, 프로세스를 죽여 줄
  운영체제도 없고, 현장에는 새니타이저도 없다. use-after-reset은 크래시하지 않는다 — 남의
  데이터를 읽고 그냥 계속 가며, 증상은 전혀 관계없는 곳에서 나타난다.
- **결과가 물리적이다.** 여기서 잘못된 바이트 하나는 모터 명령이거나 무선 패킷일 수 있다.

규칙은 여러분이 이미 아는 그것들이다: 뷰는 소유자가 살아 있는 동안에만 유효하고, 소유된 객체는
그것을 만든 allocator로 정확히 한 번 파괴되며, 호출자 소유 상태 구조체는 복사해서는 안 된다.
달라지는 것은 오차 허용 범위이고, 그것은 0이다.

Freestanding는 컨테이너 규칙을 완화하지 않는다.

잘못됨:

```text
int *p = PROVEN_ARRAY_GET_MUT(&arr, int, 0);
PROVEN_ARRAY_PUSH(&arr, int, 9);
*p = 1; /* wrong: push may have moved the array */
```

잘못됨:

```text
proven_u8str_view_t key = make_stack_view();
PROVEN_MAP_SET_U8_BORROWED(&map, key, int, 1);
return; /* wrong if map survives after key bytes go out of scope */
```

## 11. 검증

### 왜 이 가이드는 믿는 것이 아니라 검사되는가

위의 모든 것은 운영체제 없이 무엇이 컴파일되고 링크되는지에 대한 주장이며, 그런 주장은 조용히
썩는다. 관계없는 커밋에서 이식 가능한 소스 파일에 `#include <stdio.h>` 하나가 추가되면
freestanding 프로파일은 깨진다 — 호스트 빌드에서는 아무도 알아채지 못한다. `stdio.h`는 바로
거기에 있으니까.

그래서 이 프로파일은 서술되는 대신 릴리스마다 빌드된다. `./nob freestanding`은 이식 가능한 코어를
`-ffreestanding`과 이 프로파일의 정의들로 컴파일하고, 아래의 검사들을 빌드 호스트에서
**정적으로** 링크한 뒤 실행한다. 그런 다음 `./nob cross`가 같은 프로파일을 실제 임베디드 대상 —
그중에도 Cortex-M과 RISC-V — 에 대해 컴파일 전용 매트릭스로 컴파일한다.

두 검사는 무엇을 잡아낼지를 기준으로 골랐다:

- **heap 스텁** 검사는 `proven_heap_allocator()`가 여전히 존재하고 `proven_alloc_is_valid`가
  거부하는 무언가를 반환함을 증명한다. 누군가 그것을 대신 링크 실패로 만들었다면 트레이트 기반
  코드가 베어메탈용으로 컴파일되지 않게 된다 — §3이 설명하는, 이 설계가 피하는 그 실패다.
- **컴파일 검사**는 이 프로파일에 대해 대표적인 프로그램을 빌드하며, 이것이 호스티드 헤더가 이식
  가능한 파일에 몰래 들어오는 것을 잡아낸다.

어느 쪽도 실제 하드웨어에서 돌지 않으며, 이 가이드는 그렇다고 주장하지 않는다: 정렬 폴트,
엔디안, 타이밍에는 보드가 필요하다. 검증되는 것은 검증될 수 있는 부분이고, 그것이 모든 빌드에서
돈다. 전부를 아무 빌드에서도 하지 않는 대신에.

프로젝트 freestanding 명령은 빌드 호스트에서 다음 로컬 검사를 빌드하고 실행한다:

```text
tests/test_portability_freestanding_heap_stub
tests/test_portability_compile_freestanding
tests/test_portability_compile_nofloat
tests/test_portability_compile_nou16str
tests/test_portability_freestanding
```

크로스 명령은 사용 가능한 임베디드 컴파일러에 대해 컴파일 전용 검사를 수행한다:

```text
freestanding-arm-cortex-m4        arm-none-eabi-gcc -mcpu=cortex-m4 -mthumb
freestanding-riscv64-elf          riscv64-elf-gcc
freestanding-riscv64-unknown-elf  riscv64-unknown-elf-gcc
```

없는 툴체인은 건너뛴다. 실제 컴파일 실패는 명령을 실패시킨다. 런타임 동작은 여전히 대상 또는 에뮬레이터에서의 검증이 필요하다.
