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

B-018 … B-023 are argued in [`docs/RFC-0002-view-vocabulary-and-splitting.md`](RFC-0002-view-vocabulary-and-splitting.md),
which is where the measurements and the rejected alternatives live. Each item below says what is
wrong and what closes it; that RFC says why it is the right shape.

The implementation design — exact signatures, boundary tables, algorithms and commit order — is
[`docs/RFC-0003-implementing-the-view-vocabulary.md`](RFC-0003-implementing-the-view-vocabulary.md),
and it covers **B-018 … B-022 and B-024**. It deliberately excludes **B-023**, which touches the
owning string type and shares nothing with the view functions (RFC-0003 §7, §9); B-023 has a
rationale in RFC-0002 §4.6 but no implementation spec yet, and whoever picks it up writes one.

**Where an item below and RFC-0003 disagree on a name or an exit condition, RFC-0003 wins** —
it is newer and it was checked by execution. The exit conditions here have been reconciled with
it once already, and the drift is worth noticing: these items were written from RFC-0002's
sketches, and RFC-0003 renamed two functions and changed what closing B-020 means.

B-025 … B-032 are the manual rewrite, designed in
[`docs/RFC-0004-the-manual-as-a-book.md`](RFC-0004-the-manual-as-a-book.md). They are documentation
work; none of them changes library code.

**Status: the first pass is complete (v26.07.20b line).** All eight phases have run.

| | before | after |
|---|---:|---:|
| English manual | 8,004 lines | **10,192** |
| Korean mirror | 8,310 lines | **9,913** |
| Sections meeting the depth gate's prose floor | 30/88 (34%) | **67/98 (68%)** |
| Sections registered in that gate | 7 | **26** |
| Runnable examples | 18 | **22** |
| Broken internal anchors | 11 (unknown at the time) | **0** |

Every chapter now opens with why the thing exists and what goes wrong without it, declares its part
and prerequisites, and the Korean mirror reflects the current English throughout — chapter 0
included, which it never had.

What is deliberately *not* at the floor, and why: the alias index is a generated table, the
"examples and misuse cases" sections are code by design, and roughly half of the freestanding guide
is flag lists and compile commands. Padding those to clear a word count is the failure RFC-0004 §9
named specifically, so the guide states which of its sections are procedure and which carry a
decision instead.

The items below are kept as the record of what each phase set out to do.

### B-025 — the manual has no on-ramp

There is no "hello world", no statement of what the library is for, no glossary, and no map from
libc to this library. The first runnable program in the manual is at
`manual/manual-01-foundation.md:126` — 27% of the way into chapter 1, after 125 lines of type
tables. A reader who has finished one introductory C book has nowhere to start.

The vocabulary problem is the same problem. `view` appears 288 times, `arena` 115, `trait` 29,
`provenance` 26, `PAL` 18, `failure atomicity` 7 — and none is defined at first use. "Trait" is
not a C word; it first appears inside a table cell in the overview document.

Done when `manual/manual-00-start-here.md` exists with: why the library exists argued from
concrete C failures, a compiled hello-world example, the build and include model, the five
contracts that recur on every page, a glossary, and a libc → proven table. Registered in
`tests/test_docs_manual_examples.c` and mirrored in `manual-ko/`.

Not done yet because it was never anyone's chapter: every existing chapter documents a header, and
the on-ramp documents no header at all.

### B-026 — the chapter order is the header dependency graph, and nothing says so

`01 foundation → 02 allocation → 03 strings → 04 containers → 05 hosted` happens to be a correct
teaching order, since strings and containers need allocators. Nothing states it, no chapter names
its prerequisites, and two files sit wrong: the generated 416-row alias index is chapter 7, and the
formatting deep-dive is chapter 8, after it. Chapters 3 and 8 both document `fmt.h`/`scan.h` with no
stated division — the header map assigns it to chapter 3 while chapter 8 calls itself the deep dive.

