# Proven C 라이브러리 완전 매뉴얼 (v26.07.23d)

> 이 문서는 영문 매뉴얼([`manual/manual.md`](../manual/manual.md))의 한국어 번역본입니다. 코드와 API 계약을 빌드가 검증하는 정본은 영문 매뉴얼이며, 이 한국어본은 그 미러입니다. 두 문서가 어긋나면 영문본이 기준입니다.

이 매뉴얼은 세 가지 출처에서 재구성됩니다:

1. `include/proven/`의 현재 공개 헤더.
2. `src/proven/`, `platform/`, `tests/`의 현재 구현과 테스트.
3. 저장소 밖 비공개 작업 공간에 보관된 이전 개발자 매뉴얼.

목표는 이전 매뉴얼의 아키텍처 설명을 유지하면서, 현재 소스 트리에서 뽑은 실용 예제·반환값 규칙·소유권 규칙·흔한 오용 사례를 더하는 것입니다.

## 목차

1. [의도와 설계 철학](#1-의도와-설계-철학)
2. [빌드와 include 모델](#2-빌드와-include-모델)
3. [전역 계약(Global contracts)](#3-전역-계약global-contracts)
4. [소유권과 파괴 매트릭스](#4-소유권과-파괴-매트릭스)
5. [연산 동작 클래스](#5-연산-동작-클래스)
6. [매뉴얼 챕터](#6-매뉴얼-챕터)
7. [공개 헤더 맵](#7-공개-헤더-맵)
8. [플랫폼 지원과 검증](#8-플랫폼-지원과-검증)

## 1. 의도와 설계 철학

`proven`은 컴팩트한 C23 시스템 기반 라이브러리입니다. 메모리 ownership, 에러 제어 흐름, 플랫폼 접근을 전역 상태 뒤에 숨기지 않으면서 실용적인 인프라를 원하는 C 프로그램을 위한 것입니다.

libc 대체품이 아닙니다. allocator 기반 메모리 도구, 바이트 view, 컨테이너, 문자열, 형식화(formatting), 파싱(scanning), 해싱(FNV, SipHash, CRC-32, SHA-256), hex/Base64 인코딩, OS 강도 난수, 파일시스템 헬퍼, 버퍼드 스트림, 시간 헬퍼, 메모리 매핑, 스택리스 코루틴 매크로, bounded job system을 집중적으로 제공합니다.

핵심 설계 원칙:

- C23 우선: 빌드 드라이버는 `-std=c23`를 사용합니다.
- 명시적 에러: 실패 가능한 함수는 `proven_err_t` 또는 `proven_result_*_t`를 반환합니다.
- 명시적 ownership: owned 객체는 분명한 destroy 함수와 allocator 규칙을 가집니다.
- Failure atomicity(실패 원자성): grow/realloc 계열 API는 문서에 달리 적지 않는 한 할당 실패 시 기존 객체를 보존합니다.
- 포인터 provenance 규율: 원시 객체 접근은 `proven_byte_t`와 경계 있는 view를 사용합니다.
- PAL 격리: hosted OS 서비스는 `platform/` 아래에 있고 공개 wrapper를 통해 호출됩니다.
- 핵심 컨테이너는 숨겨진 락을 추가하지 않습니다. 공유 변경(shared mutation)은 호출자의 동기화가 필요합니다.
- 빌드 시스템은 저장소에 체크인된 단일 `nob.c` 하나이며, 테스트는 평범한 C 실행 파일입니다.

## 2. 빌드와 include 모델

### 설치할 것이 없습니다

이 라이브러리에는 `configure`도, CMake도, 받아올 패키지도, 어딘가에 놓아야 할 공유 오브젝트도
없습니다. 그냥 C 소스입니다. 여러분의 파일과 나란히 놓고 프로그램에 함께 컴파일하면 됩니다.

이는 의도한 선택이고, 대가가 있습니다. 시스템 패키지를 얻지 못하고, 업데이트는 버전 제약을 올리는
것이 아니라 새 소스를 받아오는 일이 됩니다. 대신 얻는 것은 이렇습니다. 라이브러리가 지금 보고 있는
것과 다른 버전일 수 없고, 여러분이 고르지 않은 빌드 플래그를 주워올 수 없으며, 배포판이 다른 옵션으로
빌드했다는 이유로 링크에 실패할 수 없습니다. hosted 시스템 *과* 베어메탈 양쪽에서 돌아가야 하는
라이브러리에게 "프로그램과 함께 컴파일한다"는 양쪽 모두에서 통하는 유일한 모델입니다.

중요한 디렉터리는 둘입니다. `src/proven/`은 이식 가능한 라이브러리 본체로, 어디에서도 OS를 호출하지
않습니다. `platform/`은 시스템 호출을 하는 얇은 계층이며, 새 타깃이 교체해야 하는 유일한 부분입니다.
freestanding 빌드는 hosted 파일들을 그냥 빼고 빌드합니다. [freestanding
가이드](manual-freestanding-ko.md)를 보십시오.

빌드 드라이버 `nob.c`는 빌드 시스템이 아니라 C 프로그램이며, 여기의 다른 모든 것과 똑같은 방식으로
컴파일합니다. 저장소에 함께 들어 있으므로 부트스트랩 단계도, 따로 설치할 버전도 없습니다.

**C23 컴파일러가 필요합니다** — GCC 13+, Clang 16+, 또는 최근 MSVC. 드라이버가 `-std=c23`을 시험해
보고, 아직 과도기 철자를 쓰는 컴파일러에는 `-std=c2x`로 물러납니다.

hosted 테스트 스위트를 빌드하고 실행:

```sh
cc nob.c -o nob
./nob build
```

자주 쓰는 검증 명령:

```sh
./nob release
./nob strict
./nob strict-error
./nob asan
./nob ubsan
./nob tsan
./nob regression
./nob regression-asan
./nob regression-ubsan
./nob freestanding
./nob cross -build-root build-out/proven_c_lib
```

전체 hosted API가 필요하면 우산(umbrella) 헤더를 사용합니다:

```c
#include "proven.h"
```

더 좁은 translation unit을 원하면 작은 include를 사용합니다:

```c
#include "proven/heap.h"
#include "proven/u8str.h"
#include "proven/fmt.h"
```

직접 hosted 애플리케이션 빌드는 `nob.c`와 같은 소스 레이아웃을 따르면 됩니다:

```sh
cc -std=c23 -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L \
  -Iinclude -Iplatform \
  app.c src/proven/*.c platform/proven_sys_*.c \
  -pthread -o app
```

## 3. 전역 계약(Global contracts)

### 3.1 Result 계약

result 값은 `err == PROVEN_OK`일 때만 사용할 수 있습니다.

올바른 예:

```c
proven_result_mem_mut_t r = alloc.alloc_fn(alloc.ctx, 128, PROVEN_DEFAULT_ALIGNMENT);
if (proven_is_ok(r.err)) {
    /* only now does r.value mean anything */
    proven_mem_mut_t mem = r.value;
    (void)proven_mem_copy(mem.ptr, mem.size, proven_mem_view_from_u8(PROVEN_LIT("hi")));
    alloc.free_fn(alloc.ctx, mem.ptr);
}
```

잘못된 예 (보이는 그대로는 컴파일되지 않음 — 에러를 확인하기 전에 값을 읽음):

```text
proven_result_mem_mut_t r = alloc.alloc_fn(alloc.ctx, 128, PROVEN_DEFAULT_ALIGNMENT);
use_bytes(r.value.ptr, r.value.size); /* wrong: r.err was not checked */
```

### 3.2 공개 struct 계약

많은 공개 struct가 C 사용성을 위해 레이아웃을 노출합니다. 그렇다고 임의 필드 변경이 지원된다는 뜻은 아닙니다. 헤더가 명시적으로 허용하지 않는 한, 내부 필드를 직접 변경하는 것은 호출자 오용으로 취급하세요.

잘못된 예:

```text
arr.len = 999;      /* wrong: breaks array invariants */
str.internal.cap=0; /* wrong: breaks string invariants */
```

불변식(invariant)을 유지하는 함수와 매크로를 사용하세요.

### 3.3 Borrowed view 계약

view는 메모리를 소유하지 않습니다. `proven_u8str_view_t`, `proven_u16str_view_t`, `proven_mem_view_t`, `proven_mem_mut_t`는 참조된 저장소가 살아 있고 이동되지 않은 동안에만 유효합니다.

잘못된 예:

```text
proven_u8str_view_t v = proven_u8str_as_view(&s);
proven_u8str_append_grow(alloc, &s, PROVEN_LIT("more"));
use_view(v); /* wrong: growth may reallocate s */
```

### 3.4 Allocator 계약

`proven_allocator_t`는 세 함수 포인터가 모두 있을 때만 유효합니다. Reallocation은 failure-atomic이어야 합니다: 실패하면 기존 할당이 그대로 유효해야 합니다.

```c
proven_allocator_t heap = proven_heap_allocator();
if (!proven_alloc_is_valid(heap)) {
    /* no usable allocator here: e.g. the freestanding heap stub */
    proven_panic("no heap allocator on this target");
}
```

### 3.5 PAL 경계 계약

`src/proven/` 아래 코드는 OS 헤더에 직접 의존하면 안 됩니다. hosted 서비스는 `platform/proven_sys_*.[ch]`와 `proven_fs_*`, `proven_sysio_*`, `proven_time_*`, `proven_mmap_*` 같은 공개 wrapper를 통해야 합니다.

애플리케이션 코드는 공개 API를 우선하세요. 직접 PAL 호출은 포팅과 플랫폼 통합용입니다.

## 4. 소유권과 파괴 매트릭스

이 라이브러리에는 **두 종류의 객체**가 있고, 그 차이가 여러분이 무엇을 해야 하는지를 결정합니다.

**Owning 객체**는 자신이 할당한 저장소를 쥐고 있으며, 반드시 destroy해야 합니다. 바로 아래 표가 그것입니다.

**Caller-owned state 객체(호출자 소유 상태)**는 아무것도 할당하지 않습니다. 여러분이 — 보통 스택에 — 선언해 생성자에 넘기는 scratch struct이고, 생성자는 그 안을 *가리키는* 작은 값 핸들을 돌려줍니다. **destroy 함수가 없습니다.** 해제할 것이 없기 때문입니다. 대신 owning 객체에는 없는 규칙 하나가 있고, 그게 바로 발목을 잡습니다:

> **핸들이 만들어진 뒤에는 caller-owned state 객체를 복사하거나 이동해서는 안 됩니다.** 핸들은 그 struct 내부를 가리키는 포인터를 쥐고 있습니다. struct를 복사하면 핸들은 여전히 원본을 가리키고, 원본이 스코프를 벗어나면 핸들은 죽은 메모리를 가리킵니다.

이들은 [§4.2](#42-caller-owned-state--destroy-없음-복사-금지)에 정리되어 있습니다.

### 4.1 Owning 객체 — 반드시 destroy해야 함

| 객체 | 저장소 소유 | allocator 보관 | Destroy 함수 | 비고 |
|---|---:|---:|---|---|
| `proven_arena_t` | 아니오 — 호출자가 backing slice 소유 | 아니오 | `proven_arena_destroy(&arena)` (no-op) | 호출자 메모리 위의 bump pointer: `alloc`은 오프셋을 전진시키고, `free`는 no-op, `reset`은 빈 상태로 되감음. backing 블록은 호출자가 소유·해제. |
| `proven_pool_t` | 예 — 아이템 + recycle bin | 예 (`base_alloc`) | `proven_pool_destroy(&pool)` | 고정 아이템 크기. **allocator 트레잇을 통해**(`proven_pool_as_allocator`) 사용합니다: 트레잇의 `free_fn`은 슬롯을 해제하는 대신 재사용을 위해 recycle bin으로 되돌립니다. `proven_pool_free`는 없습니다 — 다른 allocator와 마찬가지로 해제는 트레잇을 거칩니다. |
| `proven_buf_t` | 예 | 아니오 | `proven_buf_destroy(alloc, &buf)` | 호출자가 일치하는 allocator를 넘겨야 함. |
| `proven_u8str_t` | 예 | 아니오 | `proven_u8str_destroy(alloc, &str)` | 유효할 때 항상 NUL 종단. |
| `proven_u16str_t` | 예 | 아니오 | `proven_u16str_destroy(alloc, &str)` | 내부 길이는 바이트로 추적; API 길이는 `proven_u16` 단위. |
| `proven_array_t` | 예 | 예 | `proven_array_destroy(&arr)` 또는 `PROVEN_ARRAY_DESTROY(&arr)` | 원소를 가리키는 포인터는 성장으로 무효화될 수 있음. |
| `proven_ring_t` | 예 | 예 | `proven_ring_destroy(&ring)` 또는 `PROVEN_RING_DESTROY(&ring)` | 고정 용량; 성장 없음. |
| `proven_map_t` | 예 | 예 | `proven_map_destroy(&map)` 또는 `PROVEN_MAP_DESTROY(&map)` | Borrowed U8 키는 복사되지 않음. |
| `proven_fs_list()`가 반환한 `proven_array_t` | 예 | 예 + owned 엔트리 이름 | `proven_fs_list_destroy(alloc, &list)` | 평범한 array destroy를 쓰지 말 것; 엔트리 이름 정리가 필요. |
| `proven_mmap_t` | OS 매핑 | OS 핸들 상태 | `proven_mmap_destroy(&map)` | 매핑을 가리키는 view는 매핑과 함께 죽음. |
| `proven_job_sys_t *` | 예 | 내부 | `proven_job_system_close(sys)` 후 `proven_job_system_destroy(sys)` | destroy는 producer와 경쟁(race)하면 안 됨. |

### 4.2 Caller-owned state — destroy 없음, 복사 금지

이들은 아무것도 할당하지 않고 아무것도 해제하지 않습니다. 하나 선언해 그 주소를 생성자에 넘기고, 돌려받은 작은 핸들을 사용합니다. struct는 핸들보다 **오래 살아야** 하며, 핸들이 살아 있는 동안 **복사·이동해서는 안 됩니다**.

| State 객체 | 생성 함수 | 뒷받침하는 핸들 | 비고 |
|---|---|---|---|
| `proven_sha256_t` | `proven_sha256_init` | (직접 사용) | 해싱 컨텍스트. 시작 *전*에는 복사해도 안전하고, 스트림 도중 복사는 해시를 fork하려는 의도가 아닌 한 무의미. |
| `proven_xoshiro256ss_t` | `proven_xoshiro256ss_seed` | `proven_rng_t` | 복사하면 **수열이 복제됩니다** — 재현(replay)에는 의도적이고 유용하지만, 그 외에는 버그. |
| `proven_chacha_rng_t` | `proven_chacha_rng_seed` / `_seed_from_entropy` | `proven_rng_t` | 복사하면 키스트림이 복제됩니다: "독립적인" 두 토큰이 같은 토큰이 됨. |
| `proven_writer_buf_t` | `proven_writer_from_buffer` | `proven_writer_t` | **복사 금지.** |
| `proven_writer_u8str_t` | `proven_writer_from_u8str` | `proven_writer_t` | **복사 금지.** 문자열과 allocator도 그보다 오래 살아야 함. |
| `proven_writer_buffered_t` | `proven_writer_buffered` | `proven_writer_t` | **복사 금지.** 그것이나 그 버퍼가 죽기 전에 반드시 `proven_writer_flush` 해야 함. |
| `proven_reader_view_t` | `proven_reader_from_view` | `proven_reader_t` | **복사 금지.** |
| `proven_reader_buffered_t` | `proven_reader_buffered` | `proven_reader_t` | **복사 금지.** `proven_reader_read_line`이 반환한 view는 이 버퍼 *안을* 가리킴. |
| `proven_sysio_std_t` | `proven_sysio_stdout_writer` / `_stderr_writer` / `_stdin_reader` | `proven_writer_t` / `proven_reader_t` | **복사 금지.** writer가 가리키는 표준 핸들을 보관. |
| `proven_sysio_out_t` | `proven_sysio_stdout_buffered` / `_file_buffered` | `proven_writer_t` | **복사 금지.** 반드시 flush 필요. |
| `proven_sysio_lines_t` | `proven_sysio_lines_open` / `_stdin_lines` | (`proven_sysio_read_line`으로 사용) | 유일한 예외: `proven_sysio_read_line`이 매 호출마다 재바인딩하므로, 이것은 **이동해도** 됨. |
| `proven_sysio_scanner_t` | `proven_sysio_scanner_init` | (직접 사용) | 반대 방향의 예외: 이것은 버퍼를 **소유**하므로 `proven_sysio_scanner_deinit`을 호출해야 함. |

잘못된 예 — 이 복사는 무해해 보이지만 use-after-free입니다:

```text
proven_sysio_out_t out;
proven_writer_t w = proven_sysio_stdout_buffered(&out, buf);

proven_sysio_out_t saved = out;   /* wrong: `w` still points into `out`, not `saved` */
use_elsewhere(&saved);            /* and if `out` goes out of scope, `w` is dangling */
```

잘못된 예 — 팩토리 함수에서 state를 값으로 반환해도 같은 일이 벌어집니다:

```text
proven_sysio_out_t make_logger(void) {
    proven_sysio_out_t out;
    proven_writer_t w = proven_sysio_stdout_buffered(&out, buf);
    (void)w;
    return out;   /* wrong: any writer made from `out` addresses this dead frame */
}
```

올바른 예 — state는 제자리에 두고, 핸들이 이동합니다:

```c
proven_byte_t buf[512];
proven_sysio_out_t out;                                   /* lives as long as `w` */
proven_writer_t w = proven_sysio_stdout_buffered(&out,
    (proven_mem_mut_t){ .ptr = buf, .size = sizeof buf });

(void)proven_fprintln(w, "one syscall, not {}", PROVEN_ARG(100));
(void)proven_writer_flush(w);                             /* or the bytes never happened */
```

## 5. 연산 동작 클래스

### 한 연산, 세 가지 정직한 대답

"이 텍스트를 저 문자열에 덧붙여라"는 텍스트가 들어가지 않을 때 옹호할 만한 동작이 셋 있고, 대부분의
라이브러리는 그중 하나를 골라 그 선택을 숨깁니다. 이 라이브러리는 셋을 모두 드러내고, 각각에 다른
이름을 주며, 그 차이를 시그니처에 둡니다 — 어느 것을 원하는지는 그 텍스트가 *무엇인지*에 달려 있고,
그것은 호출자만 알기 때문입니다.

파일 경로에 덧붙이는 경우를 생각해 보십시오.

- 경로가 들어가지 않는데 **자르는 것은 치명적입니다.** `documents/report.pdf`가
  `documents/rep`가 되는데, 이는 다른 파일이고 실제로 존재할 수도 있는 파일입니다. 이 연산은
  실패하고 아무것도 바꾸지 않아야 합니다.
- 이제 로그 한 줄에 덧붙이는 경우를 생각해 보십시오. 들어가지 않으면 **자르는 것이 괜찮습니다** —
  메시지 전체를 잃느니 대부분이라도 남는 편이 낫습니다 — 얼마나 썼는지 알려주기만 한다면요.
- 그리고 쌓아 올리는 중인 버퍼에 덧붙이는 경우, 그냥 **자라기를** 원할 것입니다.

아래 세 클래스가 그 세 대답입니다. 어느 것을 받을지는 함수 이름과 allocator 매개변수의 유무가
결정하며, 플래그나 전역 변수가 결정하는 일은 결코 없습니다.

- `proven_u8str_append(str, data)` — allocator가 없으므로 자랄 수 없습니다: **거부합니다**.
- `proven_u8str_append_partial(str, data)` — 개수를 반환합니다: **자르고 알려줍니다**.
- `proven_u8str_append_grow(alloc, str, data)` — allocator를 받습니다: **자랍니다**.

라이브러리 전반의 기본값은 첫 번째이며, [챕터 0
§5](manual-00-start-here-ko.md#5-모든-페이지에서-만나게-될-다섯-가지-계약)가 그 이유를 설명합니다.
잘린 경로는 엉뚱한 파일을 열고, 잘린 명령은 엉뚱한 명령을 실행하며, 잘린 숫자는 다른 숫자입니다.

잘못된 예 — truncating 형태가 돌려준 개수를 무시:

```text
(void)proven_u8str_append_partial(&s, huge);   /* wrong: the count WAS the answer */
```

여러 API가 의도적으로 세 가지 동작 클래스를 드러냅니다:

| 클래스 | 예 | 용량 부족 시 동작 |
|---|---|---|
| Atomic 고정 용량 | `proven_u8str_append`, `proven_u16str_append`, `proven_u8str_append_fmt` | 에러를 반환하고 기존 객체를 그대로 둠. |
| Best-effort truncating | `proven_u8str_append_partial`, `proven_u16str_append_partial`, `proven_u8str_append_fmt_trunc` | 들어가는 만큼 쓰고, 유효한 객체를 보존하며, 얼마를 썼는지 보고. |
| Atomic growable | `proven_u8str_append_grow`, `proven_u16str_append_grow`, `proven_u8str_append_fmt_grow` | allocator로 성장; 할당 실패 시 기존 객체를 그대로 둠. |
| **거부하되 절대 자르지 않음** | `proven_hex_encode`, `proven_base64_encode`, `proven_base64_decode`, `proven_reader_read_line` | **아무것도 쓰지 않고** `PROVEN_ERR_OUT_OF_BOUNDS`를 반환. 반쯤 인코딩된 문자열이나 짧아진 줄은 옳아 보이는 오답이므로, 이들은 그런 것을 만들지 않음. 버퍼 크기는 눈대중이 아니라 모듈의 크기 함수(`proven_base64_encoded_size` 등)로 정할 것. |

클래스를 의도적으로 고르세요. truncating 함수를 all-or-nothing 함수처럼 다루지 마세요.

잘못된 예 — truncating과 atomic 형태가 똑같이 동작한다고 가정:

```text
/* `_partial` wrote what fit and told you so; the error you did not read is the
   difference between "all of it" and "some of it". */
(void)proven_u8str_append_partial(&s, huge);   /* wrong: the result was the point */
```

## 6. 매뉴얼 챕터

상세 레퍼런스는 읽기 쉽고 소스에 근거를 두도록 챕터별로 나뉩니다.

**이 라이브러리가 처음이라면 [0장](manual-00-start-here-ko.md)부터 읽으십시오.** 아무것도 전제하지
않는 유일한 장입니다: 이 라이브러리가 왜 존재하는지를 이 라이브러리가 답하려는 C의 버그들로부터
논증하고, hello world 프로그램, 빌드 방법, 나머지 장들이 당연하게 여기는 다섯 가지 계약, 그리고
용어집과 libc 대응표를 담고 있습니다.

### 읽는 순서

챕터들은 부(Part)로 묶여 있고, 각 부는 그 앞의 부들만 필요로 하도록 배열되어 있습니다. 이 순서는
헤더 의존 그래프와 같지 않으며, 임의로 정한 것도 아닙니다. 문자열은 allocator가 필요하고, 컨테이너도
allocator가 필요하며, hosted 서비스는 문자열이 필요합니다. 아래 순서는 자료 자체가 요구하는 순서입니다.

챕터 *번호*는 안정적인 식별자이지 읽는 순서가 아닙니다. 그중 둘은 일부러 순서를 벗어나 있습니다.
alias 인덱스는 찾아보는 부록이고, 8장은 3장이 주제를 소개한 뒤에 읽는 레퍼런스입니다.

| 부 | 읽을 것 | 선행 조건 | 그러면 할 수 있는 것 |
|---|---|---|---|
| **I — 여기서부터** | [0](manual-00-start-here-ko.md) | 입문용 C 책 한 권 | 라이브러리를 붙여 빌드하고, 아래 무엇이든 읽기 |
| **II — 모든 프로그램이 쓰는 어휘** | [1](manual-01-foundation-ko.md) → [2](manual-02-allocation-ko.md) → [3](manual-03-strings-text-ko.md) | 0장 | 에러를 값으로 다루고, 메모리를 의도적으로 소유하고, 텍스트를 안전하게 담기 |
| **III — 자료구조** | [4](manual-04-containers-algorithms-ko.md) | II부 | array, map, list, ring, 정렬, 검색, 해싱, 인코딩 |
| **IV — 텍스트 입출력** | [8](manual-08-fmt-scan-ko.md) | 3장 §3–§4 | 무엇이든 형식화하고 파싱하며, 포매터에 내 타입 가르치기 |
| **V — 운영체제와 대화하기** | [5](manual-05-hosted-services-ko.md) | II부 | 파일, 디렉터리, 스트림, 표준 I/O, 시간, 난수, 매핑 |
| **VI — 더 나아가기** | [6](manual-06-execution-and-platform-ko.md) → [freestanding](manual-freestanding-ko.md) | II–V부 | 코루틴, job, 스레드 안전성, 베어메탈, 크로스 빌드 |
| **부록** | [A](manual-07-alias-xcv-index-ko.md), [B](manual-00-start-here-ko.md#6-부록-b-용어집), [C](#7-공개-헤더-맵), [D](manual-00-start-here-ko.md#7-부록-d-libc-대응표) | — | 찾아보기 |

### 챕터 목록

0. [**여기서부터 시작**: 왜 존재하는가, hello world, 다섯 계약, 용어집, libc 대응표](manual-00-start-here-ko.md) — *I부*
1. [**Foundation**: 타입, 에러, 메모리 view, 정렬, 버전, panic](manual-01-foundation-ko.md) — *II부*
2. [**Allocation**: heap, arena, pool, byte buffer, 그리고 allocator 트레잇](manual-02-allocation-ko.md) — *II부*
3. [**문자열과 텍스트**: U8, U16, 그리고 형식화와 파싱 입문](manual-03-strings-text-ko.md) — *II부; 텍스트 자료의 튜토리얼 절반*
4. [**컨테이너와 알고리즘**: array, list, ring, map, 정렬/검색, 해싱, 인코딩](manual-04-containers-algorithms-ko.md) — *III부*
5. [**Hosted 서비스**: 파일시스템, 트리 순회, 스트림, sysio, 환경변수, 난수, mmap, 시간](manual-05-hosted-services-ko.md) — *V부*
6. [**실행과 플랫폼**: 코루틴, job, 스레드 안전성, alias, PAL, 크로스 빌드](manual-06-execution-and-platform-ko.md) — *VI부*
7. [**부록 A — Alias 인덱스**: `alias_xcv.h`의 모든 철자](manual-07-alias-xcv-index-ko.md) — *참조 전용; 읽는 자료가 아님*
8. [**형식화와 파싱**: 전체 `fmt.h`와 `scan.h` 레퍼런스](manual-08-fmt-scan-ko.md) — *IV부; 텍스트 자료의 레퍼런스 절반*

**3장과 8장은 둘 다 포매터와 스캐너를 다루며, 이 분담은 의도적입니다.** 3장은 문자열과 나란히 이들을
소개하며, 일상적인 경우와 생산성을 내기에 충분한 만큼을 담습니다. 8장은 완전한 레퍼런스입니다. 전체
형식 문법, 모든 인자 생성자, 스캐너의 에러 코드와 복구 규칙, 그리고 포매터에 내 타입을 가르치는 방법을
담습니다. 3장을 먼저 읽고, 어떤 지정자나 실패의 정확한 동작이 필요할 때 8장을 펴십시오.

## 7. 공개 헤더 맵

### 부록 C — 찾는 방법

공개 헤더는 35개이고 우산 헤더가 하나 있습니다. `#include "proven.h"`는 전부를 끌어오며, 이 매뉴얼의
예제들이 하는 방식입니다. 개별 헤더를 include하는 것은 컴파일 시간이 신경 쓰이거나 의존 관계를 파일에
드러내고 싶을 때 씁니다.

이 표가 파일 이름만으로는 알 수 없는 두 가지를 알려줍니다.

- **어느 챕터가 그것을 문서화하는가.** 모든 헤더에는 그것을 설명하는 챕터가 정확히 하나 있고, 빌드는
  모든 공개 함수가 `manual/` 어딘가에 이름을 올리도록 강제합니다 — 그러니 어떤 심벌이 기대한 챕터에
  없더라도 매뉴얼 어딘가에는 있고, 이 맵이 그 위치를 알려줍니다.
- **freestanding 빌드에서 살아남는가.** 챕터 5에 배정된 헤더들이 hosted 쪽입니다. 파일시스템, 표준
  스트림, 시계, 가상 메모리 또는 스레드가 필요합니다. 그 밖의 모든 것은 운영체제 없이 컴파일됩니다.
  모듈별 권위 있는 표는 [freestanding 가이드](manual-freestanding-ko.md)에 있습니다.

| 헤더 | 주요 용도 | 챕터 |
|---|---|---|
| `proven.h` | 우산 include | 이 파일 |
| `types.h` | 고정 폭 별칭, 검사된 산술, 에러 enum | 챕터 1 |
| `error.h` | 에러 술어(predicate) 헬퍼 | 챕터 1 |
| `memory.h` | 바이트 view, 슬라이싱, 범위 검사, memcmp | 챕터 1 |
| `align.h` | 정렬 상수와 align-up 헬퍼 | 챕터 1 |
| `version.h` | 버전 매크로 | 챕터 1 |
| `panic.h` | 등록 가능한 panic 핸들러 | 챕터 1 |
| `config.h` | 컴파일 타임 기능 토글 (`PROVEN_FREESTANDING`, `PROVEN_FMT_NO_FLOAT`, `PROVEN_NO_U16STR` 등) | 챕터 1, 6 |
| `allocator.h` | Allocator 트레잇 | 챕터 2 |
| `heap.h` | PAL 기반 heap allocator | 챕터 2 |
| `arena.h` | Bump allocator | 챕터 2 |
| `pool.h` | 고정 크기 recycler allocator | 챕터 2 |
| `buffer.h` | 고정 용량 byte buffer | 챕터 2 |
| `u8str.h` | Owned U8 문자열과 borrowed U8 view | 챕터 3 |
| `u16str.h` | Owned U16 문자열과 borrowed U16 view | 챕터 3 |
| `fmt.h` | 구조적 formatter와 format 인자 | 챕터 3 |
| `scan.h` | 구조적 scanner와 타입 있는 scan 목적지 | 챕터 3 |
| `float_parse.h` | 로케일 없는 십진수 → `double`/`float` 파서 (`proven_strtod`, `proven_parse_double_ascii`) | 챕터 8 |
| `float_format.h` | `double`/`float` → 십진수 formatter (고정 `%f`/`%e`, shortest) | 챕터 8 |
| `float_config.h` | Float 엔진 튜닝 (`PROVEN_FLOAT_BIGINT_LIMBS`, 정밀도 상한) | 챕터 6, 8 |
| `array.h` | 제네릭 growable 벡터 | 챕터 4 |
| `list.h` | 침입형(intrusive) 이중 연결 리스트 | 챕터 4 |
| `ring.h` | 고정 용량 FIFO ring | 챕터 4 |
| `map.h` | Open-addressing map | 챕터 4 |
| `algorithm.h` | Array 정렬·검색 헬퍼 | 챕터 4 |
| `hash.h` | FNV-1a, SipHash-2-4, CRC-32, SHA-256, 용도별 | 챕터 4 |
| `encode.h` | Hex와 Base64 (표준 + URL-safe), 바이트↔텍스트 | 챕터 4 |
| `fs.h` | 파일, 디렉터리, 메타데이터, 링크, 락, read-all, 트리 순회 | 챕터 5 |
| `stream.h` | 버퍼드 writer·reader와 line reader — 그리고 `sysio.h`를 통해 표준 스트림 (hosted 전용) | 챕터 5 |
| `sysio.h` | 표준 스트림을 writer/reader로, stdin 줄 입력, 버퍼드 출력, 출력, 파싱, 환경변수 접근 | 챕터 5 |
| `random.h` | 용도별 난수: xoshiro256** (재현 가능), ChaCha20 (암호학적), OS CSPRNG, 무편향 range/shuffle 헬퍼. 생성기는 freestanding에서 동작하고 OS 소스만 hosted. | 챕터 5 |
| `mmap.h` | 메모리 매핑 파일 영역 | 챕터 5 |
| `time.h` | 타임스탬프, datetime, sleep, datetime 형식화 | 챕터 5 |
| `coro.h` | 스택리스 코루틴 매크로 | 챕터 6 |
| `job.h` | Bounded worker-thread job system | 챕터 6 |
| `alias_xcv.h` | 선택적 짧은 alias 계층과 생성된 철자 맵 | 챕터 6, 7 |

## 8. 플랫폼 지원과 검증

주요 검증 hosted 대상:

- C23 모드의 GCC 또는 Clang을 쓰는 Linux x86_64.

해당 toolchain이 설치되어 있으면 compile-only 크로스 커버리지가 존재합니다:

- Linux AArch64.
- Linux ARM hard-float.
- `i686-linux-gnu-gcc` 또는 `gcc -m32` multilib을 통한 Linux i686.
- MinGW/WinAPI 경로를 통한 Windows x86_64와 i686.
- ARM Cortex-M freestanding.
- RISC-V ELF freestanding.

크로스 매트릭스는 컴파일, 공개 헤더 가시성, 대상 ABI 가정을 확인합니다. 대상 플랫폼에서의 런타임 검증을 대체하지는 않습니다.

Freestanding 모드는 OS 기반 서비스를 제거한 축소된 서브셋을 빌드합니다. 자세한 내용은 [`manual-freestanding-ko.md`](manual-freestanding-ko.md)와 챕터 6을 참조하세요.
