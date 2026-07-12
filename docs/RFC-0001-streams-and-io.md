# RFC-0001 — Streams, and the I/O layer that is missing

**Status:** proposed
**Date:** 2026-07-12
**Tracks:** `docs/BACKLOG.md` B-004 … B-010

---

## 1. The finding

Two audits went looking for weakness in `fmt` and in the I/O layer. They found a
lot of small things, and one big thing that explains most of the small things.

**There is no stream abstraction.** There is no `proven_writer_t` and no
`proven_reader_t`. What exists instead is four unrelated types:

| Role | Type today |
|---|---|
| the formatter's sink | `proven_u8str_t` — and nothing else |
| a file | `proven_file_t` |
| the in-memory scanner's source | `proven_u8str_view_t` |
| the file scanner's source | `proven_sysio_scanner_t` |

Four types, four function families, no common interface. So:

- You **cannot** write one `serialize(writer, value)` that works over both memory
  and a file.
- You **cannot** format directly into a file. `proven_sysio_print_impl` looks like
  it does, but it allocates a whole `proven_u8str_t` from the **global heap**,
  formats into it, writes it, and frees it — **every call**. The sink is never the
  file; it is always a heap string that then gets dumped.
- You **cannot** read a line. There is no line reader anywhere. The only route is
  `proven_fs_read_all_u8str` — the whole file into memory — and then split on
  `\n` by hand. That is unusable for a file larger than memory, and absurd for a
  log tail.

The measured consequences are not subtle.

## 2. What that costs, measured

**Console output is 213× more syscalls than stdio, and allocates once per line.**

| | `write()` syscalls | bytes per syscall | ns/line |
|---|---|---|---|
| `proven_println` × 10,000 | **10,000** | 18.9 | **6,442** |
| `printf` × 10,000 | 47 | 4,018.9 | 110 |

An `LD_PRELOAD` malloc counter records exactly `malloc=10000 free=10000` for
10,000 prints: one heap round-trip per line. **The logging path allocates**, which
is the one place an allocation is least welcome — a program logging its way out of
an out-of-memory condition will fail to log it.

**`proven_sysio_flush` is a no-op that documents itself as a flush.** The header
said "flushes the internal buffer of the given file/stream to the OS." Disassembly
of the POSIX build:

```text
0000000000000350 <proven_sys_io_flush>:
 350:   c3                      ret
```

One instruction. There is no buffer, and it does not `fsync`. On **Windows** the
same call runs `FlushFileBuffers` — a real disk sync, orders of magnitude more
expensive. **One API, two entirely different meanings, and neither is what the
name promised.** (The header now says so; see §6.)

**There is no durability at all.** The complete list of sync-ish libc symbols the
entire library imports is `msync`. No `fsync`, no `fdatasync`. So
`proven_fs_write_file_atomic` cannot be made crash-durable even by a caller who
wants to pay for it — you cannot sync the file, and you certainly cannot sync the
directory the `rename` happened in.

**`proven_fs_list` materialises the whole directory.** On 50,000 entries: 189 ms,
+4.2 MB RSS, 50,008 mallocs, and *nothing* visible to the caller until the last
entry is read. The PAL already has a streaming iterator
(`proven_sys_fs_dir_open` / `_dir_next` / `_dir_close`) — `fs.c` wraps it in a
loop that buffers everything and throws the stream away.

**There is no seek.** Not in the public API at all. `proven_fs_truncate` does not
exist either, so truncating a file means reading it all and rewriting it: an O(n)
copy for an O(1) operation.

## 3. The formatter's own gaps

Separate from streams, the `{}` spec grammar supports exactly four things: fill,
align, width, and lowercase `x`. Two of its failures were **silent**, and are
fixed already (see §6). What remains missing:

- **No float precision.** `{}` means six decimals, forever. `{:.3}` is
  `INVALID_FORMAT`. The engine is fully capable — `proven_float_format_f64_policy`
  does precision, scientific, and shortest-round-trip — but **it is unreachable
  from the `{}` syntax.** Consequence: float columns cannot be aligned (`12.5`
  renders as `12.500000`, `100.0` as `100.000000` — ten characters, overflowing a
  nine-wide column), and a log line with a latency to 3 decimals cannot be
  expressed at all.
- **No `{:c}`, no `bool`.** `PROVEN_ARG('Z')` prints `90`. There is no way to emit
  a single character.
- **No `{:X}`, `{:o}`, `{:b}`, `{:#x}`, `{:+}`.** A conventional uppercase hex
  dump is impossible.
- **No extension point.** `proven_arg_type_t` is a closed enum. To format a user
  struct you must pre-render it into your own buffer and pass a string — which
  means you cannot then compose it (`{:>20}` on a user type is impossible unless
  you pad it yourself).

## 4. The design

### 4.1 The stream traits

Model them on the allocator trait, which is the pattern this library already
trusts: a small vtable, passed by value, no hidden global state.