Done when `manual.md` declares reading Parts with per-chapter prerequisites, chapter 7 is relabelled
Appendix A, and chapters 3 and 8 each state which half of the text material they are. Renumbering
files is explicitly rejected in RFC-0004 §5.1: it breaks nine hardcoded gate paths, seven gate
heading strings and ten Korean mirrors, to fix something a table of contents already fixes.

### B-027 — two thirds of the manual is below the standard the manual itself sets

`tests/test_docs_manual_depth.c` requires 150 words of prose — outside tables, headings and fences
— plus a reference table and a counter-example. Applied to all 88 H2 sections, **30 pass and 58
fail**. Chapter 1, the entry point, passes **0 of 9**. The thinnest sections carry almost nothing:
`## 3. Memory mapping` has 1 word of prose, `## 5. Alignment helpers` has 2, `## 3. Ring buffer` 15,
`## 2. Intrusive list` 18.

The split is chronological, not topical: everything written for that gate leads with the reader's
problem, and everything older opens with a struct or a table. The style already exists here.

Done when every H2 section in chapters 1–6 leads with why the thing exists and what goes wrong
without it, and clears the 150-word floor. Per RFC-0004 §8 this is phased by chapter, not attempted
at once.

### B-028 — eight modules are documented only as tables

`u16str.h`, `ring.h`, `list.h`, `buffer.h`, `mmap.h`, `time.h`, `algorithm.h` and `align.h` have no
runnable example anywhere in the manual. They satisfy the symbols gate — every function is named in
a table row — and a table row is the floor, not documentation. The hard parts go unexplained: the
intrusive-node model, monotonic versus wall clock, why anyone would map a file instead of reading
it, surrogate pairs and Windows interop for UTF-16.

Done when each has a compiled example in `manual/examples/`, quoted verbatim in its chapter, plus
the motivation prose B-027 requires. `MAX_EXAMPLES` is 64 with 18 used, so the budget holds.

### B-029 — the hardest material in the book is in its second chapter

Chapter 2 §7 covers pointer provenance in the C abstract machine, CAS, the ABA problem, hazard
pointers and epoch reclamation — 130 lines arriving after six sections of ordinary allocator use,
in a chapter a beginner reads early, and nothing later in the manual depends on it.

Done when that material lives in chapter 6 (Part VI, going further), with a short plain statement
of what is and is not thread-safe left behind in chapter 2 and a forward pointer.

### B-030 — chapters 3 and 8 overlap with no stated division

Both document the formatter and the scanner. A reader has two references and no rule for which to
read. Done when chapter 3 is the tutorial half and chapter 8 the reference half, each saying so in
its opening lines, and `manual.md`'s header map agrees with both.

### B-031 — the depth gate covers 7 sections out of 88

The gate that defines the manual's standard is applied to seven hand-registered sections. Every
section rewritten under B-027 should join the register, or the new style is applied rather than
enforced and will decay the way the 58 sections in B-027 did.

Done when the register in `tests/test_docs_manual_depth.c` names every section rewritten in phases
3–6. Deliberately last per chapter: registering a section before it is rewritten blocks the work.

### B-032 — the Korean manual is mirrored by hand and gated by nothing

`test_docs_manual_symbols.c` scans `manual/` only, so `manual-ko/` can drift arbitrarily without any
build failing. It is currently in sync, and every phase of the rewrite is an opportunity for it to
stop being so.

Done when each rewritten chapter has its `-ko` mirror updated in the same release. A gate for it is
worth considering but is not this item: the mirror is a translation, and mechanical equality is the
wrong check.

### B-018 — there is no splitter, so every caller writes one, and the natural one is wrong

`docs/RFC-0001` §1 named this and it is still true: splitting a line on a separator is done
by hand at every call site. The cost is not speed, it is correctness. The loop a competent
person writes first — *"keep going while a separator is found"* — silently drops the tail
after the last separator, and it is wrong on **six of six** obvious inputs including the
common one: `"a,b,c"` yields two fields, not three. Getting it right means hoisting the last
segment out of the loop, which is exactly the step that gets skipped. The measured
alternative callers actually reach for — one owned `proven_u8str_t` per field — costs 3.4×
the time and one malloc per field (1,000,000 allocations over a 6.8 MB corpus).

