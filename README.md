# Proven C library

**English** · [한국어](README-ko.md)

`proven` is a small C23 systems library for code that should stay readable over time. It gives you the everyday pieces C projects usually end up rewriting: explicit allocators, owned and borrowed strings, dynamic arrays, maps with borrowed or owned string keys (HashDoS-resistant by default), formatting and scanning, buffered streams and line input from stdin, filesystem helpers, memory mapping, time, hashing (FNV, SipHash, CRC-32, SHA-256), randomness by use case (a reproducible generator, a cryptographic one, and the OS CSPRNG), stackless coroutines, and a bounded job system.

The point is not to hide C behind a framework. The point is to make practical C less repetitive while still keeping ownership, errors, allocator choice, and platform boundaries visible.

The build driver probes `-std=c23` first and falls back to `-std=c2x` when the compiler still uses the transitional spelling, so older GCC and Clang front ends can still build the tree without changing the library's C23 baseline.

- Version: proven_c_lib-v26.07.20b
- Standard: C23
- License: MIT
- Repository: https://github.com/rubidus-api/proven_c_lib

## why people reach for it

- Ownership is explicit, so it is easier to see who allocates and who frees.
- Fallible APIs return results instead of silently hiding failure.
- Reallocation-style operations are designed to stay failure-atomic where documented.
- Borrowed views are clearly separated from owning objects.
- Decimal/float conversion is correctly rounded (bit-for-bit equal to the host `strtod`/`snprintf`) and has been checked exhaustively for `binary32`; on typical data it is faster than glibc.
- Hosted OS access is isolated behind the PAL layer in `platform/`.
- Freestanding builds can use the reduced core without pulling in hosted filesystem, console, thread, mmap, or environment services.
- The build system is a single C file, `nob.c`, so there is no mandatory CMake, Meson, npm, or external test framework.

That makes the library useful for command-line tools, embedded-adjacent code, experiment code that may later need a stricter platform boundary, and C projects that want a compact support layer without giving up control.

## quick start

Build the driver and run the default hosted test/build path:

```sh
cc nob.c -o nob
./nob build
```

Use a different compiler if needed:

```sh
./nob strict-error -cc clang
```

Common checks:

```sh
./nob release
./nob asan
./nob ubsan
./nob tsan
./nob regression
./nob bench-float
./nob freestanding
./nob cross -build-root /home/user/work/build/proven_c_lib
```

Running `./nob` without arguments prints the full command list.

## a small example

This example creates an owned UTF-8 string, appends to it with the formatter, prints it, and then destroys it with the same allocator family.

```c
#include "proven.h"

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    proven_result_u8str_t r =
        proven_u8str_create_from_view(alloc, PROVEN_LIT("hello"));
    if (!proven_is_ok(r.err)) {
        return 1;
    }

    proven_u8str_t s = r.value;

    proven_fmt_result_t fr =
        proven_u8str_append_fmt_grow(alloc, &s, ", {}", PROVEN_ARG("world"));
    if (!proven_is_ok(fr.err)) {
        proven_u8str_destroy(alloc, &s);
        return 1;
    }

    proven_println("{}", PROVEN_ARG(proven_u8str_as_view(&s)));
    proven_u8str_destroy(alloc, &s);
    return 0;
}
```

Build it against the hosted sources:

```sh
cc -std=c23 -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L \
  -Iinclude -Iplatform \
  app.c src/proven/*.c platform/proven_sys_*.c \
  -pthread -o app
```

## containers without hidden ownership

`proven_array_t` is a growable contiguous array. It stores the allocator trait used for growth and destruction, so the call site stays honest about memory ownership.

```c
#include "proven.h"

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    proven_result_array_t r = PROVEN_ARRAY_INIT(alloc, int, 4);
    if (!proven_is_ok(r.err)) {
        return 1;
    }

    proven_array_t numbers = r.value;

    if (!proven_is_ok(PROVEN_ARRAY_PUSH(&numbers, int, 10))) goto fail;
    if (!proven_is_ok(PROVEN_ARRAY_PUSH(&numbers, int, 20))) goto fail;

    const int *first = PROVEN_ARRAY_GET(&numbers, int, 0);
    if (!first) goto fail;

    proven_println("first = {}", PROVEN_ARG(*first));
    PROVEN_ARRAY_DESTROY(&numbers);
    return 0;

fail:
    PROVEN_ARRAY_DESTROY(&numbers);
    return 1;
}
```

The same pattern appears across strings, maps, buffers, arenas, pools, and filesystem helpers: create with an allocator, check the result, keep borrowed pointers short-lived, and destroy owned objects deliberately.

