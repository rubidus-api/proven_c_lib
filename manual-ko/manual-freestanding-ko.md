# Proven Freestanding 모드 (v26.07.13m)

이 가이드는 `nob.c`와 공개 헤더가 구현한 현재의 `PROVEN_FREESTANDING` 구성을 설명한다.

Freestanding 모드는 펌웨어, 커널, 부트로더, 하이퍼바이저, 또는 일반적인 호스티드 OS 서비스와 libc 기능을 사용할 수 없거나 proven 코어에 링크되어서는 안 되는 그 밖의 환경을 위한 것이다.

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
./nob freestanding -build-root /home/user/work/build/proven_c_lib
```

컴파일 전용 크로스 매트릭스를 실행하라:

```sh
./nob cross -build-root /home/user/work/build/proven_c_lib
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
