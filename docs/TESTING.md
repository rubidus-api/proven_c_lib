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

## 5. Is this TDD? No — and here is the honest version

It would be pleasant to claim test-driven development. The claim would not survive
five minutes with `git log`, so here is what the history actually shows.

**Every commit in this repository that adds a test also changes source code in the
same commit.** There is not one commit where a failing test lands first and the
implementation follows. That is the definition of test-after.

The picture is not uniformly bad, and the parts deserve separating:

**Defect fixes are genuinely test-first in method, if not in commit shape.** The
sequence used throughout is: reproduce the defect with a program that fails →
write the test → *verify it fails against the unfixed source* → fix → watch it
pass. The failing test really does come first; it simply gets squashed into the
same commit as the fix. The discipline is real. Its visibility in the history is
not, and that is worth changing, because a discipline nobody can see is a
discipline the next person will not inherit.

**Features are written first and tested afterwards, and it costs us.** The
whole-file API is the clean case study. It was implemented, tested, documented,
and released — and then an adversarial audit found four defects in it: the buffer
doubled for *every* regular file (peak memory 3× the file size); the string variant
started from a **one-byte** buffer for pipes and `/proc`; the atomic write
**widened permissions**, republishing a `0600` key file as `0644`; and it failed
outright on a legal 250-character filename.

Those are not exotic bugs. They are the questions you ask while *designing* a
contract — *what does this cost? what happens when the size is unknown? what
happens to the mode? how long can a name be?* — and precisely the questions you do
not think to ask while confirming code you already believe in. Tests written from
the contract, before the code, would have surfaced at least the first two, because
you cannot write "this costs one allocation" as a test without noticing that it
costs two.

Worse, the header *claimed* "one allocation and one pass" in the same commit that
made it false. The test suite was green. Nothing in the process was positioned to
notice, because nothing had been asked to.

**What we do instead of TDD, and where it works.** The real engine of quality here
is not test-first; it is **adversarial verification after the fact** — sanitizers
on every mode, differential oracles, exhaustive sweeps, and independent audits
whose brief is to break the thing. It works: it found twelve real defects this
cycle, two of them memory-safety. But it works *late*. It finds bugs in shipped
code instead of preventing them in unwritten code, and every one of them costs a
fix, a test, a changelog entry, and a release.

**The open question is recorded, not resolved.** See `docs/BACKLOG.md` item
**B-003**. The decision to make is whether new *public API* is written test-first —
contract first, failing test second, implementation third, in separate commits so
the discipline is visible. This document will say which, once that is decided,
rather than describing an aspiration as if it were a practice.

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
