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

```text
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

### How it expands (and the one rule that matters)

These macros are the classic Protothreads/Duff's-device trick. `BEGIN` opens a
`switch (state)`, each `YIELD`/`AWAIT` records `__LINE__` as the resume label and
`return`s, and `END` closes the switch. Conceptually:

```text
proven_i32 f(Ctx *c) {
    switch (c->coro.state) {       /* PROVEN_CORO_BEGIN */
    case 0:
        /* ... code before the first yield ... */
        c->coro.state = 42; return 0; case 42:;   /* PROVEN_CORO_YIELD on line 42 */
        /* ... code after the yield ... */
    }
    c->coro.state = -1; return 1;  /* PROVEN_CORO_END */
}
```

The function **returns** at each yield and is **re-entered from the top**, jumping
straight to the resume `case`. The consequence — the single rule to remember:

> **Local (stack) variables do NOT survive a yield.** Anything whose value must
> persist across a `YIELD`/`AWAIT` must live in the coroutine's own struct (or be
> `static`), because the locals are re-created on every call.

Other constraints that follow from the expansion: two coroutine macros must not
share a source line (they'd collide on `__LINE__`), and a `YIELD`/`AWAIT` must not
sit inside a `switch` of your own (it would land in the wrong `case`). The
coroutine has no stack, so it cannot yield from a helper function it calls — only
from its own body.

#### Counter-example — a local across a yield

```text
static proven_i32 bad(Ctx *c) {
    PROVEN_CORO_BEGIN(&c->coro);
    int i = 0;                       /* WRONG: a local */
    while (i < 3) {
        i += 1;
        PROVEN_CORO_YIELD(&c->coro); /* on re-entry `i` is re-initialized to 0 */
    }
    PROVEN_CORO_END(&c->coro);       /* loop never ends: infinite yields */
}
/* RIGHT: put `i` in Ctx (like `value` in the Counter example below). */
```

Example (a sketch: a coroutine is a whole function, so it cannot be shown as a
statement fragment - the compiled version of this shape is the worked example in
[section 7](#7-examples-and-misuse-cases), `manual/examples/ex_06_coro.c`):

```text
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

```text
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

### Concurrency model

The queue is a fixed-size ring buffer of `max_queue_capacity` slots (must be a
power of two, so a slot index is `seq & (capacity - 1)`). Producers and consumers
do not lock the whole queue; they coordinate with **atomic sequence counters**:

- A producer (`proven_job_submit`) atomically claims the next enqueue position. If
  claiming would overrun the slowest un-consumed slot, the queue is full and submit
  returns `false` — it never blocks and never overwrites pending work.
- A consumer (a worker thread, or your own thread via `proven_job_execute_one`)
  atomically claims the next dequeue position, reads the `routine`/`arg`, marks the
  slot reusable, and runs the routine outside the claim.

Because submit and execute are MPMC-safe, *many* threads may submit and *many* may
consume at once. What the library does **not** do for you: it does not synchronize
the *data your jobs touch*. Two jobs that write the same variable still need their
own locking/atomics — the queue only orders the handoff, not the work.

Lifecycle state machine (drive it in this order):

```
init  --->  running  --(close)-->  closed  --(destroy: drain + join)-->  freed
```

- `init` reserves `num_workers` OS threads and the slot buffer up front.
- `close` flips a flag so every later `submit` returns `false`; already-queued and
  in-flight jobs still run.
- `destroy` closes if needed, then **blocks the calling thread until the queue is
  empty and every worker has joined**, then frees everything.

**Memory visibility.** A worker joining inside `destroy` is a synchronization
point: every memory effect of every job is visible to the thread that called
`destroy` *after `destroy` returns*. So the safe pattern for collecting results is
"submit all → close → destroy → read results". Reading a job's output from another
thread *before* that join needs your own synchronization.

#### Counter-examples

```text
/* WRONG: capacity must be a power of two. */
proven_job_system_init(alloc, 4, 100, &sys);   /* 100 is not 2^n */

/* WRONG: reading a result before the join makes it visible. */
int total = 0;
(void)proven_job_submit(sys, accumulate, &total);
proven_println("total = {}", PROVEN_ARG(total));  /* data race: job may still be running */
/* RIGHT: */
proven_job_system_close(sys);
proven_job_system_destroy(sys);                   /* joins workers -> writes now visible */
proven_println("total = {}", PROVEN_ARG(total));

/* WRONG: ignoring the submit result drops work silently when the ring is full. */
proven_job_submit(sys, work, arg);   /* [[nodiscard]]: a false return means NOT queued */
```

Example (a sketch, because the job routine has to be a function of its own; the
compiled version, with the atomics a shared counter really needs, is the worked
example in [section 7](#7-examples-and-misuse-cases),
`manual/examples/ex_06_job.c`):

```text
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

The lifecycle itself, with no routine in sight, is ordinary statement code:

