# Proven C library

This README is bilingual. The English section comes first, then the Korean translation follows with the same substantive content.

`proven` is a small C23 systems library for code that should stay readable over time. It gives you the everyday pieces C projects usually end up rewriting: explicit allocators, owned and borrowed strings, dynamic arrays, maps with borrowed or owned string keys (HashDoS-resistant by default), formatting and scanning, buffered streams and line input from stdin, filesystem helpers, memory mapping, time, hashing (FNV, SipHash, CRC-32, SHA-256), randomness by use case (a reproducible generator, a cryptographic one, and the OS CSPRNG), stackless coroutines, and a bounded job system.

The point is not to hide C behind a framework. The point is to make practical C less repetitive while still keeping ownership, errors, allocator choice, and platform boundaries visible.

The build driver probes `-std=c23` first and falls back to `-std=c2x` when the compiler still uses the transitional spelling, so older GCC and Clang front ends can still build the tree without changing the library's C23 baseline.

- Version: proven_c_lib-v26.07.13g
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

## whole files, and sorting that cannot be made quadratic

Reading a file whole is the operation most programs actually want, so it is one call:

```c
proven_result_u8str_t src = proven_fs_read_all_u8str(alloc, PROVEN_LIT("main.c"));
if (!proven_is_ok(src.err)) return src.err;
proven_u8str_view_t text = proven_u8str_as_view(&src.value);   /* NUL-terminated, no second copy */
proven_u8str_destroy(alloc, &src.value);
```

`proven_fs_read_all` / `proven_fs_read_all_u8str` read to EOF rather than to a pre-measured size. The file's reported size only seeds the capacity, so a regular file still costs one allocation and one pass - but a source whose size cannot be known up front (a pipe, a FIFO, a `/proc` entry) is read correctly rather than coming back empty, and a file that grows mid-read is not silently truncated.

Writing is symmetric. `proven_fs_write_file` creates or truncates; `proven_fs_write_file_atomic` writes a sibling temp file and renames it over the target, so a concurrent reader sees either the whole old file or the whole new one, and the target's permissions are preserved (a `0600` file is not republished as `0644`). It is atomic with respect to readers; durability across power loss is a separate, explicit ask - `proven_fs_sync` (fsync) and `proven_fs_write_file_durable`, which does the three steps in the only order that works: sync the temp file, rename, then sync the directory.

`proven_array_sort` is an introsort, and two of its properties are worth stating because they are the ones that bite:

- **O(n log n) is a guarantee, not a typical case.** A heapsort fallback past a bounded recursion depth is what makes it one. A sort whose worst case can be reached by an adversarial ordering is a denial of service in any program that sorts data it did not author.
- **Duplicate keys are the fast case.** Elements equal to the pivot are collected into a run that is final and never recursed into, so all-equal input costs a single pass. Low-cardinality keys - a status column, an enum, a bucket id - are what callers actually sort by, and they are exactly what a naive partition degrades on.

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

- Foundation: `types`, `error`, `memory`, `align`, `version`, `config`.
- Allocation: `allocator`, `heap`, `arena`, `pool`.
- Buffers and strings: `buffer`, `u8str`, `u16str`.
- Containers: `array`, `list`, `ring`, `map`.
- Algorithms: `algorithm`.
- Text: `fmt`, `scan`.
- Numbers: `float_parse`, `float_format`.
- Hashing and randomness: `hash` (FNV-1a, SipHash-2-4, CRC-32, SHA-256), `random` (xoshiro256** reproducible, ChaCha20 cryptographic, the OS CSPRNG, and unbiased range/shuffle helpers).
- Hosted services: `fs`, `stream`, `time`, `mmap`, `sysio`.
- Execution: `coro`, `job`.
- Diagnostics: `panic`.
- Optional short aliases: `alias_xcv`.

## what it is not

`proven` is not a libc replacement, a garbage collector, or a framework. It does not try to own your process, your build graph, or your error policy. It is a set of C components that are meant to be easy to read, easy to test, and possible to port one boundary at a time.

It is also worth saying where the platform boundary **stops**, because otherwise you find out by running into it. The PAL covers memory, the filesystem, time, memory mapping, environment variables, console I/O and threads. It does **not** cover process control (`fork` / `exec` / pipes), terminal control (raw mode, job control), or networking — a program whose substance is one of those will reach for POSIX or Win32 directly, and the "no platform `#ifdef`s" property does not extend to it.

