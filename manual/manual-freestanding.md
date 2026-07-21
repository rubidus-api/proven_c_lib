# Proven Freestanding Mode (v26.07.20g)

**Part VI — Going further. Prerequisites: Parts II–V, and
[Chapter 6](manual-06-execution-and-platform.md).**
**After this guide** you can build the library for a target with no operating system and no libc,
and you will know exactly which modules survive that and which do not.

This guide describes the current `PROVEN_FREESTANDING` configuration as implemented by `nob.c` and the public headers.

**A note on the shape of this guide.** Unlike the chapters, roughly half of it is procedure — flag
lists, file listings, and two compile commands. Those sections are deliberately short: a compile
command explained at length is a compile command nobody reads. The sections that carry a *decision*
— why this mode exists (§0), what "excluded" really means (§3), what your panic handler has to do
(§5), what you lose by dropping float (§8), why hosted calls fail at link time (§9), why the
lifetime rules matter more here (§10), and how the claims in this guide are verified (§11) — are
written out in full.

## 0. Why this mode exists, and why the whole library is shaped by it

Freestanding mode is for firmware, kernels, bootloaders, hypervisors, and anywhere else that has no
operating system underneath it — no `malloc`, no `open`, no `printf`, often no `libc` at all.

The C standard has a name for this. A **hosted** implementation gives you the whole standard
library and starts your program at `main`. A **freestanding** implementation guarantees only a
handful of headers — `<stddef.h>`, `<stdint.h>`, `<limits.h>` and a few others — and is what a
compiler targeting bare metal gives you. `-ffreestanding` tells the compiler to assume exactly that.

Most C libraries cannot be used here at all, and the reason is rarely a big one. It is a `malloc`
buried three calls down, or a `printf` in an error path, or a global that is initialised by
something that never runs. One hidden dependency on the host is enough to make a library unusable
on a microcontroller.

**This is the constraint that produced most of this library's design decisions**, and it is worth
seeing that connection, because it explains choices that otherwise look like taste:

| Design decision | Read the earlier chapters as | And the reason is |
|---|---|---|
| The allocator is a parameter ([Ch 2](manual-02-allocation.md)) | "so you can swap the strategy" | There is no `malloc` here. An arena over a `static` array is the only allocator, and code that took one as a parameter already works. |
| Errors are returned values ([Ch 1](manual-01-foundation.md)) | "so you cannot ignore them" | There is no `errno` and nothing to unwind to. A return value is the only mechanism that exists. |
| Panics go through a hook ([Ch 1 §6](manual-01-foundation.md)) | "so you can override it" | There is no `abort()` and no `stderr`. The default traps because that is all it can portably do. |
| Syscalls live only in `platform/` | "separation of concerns" | It is the only directory that has to be replaced for a new target. Everything in `src/proven/` is portable by construction. |
| Views instead of C strings ([Ch 3](manual-03-strings-text.md)) | "so lengths cannot get lost" | `strlen` is libc. A view brings its length with it and needs nothing. |

In other words: freestanding support is not a feature bolted onto the side. The rest of the manual
has been describing a library built to survive here, and this guide is where you find out what
survives and what does not.

### What you give up

Everything that needs the operating system, and nothing else:

- **No heap.** `proven_heap_allocator` does not exist. You supply memory — a `static` array is
  usual — and put an arena or a pool over it.
- **No filesystem, no standard streams, no clock, no OS randomness.** `fs.h`, `sysio.h`, `mmap.h`,
  `time.h` and the OS entropy source are all hosted services. `proven_chacha_rng_t` still works if
  you seed it yourself, which is what `proven_random_set_source` is for.
- **No float formatting by default.** `PROVEN_FMT_NO_FLOAT` drops it, because the float path is
  the largest piece of code in the formatter and most firmware never prints a `double`. §8 says how
  to turn it back on.
- **No UTF-16 strings.** `PROVEN_NO_U16STR` is on; there is no Windows API here to talk to.

What you keep is the whole portable core: errors, views, checked arithmetic, alignment, arenas,
pools, buffers, strings, arrays, maps, lists, rings, sorting, searching, hashing, encoding,
coroutines, and integer formatting and scanning.