```c
proven_job_sys_t *sys = NULL;
proven_err_t e = proven_job_system_init(alloc, 2, 64, &sys);  /* capacity: power of two */
if (proven_is_ok(e)) {
    /* ... submit work here, from this thread or from producers you own ... */
    proven_job_system_close(sys);     /* every later submit now returns false */
    proven_job_system_destroy(sys);   /* drains the queue, joins the workers */
}
```

## 3. Alias layer

`include/proven/alias_xcv.h` provides a shorter optional alias prefix. It maps canonical `proven_` and `PROVEN_` names to `xcv_` and `XCV_` names.

Include it after the canonical headers:

```c
#include "proven.h"
#include "proven/alias_xcv.h"

xcv_allocator_t heap = xcv_heap_allocator();
(void)heap;
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

- `heap`: compiled, but `proven_heap_allocator()` returns an invalid zero allocator.
- `u16str`: excluded by `PROVEN_NO_U16STR` in the current freestanding build.
- `time`: partial. `src/proven/time.c` is compiled, so `proven_time_breakdown()` and
  the datetime formatters are available, but the clock backend
  (`platform/proven_sys_time.c`) is not - so `proven_time_now()`,
  `proven_time_now_datetime()`, and `proven_time_sleep()` have no implementation to
  link against.
- `fs`, `sysio`, `mmap`, `job`: hosted/PAL services excluded from the current freestanding subset.

See `manual-freestanding.md` for the exact source list and command examples.

## 6. Cross compilation

The build driver command is:

```sh
./nob cross -build-root /home/user/work/build/proven_c_lib
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

```text
PROVEN_CORO_YIELD(&c->coro); PROVEN_CORO_YIELD(&c->coro);
/* wrong: both use the same __LINE__ value */
```

Correct:

```text
PROVEN_CORO_YIELD(&c->coro);
PROVEN_CORO_YIELD(&c->coro);
```

### Coroutine local variables are ordinary locals

A stackless coroutine returns to the caller. Any local variable whose value must survive across yields should live in the coroutine state object, not as an automatic local inside the coroutine function.

Wrong:

```text
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

```text
typedef struct Task {
proven_coro_t coro;
int temporary;
} Task;
```

### Job destroy must not race with submitters

Wrong:

```text
/* thread A */
proven_job_system_destroy(sys);

/* thread B at the same time */
proven_job_submit(sys, work, arg); /* wrong: external synchronization missing */
```

Correct:

```text
stop_producer_threads();
join_producer_threads();
proven_job_system_close(sys);
proven_job_system_destroy(sys);
```

### Job argument lifetime

Wrong:

```text
void submit_bad(proven_job_sys_t *sys) {
int value = 10;
proven_job_submit(sys, work, &value);
} /* wrong: value may be dead before work runs */
```

Correct:

```text
JobData *data = allocate_job_data();
proven_job_submit(sys, work_and_free_data, data);
```

### Alias layer should not hide canonical docs

Wrong:

```text
/* Document only xcv_map_t and forget proven_map_t. */
```

Correct: document canonical `proven_` API first, then mention aliases as optional local spelling.

### Worked example: the bounded job system

Compiled and run by the test suite. Note the ordering the contract requires: submit, then close, then destroy. `proven_job_system_destroy` must not race with `proven_job_submit`.

<!-- example: manual/examples/ex_06_job.c -->
```c
#include <stdatomic.h>

/*
 * The job system: worker threads plus a bounded lock-free queue. It orders the
 * *handoff* of work - it does not synchronize the data the work touches. That is
 * why the counter below is an atomic and not a plain int: two jobs incrementing
 * the same variable is a data race unless the caller says otherwise.
 *
 * The lifecycle is a straight line, and it is not optional:
 *
 *     init -> submit... -> close -> destroy
 *
 * destroy must not race with submit. Nothing in the library enforces that; the
 * caller has to stop its producers first. Here there is only one producer - this
 * thread - so "stop the producers" means "finish the submit loop before closing".
 */

#define JOB_COUNT 64

