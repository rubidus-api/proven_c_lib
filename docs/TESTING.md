# How proven is tested

This document is the testing contract: how tests are named, what each kind of
test is *for*, the rules a new test has to satisfy, and an honest account of how
this project actually develops — which is not quite what it would like to claim.

`TEST.md` is the catalog (what every test checks). This is the policy (why, and
how to add one).

---

## 1. Naming: the filename is the identifier

```text
tests/test_<class>_<subject>.c
```

That is the whole scheme. There are **no numbers**.

Numbers were tried and they rotted. The catalog ran `1..50` with `7a`, `30a`,
`30b`, `30c`, `40a` wedged in wherever something new arrived, and by the time
anyone looked, five numbered entries described files that had been deleted months
earlier. A number tells you when a test was written, which is the one fact nobody
needs. Meanwhile the tests themselves were called `test_phase1` … `test_phase22` —
the *development order*, which is an even purer form of the same mistake.

So the name carries what a reader needs: **what kind of question this test
answers, and about what.**

## 2. The classes

| Class | The question it answers |
|---|---|
| `unit` | Does this module do what it says, used the way a caller uses it? |
| `contract` | Does it *refuse* what it says it refuses? Misuse, corrupted structs, exhausted allocators, out-of-range input. |
| `regression` | Does a defect that actually shipped stay fixed? |
| `differential` | Does it agree with an independent oracle — the host libc, or a known-good corpus? |
| `portability` | Does it still compile, link, and keep its platform branches intact where we cannot run it? |
| `stress` | Does it survive concurrency, under a sanitizer, for long enough that a race is likely rather than theoretical? |
| `docs` | Are the claims the documentation makes still true? |
| `bench` | How fast is it? (Not a correctness gate — but a checksum drift inside one is.) |

Two of these are worth dwelling on, because they are the ones that catch what the
others structurally cannot.

**`contract` is not "unit tests for edge cases."** A unit test asks what the code
does; a contract test asks what it *promises*. The happy path is the half a caller
can infer by reading the code. The refusals are the half they cannot — and the
half that turns into a security bug when it is wrong. `test_contract_public_structs`
corrupts a public struct on purpose. `test_contract_pool_misuse` frees the same
block twice. These tests exist to make the library's "no" as reliable as its "yes."

**`differential` exists because a test you wrote can share your mistake.** If you
believe `0.1 + 0.2` prints a certain way, you will write that belief into both the
code and the test, and both will agree, and both will be wrong. An oracle you did
not write cannot make your mistake. The float engine is checked against the host
libc across an exhaustive sweep of every finite `binary32` and billions of random
`binary64` values, and that is why a defect in the shortest formatter was found at
all — no hand-written expectation would have gone looking there.

## 3. Rules a new test must satisfy

**Every test declares its intent and its failure hint, and both are for the person
who did not write it.** `PROVEN_TEST_SUITE(name, intent, hint)` and
`PROVEN_TEST_SECTION(name, intent, hint)`. The hint should name the file and the
idea to inspect — "check the borrowed-key range guard in map insertion", not
"review the logic". A failing test whose hint says "review the logic surrounding
`x == y`" has told the reader nothing they did not already have.

**A regression test must fail against the unfixed source.** This is not a
suggestion; it is what makes it a regression test. A test that passes before the
fix is testing something else, and will keep passing after the bug comes back.
Every regression test in this repository was checked this way — the pre-fix source
was reconstructed with `git show HEAD:<file>`, the test was compiled against it,
and it was watched to fail — and the check is cheap enough that there is no excuse
for skipping it.

Two of them are worth studying as examples of *what to assert*:

- `test_regression_scanner_rollback`'s first draft **passed against the buggy
  code.** It asserted that a failed scan fails again on retry — true both before
  and after the fix. The corruption was in *which bytes the scanner believed it
  still had*, so the test had to assert that, byte for byte. Assert the thing that
  broke, not a consequence you hope is downstream of it.
- `test_regression_sort_duplicates` counts **comparisons**, not wall-clock time.
  The defect was quadratic behaviour; the observable was 10.6 seconds. But a
  timing threshold is a flaky test on a shared machine, while the comparison count
  is exactly the quantity that exploded. Assert the mechanism, not the symptom.

**Assert the contract, not the implementation.** `test_unit_array` used to assert
that growing an array *changes* `arr.data` — a fact about the allocator, never a
promise of the array. When `realloc` learned to grow in place, a correct
optimisation broke a passing test. The contract was "capacity grew and the
elements survived"; that is what it asserts now.

**Sanitizers are part of the definition of passing.** `asan`, `ubsan`, and `tsan`
run the whole suite. A test that passes only without them has not passed.

## 4. What the build checks about the documentation

Documentation claims are checked mechanically, because the alternative was tried
and it failed twice in the same way:

- The alias layer claimed to cover the public API. Nobody checked. **25 of 203
  public functions had no alias**, and 22 of them had been missing for months.
- The manual claimed to teach the API by example. Of its ~190 code blocks,
  **four** could be compiled at all. The rest referenced imaginary helpers, so
  they could not rot loudly — only quietly.

Both are now enforced by tests that fail the build:

- `test_docs_alias_completeness` parses the public headers and the alias header
  and names any public function without an alias.