### What still bites you

The lifetime rules do not relax because the target got smaller — if anything they matter more,
because there is no operating system to notice a mistake and no allocator that will refuse to
reuse memory you still hold. §10 is short and is the section people skip.

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
src/proven/hash.c
src/proven/encode.c
src/proven/random.c
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

Do not add excluded hosted modules to a bare-metal build unless you also provide a real platform backend for the target.

## 3. Module availability

### How to read this table, and what "excluded" means

A module is excluded here for exactly one reason: it needs something only an operating system can
provide. It is not that the code is untested on small targets or that somebody has not got round to
it — `fs.h` needs a filesystem, `sysio.h` needs standard streams, `mmap.h` needs virtual memory,
`job.h` needs threads. There is nothing to port.

The interesting rows are the two that are neither available nor excluded:

- **`heap.h` is a stub.** `proven_heap_allocator()` still exists and still compiles, and it returns
  an allocator whose function pointers are null — one that `proven_alloc_is_valid` reports as
  invalid. It does not silently allocate from somewhere else and it does not fail to link. That
  choice matters: code written against the allocator trait keeps compiling for a bare-metal target,
  and the place it breaks is the one line that asked for a heap, at run time, with a check you can
  write. §4 shows what to pass instead.
- **`time.h` is limited.** The datetime formatting is pure arithmetic over a number you supply, so
  it compiles. What is missing is the number: reading a clock is a syscall, so `proven_time_now`
  has no backend here. Format timestamps you got from your own hardware timer.

Everything marked Available is the portable core, and "available" means the same tests that run on
a hosted build run against it — the freestanding profile is built and checked by `./nob freestanding`
on every release, not asserted in this table.

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
| `hash.h` | Available | FNV-1a, SipHash-2-4, CRC-32, SHA-256 — byte-exact, no OS dependency. |
| `encode.h` | Available | Hex and Base64 — pure computation, no OS. |
| `fmt.h` | Available without float | Current profile defines `PROVEN_FMT_NO_FLOAT`. |
| `scan.h` | Available | Scanner for memory views. |
| `time.h` | Limited | Core datetime formatting can compile; real PAL time is excluded. |
| `heap.h` | Stub | `proven_heap_allocator()` returns an invalid allocator. |
| `fs.h`, `stream.h`, `mmap.h`, `sysio.h`, `job.h` | Excluded | Require hosted PAL services. |
| `random.h` | Available | The generators and helpers are pure arithmetic. `proven_random_bytes` works here too, but only once you install an entropy source with `proven_random_set_source` — a board's TRNG, ring oscillator, or ADC noise floor. With none installed it returns **false** rather than falling back to a clock-seeded PRNG, which would look like success and be a security hole nothing reports. `proven_chacha_rng_seed_from_entropy` then turns the board's entropy into an endless cryptographic stream. |
| `coro.h` | Available | Macro-only stackless coroutine support. |
| `panic.h` | Available | Override for target-specific trap/reset behavior. |

## 4. Minimal static arena setup

There is no heap, so the memory comes from a static block and an arena is laid over
it. The arena does not own that block: `alloc` bumps an offset, `free` is a no-op,
and `reset` rewinds the whole thing at once.

This is the pattern the whole library was shaped to allow, and it is worth seeing why it works.
Every function that can allocate takes a `proven_allocator_t` as a parameter, so code written for a
hosted build against `proven_heap_allocator()` runs here unchanged once you hand it an arena
instead. Nothing in `u8str.h`, `array.h` or `map.h` knows or cares that the memory came from a
`static` array in `.bss` rather than from `malloc`.

Three decisions to make when you size that block, none of which the library can make for you:

- **How big.** The arena cannot grow, so its size is a hard limit you are choosing at compile time.
  Too small and allocations start returning `PROVEN_ERR_NOMEM`; too large and you have spent RAM
  the rest of the firmware needed. This is the trade you are being asked to make explicitly rather
  than discovering it as heap fragmentation months later.
- **When to reset.** An arena's whole advantage is freeing everything at once. The natural points
  are a loop iteration, a received packet, a command — anywhere a batch of work has a clear end.
  Everything allocated during that batch dies at the reset, so **nothing may outlive it**.
