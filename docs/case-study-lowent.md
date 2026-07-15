# Case study: building a language toolchain on `proven`

This document records what `proven` actually contributed to **lowent-lang** — a
C23 implementation of the Lowent programming language (a lexer, parser,
typechecker, region/effect analyzer, content-addressed stack IR, tree-walking
evaluator, C backend, and an interactive shell). It is a second data point next
to the terminal-editor notes in `README.md`, and it is deliberately written as a
measurement rather than an endorsement: a compiler front-end stresses a
different part of the library than an editor does, and the parts it never
touched are as informative as the parts it leaned on.

- **Downstream project:** lowent-lang (`impl/` toolchain + `lowshell/` shell)
- **`proven` version vendored:** `proven_c_lib-v26.06.24b`, on 2026-07-03
- **Integration model:** copy-only vendoring, no patches
- **Upstream defects filed:** none (see `docs/REPORT.md` — no entries from this project)

---

## 1. What was integrated, in numbers

`proven` was vendored as a byte-identical source copy under
`impl/vendor/proven/`, with the upstream revision and license recorded in a
`VENDORED.md`. A directory-level diff against upstream reports no differences:
the copy-only, do-not-patch contract held for the whole project.

The vendored tree carries 29 translation units. **The toolchain compiles 13 of
them:**

    arena  memory  buffer  u8str  array  map  panic  heap  pool  scan
    float_parse  float_decimal  platform/proven_sys_mem

That is **6,013 lines of `proven` code linked against 11,259 lines of
first-party lowent code** — the library supplies roughly a third of the C that
ships in the toolchain binary, and every line of it is code the language
implementers did not write, test, or debug.

The interactive shell (`lowshell/`) links the *same* vendored tree by relative
path rather than taking a second copy. It also vendors terminal-UI widgets from
`prov_text_editor`, whose own vendoring note points back at this rule — "uses
lowsh's existing `impl/vendor/proven`; do not vendor a second proven." Two
independent downstream projects converging on one copy of the library is a small
result, but it is the kind of thing that only works if the vendoring contract is
stated explicitly.

## 2. What `proven` bought

**The arena *is* the language's memory model.** Lowent-mini specifies an
arena-and-bulk-free runtime (SPEC §8.1). `proven_arena_t` over caller-supplied
backing memory, exposed through `proven_arena_as_allocator`, implements that
specification directly — the front-end allocates CST and IR nodes from an arena
and drops the whole region at the end of a compilation unit. No per-node
ownership, no destructor walk, and structurally no leak class to test for. This
is the rare case where a library primitive and a language's own semantics line
up exactly, and it removed a design decision rather than merely some typing.

**Zero-copy lexing came free.** `proven_u8str_view_t` — a pointer-plus-length
borrowed slice — is precisely the token type a lexer wants, so tokens are slices
into the source buffer with no interning or copying layer. `proven_u8str_view_eq`
(45 call sites) does keyword and identifier matching directly against the source
bytes. The lexer never allocates.

**Correctly-rounded float literals, for the cost of adding two files to a
Makefile.** Every language implementation must convert decimal float literals to
`binary64` exactly, and doing it correctly — Clinger fast path, Eisel–Lemire,
exact big-integer arbiter, round-to-nearest ties-to-even — is one of the hardest
and least glamorous pieces of a front-end. Lowent got it by listing
`float_parse.c` and `float_decimal.c` in `PROVEN_SRC`. The same code, reached
through `proven_scan_*`, also backs the language's `scan` builtin, which
auto-types input fields as integer, float, or string. Measured against what it
would have cost to write and validate this from scratch, this is very likely the
single largest concrete saving in the integration.

**Containers a compiler needs, already hardened.** `proven_array_t` (growable
vectors, ~90 call sites across push/get/init/destroy) and `proven_map_t` carry
the symbol tables, node lists, and interning structures. `proven_size_t`,
`proven_i64`, `proven_u8`, `proven_byte_t` and friends appear over 1,300 times —
the library's type vocabulary became the project's type vocabulary.

**A house style that transferred.** The implementation adopted `proven`'s
errors-as-values convention wholesale: result structs of `{err, value}`,
`[[nodiscard]]` on fallible calls, no exceptions, no `errno`, no macro control
flow — and its naming shape (`low_` prefix, `snake_case`, `_t` suffix) mirrors
`proven_`. Across 8,500 lines of front-end written largely in one push, having a
pre-decided answer to "how does this function report failure" is worth more than
it sounds.