Done when `proven_u8str_view_split` / `proven_u8str_view_split_next` exist, non-allocating, with
the contract **`n` separators yield `n + 1` fields, always** — where `n` counts non-overlapping
left-to-right occurrences — tested against every row of RFC-0003 §4.1 including `""`, `"a,,b"`
and the empty separator, and manual chapter 3 documents them with the wrong loop as a
counter-example.

(The names carry the `_view_` prefix, settled in RFC-0003 §2. This item originally named
`proven_u8str_split` / `_split_next` from RFC-0002's sketch, which RFC-0003 rejected for
consistency with the seven existing `proven_u8str_view_*` functions.)

Not done yet because the terminating condition needed a decision first, and RFC-0002 §2.3
found the one that forbids the obvious answer: `proven_u8str_view_slice` returns `{NULL, 0}`
for both a legitimately empty field and an out-of-range slice, so validity cannot end the
loop. The iterator carries an explicit `done` flag instead.

### B-019 — a view cannot be trimmed

No `trim`, no `trim_start`, no `trim_end`, no `remove_prefix`, no `remove_suffix`. Trimming
whitespace off a parsed field is the most ordinary thing done to a string and this library
cannot do it without an index loop at the call site. `proven_scan_skip_whitespace` exists but
operates on a `proven_scan_t`, so it is only available to code that has already committed to
the scanner.

Done when the five functions exist, each returning a view into the same memory, with
"whitespace" defined as the six ASCII characters and documented as *not* Unicode-aware.

Not done yet because it was never separated from B-018 — the two arrive together in practice,
and the trim functions are the smaller half.

### B-020 — search runs forwards only

`proven_u8str_view_find` is the whole search surface. There is no reverse search, so "the
extension after the last dot" and "the last path separator" are hand-written scans, and there
is no `contains`, so membership is a comparison against `PROVEN_INDEX_NOT_FOUND` in the
middle of an `if`.

Done when `proven_u8str_view_find_last` and `_contains` exist, implemented as the three paths
RFC-0003 §5 specifies — a backward byte scan for single-byte needles, a backward Shift-Or for
needles up to 64 bytes, and a documented fallback beyond that — and verified against the
brute-force oracle RFC-0003 §6 requires.

(This item originally required `_find_last` to "reuse the existing Two-Way / shift-or machinery
rather than becoming an `O(nm)` backwards scan". Both halves were wrong. `proven_u8_find_shiftor`
is forward-only, so path 2 reuses the *technique* and not the code; and the `> 64` fallback is
knowingly quadratic, tracked as B-024 rather than forbidden here. An exit condition that the
agreed design cannot satisfy is not a standard, it is a trap.)

Not done yet because nothing forced it; the gap only became visible when RFC-0002 listed the
view operations side by side.

### B-021 — views can be compared for equality but not ordered

Only `proven_u8str_view_eq` exists. Sorting an array of views, or using one as an ordered map
key, requires the caller to write the three-way comparison — and byte signedness is one of
the two things people get wrong when they do. `algorithm.h` has the sort and takes a
`proven_compare_fn_t`; what does not exist is a correct string comparison to hand it.

Done when `proven_u8str_view_cmp` exists, lexicographic over **unsigned** bytes,
shorter-is-less on a common prefix, with a test that would fail under `char` signedness.

Not done yet: no reason beyond nobody having needed it inside the library itself.

### B-022 — an empty view and an invalid view are the same value

`proven_u8str_view_slice` returns `{NULL, 0}` for a legitimately empty result and for an
out-of-range request, and the two are bit-identical. That is defensible, but it is undocumented
and it silently disarms the idiom that every other view library uses to end an iteration.

