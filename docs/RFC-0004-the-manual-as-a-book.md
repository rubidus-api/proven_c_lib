# RFC-0004 — The manual is a reference; it needs to become a book

**Status:** implemented (first pass); follow-up hardening is tracked in RFC-0005
**Date:** 2026-07-20
**Implemented:** 2026-07-20
**Tracks:** `docs/BACKLOG.md` B-025 ... B-032 (first pass complete)

> **What this changed.** At proposal time the manual was 8,004 lines and factually checked by
> several build gates. It was also written for someone who already knew what an arena, a trait, a
> view, and failure atomicity were, and it opened Chapter 1 with a table of type aliases. This RFC
> specified the rewrite that makes it usable by a reader who has finished one introductory C book: every
> section leads with *why the thing exists and what goes wrong without it*, the chapters declare
> a reading order instead of a header layout, and the on-ramp that does not exist today gets
> written.
>
> The implementation made no library code changes. It was staged so the build stayed green at
> every commit.
>
> The first pass is complete. Section 8.1 records the measured result; RFC-0005 records the
> post-implementation audit of the gates and the wider library.

---

## 1. The finding at proposal time

The manual is two documents interleaved, and the split is chronological rather than topical.

Everything written to satisfy `tests/test_docs_manual_depth.c` — Chapter 4 §6–§7, Chapter 5's
five unnumbered sections, Chapter 2 §7 — opens with the reader's *problem*: "There is no single
'hash'. Which function is correct depends entirely on what you are doing with the result, and
reaching for the wrong one gives you either a program that is needlessly slow or one that is
quietly insecure." Then a use-case table, then what goes wrong, then the reference, then a
counter-example, then a compiled program.

Everything written before that gate existed opens with the header. Chapter 1 §1 is a table of
fixed-width aliases. Chapter 2 §1 is three function-pointer typedefs. Chapter 3 §1 is a struct
dump under a one-sentence definition. None of them says what problem the thing solves, what C or
libc does badly here, or why the library made the choice it made.

The style the rewrite needed was therefore **already in the repository, already gated, and already
proven** - on 7 sections out of 88. The job was not to invent a voice. It was to extend one, and to
fix the three structural problems the extension will not touch on its own:

1. **There is no on-ramp.** No "hello world", no statement of what the library is for, no
   glossary. The first runnable program appears at `manual/manual-01-foundation.md:126` — 27% of
   the way into Chapter 1, after 125 lines of type tables.
2. **The chapter order is the header dependency graph, not a learning path.** It is close to a
   good order by accident, but nothing says so, no chapter states its prerequisites, and the
   generated 416-row alias index sits between two prose chapters as if it were reading material.
3. **The hardest material in the book is in its second chapter.** Chapter 2 §7 covers pointer
   provenance in the C abstract machine, CAS, the ABA problem, hazard pointers and epoch
   reclamation — 130 lines that arrive after six sections of ordinary allocator use and that
   nothing later depends on.

---

## 2. What that cost, measured at proposal time

The repository already owns a standard for how much explanation a section must carry:
`test_docs_manual_depth.c` requires **150 words of prose** — words outside tables, headings and
code fences — plus a reference table and a counter-example. It applies that standard to seven
sections.

Applying the same measurement to all 88 H2 sections in `manual/` at the proposal baseline:

| | sections | share |
|---|---:|---:|
| Would pass the repo's own 150-word prose standard | 30 | 34% |
| Would **fail** it | **58** | **66%** |

Per chapter, and the first row is the one that matters most, because it is where a new reader
starts:

| Chapter | passes / H2 sections |
|---|---|
| `manual-01-foundation.md` | **0 / 9** |
| `manual-02-allocation.md` | 3 / 8 |
| `manual-03-strings-text.md` | 2 / 6 |
| `manual-04-containers-algorithms.md` | 4 / 9 |
| `manual-05-hosted-services.md` | 7 / 11 |
| `manual-06-execution-and-platform.md` | 2 / 8 |
| `manual-08-fmt-scan.md` | 9 / 14 |
| `manual-freestanding.md` | 0 / 12 |
| `manual.md` | 3 / 9 |