The `hash` module does provide cryptographic and non-cryptographic hashes (SHA-256 alongside FNV, SipHash, and CRC-32) and `random` provides OS-strength bytes, but `proven` is not a cryptography library: deliberate non-goals, so you do not go looking, are signatures, key exchange, password hashing / KDFs, authenticated encryption, and TLS — along with path manipulation, argument parsing, and a logging framework.

## utility in a real project

These notes come from building a small terminal text editor (`prov`) on top of `proven`. They are one data point, recorded plainly rather than as a sales pitch.

What it bought:

- **Testability.** Threading `proven_allocator_t` through every module let the whole editor core run under ASan/UBSan and leak detection, including randomized edit-vs-model cross-checks. This was the largest practical gain.
- **Fewer unchecked-error and string bugs.** `proven_err_t` with `[[nodiscard]]` makes an ignored error a compile-time signal; bounded/owning `u8str` and the typed formatter removed the `snprintf` format/overflow bug class. The editor core stayed libc-free apart from its `main` entry point.
- **Portability.** The `fs` / `time` / `mmap` / terminal PAL let one codebase build for Linux x86_64/arm64/armhf and Windows x64; a file browser worked on both through `proven_fs_list` / `proven_fs_stat` / `proven_time_breakdown` with no platform `#ifdef`s in the editor.

What you accept, and should not expect:

- **It is not a performance win.** Replacing libc `memmove` with `proven_mem_move` was benchmark-neutral; the editor's 5–50× edit speedups came entirely from its own data structures (incremental line index, piece coalescing), not from the library.
- **Vendoring discipline.** A copy-only, do-not-patch integration means library gaps are filed upstream rather than fixed in place. Early editor work hit three such gaps (a Windows panic-symbol link failure, a missing fixed-capacity string constructor, and absent owner/group in `fs` stat); they were resolved or deferred upstream, not patched downstream.
- **Pervasive coupling.** Passing the allocator and Result types everywhere is a deliberate commitment. It pays off on a long-lived, multi-platform codebase and is heavier than warranted for throwaway code.
- **A young library has gaps.** Expect to occasionally find a missing primitive and to fill or report it. The value is concentrated in safety, testability, and portability — not in convenience or raw speed.

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

`proven`은 시간이 지나도 읽기 쉬운 코드를 목표로 한 작은 C23 시스템 라이브러리입니다. C 프로젝트가 자주 직접 다시 구현하게 되는 요소들, 즉 명시적 allocator, owned/borrowed 문자열, 동적 배열, borrowed 또는 owned 문자열 키를 쓰는 map(기본이 HashDoS 저항), 형식화와 파싱, 버퍼드 스트림과 stdin 줄 입력, 파일시스템 헬퍼, 메모리 매핑, 시간, 해싱(FNV, SipHash, CRC-32, SHA-256), 용도별 난수(재현 가능한 생성기·암호학적 생성기·OS CSPRNG), 스택 없는 코루틴, bounded job system을 제공합니다.

의도는 C를 프레임워크 뒤에 숨기는 것이 아닙니다. 실용적인 C를 덜 반복적으로 만들면서도 ownership, error, allocator 선택, platform boundary를 그대로 보이게 두는 데 있습니다.

빌드 드라이버는 먼저 `-std=c23`를 시도하고, 컴파일러가 아직 transitional spelling만 받아들이는 경우 `-std=c2x`로 내려갑니다. 그래서 기존 GCC와 Clang 프런트엔드도 라이브러리의 C23 기준을 바꾸지 않고 트리를 빌드할 수 있습니다.

- 버전: proven_c_lib-v26.07.13g
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
./nob bench-float
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

- Foundation: `types`, `error`, `memory`, `align`, `version`, `config`.
- Allocation: `allocator`, `heap`, `arena`, `pool`.
- Buffers and strings: `buffer`, `u8str`, `u16str`.
- Containers: `array`, `list`, `ring`, `map`.
- Algorithms: `algorithm`.
- Text: `fmt`, `scan`.
- Numbers: `float_parse`, `float_format`.
- 해싱과 난수: `hash` (FNV-1a, SipHash-2-4, CRC-32, SHA-256), `random` (xoshiro256** 재현 가능, ChaCha20 암호학적, OS CSPRNG, 무편향 범위/셔플 헬퍼).
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
