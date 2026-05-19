     1|# proven C Library Specification (v26.05.19e)
     2|
     3|## 1. Scope
     4|
     5|`proven` is a C23 systems library for explicit memory management, checked data movement, PAL-isolated system access, and compact infrastructure types. It is designed around pointer provenance, strict aliasing awareness, failure-atomic memory operations, and explicit error propagation.
     6|
     7|The library is not a general replacement for libc. It provides a focused set of primitives for projects that want small, predictable C components with clear ownership and platform boundaries.
     8|
     9|## 2. Design principles
    10|
    11|1. C23 first: build with `-std=c23`.
    12|2. Explicit ownership: callers pass allocators and destroy owned objects.
    13|3. Explicit errors: fallible APIs return `proven_err_t` or `proven_result_*_t`.
    14|4. Provenance-aware memory: raw object access uses `proven_byte_t` and memory views.
    15|5. PAL isolation: OS and C runtime calls are centralized under `platform/`.
    16|6. Failure atomicity: realloc-style operations preserve the old value on failure.
    17|7. No hidden global state in core containers.
    18|8. No external build system: `nob.c` is the build driver.
    19|9. No external test framework: tests are plain C executables.
    20|10. Optional ergonomics: `alias_xcv.h` provides short-form aliases.
    21|
    22|## 3. Dependency layers
    23|
    24|The intended dependency order is:
    25|
    26|1. Foundation: `types`, `error`, `align`, `memory`.
    27|2. Allocation: `allocator`, `heap`, `arena`, `pool`.
    28|3. Buffers and strings: `buffer`, `u8str`, `u16str`.
    29|4. Collections: `array`, `list`, `ring`, `map`, `algorithm`.
    30|5. PAL-backed services: `fs`, `time`, `mmap`, `sysio`.
    31|6. Execution helpers: `coro`, `job`.
    32|7. Text utilities: `fmt`, `scan`.
    33|8. Optional alias layer: `alias_xcv`.
    34|
    35|Lower layers must not depend on higher layers.
    36|
    37|## 4. Public headers
    38|
    39|- `proven.h`: umbrella include.
    40|- `types.h`: fixed-width aliases, size types, checked arithmetic, error enum.
    41|- `error.h`: error predicates.
    42|- `memory.h`: immutable and mutable memory views.
    43|- `align.h`: alignment helpers.
    44|- `allocator.h`: allocator trait.
    45|- `heap.h`: PAL-backed heap allocator.
    46|- `arena.h`: bump allocator.
    47|- `pool.h`: fixed-size recycler.
    48|- `buffer.h`: byte buffer operations.
    49|- `u8str.h`: UTF-8-oriented byte string container and views.
    50|- `u16str.h`: `char16_t` string container and views.
    51|- `array.h`: generic growable array macros and functions.
    52|- `list.h`: intrusive doubly-linked list.
    53|- `ring.h`: bounded ring buffer; not thread-safe by itself.
    54|- `map.h`: open-addressing hash map.
    55|- `algorithm.h`: sorting and search helpers.
    56|- `fs.h`: filesystem API.
    57|- `time.h`: time API.
    58|- `fmt.h`: formatter API.
    59|- `mmap.h`: memory mapping API.
    60|- `sysio.h`: console and environment I/O helpers.
    61|- `job.h`: bounded job system.
    62|- `scan.h`: structural scanner.
    63|- `coro.h`: stackless coroutine macros.
    64|- `panic.h`: panic and invariant helpers.
    65|- `version.h`: version macros.
    66|- `alias_xcv.h`: optional `xcv_` alias layer.
    67|
    68|## 5. Implementation units
    69|
    70|The current hosted build compiles these implementation files:
    71|
    72|- `src/proven/memory.c`
    73|- `src/proven/arena.c`
    74|- `src/proven/pool.c`
    75|- `src/proven/buffer.c`
    76|- `src/proven/heap.c`
    77|- `src/proven/u8str.c`
    78|- `src/proven/u16str.c`
    79|- `src/proven/array.c`
    80|- `src/proven/ring.c`
    81|- `src/proven/map.c`
    82|- `src/proven/algorithm.c`
    83|- `src/proven/fs.c`
    84|- `src/proven/time.c`
    85|- `src/proven/fmt.c`
    86|- `src/proven/mmap.c`
    87|- `src/proven/sysio.c`
    88|- `src/proven/job.c`
    89|- `src/proven/scan.c`
    90|- `src/proven/panic.c`
    91|
    92|PAL units:
    93|
    94|- `platform/proven_sys_mem.[ch]`
    95|- `platform/proven_sys_fs.[ch]`
    96|- `platform/proven_sys_time.[ch]`
    97|- `platform/proven_sys_env.[ch]`
    98|- `platform/proven_sys_thread.[ch]`
    99|- `platform/proven_sys_io.[ch]`
   100|- `platform/proven_sys_math.[ch]`
   101|
   102|## 6. Type model
   103|
   104|- Fixed-width aliases use project names: `proven_i8`, `proven_u8`, `proven_i16`, `proven_u16`, `proven_i32`, `proven_u32`, `proven_i64`, `proven_u64`.
   105|- `proven_u16` is based on `char16_t`.
   106|- `proven_byte_t` is `unsigned char` and is the byte-level object representation type.
   107|- `proven_size_t` maps to `size_t`.
   108|- `proven_ptrdiff_t` maps to `ptrdiff_t`.
   109|- `proven_intptr_t` and `proven_uintptr_t` are available for explicit pointer-integer use.
   110|- `uintptr_t` support is required for the current arena range model.
   111|
   112|## 7. Error and result model
   113|
   114|`proven_err_t` includes success, allocation failure, bounds failure, invalid encoding, invalid argument, I/O failure, not found, invalid state, overflow, unsupported operation, retry, EOF, busy, permission failure, and invalid format.
   115|
   116|Fallible value-returning APIs use result structs. A result contains an error code and a value field. The value is only meaningful when the error is `PROVEN_OK`.
   117|
   118|Control flow must stay visible. Use explicit `if (!proven_is_ok(err))` checks.
   119|
   120|## 8. Memory and allocation model
   121|
   122|- `proven_mem_view_t` is a non-owning read-only byte view.
   123|- `proven_mem_mut_t` is a non-owning mutable byte view.
   124|- `proven_allocator_t` contains context plus `alloc`, `realloc`, and `free` function pointers.
   125|- Allocator `realloc` must leave the old allocation valid on failure.
   126|- The heap allocator routes through PAL memory functions.
   127|- The arena allocator is linear and does not reclaim individual allocations.
   128|- The pool allocator recycles fixed-size blocks.
   129|
   130|## 9. Container model
   131|
   132|- Arrays are generic growable vectors with allocator-backed storage.
   133|- Lists are intrusive and do not allocate nodes.
   134|- Rings are bounded circular buffers.
   135|- Maps use open addressing and tombstones.
   136|- Algorithms operate on explicit views and element sizes.
   137|- Public structs expose layout for C usability, but callers must preserve documented invariants.
   138|
   139|## 10. String and formatting model
   140|
   141|- `PROVEN_LIT` creates compile-time string views without runtime length scanning.
   142|- U8 and U16 strings distinguish borrowed views from owned containers.
   143|- String and formatting APIs follow three behavior classes where applicable:
   144|  - fixed-capacity atomic operations,
   145|  - best-effort truncating operations,
   146|  - growable atomic operations.
   147|- Formatting and scanning avoid direct `stdio` dependency in core logic.
   148|- Long and unsigned long scan destinations are handled directly through dedicated scan argument wrappers.
   149|
   150|## 11. Filesystem, mmap, and system I/O
   151|
   152|Hosted system services go through PAL-backed APIs. The public layer exposes file operations, directory operations, metadata, memory mapping, console output, and environment access. Zero-length read or write operations should succeed with zero bytes processed without requiring non-null buffers. Append opens should preserve platform append semantics where available, and environment access must not impose a small fixed key-size limit.
   153|
   154|## 12. Concurrency model
   155|
