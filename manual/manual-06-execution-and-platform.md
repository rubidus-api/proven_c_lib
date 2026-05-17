     1|# Chapter 6: Execution, Aliases, PAL, Freestanding, and Cross Builds
     2|
     3|This chapter covers `coro.h`, `job.h`, `alias_xcv.h`, PAL contracts, freestanding mode, and platform build notes.
     4|
     5|## Table of contents
     6|
     7|1. [Stackless coroutines](#1-stackless-coroutines)
     8|2. [Job system](#2-job-system)
     9|3. [Alias layer](#3-alias-layer)
    10|4. [PAL contract](#4-pal-contract)
    11|5. [Freestanding subset](#5-freestanding-subset)
    12|6. [Cross compilation](#6-cross-compilation)
    13|7. [Examples and misuse cases](#7-examples-and-misuse-cases)
    14|
    15|## 1. Stackless coroutines
    16|
    17|The coroutine macros implement stackless state machines using `__LINE__` labels inside a switch. A coroutine function should return `proven_i32`: `0` means yielded or waiting, `1` means done.
    18|
    19|### Structure
    20|
    21|```c
    22|typedef struct {
    23|    proven_i32 state;
    24|} proven_coro_t;
    25|```
    26|
    27|### Macros
    28|
    29|| Macro | Intent |
    30||---|---|
    31|| `PROVEN_CORO_INIT(c)` | Set state to zero before first use. |
    32|| `PROVEN_CORO_BEGIN(c)` | Begin coroutine switch block. |
    33|| `PROVEN_CORO_YIELD(c)` | Save resume point and return 0. |
    34|| `PROVEN_CORO_AWAIT(c, cond)` | Return 0 until `cond` is true, then continue. |
    35|| `PROVEN_CORO_END(c)` | Mark done and return 1. |
    36|| `PROVEN_CORO_IS_DONE(c)` | Check whether state is -1. |
    37|
    38|Example:
    39|
    40|```c
    41|typedef struct Counter {
    42|    proven_coro_t coro;
    43|    int value;
    44|} Counter;
    45|
    46|static proven_i32 counter_next(Counter *c) {
    47|    PROVEN_CORO_BEGIN(&c->coro);
    48|    c->value = 0;
    49|    while (c->value < 3) {
    50|        c->value += 1;
    51|        PROVEN_CORO_YIELD(&c->coro);
    52|    }
    53|    PROVEN_CORO_END(&c->coro);
    54|}
    55|
    56|Counter c = {0};
    57|PROVEN_CORO_INIT(&c.coro);
    58|while (!counter_next(&c)) {
    59|    use_value(c.value);
    60|}
    61|```
    62|
    63|## 2. Job system
    64|
    65|The job system owns worker threads and a bounded MPMC-style queue. Producers submit `routine(arg)` work items. Workers execute queued jobs until the system is closed and drained.
    66|
    67|### Structures
    68|
    69|```c
    70|typedef struct {
    71|    void (*routine)(void *arg);
    72|    void *arg;
    73|} proven_job_t;
    74|
    75|typedef struct proven_job_sys proven_job_sys_t;
    76|```
    77|
    78|`proven_job_sys_t` is opaque.
    79|
    80|### Functions
    81|
    82|| API | Intent | Return |
    83||---|---|---|
    84|| `proven_job_system_init(alloc, num_workers, max_queue_capacity, out_sys)` | Allocate and start a job system. Queue capacity must be a power of two. | `proven_err_t`. |
    85|| `proven_job_system_close(sys)` | Stop accepting new jobs. | void. |
    86|| `proven_job_system_destroy(sys)` | Close if needed, drain queue, join workers, free resources. | void. |
    87|| `proven_job_submit(sys, routine, arg)` | Submit one job. Thread-safe with other submitters. | true if queued, false if full or closed. |
    88|| `proven_job_execute_one(sys)` | Let the calling thread execute one available job. | true if a job ran. |
    89|
    90|Important constraints:
    91|
    92|- `proven_job_system_destroy()` must not race with producer threads still calling `proven_job_submit()`.
    93|- Callers should externally synchronize producer shutdown, then close/destroy.
    94|- Queue sequence counters assume they do not wrap beyond signed pointer-difference range during one job-system lifetime.
    95|
    96|Example:
    97|
    98|```c
    99|static void increment(void *arg) {
   100|    int *p = arg;
   101|    *p += 1;
   102|}
   103|
   104|proven_job_sys_t *sys = NULL;
   105|proven_err_t e = proven_job_system_init(alloc, 2, 64, &sys);
   106|if (!proven_is_ok(e)) return e;
   107|
   108|int value = 0;
   109|if (!proven_job_submit(sys, increment, &value)) {
   110|    proven_job_system_close(sys);
   111|    proven_job_system_destroy(sys);
   112|    return PROVEN_ERR_BUSY;
   113|}
   114|
   115|proven_job_system_close(sys);
   116|proven_job_system_destroy(sys);
   117|```
   118|
   119|## 3. Alias layer
   120|
   121|`include/proven/alias_xcv.h` provides a shorter optional alias prefix. It maps canonical `proven_` and `PROVEN_` names to `xcv_` and `XCV_` names.
   122|
   123|Include it after the canonical headers:
   124|
   125|```c
   126|#include "proven.h"
   127|#include "proven/alias_xcv.h"
   128|
   129|xcv_allocator_t alloc = xcv_heap_allocator();
   130|xcv_println("{}", XCV_ARG(123));
   131|```
   132|
   133|Alias design rules:
   134|
   135|- Aliases are preprocessor conveniences, not a separate ABI.
   136|- The canonical API remains the source of truth.
   137|- Treat `alias_xcv.h` as a template if a project wants its own shorter local prefix.
   138|- Keep alias tests synchronized when public API names are added or removed.
   139|
   140|Alias macro families:
   141|
   142|- Formatting: `XCV_ARG`, `XCV_ARG_CSTR`, `XCV_ARG_CSTR_N`, `XCV_FMT_IS_OK`.
   143|- Containers: `XCV_ARRAY_*`, `XCV_RING_*`, `XCV_MAP_*`, `XCV_LIST_*`.
   144|- Errors and arithmetic: `XCV_ERR_*`, `XCV_IS_OK`, `XCV_CKD_*`.
   145|- Coroutines: `XCV_CORO_*`.
   146|- Filesystem and sysio: `xcv_fs_*`, `xcv_print*`, `xcv_scan*` aliases.
   147|- Types and allocators: `xcv_*_t` type aliases and allocator aliases.
   148|
   149|For an exhaustive source-grounded alias table, see [Chapter 7: Alias Index](manual-07-alias-xcv-index.md).
   150|
   151|## 4. PAL contract
   152|
   153|The Platform Abstraction Layer lives under `platform/`. It bridges portable library code to OS services.
   154|
   155|PAL areas:
   156|
   157|- `proven_sys_mem`: heap allocation backend.
   158|- `proven_sys_fs`: files, directories, paths, links, permissions, locks.
   159|- `proven_sys_time`: clock, sleep, time breakdown.
   160|- `proven_sys_env`: environment variables.
   161|- `proven_sys_thread`: threads and synchronization for the job system.
   162|- `proven_sys_io`: standard stream I/O.
   163|- `proven_sys_math`: math helpers where needed.
   164|
   165|Public application code should prefer high-level APIs:
   166|
   167|- `proven_fs_*` for filesystem operations.
   168|- `proven_sysio_*` and print/scan macros for console I/O.
   169|- `proven_time_*` for time.
   170|- `proven_job_*` for threaded jobs.
   171|
   172|PAL internal exception: thread lifecycle code may need internal platform allocation to keep opaque OS metadata alive across entry points. That exception stays inside PAL code and should not leak into core container APIs.
   173|
   174|## 5. Freestanding subset
   175|
   176|`PROVEN_FREESTANDING` builds a reduced subset for OS-free or libc-minimal targets. The current freestanding build also defines:
   177|
   178|```sh
   179|-DPROVEN_FREESTANDING -DPROVEN_FMT_NO_FLOAT -DPROVEN_NO_U16STR -ffreestanding
   180|```
   181|
   182|Available modules in the current freestanding configuration:
   183|
   184|- `types`
   185|- `error`
   186|- `memory`
   187|- `align`
   188|- `allocator`
   189|- `arena`
   190|- `pool`
   191|- `buffer`
   192|- `array`
   193|- `list`
   194|- `ring`
   195|- `map`
   196|- `algorithm`
   197|- `u8str`
   198|- `coro`
   199|- `scan`
   200|- `fmt` without floating-point formatting
   201|- `panic`
   202|
   203|Excluded or stubbed modules:
   204|
   205|- `heap`: returns an invalid zero allocator.
   206|- `u16str`: excluded by `PROVEN_NO_U16STR` in the current freestanding build.
   207|- `fs`, `sysio`, `mmap`, `time`, `job`: hosted/PAL services excluded from the current freestanding subset.
   208|
   209|See `manual-freestanding.md` for the exact source list and command examples.
   210|
   211|## 6. Cross compilation
   212|
   213|The build driver command is:
   214|
   215|```sh
   216|./nob cross -build-root /home/user/work/build/proven_c_lib
   217|```
   218|
   219|Current target categories:
   220|
   221|- Native GCC hosted.
   222|- Native Clang hosted when available.
   223|- Linux AArch64 hosted compile-only.
   224|- Linux ARM hard-float hosted compile-only.
   225|- Linux i686 hosted compile-only through `i686-linux-gnu-gcc` or `gcc -m32`.
   226|- Windows x86_64 and i686 WinAPI compile-only through MinGW.
   227|- ARM Cortex-M freestanding compile-only.
   228|- RISC-V ELF freestanding compile-only.
   229|
   230|Rules:
   231|
   232|- Missing optional compilers are skipped.
   233|- Real compile errors fail the command.
   234|- Cross compilation is compile-only; it is not runtime verification.
   235|- Runtime behavior still needs a target runner, emulator, device, or OS environment.
   236|
   237|## 7. Examples and misuse cases
   238|
   239|### Coroutine macros must not share one source line
   240|
   241|Wrong:
   242|
   243|```c
   244|PROVEN_CORO_YIELD(&c->coro); PROVEN_CORO_YIELD(&c->coro);
   245|/* wrong: both use the same __LINE__ value */
   246|```
   247|
   248|Correct:
   249|
   250|```c
   251|PROVEN_CORO_YIELD(&c->coro);
   252|PROVEN_CORO_YIELD(&c->coro);
   253|```
   254|
   255|### Coroutine local variables are ordinary locals
   256|
   257|A stackless coroutine returns to the caller. Any local variable whose value must survive across yields should live in the coroutine state object, not as an automatic local inside the coroutine function.
   258|
   259|Wrong:
   260|
   261|```c
   262|static proven_i32 next(Task *t) {
   263|    int temporary = 0;
   264|    PROVEN_CORO_BEGIN(&t->coro);
   265|    temporary = 42;
   266|    PROVEN_CORO_YIELD(&t->coro);
   267|    use_int(temporary); /* wrong assumption: ordinary local lifetime/state is not persistent */
   268|    PROVEN_CORO_END(&t->coro);
   269|}
   270|```
   271|
   272|Correct:
   273|
   274|```c
   275|typedef struct Task {
   276|    proven_coro_t coro;
   277|    int temporary;
   278|} Task;
   279|```
   280|
   281|### Job destroy must not race with submitters
   282|
   283|Wrong:
   284|
   285|```c
   286|/* thread A */
   287|proven_job_system_destroy(sys);
   288|
   289|/* thread B at the same time */
   290|proven_job_submit(sys, work, arg); /* wrong: external synchronization missing */
   291|```
   292|
   293|Correct:
   294|
   295|```c
   296|stop_producer_threads();
   297|join_producer_threads();
   298|proven_job_system_close(sys);
   299|proven_job_system_destroy(sys);
   300|```
   301|
   302|### Job argument lifetime
   303|
   304|Wrong:
   305|
   306|```c
   307|void submit_bad(proven_job_sys_t *sys) {
   308|    int value = 10;
   309|    proven_job_submit(sys, work, &value);
   310|} /* wrong: value may be dead before work runs */
   311|```
   312|
   313|Correct:
   314|
   315|```c
   316|JobData *data = allocate_job_data();
   317|proven_job_submit(sys, work_and_free_data, data);
   318|```
   319|
   320|### Alias layer should not hide canonical docs
   321|
   322|Wrong:
   323|
   324|```c
   325|/* Document only xcv_map_t and forget proven_map_t. */
   326|```
   327|
   328|Correct: document canonical `proven_` API first, then mention aliases as optional local spelling.
   329|