- `test_docs_manual_examples` requires every example the manual prints to be a
  real program under `manual/examples/` — compiled and *run* by the build driver —
  and requires the chapter and the file to agree verbatim.

The principle generalises, and it is the one lesson this project keeps relearning:
**a claim with no mechanism behind it stops being true, and nobody notices.** If
the documentation asserts something, find a way for the build to assert it too.

## 5. How we work: test-first for new public API, adversarial audit as standing practice

Two decisions, taken deliberately on 2026-07-13 and recorded here rather than left as
aspirations. They close `docs/BACKLOG.md` **B-003** and **B-011**.

### 5.1 New public API is written test-first, in separate commits

**The rule.** A new public function, type, or contract lands in *at least two commits*:

1. **The contract, and a test that fails.** The header (with the documentation that says
   what it promises), a stub that compiles and does nothing useful, and the test written
   from that contract — which must FAIL, and be seen to fail, before anything else happens.
2. **The implementation.** The same test, now passing, with no change to what it asserts.

If the second commit had to weaken the first commit's assertions, the contract was wrong,
and *that* is the finding — record it in the commit message rather than quietly editing the
test to agree with the code.

**Why.** Features used to be written first and tested afterwards, and it cost us, every
time. The whole-file API is the clean case study: implemented, tested, documented, released
— and then an adversarial audit found four defects in it. The buffer doubled for *every*
regular file (peak memory 3× the file size). The string variant started from a **one-byte**
buffer for pipes and `/proc`. The atomic write **widened permissions**, republishing a
`0600` key file as `0644`. It failed outright on a legal 250-character filename.

Those are not exotic bugs. They are the questions you ask while *designing* a contract —
*what does this cost? what happens when the size is unknown? what happens to the mode? how
long can a name be?* — and precisely the questions you do not think to ask while confirming
code you already believe in. Worse, the header *claimed* "one allocation and one pass" in
the same commit that made it false, and the suite was green: nothing in the process was
positioned to notice, because nothing had been asked to.

**What this does not change.** Defect fixes were already test-first in method — reproduce,
write the test, *watch it fail against the unfixed source*, fix, watch it pass — and they
stay that way. A regression test that passes before the fix is not a regression test. The
only thing that changes is that the discipline is now visible in the history for features
too, rather than being a thing the next person has to be told about.

**The first feature under the rule** is `proven_fs_walk` (recursive directory iteration with
cycle protection). Its contract and failing test landed in one commit and its implementation
in the next; `git log` shows both.

### 5.2 An adversarial audit is a standing part of the process

**The rule.** An adversarial audit — an independent reader whose brief is to *break* the
thing, with a **reproducer required before any finding may be reported** — runs on:

- every **new module or new public API**, after it is implemented and before it is called
  done;
- every **module that has never had one** (this is now an empty set — but a new one can
  appear the moment a module is added);
- the **fixes an audit itself produced**, because that is where the next bugs are (see
  below);
- and before a release.

Findings are reproduced, fixed, and pinned by a regression test verified to fail against the
unfixed source. A finding without a reproducer is a hypothesis, not a finding.

**Why: the evidence.** Turned on the modules that had never had it, the audit found a defect
of consequence in **every one of them**:

- the "correctly rounded" float parser was not correctly rounded — 2,923 values misrounded
  against glibc, every one an exact halfway value, every one returning `PROVEN_OK`;
- the buffered scanner could not read a **pipe**, which is the one thing it exists for;
- a map with churn grew **without bound** — 100 live entries, 33 MB;
- `close()` failures were discarded, so a write the filesystem had *refused* was reported as
  `PROVEN_OK` and an atomic rename published it anyway;
- `append_grow` of an empty view left a fresh heap block unterminated;
- the sort handed the caller's comparator a pointer of alignment 1;
- a writer that had failed reported **success** on the next flush.

Not one of those was found by the test suite, and not one of them would have been. Every one
was correct for the input the tests used and silently wrong for the input the code will
actually meet: a pipe rather than a file, a twenty-digit number rather than a short one, a
churn rather than a run, a full disk rather than an empty one.

**And audit the fixes.** The audit pointed at the code a previous audit had just produced
found three regressions in it — including a rollback that wrapped `cursor` to ~2^64 and read
`buffer[SIZE_MAX-15]` (ASan: heap-buffer-overflow). New code is where new bugs are, and code
written in a hurry to fix a bug is the newest code there is. The rule stops when a round comes
back clean.

## 6. Adding a test

1. Pick the class from §2. The class is the first question: *what kind of question
   am I answering?* If the answer is "I don't know", the test is probably not
   worth writing yet.
2. Name it `test_<class>_<subject>.c`.
3. Give it a real intent and a real failure hint — write them for whoever gets
   paged, not for yourself.
4. If it is a `regression`, **verify it fails against the unfixed source before
   you commit it.** `git show HEAD:src/proven/<file>.c` into a scratch directory
   and build the test against that.
5. Register it in `nob.c` — in `all_tests`, and in `regression_tests` too if it is
   fast and guards a shipped defect.
6. Add it to `TEST.md` under its class.
7. Run `./nob strict-error`, `./nob asan`, `./nob ubsan`. Green in all three, or
   it is not green.