```text
typedef struct {
    void *ctx;
    proven_err_t (*write_fn)(void *ctx, proven_mem_view_t chunk);
    proven_err_t (*flush_fn)(void *ctx);   /* may be NULL: nothing is buffered */
} proven_writer_t;

typedef struct {
    void *ctx;
    /* Fills `dest`, returns bytes read. 0 with PROVEN_OK means end of input. */
    proven_result_size_t (*read_fn)(void *ctx, proven_mem_mut_t dest);
} proven_reader_t;
```

Constructors:

```text
proven_writer_t proven_writer_from_file(proven_file_t file);
proven_writer_t proven_writer_from_u8str(proven_u8str_t *str, proven_allocator_t alloc);
proven_writer_t proven_writer_from_buffer(proven_mem_mut_t buf, proven_size_t *written);
proven_writer_t proven_writer_buffered(proven_writer_t inner, proven_mem_mut_t buf);

proven_reader_t proven_reader_from_file(proven_file_t file);
proven_reader_t proven_reader_from_view(proven_u8str_view_t view);
proven_reader_t proven_reader_buffered(proven_reader_t inner, proven_mem_mut_t buf);
```

Note what `proven_writer_buffered` takes: **caller-supplied backing memory**, like
`proven_arena_create`. The library does not get to decide to allocate on your
logging path. That is the whole point.

### 4.2 The formatter writes to a writer

```text
proven_fmt_result_t proven_fmt_to_writer(proven_writer_t w, const char *fmt, ...);
#define proven_fprint(file, fmt, ...)     /* an unbuffered file writer */
#define proven_fprintln(file, fmt, ...)
```

`proven_u8str_append_fmt` and friends stay exactly as they are — they are the
zero-allocation path into caller memory, they already work, and `proven_u8str_borrow`
over a stack buffer is genuinely good. They become one writer among several rather
than the only sink there is.

`proven_print` / `proven_println` are then a buffered stdout writer, and cost zero
allocations and one syscall per flush instead of one of each per line.

### 4.3 The reader gives you lines

```text
proven_result_u8str_view_t proven_reader_read_line(proven_reader_t r, proven_u8str_t *scratch);
proven_result_size_t       proven_reader_read(proven_reader_t r, proven_mem_mut_t dest);
```

The scanner then reads from a `proven_reader_t` instead of from either a view or a
`proven_sysio_scanner_t`, and the two scanner families collapse into one.

### 4.4 File operations that are simply absent

```text
proven_result_u64_t proven_fs_seek(proven_file_t f, proven_i64 off, proven_fs_whence_t w);
proven_result_u64_t proven_fs_tell(proven_file_t f);
proven_err_t        proven_fs_truncate(proven_file_t f, proven_u64 len);
proven_result_size_t proven_fs_pread(proven_file_t f, proven_mem_mut_t dest, proven_u64 off);
proven_result_size_t proven_fs_pwrite(proven_file_t f, proven_mem_view_t src, proven_u64 off);

proven_err_t proven_fs_sync(proven_file_t f);              /* fsync */
proven_err_t proven_fs_sync_dir(proven_allocator_t, proven_u8str_view_t path);

proven_result_dir_t proven_fs_dir_open(proven_allocator_t, proven_u8str_view_t path);
proven_result_dir_entry_t proven_fs_dir_next(proven_fs_dir_t *d);
void proven_fs_dir_close(proven_fs_dir_t *d);
```

`proven_fs_sync` + `proven_fs_sync_dir` are what let `write_file_atomic` grow an
opt-in durable mode. Today it cannot have one at any price.

### 4.5 Delete the hand-written syscalls

`platform/proven_sys_io.c` implements `seek` with **inline assembly raw syscalls**,
one per architecture: x86_64 (`syscall`, rax=8), i386 (`int $0x80`, 140), aarch64
(`svc #0`, x8=62). This is gratuitous, and three independent facts prove it:

- `nm -u platform/proven_sys_fs.o` in the **same library** shows `U open`, `U read`,
  `U write`, `U close`, `U mmap`. libc is already linked and already doing file
  I/O. The asm buys no independence from anything.
- Recompiling `proven_sys_io.c` with `-U__linux__`, forcing every branch into the
  plain POSIX `#else` (`lseek`/`read`/`write`), and running the project's own I/O
  suite: **12 of 12 pass**, byte-identical output.
- `./nob cross` skips ten of its eleven targets on a machine without the
  cross-toolchains, so **three of the four asm paths have zero verification here.**
  The aarch64 seek asm uses `"=r"(x0)` as a write-only output while also listing
  `x0` as an input — the weaker idiom; the read/write asm in the same file
  correctly uses `"+r"`.

There is also a real cost to it beyond risk: because the console path issues raw
`syscall` instructions, an `LD_PRELOAD` interposer counts **zero** of
`proven_println`'s ten thousand writes. **Standard tracing tooling is blind to
proven's console I/O.**

