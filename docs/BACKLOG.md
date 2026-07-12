# proven — tracked backlog

Work that is known, agreed, and not done yet.

This file is **tracked in git**. The repository already had a `BACKLOGS.md` and a
`TODO.md`, but both are gitignored — a private queue, invisible to anyone reading
the repository and impossible to reference from a commit, an issue, or a code
comment. Known work that lives only on one machine is not tracked work; it is
work that will be rediscovered.

The rule that puts an item here: **fix it now if it is small, register it here if
it is not.** An item is "not small" when fixing it properly would change more
than the thing you came to change — a missing chapter, a mechanism that does not
exist yet, a decision that has not been made.

Each item says what is wrong, what "done" looks like, and why it is not done yet.
An item with no exit condition is a complaint, not a backlog item.

---

## Open

### B-003 — the process is test-after, not test-first

**Status:** open. This is a process item, not a code item. See
`docs/tests/TESTING.md` for the full account.

Regression tests in this repository are genuinely test-first: each one was written
against the *unfixed* source, watched to fail, and only then was the fix written.
That discipline is real and it is enforced — a regression test that passes before
the fix is not a regression test, and the catalog says so.

New *features*, however, are written first and tested afterwards. The whole-file
API (`proven_fs_read_all_u8str`, `write_file`, `write_file_atomic`) was implemented
and then covered. The introsort was written and then fuzzed. That is not TDD, and
calling it TDD would be a lie the next person would have to discover for themselves.