- Core containers do not add hidden locks.
- Shared mutable containers, including ring buffers, require caller synchronization.
   158|- Coroutine support is stackless and macro-based.
   159|- The job system uses bounded queueing and atomic coordination for its scheduler internals.
   160|- Job-system destruction must not race with producers.
   161|
   162|## 13. Freestanding model
   163|
   164|`PROVEN_FREESTANDING` builds a reduced subset with OS-backed services removed. Freestanding mode also disables float formatting and U16 string support in the current build configuration through `PROVEN_FMT_NO_FLOAT` and `PROVEN_NO_U16STR`.
   165|
   166|Freestanding checks are build-time compatibility checks, not a claim that every hosted module is available without an OS.
   167|
   168|## 14. Build specification
   169|
   170|The build driver is compiled and invoked as follows:
   171|
   172|```sh
   173|cc nob.c -o nob
   174|./nob build
   175|```
   176|
   177|Supported commands include `build`, `debug`, `release`, `strict`, `strict-error`, `asan`, `ubsan`, `tsan`, `freestanding`, `cross`, `regression`, `regression-asan`, `regression-ubsan`, `clean`, and `help`.
   178|
   179|`-build-root <dir>` redirects generated output. Use `/home/user/work/build/proven_c_lib` on shared build servers.
   180|
   181|## 15. Verification specification
   182|
   183|The hosted full run currently includes 32 executable tests. The freestanding run includes 5 freestanding tests. Regression commands run the focused regression subset. `./nob cross` performs compile-only target checks and skips missing optional compilers.
   184|
   185|Required validation before release:
   186|
   187|```sh
   188|./nob clean
   189|./nob strict-error
   190|./nob regression-asan
   191|./nob regression-ubsan
   192|./nob freestanding
   193|./nob cross -build-root /home/user/work/build/proven_c_lib
   194|```
   195|
   196|Use Clang validation when available:
   197|
   198|```sh
   199|./nob strict-error -cc clang
   200|```
   201|
   202|## 16. Supported targets
   203|
   204|Primary target:
   205|
   206|- Linux x86_64 with GCC or Clang and C23 support.
   207|
   208|Experimental or partial targets:
   209|
   210|- Linux AArch64.
   211|- Linux ARM hard-float.
   212|- Linux i686 via either `i686-linux-gnu-gcc` or `gcc -m32` multilib.
   213|- Windows through WinAPI PAL paths, normally checked with MinGW cross compilers.
   214|- macOS through POSIX-like PAL paths.
   215|- MSVC when required C23 features and checked arithmetic support are available.
   216|- Freestanding C environments for the reduced subset, including ARM Cortex-M and RISC-V ELF compile-only checks when toolchains are installed.
   217|
   218|## 17. Non-goals
   219|
   220|- No C++ API layer.
   221|- No mandatory dependency on CMake, Make, Meson, or npm.
   222|- No hidden garbage collection.
   223|- No implicit process-wide allocator.
   224|- No promise that internal public-struct fields may be mutated arbitrarily by callers.
   225|- No Wine-based validation requirement for Windows behavior.
   226|