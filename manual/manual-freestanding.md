# Proven Freestanding Mode (v26.07.12e)

This guide describes the current `PROVEN_FREESTANDING` configuration as implemented by `nob.c` and the public headers.

Freestanding mode is for firmware, kernels, bootloaders, hypervisors, or other environments where normal hosted OS services and libc facilities are not available or should not be linked into the proven core.

## 1. Build profile

The build driver uses these freestanding flags:

```sh
-std=c23
-ffreestanding
-DPROVEN_FREESTANDING
-DPROVEN_FMT_NO_FLOAT
-DPROVEN_NO_U16STR
```

The driver still probes `-std=c23` first and falls back to `-std=c2x` when the compiler uses the transitional spelling. The freestanding source contract stays C23-first even though the build wrapper accepts either flag spelling.

The hosted `./nob freestanding` command also links its local freestanding test executables statically on the build host. Cross freestanding targets in `./nob cross` use compile-only object checks.

Run the project freestanding check:

```sh
cc nob.c -o nob
./nob freestanding -build-root /home/user/work/build/proven_c_lib
```

Run the compile-only cross matrix:

```sh
./nob cross -build-root /home/user/work/build/proven_c_lib
```

## 2. Available source files

In the current freestanding build, `nob.c` compiles the portable source files except hosted-only modules.

Included core modules:

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
src/proven/time.c
src/proven/fmt.c
src/proven/scan.c
src/proven/panic.c
platform/proven_sys_math.c
```

Excluded hosted modules:

```text
src/proven/u16str.c
src/proven/fs.c
src/proven/sysio.c
src/proven/mmap.c
src/proven/job.c
platform/proven_sys_fs.c
platform/proven_sys_thread.c
platform/proven_sys_io.c
platform/proven_sys_env.c
platform/proven_sys_time.c
platform/proven_sys_mem.c
```

Do not add excluded hosted modules to a bare-metal build unless you also provide a real platform backend for the target.

## 3. Module availability

| Module | Status in current freestanding profile | Notes |
|---|---|---|
| `types.h`, `error.h`, `align.h`, `memory.h` | Available | Requires fixed-width integer and `uintptr_t` support. |
| `allocator.h` | Available | Trait only. Caller supplies backing allocators. |
| `arena.h` | Available | Primary allocator for static memory regions. |
| `pool.h` | Available | Uses a caller-provided base allocator, often an arena. |
| `buffer.h` | Available | Fixed-capacity byte buffer. |
| `u8str.h` | Available | U8 strings and views. |
| `u16str.h` | Excluded | Current profile defines `PROVEN_NO_U16STR`. |
| `array.h`, `list.h`, `ring.h`, `map.h` | Available | No hidden OS dependency. |
| `algorithm.h` | Available | Sort/search helpers for arrays. |
| `fmt.h` | Available without float | Current profile defines `PROVEN_FMT_NO_FLOAT`. |
| `scan.h` | Available | Scanner for memory views. |
| `time.h` | Limited | Core datetime formatting can compile; real PAL time is excluded. |
| `heap.h` | Stub | `proven_heap_allocator()` returns an invalid allocator. |
| `fs.h`, `mmap.h`, `sysio.h`, `job.h` | Excluded | Require hosted PAL services. |
| `coro.h` | Available | Macro-only stackless coroutine support. |
| `panic.h` | Available | Override for target-specific trap/reset behavior. |

## 4. Minimal static arena setup

```c
#include "proven/types.h"
#include "proven/memory.h"
#include "proven/arena.h"
#include "proven/array.h"
#include "proven/panic.h"

static alignas(PROVEN_MAX_ALIGN) proven_byte_t g_storage[4096];
static proven_arena_t g_arena;
static proven_allocator_t g_alloc;

void platform_init(void) {
g_arena = proven_arena_create((proven_mem_mut_t){
.ptr = g_storage,
.size = sizeof g_storage,
});
g_alloc = proven_arena_as_allocator(&g_arena);
}

