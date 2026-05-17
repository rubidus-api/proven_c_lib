     1|# Proven C library
     2|
     3|Version: proven_c_lib-v26.05.18
     4|Standard: C23
     5|License: MIT
     6|Repository: https://github.com/rubidus-api/proven_c_lib
     7|
     8|`proven` is a small C23 foundation library for systems programs that should stay readable after the first month. It gives you strings, arrays, maps, formatting, scanning, filesystem helpers, memory mapping, time, stackless coroutines, and a bounded job system, while keeping the important choices visible: which allocator is used, who owns memory, which errors can occur, and where the program crosses into the operating system.
     9|
    10|The goal is not to make C look like another language. The goal is to make practical C less repetitive without hiding the parts that matter.
    11|
    12|## why it exists
    13|
    14|A lot of C projects grow the same private support code: a string type, a vector, a map, an arena, a few wrappers around files and time, a formatter that does not drag `printf` everywhere, and a test runner nobody wants to maintain.
    15|
    16|`proven` packages those pieces as one small layer with a few rules:
    17|
    18|- Ownership is explicit. If an object owns memory, the allocator used to create it is part of the story.
    19|- Errors are values. Fallible functions return `proven_err_t` or `proven_result_*_t`.
    20|- Reallocation-style operations are failure-atomic where documented. If growth fails, the old object remains valid.
    21|- Borrowed views are named as views. They do not pretend to own bytes.
    22|- Raw memory uses byte views instead of type-punning through unrelated pointers.
    23|- Hosted OS access is isolated behind the PAL layer in `platform/`.
    24|- The build is one C file: `nob.c`. No mandatory CMake, Make, Meson, npm, or external test framework.
    25|- Freestanding builds can use the reduced core without pulling in hosted filesystem, console, thread, mmap, or environment services.
    26|
    27|That makes the library useful for ordinary command line tools, embedded-adjacent code, experiments that may later need a stricter platform boundary, and C projects that want enough infrastructure to move quickly without losing control.
    28|
    29|## quick start
    30|
    31|```sh
    32|cc nob.c -o nob
    33|./nob build
    34|```
    35|
    36|On Windows/MSYS2, use the shell form that matches the binary you built:
    37|
    38|```sh
    39|# MSYS2/UCRT64/CLANG64 shell
    40|cc nob.c -o nob
    41|./nob build
    42|
    43|# cmd.exe or PowerShell
    44|cc nob.c -o nob.exe
    45|.\nob.exe build
    46|```
    47|
    48|`nob.c` writes build output under `build/` by default and prints `[PROVEN][BUILD][BEGIN]` before it creates platform-specific output directories.
    49|
    50|Common checks:
    51|
    52|```sh
    53|./nob strict-error
    54|./nob asan
    55|./nob ubsan
    56|./nob tsan
    57|./nob regression
    58|./nob freestanding
    59|./nob cross -build-root /home/user/work/build/proven_c_lib
    60|```
    61|
    62|Use another compiler:
    63|
    64|```sh
    65|./nob strict-error -cc clang
    66|```
    67|
    68|Running `./nob` without arguments prints the full command list.
    69|
    70|## first program
    71|
    72|This example uses the heap allocator, creates an owned UTF-8 byte string, grows it through the formatter, prints it, and then destroys it with the same allocator family.
    73|
    74|```c
    75|#include "proven.h"
    76|
    77|int main(void) {
    78|    proven_allocator_t alloc = proven_heap_allocator();
    79|
    80|    proven_result_u8str_t r =
    81|        proven_u8str_create_from_view(alloc, PROVEN_LIT("hello"));
    82|    if (!proven_is_ok(r.err)) return 1;
    83|
    84|    proven_u8str_t s = r.value;
    85|
    86|    proven_fmt_result_t fr =
    87|        proven_u8str_append_fmt_grow(alloc, &s, ", {}", PROVEN_ARG("world"));
    88|    if (!proven_is_ok(fr.err)) {
    89|        proven_u8str_destroy(alloc, &s);
    90|        return 1;
    91|    }
    92|
    93|    proven_println("{}", PROVEN_ARG(proven_u8str_as_view(&s)));
    94|    proven_u8str_destroy(alloc, &s);
    95|    return 0;
    96|}
    97|```
    98|
    99|Build it against the hosted library sources:
   100|
   101|```sh
   102|cc -std=c23 -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L \
   103|  -Iinclude -Iplatform \
   104|  app.c src/proven/*.c platform/proven_sys_*.c \
   105|  -pthread -o app
   106|```
   107|
   108|## containers without hidden ownership
   109|
   110|`proven_array_t` is a growable contiguous array. It stores the allocator trait used for growth and destruction, so the call site stays honest about allocation.
   111|
   112|```c
   113|#include "proven.h"
   114|
   115|int main(void) {
   116|    proven_allocator_t alloc = proven_heap_allocator();
   117|
   118|    proven_result_array_t r = PROVEN_ARRAY_INIT(alloc, int, 4);
   119|    if (!proven_is_ok(r.err)) return 1;
   120|
   121|    proven_array_t numbers = r.value;
   122|
   123|    if (!proven_is_ok(PROVEN_ARRAY_PUSH(&numbers, int, 10))) goto fail;
   124|    if (!proven_is_ok(PROVEN_ARRAY_PUSH(&numbers, int, 20))) goto fail;
   125|
   126|    const int *first = PROVEN_ARRAY_GET(&numbers, int, 0);
   127|    if (!first) goto fail;
   128|
   129|    proven_println("first = {}", PROVEN_ARG(*first));
   130|    PROVEN_ARRAY_DESTROY(&numbers);
   131|    return 0;
   132|
   133|fail:
   134|    PROVEN_ARRAY_DESTROY(&numbers);
   135|    return 1;
   136|}
   137|```
   138|
   139|The same pattern appears across strings, maps, buffers, arenas, pools, and filesystem helpers: create with an allocator, check the result, keep borrowed pointers short-lived, destroy owned objects deliberately.
   140|
   141|## text formatting with bounded input
   142|
   143|`PROVEN_ARG("literal")` is convenient for trusted NUL-terminated strings. For text from outside your program, prefer bounded views or bounded C-string arguments so formatting does not search past the bytes you meant to expose.
   144|
   145|```c
   146|const char packet_text[5] = { 'o', 'k', '!', '!', 'x' };
   147|proven_println("rx: {}", PROVEN_ARG_CSTR_N(packet_text, 4));
   148|```
   149|
   150|The formatter has three useful modes for owned strings:
   151|
   152|- `proven_u8str_append_fmt`: fixed-capacity and atomic.
   153|- `proven_u8str_append_fmt_trunc`: fixed-capacity and truncating.
   154|- `proven_u8str_append_fmt_grow`: allocator-backed and atomic.
   155|
   156|## platform boundary
   157|
   158|Portable implementation files live in `src/proven/`. OS and C runtime calls are isolated under `platform/`:
   159|
   160|- heap allocation
   161|- filesystem operations
   162|- time
   163|- environment access
   164|- console I/O
   165|- threads
   166|- memory mapping
   167|- math helpers where needed
   168|
   169|This split keeps the core library easier to audit and gives ports a clear place to work. Hosted Linux is the primary runtime target today. The build also has compile-only checks for optional targets when the toolchains are installed: Linux AArch64, Linux ARM hard-float, Linux i686, MinGW Windows x86_64/i686, ARM Cortex-M freestanding, and RISC-V ELF freestanding.
   170|
   171|Cross compilation proves that headers, source visibility, ABI assumptions, and compile-time platform branches line up. It does not replace runtime tests on the target machine.
   172|
   173|## main modules
   174|
   175|- Foundation: `types`, `error`, `memory`, `align`.
   176|- Allocation: `allocator`, `heap`, `arena`, `pool`.
   177|- Buffers and strings: `buffer`, `u8str`, `u16str`.
   178|- Containers: `array`, `list`, `ring`, `map`.
   179|- Algorithms: `algorithm`.
   180|- Text: `fmt`, `scan`.
   181|- Hosted services: `fs`, `time`, `mmap`, `sysio`.
   182|- Execution: `coro`, `job`.
   183|- Diagnostics: `panic`.
   184|- Optional short aliases: `alias_xcv`.
   185|
   186|## what it is not
   187|
   188|`proven` is not a libc replacement, a garbage collector, or a framework. It does not try to own your process, your build graph, or your error policy. It is a set of C components that are meant to be easy to read, easy to test, and possible to port one boundary at a time.
   189|
   190|## documentation

- User manual: `manual/manual.md`
- Freestanding guide: `manual/manual-freestanding.md`
- Specification: `SPEC.md`
- Test matrix: `TEST.md`
- Project guide: `AGENTS.md`
- Durable facts: `MEMORY.md`
- Bug lessons: `CHECKLIST.md`

## status
   199|
   200|The primary verified target is Linux x86_64 with GCC or Clang in C23 mode. Sanitizer, regression, freestanding, and cross compile checks are driven by `nob.c`. Optional cross targets are checked when the corresponding toolchains are present.
   201|
   202|## author and license
   203|
   204|Developed by rubidus-api.
   205|Email: rubidus@gmail.com
   206|License: MIT License. See `LICENSE`.
   207|