Done when a `proven_u8str_view_is_well_formed` predicate exists with its meaning stated precisely
(structurally sound — `s.size == 0 || s.ptr != NULL`, so `true` for the empty view), and
`u8str.h` plus manual chapter 3 record the ambiguity and say in words that it must not be used as
a loop terminator.

(**Not** `_is_valid`. RFC-0003 §2 rejected that name because `proven_u8str_is_valid` already
exists on the *owning* type with a different meaning, and two predicates separated by one token
is a trap for a reader in a hurry — which is exactly the mistake this item's original wording
would have caused.)

Not done yet because it should land *after* B-018 — the documentation needs to point at the
splitter as the thing to use instead, and pointing at something that does not exist is the
failure mode the documentation gates were built to stop.

### B-023 — nothing checks that a string is destroyed by the allocator that created it

`proven_u8str_t` does not store its allocator, so the caller must pass the same one to
`_create`, `_reserve`, `_append_grow` and `_destroy`. Nothing verifies it. Passing a
different allocator is heap corruption that surfaces later, somewhere else.

Storing the allocator in the struct — as the ACCU talk's `str_buf` does — is rejected in
RFC-0002 §3: it doubles the struct from 4 words to 8 for every string in every array, and it
breaks `proven_u8str_borrow`, which has no allocator at all.

Done when a debug-only field records the identity of the creating allocator and a mismatch
asserts at the offending call, compiled out entirely in release builds.

Not done yet because it is the only item in RFC-0002 that touches the owning type, and it is
deliberately separable from B-018 … B-022, which add only view functions.

### B-024 — the reverse search ships with a slow average case and a quadratic tail

Opened by RFC-0003 §5, and only meaningful once B-020 lands. `proven_u8str_view_find_last` is
specified in three paths: a backward byte scan for single-byte needles, a backward Shift-Or for
needles up to 64 bytes, and — beyond that — a loop over the forward `proven_u8str_view_find`
advancing one byte past each match. Two known weaknesses, both documented in the header rather
than hidden:

**The average case for 2..64-byte needles.** Backward Shift-Or touches every byte. The forward
`proven_u8str_view_find` does not: its default path anchors on the rarest needle byte and skips
ahead with `memchr`, so on ordinary text `find_last` will be materially slower than `find` for
the same needle. That is an accepted trade in the first implementation — one code path, never
quadratic — not a permanent design.

**The tail beyond 64 bytes.** The loop-over-`find` fallback costs `O(n + k*(m + C))` for `k`
occurrences of an `m`-byte needle, where `C` is `find`'s per-call constant (up to a 256-entry
mask build plus a 256-byte entropy sample). It degrades to quadratic on periodic input:
`"aaaa...a"` searched for `"aa...a"` is `n/2` matches each costing `O(m)` to verify. Bounded by
a needle length the caller chooses, so it is not reachable by an attacker who controls only the
haystack — but it is by one who controls both.

Note what this item does **not** claim. An earlier draft justified it by saying the forward
search guarantees worst-case linear time and the reverse one must match. That is false, and
RFC-0003 §1.2 records why: `find`'s default path is an anchored `memchr` scan that is `O(n*m)`
in the worst case, with Shift-Or / Two-Way as an entropy-triggered fallback. The reverse search
is not breaking a guarantee; it is making a different speed/robustness trade, and this item is
about revisiting that trade with measurements rather than about restoring a property.

Done when there are benchmark numbers for `find_last` against `find` on ordinary text, and
either an anchored backward fast path or a recorded decision that the simpler one is good
enough — plus a backward Two-Way for the `> 64` case if the quadratic tail survives review.
Correctness is already covered by the brute-force oracle RFC-0003 §6 requires; this is a
performance item and needs the periodicity corpus (`"aab"` runs, single-byte runs) to be
meaningful.

Not done yet, deliberately: a second search implementation is the highest-risk code in the whole
plan, and blocking ten straightforward view functions on it would be the wrong order.

---

## Done

