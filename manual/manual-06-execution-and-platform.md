# Chapter 6: Execution, Aliases, PAL, Freestanding, and Cross Builds

This chapter covers `coro.h`, `job.h`, `alias_xcv.h`, PAL contracts, freestanding mode, and platform build notes.

## Table of contents

1. [Stackless coroutines](#1-stackless-coroutines)
2. [Job system](#2-job-system)
3. [Alias layer](#3-alias-layer)
4. [PAL contract](#4-pal-contract)
5. [Freestanding subset](#5-freestanding-subset)
6. [Cross compilation](#6-cross-compilation)
7. [Examples and misuse cases](#7-examples-and-misuse-cases)

## 1. Stackless coroutines

The coroutine macros implement stackless state machines using `__LINE__` labels inside a switch. A coroutine function should return `proven_i32`: `0` means yielded or waiting, `1` means done.

### Structure

```c
typedef struct {
    proven_i32 state;
} proven_coro_t;
```

### Macros

| Macro | Intent |
|---|---|
| `PROVEN_CORO_INIT(c)` | Set state to zero before first use. |
| `PROVEN_CORO_BEGIN(c)` | Begin coroutine switch block. |
| `PROVEN_CORO_YIELD(c)` | Save resume point and return 0. |
| `PROVEN_CORO_AWAIT(c, cond)` | Return 0 until `cond` is true, then continue. |
| `PROVEN_CORO_END(c)` | Mark done and return 1. |
| `PROVEN_CORO_IS_DONE(c)` | Check whether state is -1. |

Example:

```c
typedef struct Counter {
    proven_coro_t coro;
    int value;
} Counter;

static proven_i32 counter_next(Counter *c) {
    PROVEN_CORO_BEGIN(&c->coro);
    c->value = 0;
    while (c->value < 3) {
        c->value += 1;
        PROVEN_CORO_YIELD(&c->coro);
    }
    PROVEN_CORO_END(&c->coro);
}

Counter c = {0};
PROVEN_CORO_INIT(&c.coro);
while (!counter_next(&c)) {
    use_value(c.value);
}
```

## 2. Job system

The job system owns worker threads and a bounded MPMC-style queue. Producers submit `routine(arg)` work items. Workers execute queued jobs until the system is closed and drained.

### Structures

```c
typedef struct {
    void (*routine)(void *arg);
    void *arg;
} proven_job_t;

typedef struct proven_job_sys proven_job_sys_t;
```

`proven_job_sys_t` is opaque.

### Functions

| API | Intent | Return |
|---|---|---|
| `proven_job_system_init(alloc, num_workers, max_queue_capacity, out_sys)` | Allocate and start a job system. Queue capacity must be a power of two. | `proven_err_t`. |
| `proven_job_system_close(sys)` | Stop accepting new jobs. | void. |
| `proven_job_system_destroy(sys)` | Close if needed, drain queue, join workers, free resources. | void. |
| `proven_job_submit(sys, routine, arg)` | Submit one job. Thread-safe with other submitters. | true if queued, false if full or closed. |
| `proven_job_execute_one(sys)` | Let the calling thread execute one available job. | true if a job ran. |

Important constraints:

- `proven_job_system_destroy()` must not race with producer threads still calling `proven_job_submit()`.
- Callers should externally synchronize producer shutdown, then close/destroy.
- Queue sequence counters assume they do not wrap beyond signed pointer-difference range during one job-system lifetime.

Example:

```c
static void increment(void *arg) {
    int *p = arg;
    *p += 1;
}

proven_job_sys_t *sys = NULL;
proven_err_t e = proven_job_system_init(alloc, 2, 64, &sys);
if (!proven_is_ok(e)) return e;

int value = 0;
if (!proven_job_submit(sys, increment, &value)) {
    proven_job_system_close(sys);
    proven_job_system_destroy(sys);
    return PROVEN_ERR_BUSY;
}

proven_job_system_close(sys);
proven_job_system_destroy(sys);
```

## 3. Alias layer

`include/proven/alias_xcv.h` provides a shorter optional alias prefix. It maps canonical `proven_` and `PROVEN_` names to `xcv_` and `XCV_` names.

Include it after the canonical headers:

```c
#include "proven.h"
#include "proven/alias_xcv.h"

xcv_allocator_t alloc = xcv_heap_allocator();
xcv_println("{}", XCV_ARG(123));
```

Alias design rules:

- Aliases are preprocessor conveniences, not a separate ABI.
- The canonical API remains the source of truth.
- Treat `alias_xcv.h` as a template if a project wants its own shorter local prefix.
- Keep alias tests synchronized when public API names are added or removed.

Alias macro families:

- Formatting: `XCV_ARG`, `XCV_ARG_CSTR`, `XCV_ARG_CSTR_N`, `XCV_FMT_IS_OK`.
- Containers: `XCV_ARRAY_*`, `XCV_RING_*`, `XCV_MAP_*`, `XCV_LIST_*`.
- Errors and arithmetic: `XCV_ERR_*`, `XCV_IS_OK`, `XCV_CKD_*`.
- Coroutines: `XCV_CORO_*`.
- Filesystem and sysio: `xcv_fs_*`, `xcv_print*`, `xcv_scan*` aliases.
- Types and allocators: `xcv_*_t` type aliases and allocator aliases.

For an exhaustive source-grounded alias table, see [Chapter 7: Alias Index](manual-07-alias-xcv-index.md).

## 4. PAL contract

The Platform Abstraction Layer lives under `platform/`. It bridges portable library code to OS services.

PAL areas:

- `proven_sys_mem`: heap allocation backend.
- `proven_sys_fs`: files, directories, paths, links, permissions, locks.
- `proven_sys_time`: clock, sleep, time breakdown.
- `proven_sys_env`: environment variables.
- `proven_sys_thread`: threads and synchronization for the job system.
- `proven_sys_io`: standard stream I/O.
- `proven_sys_math`: math helpers where needed.

Public application code should prefer high-level APIs:

- `proven_fs_*` for filesystem operations.
- `proven_sysio_*` and print/scan macros for console I/O.
- `proven_time_*` for time.
- `proven_job_*` for threaded jobs.

PAL internal exception: thread lifecycle code may need internal platform allocation to keep opaque OS metadata alive across entry points. That exception stays inside PAL code and should not leak into core container APIs.

## 5. Freestanding subset

`PROVEN_FREESTANDING` builds a reduced subset for OS-free or libc-minimal targets. The current freestanding build also defines:

```sh
-DPROVEN_FREESTANDING -DPROVEN_FMT_NO_FLOAT -DPROVEN_NO_U16STR -ffreestanding
```

Available modules in the current freestanding configuration:

- `types`
- `error`
- `memory`
- `align`
- `allocator`
- `arena`
- `pool`
- `buffer`
- `array`
- `list`
- `ring`
- `map`
- `algorithm`
- `u8str`
- `coro`
- `scan`
- `fmt` without floating-point formatting
- `panic`

Excluded or stubbed modules:

- `heap`: returns an invalid zero allocator.
- `u16str`: excluded by `PROVEN_NO_U16STR` in the current freestanding build.
- `fs`, `sysio`, `mmap`, `time`, `job`: hosted/PAL services excluded from the current freestanding subset.

See `manual/FREESTANDING.md` for the exact source list and command examples.

## 6. Cross compilation

The build driver command is:

```sh
./nob cross -build-root /mnt/ai-share/build/proven
```

Current target categories:

- Native GCC hosted.
- Native Clang hosted when available.
- Linux AArch64 hosted compile-only.
- Linux ARM hard-float hosted compile-only.
- Linux i686 hosted compile-only through `i686-linux-gnu-gcc` or `gcc -m32`.
- Windows x86_64 and i686 WinAPI compile-only through MinGW.
- ARM Cortex-M freestanding compile-only.
- RISC-V ELF freestanding compile-only.

Rules:

- Missing optional compilers are skipped.
- Real compile errors fail the command.
- Cross compilation is compile-only; it is not runtime verification.
- Runtime behavior still needs a target runner, emulator, device, or OS environment.

## 7. Examples and misuse cases

### Coroutine macros must not share one source line

Wrong:

```c
PROVEN_CORO_YIELD(&c->coro); PROVEN_CORO_YIELD(&c->coro);
/* wrong: both use the same __LINE__ value */
```

Correct:

```c
PROVEN_CORO_YIELD(&c->coro);
PROVEN_CORO_YIELD(&c->coro);
```

### Coroutine local variables are ordinary locals

A stackless coroutine returns to the caller. Any local variable whose value must survive across yields should live in the coroutine state object, not as an automatic local inside the coroutine function.

Wrong:

```c
static proven_i32 next(Task *t) {
    int temporary = 0;
    PROVEN_CORO_BEGIN(&t->coro);
    temporary = 42;
    PROVEN_CORO_YIELD(&t->coro);
    use_int(temporary); /* wrong assumption: ordinary local lifetime/state is not persistent */
    PROVEN_CORO_END(&t->coro);
}
```

Correct:

```c
typedef struct Task {
    proven_coro_t coro;
    int temporary;
} Task;
```

### Job destroy must not race with submitters

Wrong:

```c
/* thread A */
proven_job_system_destroy(sys);

/* thread B at the same time */
proven_job_submit(sys, work, arg); /* wrong: external synchronization missing */
```

Correct:

```c
stop_producer_threads();
join_producer_threads();
proven_job_system_close(sys);
proven_job_system_destroy(sys);
```

### Job argument lifetime

Wrong:

```c
void submit_bad(proven_job_sys_t *sys) {
    int value = 10;
    proven_job_submit(sys, work, &value);
} /* wrong: value may be dead before work runs */
```

Correct:

```c
JobData *data = allocate_job_data();
proven_job_submit(sys, work_and_free_data, data);
```

### Alias layer should not hide canonical docs

Wrong:

```c
/* Document only xcv_map_t and forget proven_map_t. */
```

Correct: document canonical `proven_` API first, then mention aliases as optional local spelling.
