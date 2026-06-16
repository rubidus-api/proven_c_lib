# Proven C library

This README is bilingual. The English section comes first, then the Korean translation follows with the same substantive content.

`proven` is a small C23 systems library for code that should stay readable over time. It gives you the everyday pieces C projects usually end up rewriting: explicit allocators, owned and borrowed strings, dynamic arrays, maps with borrowed or owned string keys, formatting and scanning, filesystem helpers, memory mapping, time, stackless coroutines, and a bounded job system.

The point is not to hide C behind a framework. The point is to make practical C less repetitive while still keeping ownership, errors, allocator choice, and platform boundaries visible.

The build driver probes `-std=c23` first and falls back to `-std=c2x` when the compiler still uses the transitional spelling, so older GCC and Clang front ends can still build the tree without changing the library's C23 baseline.

- Version: proven_c_lib-v26.05.19u
- Standard: C23
- License: MIT
- Repository: https://github.com/rubidus-api/proven_c_lib

## why people reach for it

- Ownership is explicit, so it is easier to see who allocates and who frees.
- Fallible APIs return results instead of silently hiding failure.
- Reallocation-style operations are designed to stay failure-atomic where documented.
- Borrowed views are clearly separated from owning objects.
- Decimal/float conversion is correctly rounded (bit-for-bit equal to the host `strtod`/`snprintf`) and has been checked exhaustively for `binary32`; on typical data it is faster than glibc.
- Hosted OS access is isolated behind the PAL layer in `platform/`.
- Freestanding builds can use the reduced core without pulling in hosted filesystem, console, thread, mmap, or environment services.
- The build system is a single C file, `nob.c`, so there is no mandatory CMake, Meson, npm, or external test framework.

That makes the library useful for command-line tools, embedded-adjacent code, experiment code that may later need a stricter platform boundary, and C projects that want a compact support layer without giving up control.

## quick start

Build the driver and run the default hosted test/build path:

```sh
cc nob.c -o nob
./nob build
```

Use a different compiler if needed:

```sh
./nob strict-error -cc clang
```

Common checks:

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

Running `./nob` without arguments prints the full command list.

## a small example

This example creates an owned UTF-8 string, appends to it with the formatter, prints it, and then destroys it with the same allocator family.

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

Build it against the hosted sources:

```sh
cc -std=c23 -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L \
  -Iinclude -Iplatform \
  app.c src/proven/*.c platform/proven_sys_*.c \
  -pthread -o app
```

## containers without hidden ownership

`proven_array_t` is a growable contiguous array. It stores the allocator trait used for growth and destruction, so the call site stays honest about memory ownership.

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

The same pattern appears across strings, maps, buffers, arenas, pools, and filesystem helpers: create with an allocator, check the result, keep borrowed pointers short-lived, and destroy owned objects deliberately.

## text formatting with bounded input

`PROVEN_ARG("literal")` is convenient for trusted NUL-terminated strings. For text from outside your program, prefer bounded views or bounded C-string arguments so formatting does not search past the bytes you meant to expose.

```c
const char packet_text[5] = { 'o', 'k', '!', '!', 'x' };
proven_println("rx: {}", PROVEN_ARG_CSTR_N(packet_text, 4));
```

The formatter has three useful modes for owned strings:

- `proven_u8str_append_fmt`: fixed-capacity and atomic.
- `proven_u8str_append_fmt_trunc`: fixed-capacity and truncating.
- `proven_u8str_append_fmt_grow`: allocator-backed and atomic.

## correct, fast number conversion

Decimal-to-`double` parsing and `double`/`float`-to-decimal formatting are
correctly rounded (round-to-nearest, ties to even) and produced by an
integer-only engine — no `long double`. The parser is bit-for-bit identical to
the host `strtod`; fixed `%f`/`%e` output matches the host `snprintf`; and a
shortest mode emits the minimal round-trippable string.

```c
#include "proven/scan.h"
#include "proven/float_format.h"

/* Parse: correctly rounded, NUL terminator not required (length-bounded view). */
proven_scan_t sc = proven_scan_init(proven_u8str_view_from_cstr("3.14159e2"));
double v = proven_scan_f64(&sc).val;            /* 314.159 */

/* Format shortest: the minimal string that round-trips back to the same value. */
char buf[64];
proven_size_t n = 0;
proven_float_format_f64_policy(buf, sizeof buf, 0.1,
    PROVEN_FLOAT_FORMAT_POLICY_RYU,
    proven_float_format_options_shortest(), &n);  /* buf == "0.1" */
```

