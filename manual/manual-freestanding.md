     1|# Proven Freestanding Mode
     2|
     3|This guide describes the current `PROVEN_FREESTANDING` configuration as implemented by `nob.c` and the public headers.
     4|
     5|Freestanding mode is for firmware, kernels, bootloaders, hypervisors, or other environments where normal hosted OS services and libc facilities are not available or should not be linked into the proven core.
     6|
     7|## 1. Build profile
     8|
     9|The build driver uses these freestanding flags:
    10|
    11|```sh
    12|-std=c23
    13|-ffreestanding
    14|-DPROVEN_FREESTANDING
    15|-DPROVEN_FMT_NO_FLOAT
    16|-DPROVEN_NO_U16STR
    17|```
    18|
    19|The hosted `./nob freestanding` command also links its local freestanding test executables statically on the build host. Cross freestanding targets in `./nob cross` use compile-only object checks.
    20|
    21|Run the project freestanding check:
    22|
    23|```sh
    24|cc nob.c -o nob
    25|./nob freestanding -build-root /home/user/work/build/proven_c_lib
    26|```
    27|
    28|Run the compile-only cross matrix:
    29|
    30|```sh
    31|./nob cross -build-root /home/user/work/build/proven_c_lib
    32|```
    33|
    34|## 2. Available source files
    35|
    36|In the current freestanding build, `nob.c` compiles the portable source files except hosted-only modules.
    37|
    38|Included core modules:
    39|
    40|```text
    41|src/proven/memory.c
    42|src/proven/arena.c
    43|src/proven/pool.c
    44|src/proven/buffer.c
    45|src/proven/heap.c
    46|src/proven/u8str.c
    47|src/proven/array.c
    48|src/proven/ring.c
    49|src/proven/map.c
    50|src/proven/algorithm.c
    51|src/proven/time.c
    52|src/proven/fmt.c
    53|src/proven/scan.c
    54|src/proven/panic.c
    55|platform/proven_sys_math.c
    56|```
    57|
    58|Excluded hosted modules:
    59|
    60|```text
    61|src/proven/u16str.c
    62|src/proven/fs.c
    63|src/proven/sysio.c
    64|src/proven/mmap.c
    65|src/proven/job.c
    66|platform/proven_sys_fs.c
    67|platform/proven_sys_thread.c
    68|platform/proven_sys_io.c
    69|platform/proven_sys_env.c
    70|platform/proven_sys_time.c
    71|platform/proven_sys_mem.c
    72|```
    73|
    74|Do not add excluded hosted modules to a bare-metal build unless you also provide a real platform backend for the target.
    75|
    76|## 3. Module availability
    77|
    78|| Module | Status in current freestanding profile | Notes |
    79||---|---|---|
    80|| `types.h`, `error.h`, `align.h`, `memory.h` | Available | Requires fixed-width integer and `uintptr_t` support. |
    81|| `allocator.h` | Available | Trait only. Caller supplies backing allocators. |
    82|| `arena.h` | Available | Primary allocator for static memory regions. |
    83|| `pool.h` | Available | Uses a caller-provided base allocator, often an arena. |
    84|| `buffer.h` | Available | Fixed-capacity byte buffer. |
    85|| `u8str.h` | Available | U8 strings and views. |
    86|| `u16str.h` | Excluded | Current profile defines `PROVEN_NO_U16STR`. |
    87|| `array.h`, `list.h`, `ring.h`, `map.h` | Available | No hidden OS dependency. |
    88|| `algorithm.h` | Available | Sort/search helpers for arrays. |
    89|| `fmt.h` | Available without float | Current profile defines `PROVEN_FMT_NO_FLOAT`. |
    90|| `scan.h` | Available | Scanner for memory views. |
    91|| `time.h` | Limited | Core datetime formatting can compile; real PAL time is excluded. |
    92|| `heap.h` | Stub | `proven_heap_allocator()` returns an invalid allocator. |
    93|| `fs.h`, `mmap.h`, `sysio.h`, `job.h` | Excluded | Require hosted PAL services. |
    94|| `coro.h` | Available | Macro-only stackless coroutine support. |
    95|| `panic.h` | Available | Override for target-specific trap/reset behavior. |
    96|
    97|## 4. Minimal static arena setup
    98|
    99|```c
   100|#include "proven/types.h"
   101|#include "proven/memory.h"
   102|#include "proven/arena.h"
   103|#include "proven/array.h"
   104|#include "proven/panic.h"
   105|
   106|static alignas(PROVEN_MAX_ALIGN) proven_byte_t g_storage[4096];
   107|static proven_arena_t g_arena;
   108|static proven_allocator_t g_alloc;
   109|
   110|void platform_init(void) {
   111|    g_arena = proven_arena_create((proven_mem_mut_t){
   112|        .ptr = g_storage,
   113|        .size = sizeof g_storage,
   114|    });
   115|    g_alloc = proven_arena_as_allocator(&g_arena);
   116|}
   117|
   118|proven_err_t make_values(void) {
   119|    proven_result_array_t r = PROVEN_ARRAY_INIT(g_alloc, proven_i32, 16);
   120|    if (!proven_is_ok(r.err)) return r.err;
   121|
   122|    proven_array_t values = r.value;
   123|    proven_err_t e = PROVEN_ARRAY_PUSH(&values, proven_i32, 42);
   124|    PROVEN_ARRAY_DESTROY(&values); /* arena free is a no-op */
   125|    return e;
   126|}
   127|```
   128|
   129|Common mistake:
   130|
   131|```c
   132|proven_allocator_t heap = proven_heap_allocator();
   133|heap.alloc_fn(heap.ctx, 64, 8); /* wrong: heap is invalid in PROVEN_FREESTANDING */
   134|```
   135|
   136|Correct:
   137|
   138|```c
   139|proven_allocator_t heap = proven_heap_allocator();
   140|if (!proven_alloc_is_valid(heap)) {
   141|    /* use an arena, pool, or target-provided allocator instead */
   142|}
   143|```
   144|
   145|## 5. Panic handler override
   146|
   147|`proven_arena_alloc_or_panic()` and related panic paths call `proven_panic_handler()`.
   148|
   149|```c
   150|#include "proven/panic.h"
   151|
   152|void proven_panic_handler(const char *msg) {
   153|    (void)msg;
   154|    /* optional: write msg to UART or crash log */
   155|    for (;;) {
   156|        /* or reset the MCU */
   157|    }
   158|}
   159|```
   160|
   161|A production panic handler should not return. If it returns, the allocation result that triggered the panic is not guaranteed to be valid.
   162|
   163|## 6. Example Cortex-M compile command
   164|
   165|```sh
   166|arm-none-eabi-gcc \
   167|  -std=c23 \
   168|  -mcpu=cortex-m4 -mthumb \
   169|  -ffreestanding -nostdlib \
   170|  -DPROVEN_FREESTANDING \
   171|  -DPROVEN_FMT_NO_FLOAT \
   172|  -DPROVEN_NO_U16STR \
   173|  -Iinclude -Iplatform \
   174|  -c src/proven/memory.c -o memory.o
   175|```
   176|
   177|A real firmware build should compile the included freestanding modules listed above, compile your target startup code, and link with your linker script.
   178|
   179|## 7. Example RISC-V ELF compile command
   180|
   181|```sh
   182|riscv64-elf-gcc \
   183|  -std=c23 \
   184|  -ffreestanding -nostdlib \
   185|  -DPROVEN_FREESTANDING \
   186|  -DPROVEN_FMT_NO_FLOAT \
   187|  -DPROVEN_NO_U16STR \
   188|  -Iinclude -Iplatform \
   189|  -c src/proven/arena.c -o arena.o
   190|```
   191|
   192|If your toolchain uses the `riscv64-unknown-elf-gcc` name, use that compiler instead. The project cross matrix checks both names when available.
   193|
   194|## 8. Formatting in freestanding mode
   195|
   196|Float formatting is disabled by `PROVEN_FMT_NO_FLOAT`. Integer and string-view formatting remain available.
   197|
   198|Correct:
   199|
   200|```c
   201|proven_u8str_t s = make_fixed_string_from_arena();
   202|proven_u8str_append_fmt_grow(g_alloc, &s, "value={}", PROVEN_ARG(123));
   203|```
   204|
   205|Wrong:
   206|
   207|```c
   208|proven_u8str_append_fmt_grow(g_alloc, &s, "{}", PROVEN_ARG(3.14));
   209|/* wrong in the current freestanding profile: float args are excluded */
   210|```
   211|
   212|## 9. Avoid hosted APIs
   213|
   214|Do not call these in the current freestanding profile:
   215|
   216|```c
   217|proven_fs_open(...);
   218|proven_println(...);
   219|proven_env_get(...);
   220|proven_mmap_create(...);
   221|proven_job_system_init(...);
   222|```
   223|
   224|They require hosted PAL files that are intentionally excluded.
   225|
   226|## 10. Lifetime rules still apply
   227|
   228|Freestanding does not relax container rules.
   229|
   230|Wrong:
   231|
   232|```c
   233|int *p = PROVEN_ARRAY_GET_MUT(&arr, int, 0);
   234|PROVEN_ARRAY_PUSH(&arr, int, 9);
   235|*p = 1; /* wrong: push may have moved the array */
   236|```
   237|
   238|Wrong:
   239|
   240|```c
   241|proven_u8str_view_t key = make_stack_view();
   242|PROVEN_MAP_SET_U8_BORROWED(&map, key, int, 1);
   243|return; /* wrong if map survives after key bytes go out of scope */
   244|```
   245|
   246|## 11. Verification
   247|
   248|The project freestanding command builds and runs these local checks on the build host:
   249|
   250|```text
   251|tests/test_freestanding_heap_stub
   252|tests/test_compile_freestanding
   253|tests/test_compile_nofloat
   254|tests/test_compile_nou16str
   255|tests/test_freestanding
   256|```
   257|
   258|The cross command performs compile-only checks for available embedded compilers:
   259|
   260|```text
   261|freestanding-arm-cortex-m4        arm-none-eabi-gcc -mcpu=cortex-m4 -mthumb
   262|freestanding-riscv64-elf          riscv64-elf-gcc
   263|freestanding-riscv64-unknown-elf  riscv64-unknown-elf-gcc
   264|```
   265|
   266|Missing toolchains are skipped. Real compile failures fail the command. Runtime behavior still needs validation on the target or emulator.
   267|