The cost is not hypothetical: the second audit found four defects in the
whole-file API *after* it had been written and tested — a doubled allocation, a
one-byte starting buffer, a permission widening, a `NAME_MAX` failure. Tests
written first, from the contract, would have caught at least the first two,
because they are questions you ask when you are designing the contract ("what
does it cost?", "what happens when the size is unknown?") and questions you do
not think to ask when you are confirming code you already believe in.

**Done looks like:** a decision, recorded in `docs/tests/TESTING.md`, on whether
new public API is written test-first — and if so, the first feature that follows
it end to end, with the failing test in one commit and the implementation in the
next, so the discipline is visible in the history rather than merely claimed.

**Why not now:** it is a change to how the project works, and it should be a
deliberate choice, not something a documentation pass slips in.

---

## Done

Items are moved here with the commit that closed them, so the reasoning survives.

### B-002 — the manual's code blocks were unverifiable sketches (closed v26.07.12f)

Of ~190 fenced `c` blocks, **four** could be compiled. The rest referenced imaginary
helpers, discarded `[[nodiscard]]` results, or were signature listings masquerading as
code — so nothing could tell anyone when they stopped being true, and several had not
been true for a long time.

Every block in every chapter is now either a compiled-and-run program from
`manual/examples/`, a fragment that the build **syntax-checks**, or a `text` fence for
things that are not runnable code. `nob` compiles every chapter's blocks as part of the
build; a chapter that stops compiling stops the build.

Converting them found six more false claims, all fixed: the pool's `item_align` is an
upper bound rather than an exact match; the panic fallback is `while(1)` on non-GCC;
the buffered scanner *does* refill and retry rather than failing on a token that reaches
the end of the buffer; `proven_fs_stat_t.created_at` is always 0; `PROVEN_FS_TYPE_OTHER`
is never produced; and `src/proven/time.c` *is* compiled freestanding (only the clock
backend is absent).

### B-001 — manual chapter 8 was missing sections 7 through 13 (closed v26.07.12e)

The chapter listed thirteen sections and ended at a bare `## 7. Scanner data model`
heading: the whole scanner half was absent, and the scanner is the side with no
libc analogue to fall back on.

Sections 7-13 are written, against the header and against *measured* behaviour
rather than against the header alone — which is how the surprising parts got
documented at all: `proven_scan_i64("0x10")` is decimal zero, `"1e309"` overflows
while `"1e-400"` quietly rounds to zero, and a failed structural scan has **already
written through** the destinations before the mismatch.

`manual/examples/ex_08_scan_recovery.c` provokes every error code on purpose, and
`tests/test_docs_manual_ch08_contracts` asserts each of the 18 behaviours the
chapter states as fact. The chapter cannot drift from the scanner without the build
saying so.

### B-004 — the hand-written syscall assembly should be `lseek`

**Status:** open. See `docs/RFC-0001-streams-and-io.md` §4.5.

`platform/proven_sys_io.c` implements seek with inline-assembly raw syscalls, one
per architecture. It is gratuitous — `proven_sys_fs.o` in the same library already
imports `open`, `read`, `write`, `close` from libc — and it has a real cost:
because the console path issues raw `syscall` instructions, **an LD_PRELOAD
interposer counts zero of `proven_println`'s ten thousand writes.** Standard tracing
tooling is blind to proven's console I/O.

Proven safe to remove: recompiling with `-U__linux__`, forcing every branch into the
POSIX `#else`, passes 12 of 12 I/O tests with byte-identical output. Three of the
four asm paths have zero verification on any machine without cross-toolchains.

**Done looks like:** the asm is gone, `lseek` is called, and the cross matrix still
compiles.

### B-005 — no seek, tell, truncate, positional I/O, or streaming directory iteration

**Status:** open. RFC-0001 §4.4.

`fs.h` has no seek at all. Truncating a file therefore means reading the whole thing
and rewriting it: an O(n) copy for an O(1) operation. `proven_fs_list` materialises
the entire directory — measured on 50,000 entries: 189 ms, +4.2 MB RSS, 50,008
mallocs, and nothing visible to the caller until the last entry is read. The PAL
already has a streaming iterator; `fs.c` wraps it in a loop that buffers everything.

### B-006 — no durability: `fsync` is unreachable

**Status:** open. RFC-0001 §4.4.

The complete list of sync-ish libc symbols the whole library imports is `msync`.
There is no `fsync` and no `fdatasync`, so `proven_fs_write_file_atomic` cannot be
made crash-durable **even by a caller willing to pay for it** — you cannot sync the
file, and you cannot sync the directory the rename happened in.

### B-007 — no stream abstraction; console output is 213x the syscalls of stdio

**Status:** open. **This is the keystone item.** RFC-0001 §4.1, §4.2.

There is no `proven_writer_t` and no `proven_reader_t`. The formatter's only sink is
`proven_u8str_t`; a file is a `proven_file_t`; the scanner reads either a view or a
`proven_sysio_scanner_t`. Four types, four function families, no common interface —
so you cannot write one `serialize(writer, value)` that works over both memory and a
file, and you cannot format directly into a file at all.

Measured: `proven_println` × 10,000 issues **10,000 `write()` syscalls** (stdio: 47)
and **10,000 mallocs** — one heap round-trip per line. **The logging path allocates**,
which is the one place an allocation is least welcome: a program logging its way out
of an out-of-memory condition will fail to log it.

`proven_sysio_flush` is a no-op on POSIX (`ret`, one instruction) and a full disk
sync on Windows. One API, two meanings, neither of them what the name promised. Its
documentation now says so.

### B-008 — no line reader

**Status:** open. RFC-0001 §4.3.

There is no way to read a file line by line. The only route is
`proven_fs_read_all_u8str` — the whole file into memory — then split on `\n` by hand.
Unusable for a file larger than memory; absurd for a log tail. The buffered scanner
is token-oriented and has no delimiter entry point.

### B-009 — the formatter cannot ask for float precision, and has no `{:c}`

**Status:** open. RFC-0001 §3.

`{}` means six decimals, forever. `{:.3}` is `INVALID_FORMAT`. The engine behind
`proven_float_format_f64_policy` does precision, scientific and shortest-round-trip
— **and the `{}` syntax cannot reach any of it.** Consequences: a float column cannot
be aligned (12.5 renders nine wide, 100.0 renders ten, overflowing the column), and a
log line with a latency to three decimals cannot be expressed at all.

Also missing: `{:c}` (there is no way to emit a single character — `PROVEN_ARG('Z')`
prints `90`), `bool`, `{:X}`, `{:o}`, `{:b}`, `{:#x}`, `{:+}`.

### B-010 — the formatter has no extension point

**Status:** open. RFC-0001 §3.

`proven_arg_type_t` is a closed enum. To format a user struct you must pre-render it
into your own buffer and pass a string — which means you cannot then compose it:
`{:>20}` on a user type is impossible unless you pad it yourself. Wanted:
`proven_arg_custom(user, render_fn)`.

### B-000 — the alias layer had drifted (closed v26.07.12c)

25 of 203 public functions had no `xcv_*` alias; 22 had been missing for months.
Closed by adding the aliases and by `tests/test_docs_alias_completeness`, which
parses the headers and fails the build if a public function has no alias. The
lesson is the one B-002 repeats: **a list nobody checks stops being true.**
