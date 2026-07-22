# Changelog

All notable changes to this project will be documented in this file.

The format follows Keep a Changelog:

- keep the log human-curated
- order entries chronologically with the newest first
- group notable changes by release or by `Unreleased`
- use the standard sections `Added`, `Changed`, `Deprecated`, `Removed`,
  `Fixed`, and `Security` when they apply
- avoid dumping raw commit history into the file

## [2026-07-23] — proven_c_lib-v26.07.23a

Operating-convention cleanup. No library code changed; `src/` and `include/` are identical
to v26.07.20g apart from the version constants. The change is to the project's own process
docs, which had accumulated redundancy and one stale claim.

### Changed

- **The release checklist and the G7 gate description now name `README-ko.md` explicitly.**
  `CHECKLIST.md` and `docs/DOCUMENTING.md` still said to sync "`README.md` (both language
  halves)" — wording left over from when the README was one bilingual file. The README has
  since been split into `README.md` (English) and `README-ko.md` (Korean), and the actual
  gate (`test_docs_version_sync`) already checks them separately; only the prose lagged. It
  now matches the gate. `docs/operations/README.md` likewise names both READMEs in its
  document-update rules.

- **The resume-packet model is consolidated.** `docs/operations/README.md` and
  `scripts/check-docs.py` now describe a single local queue (`BACKLOGS.md`) and a resume
  packet that lives in `CONTEXT.md`, replacing the previous split across separate handoff
  and backlog files.

### Removed

- **Stale reference to a retired local file** in `docs/BACKLOG.md` (it named a second
  gitignored queue that no longer exists).

## [2026-07-20] — proven_c_lib-v26.07.20g

The provenance section answers the fair objection — "every example is contrived" — instead of
dodging it. No library code changed; `src/` and `include/` are identical to v26.07.20f apart from
the version constants.

### Added

- **An honest section on why the runnable examples look artificial**, because they have to. A
  compiler only exploits provenance when it can *see* where a pointer came from, and that
  visibility is exactly what a small example has and a realistic one hides behind a `malloc` or a
  function call. Every realistic idiom that reconstructs pointers by arithmetic — tagged pointers,
  XOR linked lists, a `refcount` header reached through `data[-1]`, a `uintptr_t` round trip — was
  compiled at `-O2`/`-O3` and **all produced the correct answer**, because the model WG14 chose
  (PNVI-ae-udi, exposed addresses) was designed to keep those idioms working. The dangerous part is
  that "mostly works": the rule is still in force and only waits for enough visibility.

- **A latent example that looks like production code**, not a puzzle: a `checksum(head, 64 + 192)`
  with an off-by-count loop that walks a pointer derived from `head[]` past its end. Compiled on the
  build machine, it prints `448` at `-O0` (reads out of bounds into adjacent memory) and `64` at
  `-O2` (the compiler knows the pointer came from a 64-element array and silently drops 192 of the
  256 iterations — no warning). Neither is the sum the author intended, and the two disagree by
  optimisation level alone. This is the shape a real provenance bug ships in: not a reproducible
  crash, but a correct-looking function with an expiry date set by the toolchain — and the argument
  for keeping bounds *with* the data, where "process both buffers as one" cannot be written by
  accident.

Mirrored in `README-ko.md` in 합니다체.

## [2026-07-20] — proven_c_lib-v26.07.20f

The provenance section now opens with the shock instead of the definition. No library code changed
— `src/` and `include/` are identical to v26.07.20e apart from the version constants.

### Changed

- **The first provenance example is now a runnable program that prints its own contradiction.** The
  previous lead — `int *p = a + 4;` with a comment — asked the reader to take the point on faith and
  invited the reaction "so what?". It is replaced by a complete program whose `-O2` output is
  `*p = 11, *q = 2`: two pointers holding a bit-for-bit identical address (checked with `memcmp`),
  where dereferencing one gives `11` and the other gives `2`. **One address, two values**,
  deterministically, every run — because the compiler tracks that `p` came from `x` and keeps `y` in
  a register. Verified on the build machine (GCC 14.2), stable across `-O2` and `-O3`.

- **The section leads with that program and the "two rules" contrast references back to it**, rather
  than showing the same provenance bug twice. The strict-aliasing example and the
  `-fno-strict-aliasing` distinguishing table are unchanged.

- The README is honest that GCC *warns* about the `&x + 1` store in the lead example — and
  miscompiles it anyway, which is more unsettling than a silent one, not less.

Mirrored in `README-ko.md` in 합니다체.

## [2026-07-20] — proven_c_lib-v26.07.20e

The provenance section is corrected and made honest. No library code changed — `src/` and
`include/` are identical to v26.07.20d apart from the version constants.

### Fixed

- **The provenance example in the README was wrong, and it was pointed out.** It showed
  `int *p = a + 4;` and claimed `*p` is undefined "because provenance". Forming a one-past-the-end
  pointer is explicitly *legal*; `*p` being undefined is just an out-of-bounds read and demonstrates
  nothing about provenance. The example now says what is actually true: `p` and `b` can hold the
  same address and still not be the same pointer, because `p` remembers it came from `a`, and using
  `p` to reach `b` is what the compiler assumes cannot happen.

### Added

- **A reproducible provenance miscompilation, verified on the build machine.** Two `int *`
  pointers, no type punning, only the origin differs: at `-O1` a write through `&x + 1` reaches
  `y`; at `-O2` the same write, to a bit-for-bit identical address, does *not* — the compiler keeps
  `y` in a register because `p` came from `x`. Every number in the README was produced by compiling
  and running the example (GCC 14.2), not asserted.

- **Provenance and strict aliasing, separated and contrasted.** They are two distinct rules — one
  asks *what type* is at an address, the other *which object* a pointer may reach — and the README
  now proves it: `-fno-strict-aliasing` fixes a strict-aliasing miscompilation and does **nothing**
  for the provenance one. Both bugs are shown, with the exact compiler output and the flag that
  distinguishes them. The strict-aliasing example is also placed in
  [manual chapter 0 §2](manual/manual-00-start-here.md) as the motivation for `proven_byte_t`.

- **An honest limit.** The library does **not** claim strict-provenance purity, and the README says
  why: the intrusive list's `container_of` — `(type *)((proven_byte_t *)ptr - offsetof(...))` —
  recovers a whole struct from a pointer to one of its members, an idiom the strictest readings of
  the object model have never comfortably blessed and that is nonetheless everywhere real C lives,
  the Linux kernel included. So the library defends the settled, agreed-upon undefined behaviour and
  treats the unsettled frontier — where a dominant technique sits at odds with the strict model — as
  exactly that. Provenance is the direction it leans, not a finished guarantee.

## [2026-07-20] — proven_c_lib-v26.07.20d

The README explains what the library is *for*. No library code changed — `src/` and `include/`
are identical to v26.07.20c apart from the version constants.

### Changed

- **`README.md` and `README-ko.md` rewritten for a reader who has just finished one C book.**
  343 → 567 lines. The previous README opened with a feature list; this one opens with the
  problems a beginner has already hit — `strcpy` not knowing the destination size, `malloc`'s
  ignorable `NULL`, `printf("%d", 3.0)` compiling — and works outward from there.