**It held up without patching.** `prov_text_editor` hit three library gaps in its
first months and filed all three upstream. Lowent, building something
substantially larger against v26.06.24b, filed none. That is not proof of
maturity, but it is the difference between a library at the stage where
integration means bug-hunting and one where integration means reading the header.

## 3. What `proven` did not do — stated plainly

**It did not influence the language design.** This needs saying because the word
invites confusion. Lowent's specification documents contain many occurrences of
"proven", and essentially none of them refer to this library: they are
*provenance* (the memory-model term), the safety grade `proven | static |
dynamic` from RFC-0017, or *proven-lang*, an unrelated earlier language-design
lineage. RFC-0043, which specifies allocator-as-capability — the one place where
one might expect `proven`'s allocator trait to be cited — names Zig, Rust's
`Allocator`, and C++ `pmr` as its models, and does not mention `proven_c_lib` at
all. **The contribution was to the implementation: memory strategy, string type,
container substrate, error idiom, naming. Not to the language.** A library that
shapes how a compiler is built is a useful library; claiming it shaped the
language it compiles would be false.

**Over half the library was never compiled.** The 16 unused translation units
include `fs`, `sysio`, `mmap`, `time`, `coro`, `job`, `fmt`, `float_format`,
`algorithm`, `ring`, `list`, `u16str` and five of the six platform PALs. This is
the library working as intended — a menu, not a framework — but it means the
portability story that `prov_text_editor` reports (one codebase across Linux
x86_64/arm64/armhf and Windows, no `#ifdef`s, via the `fs`/`time` PAL) simply
did not arise here. A compiler reads files and writes files; it did not need it.

**The shell could not use the PAL at all.** `lowshell` drives `fork`, `execve`,
`termios`, `tcsetpgrp`, `glob`, and `getpwnam` directly against POSIX, because
`proven` has no process-control or terminal PAL. That is why the shell's
cross-compile matrix covers five Linux architectures and excludes Windows
outright. Portability here came from POSIX, not from `proven`.

**The toolchain's `main` uses libc directly.** `impl/src/main.c` reads source
files with `fopen`/`fread`/`malloc` rather than `proven/fs.h`, even though `fs`
is sitting vendored in the tree. Whether that is a gap in `proven` or simply an
unadopted module, it is an honest dent in the project's own stated "reuse proven"
doctrine, and it is recorded here rather than smoothed over.

**`fmt` contributed a grammar, not code.** The language's `fmt` builtin adopted
`proven/fmt.h`'s `{}` / `{N}` / `{:[fill]align[width][x]}` format grammar — but
reimplemented it inside the evaluator instead of linking `fmt.c`, because the
builtin formats language *values*, not C variadic arguments. The specification
was reusable; the implementation was not.

**No hash primitive existed, so one was hand-written.** Lowent's content-addressed
IR needs a cryptographic digest. `proven` has none, so the project wrote
BLAKE3-256 itself (126 lines) — in `proven`'s types (`proven_u32`,
`proven_byte_t`, `proven_size_t`), but as its own algorithm.

## 4. Observations that may be worth acting on upstream

These are not defect reports and are not filed in `docs/REPORT.md`; they are the
shape of the holes this integration fell into.

1. **No hash / digest module.** Content-addressed IR, build caches, and file
   fingerprinting are ordinary systems work, and every project that needs one
   writes its own. A `proven_hash` with a well-specified digest would have
   removed 126 lines and a correctness risk from this project.
2. **No process or terminal PAL.** `fork`/`exec`/`termios` is the boundary at
   which `proven`'s otherwise-clean platform isolation stops, and it is exactly
   the boundary a shell lives on. It is a legitimate scope decision — but it is
   the reason one of the two downstream binaries here has raw `#include
   <termios.h>` in it.
3. **`fmt` is hard to reuse for a value-formatting host.** Its `{}` grammar is
   good enough that a downstream reimplemented it; a lower-level entry point that
   formats a caller-supplied value rather than a `proven_arg_t` might make the
   code, not just the syntax, reusable.

## 5. Summary judgment

For a language toolchain, `proven` behaved as a **substrate, not a framework**,
and the split is clean: it supplied memory, strings, containers, numeric
conversion, and an error discipline — about a third of the shipped C — and
supplied nothing at all to the language's design or to the shell's OS boundary.
The two highest-value contributions were the ones nobody would put on a feature
list: an arena that happened to be the exact memory model the language had
already specified, and a correctly-rounded decimal-to-`binary64` parser obtained
by editing one line of a Makefile.