How far this is checked, stated plainly:

- Exhaustive: all 4,278,190,080 finite `binary32` values, zero mismatches against
  the host C library (shortest round-trip + minimality, parser vs `strtod`).
- Large-scale: 2,560,000,000 random `binary64` values, zero mismatches (this sweep
  found and fixed one real formatting defect — see the doc).
- Speed vs glibc 2.41 on this machine (x86-64): faster at parsing typical numbers
  and at shortest formatting (~4-5x); `%f`/`%e` are faster at normal magnitudes and
  slower at extreme magnitudes, where the engine does exact arbitrary-precision work.

Methodology, algorithms, and the full benchmark are in
[`docs/float-correctness-and-performance.md`](docs/float-correctness-and-performance.md).

## platform boundary

Portable implementation files live in `src/proven/`. OS and C runtime calls are isolated under `platform/`:

- heap allocation
- filesystem operations
- time
- environment access
- console I/O
- threads
- memory mapping
- math helpers where needed

This split keeps the core library easier to audit and gives ports a clear place to work. Hosted Linux is the primary runtime target today. The build also has compile-only checks for optional targets when the toolchains are installed: Linux AArch64, Linux ARM hard-float, Linux i686, MinGW Windows x86_64/i686, ARM Cortex-M freestanding, and RISC-V ELF freestanding.

Cross compilation shows that headers, source visibility, ABI assumptions, and compile-time platform branches line up. It does not replace runtime tests on the target machine.

## main modules

- Foundation: `types`, `error`, `memory`, `align`.
- Allocation: `allocator`, `heap`, `arena`, `pool`.
- Buffers and strings: `buffer`, `u8str`, `u16str`.
- Containers: `array`, `list`, `ring`, `map`.
- Algorithms: `algorithm`.
- Text: `fmt`, `scan`.
- Numbers: `float_parse`, `float_format`.
- Hosted services: `fs`, `time`, `mmap`, `sysio`.
- Execution: `coro`, `job`.
- Diagnostics: `panic`.
- Optional short aliases: `alias_xcv`.

## what it is not

`proven` is not a libc replacement, a garbage collector, or a framework. It does not try to own your process, your build graph, or your error policy. It is a set of C components that are meant to be easy to read, easy to test, and possible to port one boundary at a time.

## documentation

- User manual: `manual/manual.md` (chapters under `manual/`)
- Freestanding guide: `manual/manual-freestanding.md`
- Float correctness and performance: `docs/float-correctness-and-performance.md`
- Test matrix: `TEST.md`
- Changelog: `CHANGELOG.md`
- Contributor checklist: `CHECKLIST.md`

## status

The primary verified target is Linux x86_64 with GCC or Clang in C23 mode. Sanitizer, regression, freestanding, and cross compile checks are driven by `nob.c`. Optional cross targets are checked when the corresponding toolchains are present. The build driver probes `-std=c23` first and falls back to `-std=c2x` when needed.

Cross compilation is compile-time coverage only. It checks that the headers, PAL splits, and target-specific branches compile together; it does not replace runtime validation on each target.

Borrowed views require caller-managed lifetime. Public structs expose layout for C use, but callers should not manually corrupt them; validation helpers exist for defensive checks.

Strict pointer provenance is not fully claimed here. The library is designed to avoid common C undefined-behavior hazards under documented contracts on conventional hosted-systems C implementations.

## author and license

Developed by rubidus-api.
Email: rubidus@gmail.com
License: MIT License. See `LICENSE`.

---

# Proven C 라이브러리

이 README는 이중 언어로 작성되어 있습니다. 먼저 영어 본문을 두고, 그 아래에 같은 내용의 한국어 번역을 배치했습니다.

`proven`은 시간이 지나도 읽기 쉬운 코드를 목표로 한 작은 C23 시스템 라이브러리입니다. C 프로젝트가 자주 직접 다시 구현하게 되는 요소들, 즉 명시적 allocator, owned/borrowed 문자열, 동적 배열, borrowed 또는 owned 문자열 키를 쓰는 map, 형식화와 파싱, 파일시스템 헬퍼, 메모리 매핑, 시간, 스택 없는 코루틴, bounded job system을 제공합니다.