## text formatting with bounded input

`PROVEN_ARG("literal")` is convenient for trusted NUL-terminated strings. For text from outside your program, prefer bounded views or bounded C-string arguments so formatting does not search past the bytes you meant to expose.

```c
const char packet_text[5] = { 'o', 'k', '!', '!', 'x' };
proven_println("rx: {}", PROVEN_ARG_CSTR_N(packet_text, 4));
```

The formatter has three useful modes for owned strings:

- `proven_u8str_append_fmt`: fixed-capacity and atomic.
- `proven_u8str_append_fmt_trunc`: fixed-capacity and truncating.
- `proven_u8str_append_fmt_grow`: allocator-backed and atomic.

## whole files, and sorting that cannot be made quadratic

Reading a file whole is the operation most programs actually want, so it is one call:

```c
proven_result_u8str_t src = proven_fs_read_all_u8str(alloc, PROVEN_LIT("main.c"));
if (!proven_is_ok(src.err)) return src.err;
proven_u8str_view_t text = proven_u8str_as_view(&src.value);   /* NUL-terminated, no second copy */
proven_u8str_destroy(alloc, &src.value);
```

`proven_fs_read_all` / `proven_fs_read_all_u8str` read to EOF rather than to a pre-measured size. The file's reported size only seeds the capacity, so a regular file still costs one allocation and one pass - but a source whose size cannot be known up front (a pipe, a FIFO, a `/proc` entry) is read correctly rather than coming back empty, and a file that grows mid-read is not silently truncated.

Writing is symmetric. `proven_fs_write_file` creates or truncates; `proven_fs_write_file_atomic` writes a sibling temp file and renames it over the target, so a concurrent reader sees either the whole old file or the whole new one, and the target's permissions are preserved (a `0600` file is not republished as `0644`). It is atomic with respect to readers; durability across power loss is a separate, explicit ask - `proven_fs_sync` (fsync) and `proven_fs_write_file_durable`, which does the three steps in the only order that works: sync the temp file, rename, then sync the directory.

`proven_array_sort` is an introsort, and two of its properties are worth stating because they are the ones that bite:

- **O(n log n) is a guarantee, not a typical case.** A heapsort fallback past a bounded recursion depth is what makes it one. A sort whose worst case can be reached by an adversarial ordering is a denial of service in any program that sorts data it did not author.
- **Duplicate keys are the fast case.** Elements equal to the pivot are collected into a run that is final and never recursed into, so all-equal input costs a single pass. Low-cardinality keys - a status column, an enum, a bucket id - are what callers actually sort by, and they are exactly what a naive partition degrades on.

## hashes, tokens, and text you can put in a URL

There is no single "hash" and no single "random". Which one is correct depends on what you are doing with the result, and reaching for the wrong one gives you a program that is either needlessly slow or quietly insecure. Both modules are organised so the choice is made once you name the job — and so the wrong choice is hard to make by accident.

| Your job | Use |
|---|---|
| Hash keys into **your own** table (trusted input) | `proven_hash_bytes` — FNV-1a, fast |
| Hash keys from **untrusted** input | `proven_hash_keyed` — SipHash. (`proven_map` already does this for you: string-key maps are HashDoS-resistant by default.) |
| Detect **corruption** on disk or in transit | `proven_crc32` — interoperates with gzip/zlib/PNG |
| **Fingerprint** content — dedup, "same file?" | `proven_sha256` — the only one safe against a *deliberately* forged match |
| A key, a token, a nonce | `proven_random_bytes` (the OS CSPRNG), or a `proven_chacha_rng_t` seeded from it |
| A **reproducible** run — a simulation, a test | `proven_xoshiro256ss_t`. Fast, replays exactly from its seed, and **never** for a secret |

```c
/* A URL-safe session token: strong bytes, then text that needs no escaping. */
proven_byte_t raw[16];
proven_byte_t token[32];
proven_size_t n = 0;

if (proven_random_bytes(raw, sizeof raw) &&
    proven_is_ok(proven_base64url_encode(
        (proven_mem_view_t){ raw, sizeof raw }, token, sizeof token, &n))) {
    proven_println("token: {}", PROVEN_ARG_CSTR_N((const char *)token, n));
}
```

`encode.h` is the other half of this: `hex`, `base64`, and `base64url` (no padding, nothing to escape). The decoders **validate the whole input before writing a byte** — a stray character is `PROVEN_ERR_INVALID_ENCODING`, never a read past the end or a silently short result — and the output size is a function you call, not a number you remember.

The generators and hashes are pure arithmetic, so they work on a bare-metal target too. On a board with no OS, hand the library its hardware entropy once (`proven_random_set_source`) and the cryptographic generator works with no operating system at all.