static void increment(void *arg) {
    atomic_int *counter = arg;
    /* relaxed is enough: we only need the total to be right, not to order anything
     * against it. The join inside destroy is what publishes the result to us. */
    atomic_fetch_add_explicit(counter, 1, memory_order_relaxed);
}

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    proven_job_sys_t *sys = NULL;
    /* Queue capacity must be a power of two - the ring maps a sequence number to a
     * slot with a mask. Sized above JOB_COUNT so a submit cannot find the ring
     * full even if every worker is still starting up. */
    proven_err_t err = proven_job_system_init(alloc, 4, 128, &sys);
    EXAMPLE_REQUIRE(proven_is_ok(err), "starting a job system with 4 workers should succeed");
    if (!proven_is_ok(err)) return 1;

    /* Lives until after destroy: a job's arg must outlive the job, and jobs run
     * until destroy has drained the queue. */
    atomic_int counter = 0;

    proven_size_t submitted = 0;
    for (proven_size_t i = 0; i < JOB_COUNT; ++i) {
        /* submit returns false when the ring is full or the system is closed - it
         * never blocks and never drops work silently. Ignoring the answer is how
         * you lose jobs, which is why it is [[nodiscard]]. */
        if (!proven_job_submit(sys, increment, &counter)) {
            /* A real caller would back off and retry, or run the job inline with
             * proven_job_execute_one. Here a full ring means the sizing above is
             * wrong, so say so rather than paper over it. */
            EXAMPLE_REQUIRE(false, "the queue was sized to hold every job");
            break;
        }
        ++submitted;
    }

    /* This thread is the only producer, and it is done submitting - so it is safe
     * to close. close makes every later submit fail; jobs already queued still run. */
    proven_job_system_close(sys);

    /* destroy blocks until the queue is empty and every worker has been joined.
     * That join is the synchronization point: after destroy returns, every memory
     * effect of every job is visible to this thread. Reading `counter` before this
     * line would be reading a value the workers are still writing. */
    proven_job_system_destroy(sys);

    int ran = atomic_load(&counter);
    EXAMPLE_REQUIRE(submitted == JOB_COUNT, "every job should have been accepted");
    EXAMPLE_REQUIRE(ran == (int)submitted, "every submitted job should have run exactly once");

    printf("submitted %zu jobs, %d ran\n", (size_t)submitted, ran);

    return EXAMPLE_OK();
}
```

### Worked example: a stackless coroutine

Compiled and run by the test suite. The rule that catches everyone: locals do not survive a yield, so every piece of state the coroutine needs across a suspension has to live in its struct.

<!-- example: manual/examples/ex_06_coro.c -->
```c
/*
 * A stackless coroutine is a switch statement in disguise: BEGIN opens a
 * switch on the saved state, each YIELD records __LINE__ as a resume label and
 * *returns*, and the next call re-enters the function from the top and jumps
 * straight back to that label.
 *
 * Everything that follows comes from that one fact:
 *
 *   - Locals do NOT survive a yield. The function returned; its stack frame is
 *     gone. Anything that must persist lives in the coroutine's own struct - which
 *     is what `value` and `remaining` are doing below.
 *   - Two coroutine macros must not share a source line (they would collide on
 *     __LINE__).
 *   - It cannot yield from a helper it calls: there is no stack to suspend.
 *
 * The payoff is that a suspended coroutine costs exactly its struct - four bytes
 * of state plus whatever you put next to it - and no thread, no stack, no context
 * switch.
 */

typedef struct {
    proven_coro_t coro;
    /* The generator's state. These would be `int i` locals in a normal loop; here
     * they have to be fields, or they would be reset to their initial values on
     * every resume and the loop would never end. */
    int value;
    int remaining;
} squares_t;

/* A coroutine returns proven_i32: 0 = suspended (call me again), 1 = done. */
static proven_i32 squares_next(squares_t *g) {
    PROVEN_CORO_BEGIN(&g->coro);

    g->remaining = 5;
    g->value = 1;

    while (g->remaining > 0) {
        g->value = g->value * g->value;
        PROVEN_CORO_YIELD(&g->coro);      /* the caller reads g->value here */
        g->value = g->value + 1;          /* resumes exactly on this line */
        g->remaining -= 1;
    }

    PROVEN_CORO_END(&g->coro);
}

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    /* The coroutine owns no memory, so there is nothing to destroy - but the values
     * it produces have to go somewhere, and that string does have an owner. */
    proven_result_u8str_t out = proven_u8str_create(alloc, 32);
    EXAMPLE_REQUIRE(proven_is_ok(out.err), "creating the output string should succeed");
    if (!proven_is_ok(out.err)) return 1;

    squares_t gen = {0};
    PROVEN_CORO_INIT(&gen.coro);   /* unconditional, exactly once, before the first call */

    int produced = 0;
    int last = 0;

    /* Drive it to completion. squares_next returns 1 on the call that runs off the
     * end of the body - that call produces no value, so the loop body only runs
     * while it returned 0. */
    while (!squares_next(&gen)) {
        proven_fmt_result_t r = proven_u8str_append_fmt_grow(alloc, &out.value, "{} ",
                                                             PROVEN_ARG(gen.value));
        EXAMPLE_REQUIRE(PROVEN_FMT_IS_OK(r), "appending a generated value should succeed");
        last = gen.value;
        ++produced;
    }

    /* Done is sticky: the state is -1 and stays there. Calling it again would just
     * return 1 without re-running the body. */
    EXAMPLE_REQUIRE(PROVEN_CORO_IS_DONE(&gen.coro), "the generator should have finished");
    EXAMPLE_REQUIRE(squares_next(&gen) == 1, "a finished coroutine stays finished");

    /* 1, then (1+1)^2 = 4, then (4+1)^2 = 25, then 676, then 458329. */
    EXAMPLE_REQUIRE(produced == 5, "the generator yields once per iteration");
    EXAMPLE_REQUIRE(last == 458329, "the state carried across every yield");
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_as_view(&out.value),
                                         PROVEN_LIT("1 4 25 676 458329 ")),
                    "the generated sequence should be exactly this");

    printf("squares: %s\n", proven_u8str_as_cstr(&out.value));

    proven_u8str_destroy(alloc, &out.value);
    return EXAMPLE_OK();
}
```