- **Alignment of the backing array.** `alignas(max_align_t)` on the declaration, as below. Without
  it the array may start at an address that cannot hold a `double`, and the arena has nothing to
  fix that with.

Wrong — a view built in the arena that outlives the reset:

```text
proven_u8str_view_t label = build_label(arena_alloc);   /* lives in the arena */
proven_arena_reset(&arena);
send(label);   /* wrong: those bytes are free space now, and the next allocation takes them */
```

On a hosted build this often survives long enough to look fine in testing. Here the next allocation
gets the same bytes immediately, so it does not.

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

Common mistake:

```text
proven_allocator_t heap = proven_heap_allocator();
heap.alloc_fn(heap.ctx, 64, 8); /* wrong: heap is invalid in PROVEN_FREESTANDING */
```

Correct:

```c
proven_allocator_t heap = proven_heap_allocator();
if (!proven_alloc_is_valid(heap)) {
    /* use an arena, pool, or target-provided allocator instead */
    proven_panic("no heap on this target");
}
```

## 5. Panic handler override

### Why installing one is not optional here

On a hosted system a panic that traps is survivable: the process dies, the operating system cleans
up, and something restarts it. On bare metal there is nothing underneath. A trap halts the core,
and whatever the device was doing — holding a motor at speed, keeping a radio link, driving a
heater — it is still doing when the CPU stops.

So the default handler is a placeholder for the one you must write, and what yours does is a
product decision rather than a programming one. The usual shapes:

- **Put the hardware in a safe state, then halt.** Motors off, outputs to a known level, then spin
  or wait for a debugger. Correct for anything with a physical actuator.
- **Record and reset.** Write a reason code to a register or a reserved RAM area that survives
  reset, then trigger the watchdog. The next boot reports why the last one ended.
- **Halt loudly.** Blink an LED in a recognisable pattern. On a board with no console this is the
  entire diagnostic channel, and it is worth more than it sounds.

The rule from [Chapter 1 §6](manual-01-foundation.md) applies with more force here: **the handler
must not return.** If it does, `proven_arena_alloc_or_panic` proceeds with a block that was never
allocated, and the failure moves from "the device stopped" to "the device is writing through a
null pointer".

Wrong — a handler that logs and returns:

```text
static void my_panic(const char *msg) {
    uart_write(msg);   /* wrong: this returns, and the caller uses a block it never got */
}
```

`proven_arena_alloc_or_panic()` and related panic paths call `proven_panic()`, which dispatches to the handler installed with `proven_set_panic_handler()`. The default handler traps.

The handler is a whole function, so this is a listing rather than a fragment:

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

### What is left, and why that is usually enough

The formatter is portable computation — it builds bytes in a destination you supply — so almost
all of it survives here. What is gone is one placeholder type: `double`.

That sounds like a big loss and rarely is. Correct float formatting means emitting the shortest
decimal that reads back as the same value, on every input including subnormals, and doing that
requires big-integer arithmetic and lookup tables. It is the largest single piece of code in the
formatter. Most firmware formats integers, string views, characters and pointers — a sensor
reading is a scaled integer, a status line is text — so the profile drops the float path by
default and the binary is smaller for it.

What remains: `{}` for integers of every width, views, characters, booleans and pointers; the
whole spec grammar (width, fill, alignment, base, sign); `PROVEN_ARG_OF` for your own types; and the
scanner in full, including float *parsing*, which does not carry the same weight.

If you do need floats on a small target, the switch is `PROVEN_FMT_NO_FLOAT` and §8a covers the
capacity knob that goes with it. Measure the size change before deciding — on a part with 32 KB of
flash it is not a rounding error.

Note the other consequence: **there is no `proven_println` here.** Formatting appends into a
`proven_u8str_t` or a buffer, and getting those bytes to a UART or an RTT channel is your platform
code, because a freestanding build has no standard output to assume.

Float formatting is disabled by `PROVEN_FMT_NO_FLOAT`. Integer and string-view formatting remain available.

Correct (`alloc` here is an arena allocator, as in section 4 - there is no heap):