### B-017 — the standard streams were not writers, and `flush` was a lie (closed v26.07.13h)

`stream.h` had writers, readers, buffered writers, a line reader and a formatter that
writes straight into a writer. `sysio.h` had stdin, stdout and stderr. The two had never
been introduced, and the cost was concrete: **there was no way to read stdin a line at a
time** — the most common thing a program does with it — and the formatter could not be
aimed at a standard stream at all, so every `proven_print` was its own write syscall.
Meanwhile `proven_sysio_flush` claimed to flush a buffer that did not exist: a no-op on
POSIX, a *disk sync* on Windows, and its own header told callers not to use it.

Closed by bridging sysio onto the stream layer: `proven_sysio_stdin_lines` +
`proven_sysio_read_line` (stdin, a line at a time), `proven_sysio_stdout_buffered` (a
thousand small prints, one syscall), and `proven_sysio_stdout_writer` / `_stderr_writer`
/ `_stdin_reader` for the unbuffered path. Nothing re-implements what `stream.c` already
does — a second buffered reader would be a second place for the same bug.

`proven_sysio_flush` is **deleted**, and the delete is the point. Flushing a buffered
writer's bytes to the OS is `proven_writer_flush`; pushing the OS's bytes to the disk is
`proven_fs_sync`. They are different things and now say so. Buffering stays opt-in, and
nothing registers an atexit handler to flush behind the caller's back.

### B-016 — randomness was one algorithm, and the wrong one for most jobs (closed v26.07.13h)

`random.h` offered exactly one thing — the OS CSPRNG — and said, in its own header, that
this was deliberate: a fast PRNG and a secure one are different tools, and shipping one
under a name that suggests the other is how insecure tokens get written. The reasoning was
right. The conclusion — offer neither — was wrong. A caller who needs a reproducible
sequence does not stop needing one: they write `rand()`, or a hand-rolled LCG, and end up
with something worse than what the library declined to give them. And a bare-metal target,
which has no OS CSPRNG at all, was left with nothing.

Closed by organising the module by use case, the way `hash.h` is:

- `proven_xoshiro256ss_t` — fast and **reproducible**, for simulations, tests, games. Named
  so it cannot be mistaken for a secret-grade generator, which it explicitly is not.
  SplitMix64-expanded seeding, so seed 0 (or 1, or 2 — what callers actually pass) is not
  degenerate.
- `proven_chacha_rng_t` — cryptographic, and pure arithmetic: it needs no OS once seeded,
  which makes it the answer on a bare-metal target and the fast answer for bulk random data
  on a hosted one. Verified byte-for-byte against the standard's keystream.
- `proven_random_bytes` — the OS CSPRNG, unchanged.
- `proven_rng_below` / `_range` / `_f64` / `_shuffle` — unbiased, over any source. `% n` is
  biased and everyone writes it anyway.

The trait (`proven_rng_t`) is **infallible** by design: asking an OS for entropy can fail,
so that failure is confined to seeding, checked once, and every draw downstream is total.
The generators are pure computation, so `random.h` is now available freestanding; only the
OS entropy source stays hosted.

### B-015 — map hashed untrusted string keys with a non-keyed hash (closed v26.07.13d)

**Decision taken (2026-07-13): SipHash by default, with a trusted-key opt-out.** `map` now
hashes string keys with keyed SipHash-2-4 under a per-process secret from the OS CSPRNG, so an
attacker who controls the keys cannot compute collisions and flood a bucket. `proven_map_create`
is the safe default; `proven_map_create_trusted` keeps fast FNV for keys the program chooses
itself; `proven_map_hash` makes the choice observable. The secret is seeded exactly once even
under a 64-thread race (TSan-verified), integer keys are unchanged, and a freestanding build
falls back to FNV. The measured cost of the default is ~9% on insert and ~40% on a short-key
lookup — which is why the opt-out exists. Required a new randomness PAL (`proven/random.h`).

### B-014 — no hash / digest module (closed v26.07.13c)