- **The name is explained, and it is not what it looks like.** `proven` comes from **provenance**,
  not from *prove*. The new section explains provenance as C's memory model uses it — a pointer
  carries the identity of the storage it came from, so two pointers can hold the same value and
  still not be interchangeable — with a worked example and the WG14 Technical Specification
  ([N2577](https://www.open-std.org/jtc1/sc22/wg14/www/docs/n2577.pdf) →
  [N3005](https://www.open-std.org/jtc1/sc22/wg14/www/docs/n3005.pdf), PNVI-ae-udi), stated
  accurately as a TS pending publication rather than as part of C23.

  It also records why the library exists at all: the author met strict aliasing and provenance
  late, found that C's rules about memory are considerably stricter than the mental model he had
  been carrying, and concluded — **precisely because he does not hold those rules reliably while
  writing ordinary code** — that they belong in a library and in conventions visible in every
  signature rather than in a programmer's attention. That `proven` also happens to mean *tested*
  is a coincidence of two unrelated Latin roots, and the accident describes the project better
  than the intention did.

- **"C is not portable assembly" is argued rather than asserted.** Effective types and strict
  aliasing (memory in C's model *has a type*; assembly has no such concept), provenance, and the
  fact that undefined behaviour means the standard imposes no requirement at all — not "whatever
  the machine does". The summary the section lands on: C is permissive about what you can write
  and strict about what it promises, and the "portable assembly" view conflates the two.

- **Where systems languages are going**, because this library is C's version of the same answers:
  the length-beside-pointer table from Pascal's length prefix through C++17 `string_view`, Rust's
  `&str`, and Zig's slices; why owned and borrowed must be *different types*; the allocator as a
  parameter, with heap/arena/pool compared; and what C99, C11 and C23 already provide. Links to
  Luca Sas's ACCU 2021 talk and Andre Weissflog's *Modern C for C++ Peeps*.

- **What the library is for**, in three parts: replacing the tired parts of the standard library
  from the bottom up **without excluding it**, being pleasant for a person and safe for a person
  working with an AI (because every important fact is local to the call), and testing in public
  whether modern C actually holds up.

### Fixed

- **`README-ko.md` now uses one register throughout.** It had been mixed — the new material in
  평서체, the older half inconsistent within single sections. It is 합니다체 end to end, with the
  first-person origin story kept personal rather than bureaucratic. Verified: every code fence
  identical to the English modulo Korean comments, and zero 평서체 endings outside code.
- **Appendix A's Korean mirror** was the one reference chapter still in 경어체; it now matches
  chapters 1–8, and gained the "this is a lookup table, not reading material" opener the English
  side already had.

## [2026-07-20] — proven_c_lib-v26.07.20c

The manual becomes a book. No library code changed — `src/` and `include/` are identical to
v26.07.20b apart from the version constants.

### Added

- **Chapter 0, the on-ramp that did not exist.** Until now the manual's first page was a table of
  fixed-width type aliases and the first runnable program was 126 lines into chapter 1. A reader
  who had finished one introductory C book had nowhere to start, and no glossary for the words the
  manual used as if they were ordinary C — `view` appeared 288 times, `arena` 115, `trait` 29,
  `provenance` 26, none defined at first use.

  `manual/manual-00-start-here.md` states who the manual is for, argues why the library exists
  from five C bugs the reader has already met — `strcpy` not knowing the destination size,
  `malloc`'s ignorable `NULL`, `printf` believing the format string, `char *` meaning four
  different ownerships, `qsort`'s untypecheckable comparator — and says what each answer **costs**.
  It ships a compiled hello-world, the build model, the five contracts that recur on every page, a
  glossary, and a libc → `proven` map.

- **A declared reading order.** The chapter order was the header dependency graph and nothing said
  so. `manual.md` now declares six parts with prerequisites and an outcome for each, every chapter
  names what you should have read first, chapter 7 is relabelled Appendix A (a lookup table, not
  reading material), and chapters 3 and 8 state which half of the text material they are.

- **Four new runnable examples** — `ex_00_hello`, `ex_04_list`, `ex_04_ring`, `ex_05_time` — each
  compiled and run by the build like the existing eighteen.

### Changed

- **Every chapter now leads with why the thing exists.** Measured against the manual's own standard
  — `test_docs_manual_depth`'s 150 words of prose outside tables, headings and fences — sections
  meeting it went from **30 of 88 to 67 of 98**. Chapter 1, the entry point, went from 0 of 9 to 6
  of 9.

  Chapter 1 opens on the error model rather than type tables. Chapter 2 opens on the heap — the
  obvious case — and shows the allocator trait fourth, once you have used three allocators.
  Chapter 3 opens on the 1972 decision to end a string with a zero byte. The dynamic array opens on
  the two bugs in the resize line everyone writes by hand. Memory mapping, which had **one word**
  of prose, now explains that its failure modes are signals rather than return values.

- **The hardest material moved out of the second chapter.** Pointer provenance, CAS, ABA, hazard
  pointers and epoch reclamation are now chapter 6 §3, where the rest of the concurrency subject
  is, instead of arriving after six sections of ordinary allocator use.

- **The depth gate's register grew from 7 sections to 26**, so most of the new writing is enforced
  rather than merely applied.

- **The Korean mirror reflects the current English throughout**, chapter 0 included — it never had
  one. 8,310 → 9,913 lines. Chapter 0 is deliberately written in 경어체 while the reference
  chapters keep 평서체, and says so in the text.

### Fixed

- **A code block that had never been compiled by anything.** An indented ```c fence inside a bullet
  list opens a block whose indented closer the extractor cannot see, so it hit end-of-search and
  `break` — silently skipping that block *and every block after it in the chapter*. In chapter 2 it
  was the last section, so nothing followed and nothing showed. `nob.c` now fails on an
  unterminated block instead of breaking out of the loop.
- **The chapter 1 version excerpt had drifted** and only one of its three lines was checked:
  the manual said `PROVEN_VERSION_NUM 260713` and `SUFFIX "m"` while `version.h` said `260720` and
  `"b"`. All three lines are gated now. A partly-checked quotation is worse than an unchecked one,
  because it looks verified.
- **Eleven broken internal anchors**, found by auditing every `](#...)` in all 21 manual files.
  Two were introduced by renumbering chapter 6; seven were English anchors under Korean headings in
  the Korean chapter 4; two were pre-existing, including one where an em dash in a heading produces
  a double hyphen in the slug. Zero remain.

### Note

Design work only. `docs/RFC-0004-the-manual-as-a-book.md` is the plan this executed, and
B-025 … B-032 in `docs/BACKLOG.md` record what each phase set out to do.

## [2026-07-20] — proven_c_lib-v26.07.20b

The repository's non-C checks are part of the build now, and the one thing they had ever
reported turned out to be noise.

### Fixed

- **`scripts/project-check.sh` runs from `./nob`, before anything is compiled, and a failure
  fails the build.** It had been failing continuously since 2026-07-12 — through six releases,
  v26.07.13g to v26.07.13m — and nobody found out, because it was not part of any command anyone
  ran. This is the same lesson the documentation gates were built on, arriving from the other
  direction: the gates work because the build runs them, and a check the build does not run is a
  check that stops being true without telling anyone.

  Four details, each one a defect found while wiring it:

  - **Early, not late.** Placed after the tests, a documentation failure surfaces through
    `test_portability_nob_std_probe` — which invokes this driver as a subprocess and reports
    *"build driver completes with the fallback compiler standard"*. True, useless, and three
    steps from the cause. Verified in both directions: a planted violation now stops the build
    with **zero tests run** and names the offending file.
  - **Skipped for `regression` and cross runs**, so the check does not run twice per build (the
    probe above invokes `./nob regression`), and because a cross matrix links no hosted tests.
  - **Skipped loudly, never silently.** No script, no POSIX shell, or no `python3` each log a
    SKIP with the reason rather than passing quietly.
  - **Logged under `[PROVEN][PROJECT_CHECK]`.** `[PROVEN][CHECK]` already belongs to the test
    framework's assertions (`tests/proven_test.h:29`), and two things writing the same tag makes
    both unreadable.

- **The privacy scan's only finding was a false positive, and the example was wrong, not the
  scan.** `manual/examples/ex_03_u8str.c` built a throwaway path under the host's storage-pool
  mount prefix while demonstrating string editing. That prefix is a real private location on the
  machine this library is developed on — it is where the development container's own root
  overlay lives — so the scan was right to reject it and the example had no reason to use it.
  Changed to `/srv/etc/fstab` in the example, the chapter that quotes it, and the Korean mirror.

  This entry deliberately does not reproduce the old path. The first draft of it did, and **the
  new gate failed the build on the changelog** — which is the gate working, one commit after it
  was installed.

  The pattern was **not** narrowed to make the message go away. Weakening a privacy scan to
  silence a collision is how the real leak ships six months later; the example path was
  arbitrary and the lesson it teaches — inserting a prefix — is unchanged.

### Note

No library code changed. `src/` and `include/` are identical to v26.07.20a apart from the
version constants; the diff is `nob.c`, one example program, and two manual chapters.

## [2026-07-20] — proven_c_lib-v26.07.20a

A design release. No library code changed — `src/` and `include/` are identical to v26.07.13m
apart from the version constants. What shipped is two RFCs, the work that produced them, and two
harnesses that make them checkable rather than merely readable.

### Added

- **RFC-0002 — the view vocabulary, and the splitter every caller writes wrong.** Read Luca Sas's
  *Modern C and What We Can Learn From It* (ACCU 2021) against this library. The useful result was
  mostly negative: the talk argues for the owning/non-owning string split, values over
  out-parameters, allocator-as-parameter, a typed formatter extension point and a freestanding
  posture — and this library already does all of it, in several places more thoroughly than the
  talk proposes. The gap it exposes is narrow and entirely on the **non-owning** side. Of the nine
  view operations the talk shows, this library has three.

  The centre of it is splitting, and the finding is not that splitting is slow here. It is that
  there is no splitter, so every caller writes one, and **the loop a competent person writes first
  is wrong on six of six inputs** — including `"a,b,c"`, which silently loses `c`. The measured
  alternative people actually reach for, one owned string per field, costs 3.4× the time and **one
  malloc per field**: a million allocations to read 6.8 MB.

- **RFC-0003 — implementing the view vocabulary.** Exact declarations, every boundary as a table,
  the algorithms with their real complexity, the six files a public symbol costs in this
  repository, and a commit order. Writing it invalidated four things the design document stated
  confidently, and each correction is recorded rather than quietly patched:

  - `proven_u8str_view_find` returns `start_offset` for an empty needle, not `NOT_FOUND`, so
    RFC-0002's sketched iterator advances by zero bytes and **hangs**.
  - The forward search is **not** "shift-or for short needles, Two-Way for long ones" — that is
    its entropy-triggered fallback. The default path is a rarest-byte anchored `memchr` scan,
    `O(n·m)` in the worst case. The reverse-search design no longer rests on a worst-case
    guarantee the library does not actually make.
  - `_cmp` cannot inherit NULL-safety from a platform function three layers down: `{NULL, 5}` is
    representable and would compare five bytes against a NULL pointer.
  - The first draft's commit plan put the failing test before the header, which does not compile
    and violates the test-first rule it cited.

- **Two harnesses, so the documents can be checked instead of believed.** Neither is built by
  `./nob`; both compile against the library directly and the command is in the file.

  - `docs/rfc-0002-benchmark.c` reproduces every number in RFC-0002 §2 — the six-of-six wrong
    table, ns/field and allocation counts for four splitting strategies, and the
    empty-versus-invalid view demonstration. RFC-0001's measurements were never committed and are
    now unreproducible; this is the correction.
  - `docs/rfc-0003-spec-check.c` **executes RFC-0003's specification** against its own tables: all
    thirteen rows and all three properties, plus 200,000 randomised cases. This is how a hole was
    found before it shipped — an ill-formed `{NULL,5}` source was yielded as a five-byte field
    over a NULL pointer, with eleven of twelve rows green.

- **B-018 … B-024 in `docs/BACKLOG.md`**, each with an exit condition reconciled against RFC-0003.

### Fixed

- **RFC-0002's two descriptions of the substring search**, corrected in place with a pointer to
  the evidence rather than silently rewritten.
- **Three backlog exit conditions named APIs the design had already rejected**, which made them
  unclosable — and B-022's original wording would have led a contributor to ship the exact naming
  trap RFC-0003 exists to avoid.
- **A coverage claim that was not itself checked.** `rfc-0003-spec-check.c` claimed to run "every
  row" and "the two properties" while skipping the NULL-argument row and two of the three
  properties. It now checks them, and prints how much it exercised (380,883 fields, 86,325 forked
  iterators) so a vacuous pass is visible. An unchecked coverage claim is the same defect as an
  unchecked specification, one level up.
- Both `malloc` calls in the benchmark harness report failure instead of segfaulting in `memcpy`.

### Note

Nothing in RFC-0003 is implemented. The `n` separators → `n + 1` fields contract cannot be
revisited once callers depend on it, so it is the decision to settle before code.

## [2026-07-15] — proven_c_lib-v26.07.13m

### Added

- **The documentation rules are gates now, and the manual's claims are assertions.** You cannot
  test-drive prose — but almost nothing that has gone wrong in this manual was a matter of taste.
  It was a claim that had stopped being true, a symbol that no longer existed, a number that
  disagreed with itself, a section that was listed instead of explained. Each of those is a
  *proposition*, and a proposition can be checked by the build. Five new gates, each one
  motivated by a failure that already happened:

  - **A function the manual documents must exist.** `proven_sysio_flush` was deleted and the
    manual went on declaring it as public API, in the present tense. **`proven_pool_free` never
    existed at all** — the real symbol is a static `proven_pool_free_trait`, and freeing a pool
    slot goes through the allocator trait — and the manual described it as a callable function for
    as long as the manual has existed. Fixed, and guarded. A reader who follows the manual and
    gets a *linker* error stops believing the rest of it.
  - **Every public function must be named in the manual** (the streaming directory API went
    undocumented for months).
  - **The version string must agree with itself** across `version.h`, the README's two halves,
    TEST.md, the manual headings, chapter 1's excerpt and the CHANGELOG's newest entry.
    `CHECKLIST.md` always required it; nothing checked, and `version.h` once sat five releases
    behind the CHANGELOG.
  - **Every module section must be documented to depth** — real prose, a reference table, the
    structures the caller declares, a runnable example, and **at least one counter-example**. The
    five modules added this cycle each had an intent paragraph and a table and *not one had a
    counter-example*; they passed every check that existed and were still half-written. The gate
    found two more gaps the moment it existed (the tree walk's entry struct, and its
    borrowed-view trap), both now filled.
  - **Every factual claim must be true** — the oracle. Twenty assertions drawn straight from
    sentences the chapters state as fact: the CRC check value the interoperability promise rests
    on, the standard digest, chunk-independence, base64url's missing padding, a refused call
    writing *nothing*, an unseeded generator being inert, a line that exactly fills the buffer
    being returned while a longer one is refused.

  The rule that makes it work, and the useful half of the idea: **when you write a sentence a
  reader could act on, write the assertion for it.** If you cannot state the assertion, the
  sentence is too vague to be in the manual.

- **`docs/DOCUMENTING.md`** — the process (survey → plan → edit → verify) and the gate table, with
  the failure that motivated each gate. `CHECKLIST.md` points at it.

### Fixed

- **`proven_pool_free`, a function the manual documented and which does not exist.** Freeing a
  pool slot goes through the allocator trait (`proven_pool_as_allocator`), like every other
  allocator.
- The tree walk's `proven_fs_walk_entry_t` was never listed, and its borrowed-`path` trap — every
  entry aliases one reused buffer — had no counter-example.

## [2026-07-15] — proven_c_lib-v26.07.13l

A documentation release. The library grew five modules this cycle and the manual had kept up
only in the sense that it *mentioned* them; this brings them to the depth of the chapters
around them, and finishes the job.

### Changed

- **The ownership matrix has a second class.** It used to be a list of things you must destroy,
  and the sixteen public structs added this cycle destroy nothing — they are a different kind of
  object, and it needed a name: **caller-owned state**. You declare one, hand its address to a
  constructor, and use the handle you get back. The rule owning objects do not have is now
  stated where a reader meets it before they hit it: *a caller-owned state object must not be
  copied or moved once a handle has been made from it — the handle holds a pointer into the
  struct.* All sixteen are tabulated with what constructs them and their individual sharp edge
  (copying a seeded generator clones the keystream: two "independent" tokens become one token).

- **The new modules got their depth.** Hashing, encoding, randomness, streams and the standard
  streams each gained the three things the older chapters have and they lacked: the structures
  the caller holds, an API reference table, and — the gap that mattered most — **counter-examples**.
  The older chapters teach as much through their `Wrong:` blocks as through their prose. Now
  these do too: a keyed hash with a fixed key; CRC-32 used to decide two things are "the same"
  where someone gains by fooling you; a session token from the *reproducible* generator; a
  ChaCha seed taken from the clock; an ignored seeding bool; `% 6`; a buffered writer never
  flushed; a line view kept past the next read; the state struct copied, which is the
  use-after-free an audit reproduced.

- **README** showcases the two capabilities that previously only had a name in the module list:
  hashes/tokens/encoding (with the by-use-case table that is the point of those modules), and
  streams — reading stdin a line at a time, and printing without a syscall per line. Both
  language halves; both snippets compiled against the library before being pasted.

### Added

- **`proven_fs_dir_open` / `_next` / `_close` is documented at last** — a whole API that had no
  section. It is the answer to a real problem (`proven_fs_list` reads the entire directory before
  you see any of it and allocates a string per name: 50,000 entries cost 189 ms, +4.2 MB and
  50,008 allocations), and a reader who never learns it exists reaches for the wrong call on a
  mail spool.
- The last eleven functions nothing had ever documented. **Every public function in
  `include/proven/` is now named somewhere in `manual/`.**

## [2026-07-15] — proven_c_lib-v26.07.13k

### Added

- **`encode.h` — hex and Base64, by use case (RFC 4648).** Once you can hash a thing and draw a
  random token, you need to write those bytes somewhere that only holds text - a URL, a header,
  a log line. The library had no general encoding; the only bytes-to-text it owned was SHA-256's
  own `to_hex`. Now: `proven_hex_encode`/`_decode` (lowercase, what sha256sum and git print);
  `proven_base64_encode` (standard `+/`, `=`-padded, for HTTP/MIME/JSON);
  `proven_base64url_encode` (`-_`, no padding, for URLs and filenames); and
  `proven_base64_decode`, which accepts BOTH alphabets and padded-or-not. The two ways these are
  usually got wrong are refused: a decode validates its whole input before writing a byte
  (a stray character, bad length, bad padding, or embedded whitespace is
  `PROVEN_ERR_INVALID_ENCODING` with nothing committed - never a read past the end or a silently
  short result), and the output size is a function you call, not a number you remember (a short
  buffer is `PROVEN_ERR_OUT_OF_BOUNDS`, never a truncated prefix). Pure computation, available
  freestanding. Checked against RFC 4648's own vectors and differentially against Python and zlib.

- **A primitive throughput benchmark** (`tests/test_bench_primitives`, run through
  `./nob bench-float`), and the doc it produced, `docs/primitives-benchmark.md`. It times the
  hashes, encoders, and generators over a fixed buffer, folding each output into a checksum so
  the work cannot be optimised away and a drift surfaces as a correctness failure.

### Changed

- **CRC-32 is ~4.2× faster.** The benchmark put it at ~104 MB/s - four times slower than FNV -
  because it was the textbook bitwise form (eight shift-and-xor iterations per byte, no table).
  Replaced with the standard 256-entry reflected table, turning eight iterations into one lookup:
  ~104 → ~432 MB/s, for 1 KiB of `.rodata`. Byte-identical output: the table's `"123456789" →
  0xcbf43926` check value is verified against it, and it still matches `zlib.crc32`.

### Fixed (found by the standing adversarial audit of the new encode module)

- **`proven_base64_decoded_size` under-reported for unpadded input.** It returned `(n/4)*3`,
  which floors away the 1-2 bytes an unpadded tail carries - so a caller who sized a buffer with
  the documented function could not decode the library's own `proven_base64url_encode` output
  (`OUT_OF_BOUNDS` on valid input; the audit hit it 3.3M times in 5M fuzzed inputs). Fixed to
  `((n+3)/4)*3`. Not a memory bug - the decoder checked the true length internally - a broken
  size contract.
- **The decoders lacked the `{out == NULL, out_cap > 0}` guard the encoders have**, so that shape
  stored through NULL (SEGV). Both now return `PROVEN_ERR_INVALID_ARG`, matching the encoders.

## [2026-07-15] — proven_c_lib-v26.07.13j

Two regressions the standing audit found in the previous release's own fixes — the place the
process says the next bugs are (docs/TESTING.md §5.2) — plus a sweep of long-standing doc debt.

### Fixed

- **A stream byte was stranded after a line too long for the buffer.** The read-line lookahead
  added last release stashes one byte when a line overruns; a following raw `proven_reader_read`
  then got a spurious `PROVEN_ERR_EOF` while that peeked byte sat unread, because
  `reader_buffered_fill` reported "did the source hand over bytes" rather than "is there
  anything to read". A read-to-EOF loop never came back for it, so the byte was silently lost.
  Fill now reports whether the buffer became non-empty; a re-inserted peek is progress too.
  Confirmed clean over 2,000,000 rounds of read_line/raw-read interleaving vs a reference.

- **The ChaCha seed scrub compiled to nothing.** `seed_from_entropy` cleared its 32-byte seed
  with a plain loop, under a comment saying not to leave key material on the stack — but nothing
  reads the seed afterward, so the stores were dead and the optimiser removed them (at -O1
  entirely). The raw OS entropy persisted in the frame. Replaced with a `secure_zero` that
  writes through a volatile pointer (an observable side effect the optimiser must keep, and
  freestanding-safe, unlike `explicit_bzero`/`memset_s`). Verified in the -O2 disassembly.

- **Documentation debt, swept.** The manual still *declared* `proven_sysio_flush` — deleted a
  release ago — as public API, and described it in the present tense. "`proven` exposes no
  fsync" was still asserted in the manual and an example, false since v26.07.12g. The
  `proven_rng_t` trait's obligation (a degenerate hand-written source makes `proven_rng_below`
  spin) was undocumented. And nothing said why sysio has both a line reader and a token scanner.
  All fixed, and TEST.md gained the suite descriptions for tests that had been counted but not
  catalogued.

### Added

- **A coverage sweep of the public functions no test had ever called** (`proven_fs_symlink`, the
  bounds-checked mem slices, the formatter's caller-scratch path, the mutable map/array lookups,
  `linear_search`, `proven_u16str_create_from_view`, and the standard-stream bridges left
  uncovered). No defect surfaced; the surface is no longer unexercised.

## [2026-07-14] — proven_c_lib-v26.07.13i

### Added

- **The entropy source is a thing you can install.** `random.h` could get entropy from an
  operating system and from nowhere else. That is fine until the target has no operating system
  — which is precisely the target the ChaCha generator was added for. The bare-metal story
  stopped one step short of being usable: the generator ran anywhere, and there was no way to
  seed it, because `proven_random_bytes` was compiled out of a freestanding build entirely.

  Entropy is the one thing a program cannot compute for itself, so it is now a hook rather than
  a hard-coded call. `proven_random_set_source(fn, ctx)` installs it:

  - **Hosted:** the OS CSPRNG is already installed. You call nothing.
  - **Bare metal:** a board *has* real entropy — an on-chip TRNG, a ring oscillator, an ADC's
    noise floor — and the library cannot know where. Hand it over once at startup, and
    `proven_random_bytes` and `proven_chacha_rng_seed_from_entropy` work unchanged: a few hundred
    bytes of hardware entropy become an endless cryptographic stream that needs nothing further.

  With no source installed, `proven_random_bytes` returns **false**. It does not fall back to a
  clock-seeded PRNG, because that looks like success and is a security hole nothing reports.

  There is deliberately **no built-in `RDRAND` / `RNDR` backend**: on a hosted target the OS
  already mixes the CPU's instruction into its own pool, so calling it directly buys nothing and
  costs you that mixing — and a raw hardware instruction used as the sole source is the
  arrangement people have argued about for a decade. It is four lines behind this hook, and then
  the choice is visibly yours.

### Fixed

- **`getentropy` was documented and never called.** The header claimed the OS backend was
  "`getrandom` on Linux, `getentropy` on the BSDs and macOS, `BCryptGenRandom` on Windows". The
  BSDs and macOS quietly fell through to `/dev/urandom` — which works, but is not what the
  documentation said, and needs a file descriptor that an fd-exhausted process cannot open,
  which is not a moment at which a key derivation should start failing. `getentropy` is now
  actually called there, in 256-byte chunks, with `/dev/urandom` kept as the last resort for
  systems that have neither call.

### Changed

- **`proven_chacha_rng_seed_from_os` is renamed `proven_chacha_rng_seed_from_entropy`.** On a
  board there is no "OS" to seed from, and the old name said there was. The call is otherwise
  unchanged. (Breaking, one release after it was introduced; the library is pre-1.0 and says so.)

## [2026-07-14] — proven_c_lib-v26.07.13h

Two modules that each offered one answer to a question that has several.

### Added

- **Randomness, by use case — a reproducible generator, a cryptographic one, and the OS.**
  `random.h` offered exactly one thing, the OS CSPRNG, and said in its own header that this was
  deliberate: a fast PRNG and a secure one are different tools, and shipping one under a name
  that suggests the other is how insecure tokens get written. The reasoning was right; the
  conclusion — offer neither — was wrong. A caller who needs a reproducible sequence does not
  stop needing one. They write `rand()`, or a hand-rolled LCG, and end up with something worse
  than what the library declined to give them. And a bare-metal target, which has no OS CSPRNG
  at all, was left with nothing.

  The module is now organised by use case, the way `hash.h` is:

  - `proven_xoshiro256ss_t` — fast and **reproducible**, for simulations, tests, games. The
    same seed replays the same run, which is what makes a failing test debuggable. Explicitly
    not secret-grade, and named so it cannot be mistaken for one. Seeded through SplitMix64, so
    seed 0 — or 1, or 2, which is what callers actually pass — lands on a well-distributed state
    instead of the all-zero state xoshiro can never leave.
  - `proven_chacha_rng_t` — cryptographic, and pure arithmetic: it needs no OS once seeded,
    which makes it the answer on a bare-metal target and the fast answer for bulk random data
    on a hosted one (no syscall per draw). Verified byte for byte against the standard's
    keystream, over six random keys and streams of 140 blocks, using OpenSSL as the oracle —
    which was itself first checked against RFC 8439's own vector.
  - `proven_rng_below` / `_range` / `_f64` / `_shuffle` — unbiased, over any source.
    `x % n` is biased whenever `n` does not divide 2^64, and everyone writes it anyway; these
    use Lemire's multiply-and-reject and an unbiased Fisher-Yates.

  The trait, `proven_rng_t`, is **infallible** by design. That is not a simplification — it is
  where the failure went: asking an operating system for entropy can fail, so that failure is
  confined to seeding, checked once, and every draw downstream is total.

  The generators are pure computation, so **`random.h` now works freestanding**; only the OS
  entropy source stays hosted. A board with no OS gets ChaCha20 seeded from its own hardware
  entropy — not a clock-seeded PRNG pretending to be a CSPRNG.

- **The standard streams are writers and readers.** `stream.h` had writers, readers, buffered
  writers and a line reader; `sysio.h` had stdin, stdout and stderr; the two had never been
  introduced. The cost was concrete: **there was no way to read stdin a line at a time** — the
  most common thing a program does with it — and the formatter could not be aimed at a standard
  stream at all, so every `proven_print` was its own write syscall.

  `proven_sysio_stdin_lines` + `proven_sysio_read_line` read stdin a line at a time;
  `proven_sysio_stdout_buffered` puts stdout behind a buffered writer, so a thousand small
  prints cost one syscall instead of a thousand, and `proven_fprintln` can finally be aimed at a
  standard stream; `proven_sysio_stdout_writer` / `_stderr_writer` / `_stdin_reader` are the
  unbuffered bridge. Nothing re-implements what `stream.c` already does — a second buffered
  reader would be a second place for the same bug.

  Buffering stays opt-in, and nothing registers an `atexit` handler to flush behind your back.
  The direct calls remain unbuffered: what they write is on its way out before they return.

### Fixed (found by the standing adversarial audit of both new modules)

The maths came back clean — ChaCha20 byte-identical to OpenSSL over 12 random keys and 10 KB
streams, xoshiro identical to the upstream reference over 2.56M outputs, Lemire's accept/reject
exact over 1M cases, the portable 64×64→128 fallback exact over 8M pairs, every chi-square
inside 2σ. The defects were all in the *failure* paths, and every one handed the caller bytes
that looked fine.

- **A ChaCha generator that was never usable handed back plausible bytes.** Three defects, two
  of them one bug: `proven_chacha_rng_next(NULL)` read an uninitialised local and returned this
  frame's stack as randomness (it was the only entry point in the module without a NULL guard);
  a never-seeded, stack-declared generator has `used == 0`, which the fill path read as "a full
  block of fresh keystream is ready" and copied its own uninitialised `block[]` out; and a
  generator whose `seed_from_os` *failed* was left zeroed — which produces an all-zero first
  block, so the claim "you get zeros, not plausible garbage" was true for exactly 64 bytes, and
  then the counter advanced and block 1 was a normal-looking, **fixed, publicly derivable**
  keystream. `used` alone could not encode usability, because a zero-initialised struct is the
  shape of "never seeded". The generator now carries an explicit seeded marker: unseeded or
  failed, it is inert — invalid trait, `next()` of 0, `fill()` of zeros.

- **`proven_reader_read_line` refused a line that fit.** It documented "a line LONGER than the
  buffer is `PROVEN_ERR_OUT_OF_BOUNDS`" and enforced something stricter — it rejected any line
  that *filled* the buffer. It had to, because it answered "too long" before attempting a fill,
  and a fill cannot tell "buffer full" from "source ended". But a full buffer means one of three
  things and only one is an error: the next byte is the newline that ends the line; the source
  has ended and what is held IS the final line; or the line really is too long. The middle case
  was data loss, and not exotic — **a 4-byte file with no trailing newline, read through a
  4-byte buffer, came back `OUT_OF_BOUNDS` with its entire contents unreachable.** One byte of
  lookahead tells the three apart, so the documented rule is now the enforced one.

- **`proven_sysio_out_t` / `_lines_t` contained a pointer to themselves**, so copying one
  dangled (ASan: heap-use-after-free). `proven_sysio_read_line` takes its state *by pointer* —
  the shape that says "relocatable" — so it now re-binds the inner reader on every call, making
  the implied contract true. The writer states cannot do that, and the header now says plainly
  that they must not be copied or moved once a writer has been made from one.

### Removed

- **`proven_sysio_flush` is deleted, and the delete is the point.** It claimed to flush a buffer
  that did not exist: a no-op on POSIX, a *disk sync* on Windows, and its own header told callers
  not to use it. Flushing a buffered writer's bytes to the OS is `proven_writer_flush`; pushing
  the OS's bytes to the disk is `proven_fs_sync`. They are different operations and now say so.
  Nothing in the library depended on it.

### Fixed

- **The README claimed `proven` exposes no fsync.** It has since v26.07.12g
  (`proven_fs_sync`, `proven_fs_write_file_durable`). Corrected in both language halves.

## [2026-07-13] — proven_c_lib-v26.07.13g

The time formatter's two encodings were quietly disagreeing, found and closed by the standing
adversarial audit, which otherwise came back clean across every remaining module.

### Fixed

- **`proven_time_u16_fmt` silently dropped fill/align/width specs the u8 formatter honoured.**
  The two time formatters are documented as one formatter differing only in output encoding
  ("specifiers matching `fmt.h`"), but the u16 path hand-rolled a parser that recognised only
  zero-fill `:0>N`: `{month:>4}` came back as `"3"` instead of `"   3"`, `{Weekday:>12}` was
  left unpadded, `{Month:*^10}` ignored its spec entirely. A silently discarded spec that emits
  the wrong-width string is exactly the quiet wrong answer this library refuses. The u16
  formatter now renders through the u8 path (which delegates each field to the `fmt.h` spec
  engine) and widens the result to code units, so the whole `{}` grammar is honoured for numeric
  and named fields alike and the two encodings can never again disagree. The duplicate
  hand-rolled `proven_sys_time_format_int_u16` PAL function (both the `wsprintfW` and the manual
  POSIX variant) is deleted. Pinned by `tests/test_unit_time_fmt_u16_parity`, which drives a
  broad spec matrix through both formatters and asserts byte equality.

- **A negative year under zero-fill padded one column too wide in the u16 formatter.** `{year:0>4}`
  of -44 rendered as `"-044"` through the u8 path (the sign counts toward the field width, as in
  printf's `%04d`) but `"-0044"` through the u16 path. Subsumed by the parity fix above, and kept
  pinned by `tests/test_regression_time_fmt_neg_year`.

### Changed

- **Documentation: the `hash` and `random` modules are now surfaced in `README.md`** (both language
  halves): the module index lists them, the intro sentence names hashing and OS randomness, and the
  "what it is not" section no longer claims "no cryptographic hashing" — it now scopes the real
  non-goals (signatures, key exchange, KDFs, authenticated encryption, TLS) around the hash/CSPRNG
  primitives that do exist. The `stream` module is listed too.
- **`ring.h` and `pool.h` gained contract notes** raised as non-defect observations by the audit:
  the raw `proven_ring_create` does not check `elem_size % align` (the `PROVEN_RING_INIT` macro
  always threads consistent values), and pool blocks must be freed exactly once and only through
  the owning pool (a double-free is trapped only in debug / `PROVEN_HARDENED` builds).

## [2026-07-13] — proven_c_lib-v26.07.13f

### Added

- **`{:e}` — scientific float formatting, completing the printf trio.** `{:f}` gives fixed and
  `{:g}` gives shortest, but neither is `%e`: `{:f}` never shows an exponent and `{:g}` uses one
  only when it is shorter, so there was no way to ask for "always scientific, N digits after the
  point" — the form you want for an aligned column of magnitudes or to match existing `%e`
  output. `{:e}` / `{:.Ne}` now render exactly what printf does: mantissa, a signed
  two-digit-minimum exponent, correctly rounded (half-to-even), at any magnitude including the
  smallest subnormal. The correctly-rounded scientific core was already there (the default form
  uses it at the extremes); this exposes it as `PROVEN_FLOAT_FORMAT_MODE_SCIENTIFIC`. Written
  test-first against printf's own output, and checked differentially against `%.Ne` over 200,000
  random doubles at every precision (0 mismatches).

### Fixed

- **Zero-fill on a float put the zeros before the sign.** `{:08.2f}` on -3.14 produced
  `000-3.14` and `{:+08.2f}` on 3.14 produced `000+3.14` - not numbers a reader or a numeric
  column can parse. The integer path already placed zero-fill between the sign and the digits
  (the `0000+42` fix); the float path never did, and rendered the whole string and padded it,
  sign included. Pre-existing, and made obvious by `{:e}`, which makes signed scientific columns
  common. The sign is lifted out and the zeros placed after it now, matching printf: `-0003.14`,
  `+0003.14`, `-03.14e+00`.

## [2026-07-13] — proven_c_lib-v26.07.13e

Two defects found while exercising the modules together rather than one at a time - a
fmt -> file -> scanner -> float round-trip over 20,000 lines, and a fresh audit of the
string modules.

### Fixed

- **A float split across the buffered scanner's refill boundary was scanned wrong** - in two
  invisible ways. If the boundary fell on the exponent, "-3.0448…e" parsed as a valid float
  (the mantissa) and silently dropped the "e-222" that had not arrived: a truncated value
  committed as a success. If it fell on the sign, "-" alone is a parse FAILURE, and the
  scanner dropped the byte and desynced every later scan instead of asking for the rest. An
  integer that runs out mid-token already said "need more"; a float could not, because unlike
  an integer it can look complete, or look like garbage, when it is merely unfinished.
  proven_scan_f64 now flags needs_more both when a valid float might still grow (it ran to the
  buffer end, or stopped at a splittable exponent) and when a failed parse left only a float
  PREFIX - genuine garbage like "abc" is still the error it is, and does not wedge the scanner
  waiting for it to become a number. Found by the round-trip; pinned by
  tests/test_regression_scanner_float_split, which drives the split to every byte of the token.

- **proven_u16str_append_grow sealed its 2-byte terminator at the byte index, not the unit
  index.** internal.len is a byte count, so `[len]` writes the NUL at unit `2*len` instead of
  `len`. Latent under the doubling growth policy - the stray zero lands in unused slack and the
  following append re-seals the real terminator - but an out-of-bounds heap write the instant
  growth becomes exact-fit. Found by the u16str audit's poisoned-allocator repro; the invariant
  is pinned in tests/test_regression_v26_07.

## [2026-07-13] — proven_c_lib-v26.07.13a

An adversarial audit of the modules that had never had one — the scanner and the float
engine, the containers, and the platform layer. Everything below was **reproduced** before
it was fixed, and every fix has a regression test that was checked to fail against the
unfixed source.

The theme, again: **code that is correct for the input it was tested with and silently
wrong for the input it will actually meet.**

### Fixed (third audit round: the fixes from the second round)

Pointing the audit at the code the previous audit had just produced found three regressions
in it. That is the point of B-011, made twice in one day.

- **Copying a read-only file worked once and failed forever after.** Carrying the source's
  mode meant a 0400 destination — and `open(O_WRONLY)` on a 0400 file fails, so the *second*
  copy could not even open it, returned `PROVEN_ERR_IO`, and left the destination holding its
  old contents. An unwritable destination is made writable first (we are about to overwrite it
  anyway), the payload is written under 0600, and the real mode goes on at the end — so the
  contents are still never exposed under a wider mode than the original's.

- **The buffered writer remembered an inner *error* but not an inner *stall*.** A sink that
  takes nothing and reports no error — every wedged sink looks like this — made
  `proven_writer_write` return `PROVEN_ERR_IO` while leaving the writer thinking it was
  healthy, so it went on accepting writes: the receiver got `ABC`, a 27-byte hole, and `XYZ`,
  with the second write reporting success. The flush path had always treated `{OK, 0}` as a
  failure; the write path did not, and the two disagreed.

- **A symlink to a regular file was `PROVEN_FS_TYPE_OTHER` in a listing and
  `PROVEN_FS_TYPE_FILE` from `stat`.** The walk stat'd with `AT_SYMLINK_NOFOLLOW`. Two answers
  to the same question is worse than either answer, and a caller filtering a listing on
  `type == FILE` skipped files it could open and read. The walk follows now, exactly as `stat`
  does; a *dangling* link fails the follow and is still `OTHER`, which is honest — it cannot be
  opened at all.

- **`proven_writer_from_u8str` had no flush**, so a render that ran out of memory halfway left
  the string holding a valid, NUL-terminated *prefix* of the output — and `proven_writer_flush`
  answered `PROVEN_OK` on it. Found by asking whether every writer implementation keeps the same
  contract, which is the same question that found the pool refusing to free a block.

### Fixed (second audit round: the new code, the allocators, the filesystem)

- **`close()` failures were thrown away** — and `close()` is the last chance a filesystem
  has to say a write did not land. On NFS, CIFS and quota-enforcing filesystems it is the
  *only* chance: the bytes were buffered, `write()` said yes, and the refusal surfaces
  there or nowhere. `proven_fs_write_file` returned `PROVEN_OK` for a file the filesystem
  had just refused, and `write_file_atomic` went on to `rename()` the temp over the target,
  **publishing content the disk had rejected**. Reproduced with an `LD_PRELOAD` that fails
  `close()`. `proven_fs_close` now returns `proven_err_t` and is `[[nodiscard]]`; the write
  paths check it, and a failed close aborts the rename and removes the temp.

- **An atomic write exposed its contents under a wider mode.** The temp was chmod'd to the
  target's mode at the *end*, so the entire new contents of a 0600 file sat in a
  world-readable temp for as long as the write took. A watcher thread stat'ing the temp
  during a 64 MiB rewrite saw `0644`. The mode goes on before the first byte does.

- **`proven_fs_copy` widened permissions.** Copying a 0600 file produced a 0644 one — `cp`
  does not do that. The source's mode is carried across, and set before the contents go in.

- **A symlink, a FIFO, a socket or a device was reported as `PROVEN_FS_TYPE_FILE`**, which
  tells a caller it may open it and read bytes out of it. A dangling symlink cannot even be
  opened; a FIFO blocks forever on a writer that never comes. They are `PROVEN_FS_TYPE_OTHER`.

- **`proven_mmap_sync` on a PRIVATE mapping returned `PROVEN_OK` and persisted nothing.**
  A copy-on-write mapping has nothing to write back. It is `PROVEN_ERR_UNSUPPORTED`.

- **The scanner's rollback wrapped `cursor` and `length` to ~2^64** — a defect introduced by
  the pipe fix earlier the same day, and the worst kind: the whitespace refill moved the
  cursor *before* the fill that compacts the buffer, so the reported shift exceeded the
  snapshot the rollback subtracts it from. Both indices wrapped by the same amount, so every
  guard still compared true, and the next scan read `buffer[SIZE_MAX-15]`. ASan called it a
  heap-buffer-overflow; on a pipe it silently discarded the buffered bytes instead. The
  whitespace is now stepped over *before* the snapshot, where there is nothing to roll back
  to yet.

- **`{:f}` refused any double above ~1e121** with `PROVEN_ERR_INVALID_FORMAT` — a *bad format
  string* — because the fixed form was rendered into a 128-byte scratch. The number was fine;
  the scratch was not, and no output buffer the caller supplied could help. `{:.60f}` on
  1e308 (370 characters) now renders whole.

- **The buffered reader dropped the bytes of an EOF that carried them.** A reader may return
  `{PROVEN_ERR_EOF, N}` with N nonzero — the library's own `read_all` does — and the last N
  bytes of a file are still bytes. The final line of a file vanished whenever the source
  reported its end and its last bytes in the same breath.

- **The pool refused every `realloc`, including "free it" and "make it smaller".** Every call
  was `PROVEN_ERR_INVALID_ARG` - which a trait-generic caller reads as *"you passed me
  garbage"* and goes hunting for a bug in its own code. A pool genuinely cannot grow a block,
  and that is `PROVEN_ERR_UNSUPPORTED` now ("not my job"); `realloc(ptr, 0)` frees and returns
  NULL like every other allocator, and a shrink within the item size is the no-op it obviously
  is. A size the pool does not serve is `UNSUPPORTED` for the same reason.

- **The allocator trait meant different things per allocator**: `alloc(0)` was `NOMEM` on the
  heap (a lie — nothing was out of memory) and `PROVEN_OK` with a live pointer on the arena;
  `realloc(ptr, 0)` returned NULL on the heap and a non-NULL pointer on the arena, though the
  trait documents NULL; and a pure *shrink* of a non-tail arena block failed with `NOMEM` on a
  full arena, which is an absurd answer to "please use less". All three now answer identically,
  pinned by `tests/test_contract_allocator_trait`, which runs the same code against both.

- `fmt.h` claimed `PROVEN_ARG('Z')` prints `Z`. It cannot: `'Z'` has type `int` in C. A `char`
  *variable* does print as a character; the note now says which is which.

### Fixed

- **The "correctly rounded" float parser was not correctly rounded.** The exact
  big-integer fallback — the tier whose entire job is to decide, bit for bit, which way a
  decimal sitting on a rounding boundary goes — built `5^q` for `56 <= q <= 350` by taking
  the *Eisel-Lemire* table entry and shifting it left. That entry is a 128-bit mantissa
  **rounded** to 128 bits, and `5^q` is odd, so the shift can never be exact (`q=55`: the
  table says `…078124`, the truth is `…078125`). The exactness tier was comparing against a
  corrupted power of five. Every exact halfway value in that exponent window broke the tie
  in a fixed direction instead of to even, and any value within ~1e-38 of a boundary came
  out one ULP wrong — with `PROVEN_OK`. A differential run against glibc found **2,923**
  of them. `scan.h` and manual chapter 8 both promise ties-to-even, bit-identical to a
  correct `strtod`; for any input with 20+ significant digits in that range, that promise
  was false. Above the exact table, `5^q` is now multiplied, not looked up.

- **The buffered scanner could not read a pipe** — the one thing it exists for. `read()`
  on a pipe returns whatever has arrived so far; `scanner_fill` treated *any* short read as
  end-of-input and **latched** it. So a token straddling the boundary was committed
  **truncated** (a writer sending `"123"`, a pause, then `"456 789\n"` produced the integer
  **123**, reported as a successful scan), and every later scan returned `PROVEN_ERR_EOF`
  while the rest of the stream sat unread in the pipe forever. Only a zero-byte read is an
  end of input now — which is what `read()` itself has always meant by it. Regular files hid
  the bug completely: a file read is short only at real EOF.

- **A failed read was reported as a clean end of input** (scanner). `proven_sys_io_read_once`
  reports a failed `read()` as `{PROVEN_ERR_IO, 0}`, and the end-of-input test accepted any
  zero-byte result — so every `EBADF`, `EIO` and `ECONNRESET` became a tidy EOF. A stream
  that broke halfway through was indistinguishable from one that finished.

- **A map with churn grew forever.** `remove()` cannot free a bucket — an open-addressed
  table needs the tombstone — so `used` only ever rises, and the rehash it triggers doubled
  the capacity **unconditionally**, even when the live count had not moved. That is every
  cache, every session table, every work queue. Measured: 100 live entries, two million
  operations, capacity **1,048,576**, **33 MB** held. Not a leak — every byte reachable,
  every byte freed at destroy — which is precisely why no leak checker ever mentioned it.
  The rehash now grows only when the *live* set needs the room, and otherwise reclaims
  tombstones at the same capacity (same walk, same cost). The 33 MB is now 8 KB.

- **`proven_u8str_append_grow` of an empty view left the string unterminated.** The grow
  allocated the block, then delegated to `append`, which returns early on an empty view —
  before sealing the NUL. `as_cstr()` is documented as always terminated, and it read
  straight off the end of a fresh heap block (ASan confirms). `proven_u16str_append_grow`
  had the identical bug. The seal now happens where the block is allocated, as `reserve()`
  has always done.

- **The sort handed the comparator a misaligned element.** `insertion_sort` held the moving
  element in a `proven_byte_t[128]` — alignment **1** — and passed its address to the
  caller's comparator, which reads it as the element type. For any over-aligned element that
  is a misaligned typed access: UB, flagged by UBSan at every optimisation level, and a fault
  on a strict-alignment target. It never crashed on x86, which is why it survived. The scratch
  is over-aligned now, and elements aligned more strictly than the scratch take the swap path,
  which only ever shows the comparator real array elements.

- **A fixed-buffer writer that had overflowed reported success on flush**, because it had no
  flush function at all — `proven_writer_flush` returned `PROVEN_OK` for it. "Render, render,
  render, check the flush" is what every caller does, and it was told a buffer that had refused
  half the output was fine. It reports the overflow now, and a later chunk that *would* fit is
  refused as well: writing it would land it after the hole.

- **A buffered writer that had failed reported success on the next flush.** The failing
  write emptied nothing into the buffer, so `flush` found nothing to fail on and answered
  `PROVEN_OK` — and "write, write, write, check the flush", which is how almost everyone
  uses a buffered writer, reported success on a stream missing every byte. Reproduced
  against `/dev/full`. The writer is sticky now: once it has lost bytes, every later write
  and flush returns the original error.

- **The panic handler was a data race** (TSan). It is read on the `*_or_panic` allocator
  paths, which run on every thread, and written by `proven_set_panic_handler`, which a
  program may call while those threads are running. It is atomic now.

### Changed

- **The scan engine can say "I ran out of input" (`proven_scan_t::needs_more`).** On a
  complete view, "the input ran out" and "the input is wrong" are the same fact, so the
  engine reported both as a malformed input — and the buffered scanner on top of it had no
  way to tell them apart. Over a stream they are *opposite* facts: a pipe that delivered
  `-` and then, 150 ms later, `12` produced `PROVEN_ERR_INVALID_ARG`, and `key=` against a
  pipe that had so far sent `ke` produced `PROVEN_ERR_NOT_FOUND`. Both are now refilled and
  retried. A wrong byte that is actually present is still an error — the scanner does not
  wait for input that cannot fix it.

- **`5^q` is built 27 exponents at a time.** The exact fallback multiplied by 5 once per
  unit of exponent below 64, and ran exponentiation-by-squaring over full big integers above
  it. `5^27` is the largest power of five that fits in a `u64`, so `5^350` now costs thirteen
  single-limb multiplies. It matters because the (now correct) exact tier is the only way to
  build `5^q`: the hard-input parse went 3,117 ns → **1,945 ns**, i.e. correctness cost about
  6% over the old *wrong* answer rather than 70%.

- **`proven_u8str_append_fmt` renders each argument once, not twice.** The allocation-free
  fixed-capacity path measured the whole output and then formatted it all over again — for a
  double, that is the correctly-rounded decimal engine run twice with the first answer thrown
  away. It writes as it goes now and restores the original length if it fails, so "atomic"
  still means what it said. Measured: an int/string/int line **254 ns → 155 ns**; a double
  **545 ns → 277 ns**. Pinned by `tests/test_contract_fmt_atomic`, because atomicity now rests
  on a rollback rather than on never having written.

- **Interior pointers are documented as perishable.** `proven_array_get(_mut)`,
  `proven_map_get(_mut)` and `proven_u8str_as_view` return pointers into the container's
  storage that the next growing operation frees. The headers now say so; all three were
  demonstrably a use-after-free waiting to happen.

- **`platform/proven_sys_time.h` no longer calls the wall clock "monotonic".** It reads
  `CLOCK_REALTIME` (Windows: `GetSystemTimeAsFileTime`) and always has. Code measuring an
  interval by subtracting two of these can get a negative duration; the header was telling
  it that could not happen.

### Added

- **`map` is HashDoS-resistant by default (B-015).** `map` hashed untrusted string keys with
  non-keyed FNV-1a, so an attacker who controls the keys could compute collisions offline and
  flood one bucket — turning the map's O(1) into O(n²) on demand. It now hashes string keys
  with **keyed SipHash-2-4 under a per-process secret** drawn once from the OS CSPRNG, exactly
  the switch Python, Rust and the Linux kernel made for their built-in tables. `proven_map_create`
  gives you this safe default; `proven_map_create_trusted` opts into fast FNV for keys your own
  program chooses. `proven_map_hash` exposes the function a map actually uses, so the choice is
  observable (and useful for inspecting distribution). Integer keys are unaffected; on a
  freestanding target, which has no CSPRNG and no attacker model, string keys fall back to FNV.
  The per-process key is seeded exactly once even under a 64-thread race (verified under TSan);
  a map created in one process hashes a given key differently from the same map in another,
  which is the unpredictability the defence rests on.

- **`proven/random.h` — cryptographically strong OS randomness.** `proven_random_bytes` fills a
  buffer from the OS CSPRNG (getrandom / getentropy / BCryptGenRandom), returning false where
  there is none rather than handing back weak bytes. No user-visible PRNG: a fast reproducible
  generator and a secure one are different tools, and offering one under the other's name is how
  insecure tokens ship. It is what seeds map's keyed hash, and what any caller needing a key, a
  token, or a nonce should use.

- **`proven/hash.h` — hashing, organised by use case.** lowent's case study needed a
  content-addressing digest, found proven had no hash of any kind, and hand-wrote BLAKE3-256
  — "exactly the kind of code that should not be hand-rolled". There is no single "hash", so
  the module gives one primitive per job and names the job: `proven_hash_bytes` (FNV-1a) for
  your own table on trusted input; `proven_hash_keyed` (SipHash-2-4) for a table on *untrusted*
  input, where a non-keyed hash lets an attacker collide every key into one bucket;
  `proven_crc32` (IEEE, gzip/zlib/PNG-compatible, streaming) for detecting *corruption*; and
  `proven_sha256` (FIPS 180-4, streaming, with a git/sha256sum-style hex) for *fingerprinting*
  content safely against a forged match. Every one is byte-exact and endianness-independent,
  all are royalty-free, all are implemented from their specifications rather than copied, and
  each is checked against its standard's own known-answer vectors (`tests/test_unit_hash`) and
  differentially against Python's hashlib/zlib and an independent SipHash over every length to
  300. The second feature written test-first (`docs/TESTING.md` §5.1).

- **`proven_fs_walk`** — recursive, pre-order directory iteration that **cannot loop and cannot
  escape**. The manual had been telling callers to guard against symlink cycles themselves ever
  since the walk learned to follow links; this is the guard. It never descends *through* a
  symlink (the symlinked directory is still reported — it exists — it is simply not entered),
  which buys both guarantees at once: a link to an ancestor cannot loop it, and a link anywhere
  else cannot walk it out of the tree you asked about. It also carries the `(dev, ino)` of every
  directory on the current path, for the loops a symlink is not needed for. A directory it cannot
  read is **reported** — `proven_fs_walk_next` returns that directory's error with the entry
  naming it, and the walk goes on — because a tree walker that silently skips an unreadable
  subtree is how a backup misses files and reports success. Memory is bounded by **depth**, not
  breadth: one handle and one `(dev, ino)` per level, plus a single reused path buffer.

  This is the first feature written under the test-first rule (`docs/TESTING.md` §5.1): the
  contract and a failing test in one commit, the implementation in the next, and then the
  standing adversarial audit (§5.2). Between them they caught, before and after it shipped: the
  first draft of the contract ("follow, but stop at a cycle") quietly walking all of `/tmp`; a
  300-level tree silently truncated at 256 and reported as a clean end-of-walk; a `readdir()`
  failure reporting the whole path instead of the directory name and a depth one too high; and
  a **TOCTOU escape** — a directory swapped for a symlink between being listed and being
  entered was followed out of the tree, until the descent was made fd-relative and
  `O_NOFOLLOW`. Every one is pinned by a regression test verified to fail against the code
  before its fix.

- `tests/test_regression_float_exact_pow5`, `tests/test_regression_scanner_short_read`,
  `tests/test_regression_map_churn`, `tests/test_contract_sort_alignment`,
  `tests/test_contract_fmt_atomic`, and an empty-view NUL-seal section in
  `tests/test_regression_v26_07`.

## [2026-07-12] — proven_c_lib-v26.07.12i

Closes `docs/BACKLOG.md` B-005, B-009 and B-010.

### Added

- **The format spec grammar grew the things it was missing.** It supported exactly
  four: fill, align, width, and lowercase `x`. Now:

  ```text
  {:[[fill]align][sign][#][0][width][.precision][type]}
  ```

  - **Float precision**: `{:.3}`, `{:.0}`, `{:f}` (fixed), `{:g}` (shortest
    round-trip). This is the one that mattered. Every float came out with **exactly six
    decimals, forever**, so a float column could not be aligned - 12.5 rendered nine
    characters wide and 100.0 rendered ten, and the column broke. The exact engine could
    always do precision; the `{}` grammar simply could not reach it.
  - **Bases and case**: `{:x}` `{:X}` `{:o}` `{:b}`, with `#` for the `0x` / `0X` /
    `0o` / `0b` prefix. A conventional uppercase hex dump was impossible before.
  - **Sign**: `{:+}` and `{: }`. Zero-padding goes *between* the sign and the digits -
    `{:+08}` on 42 is `+0000042`, never `0000+42`.
  - **`char` and `bool` arguments.** `PROVEN_ARG('Z')` printed `90`; there was no way
    to emit a single character at all. A `bool` renders `true` / `false`.

- **`PROVEN_ARG_OF(&obj, render)`** - the formatter is no longer a closed set. `PROVEN_ARG`
  is built on `_Generic`, which cannot be taught a type it was not told about at compile
  time, so a user struct could not reach `{}` at all: you pre-rendered it into a scratch
  string (an allocation and a copy per value, in the logging path, which is the one path
  that must keep working when allocation is what has failed) or you printed the fields one
  at a time and gave up on aligning the column.

  A renderer takes a sink, not a buffer, so it composes - it may call the formatter again.
  And the formatter runs it **twice**, once against a counting sink, which is what lets
  `{:>20}` align a user type exactly as it aligns an int with nothing allocated. A renderer
  whose two passes disagree is an error rather than a silently misaligned field, and a spec
  the library cannot interpret for your type (`{:x}`, `{:.2}`, `{:+}`) is refused rather
  than guessed at. Closes `docs/BACKLOG.md` B-010.

- **`proven_fs_dir_open` / `_next` / `_close`** - streaming directory iteration.
  `proven_fs_list` reads the whole directory before the caller sees any of it. Measured
  on 20,000 entries: 80 ms and **+1,664 KB** resident, nothing visible until the last
  one. The iterator: 64 ms and **+0 KB**. The entry name is borrowed from the
  iterator's storage, which is what lets a huge directory be walked without an
  allocation per entry.

- `tests/test_unit_fmt_spec`, `tests/test_regression_stream_partial_write`, and
  directory-iteration coverage in `tests/test_unit_fs_position_and_sync`.

### Changed

- **A writer's `write_fn` now returns how many bytes the sink took**
  (`proven_result_size_t`), not just success or failure, and `proven_writer_write_partial`
  exposes that count to callers. The old trait said "consume the whole chunk or fail",
  which is a promise a pipe, a socket, or a filling disk cannot keep - so no correct sink
  could be written against it. `proven_writer_write` still means all-or-nothing; it loops.

- **The PAL directory walk reports failure.** `proven_sys_fs_dir_step` returns 1 / 0 / -1
  (entry / end / failure) in place of a bool, because `readdir()` returns NULL for both
  "the directory ended" and "the read failed", and errno is the only thing that tells
  them apart.

### Fixed

- **The buffered writer duplicated bytes on a partial write.** A sink that accepted
  4096 of 6000 bytes and then failed left the whole buffer in place, and the next flush
  re-sent it from the start: the receiver got **10,096 bytes, with the first 4096
  twice**, and no way to detect it. The flush now advances past the bytes the sink
  acknowledged and keeps only the unsent tail. Losing data is bad; silently doubling it
  is worse.

- **A failed read looked like a clean end of file.** The buffered reader had nowhere to
  put an error, so a source that broke mid-file set `eof` and the caller saw a complete,
  successfully-read file. It carries the error now. A file truncated by a disk error and
  a file that simply ended are not the same fact.

- **`{:f}` did not force the fixed form.** It set the SIMPLE policy and the dispatcher
  then ignored it, switching to scientific above 1e18 and below 1e-4 anyway - so `{:f}`
  on `1e20` rendered `1.000000e+20`, and `{}` and `{:f}` were byte-identical for every
  input in the language. The exact fixed-point engine was there the whole time; nothing
  called it.

- **A failed directory read was reported as end-of-directory.** `proven_fs_dir_next`
  mapped a NULL `readdir()` to `PROVEN_ERR_EOF` whether the directory had ended or the
  disk had failed, so a listing cut short by an I/O error was indistinguishable from a
  complete one. That is how a backup silently skips files. It is `PROVEN_ERR_IO` now,
  and `proven_fs_list` propagates it instead of returning half a directory.

- **The float engine silently rewrote precision 0 to 6.** `{:.0}` on 3.7 gave
  `3.700000`. "No decimals" is what `%.0f` means everywhere, and answering it with six
  is the same disease as accepting a spec and then ignoring it: the caller asked for
  something, got something else, and was told it worked.

- **A wide zero-fill was silently truncated.** The first version of the new integer
  renderer assembled the padded number in a fixed 128-byte buffer, so `{:#0200x}` - a
  legal request, since the parser allows a width up to 10000 - produced **127
  characters and returned PROVEN_OK**. The padding is now emitted through the same path
  that already knows how to write N of something without holding N of it.

## [2026-07-12] — proven_c_lib-v26.07.12h

Steps 4-6 of `docs/RFC-0001-streams-and-io.md`: the keystone. Closes B-007 and B-008.

### Added

- **`proven_writer_t` and `proven_reader_t`** (`include/proven/stream.h`). There was no
  stream abstraction at all. The formatter's only sink was a `proven_u8str_t`; a file
  was a `proven_file_t`; the two scanners read two other things again. Four types, four
  function families, no common interface — so you could not write one
  `serialize(sink, value)` that worked over both memory and a file, and you could not
  format into a file at all.

  Both are small vtables passed by value, modelled on `proven_allocator_t`, with sinks
  over a file, an owned string, and a fixed caller buffer; sources over a file and over
  bytes you already have; and buffered adapters for each.

  **Buffering uses memory you supply**, exactly like `proven_arena_create`. There is no
  hidden global buffer — which means there is no destructor to flush it for you, and
  you must flush before it goes out of scope. In exchange, the logging path never
  allocates, and a program logging its way out of an out-of-memory condition can still
  log.

- **`proven_fmt_to_writer_impl`, `proven_fprint`, `proven_fprintln`** — format straight
  into a writer, through a stack scratch buffer. No allocation.

- **`proven_reader_read_line`.** Reading a file line by line was impossible: the only
  route was loading the whole file and splitting by hand. Two contracts are pinned by
  tests because a naive implementation gets them wrong: the **final line with no
  trailing newline is still returned** (dropping it is how the last record of a file
  goes missing), and a **line too long for the buffer is an error, not a truncated
  line** (a truncated line handed back as if it were whole is a corruption the caller
  cannot detect).

- `tests/test_unit_stream`, and `manual/examples/ex_05_stream.c` — one serializer
  writing into a string, a fixed buffer and a file, then reading it back line by line.
  Compiled and run by the build.

### Fixed

- **`proven_print` no longer allocates.** It built a fresh heap `proven_u8str_t` for
  *every* call: ten thousand log lines meant ten thousand mallocs and ten thousand
  frees, on the logging path — the one place an allocation is least welcome. It now
  formats into a stack buffer and only reaches for the heap if the line will not fit.

  Measured, 10,000 lines: `malloc()` **10,000 → 0**. A buffered writer over 8 KiB of
  caller memory takes it further: `write()` **10,000 → 24**, `malloc()` **0**.

  `proven_print` remains one syscall per line by design. Buffering it would require
  hidden global state, which this library does not have; a caller who wants the 24
  builds a buffered writer and says so.

### Changed

- `stream.h` is hosted-only — it sits on `fs`. The freestanding build excludes it.

## [2026-07-12] — proven_c_lib-v26.07.12g

Steps 1-3 of `docs/RFC-0001-streams-and-io.md`. Subtraction first, then the two things
the library simply could not do.

### Removed

- **The hand-written syscall assembly.** `platform/proven_sys_io.c` implemented read,
  write and seek in inline assembly, one raw-syscall path per architecture: x86_64,
  i386, aarch64, plus an opt-in ARM32 path. It bought nothing — `proven_sys_fs.c` in
  the same library already called libc's `open`, `read`, `write` and `close`, so libc
  was always linked and always doing file I/O.

  What it cost was real. Three of the four paths could not be verified on a machine
  without the cross-toolchains, which is most machines. And because the console path
  issued raw `syscall` instructions, **standard tracing tooling was blind to every one
  of this library's console writes** — an LD_PRELOAD interposer counted zero of
  `proven_println`'s ten thousand. It now counts all of them.

  Removing it changed no behaviour: forcing every branch into the POSIX fallback passed
  12 of 12 I/O tests byte-identically before the change was made.
  `tests/test_portability_source_contracts` now *forbids* the assembly rather than
  requiring it.

### Added

- **`proven_fs_seek`, `proven_fs_tell`, `proven_fs_truncate`, `proven_fs_pread`,
  `proven_fs_pwrite`.** None of these existed. Truncating a file meant reading all of
  it and rewriting the part you kept — an O(n) copy for an O(1) operation.

  A handle that cannot seek — a pipe, a FIFO, a terminal — returns
  `PROVEN_ERR_UNSUPPORTED`, **not** `PROVEN_ERR_IO`. Not being seekable is a property
  of the thing, not a failure of the call, and code that adapts to it has to be able to
  tell them apart.

  `pread` and `pwrite` do not move the file position. That is the whole point: two
  readers sharing a handle cannot race on a cursor that neither of them moves.

- **`proven_fs_sync`, `proven_fs_sync_dir`, `proven_fs_write_file_durable`.** The
  library imported no `fsync` and no `fdatasync` — a caller who wanted their bytes on
  the disk could not ask **at any price**.

  `write_file_durable` does the three steps in the only order that works: fsync the
  temp file, rename it over the target, then fsync the directory. Atomicity and
  durability are different promises, and conflating them is how data gets lost:
  `write_file_atomic` guarantees a reader never sees a half-written file, and says
  nothing about a power cut. Syncing the file but not the *directory* leaves a window
  in which the bytes are safe and the name that points at them is not — which is
  exactly the corruption an atomic write exists to prevent.

  It is slow, and it is meant to be. It waits for the storage device, twice.

- `tests/test_unit_fs_position_and_sync` — covers all of it, including the contracts
  that are easy to get wrong: a FIFO seek is `UNSUPPORTED`, `pread`/`pwrite`/`truncate`
  leave the position alone, growing a file zero-fills, and a durable rewrite still
  preserves the target's permissions and leaves no temp debris.

### Fixed

- `proven_sysio_flush` no longer calls `FlushFileBuffers` on Windows. It did nothing on
  POSIX and forced a full disk sync on Windows: one API, two meanings, neither of them
  what "flush" promised. It now does nothing everywhere, and durability is its own
  explicit call that a caller pays for knowingly.

## [2026-07-12] — proven_c_lib-v26.07.12f

Two audits went looking for weakness in the formatter and the I/O layer. They found
several things that were quietly wrong — fixed here — and one thing that is missing,
which is now designed rather than patched: see `docs/RFC-0001-streams-and-io.md`.

### Fixed

- **`{:08}` was accepted and silently wrong.** The `0` was eaten as the first digit of
  the width, so `{:08}` on 42 produced `"      42"` — space-padded, eight wide, no
  error — and `{:08x}` produced `"      2a"`. That is the spelling every C, Python and
  Rust programmer reaches for. A near-miss that is accepted and quietly does the wrong
  thing is worse than one that is rejected. A leading zero now means zero-fill; an
  explicit fill still wins.
- **A format spec the argument could not honour was ignored.** `{:x}` on a double
  printed `3.500000`; on a string it printed the string. The request was parsed and
  dropped on the floor, and the call reported success. It is now
  `PROVEN_ERR_INVALID_FORMAT`.
- **`proven_sysio_flush`'s documentation was a lie.** It claimed to flush an internal
  buffer to the OS. There is no buffer: on POSIX it is a single `ret` instruction, and
  on Windows it is `FlushFileBuffers` — a full disk sync. One API, two meanings,
  neither of them the promised one. The header now says exactly that, and says not to
  use it.
- **`proven_arg_f64`'s documentation was wrong twice.** It is not round-half-up (it is
  correctly rounded, ties-to-even, matching `printf("%.6f")`), and the form is not
  always fixed-point (`5e-7` renders as `5.000000e-07`, not `0.000000`).
- Six more false claims found while making the manual's code compile: the pool's
  `item_align` is an upper bound, not an exact match; the panic fallback is `while(1)`
  on non-GCC compilers; the buffered scanner *does* refill and retry rather than
  failing on a token that reaches the end of the buffer; `proven_fs_stat_t.created_at`
  is always 0; `PROVEN_FS_TYPE_OTHER` is never produced; and `src/proven/time.c` *is*
  compiled in freestanding builds (only the clock backend is absent).

### Added

- **`docs/RFC-0001-streams-and-io.md`** — the design for what is missing, with the
  measurements behind it. The short version: **there is no stream abstraction.** No
  `proven_writer_t`, no `proven_reader_t`. The formatter's only sink is
  `proven_u8str_t`, so you cannot format into a file; there is no line reader, so you
  cannot read a file line by line without loading all of it; and `proven_println`
  issues **10,000 `write()` syscalls and 10,000 mallocs for 10,000 lines** (stdio: 47
  syscalls, 0). The logging path allocates — the one place an allocation is least
  welcome. Seven backlog items (B-004 … B-010) and a ten-step plan, ordered so that
  each step is useful on its own.
- `tests/test_regression_fmt_spec_silently_wrong` — pins both silent formatter
  defects. Verified to fail against the pre-fix source.
- **The build now compiles every code block in the manual.** `nob` extracts each `c`
  block, wraps it in a function body, and syntax-checks it. A chapter whose code stops
  compiling stops the build.

### Changed

- **Every code block in the manual is now real.** Of ~190 fenced `c` blocks, four could
  be compiled before this cycle. Every one is now either a compiled-and-run program
  from `manual/examples/`, a fragment the build syntax-checks, or a `text` fence for
  the things that are not runnable code (signature listings, struct listings,
  deliberate counter-examples). Closes `docs/BACKLOG.md` B-002.

## [2026-07-12] — proven_c_lib-v26.07.12e

### Added

- **Manual chapter 8, sections 7-13** — the scanner half of the chapter, which had
  never been written. The chapter listed thirteen sections and ended at a bare
  `## 7. Scanner data model` heading. Closes `docs/BACKLOG.md` **B-001**.

  It was written against *measured* behaviour, not against the header, and that is
  how the surprising parts came to be documented at all:

  - `proven_scan_i64("0x10")` is the integer **zero**, cursor at 1. The integer
    scanners are decimal only - no hex, no octal, no base prefix.
  - `"1e309"` is `PROVEN_ERR_OVERFLOW`, but `"1e-400"` is `PROVEN_OK` with the value
    `0.0`. The asymmetry is deliberate: underflow to zero *is* the correctly rounded
    answer, while overflow has no correct finite answer at all.
  - `proven_scan_u64("-1")` is rejected rather than wrapping to a huge unsigned
    value - which is how a bounds check gets defeated.
  - **The structural scanner is not transactional.** When a literal fails to match,
    the placeholders *before* the mismatch have already been written through: the
    call returns `PROVEN_ERR_NOT_FOUND` and your destination holds a value anyway.
    On failure, treat every destination as clobbered.
  - Trailing input is **not** an error. The scanner matches what you asked for and
    stops; it does not police what you did not ask about.

- `manual/examples/ex_08_scan_recovery.c` — provokes every scan error code on
  purpose, including the non-transactional failure. Compiled and run by the build.
- `tests/test_docs_manual_ch08_contracts` — asserts each of the 18 behaviours
  chapter 8 states as fact. Prose is where a contract goes to drift; this one
  cannot. A false claim fails the build and names itself.

## [2026-07-12] — proven_c_lib-v26.07.12d

The manual's examples are now programs, the tests are named for what they check,
and the testing policy says out loud how this project actually develops.

### Added

- `manual/examples/` — eleven complete programs, one per topic the manual teaches.
  The build driver compiles and **runs** every one of them, under every sanitizer
  mode. They are written the way a caller writes code: explicit allocator, real
  error handling, a destroy for everything owned.
- `tests/test_docs_manual_examples` — requires every example the manual prints to
  be one of those programs, quoted verbatim; fails the build if a chapter and its
  example disagree, if a chapter quotes an example that does not exist, or if an
  example exists that no chapter shows.
- `docs/TESTING.md` — the testing policy: the naming scheme, what each test class
  is *for*, the rules a new test must satisfy, and an honest account of how this
  project develops. It records plainly that this is not TDD: every commit that
  adds a test also changes source in the same commit, and there is not one where a
  failing test lands first.
- `docs/BACKLOG.md` — a **tracked** backlog. The repository had `BACKLOGS.md` and
  `TODO.md`, but both are gitignored: a private queue nobody else can read and no
  commit can reference. Known work that lives on one machine is not tracked work.

### Changed

- **Tests are renamed for what they check.** `test_phase1` … `test_phase22` encoded
  the order they were written in, which is the one fact about a test nobody needs.
  Every test is now `test_<class>_<subject>`, where the class is one of `unit`,
  `contract`, `regression`, `differential`, `portability`, `stress`, `docs`,
  `bench`. 75 files renamed.
- **The test catalog has no numbers.** It ran `1..50` with `7a`, `30a`, `30b`,
  `30c`, `40a` wedged in wherever something new arrived — and five of its entries
  described files deleted months earlier. The filename is the identifier now, and
  the catalog is grouped by class.

### Fixed

- manual chapter 3 listed `proven_u8str_t` without its `borrowed` field, and told
  readers to use `internal.size` for the length. There is no `size` member -
  `proven_buf_t` is `ptr` / `len` / `cap` - so code following the manual did not
  compile.
- manual chapter 5 never said how end-of-file is reported. `proven_fs_read`
  returns `PROVEN_ERR_EOF`, not a zero-byte success, so the obvious read loop
  (`if (r.value == 0) break;`) never takes that branch and treats the end of the
  file as an I/O failure. The chapter now says so, and the worked example shows the
  correct shape.
- manual chapter 5's `proven_fs_stat_t` listing claimed a symlink file type.
  `proven_fs_type_t` has only `_FILE`, `_DIR` and `_OTHER`.

### Known

Two items are registered in `docs/BACKLOG.md` rather than rushed:

- **B-001** — manual chapter 8 ends mid-chapter at a bare `## 7. Scanner data
  model` heading. Sections 7-13 are in the table of contents and absent from the
  document: roughly half the chapter, and the half covering the scanner.
- **B-002** — of the manual's ~190 fenced code blocks, four could be compiled
  before this release. Eleven are now real programs; the rest are still sketches
  that reference imaginary helpers. They are being converted chapter by chapter,
  with the mechanism already in place to keep each finished chapter finished.

## [2026-07-12] — proven_c_lib-v26.07.12c

A documentation-currency release, plus the API-surface gap that the sweep turned up.

### Added

- 25 missing `xcv_*` aliases. `include/proven/alias_xcv.h` claims to cover the
  public API and did not: three functions added in v26.07.12b had no alias, and
  22 more had been missing for months - among them `proven_fs_write_all`,
  `proven_panic`, `proven_strtod`, `proven_pool_destroy` and the `sysio` scanner
  entry points. An alias layer that covers most of the API is worse than none:
  the caller finds the gaps one compile error at a time, at whichever call site
  happens to need the one function nobody aliased. The layer now covers all 203
  public functions.
- `tests/test_alias_completeness` - parses the public headers and the alias
  header and fails the build if any public function has no alias. `test_alias_smoke`
  could never have caught this: it hand-picks a subset of aliases and only checks
  that they compile, so it cannot notice one that is absent. This is the only way
  a list like that stays true.
- Two tests that existed on disk but were never registered in `nob.c` -
  `tests/test_float_format_shortest_roundtrip` and `tests/test_float_parse_benchmark` -
  are registered and now actually run. Both pass.

### Fixed

- `proven_array_sort`'s header comment still described it as "a robust quicksort".
  It is an introsort, and the two properties that matter to a caller - the
  O(n log n) guarantee, and duplicate keys being the fast case rather than the
  quadratic one - were documented nowhere a caller would look.
- The allocator's alignment-class contract was undocumented. v26.07.12b made the
  heap allocator route `align <= alignof(max_align_t)` through malloc/realloc (so
  growth can happen in place) and over-aligned requests through the aligned
  family. A block must therefore be reallocated and freed with the alignment it
  was allocated with - a real obligation on callers that existed only as a comment
  in a `.c` file. Now stated in `allocator.h`, `platform/proven_sys_mem.h`, and
  manual chapter 2.
- `docs/float-correctness-and-performance.md` still said the parser used
  `long double` to seed its exponent estimate. It has not since v26.07.12b; the
  whole engine is integer-only now, formatter and parser alike.
- `proven_fs_stat`'s `perms` field is documented as carrying only the nine
  permission bits, and the manual now says why that changed: it used to hand back
  the raw `st_mode`, whose file-type bits `proven_fs_chmod` rejects, so the
  obvious round-trip failed for every real file.
- `TEST.md` claimed 48 hosted tests (there are 75), documented five test files
  that were deleted months ago, and never mentioned five that exist. It now
  matches the tests on disk and the registry in `nob.c`.
- `manual/manual-07-alias-xcv-index.md` was missing 37 aliases and 340 of its 379
  rows had the wrong line number. Regenerated from the header, and the line-number
  column is gone: it was wrong after every alias inserted above it, which is worse
  than not having the column.
- `manual/manual-01-foundation.md` showed a stale `PROVEN_VERSION_NUM` and suffix.
- `CHECKLIST.md` told the maintainer to sync the version string in `SPEC.md` and
  `docs-site/index.html`, neither of which exists, and its "Active Task" was work
  finished several releases ago.
- References to `docs/internal/` now say plainly that it is maintainer-local and
  not part of the published repository, instead of reading as a path the reader
  could follow.

### Changed

- README states where the platform boundary stops - the PAL covers memory,
  filesystem, time, mmap, environment, console I/O and threads, and does *not*
  cover process control, terminal control, or networking - and names the
  deliberate non-goals (hashing, path manipulation, argument parsing, logging).
  A boundary you have to discover by running into it is a worse boundary than one
  that is written down.
- README documents the whole-file I/O added in v26.07.12b and the sort's
  guarantees, in both language halves; the Korean quick start now matches the
  English one.

## [2026-07-12] — proven_c_lib-v26.07.12b

### Fixed

- `proven_array_sort` was quadratic on duplicate keys. The Lomuto partition used
  a strict `cmp(x, pivot) < 0` test, so every element *equal* to the pivot went
  into the right partition; on a low-cardinality key - a status column, an enum,
  a bucket id - the split collapsed to 1/(n-1). Sorting 100,000 identical
  `int32` keys took **10.6 seconds**. A caller sorting data an attacker can shape
  had a denial of service, not merely a slow path. Replaced with an introsort:
  a Bentley-McIlroy three-way partition (so an equal run is final and never
  recursed into, and every element is compared exactly once), an insertion-sort
  cutoff, and a heapsort fallback past a depth of `2*log2(n)` - which is what
  makes the O(n log n) bound a guarantee rather than a hope, since
  median-of-three alone can still be driven quadratic.
- A `proven_job_system` worker could exit leaving an accepted job unrun. A
  submitter that passed `begin_submit` before the close can have claimed its slot
  with the enqueue CAS without yet publishing `cell->sequence`, which to a
  dequeuer is indistinguishable from an empty queue. The last worker could
  therefore exit, the submitter then publish, and `proven_job_submit` return true
  for a job nobody would ever run - while `proven_job_system_destroy`, documented
  to block until the queue is exhausted, returned. A worker now leaves only once
  no submitter is in flight *and* the queue is still empty when re-checked.
- `proven_fs_read_all` allocated twice the file size for every regular file: the
  buffer was seeded to the exact reported size, so the read loop filled it and
  then had to grow before it could issue the read that would observe EOF. Peak
  memory was 3x the file. EOF is now confirmed with a one-byte probe, and the
  buffer grows only if the source really does outrun its reported size.
- `proven_fs_read_all_u8str` started from a one-byte buffer for any source that
  reports no size: the chunk fallback tested the capacity, which is never 0 once
  a terminator byte is reserved. Reading `/proc/self/status` took 12 reallocs.
- `proven_fs_write_file_atomic` widened permissions. The temp sibling is created
  with `0666 & ~umask` and `rename` carries its mode onto the target, so
  atomically rewriting a `0600` key file republished it as `0644`. The target's
  mode is now copied onto the temp before the rename.
- `proven_fs_write_file_atomic` failed on legal long filenames: a 250-character
  basename - which `proven_fs_write_file` accepts - made the temp sibling exceed
  `NAME_MAX`. The copied stem is now trimmed to leave room for the suffix.
- `proven_fs_stat` put the raw `st_mode` into `perms`, a field typed
  `proven_fs_perms_t`. `st_mode` also carries the file-type bits, and
  `proven_fs_chmod` rejects any bit outside the nine it supports - so
  `chmod(path, stat(path).perms)`, the obvious use of the field, returned
  `PROVEN_ERR_INVALID_ARG` for every real file. `perms` is now masked to the low
  nine bits.

### Changed

- The decimal parser's exponent-bounds estimate no longer uses `long double`,
  the one type in C whose width differs across the targets this library builds
  for (80-bit on x86, 128-bit on aarch64, plain 64-bit on armhf and MSVC). The
  estimate happened to come out identical on all of them - verified over the
  entire input range it can see - so nothing was ever wrong, but a
  correctness-critical path had no business depending on it, and on a soft-float
  target it pulled in libgcc routines for no reason. Replaced with an integer
  fixed-point computation, bit-identical to the exact `floor(k * log2(10))` for
  every k in range, and verified to produce byte-identical parse results over
  3,000,000 randomized decimal inputs.
- `proven_fmt` appends literal text in runs instead of one character at a time.
  Each literal character used to cost a checked add, an out-of-line one-byte
  move and a NUL reseal - twice, since the format string is walked once to
  measure and once to write. A 101-character literal went from 1322 ns to 166 ns;
  a four-argument log line from 998 ns to 307 ns.
- `proven_map_set` no longer probes the same chain twice. It validated the map,
  then `set_with_scratch` validated it again, then walked the probe chain looking
  for an existing key, then called `map_insert_no_grow` - which walks the same
  chain and already overwrites a key it finds. The probe is now taken only when
  the map is about to grow, where it still saves an unnecessary rehash. 500k
  int inserts: 215.5 ns -> 190.2 ns per op.
- Sorting wide elements is faster: `swap_elements` takes a bulk-copy branch above
  16 bytes instead of swapping a byte at a time. 100k 48-byte structs:
  59.2 ms -> 16.2 ms.

## [2026-07-12] — proven_c_lib-v26.07.12a

### Fixed

- `proven_fs_read_all` silently returned an empty buffer for any source whose
  size cannot be measured up front. `proven_sys_fs_size` reports 0 for anything
  that is not a regular file, and `read_all` used that 0 as its buffer size, so
  reading a FIFO, a character device, or a `/proc` entry succeeded with zero
  bytes and dropped the contents. `proven_fs_size("/proc/self/status")` is
  `0`/`PROVEN_OK`, so a 1516-byte file read as empty. `read_all` now reads to
  EOF and uses the reported size only to seed the initial capacity: a regular
  file is still one allocation and one pass, an unmeasurable source is read
  correctly, and a regular file that grows mid-read is no longer truncated.
- Stack buffer overflow formatting a `proven_datetime_t` with a negative year.
  `year` is `proven_i32`, but it was cast to `unsigned long long` before
  conversion, so `-1` became `18446744073709551615` — twenty digits plus a NUL
  into a twenty-byte scratch buffer (ASan: stack-buffer-overflow in `itoa_raw`).
  The year now renders with its sign, and the scratch holds any 64-bit value.
- `proven_sysio_scanner_scan_impl` corrupted the stream when it rolled back a
  failed scan. `scanner_fill` compacts the buffer, but the rollback restored the
  cursor and length captured *before* that compaction, so the restored indices
  described different bytes: one byte was dropped from the front of the stream
  and one byte — already returned to the file by the rewind — was read twice.
  The rollback now accounts for how far the buffer moved. On a non-seekable
  input, where the rewind cannot succeed, the bytes already read are kept
  buffered instead of being discarded.
- `proven_u8str_reserve` and the growth path of the formatter left `ptr[len]`
  uninitialized. Both allocate, and allocators do not return zeroed memory, so
  reserving on a zero-initialized string — or formatting something that produces
  no output — broke the NUL seal that `proven_u8str_as_cstr` is documented to
  rely on, and `proven_u8str_is_valid` rejected the result. Both paths now seal
  the terminator.
- `proven_pool_init` published `bin_cap` before allocating the bin behind it, so
  a failed init left a pool claiming slots it did not have. The free trait tests
  `bin_len < bin_cap` and then writes `bin[bin_len]`, which with `bin == NULL` is
  a null write. `bin_cap` is now set only after the bin exists.
- Unchecked `count * size` arithmetic in `proven_sysio_scanner_scan_impl`, a
  public entry point that takes `args_count` from the caller. It is now routed
  through `PROVEN_CKD_MUL` like every other size computation in the library.

### Added

- `proven_fs_read_all_u8str`: the whole-file read most callers actually want,
  returning a NUL-terminated owned `proven_u8str_t` so `proven_u8str_as_view` and
  `proven_u8str_as_cstr` work on the result without a second copy. The terminator
  slot is reserved up front, so it costs no extra allocation over `read_all`.
- `proven_fs_write_file`: one-call create-or-truncate whole-file write, the half
  of the API that was missing next to `read_all`.
- `proven_fs_write_file_atomic`: writes through a sibling temp file and renames
  it over the target, so a concurrent reader never observes a half-written file.
  Atomic with respect to readers, not durable across power loss — proven exposes
  no fsync, and the header says so.
- `[[nodiscard]]` on `proven_sysio_scanner_scan_impl` and
  `proven_sysio_scan_chunk_impl`. `proven_sysio_print_impl` is deliberately left
  unannotated: `proven_print` expands to it and is used as `printf` is.

### Changed

- `proven_sys_mem_realloc` can now grow a block in place. Every allocation used
  to go through `posix_memalign` / `_aligned_malloc`, which cannot be handed to
  `realloc()`, so growth always paid a full copy. Requests at or below
  `alignof(max_align_t)` — every string, buffer, and byte array in the library —
  now come from `malloc` and grow through `realloc`, which for large blocks
  remaps pages instead of copying them. Over-aligned requests keep the aligned
  path. Windows keeps every block on the aligned family (`free` and
  `_aligned_free` are not interchangeable, and the free trait is not told the
  alignment) and uses `_aligned_realloc`. Failure atomicity is unchanged.
  Measured, with every byte of the buffer written: growing a buffer to 256 MiB by
  doubling went from 0.69s to 0.32s (2.1x); 200k small allocations with six
  reallocs each went from 0.05s to 0.035s (1.4x).

## [2026-06-24] — proven_c_lib-v26.06.24b

### Fixed

- Build break on older GCC under the `-std=c2x` fallback: `src/proven/job.c`
  used `alignof` without `<stdalign.h>`. `alignof` is a first-class keyword only
  in C23; under the documented `-std=c2x` fallback (and C11/C17) it is a macro
  that `<stdalign.h>` provides. On a compiler new enough to keyword-ify `alignof`
  under c2x (e.g. GCC 14) it built anyway, which is why builds on newer toolchains
  did not catch it; on an older GCC the c2x fallback failed to compile at
  `job.c:111`, taking down the whole hosted build. Reported by an external tester
  whose default GCC fell back to c2x (their clang regression / ASan / UBSan /
  freestanding runs passed).
- Fix is centralized: `<stdalign.h>` is now included from `include/proven/types.h`,
  the foundation header every translation unit pulls in, so all `alignof`/`alignas`
  users are covered regardless of their own include list. This also closes the same
  latent gap in `fmt.c`, `pool.c`, and `sysio.c` (which previously relied on
  transitive includes). Verified: `alignof` via `proven/types.h` compiles even
  under `-std=c11` (where it is not a keyword); full gcc build, `strict-error`, and
  `freestanding` gates pass.

## [2026-06-24] — proven_c_lib-v26.06.24a

### Changed

- Documentation release (no library code changes). Brought the manuals and README
  current with v26.06.22a and added deep-dive sections to the chapters:
  - `manual/manual-04`: how the hash map works internally (bucket layout,
    FNV-1a / bit-mix hashing, linear probing, tombstones, the 3/4 load factor and
    rehash, the three key modes, and the `set_with_scratch` alias case).
  - `manual/manual-06`: the job system's concurrency model (atomic MPMC ring,
    lifecycle state machine, memory-visibility via the destroy/join sync point) and
    the stackless-coroutine expansion with the "locals do not survive a yield" rule.
  - `manual/manual-02`: how the pool's recycle bin works, with misuse cases.
  - `manual/manual-08`: an "Inside the engine" section for the float parse tiers
    (Clinger / Eisel-Lemire / exact big-integer) and the two formatters (Grisu3 +
    Dragon4 shortest, exact integer `%f`/`%e`).
  - `manual/manual-03`: the `proven_u8str_t` internal layout (`proven_buf_t internal`
    + `borrowed`) with a borrowed-string counter-example.
  - `manual/manual-05`: `proven_fs_stat_t` now documents `uid`/`gid`.
  - `manual/manual.md`: corrected the arena ownership row (caller-backed bump
    pointer, not an owner); header map adds `config.h`, `float_parse.h`,
    `float_format.h`, `float_config.h`.
- Moved the internal-only docs (`docs/internal/`: benchmarks, RFC drafts, overhaul
  plans) out of the repository into the private workspace and gitignored the path.

## [2026-06-22] — proven_c_lib-v26.06.22a

### Added

- `proven_fs_stat` now exposes file ownership: `proven_fs_stat_t` gains
  `unsigned long long uid` and `gid`, populated from `st_uid` / `st_gid` on
  POSIX and set to `0` on Windows (which has no POSIX ownership). The sys-level
  `proven_sys_fs_stat_t` carries the same two fields. Resolves the prov_text_editor
  enhancement request (docs/REPORT.md, 2026-06-19) that blocked the file browser's
  owner/group columns. Verified in `tests/test_phase14_fs_advanced.c` (uid/gid
  equal `getuid()`/`getgid()` for a just-created file on POSIX).

## [2026-06-21] — proven_c_lib-v26.06.21a

### Fixed

- `map.c`: silenced a `-Wunused-parameter` warning on the `map` argument of
  `map_key_is_valid`. Its only use is the hardened overlap check, which is
  compiled out on `-DNDEBUG` non-hardened builds, so downstream release builds
  (`-Wall -Wextra -DNDEBUG`) saw the warning. Added `(void)map;`. Reported via
  `docs/REPORT.md`.

### Changed

- Synced the version string to `proven_c_lib-v26.06.21a` across
  `include/proven/version.h`, `README.md`, `TEST.md`, and the `manual/`
  chapters. Also corrected the `manual-01` version-macro example, whose
  `STRING`/`NUM`/`SUFFIX` lines had drifted out of sync with each other.

## [2026-06-18] — proven_c_lib-v26.06.18b

### Added

- `proven_mem_move(dst, dst_cap, src_view)` (`memory.h`): a bounded,
  overlap-safe byte move with the same guards as `proven_mem_copy` (overflow →
  `PROVEN_ERR_OUT_OF_BOUNDS` without writing, null with size → `INVALID_ARG`,
  zero size → no-op). Lets downstream code drop libc `memmove` for overlapping
  array-element shifts. XCV alias `xcv_mem_move`.

## [2026-06-18] — proven_c_lib-v26.06.18a

### Added

- `proven_u8str_borrow(buf, cap)` and `proven_u8str_reset(str)` (`u8str.h`):
  wrap caller-owned memory as a fixed-capacity string and truncate-to-empty for
  reuse. A new `borrowed` flag on `proven_u8str_t` defaults to owned, so a
  zero-initialized handle keeps its existing semantics. The fixed-capacity
  operations and `proven_u8str_append_fmt` work on a borrowed string; the
  growing operations (`reserve`, `*_grow`, `append_byte`, `append_fmt_grow`)
  still succeed while the data fits but return `PROVEN_ERR_OUT_OF_BOUNDS`
  instead of reallocating caller memory, and `proven_u8str_destroy` is a no-op
  for a borrowed string. This lets allocator-free and per-frame call sites use
  the proven string system / formatter without heap allocation. Requested by a
  downstream project (`docs/REPORT.md`, 2026-06-18).
- `proven_mem_copy(dst, dst_cap, src_view)` (`memory.h`): a bounded byte copy
  that rejects overflow without writing, treats a zero-size source as a no-op,
  and rejects null pointers.
- XCV aliases `xcv_u8str_borrow`, `xcv_u8str_reset`, `xcv_mem_copy`.

### Changed

- `proven_u8str_t` gains a trailing `bool borrowed` field. `proven_buf_t`
  layout is unchanged. No public API consumes `sizeof(proven_u8str_t)` by
  contract; source compatibility holds after a recompile.

### Fixed

- `proven_diy_fp_normalize` (`float_decimal.c`) left-shifted a 64-bit value by
  the type width when the significand was zero (`clz` returns 64), which is
  undefined behavior surfaced by UBSan. Guard the zero case to mirror the
  previously-masked result (significand stays 0) without the UB; no change on
  any non-zero input, so formatter output is unchanged.

## [2026-06-17] — proven_c_lib-v26.06.17a

### Fixed

- Made `src/proven/float_decimal.c` compile on hosted targets without 128-bit
  integers (e.g. 32-bit ARM / `linux-armhf-hosted`). The Eisel-Lemire fast path
  is `__int128`-only, but its guard boundaries were inconsistent: helper calls
  sat outside the guard while definitions sat inside (and vice versa), so the
  cross matrix's new Windows link smoke exposed it via `arm-linux-gnueabihf-gcc`
  failures (implicit declarations, used-but-undefined, and unused-function
  `-Werror`). Moved the int128-free `proven_float_pack_binary64_candidate` out
  of the guard, added `#else` stubs for the two Eisel-Lemire entry helpers so
  the unconditional dispatcher links and reports "unsupported" (falling back to
  the scalar exact path, which already had a non-int128 multiply), and marked
  the Eisel-Lemire-only helpers `[[maybe_unused]]`. No behavior change on
  targets with `__int128` (x86-64 output is unchanged). `./nob cross` now passes
  every target, including both Windows link smokes.

## [2026-06-16] — proven_c_lib-v26.06.16x

### Fixed

- Made the panic handler link on Windows / PE-COFF (mingw-w64). The previous
  weakly-linked `proven_panic_handler` default linked on ELF but not on PE: a
  weak function definition in a separate object did not satisfy references,
  producing `undefined reference to proven_panic_handler` on every Windows link
  (the cross matrix is compile-only, so this was latent). Reported in
  `docs/REPORT.md`.

### Added

- Cross matrix link smoke for the Windows targets (`./nob cross`): the
  `windows-*` targets now link the full proven object set into an executable
  (`tests/test_cross_link_smoke.c`) instead of compiling only, so link-time
  symbol-resolution differences from ELF (such as the PE/COFF weak-symbol issue
  above) are caught instead of slipping through.

### Changed

- Replaced the weak-symbol panic override with a portable registration model:
  the library now raises panics via `proven_panic()` and installs handlers via
  `proven_set_panic_handler(proven_panic_handler_t)` (pass `NULL` to restore the
  trapping default). **Breaking:** defining a strong `proven_panic_handler` no
  longer overrides the handler; call `proven_set_panic_handler()` instead.
  Updated call sites (`pool.c`, `arena.h`), the override tests, and the panic
  documentation in `manual/` and `TEST.md`.
- Bumped the version to `proven_c_lib-v26.06.16x` and synced the version string
  across `include/proven/version.h`, `README.md`, `TEST.md`, and `manual/`.

## [2026-06-16] — proven_c_lib-v26.06.16w

### Changed

- Bumped the version to `proven_c_lib-v26.06.16w`, releasing the editor-oriented `proven_u8str` work: the growing in-place edit variants and the multi-algorithm substring search. Synced the version string across `include/proven/version.h`, `README.md`, `TEST.md`, and the `manual/` chapters.

### Added

- Added growing variants of the in-place string edits: `proven_u8str_insert_grow` and `proven_u8str_replace_at_grow` (plus `xcv_` aliases). They have the same semantics as `proven_u8str_insert` / `proven_u8str_replace_at` but grow the buffer (doubling capacity) when the edit does not fit instead of returning `PROVEN_ERR_OUT_OF_BOUNDS`, so callers (for example a text editor making mid-buffer edits) no longer have to `reserve` manually before every insert. On allocation failure the string is left unchanged. New unit coverage in `tests/test_phase7_u8str_mut`.
- Added `proven_sys_mem_chr` to the platform memory layer: the system `memchr` when hosted, a freestanding-safe SWAR (word-at-a-time) scan otherwise. Verified against `memchr` over 2,000,000 randomized cases.

### Changed

- Rewrote `proven_u8str_view_find` from a naive O(n·m) byte loop to a multi-algorithm search that is self-contained (does not rely exclusively on `memchr`) and behaves identically under freestanding. The fast path samples the haystack, anchors on the rarest needle byte, scans with `proven_sys_mem_chr`, and verifies — fast on real text because a typical needle has a rare or absent byte. When the sample shows a low-entropy haystack (small effective alphabet: DNA, binary, long runs), it falls back to a linear, alphabet-independent algorithm: **Shift-Or / bitap** for needles up to 64 bytes, and **Two-Way (Crochemore-Perrin)** for longer needles. The long-needle fallback is compile-time selectable via `PROVEN_U8STR_FIND_LONG` (1 = Two-Way default, 2 = memchr-adaptive); Two-Way was chosen by benchmark (0.97× vs glibc `memmem` on a long-needle/long-verify input where memchr-adaptive is 3.1×). On realistic text the search is 4–30× faster than glibc `memmem`; single-byte search equals `memchr`; low-entropy cases stay at or below `memmem`. Validated for first-match equivalence against host `memmem` over 3,000,000 cases per alphabet (2/4/26 symbols, needles 0–139 bytes) for the dispatch and each forced algorithm, with zero mismatches; ASan/UBSan clean. Benchmark: `docs/internal/benchmarks/20260616-152810-u8str-find-multi-algorithm.md`.

## [2026-06-16] — proven_c_lib-v26.06.16v

### Changed

- Bumped the version to `proven_c_lib-v26.06.16v` (`PROVEN_VERSION_NUM` 260616), releasing the exact/fast floating-point parser and formatter work, the exhaustive and large-scale validation, and the documentation overhaul. Synced the version string across `include/proven/version.h`, `README.md`, `TEST.md`, and the `manual/` chapters.
- Documentation reorganization and refresh. Moved the dated benchmark reports and the design proposals/RFC audits under `docs/internal/` (with a `docs/internal/README.md` describing the folder), since they are development records rather than user docs; `docs/float-correctness-and-performance.md` remains the user-facing summary. Updated the floating-point section of `manual/manual-08-fmt-scan.md` to describe the current exact, correctly-rounded (round-half-to-even) formatter and three-tier parser with worked examples (the old text still described an approximate six-digit/round-half-up formatter). Rewrote the relevant parts of `README.md` (both language halves) to add a "correct, fast number conversion" section with example code and objective validation/benchmark numbers, list the `float_parse`/`float_format` modules, and fix the documentation index, which pointed at files that are not part of the published repository (`SPEC.md`, `AGENTS.md`, `MEMORY.md`).

- Replaced the shortest float formatter. First with a single-pass exact algorithm (Burger-Dybvig / Dragon4, round-to-nearest-ties-to-even) for binary64 and binary32, then with a Grisu3 fast path (64-bit diy_fp + a generated cached-power table) that falls back to the exact path only when it cannot prove the result is shortest. Net result is about 670x faster than the original round-trip-search formatter (59,018 -> 88 ns/call on a mixed corpus) and uniform across magnitudes, with the same correctly-rounded minimal output (validated round-trip and minimality over ~3M doubles and ~5M floats).

### Removed

- Removed the obsolete hand-maintained shortest literal table (`proven_float_shortest_literal_f64`/`_f32` and their tables) now that the shortest formatter computes every value directly, along with the structural tests that pinned the old staged round-trip-search backend.
- Removed the dead round-trip-search fixed-precision machinery in `float_format.c` (`proven_float_format_roundtrip_search_fixed`, `candidate_exact`, `candidate_roundtrips`, `roundtrips_f64`/`_f32`, `adjust_fixed_neighbor`, `build_scientific_ld`, `normalize_scientific_ld`). It was reachable only through the `RYU` policy in `FIXED` mode, which no caller or test used; that combination now routes to the same exact integer path as the other policies. This removes the formatter's last `long double` use, so `float_format.c` is now entirely integer-based. Behavior of the exercised paths is unchanged (re-validated against host `snprintf` over ~3M doubles at precisions 1..18, zero mismatches).

### Fixed

- Canonicalized shortest float output for values just below a power of ten. The digit generators (both Grisu3 and Dragon4) could leave a spurious leading zero with the decimal exponent one too high — e.g. `9.995442674871462e-265` was emitted as `0.9995442674871462e-264` — which round-tripped correctly but was non-canonical and inflated the reported significant-digit count by one. `proven_float_shortest_digits`/`_f32` now strip leading zeros and lower the decimal exponent. Found by a 2.56-billion-value `binary64` differential check against host `strtod` (93 affected values, all near a power of ten); the value, round-trip property, and minimal length are unchanged. The exhaustive `binary32` sweep was unaffected (it had no such cases) and still passes.
- Made fixed-precision float formatting (`%f`/`%e` via `proven_float_format_f64_policy` and the `{}` formatter) exact and correctly rounded. The previous path used `double`/`long double` arithmetic capped at 18 fractional digits and produced wrong digits for high precision, values at or above 2^64, subnormals, and boundary cases; a differential check against host `snprintf` went from roughly 20% mismatches to zero across 4,000,000 value/precision pairs. The new path is integer-only (no long double), correctly rounds to nearest-even, and supports arbitrary precision up to the big-integer capacity.
- Fixed an out-of-bounds read in `proven_float_bigint_cmp_shift_left` when the shift was a multiple of 64: the low zero-padding limbs were not compared and the index underflowed. This helper is shared with the decimal parser's exact comparison; the parser's differential fuzz remains at zero mismatches after the fix.
- Corrected the shortest float formatter to emit the true minimal round-tripping form. The shortest search now generates candidates with the exact digit engine and no longer consults the hand-maintained literal table, several of whose entries were non-minimal (for example the largest subnormal and `FLT_MIN`).

### Added

- Added `docs/float-correctness-and-performance.md`, a self-contained reference describing the parsing/formatting algorithms (three-tier Clinger / Eisel-Lemire / exact-fallback parser; Grisu3 + Dragon4 shortest; exact big-integer `%f`/`%e`), the validation methodology, and the performance comparison against the host C library. Backed by an **exhaustive** sweep of all 4,278,190,080 finite `binary32` values (shortest round-trip + minimality via host `strtof`, parser bit-exact vs host `strtod`) and a **2,560,000,000-value** randomized `binary64` differential sweep against host `strtod`, both with **zero** failures, plus a host-comparison benchmark. Dated raw outputs live under `docs/internal/benchmarks/` (`*-f32-exhaustive-validation.md`, `*-float-vs-host-benchmark.md`). The benchmark shows the library is faster than glibc at parsing typical numbers, shortest formatting (~4-5x), and `%f`/`%e` at normal magnitudes, while staying bit-identical to `strtod`/`snprintf`.
- Added a big-integer division helper (`proven_float_bigint_divmod`, Knuth Algorithm D on base-2^32 limbs, no `__int128`, freestanding-safe) with a limb-array entry point `proven_float_bigint_divmod_u64` and a unit test (`tests/test_float_bigint_divmod`). It is a validated reusable primitive; a measured experiment showed that using it to compute the exact-fallback float result is slower than the existing estimate-seeded search, so the parser keeps the search and the division is not on the parse hot path.
- Made the exact-fallback big-integer capacity configurable through `PROVEN_FLOAT_BIGINT_LIMBS` (in `include/proven/float_config.h`, default 160). Lowering it shrinks the exact-fallback and division stack footprint for embedded targets (for example `-DPROVEN_FLOAT_BIGINT_LIMBS=48` cuts the division frame from ~10.5 KB to ~3.3 KB and the converter from ~6.7 KB to ~2.2 KB). The kept-significand cap is derived from the capacity, so reduced builds still parse correctly up to that many significant digits and stay within one ULP beyond it; the Clinger and Eisel-Lemire fast paths never use the big integer. The build driver now also forwards `-cflags` to test compilation so such config macros stay consistent between library objects and tests.

### Changed

- Made the float parser treat a dangling exponent marker the way `strtod` does. A trailing `e`, `e+`, or `e-` that is not followed by exponent digits (for example `1e`, `1e+`, `1.5eZ`) is no longer rejected; the parser keeps the mantissa parsed so far and stops at the `e`. This affects `proven_parse_double_ascii`, `proven_strtod`, and `proven_scan_f64`, which share the ASCII token scanner.

### Fixed

- Removed the hard limit that rejected decimal inputs with more significant digits than the exact-fallback bigint could hold (about 3080 digits), which previously returned `0` and consumed nothing. The exact significand is now capped at a fixed number of kept digits (800, above the 767-digit worst case for binary64 rounding); digits past the cap shift `exp10` and set a sticky flag that breaks an exact-equal comparison upward, so arbitrarily long inputs parse correctly in bounded time. Validated against host `strtod` with a 300,000-case fuzz over 700-3099 digit mantissas and targeted midpoint sticky tie-break cases.
- Corrected `significant_digits` in the decimal metadata so it no longer counts trailing zeros that are already folded into `exp10`. The inflated count biased the magnitude estimate and the derived binary-exponent search bounds high, which could place the true result outside the exact-search range and yield a power-of-two result for some long-mantissa inputs (for example `12345678901234567890` and `109.31074080952665007690591502623020`). A 5,000,000-case randomized differential check against host `strtod` now reports zero mismatches.

### Changed

- Seeded the exact-fallback binary search from a cheap reconstructed double estimate, narrowing the search to a verified window around it before falling back to the full exponent-bracket range. The window is only adopted after a two-point bracket check, so the rounded result is unchanged while the `fallback` and `boundary_tie` benchmark groups drop by roughly one half.

### Removed

- Removed four unused static float helpers (`proven_float_bigint_add`, `proven_float_compare_mantissa_to_scaled`, `proven_float_compare_decimal_to_bits_legacy`, `proven_float_compare_decimal_to_midpoint_legacy`) left over from the decimal-to-binary64 rewrite; they were dead code and tripped the `-Werror` strict and freestanding builds.

### Changed

- Reverted the staged positive-exponent scalar helper experiment after benchmark runs showed it regressed the path corpus, restoring the prior baseline for `staged_scientific`, `fallback`, and `boundary_tie`.
- Optimized the legacy negative-exponent exact compare path to build a cached pow5 factor once before comparing, which shaved a little more off `fallback` and `boundary_tie`.
- Kept the cached pow5 factor reuse on the legacy negative-exponent compare path after the follow-up compare-path tweak showed mixed results; the retained change avoids repeated `5` multiplication while leaving the public parser behavior unchanged.
- Added an equal-exponent fast path in the adjacent-midpoint helper so the exact boundary compare can skip two generic shifts when both sides already share the same exponent.
- Reworked the hot `proven_float_bigint_mul_u64_factor()` carry step to use a plain low-limb add-and-carry extraction, which remains the retained optimization in the latest benchmarked state after the latest path benchmark run.

### Added

- Added `proven_parse_f64_ascii()` and `proven_strtod()` as public float-parse entry points over the shared decimal-to-binary64 backend.
- Added internal float-parse path counters so tests can distinguish Clinger hits, staged Eisel-Lemire hits, and exact bigint fallback hits.
- Added `THIRD_PARTY_NOTICES.md` to record the clean-room status of the decimal-to-binary64 parser rewrite.
- Added `scripts/generate_float_decimal_tables.py` and a generated cached-`5^q` header so the current fast path no longer depends on hand-maintained power tables.
- Added an opt-in `bench-float` build-driver command plus a dated `docs/benchmarks/2026-06-13-float-parse-benchmark.md` report comparing `proven_parse_double_ascii`, `proven_strtod`, and host `strtod` on a representative decimal corpus.
- Added a dated `docs/benchmarks/2026-06-13-float-parse-path-matrix.md` guide that breaks the float parse workload into Clinger, staged cached-power, exact fallback, wrapper, and host-reference paths.
- Added a dated `docs/benchmarks/2026-06-13-float-parse-path-benchmark.md` report that splits the float parse workload into short-exact, staged-scientific, fallback, and boundary-tie corpora.
- Added a timestamped `docs/benchmarks/2026-06-12-194411-float-parse-path-benchmark.md` report capturing a fresh path benchmark run against host `strtod`.
- Added a timestamped `docs/benchmarks/2026-06-12-192443-float-parse-path-benchmark.md` report capturing the updated path benchmark after the fast-path significand handling fix.
- Adjusted fast-path significand preparation so the staged Eisel-Lemire validation can keep its representative scientific inputs on the staged path without regressing the Clinger-only case.
- Reduced exact-fallback comparison cost by caching the shared `5^q` state across fallback midpoint checks, which cuts the fallback-heavy and boundary-tie benchmark groups materially without changing the public parser API.
- Deferred exact-bigint construction until the parser actually falls back, and switched staged Eisel-Lemire validation to the lightweight mantissa/exponent representation, which pulled the `staged_scientific` benchmark back into the sub-microsecond band.
- Reused generated cached `5^q` tables when preparing exact-compare state, which removed a large repeated-multiply cost from the remaining exact and staged validation paths.
- Reverted a pow5-state-hoist experiment after benchmark runs showed slower `boundary_tie` corpora, restoring the prior wrapper-owned validation state path.
- Collapsed the decimal metadata build into a single input scan instead of scanning the same token twice before fallback or staged validation.
- Replaced the cached-factor bigint multiply loop with a single schoolbook pass, which cut the remaining fallback and boundary-tie exact-compare cost substantially without changing parse results.
- Built adjacent-float midpoints directly from raw mantissa/exponent words instead of through two bigint shifts and an add, which trimmed the remaining boundary-tie validation cost.
- Specialized the common 1-limb cached-factor multiply case, which shaved more time off the remaining fallback-heavy and boundary-tie paths.
- Fixed exact-compare state prep so negative exponents cache the reciprocal pow5 factor again instead of dropping to the legacy multiply loop, which restored the intended cached-factor path in the fallback-heavy cases.
- Fixed `proven_float_bigint_copy_mul_factor()` so the 1-limb fast path copies the source into the working product before multiplying, keeping the negative-exponent exact path correct.
- Added a fused 1-limb copy-multiply loop so the negative-exponent exact path no longer does a separate copy pass before multiplying small cached factors.
- Simplified the fused 1-limb copy-multiply loop to a single carry scan, then benchmarked a direct 128-bit variant and reverted it after it regressed `fallback` and `boundary_tie`; the single carry scan remains the retained baseline.
- Verified the retained fused 1-limb copy-multiply loop on the path benchmark: `short_exact` and `staged_scientific` improved again, while the slower groups stayed at the retained baseline after the reverted experiment.
- Specialized the negative-exponent exact compare path for 1-limb rhs operands, which lowered `fallback` and `boundary_tie` again while keeping `short_exact` and `staged_scientific` near the same band.
- Re-ran the path benchmark on that compare-site specialization and kept the improvement: `fallback` and `boundary_tie` stayed materially faster while the short/staged corpora stayed close to the prior run.
- Applied the same 1-limb rhs specialization to staged validation, which nudged `staged_scientific` down while keeping the fallback-heavy corpora improved.
- Folded the 1-limb rhs multiply into a shared helper used by both the staged and exact negative-exponent compare sites, which kept the benchmark gains while reducing duplicated path logic.
- Added a 2-limb copy-multiply specialization for the negative-exponent exact compare path, which trimmed the fallback-heavy and boundary-tie benchmark groups again.
- Added a tiny-limb cached-factor multiply path for the remaining exact-comparison cases, which kept the fallback path moving while preserving corpus agreement.
- Avoided full 160-limb bigint zeroing when preparing hot compare operands, which materially reduced the cost of the remaining exact fallback and boundary-tie paths.
- Narrowed bigint copies to the live limb prefix instead of copying the full 160-limb backing store, which cut more memory traffic from the remaining exact compare path.
- Removed the last full-buffer zeroing from `proven_float_bigint_mul_factor()` by clearing only the live output prefix, which kept the remaining exact compare work moving down.
- Switched cached pow5 setup and the hot exact-compare bigint constructors to skip unnecessary full-buffer zeroing, which simplified the preparation path without changing parse results.
- Removed the extra bigint copy from the exact compare shift path so only the shifted operand is materialized in the common fallback and boundary-tie validation cases.
- Specialized hot bigint copies so 1- and 2-limb operands use direct assignments instead of a generic limb loop, which pulled the exact fallback and boundary-tie paths down again.
- Kept the hot bigint copy fast path specialized through four limbs and collapsed copy-plus-factor multiplication into one helper, which shaved more time off the exact compare-heavy benchmark groups.
- Re-ran the path benchmark after the copy-plus-factor helper change and confirmed the exact-compare-heavy corpora still match host `strtod`; a later small-rhs shortcut was reverted after it regressed `fallback` and `boundary_tie`, restoring the better numbers.
- Split the exact shifted-compare loop to handle the lowest shifted limb separately from the main walk, which shaved another small amount of work off the fallback-heavy corpora without changing results.
- Turned the remaining lower zero-prefix check in the exact shifted-compare loop into a branchless accumulation, which cut a little more overhead out of the fallback-heavy corpora.
- Removed an unnecessary full-biginteger zeroing from decimal token setup, which cut a large amount of common-path overhead out of the short-exact, staged, fallback, and boundary corpora.
- Delayed the legacy exact-compare RHS copy until the compare actually needs it, which removed another avoidable copy from the positive-exponent compare cases.
- Replaced the bigint limb-shift loop with `memmove`/`memset` in the exact compare path, which shaved more time off the fallback-heavy and boundary-tie corpora.
- Unrolled the common 1- and 2-limb shift case in the bigint exact-compare path, which cut another slice of overhead out of the fallback-heavy and boundary-tie corpora.
- Compared shifted bigints directly without materializing a shifted copy in the exact compare path, which removed another avoidable pass from the fallback-heavy and boundary-tie corpora.
- Rewrote the direct shift-compare loop to compute shifted limbs inline instead of routing through a per-limb helper, which shaved more time off `fallback` while keeping checksum agreement intact.
- Split the shifted-biginteger compare loop into separate shift and no-shift regions, which cut a little more branch noise out of the fallback-heavy path without changing results.
- Reworked `proven_strtod()` to reuse the parser's nonzero-digit metadata instead of rescanning the parsed token for underflow bookkeeping, which trimmed wrapper overhead without changing parse results.
- Switched the hosted `proven_strtod()` input-length scan to `strlen()`, which shaved a little more off the wrapper path in the hosted benchmark run.
- Reused the parser's sign flag for overflow fallback selection instead of re-reading the first token byte, which cleaned up the rare overflow path without changing parse results.
- Split the exact compare loop's top shifted limb into a separate check before the main walk, which removed a small amount of work from the fallback-heavy corpora without changing rounding.
- Removed unused exponent accumulation from token validation, which drops one redundant scan over exponent digits before the parser reaches the exact backend.
- Removed a dead zero-case branch from the decimal builder, which keeps the zero path simple without changing parser behavior.
- Skipped full 160-limb zeroing when rebuilding the exact significand, which cuts unnecessary memory traffic from the fallback path.
- Restored the safe direct shifted-compare helper after a copy-eliding simplification caused a `1e100` benchmark mismatch, keeping the benchmark corpus aligned with host `strtod`.
- Switched bigint zeroing and prefix clearing to `memset` and skipped unnecessary full-buffer zeroing in bigint constructors, which kept the fallback-heavy exact path moving without changing the parser API.
- Reverted a regressing midpoint-fast-path experiment in the exact compare helper after benchmark runs showed slower `fallback` and `boundary_tie` corpora.
- Restored the small-compare helper's cached-power overflow checks after the midpoint experiment, which improved the fallback-heavy and boundary-tie corpora again while keeping the benchmark corpus checksum-stable.

### Fixed

- `proven_sysio_scanner_deinit()` now clears the full scanner state after releasing the buffer.
- `proven_map_is_valid()` now checks the public `internal.size` against the bucket layout.
- `proven_fs_open()` now rejects unsupported mode bits before reaching the PAL layer.
- `proven_fs_open()` now rejects truncation requests that do not carry write intent.
- `proven_fs_lock()` now rejects unsupported lock modes instead of treating them as unlock.
- `proven_fs_chmod()` now rejects unsupported permission bits before reaching the PAL layer.
- `proven_float_format_f64_policy()` now rejects out-of-range fixed-mode precision values with `INVALID_ARG`.
- `proven_float_format_f64_policy()` now keeps tiny finite subnormals on the shortest formatting path instead of falling back to `UNSUPPORTED`.
- `proven_scan_f64()` now routes decimal parsing through a shared exact backend that tokenizes ASCII input once, compares candidate midpoints with bigint arithmetic, and preserves correct rounding at normal, subnormal, and overflow boundaries without host `strtod`.
- The staged `proven_float_try_eisel_lemire()` layer now handles a conservative exact negative-exponent subset in addition to the generated-`5^q` positive-exponent subset.
- The staged `proven_float_try_eisel_lemire()` layer now also uses `__uint128_t` to round a bounded negative-exponent ratio subset for normal-range candidates.
- The staged `proven_float_try_eisel_lemire()` layer now accepts wide-shift normal-range negative exponent ratios such as `1e-27` instead of forcing them into the bigint fallback.
- The staged `proven_float_try_eisel_lemire()` layer now validates candidate bits against exact midpoint comparisons before accepting them, which lets tie-to-even zero-exponent integers such as `9007199254740993` stay on the staged fast path without reintroducing one-ULP regressions.
- The staged `proven_float_try_eisel_lemire()` layer now uses a generated `u128` power-of-5 table for wider exact positive-exponent subsets such as `1e40` before falling back to the bigint path.
- The staged `proven_float_try_eisel_lemire()` layer now also uses the generated `u128` power-of-5 table for wider negative-exponent ratio subsets such as `1e-30` before falling back to the bigint path.
- The staged `proven_float_try_eisel_lemire()` layer now lets the `u256` negative-ratio scaler keep deeper wide-shift cases such as `1e-40` on the staged fast path before exact fallback.
- The staged `proven_float_try_eisel_lemire()` layer now uses a generated reciprocal `5^-q` cache for wider negative-exponent candidates such as `1e-100` before falling back to the exact bigint path.
- The staged `proven_float_try_eisel_lemire()` layer now uses a generated scaled `5^q` cache for wider positive-exponent candidates such as `1e100` before falling back to the exact bigint path.
- The widened staged `proven_float_try_eisel_lemire()` paths now share common cached-power candidate finalization so positive, negative, and subnormal candidates all pass through the same exact midpoint validator.
- The widened staged `proven_float_try_eisel_lemire()` paths now also share one cached-power `u64 x u128 -> u256` product helper for both positive scaled-`5^q` and negative reciprocal-`5^-q` candidate assembly.
- The widened staged `proven_float_try_eisel_lemire()` paths now round wide products into a shared `53-bit significand + unbiased exponent` packing step, which keeps representative subnormals such as `5e-324` on the staged fast path while leaving below-half true-min cases on exact fallback.
- The staged `proven_float_try_eisel_lemire()` entry logic now routes positive exponents through one generated power-of-5 product path and negative exponents through one bounded denominator-or-reciprocal normalization path instead of keeping separate exact-cancellation branches.
- Negative exponents now try the generated reciprocal cached-power candidate path first across the whole staged band, using the older small-`q` denominator normalization only when the reciprocal candidate remains uncertain.
- The staged `proven_float_try_eisel_lemire()` layer now builds explicit candidate plans for positive products, negative reciprocals, and denominator fallbacks, then executes them through one shared plan-dispatch seam.
- The staged `proven_float_try_eisel_lemire()` layer now feeds both positive `5^q` products and negative reciprocal `5^-q` candidates through one signed cached-power product-plan builder before considering the narrow negative denominator fallback.
- The staged `proven_float_try_eisel_lemire()` layer now drops the separate negative denominator-normalization family; uncertain negative cached-power candidates defer directly to the exact bigint fallback.
- Internal staged-path metrics now expose shared cached-power product-plan hits so tests can verify the staged representative corpus stays on that single success family.
- Added an explicit `rfc-0001` audit corpus that pins the public parser against the named RFC cases for basics, specials, 2^53 ties-even boundaries, true-min midpoint below/exact/above, very long significands, huge exponents, and malformed/endptr behavior.
- `proven_scan_f64()` now uses an exact cached-`5^q` positive-exponent fast path ahead of the bigint fallback for a broader range of large finite decimal integers.
- `proven_u8str_view_slice()` now allows empty slices at the end of a view.
- `proven_float_format_*_policy()` now rejects invalid policy enums before mode-specific dispatch.
- `proven_sysio_scan_chunk_impl()` now accepts exact 4096-byte chunk fits instead of treating them as truncation.
- `proven_u8str_fmt_internal()` now rejects unknown argument types instead of silently dropping them.

### Changed

- Tightened repository documentation rules and clarified how local-only notes
  differ from public release notes.

## [2026-06-02]

### Changed

- Removed local-only project state files from public Git history and restored
  them locally as ignored files.
- Updated local handoff documents so the next session resumes from the
  post-float-backend state.

### Fixed

- Added `.gitignore` entries for the restored local-only project files.

## [2026-06-01]

### Changed

- Closed the staged float backend work.
- Extended float boundary and shortest-literal coverage.

### Fixed

- Corrected the float scan underflow edge.
- Fixed scientific exponent padding and shortest-float candidate selection.