## streams: a line from stdin, and printing without a syscall per line

A writer is a byte sink; a reader is a byte source. Both are small vtables passed by value, like the allocator — so one `serialize(writer, value)` works over memory, a file, or a standard stream, and the formatter can be aimed at any of them.

```c
/* Read stdin a line at a time. One buffer, no allocation per line. */
proven_byte_t buf[4096];
proven_sysio_lines_t lines;
if (proven_is_ok(proven_sysio_stdin_lines(&lines,
        (proven_mem_mut_t){ .ptr = buf, .size = sizeof buf }))) {
    for (;;) {
        proven_result_u8str_view_t line = proven_sysio_read_line(&lines);
        if (line.err == PROVEN_ERR_EOF) break;
        if (!proven_is_ok(line.err)) break;   /* a line longer than `buf` is refused, not truncated */
        proven_println("{}", PROVEN_ARG(line.val));
    }
}
```

The view points *into* your buffer and is valid until the next call — which is what makes a million lines cost one buffer instead of a million allocations. Copy it if you need to keep it.

Buffer the output side and a thousand small prints cost one syscall instead of a thousand — but **you must flush**: there is no hidden global buffer, so there is also no destructor and no `atexit` handler to flush it behind your back. The direct calls (`proven_println`, `proven_eprintln`) stay unbuffered for exactly that reason: what they write is on its way out before they return.

## correct, fast number conversion

Decimal-to-`double` parsing and `double`/`float`-to-decimal formatting are
correctly rounded (round-to-nearest, ties to even) and produced by an
integer-only engine — no `long double`. The parser is bit-for-bit identical to
the host `strtod`; fixed `%f`/`%e` output matches the host `snprintf`; and a
shortest mode emits the minimal round-trippable string.

```c
#include "proven/scan.h"
#include "proven/float_format.h"

/* Parse: correctly rounded, NUL terminator not required (length-bounded view). */
proven_scan_t sc = proven_scan_init(proven_u8str_view_from_cstr("3.14159e2"));
double v = proven_scan_f64(&sc).val;            /* 314.159 */

/* Format shortest: the minimal string that round-trips back to the same value. */
char buf[64];
proven_size_t n = 0;
proven_float_format_f64_policy(buf, sizeof buf, 0.1,
    PROVEN_FLOAT_FORMAT_POLICY_RYU,
    proven_float_format_options_shortest(), &n);  /* buf == "0.1" */
```

How far this is checked, stated plainly:

- Exhaustive: all 4,278,190,080 finite `binary32` values, zero mismatches against
  the host C library (shortest round-trip + minimality, parser vs `strtod`).
- Large-scale: 2,560,000,000 random `binary64` values, zero mismatches (this sweep
  found and fixed one real formatting defect — see the doc).
- Speed vs glibc 2.41 on this machine (x86-64): faster at parsing typical numbers
  and at shortest formatting (~4-5x); `%f`/`%e` are faster at normal magnitudes and
  slower at extreme magnitudes, where the engine does exact arbitrary-precision work.

Methodology, algorithms, and the full benchmark are in
[`docs/float-correctness-and-performance.md`](docs/float-correctness-and-performance.md).

## platform boundary

Portable implementation files live in `src/proven/`. OS and C runtime calls are isolated under `platform/`:

- heap allocation
- filesystem operations
- time
- environment access
- console I/O
- threads
- memory mapping
- math helpers where needed

This split keeps the core library easier to audit and gives ports a clear place to work. Hosted Linux is the primary runtime target today. The build also has compile-only checks for optional targets when the toolchains are installed: Linux AArch64, Linux ARM hard-float, Linux i686, MinGW Windows x86_64/i686, ARM Cortex-M freestanding, and RISC-V ELF freestanding.

Cross compilation shows that headers, source visibility, ABI assumptions, and compile-time platform branches line up. It does not replace runtime tests on the target machine.

## main modules

