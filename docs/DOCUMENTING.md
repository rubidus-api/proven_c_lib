# Keeping the documentation true

Documentation rots in one direction: the code moves and the prose does not. Every doc defect
this project has had was that — a function deleted and still declared, a claim ("`proven` exposes
no fsync") that stopped being true, an API nobody documented for months, a version string that
disagreed with itself in three places.

None of them were caught by reading. They were caught by **comparing the documents to the code**,
mechanically — and once you have done that comparison by hand, the obvious next step is to make
the build do it. So this file has two halves:

1. **The process** — what to do when the code changes. Survey, plan, edit, verify.
2. **The gates** — the propositions the documentation must satisfy, each one enforced by a test
   that fails the build. A requirement nothing enforces is a requirement that will quietly stop
   holding, which is how every defect in the list above got in.

You cannot test-drive prose. But you *can* state, for each thing a section must contain and each
claim it makes, a proposition that is either true or false of the text — and then let the build
decide. That is what the gates are.

---

## 1. The process

Run it whenever a change adds, removes, or alters public API. It is deliberately survey-first:
the temptation is to edit the chapter you are thinking about, and the defects are always in the
chapter you are not.

### Step 1 — Survey. Do not edit yet.

Establish what is actually there, in numbers, before forming an opinion:

```sh
./nob build          # the mechanical checks below run as part of this
wc -l manual/*.md README.md
ls manual/examples/
```

Then ask the three questions the mechanical checks cannot:

- **Is each new module documented at the DEPTH of the chapters around it?** The older chapters
  carry: intent (*why does this exist, and what goes wrong without it*), a reference table, the
  structures the caller holds, cautions, a runnable example, and **counter-examples** — the
  `Wrong:` blocks that teach as much as the prose. A new module with only intent and a table is
  half-documented, and the coverage check will still pass.
- **Which claims are now false?** Prose ages worst. Grep for the ones that are absolute:
  `grep -rniE "no |never |only |cannot |there is no" manual/ README.md` and check each survivor
  against the code.
- **What did the last change actually change?** `git log --oneline <last-doc-release>..HEAD`.

### Step 2 — Write down the gap, then the plan.

Not in your head. A table of *what is missing where*, and then the order you will fix it in,
split into pieces small enough to verify one at a time. The order that has worked:

1. `manual.md` — the spine (ownership matrix, behaviour classes, header map). Everything else
   references it.
2. The module chapters, one module per step.
3. The functions nothing documents (the mechanical check prints them).
4. `README.md` — last, because it is a summary of the manual and cannot be written before it.
5. Version, CHANGELOG, release.

### Step 3 — Edit, one piece per turn, verifying each.

After **every** piece, run `./nob build`. It compiles every ```c block in the manual, so a
correct example that stops compiling stops the build. That is the point: the examples are
checked, and the counter-examples are fenced as ```text precisely because they must *not*
compile.

### Step 4 — Verify.

`./nob build` runs every gate in §2. If you added a module section, register it in
`tests/test_docs_manual_depth.c` so **G8** guards it, and write the assertion for every claim you
made into `tests/test_docs_manual_claims.c` so **G9** does.

Then the human half, which no gate can do: re-read what you wrote as somebody who does not know
the answer. The gates prove the section *has* an intent paragraph and a counter-example; only you
can tell whether the intent explains anything and whether the counter-example is the mistake a
reader would actually make.

---

## 2. The gates

A **gate** is a proposition about the documentation that is either true or false, checked by the
build, and blocking. Not a guideline — a guideline is something that stops holding the first busy
week.

You cannot test-drive prose. But almost everything that has actually gone wrong here was not a
matter of taste: it was a claim that had stopped being true, a symbol that no longer existed, a
number that disagreed with itself, a section that was listed instead of explained. Every one of
those is a proposition. So they are gates.

| # | Gate — the proposition it enforces | Enforced by |
|---|---|---|
| **G1** | Every ```c block in the manual **compiles**. Anything not runnable — a signature listing, a struct listing, a deliberate counter-example — is fenced as ```text. | `check_manual_code_blocks` in `nob.c` |
| **G2** | Every `manual/examples/*.c` **compiles and runs**, and the chapter quotes its body **verbatim**. A chapter and its example cannot drift apart. | `test_docs_manual_examples` |
| **G3** | Every example file is **quoted by some chapter**. An example nobody shows is an example nobody maintains. | `test_docs_manual_examples` |
| **G4** | Every public function is **named in `manual/`**. (The PAL is exempt: it is for porting, not the API a caller programs against.) | `test_docs_manual_symbols` |
| **G5** | The manual **never documents a function that does not exist**. Writing `proven_x(...)` is a promise the reader can link it. | `test_docs_manual_symbols` |
| **G6** | Every public function has an `xcv_` **alias**. | `test_docs_alias_completeness` |
| **G7** | The **version string agrees with itself**: version.h, README (both halves), TEST.md, the manual headings, chapter 1's excerpt, and the CHANGELOG's newest entry. | `test_docs_version_sync` |
| **G8** | Every registered module section is documented to **depth**: real prose (why, not just what), a reference table, the structures the caller declares, a runnable example, and **at least one counter-example**. | `test_docs_manual_depth` |
| **G9** | Every **factual claim** the chapters make is **true** — the CRC check value, the standard digest, the refusal that writes nothing, the line that exactly fills the buffer. | `test_docs_manual_claims`, `test_docs_manual_ch08_contracts` |

### Why these, and not a style guide

Each gate exists because the thing it forbids **already happened**:

- **G5** — `proven_sysio_flush` was deleted and the manual went on declaring it as public API, in
  the present tense, for a release. `proven_pool_free` never existed at all: the real symbol is a
  static `proven_pool_free_trait`, and the manual described it as a callable function for as long
  as the manual has existed. A reader who follows the manual gets a *linker* error, which is the
  moment they stop believing any of it.
- **G4** — `proven_fs_dir_open/_next/_close`, the streaming directory iterator, went undocumented
  for months. It is the answer to `proven_fs_list` reading a 50,000-entry directory into 4.2 MB
  before you see any of it; a reader who never learns it exists reaches for the wrong call.
- **G7** — `version.h` sat five releases behind the CHANGELOG while the README claimed a third
  value that matched neither. That is the number a downstream project pins.
- **G8** — the five modules added in the v26.07.13 line each had an intent paragraph and a table,
  and **not one had a counter-example**. They passed every check that existed and were still
  half-written.
- **G9** — the README asserted "`proven` exposes no fsync" for a month after `proven_fs_sync`
  shipped. Nobody had written down what that sentence was asserting, so nothing could object.

### The rule for adding to G9

**When you write a sentence a reader could act on — a value, a boundary, a refusal, a guarantee —
write the assertion for it in `tests/test_docs_manual_claims.c`.**

If you cannot state the assertion, the sentence is too vague to be in the manual. That is not a
slogan; it is the useful half of the test. "This is fast" has no assertion and should be a
number. "The decoder validates the whole input before writing a byte" has one, and it is the
claim that makes the module worth having over a two-line loop — so it is checked.

### Exemptions

A gate that cannot be argued with is a gate people route around. So G8 lets a section be exempt
from *structures* or *an example* — but the exemption is **declared in the test, in code, with a
reason**, not taken by quietly not having the thing. If the reason does not survive being written
down, it was not a reason.

## 3. The depth checklist (what G8 enforces, and what it cannot)

G8 checks that each of these is *present*. Whether each is any *good* is the part no test can
check — and it is the part that decides whether the manual is worth reading. A section is finished
when a reader who has never seen it can answer all six.

1. **Intent.** Why does this exist? What does a program do *without* it, and what goes wrong?
   (For the hashes: *there is no single "hash", and reaching for the wrong one gives you a
   program that is either needlessly slow or quietly insecure.*)
2. **The choice.** If the module offers several things that look alike, a table that makes the
   choice for the reader once they name their job. This is what stops the wrong one being picked.
3. **The structures.** Every struct the caller declares, with the role of each field — and, for
   caller-owned state, the copy rule.
4. **The reference.** `API | Intent | Return`. Say which call can **fail** and what it leaves
   behind when it does.
5. **A runnable example.** In `manual/examples/`, compiled and run by the build, quoted by the
   chapter.
6. **Counter-examples.** At least one `Wrong:` block, fenced as ```text. Write the code a reader
   would *actually* write — the mistake that looks correct. The `Wrong:` blocks are where the
   real teaching happens:

   - a keyed hash with a **fixed key** ("a secret everyone knows");
   - a **session token from the reproducible generator**;
   - an **ignored** seeding `bool`, which is tempting precisely because it is the only thing that
     can fail;
   - a **buffered writer never flushed** — output that never happened;
   - a **borrowed view kept** past the call that invalidates it;
   - a **state struct copied**, which is the use-after-free an audit reproduced.

   Then show the correct form as a ```c block, so the build checks it.

---

## 4. When you delete a public function

This is the case that produced the worst defect, so it gets its own list. Deleting
`proven_sysio_flush` left the manual **declaring it as public API, in the present tense**, for a
release.

1. Delete it from the header and the source.
2. Delete its `xcv_` alias (2.6 will fail otherwise).
3. Delete every **signature listing** of it in `manual/`.
4. Delete or rewrite every **present-tense** description of it.
5. If the manual explains *why* it is gone, write that in the **past tense** — a historical note
   is not a promise, and 2.5 only objects to `name(...)` written as a live call.
6. Grep the whole tree, including the examples: `grep -rn "the_name" manual/ README.md docs/`.
7. `./nob build`.