Chapter 1 — the entry point — passes none of its nine sections. The thinnest sections in the book
carry almost no prose at all: `## 3. Memory mapping` has **1 word**, `## 5. Alignment helpers` has
**2**, `## 3. Ring buffer` has **15**, `## 2. Intrusive list` has **18**.

Section openers, counted mechanically across all 270 H2/H3 headings:

| The section opens with | count | share |
|---|---:|---:|
| Prose | 180 | 67% |
| A code fence — struct or typedef dump | 52 | 19% |
| A reference table | 38 | 14% |

Two-thirds opening in prose looks healthy and is not: most of those openers are a single
definitional sentence before the tables start. *"An arena allocates linearly from
caller-provided backing storage. Individual frees are no-ops."* is prose, and it is not
motivation — it says what the thing does, never why you would want it or what happens if you use
`malloc` instead. The 150-word measurement above is the honest one, because it cannot be
satisfied by a definition.

### The vocabulary problem, counted

Load-bearing terms, none defined at first use:

| term | occurrences in `manual/` | first use |
|---|---:|---|
| `view` | 288 | Chapter 3 §1, in the defining sentence |
| `arena` | 115 | `manual.md` §4 table |
| `trait` | 29 | `manual.md:181`, in a table cell |
| `provenance` | 26 | `manual.md:36`, in a bullet; defined 700 lines later |
| `PAL` | 18 | `manual.md:37`, unexpanded |
| `failure atomicity` | 7 | `manual.md:35`, in a bullet |

"Trait" is not a C word. `[[nodiscard]]` appears in Chapter 1 as a load-bearing part of an
explanation, unexplained. There is no glossary.

---

## 3. Who this is written for

The rewrite needs a stated reader, because "friendlier" is not a specification and cannot be
reviewed.

**The reader has finished one introductory C book.** They know: variables, `if`/`while`/`for`,
functions, arrays, `struct`, pointers and `*`/`&`, `malloc`/`free`, `printf`, `char *` strings and
`strlen`, and compiling with `gcc main.c -o main`.

**They have not met:** ownership as a discipline, borrowed versus owned data, arenas or pools,
function-pointer vtables as an interface, C23 attributes, undefined behaviour as a thing the
optimizer exploits, alignment beyond "it just works", atomics, or the idea that a library might
refuse an operation rather than truncate it.

**Two consequences for every page.** A term from the second list is either defined where it is
first used or linked to the glossary — no exceptions. And an explanation may assume the first
list freely: this is not a C tutorial, and re-teaching pointers would insult the reader and
bloat the book.

---

## 4. The four rules the rewrite follows

**1. Motivation before mechanics, always.** Every section answers four questions in order, before
any struct appears:

- *What job are you trying to do?*
- *What does plain C or libc give you for that job, and how does it fail?* — concretely, with the
  failure named: `strcpy` has no idea how big the destination is; `malloc` returns `NULL` and
  nothing makes you look; `printf("%d", 3.0)` compiles; `errno` is a global you must remember to
  clear.
- *What does this library do instead, and what did it trade away?*
- *Then*: the types, the functions, the table.

**2. Easy before hard, and prerequisites stated.** Each chapter opens with what you should have
read first and what you will be able to do after. Advanced material moves out of beginner
chapters rather than sitting at the end of them.

**3. Every section earns its keep three times.** Prose that explains *why* (the 150-word floor),
a reference table (so it stays usable as a reference), and a counter-example — real code that
gets it wrong, with the failure named. The counter-example requirement is not decoration: it is
the fastest way to teach a contract, and the repo's existing gate already requires it.

**4. Nothing in the manual is unverified.** A sentence a reader could act on — a value, a
boundary, a refusal, a guarantee — becomes an assertion in `tests/test_docs_manual_claims.c`. A
program shown becomes a file in `manual/examples/` that the build compiles and runs. This rule
already exists; the rewrite must not outrun it by adding prose faster than it adds checks.

---

## 5. Structure: what changes, and what deliberately does not

### 5.1 The chapter order is nearly right already — the map is what is missing

The obvious move is to renumber every chapter into a teaching order. Checked against the
dependency reality, that turns out to be mostly churn:

```text
current:  01 foundation → 02 allocation → 03 strings → 04 containers
          → 05 hosted → 06 execution+platform → 07 alias index → 08 fmt/scan
```

Strings need allocators, containers need allocators, hosted services need strings. So
`01 → 02 → 03 → 04 → 05` is already a correct teaching sequence. Only two files sit wrong: the
**alias index** (a generated 416-row table, reading material for nobody) is Chapter 7, and the
**formatting deep-dive** is Chapter 8, after it. `manual-freestanding.md` is outside the numbering
entirely.

**Decision: do not renumber.** Renaming files breaks nine hardcoded paths in
`test_docs_manual_examples.c`, seven exact heading strings in `test_docs_manual_depth.c`, every
cross-reference in ten Korean mirror files, and any link anyone has saved — in exchange for
cosmetic ordering that a table of contents already solves. Instead:

- **Parts are introduced as the organising principle**, declared in `manual.md`, and each chapter
  is labelled with its part and its prerequisites.
- **Chapter 7 is relabelled Appendix A** in its title and in the reading order. The filename stays.
- **Chapter 8 is declared the reference half of the text material**, with Chapter 3 as the
  tutorial half, and each says so at the top. Today they overlap and neither states the division —
  `manual.md`'s header map assigns `fmt.h` to Chapter 3 while Chapter 8 calls itself the deep dive.

The declared reading order:

| Part | Chapters | You can do this after |
|---|---|---|
| **I — Start here** | 0 (new) | Build a program against the library and read any later chapter |
| **II — The vocabulary every program uses** | 1, 2, 3 | Handle errors, own memory deliberately, hold text safely |
| **III — Data structures** | 4 | Arrays, maps, lists, rings, sorting, hashing, encoding |
| **IV — Text in and out** | 3 §3–4 (tutorial), 8 (reference) | Format and parse without `printf`/`scanf` |
| **V — Talking to the operating system** | 5 | Files, streams, standard I/O, time, randomness, mapping |
| **VI — Going further** | 6, freestanding | Coroutines, jobs, thread-safety, bare metal, cross builds |
| **Appendices** | A (alias index), B (glossary), C (header map), D (libc → proven) | Look things up |

### 5.2 Chapter 0 is new, and it is the whole point

`manual/manual-00-start-here.md`. It does not exist and nothing else in the plan works without it:

- **Why this library exists**, argued from five concrete C failures a beginner has already met or
  will meet — a `strcpy` overflow, an unchecked `malloc`, a `printf` format mismatch that
  compiles, `errno` clobbered by an intervening call, a `qsort` comparator with the wrong
  signature — and what this library does about each.
- **Hello world**: the shortest complete program, compiled and run by the build like every other
  example.
- **How to build and include**, in one place.
- **The five contracts you will meet on every page** — results instead of `errno`, borrowed views,
  the allocator parameter, caller-owned state that must not be copied, and *refuse rather than
  truncate* — each with one sentence and one counter-example.
- **A glossary** (Appendix B, linked from every first use): view, owned, borrowed, arena, pool,
  trait, PAL, freestanding, failure atomicity, provenance, `[[nodiscard]]`, UB, CSPRNG.
- **A libc → proven map** (Appendix D): `malloc`→, `strcpy`→, `strtok`→, `printf`→, `sprintf`→,
  `fopen`→, `qsort`→, `rand`→, `strtod`→. This is the table an experienced C programmer reads
  first and the manual does not have.

### 5.3 Chapter 2 §7 moves

Pointer provenance, CAS, ABA, hazard pointers and epoch reclamation move from Chapter 2 (Part II,
beginner) to Chapter 6 (Part VI, going further). They are correct and worth keeping; they are in
the wrong book. What stays behind in Chapter 2 is a short, plain statement of what is and is not
thread-safe, with a pointer forward.

### 5.4 Intra-chapter order

Two chapters open on their hardest material and are fixed in place, without renaming anything:

- **Chapter 1** currently opens with a table of fixed-width aliases. It will open with the error
  model — the thing every later line of code touches — and lead with the `ex_01_errors.c` program
  that currently appears at line 126. Type tables become reference material later in the chapter.