proven_err_t make_values(void) {
proven_result_array_t r = PROVEN_ARRAY_INIT(g_alloc, proven_i32, 16);
if (!proven_is_ok(r.err)) return r.err;

proven_array_t values = r.value;
proven_err_t e = PROVEN_ARRAY_PUSH(&values, proven_i32, 42);
PROVEN_ARRAY_DESTROY(&values); /* arena free is a no-op */
return e;
}
```

Common mistake:

```c
proven_allocator_t heap = proven_heap_allocator();
heap.alloc_fn(heap.ctx, 64, 8); /* wrong: heap is invalid in PROVEN_FREESTANDING */
```

Correct:

```c
proven_allocator_t heap = proven_heap_allocator();
if (!proven_alloc_is_valid(heap)) {
/* use an arena, pool, or target-provided allocator instead */
}
```

## 5. Panic handler override

`proven_arena_alloc_or_panic()` and related panic paths call `proven_panic()`, which dispatches to the handler installed with `proven_set_panic_handler()`. The default handler traps.

```c
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

A production panic handler should not return. If it returns, the allocation result that triggered the panic is not guaranteed to be valid.

## 6. Example Cortex-M compile command

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

A real firmware build should compile the included freestanding modules listed above, compile your target startup code, and link with your linker script.

## 7. Example RISC-V ELF compile command

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

If your toolchain uses the `riscv64-unknown-elf-gcc` name, use that compiler instead. The project cross matrix checks both names when available.

## 8. Formatting in freestanding mode

Float formatting is disabled by `PROVEN_FMT_NO_FLOAT`. Integer and string-view formatting remain available.

Correct:

```c
proven_u8str_t s = make_fixed_string_from_arena();
proven_u8str_append_fmt_grow(g_alloc, &s, "value={}", PROVEN_ARG(123));
```

Wrong:

```c
proven_u8str_append_fmt_grow(g_alloc, &s, "{}", PROVEN_ARG(3.14));
/* wrong in the current freestanding profile: float args are excluded */
```

## 8a. Tuning the float big-integer capacity

The exact decimal-to-binary64 fallback and the big-integer division helper use a
fixed-size big integer whose capacity is set by `PROVEN_FLOAT_BIGINT_LIMBS`
(default 160, in `proven/float_config.h`). This is the dominant factor in their
stack usage: each big integer is `8 * PROVEN_FLOAT_BIGINT_LIMBS` bytes, and a
division uses several plus 32-bit scratch.

Targets with tight stacks can lower it, for example:

```sh
-DPROVEN_FLOAT_BIGINT_LIMBS=48
```

The kept-significand cap (`PROVEN_FLOAT_MAX_SIGNIFICAND_DIGITS`) is derived from
the capacity, so a smaller value never overflows: the parser still rounds
correctly for inputs up to the derived number of significant digits and stays
within one ULP for longer inputs. The Clinger and Eisel-Lemire fast paths do not
use the big integer, so typical short inputs are unaffected. Keep the default
(160) when exact rounding of pathological long decimals matters, since a binary64
rounding boundary can need up to 767 significant digits.

## 9. Avoid hosted APIs

Do not call these in the current freestanding profile:

```c
proven_fs_open(...);
proven_println(...);
proven_env_get(...);
proven_mmap_create(...);
proven_job_system_init(...);
```

They require hosted PAL files that are intentionally excluded.

## 10. Lifetime rules still apply

Freestanding does not relax container rules.

Wrong:

```c
int *p = PROVEN_ARRAY_GET_MUT(&arr, int, 0);
PROVEN_ARRAY_PUSH(&arr, int, 9);
*p = 1; /* wrong: push may have moved the array */
```

Wrong:

```c
proven_u8str_view_t key = make_stack_view();
PROVEN_MAP_SET_U8_BORROWED(&map, key, int, 1);
return; /* wrong if map survives after key bytes go out of scope */
```

## 11. Verification

The project freestanding command builds and runs these local checks on the build host:

```text
tests/test_portability_freestanding_heap_stub
tests/test_portability_compile_freestanding
tests/test_portability_compile_nofloat
tests/test_portability_compile_nou16str
tests/test_portability_freestanding
```

The cross command performs compile-only checks for available embedded compilers:

```text
freestanding-arm-cortex-m4        arm-none-eabi-gcc -mcpu=cortex-m4 -mthumb
freestanding-riscv64-elf          riscv64-elf-gcc
freestanding-riscv64-unknown-elf  riscv64-unknown-elf-gcc
```

Missing toolchains are skipped. Real compile failures fail the command. Runtime behavior still needs validation on the target or emulator.