lowent's case study asked for this by name: content-addressed IR needed a cryptographic
digest, proven had no hash of any kind, and the project hand-wrote BLAKE3-256 - "exactly
the kind of code that should not be hand-rolled". Closed by `proven/hash.h`, organised by
use case: `proven_hash_bytes` (FNV-1a) for trusted-key tables, `proven_hash_keyed`
(SipHash-2-4) for untrusted-key tables, `proven_crc32` (IEEE, streaming) for corruption
detection, and `proven_sha256` (FIPS 180-4, streaming, with git-style hex) for
fingerprinting. All royalty-free, implemented from spec, checked against each standard's
known-answer vectors and differentially against Python hashlib/zlib and an independent
SipHash over every length to 300. The second feature written test-first. The map-HashDoS
question it makes answerable is tracked as B-015.

### B-003 — the process was test-after (closed v26.07.13b)

**Decision (2026-07-13): new public API is written test-first, in separate commits** — the
contract and a failing test in one, the implementation in the next. Recorded in
`docs/TESTING.md` §5.1, with the reasoning and the case study (the whole-file API, which was
written first, tested afterwards, and then found to have four defects an audit had to catch).
`proven_fs_walk` is the first feature to follow the rule end to end; `git log` shows the two
commits.

Defect fixes were already test-first in method and stay that way.

### B-011 — the adversarial audit was ad hoc (closed v26.07.13b)

**Decision (2026-07-13): the adversarial audit is a standing part of the process.** It runs on
every new module or public API, on any module that has never had one, on the fixes an audit
itself produced (that is where the next bugs are — one such round found three regressions,
including a heap-buffer-overflow), and before a release. A reproducer is required before any
finding may be reported. Recorded in `docs/TESTING.md` §5.2, with the evidence: pointed at the
modules that had never had it, it found a defect of consequence in every single one.

Items are moved here with the commit that closed them, so the reasoning survives.

### B-013 — the second audit round (closed v26.07.13a)

Pointed at the code written that same day, at the allocators, and at the filesystem. It
found, and every one was reproduced first:

- **`close()` failures were discarded**, and `close()` is the last chance a filesystem has
  to say a write did not land — on NFS, CIFS or over quota, the only chance. `write_file`
  returned `PROVEN_OK` for a file the filesystem had refused; `write_file_atomic` renamed
  the temp over the target anyway. `proven_fs_close` returns an error now, and is
  `[[nodiscard]]` so a write path cannot ignore it.
- **the scanner's rollback wrapped `cursor` and `length` to ~2^64** — introduced by the pipe
  fix earlier the same day. Heap-buffer-overflow on a file (ASan), silent buffer discard on
  a pipe. New code is where new bugs are; that is the whole lesson of B-011.
- an atomic write left the whole payload of a 0600 file in a **world-readable temp** for the
  duration of the write; `proven_fs_copy` widened 0600 to 0644;
- symlinks, FIFOs and devices were reported as **regular files**;
- `proven_mmap_sync` on a PRIVATE mapping reported **success while persisting nothing**;
- `{:f}` refused any double above ~1e121 as a *bad format string*;
- the buffered reader **dropped the bytes of an EOF that carried them**;
- the allocator **trait** answered `alloc(0)`, `realloc(ptr, 0)` and a non-tail shrink
  differently depending on which allocator you were handed.

### B-012 — the audit findings (closed v26.07.13a)

Every one reproduced before it was fixed, every fix pinned by a regression test that was
checked to fail against the unfixed source:

- the exact float tier compared against a **rounded** `5^q` (`float_decimal.c`), so every
  exact halfway value in the `56..350` exponent window broke to a fixed direction instead
  of to even — 2,923 misroundings against glibc, all reporting `PROVEN_OK`;
- the buffered scanner treated a **short read as EOF** and latched it, so a token
  straddling a pipe's read boundary was committed **truncated** and the rest of the stream
  became unreachable; a failed read was reported as a clean end of input;