```c
proven_result_u8str_t r = proven_u8str_create(alloc, 32);
if (proven_is_ok(r.err)) {
    proven_fmt_result_t f = proven_u8str_append_fmt_grow(alloc, &r.value,
                                                         "value={}", PROVEN_ARG(123));
    (void)f;
    proven_u8str_destroy(alloc, &r.value);
}
```

Wrong:

```text
proven_u8str_append_fmt_grow(alloc, &s, "{}", PROVEN_ARG(3.14));
/* wrong in the current freestanding profile: float args are excluded, so
   PROVEN_ARG has no _Generic association for double and this fails to compile */
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

### Why this is a link error rather than a run-time surprise

The hosted modules are not compiled into a freestanding build at all. Calling one is therefore a
**link error** — an undefined symbol, at build time, naming the function you should not have used.

That is the design working. The alternative, which many embedded libraries choose, is to provide
stubs that return an error at run time; then a call to `proven_fs_open` on a microcontroller
compiles, links, ships, and fails in the field on a path nobody exercised. Here it cannot get out
of the build.

The practical consequence for your own code: if a module is shared between a hosted tool and
firmware, the hosted-only calls need to be behind `#ifndef PROVEN_FREESTANDING`, and the link error
tells you exactly which ones you missed.

Do not call these in the current freestanding profile:

```text
proven_fs_open(...);
proven_println(...);
proven_env_get(...);
proven_mmap_create(...);
proven_job_system_init(...);
```

They require hosted PAL files that are intentionally excluded.

## 10. Lifetime rules still apply

### The section people skip, and why it costs more here

Freestanding does not relax any of the ownership rules in
[Chapter 0 §5](manual-00-start-here.md#5-the-five-contracts-you-will-meet-on-every-page). If anything it sharpens them, for three reasons that all
point the same way:

- **There is no allocator between you and the memory.** A dangling pointer into a heap block often
  survives for a while because the allocator has not handed that block out again. An arena hands the
  same bytes out on the very next allocation after a reset, so a stale view is overwritten
  immediately and deterministically.
- **There is nothing to catch you.** No MMU trap on a small target, no operating system to kill the
  process, no sanitizer in the field. A use-after-reset does not crash — it reads someone else's
  data and carries on, and the symptom appears somewhere unrelated.
- **The consequences are physical.** The wrong byte here can be a motor command or a radio packet.

The rules are the same ones you already know: a view is valid only while its owner is, an owned
object is destroyed exactly once with the allocator that made it, and caller-owned state structs
must not be copied. What changes is the margin for error, which is zero.

Freestanding does not relax container rules.

Wrong:

```text
int *p = PROVEN_ARRAY_GET_MUT(&arr, int, 0);
PROVEN_ARRAY_PUSH(&arr, int, 9);
*p = 1; /* wrong: push may have moved the array */
```

Wrong:

```text
proven_u8str_view_t key = make_stack_view();
PROVEN_MAP_SET_U8_BORROWED(&map, key, int, 1);
return; /* wrong if map survives after key bytes go out of scope */
```

## 11. Verification

### Why this guide is checked rather than believed

Everything above is a claim about what compiles and links without an operating system, and claims
like that rot quietly. One `#include <stdio.h>` added to a portable source file in an unrelated
commit and the freestanding profile is broken — on a host build nothing would notice, because
`stdio.h` is right there.

So the profile is built on every release rather than described. `./nob freestanding` compiles the
portable core with `-ffreestanding` and the profile's defines, links the checks below **statically**
on the build host, and runs them. `./nob cross` then compiles the same profile for real embedded
targets — Cortex-M and RISC-V among them — as a compile-only matrix.

The two checks are chosen for what they would catch:

- The **heap stub** check proves `proven_heap_allocator()` still exists and returns something
  `proven_alloc_is_valid` rejects. If somebody made it fail to link instead, trait-based code would
  stop compiling for bare metal — the failure §3 explains this design avoids.
- The **compile check** builds a representative program against the profile, which is what catches
  a hosted header sneaking into a portable file.

Neither runs on the actual hardware, and this guide does not claim otherwise: alignment faults,
endianness and timing need a board. What is verified is the part that can be, on every build,
rather than the whole thing on no build.

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