의도는 C를 프레임워크 뒤에 숨기는 것이 아닙니다. 실용적인 C를 덜 반복적으로 만들면서도 ownership, error, allocator 선택, platform boundary를 그대로 보이게 두는 데 있습니다.

빌드 드라이버는 먼저 `-std=c23`를 시도하고, 컴파일러가 아직 transitional spelling만 받아들이는 경우 `-std=c2x`로 내려갑니다. 그래서 기존 GCC와 Clang 프런트엔드도 라이브러리의 C23 기준을 바꾸지 않고 트리를 빌드할 수 있습니다.

- 버전: proven_c_lib-v26.05.19u
- 표준: C23
- 라이선스: MIT
- 저장소: https://github.com/rubidus-api/proven_c_lib

## 사람들이 이 라이브러리를 찾는 이유

- ownership가 명시적이라 누가 할당하고 누가 해제하는지 보기가 쉽습니다.
- 실패 가능한 API는 실패를 조용히 숨기지 않고 result를 반환합니다.
- reallocation 계열 연산은 문서에 적힌 범위에서는 failure-atomic하게 동작하도록 설계되었습니다.
- borrowed view와 owning 객체가 분명히 구분됩니다.
- 십진수/부동소수점 변환이 정확히 반올림되며(호스트 `strtod`/`snprintf`와 비트 단위 동일), `binary32`는 전수 검증되었습니다. 일반적인 데이터에서는 glibc보다 빠릅니다.
- hosted OS 접근은 `platform/` 안의 PAL 계층 뒤로 격리되어 있습니다.
- freestanding 빌드는 hosted filesystem, console, thread, mmap, environment 서비스를 끌어오지 않고 축소된 핵심만 사용할 수 있습니다.
- 빌드 시스템은 단일 C 파일 `nob.c`라서 CMake, Meson, npm, 외부 테스트 프레임워크가 필수가 아닙니다.

이 때문에 이 라이브러리는 명령행 도구, 임베디드에 가까운 코드, 나중에 더 엄격한 platform boundary가 필요할 수 있는 실험 코드, 그리고 제어권을 잃지 않으면서도 작고 단단한 지원 계층을 원하는 C 프로젝트에 잘 맞습니다.

## 빠른 시작

드라이버를 빌드하고 기본 hosted 빌드/테스트 경로를 실행합니다.

```sh
cc nob.c -o nob
./nob build
```

필요하면 다른 컴파일러를 사용할 수 있습니다.

```sh
./nob strict-error -cc clang
```

자주 쓰는 점검 명령:

```sh
./nob release
./nob asan
./nob ubsan
./nob tsan
./nob regression
./nob freestanding
./nob cross -build-root /home/user/work/build/proven_c_lib
```

인자 없이 `./nob`를 실행하면 전체 명령 목록이 출력됩니다.

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

## platform boundary

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

- Foundation: `types`, `error`, `memory`, `align`.
- Allocation: `allocator`, `heap`, `arena`, `pool`.
- Buffers and strings: `buffer`, `u8str`, `u16str`.
- Containers: `array`, `list`, `ring`, `map`.
- Algorithms: `algorithm`.
- Text: `fmt`, `scan`.
- Numbers: `float_parse`, `float_format`.
- Hosted services: `fs`, `time`, `mmap`, `sysio`.
- Execution: `coro`, `job`.
- Diagnostics: `panic`.
- Optional short aliases: `alias_xcv`.

## 이 라이브러리가 아닌 것

`proven`은 libc 대체품도 아니고, garbage collector도 아니고, 프레임워크도 아닙니다. 프로세스, build graph, error policy를 대신 소유하려고 하지도 않습니다. 읽기 쉽고, 테스트하기 쉽고, 한 경계씩 포팅할 수 있도록 만든 C 컴포넌트 모음입니다.

## 문서

- 사용자 매뉴얼: `manual/manual.md` (`manual/` 아래 챕터들)
- freestanding 가이드: `manual/manual-freestanding.md`
- 부동소수점 정확성과 성능: `docs/float-correctness-and-performance.md`
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