- the scan engine could not say *"I ran out of input"*, so a number or a literal cut in
  half by the read boundary was reported as malformed rather than waited for;
- `map_rehash` doubled unconditionally, so tombstone pressure grew a steady-state cache
  **forever** (100 live entries → 33 MB);
- `proven_u8str_append_grow` / `proven_u16str_append_grow` of an **empty view** left a
  freshly allocated block unterminated, and `as_cstr` read past the end of it;
- `insertion_sort` handed the caller's comparator a scratch buffer of alignment **1**;
- the panic handler was a data race;
- the buffered writer **duplicated bytes** on a partial write, and the buffered reader
  turned an I/O error into a clean EOF;
- `{:f}` did not force the fixed form;
- `proven_fs_dir_next` reported a failed `readdir()` as end-of-directory.

### B-005 — streaming directory iteration (closed v26.07.12i)

`proven_fs_list` read the WHOLE directory before the caller saw any of it. Measured on
20,000 entries: 80 ms and **+1,664 KB of resident memory**, with nothing visible until
the last entry was read.

`proven_fs_dir_open` / `_next` / `_close` walk it one entry at a time: 64 ms and
**+0 KB**. The entry name is *borrowed* from the iterator's storage — which is the
point, and is what lets a million-entry directory be walked without a million
allocations. `proven_fs_list` remains, because materialising a small directory is often
exactly what you want.

### B-009 — the format spec grammar (closed v26.07.12i)

Precision (`{:.3}`, `{:.0}`), fixed and shortest float forms (`{:f}`, `{:g}`), bases and
case (`{:x}` `{:X}` `{:o}` `{:b}`), alternate form (`{:#x}`), sign (`{:+}`, `{: }`), and
`char` / `bool` argument types.

The float gap was the one that mattered: every float came out with **exactly six
decimals, forever**, so a float column could not be aligned — 12.5 rendered nine
characters wide and 100.0 rendered ten, and the column broke. The exact engine could
always do precision; the `{}` grammar simply could not reach it.

Two silent defects were found while doing it, and both are pinned by tests:

- The float engine rewrote **precision 0 to 6**. "No decimals" is what `%.0f` means
  everywhere, and answering it with six decimals is the same disease as accepting a
  spec and ignoring it.
- The first version of the new integer renderer assembled the padded number in a fixed
  128-byte buffer, so `{:#0200x}` — a legal request, since the parser allows a width up
  to 10000 — **silently produced 127 characters and returned PROVEN_OK**. The padding
  is now emitted rather than assembled.

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

### B-010 — the formatter had no extension point (closed v26.07.12i)

`proven_arg_type_t` was a closed enum, and `_Generic` cannot be taught a type it was
not told about at compile time, so a user struct could not reach `{}` at all. The ways
out were to pre-render into a scratch string (an allocation and a copy per value, in
the logging path) or to print the fields one at a time and give up on alignment.

Closed by `proven_arg_custom(obj, render_fn)` / `PROVEN_ARG_OF(&obj, render_fn)`. The
renderer receives a `proven_fmt_sink_t`, not a buffer, so it composes — it may call the
formatter again — and the formatter runs it **twice**, once against a counting sink, so
that `{:>20}` aligns a user type exactly as it aligns an int, without allocating. A
renderer whose two passes disagree is an error, not a silently misaligned column, and a
spec the library cannot interpret for a user type (`{:x}`, `{:.2}`, `{:+}`) is refused
rather than guessed at. Covered by `tests/test_unit_fmt_custom` and
`manual/examples/ex_08_fmt_custom.c`; documented in manual chapter 8 §5.1.

### B-000 — the alias layer had drifted (closed v26.07.12c)

25 of 203 public functions had no `xcv_*` alias; 22 had been missing for months.
Closed by adding the aliases and by `tests/test_docs_alias_completeness`, which
parses the headers and fails the build if a public function has no alias. The
lesson is the one B-002 repeats: **a list nobody checks stops being true.**
