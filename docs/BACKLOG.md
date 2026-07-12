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

### B-001 — manual chapter 8 is missing sections 7 through 13

**Status:** open. Found 2026-07-12 while writing the verified examples.

`manual/manual-08-fmt-scan.md` lists thirteen sections in its table of contents
and ends mid-chapter at `## 7. Scanner data model` — a bare heading with no body.
Sections 7-13 (scanner data model, primitive scan APIs, the scan-arg model, the
structural scan grammar, the scan APIs, the error-code guide, and the examples)
do not exist. Roughly half the chapter's promised content is absent, and the half
that is missing is the scanner — the side a reader is most likely to need help
with, because `proven_scan_*` has no libc analogue to fall back on.

**Done looks like:** sections 7-13 written against `include/proven/scan.h` and
`src/proven/scan.c`, each API with its return contract and its failure modes, and
at least one worked example under `manual/examples/` quoted by the chapter (so it
is compiled and run like the rest).

**Why not now:** it is a chapter to write, not a line to fix. The scanner's
`_Generic` arg model and the `NEED_MORE` / partial-consume semantics need to be
explained carefully or the chapter will do more harm than the gap does. Tracked
rather than rushed.

### B-002 — most manual code blocks are still unverifiable sketches

**Status:** open, partially mitigated. Found 2026-07-12.

The manual carries ~190 fenced `c` blocks. Before this cycle **four** of them
could be compiled at all; the rest referenced imaginary helpers (`do_work()`,
`get_view()`) or discarded `[[nodiscard]]` results, so no compiler ever saw them
and nothing could tell you when they went stale. That is the same disease as the
alias layer nobody checked: a claim with no mechanism behind it.

**Mitigated so far:** eleven examples now live in `manual/examples/` as real
programs. The build compiles and runs every one, and `tests/test_docs_manual_examples`
fails if a chapter and its example disagree, if a chapter quotes an example that
does not exist, or if an example exists that no chapter shows.

**Still open:** the remaining inline fragments. Each is one of three things and
should be converted accordingly:

1. an illustration of real API use → fold into an example file and quote it;
2. a signature or struct listing → fence as `text`, not `c` (it is not code a
   reader can run, and pretending otherwise is what let the errors hide);
3. wrong → fix it.

**Done looks like:** every `c` block in `manual/` is either quoted from
`manual/examples/` or compiles, and `tests/test_docs_manual_examples` enforces
that — so the class of defect cannot come back.

**Why not now:** ~180 blocks across eight chapters. Doing it chapter by chapter,
with the mechanism already in place to keep each finished chapter finished, is
the way this stays done. Doing it in one pass is how it gets done badly.

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

### B-000 — the alias layer had drifted (closed v26.07.12c)

25 of 203 public functions had no `xcv_*` alias; 22 had been missing for months.
Closed by adding the aliases and by `tests/test_docs_alias_completeness`, which
parses the headers and fails the build if a public function has no alias. The
lesson is the one B-002 repeats: **a list nobody checks stops being true.**
