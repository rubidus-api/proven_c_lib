     1|# Proven C Library Complete Manual (v26.05.19h)
     2|
     3|This manual is rebuilt from three sources:
     4|
     5|1. The current public headers in `include/proven/`.
     6|2. The current implementation and tests in `src/proven/`, `platform/`, and `tests/`.
     7|3. The older developer manual kept as `PROVEN_MANUAL_old-20260511.md` in the shared workspace.
     8|
     9|The goal is to keep the architectural explanation from the older manual while adding practical examples, return-value rules, ownership rules, and common misuse cases from the current source tree.
    10|
    11|## Table of contents
    12|
    13|1. [Intent and design philosophy](#1-intent-and-design-philosophy)
    14|2. [Build and include model](#2-build-and-include-model)
    15|3. [Global contracts](#3-global-contracts)
    16|4. [Ownership and destruction matrix](#4-ownership-and-destruction-matrix)
    17|5. [Operation behavior classes](#5-operation-behavior-classes)
    18|6. [Manual chapters](#6-manual-chapters)
    19|7. [Public header map](#7-public-header-map)
    20|8. [Platform support and verification](#8-platform-support-and-verification)
    21|
    22|## 1. Intent and design philosophy
    23|
    24|`proven` is a compact C23 systems foundation library. It is intended for C programs that want practical infrastructure without hiding memory ownership, error control flow, or platform access behind global state.
    25|
    26|It is not a libc replacement. It provides a focused set of allocator-driven memory tools, byte views, containers, strings, formatting, scanning, filesystem helpers, time helpers, memory mapping, stackless coroutine macros, and a bounded job system.
    27|
    28|Core design principles:
    29|
    30|- C23 first: the build driver uses `-std=c23`.
    31|- Explicit errors: fallible functions return `proven_err_t` or `proven_result_*_t`.
    32|- Explicit ownership: owned objects have clear destroy functions and allocator rules.
    33|- Failure atomicity: grow/realloc-style APIs preserve the old object on allocation failure unless documented otherwise.
    34|- Pointer provenance discipline: raw object access uses `proven_byte_t` and bounded views.
    35|- PAL isolation: hosted OS services live under `platform/` and are called through public wrappers.
    36|- Core containers do not add hidden locks. Shared mutation requires caller synchronization.
    37|- The build system is a single checked-in `nob.c`; tests are plain C executables.
    38|
    39|## 2. Build and include model
    40|
    41|Build and run the hosted test suite:
    42|
    43|```sh
    44|cc nob.c -o nob
    45|./nob build
    46|```
    47|
    48|Common validation commands:
    49|
    50|```sh
    51|./nob release
    52|./nob strict
    53|./nob strict-error
    54|./nob asan
    55|./nob ubsan
    56|./nob tsan
    57|./nob regression
    58|./nob regression-asan
    59|./nob regression-ubsan
    60|./nob freestanding
    61|./nob cross -build-root /home/user/work/build/proven_c_lib
    62|```
    63|
    64|Use the umbrella header when you want the full hosted API:
    65|
    66|```c
    67|#include "proven.h"
    68|```
    69|
    70|Use smaller includes when you want a narrower translation unit:
    71|
    72|```c
    73|#include "proven/heap.h"
    74|#include "proven/u8str.h"
    75|#include "proven/fmt.h"
    76|```
    77|
    78|A direct hosted application build can follow the same source layout as `nob.c`:
    79|
    80|```sh
    81|cc -std=c23 -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L \
    82|  -Iinclude -Iplatform \
    83|  app.c src/proven/*.c platform/proven_sys_*.c \
    84|  -pthread -o app
    85|```
    86|
    87|## 3. Global contracts
    88|
    89|### 3.1 Result contract
    90|
    91|A result value is usable only when `err == PROVEN_OK`.
    92|
    93|Correct:
    94|
    95|```c
    96|proven_result_mem_mut_t r = alloc.alloc_fn(alloc.ctx, 128, PROVEN_DEFAULT_ALIGNMENT);
    97|if (!proven_is_ok(r.err)) {
    98|    return r.err;
    99|}
   100|proven_mem_mut_t mem = r.value;
   101|```
   102|
   103|Wrong:
   104|
   105|```c
   106|proven_result_mem_mut_t r = alloc.alloc_fn(alloc.ctx, 128, PROVEN_DEFAULT_ALIGNMENT);
   107|use_bytes(r.value.ptr, r.value.size); /* wrong: r.err was not checked */
   108|```
   109|
   110|### 3.2 Public struct contract
   111|
   112|Many public structs expose layout for C usability. This does not mean arbitrary field mutation is supported. Treat direct mutation of internal fields as caller misuse unless the header explicitly allows it.
   113|
   114|Wrong:
   115|
   116|```c
   117|arr.len = 999;      /* wrong: breaks array invariants */
   118|str.internal.cap=0; /* wrong: breaks string invariants */
   119|```
   120|
   121|Use the functions and macros that maintain invariants.
   122|
   123|### 3.3 Borrowed view contract
   124|
   125|Views do not own memory. A `proven_u8str_view_t`, `proven_u16str_view_t`, `proven_mem_view_t`, or `proven_mem_mut_t` is valid only while the referenced storage remains alive and unmoved.
   126|
   127|Wrong:
   128|
   129|```c
   130|proven_u8str_view_t v = proven_u8str_as_view(&s);
   131|proven_u8str_append_grow(alloc, &s, PROVEN_LIT("more"));
   132|use_view(v); /* wrong: growth may reallocate s */
   133|```
   134|
   135|### 3.4 Allocator contract
   136|
   137|A `proven_allocator_t` is valid only when all three function pointers are present. Reallocation must be failure-atomic: if it fails, the old allocation remains valid.
   138|
   139|```c
   140|proven_allocator_t alloc = proven_heap_allocator();
   141|if (!proven_alloc_is_valid(alloc)) {
   142|    return PROVEN_ERR_UNSUPPORTED;
   143|}
   144|```
   145|
   146|### 3.5 PAL boundary contract
   147|
   148|Code under `src/proven/` should not depend on OS headers directly. Hosted services should route through `platform/proven_sys_*.[ch]` and public wrappers such as `proven_fs_*`, `proven_sysio_*`, `proven_time_*`, and `proven_mmap_*`.
   149|
   150|Application code should prefer public APIs. Direct PAL calls are for porting and platform integration.
   151|
   152|## 4. Ownership and destruction matrix
   153|
   154|| Object | Owns storage | Stores allocator | Destroy function | Notes |
   155||---|---:|---:|---|---|
   156|| `proven_buf_t` | yes | no | `proven_buf_destroy(alloc, &buf)` | Caller must pass the matching allocator. |
   157|| `proven_u8str_t` | yes | no | `proven_u8str_destroy(alloc, &str)` | Always NUL-terminated when valid. |
   158|| `proven_u16str_t` | yes | no | `proven_u16str_destroy(alloc, &str)` | Tracks byte length internally; API length is in `proven_u16` units. |
   159|| `proven_array_t` | yes | yes | `proven_array_destroy(&arr)` or `PROVEN_ARRAY_DESTROY(&arr)` | Pointers into elements may be invalidated by growth. |
   160|| `proven_ring_t` | yes | yes | `proven_ring_destroy(&ring)` or `PROVEN_RING_DESTROY(&ring)` | Fixed capacity; no growth. |
   161|| `proven_map_t` | yes | yes | `proven_map_destroy(&map)` or `PROVEN_MAP_DESTROY(&map)` | Borrowed U8 keys are not copied. |
   162|| `proven_array_t` from `proven_fs_list()` | yes | yes plus owned entry names | `proven_fs_list_destroy(alloc, &list)` | Do not use plain array destroy; entry names need cleanup. |
   163|| `proven_mmap_t` | OS mapping | OS handle state | `proven_mmap_destroy(&map)` | Views into the mapping die with the mapping. |
   164|| `proven_job_sys_t *` | yes | internal | `proven_job_system_close(sys)` then `proven_job_system_destroy(sys)` | Destroy must not race with producers. |
   165|
   166|## 5. Operation behavior classes
   167|
   168|Several APIs intentionally expose three behavior classes:
   169|
   170|| Class | Example | Behavior on insufficient capacity |
   171||---|---|---|
   172|| Atomic fixed-capacity | `proven_u8str_append`, `proven_u16str_append`, `proven_u8str_append_fmt` | Return an error and leave the old object unchanged. |
   173|| Best-effort truncating | `proven_u8str_append_partial`, `proven_u16str_append_partial`, `proven_u8str_append_fmt_trunc` | Write as much as fits, preserve a valid object, report how much was written. |
   174|| Atomic growable | `proven_u8str_append_grow`, `proven_u16str_append_grow`, `proven_u8str_append_fmt_grow` | Grow with an allocator; on allocation failure, leave the old object unchanged. |
   175|
   176|Choose the class deliberately. Do not treat a truncating function as an all-or-nothing function.
   177|
   178|## 6. Manual chapters
   179|
   180|The detailed reference is split by chapter so it can stay readable and source-grounded.
   181|
   182|1. [Foundation: types, errors, memory, alignment, version, panic](manual-01-foundation.md)
   183|2. [Allocation: allocator trait, heap, arena, pool, byte buffers](manual-02-allocation.md)
   184|3. [Strings and text: U8, U16, formatting, scanning](manual-03-strings-text.md)
   185|4. [Containers and algorithms: array, list, ring, map, sort/search](manual-04-containers-algorithms.md)
   186|5. [Hosted services: filesystem, sysio, environment, mmap, time](manual-05-hosted-services.md)
6. [Execution and platform: coroutines, jobs, aliases, PAL, freestanding, cross builds](manual-06-execution-and-platform.md)
7. [Alias index: every `alias_xcv.h` spelling map](manual-07-alias-xcv-index.md)
8. [Formatting and scanning: full `fmt.h` and `scan.h` reference](manual-08-fmt-scan.md)
   189|
   190|## 7. Public header map
   191|
   192|| Header | Main purpose | Chapter |
   193||---|---|---|
   194|| `proven.h` | Umbrella include | This file |
   195|| `types.h` | Fixed-width aliases, checked arithmetic, error enum | Chapter 1 |
   196|| `error.h` | Error predicate helpers | Chapter 1 |
   197|| `memory.h` | Byte views, slicing, range checks, memcmp | Chapter 1 |
   198|| `align.h` | Alignment constants and align-up helpers | Chapter 1 |
   199|| `version.h` | Version macros | Chapter 1 |
   200|| `panic.h` | Weak panic hook | Chapter 1 |
   201|| `allocator.h` | Allocator trait | Chapter 2 |
   202|| `heap.h` | PAL-backed heap allocator | Chapter 2 |
   203|| `arena.h` | Bump allocator | Chapter 2 |
   204|| `pool.h` | Fixed-size recycler allocator | Chapter 2 |
   205|| `buffer.h` | Fixed-capacity byte buffer | Chapter 2 |
   206|| `u8str.h` | Owned U8 string and borrowed U8 views | Chapter 3 |
   207|| `u16str.h` | Owned U16 string and borrowed U16 views | Chapter 3 |
   208|| `fmt.h` | Structural formatter and format arguments | Chapter 3 |
   209|| `scan.h` | Structural scanner and typed scan destinations | Chapter 3 |
   210|| `array.h` | Generic growable vector | Chapter 4 |
   211|| `list.h` | Intrusive doubly-linked list | Chapter 4 |
   212|| `ring.h` | Fixed-capacity FIFO ring | Chapter 4 |
   213|| `map.h` | Open-addressing map | Chapter 4 |
   214|| `algorithm.h` | Array sort and search helpers | Chapter 4 |
   215|| `fs.h` | Files, directories, metadata, links, locks, read-all | Chapter 5 |
   216|| `sysio.h` | Standard streams, printing, scanning, environment access | Chapter 5 |
   217|| `mmap.h` | Memory-mapped file regions | Chapter 5 |
   218|| `time.h` | Timestamp, datetime, sleep, datetime formatting | Chapter 5 |
   219|| `coro.h` | Stackless coroutine macros | Chapter 6 |
   220|| `job.h` | Bounded worker-thread job system | Chapter 6 |
   221|| `alias_xcv.h` | Optional short alias layer and generated spelling map | Chapters 6 and 7 |
   222|
   223|## 8. Platform support and verification
   224|
   225|Primary verified hosted target:
   226|
   227|- Linux x86_64 with GCC or Clang in C23 mode.
   228|
   229|Compile-only cross coverage exists when the corresponding toolchains are installed:
   230|
   231|- Linux AArch64.
   232|- Linux ARM hard-float.
   233|- Linux i686 through `i686-linux-gnu-gcc` or `gcc -m32` multilib.
   234|- Windows x86_64 and i686 through MinGW/WinAPI paths.
   235|- ARM Cortex-M freestanding.
   236|- RISC-V ELF freestanding.
   237|
   238|The cross matrix checks compilation, public header visibility, and target ABI assumptions. It does not replace runtime validation on the target platform.
   239|
   240|Freestanding mode builds a reduced subset with OS-backed services removed. See `manual-freestanding.md` and Chapter 6 for details.
   241|