- Foundation: `types`, `error`, `memory`, `align`, `version`, `config`.
- Allocation: `allocator`, `heap`, `arena`, `pool`.
- Buffers and strings: `buffer`, `u8str`, `u16str`.
- Containers: `array`, `list`, `ring`, `map`.
- Algorithms: `algorithm`.
- Text: `fmt`, `scan`.
- Numbers: `float_parse`, `float_format`.
- Hashing and encoding: `hash` (FNV-1a, SipHash-2-4, CRC-32, SHA-256), `encode` (hex, Base64, Base64URL).
- Randomness: `random` (xoshiro256** reproducible, ChaCha20 cryptographic, unbiased range/shuffle helpers, and a pluggable entropy source — the OS CSPRNG by default, a board's hardware TRNG on bare metal).
- Hosted services: `fs`, `stream`, `time`, `mmap`, `sysio`.
- Execution: `coro`, `job`.
- Diagnostics: `panic`.
- Optional short aliases: `alias_xcv`.

## what it is not

`proven` is not a libc replacement, a garbage collector, or a framework. It does not try to own your process, your build graph, or your error policy. It is a set of C components that are meant to be easy to read, easy to test, and possible to port one boundary at a time.

It is also worth saying where the platform boundary **stops**, because otherwise you find out by running into it. The PAL covers memory, the filesystem, time, memory mapping, environment variables, console I/O and threads. It does **not** cover process control (`fork` / `exec` / pipes), terminal control (raw mode, job control), or networking — a program whose substance is one of those will reach for POSIX or Win32 directly, and the "no platform `#ifdef`s" property does not extend to it.

The `hash` module does provide cryptographic and non-cryptographic hashes (SHA-256 alongside FNV, SipHash, and CRC-32) and `random` provides OS-strength bytes, but `proven` is not a cryptography library: deliberate non-goals, so you do not go looking, are signatures, key exchange, password hashing / KDFs, authenticated encryption, and TLS — along with path manipulation, argument parsing, and a logging framework.

## utility in a real project

These notes come from building a small terminal text editor (`prov`) on top of `proven`. They are one data point, recorded plainly rather than as a sales pitch.

What it bought:

- **Testability.** Threading `proven_allocator_t` through every module let the whole editor core run under ASan/UBSan and leak detection, including randomized edit-vs-model cross-checks. This was the largest practical gain.
- **Fewer unchecked-error and string bugs.** `proven_err_t` with `[[nodiscard]]` makes an ignored error a compile-time signal; bounded/owning `u8str` and the typed formatter removed the `snprintf` format/overflow bug class. The editor core stayed libc-free apart from its `main` entry point.
- **Portability.** The `fs` / `time` / `mmap` / terminal PAL let one codebase build for Linux x86_64/arm64/armhf and Windows x64; a file browser worked on both through `proven_fs_list` / `proven_fs_stat` / `proven_time_breakdown` with no platform `#ifdef`s in the editor.

What you accept, and should not expect:

- **It is not a performance win.** Replacing libc `memmove` with `proven_mem_move` was benchmark-neutral; the editor's 5–50× edit speedups came entirely from its own data structures (incremental line index, piece coalescing), not from the library.
- **Vendoring discipline.** A copy-only, do-not-patch integration means library gaps are filed upstream rather than fixed in place. Early editor work hit three such gaps (a Windows panic-symbol link failure, a missing fixed-capacity string constructor, and absent owner/group in `fs` stat); they were resolved or deferred upstream, not patched downstream.
- **Pervasive coupling.** Passing the allocator and Result types everywhere is a deliberate commitment. It pays off on a long-lived, multi-platform codebase and is heavier than warranted for throwaway code.
- **A young library has gaps.** Expect to occasionally find a missing primitive and to fill or report it. The value is concentrated in safety, testability, and portability — not in convenience or raw speed.

## documentation

- User manual: `manual/manual.md` (chapters under `manual/`)
- Korean manual (한국어 매뉴얼): `manual-ko/manual-ko.md`
- Freestanding guide: `manual/manual-freestanding.md`
- Float correctness and performance: `docs/float-correctness-and-performance.md`
- Case study, language toolchain: `docs/case-study-lowent.md`
- Primitive throughput (hash/encode/random): `docs/primitives-benchmark.md`
- Test matrix: `TEST.md`
- Changelog: `CHANGELOG.md`
- Contributor checklist: `CHECKLIST.md`

## status

The primary verified target is Linux x86_64 with GCC or Clang in C23 mode. Sanitizer, regression, freestanding, and cross compile checks are driven by `nob.c`. Optional cross targets are checked when the corresponding toolchains are present. The build driver probes `-std=c23` first and falls back to `-std=c2x` when needed.

Cross compilation is compile-time coverage only. It checks that the headers, PAL splits, and target-specific branches compile together; it does not replace runtime validation on each target.

Borrowed views require caller-managed lifetime. Public structs expose layout for C use, but callers should not manually corrupt them; validation helpers exist for defensive checks.

Strict pointer provenance is not fully claimed here. The library is designed to avoid common C undefined-behavior hazards under documented contracts on conventional hosted-systems C implementations.

## author and license

Developed by rubidus-api.
Email: rubidus@gmail.com
License: MIT License. See `LICENSE`.
