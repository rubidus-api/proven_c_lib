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

### B-007 — streams (closed v26.07.12h)

There was no `proven_writer_t` and no `proven_reader_t`: the formatter's only sink was
a `proven_u8str_t`, a file was a `proven_file_t`, and the two scanners read two other
things again. Four types, four function families, no common interface — so one
`serialize(sink, value)` over both memory and a file was impossible, and formatting
into a file meant building a whole heap string and dumping it.

Both traits now exist, modelled on the allocator trait: a small vtable passed by value,
with **buffering over caller-supplied memory**, because a logging path that allocates
is a logging path that fails when you most need it.

Measured over 10,000 lines:

| | `write()` | `malloc()` |
|---|---|---|
| `proven_println`, before | 10,000 | 10,000 |
| `proven_println`, after | 10,000 | **0** |
| buffered writer, 8 KiB of caller memory | **24** | **0** |

`proven_println` is still one syscall per line on purpose — buffering it would need
hidden global state, which this library does not have. A caller who wants the 24 builds
a buffered writer and says so.

### B-008 — a line reader (closed v26.07.12h)

`proven_reader_read_line`. Reading a file line by line was impossible: the only route
was `read_all_u8str` — the whole file into memory — and splitting by hand.

The two contracts that matter are the ones a naive implementation gets wrong, and both
are pinned by tests: **the final line with no trailing newline is still returned**
(dropping it is how the last record of a file goes missing), and **a line too long for
the buffer is an error, not a truncated line** (a truncated line handed back as if it
were whole is a corruption the caller cannot detect).

### B-004 — the hand-written syscall assembly is gone (closed v26.07.12g)

The PAL implemented read, write and seek in inline-assembly raw syscalls, one path per
architecture. It bought nothing — `proven_sys_fs.c` in the same library already called
libc's `open`, `read`, `write`, `close` — and it cost real things: three of the four
paths were unverifiable on a machine without cross-toolchains, and because the console
path issued raw `syscall` instructions, **standard tracing tooling was blind to every
one of this library's console writes.**

Replaced with plain libc. Proof it was gratuitous: forcing every branch into the POSIX
fallback passed 12 of 12 I/O tests byte-identically *before* the change. Proof the
blindness is fixed: an LD_PRELOAD interposer now counts 5 of 5 writes from five
`proven_println` calls, where it used to count 0.

`tests/test_portability_source_contracts` now *forbids* the assembly rather than
requiring it — an architecture-specific syscall path is something you add on purpose,
not something that creeps back.

### B-006 — durability is reachable (closed v26.07.12g)

The library imported no `fsync` and no `fdatasync`, so a caller who wanted their bytes
on the platter could not ask at any price. Added `proven_fs_sync` (fsync),
`proven_fs_sync_dir`, and `proven_fs_write_file_durable` — which does the three steps
in the only order that works: fsync the temp file, rename, fsync the directory.
Syncing the file but not the directory leaves a crash window in which the bytes are
safe and the name that points at them is not.

`proven_sysio_flush` was never this. It does nothing.

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

### B-005 — `proven_fs_list` still materialises the whole directory

**Status:** open (partially closed in v26.07.12g). RFC-0001 §4.4.

Seek, tell, truncate, pread and pwrite **landed** in v26.07.12g. What remains is the
directory iterator: `proven_fs_list` reads, `fstatat`s, allocates and sorts every
entry before the caller sees any of them. Measured on 50,000 entries: 189 ms,
+4.2 MB RSS, 50,008 mallocs, and nothing visible until the last one. The PAL already
has a streaming iterator (`proven_sys_fs_dir_open` / `_dir_next` / `_dir_close`);
`fs.c` wraps it in a loop that buffers everything and throws the stream away.

**Done looks like:** `proven_fs_dir_open` / `_next` / `_close` exposed, and
`proven_fs_list` reimplemented on top of them rather than the other way round.

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