The integration ran to completion, under ASan and UBSan, without patching the
library and without filing a single defect against it.

---

# 한국어 요약

이 문서는 `proven`이 **lowent-lang**(Lowent 언어의 C23 구현 — 렉서, 파서,
타입체커, 리전/이펙트 분석, 내용주소화 스택 IR, 평가기, C 백엔드, 대화형 셸)에
실제로 무엇을 기여했는지 기록한 것입니다. `README.md`의 텍스트 에디터 사례에
이어지는 두 번째 데이터 포인트이며, 홍보가 아니라 측정으로 적었습니다.

**통합 규모.** v26.06.24b를 무수정 소스 복사로 vendoring(상류와 바이트 단위
동일, 무패치 계약 준수). 29개 번역 단위 중 **13개만 컴파일** — `proven` 코드
6,013줄이 lowent 자체 코드 11,259줄과 함께 링크됩니다. 즉 툴체인 바이너리에
들어가는 C의 약 3분의 1이 라이브러리 몫입니다.

**얻은 것.**
- **아레나가 곧 언어의 메모리 모델이었습니다.** lowent-mini 명세 §8.1의
  아레나+일괄해제가 `proven_arena_t`와 정확히 일치해, CST/IR 노드를 아레나에서
  할당하고 통째로 버립니다. 누수 클래스 자체가 사라졌습니다.
- **무복사 렉싱이 공짜였습니다.** `proven_u8str_view_t`가 토큰 슬라이스 타입 그
  자체라 렉서는 할당을 하지 않습니다.
- **정확 반올림 float 리터럴을 Makefile 두 줄로 얻었습니다.** 프런트엔드에서
  가장 어렵고 티 안 나는 부분(Clinger → Eisel–Lemire → 정확 bigint)을 그대로
  가져왔습니다. 이 통합에서 가장 큰 실질적 절약일 가능성이 높습니다.
- 배열/맵 컨테이너, 그리고 errors-as-values + `[[nodiscard]]` 관례가 8,500줄
  구현 전체의 하우스 스타일이 되었습니다.
- **패치 없이 버텼습니다.** prov_text_editor는 초기에 결함 3건을 상류에
  보고했지만, 더 큰 규모인 lowent는 **0건**입니다.

**하지 않은 것(과장 방지).**
- **언어 설계에는 기여하지 않았습니다.** 명세 문서의 "proven"은 대부분
  *provenance*(메모리 모델 용어), RFC-0017의 안전 등급 `proven|static|dynamic`,
  또는 별개 계보인 *proven-lang*입니다. allocator-as-capability를 규정한
  RFC-0043조차 Zig·Rust·C++ pmr을 모델로 들 뿐 `proven_c_lib`을 언급하지
  않습니다. 기여는 **구현**(메모리 전략·문자열 타입·컨테이너·에러 관례·명명)에
  한정됩니다.
- 라이브러리의 절반 이상(`fs`, `sysio`, `mmap`, `time`, `coro`, `job`, `fmt`,
  PAL 5개 등 16개 단위)은 컴파일조차 되지 않았습니다. 에디터가 보고한 이식성
  이점은 여기서는 발생하지 않았습니다.
- **셸은 PAL을 전혀 쓸 수 없었습니다.** `fork`/`termios`/`tcsetpgrp`를 POSIX로
  직접 호출합니다(그래서 Windows는 대상 외).
- `impl/src/main.c`는 `proven/fs.h` 대신 libc `fopen`/`fread`를 씁니다 — 프로젝트
  자신의 "proven 재사용" 원칙에 난 흠집이며, 덮지 않고 그대로 기록합니다.
- `fmt`는 **문법만** 차용되고 코드는 재구현됐습니다.
- 해시가 없어 BLAKE3-256(126줄)을 직접 작성했습니다.

**상류에서 고려해 볼 만한 점.** (결함 보고가 아니라 관찰입니다.)
해시/다이제스트 모듈 부재, 프로세스·터미널 PAL 부재, 값 포맷팅 호스트가
재사용하기 어려운 `fmt` 진입점.

**총평.** 언어 툴체인에서 `proven`은 **프레임워크가 아니라 기반(substrate)**으로
동작했고, 그 경계는 깨끗합니다. 가장 값진 두 기여는 기능 목록에 올릴 만한
것들이 아니었습니다 — 언어가 이미 명세해 둔 메모리 모델과 우연히 정확히 일치한
아레나, 그리고 Makefile 한 줄로 얻은 정확 반올림 십진→`binary64` 파서입니다.