Replace all of it with `lseek`.

## 5. What is genuinely fine — stated plainly

An audit that finds everything broken has not been reading carefully.

- **`proven_fs_read_all` / `read_all_u8str` are well built.** Read to EOF rather
  than to a pre-measured size, one allocation for a regular file, correct on pipes
  and `/proc`. The header's notes match the code.
- **`proven_fs_write_file_atomic` is a correct atomicity primitive.** Sibling temp
  + rename, permissions preserved, temp removed on failure. Only durability is
  missing, and it says so.
- **Append mode is correct.** `O_APPEND`, so appends are kernel-atomic.
- **The buffered scanner really does work on non-seekable input**, and its
  rollback-on-failure logic is subtle and right. `proven_scan_fmt_from_file` on a
  pipe correctly returns `PROVEN_ERR_UNSUPPORTED` via a zero-offset seek probe
  rather than eating bytes.
- **Zero-allocation formatting into caller memory already works** —
  `proven_u8str_borrow` over a stack buffer costs zero mallocs. That is the right
  primitive and it is there.
- **The formatter's error detection beats printf.** Too many args is an error
  (printf ignores them). Unknown spec, unmatched brace, bad positional index — all
  caught. Type safety via `_Generic` means the `%d`-with-a-`double` bug class does
  not exist here at all.
- **Its self-alias patching** — a `str_view` argument pointing *into* the
  destination string survives the destination's realloc — is a genuinely
  thoughtful piece of engineering that printf cannot do safely.

## 6. Already fixed (v26.07.12f)

The two silent formatter defects were small, so they were fixed rather than
tracked:

- **`{:08}` was accepted and silently wrong.** The `0` was eaten as the first digit
  of the width, so `{:08}` on 42 produced `"      42"` — space-padded, with no
  error — and `{:08x}` produced `"      2a"`. A near-miss spelling that is accepted
  and quietly does the wrong thing is worse than one that is rejected. A leading
  zero now means zero-fill, as it does in C, Python and Rust. An explicit fill
  still wins.
- **A spec the argument could not honour was ignored.** `{:x}` on a double printed
  `3.500000` and returned OK. It is now `PROVEN_ERR_INVALID_FORMAT`.
- **`proven_sysio_flush`'s documentation was a lie.** It now says exactly what it
  does — nothing on POSIX, a disk sync on Windows — and says not to use it.

`tests/test_regression_fmt_spec_silently_wrong` pins the first two, verified to
fail against the pre-fix source.

## 7. Implementation plan

Ordered so that each step is useful on its own and nothing is a big-bang rewrite.

| Step | Item | Why here | Backlog |
|---|---|---|---|
| 1 | Delete the inline-asm syscalls; use `lseek`. | Pure subtraction. Unblocks tracing tools, removes three unverified code paths, changes no behaviour (proven: 12/12 tests pass with it forced off). | **B-004** |
| 2 | `proven_fs_seek` / `_tell` / `_truncate` / `_pread` / `_pwrite`. | Trivial once step 1 lands. Truncate stops being an O(n) copy. | **B-005** |
| 3 | `proven_fs_sync` / `_sync_dir`. | Small, and it is the only way `write_file_atomic` can ever become durable. | **B-006** |
| 4 | `proven_writer_t` + `from_file` / `from_u8str` / `from_buffer` / `buffered`. | The keystone. Everything downstream is easy after it. | **B-007** |
| 5 | `proven_fmt_to_writer`, `proven_fprint`; re-base `proven_print` on a buffered stdout writer. | Kills the 213× syscall amplification **and** the malloc-per-log-line. | **B-007** |
| 6 | `proven_reader_t` + `read_line`; re-base the scanner on it. | Collapses two scanner families into one, and makes "read a file line by line" possible at all. | **B-008** |
| 7 | Float precision and mode in the spec: `{:.3}`, `{:.3f}`, `{:e}`, `{:g}`, shortest. | The engine exists and is unreachable. Also the only way to align a float column. | **B-009** |
| 8 | `{:c}`, `bool`, `{:X}`, `{:o}`, `{:b}`, `{:#x}`, `{:+}`. | Ordinary completeness. | **B-009** |
| 9 | `proven_arg_custom(user, render_fn)`. | The extension point. With it, the library can add types without touching the core — and so can a caller. | **B-010** |
| 10 | `proven_fs_dir_open` / `_next` / `_close`. | The PAL iterator already exists; `fs.c` just has to stop buffering it. | **B-005** |

Steps 1-3 are subtraction and small additions with no design risk. Step 4 is the
one that needs care, and it is the one that pays for the rest.

Deliberately **not** in this plan: a path module, a temp-file API, and an mtime
setter. They are real gaps, but they are not this RFC's subject, and a plan that
includes everything is a plan that finishes nothing.