- **Chapter 2** currently opens with three function-pointer typedefs. It will open with
  `proven_heap_allocator()` — the case where nothing is unusual and a beginner can act
  immediately — then arena, then pool, and only then the trait that lets you swap them. You should
  have *used* three allocators before being shown the interface they share.

---

## 6. What was genuinely fine - stated plainly

An audit that finds everything broken has not been reading carefully. The rewrite is an
extension of this manual, not a replacement, and most of it needs no change:

- **The facts are checked, and that is rare.** Five direct manual gates, compiled code-block
  checks, and the repository policy check mean the
  manual cannot claim a function that does not exist, quote an example that does not compile, or
  state one of the registered executable claims falsely without failing the build. The
  post-implementation audit in RFC-0005 narrows this statement: a gate proves only the claims it
  actually parses.
- **The counter-example habit is already established** — roughly 150 `Wrong:` blocks across the
  chapters, several of them excellent (`proven_byte_t key[16] = { 0 };  /* wrong: a "secret"
  everyone knows */`). The rewrite spreads this, it does not introduce it.
- **The best sections are genuinely good.** Chapter 4 §6 (hashing by use case), Chapter 5's
  randomness and streams sections, Chapter 2 §7.1–7.5 — these are the target style, written here,
  by this project.
- **Every public symbol is documented somewhere**, enforced in both directions. The floor is a
  table row, which is the complaint of §2 — but the floor exists and holds.
- **Chapter 8 is a serious reference** for a genuinely hard subject, and its 18 executed contract
  claims are the strongest verification in the manual.
- **The 22 examples are real programs**, compiled and run by `./nob build`, quoted verbatim under
  a gate that fails if the chapter and the file disagree.

---

## 7. What constrained the rewrite

The gates are permissive about structure and strict about facts. Precisely:

| Gate | Enforces | What the rewrite must respect |
|---|---|---|
| `test_docs_manual_symbols.c` | Every public function is named in `manual/` inside backticks; every call the manual writes must exist | **Reference tables cannot simply be deleted** in favour of prose unless each symbol is still named somewhere |
| `test_docs_manual_depth.c` | 26 registered sections: >=150 prose words, a table, a counter-example, sometimes structs and an example | Renaming any registered exact heading breaks the build unless the register is updated in the same commit |
| `test_docs_manual_examples.c` | Marker -> file exists -> the next ```c fence equals the file verbatim; no unquoted example files | New chapters must be added to its **hardcoded 10-path list**; `MAX_EXAMPLES` is 64 and 22 are used, so 42 new examples fit |
| `test_docs_manual_claims.c` | Named manual sentences are executed as assertions | New factual sentences need new assertions here |
| `test_docs_manual_ch08_contracts.c` | 18 Chapter 8 scanner claims | Chapter 8 edits must keep these true |
| `scripts/project-check.sh` (since v26.07.20b) | Privacy scan over every tracked file | Example paths must not collide with host paths — see the `/srv` change |

Two consequences worth stating before anyone starts writing: **adding a chapter file means editing
`test_docs_manual_examples.c`**, and **the Korean mirror in `manual-ko/` is not gated by anything**,
so it drifts unless each phase updates it deliberately.

---

## 8. Implementation plan and result

Staged so each phase is independently useful and the build is green at every commit. Nothing here
is a big-bang rewrite of 8,004 lines.

| Phase | Work | Why here | Backlog |
|---|---|---|---|
| 1 | **Chapter 0** — why the library exists, hello world, build/include, the five contracts, and the glossary and libc map as appendices | Everything else can link to it. It is also the phase that proves the new-chapter mechanics: gate register, example, Korean mirror | **B-025** |
| 2 | **The map** — Parts, reading order and per-chapter prerequisites in `manual.md`; Chapter 7 relabelled Appendix A; the Chapter 3 / Chapter 8 division stated in both | Small, and it makes the rest navigable while it is still under construction | **B-026** |
| 3 | **Chapter 1 and Chapter 2 rewrites** — motivation-first, reordered per §5.4; Chapter 2 §7 moves to Chapter 6 | The two chapters a beginner meets first, and the two worst offenders (0/9 and 3/8) | **B-027**, **B-029** |
| 4 | **Chapter 3 rewrite** as the tutorial half of text, with the ownership vocabulary defined where it is first used | Highest jargon density in the book (`view` × 288) | **B-027**, **B-030** |
| 5 | **Chapter 4 and Chapter 5 gap-fill** — motivation-first openers for the sections still below the floor, and the missing examples for `ring.h`, `list.h`, `buffer.h`, `mmap.h`, `time.h`, `algorithm.h`, `align.h`, `u16str.h` | These chapters are half-done already; this finishes them | **B-027**, **B-028** |
| 6 | **Chapter 6 and freestanding** — receives Chapter 2 §7, gets motivation-first openers (0/12 today) | Advanced material, last, and it is where the moved sections land | **B-027** |
| 7 | **Extend the depth gate register** from 7 sections to every section rewritten in phases 3–6 | The style becomes enforced rather than merely applied. Done last per chapter, so the gate never blocks work in progress | **B-031** |
| 8 | **Korean mirror sync** for every chapter touched | Nothing enforces it, so it is a deliberate step rather than a hope | **B-032** |

Each phase ends with `./nob build` green, and each chapter rewrite lands with its examples and its
claims assertions in the same commit — not as follow-up work, because follow-up work is what
produced the 58 sections in §2.

### 8.1 Measured first-pass result

All eight phases completed on the v26.07.20b line. The baseline columns below are the proposal
measurements from sections 1 and 2; the result columns were measured from the implemented tree.

| Measure | Proposal baseline | First-pass result |
|---|---:|---:|
| English manual | 8,004 lines | 10,211 lines |
| Korean mirror | 8,310 lines | 9,939 lines |
| Sections meeting the prose-depth floor | 30/88 (34%) | 67/98 (68%) |
| Sections registered in the depth gate | 7 | 26 |
| Runnable examples | 18 | 22 |
| Broken internal anchors | 11 discovered during implementation | 0 |

The result deliberately does not force every section over a word-count threshold. Generated alias
tables, example-only sections, flag lists, and compile procedures are reference material; padding
them would realize the first risk in section 9. Every chapter now has a motivation-first entry,
parts and prerequisites are explicit, Chapter 0 supplies the on-ramp and glossary, and the Korean
mirror includes the same first-pass structure.

### 8.2 Post-implementation audit

The later whole-library audit found that the rewrite itself landed, but several adjacent gates
were narrower than their prose claimed. In particular, the test catalog originally ignored
duplicate registry entries and did not verify its printed totals, and one documentation gate
imported a POSIX-only directory API despite the supported Windows target. Those gate defects are
fixed in the follow-up work, while the remaining library, portability, and performance findings
are specified in [`RFC-0005`](RFC-0005-whole-library-audit-and-hardening.md).

---

## 9. What could go wrong

- **The manual gets longer without getting clearer.** 150 words of prose is a floor, not a goal,
  and a gate that counts words can be satisfied by padding. The counter-example requirement is the
  real defence: you cannot write a plausible "here is how this breaks" without understanding the
  contract. Reviewers should read the counter-example first.
- **Prose outruns verification.** Rule 4 of §4 exists because adding explanation is cheap and
  adding assertions is not. If a phase cannot state its new claims as assertions, that is evidence
  the prose is too vague, not that the gate is too strict.
- **The Korean mirror falls behind**, since no gate holds it. Phase 8 is scheduled for exactly
  this reason, and it should not be allowed to slide past one release.
- **Chapter 0 becomes a C tutorial.** The reader definition in §3 is the boundary: pointers,
  `malloc` and `printf` may be assumed. If Chapter 0 starts explaining what a pointer is, it has
  lost its subject.
- **`MAX_EXAMPLES` is 64.** Phase 5 alone proposes eight new examples; the budget is comfortable
  but not infinite, and the cap is a build failure rather than a warning.

---

## 10. Deliberately not in this plan

A tutorial series or a cookbook separate from the manual; an HTML or generated-docs pipeline;
API reference generation from header comments; translating the manual into a third language;
restructuring `manual-07`'s generated alias table; and any change to library code.

Each is defensible and none is this RFC's subject. A plan that includes everything is a plan that
finishes nothing.
