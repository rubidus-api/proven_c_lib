# RFC-0002 — The view vocabulary, and the splitter every caller writes wrong

**Status:** proposed
**Date:** 2026-07-19
**Tracks:** `docs/BACKLOG.md` B-018 … B-023 (open)

> **Source material.** This RFC came out of reading Luca Sas, *Modern C and What We Can
> Learn From It* (ACCU 2021 — [slides](https://accu.org/conf-docs/PDFs_2021/luca_sass_modern_c_and_what_we_can_learn_from_it.pdf),
> [video](https://www.youtube.com/watch?v=QpAhX-gsHMs)), together with the post it cites as
> its base, Andre Weissflog, *Modern C for C++ Peeps*. The talk is 145 slides arguing that C
> is capable of a value-oriented, allocator-aware, view-based style, and it uses string
> handling as its worked example. Reading it against this library was useful in an unexpected
> way: **most of what the talk argues for, this library already does.** The gap it exposes is
> narrow, specific, and entirely on the non-owning side of the string API. That gap is the
> subject of this document. Everything the talk proposes that we should *not* adopt is in §3,
> with the reason, because a proposal that only lists what to copy is not an evaluation.

---

## 1. The finding

The talk's central string slide is a two-type split: a non-owning `str` (pointer + size) and
an owning `str_buf` (pointer + size + capacity + allocator). This library made that same
decision years of commits ago — `proven_u8str_view_t` and `proven_u8str_t`. The owning half is
in good shape and, measured against the talk's own list, is more complete than what the talk
proposes.

The non-owning half is not. The talk shows nine operations on `str`; this library has three
of them.

| Talk's `str` operation | In this library | |
|---|---|---|
| `str_match(a, b)` | `proven_u8str_view_eq` | ✅ |
| `str_sub(src, begin, end)` | `proven_u8str_view_slice(str, index, len)` | ✅ |
| `str_find_first(hay, needle)` | `proven_u8str_view_find` | ✅ |
| `str_contains(hay, needle)` | — | ❌ |
| `str_find_last(hay, needle)` | — | ❌ |
| `str_remove_prefix(src, prefix)` | — | ❌ |
| `str_remove_suffix(src, suffix)` | — | ❌ |
| `str_valid(s)` | — | ❌ |
| `str_pop_first_split(&src, sep)` / `for_str_split` | — | ❌ |

And two more that the talk does not list but that the same audit surfaced: there is no
three-way compare on views (so views cannot be sorted or used as ordered keys — only
`_eq` exists), and there is no way to trim whitespace from a view.

The one that matters most is splitting, and the talk is unusually well-placed to say why. Its
closing story is a production one: the speaker's first optimisation at Creative Assembly was a
string split — *"a seemingly cheap function"* whose cost only appeared cumulatively, fixed by
*"a non-allocating, lazily evaluated string splitter"* (slides 136–139). This library has no
splitter at all. `docs/RFC-0001` §1 already named it — *"split on `\n` by hand"* — and it is
still by hand.

"By hand" is the actual finding. Not that splitting is slow here, but that **the loop a
competent person writes first is wrong**, and nothing in the library stops them.

---

## 2. What that costs, measured

Everything in this section is reproducible from a committed harness — RFC-0001's numbers were
not, and became unreproducible the moment the machine changed:

```text
gcc -std=c2x -O2 -D_GNU_SOURCE -Iinclude -Iplatform \
    -o /tmp/rfc0002 docs/rfc-0002-benchmark.c src/proven/*.c platform/*.c && /tmp/rfc0002
```

`docs/rfc-0002-benchmark.c` is not built by `./nob` and is not part of the library. It contains
a local prototype of the iterator §4.1 proposes, because the library does not have one yet.

### 2.1 The hand-written loop is wrong

The natural formulation over today's API is "keep going while a separator is found":

```c
proven_size_t off = 0, at;
while ((at = proven_u8str_view_find(s, off, sep)) != PROVEN_INDEX_NOT_FOUND) {
    emit(proven_u8str_view_slice(s, off, at - off));
    off = at + sep.size;
}
```

It silently drops the tail after the last separator. Run against the obvious cases:

```text
input     correct  natural   proposed verdict
a,b,c           3        2          3 WRONG
a               1        0          1 WRONG
a,              2        1          2 WRONG
,a              2        1          2 WRONG
a,,b            3        2          3 WRONG
""              1        0          1 WRONG
natural loop wrong on 6 of 6; proposed iterator wrong on 0 of 6
```

Six of six. Not an edge case — the *common* case, `"a,b,c"`, loses `c`. Getting it right means
hoisting the final segment out of the loop as a special case, which is precisely the shape
people get wrong under deadline. The library currently asks every caller to write this, and
gives them nothing to check it against.

### 2.2 The cost of not having one

200,000 records of `2021/03/12,alpha,beta,gamma,delta\n` — 1,000,000 fields, 6.8 MB — split
into fields four ways. Each variant folds every field's bytes into an FNV checksum so the
optimiser cannot delete the work, and all four checksums must agree; they do
(`9f71ae6dd9dacc25`), so all four are doing the same job. gcc `-O2`, single thread, this
machine, median of three runs. Run-to-run spread is about ±1 ns/field for A, C and D and about
±2 for B (which is at the mercy of the allocator), so every difference the table is used to
argue about — including the smallest, A versus C at 3.2 ns — sits outside the noise. Absolute
values will differ on other hardware; the ratios are the claim.

| variant | ns/field | allocations | notes |
|---|---:|---:|---|
| A — hand-rolled `find` + `slice` (today, done correctly) | 17.0 | 0 | 10 lines for the inner loop |
| B — one owned `proven_u8str_t` per field | 56.9 | 1,000,000 | what you write when there is no splitter |
| C — the proposed iterator | 20.2 | 0 | 3 lines for the inner loop |
| D — libc `strtok_r` | 25.3 | 1 (whole-input copy) | destroys its input |

Three things come out of this table, and one of them is against the proposal.

**Against it: the iterator is 3.2 ns/field slower than a correct hand-rolled loop** — about
18%. That is real and it is inherent: the iterator re-slices `rest` on every step and cannot
keep the running offset in a register across the call boundary the way an inlined loop does.
The honest claim for §4 is therefore *not* that the splitter is faster. It is that it is
**correct, 3 lines instead of 10, and within 18% of a hand-rolled loop that most callers will
not write correctly** (§2.1). A library that trades 3 ns/field for six-of-six correctness on
that table is making the right trade, but it should say that is the trade.

**For it: variant B is the one that actually ships.** 3.4× slower and one malloc *per field* —
1,000,000 allocations to read 6.8 MB. This is exactly the talk's Chrome figure (half of all
allocations are `std::string`; one keystroke in the Omnibox once cost 25,000 allocations) and
it is what a caller writes when the library offers no way to iterate fields as views. The
splitter's real competition is not variant A. It is variant B.

**Also for it: `strtok_r` is not an escape hatch.** It is slower than the hand-rolled loop
*and* it destroys its input, so it needs a mutable copy of the entire corpus first. The talk
makes this point with a bug rather than a benchmark (slides 130–131): `char *date = "1981/04/01";
strtok(date, "/")` writes into a string literal. Undefined behaviour, and it compiles clean.
Views cannot express that bug.

### 2.3 A blocking discovery: empty and invalid views are the same value

The talk's iteration idiom terminates on validity — `str_valid(s)` is `s.data != NULL`, and
`for_str_split` loops while the remainder is valid. That cannot be ported here, and the reason
is not stylistic:

```text
empty field  : ptr=NULL size=0
out-of-range : ptr=NULL size=0
=> a valid empty field and an invalid slice are BIT-IDENTICAL: yes
```

`proven_u8str_view_slice` returns `{NULL, 0}` both for a legitimately empty result (the middle
field of `"a,,b"`) and for a slice that is out of range (`src/proven/u8str.c`, the `len == 0`
and `index >= str.size` branches). A pointer-validity test cannot separate *"an empty field"*
from *"there are no more fields"*, so an iterator built on it silently truncates `"a,,b"` and
`"a,"` — the same class of bug as §2.1, arriving by a different road.

This settles a design question in §4.1 before it is asked: **end-of-iteration must be its own
signal, not an inferred property of the returned view.**

---

## 3. What the talk proposes that we should not take

Three of the talk's ideas are good ideas that are wrong *for this library*, and saying so is
half the value of reading it.

**The `str_buf` stores its allocator; ours should not.** The talk's owning string carries
`allocator_cb allocator` in the struct, which removes a real hazard — this library requires the
caller to pass the *same* allocator to `_destroy` and `_append_grow` that they passed to
`_create`, and nothing enforces it. Tempting. But the allocator here is a 4-pointer value
(`ctx` + three function pointers); embedding it grows `proven_u8str_t` from 4 words to 8,
doubling the cost of every string in an array of strings, in a library whose stated posture is
that data structures are small and hosted services are passed in. It also breaks
`proven_u8str_borrow`, whose whole point is a string over a stack buffer with no allocator
anywhere. **Rejected** — but the hazard is real, and the mitigation belongs in §4.6 rather than
in the struct.

**`_Generic` overloading so call sites can pass bare `"literals"`.** The talk uses
`_Generic(split_by, const char*: str_pop_first_split_impl(src, cstr(split_by)), default: ...)`
so `str_pop_first_split(&date, "/")` works. It reads better than `PROVEN_LIT("/")`. It also
hides an O(n) `strlen` scan inside what looks like a no-op conversion, and this library already
made the opposite call deliberately: `PROVEN_LIT` is a compile-time `sizeof` and
`proven_u8str_view_from_cstr` is a visible scan, because the difference between the two is the
difference between free and linear. **Rejected for the string API.** (`fmt.h` already uses
`_Generic` for `PROVEN_ARG`, where the argument is genuinely of unknown type and no cost is
being concealed — that is a different situation, not a precedent.)

**The `for_str_split` iteration macro. Deferred, and the first draft of this section got the
reason wrong.** A `for`-loop macro with declarations in the initialiser is the ergonomic payoff
of the whole design, and it is the thing most likely to be asked for. This RFC originally
rejected it on two grounds, and an adversarial re-check of its own claims destroyed one of them
and narrowed the other. Both are recorded here because a rejection resting on a false premise is
worth less than no rejection at all.

The claim was that such a macro has no house precedent. It is false, and it was asserted with
the words "checked, not assumed", which is how a bad grep becomes a documented fact — the search
was for `FOREACH` and the library spells it `FOR_EACH`:

```text
include/proven/list.h:90:#define PROVEN_LIST_FOR_EACH(iter, list)
include/proven/list.h:96:#define PROVEN_LIST_FOR_EACH_SAFE(iter, safe_next, list)
include/proven/alias_xcv.h:84:#define XCV_LIST_FOR_EACH PROVEN_LIST_FOR_EACH
```

Both are `for`-loop macros, both are publicly aliased, and `_SAFE` even has the
comma-in-the-initialiser shape. **The precedent exists**, and a `for_str_split` would be
following it rather than establishing anything.

The second ground was `error.h`'s *"strictly avoids macros for control flow"*. That sentence is
real (`include/proven/error.h:9`) but it sits in `error.h`'s own `@file` block and describes
*error.h* — no `PROVEN_TRY` that hides a `return`. Reading it as library-wide policy was scope
inflation, and `list.h` is the proof that the library never held to it globally.

What survives is smaller and is the actual reason: a macro that hides a loop hides where `break`
and `continue` land, and macro state is invisible to a debugger. That is enough to **defer**,
not to reject. The plain iterator in §4.1 is three lines at the call site; ship it first, and add
the macro if three lines proves to be two too many. The ordering is the point — a macro can be
added later as a pure addition, and cannot be taken back once callers use it.

One more, smaller: the talk's error model at its midpoint (slides 75–80) is a `valid` flag
inside the payload struct that propagates through chained calls — `crop_to_cat` on an invalid
image returns an invalid image, so you check once at the end. It is elegant and it loses which
step failed, and it does work on values already known to be garbage. By slide 81 the talk has
moved to `error_code_t` inside the struct, which is this library's `proven_result_*` — so the
talk's own arc ends where this library already stands. Nothing to take.

---

## 4. The design

Six additions, all on views, all non-allocating, all pure functions of their inputs. No new
types except one iterator struct. Sketches are in `text` fences because they are sketches.

### 4.1 Splitting: an explicit iterator with an explicit end

```text
typedef struct {
    proven_u8str_view_t rest;   /* what has not been yielded yet */
    proven_u8str_view_t sep;
    bool                done;   /* set after the final field is yielded */
} proven_u8str_split_t;

proven_u8str_split_t proven_u8str_split(proven_u8str_view_t src, proven_u8str_view_t sep);
bool proven_u8str_split_next(proven_u8str_split_t *it, proven_u8str_view_t *out);
```

`_split_next` returns `true` and writes a field, or returns `false` exactly once the input is
exhausted. The `done` flag — not the emptiness of `out`, per §2.3 — is what ends the loop.

The contract must be stated in one sentence and never bent: **`n` separators yield `n + 1`
fields, always.** So `"a,b,c"` → 3, `"a"` → 1, `"a,"` → 2 (the second empty), `",a"` → 2,
`"a,,b"` → 3, `""` → 1 (empty). That is the rule that makes §2.1's table come out right, and it
is the rule `strtok` breaks (it collapses runs of separators, which is why `strtok` cannot parse
CSV). A caller who wants `strtok`'s behaviour filters empty fields, which is one `if`; a caller
who has `strtok`'s behaviour and wants this one cannot recover the information.

Call site, whole thing:

```text
proven_u8str_split_t it = proven_u8str_split(line, PROVEN_LIT(","));
proven_u8str_view_t field;
while (proven_u8str_split_next(&it, &field)) { ... }
```

The iterator is a caller-owned value with no self-referential pointers — `rest` points into the
*source*, not into the iterator — so it is copyable, unlike the state structs in `stream.h`.
That property is worth a test.

### 4.2 Trimming and affix removal return views

```text
proven_u8str_view_t proven_u8str_view_trim(proven_u8str_view_t s);        /* both ends */
proven_u8str_view_t proven_u8str_view_trim_start(proven_u8str_view_t s);
proven_u8str_view_t proven_u8str_view_trim_end(proven_u8str_view_t s);
proven_u8str_view_t proven_u8str_view_remove_prefix(proven_u8str_view_t s, proven_u8str_view_t prefix);
proven_u8str_view_t proven_u8str_view_remove_suffix(proven_u8str_view_t s, proven_u8str_view_t suffix);
```

`_remove_prefix` returns `s` unchanged when the prefix is absent — the same shape as the talk's,
and it composes with `_split_next` into the ordinary "parse a config line" job without a single
allocation or index variable.

"Whitespace" here means ASCII space, `\t`, `\n`, `\v`, `\f`, `\r` and nothing else. Not
locale-dependent, not Unicode — this library does not have a Unicode layer and must not grow
one by implication in a trim function.

### 4.3 Reverse search and `contains`

```text
proven_size_t proven_u8str_view_find_last(proven_u8str_view_t hay, proven_u8str_view_t needle);
bool          proven_u8str_view_contains(proven_u8str_view_t hay, proven_u8str_view_t needle);
```

`_find_last` returns an index and `PROVEN_INDEX_NOT_FOUND`, matching `_find` rather than the
talk's view-returning form — an index composes with `_slice` in both directions, a returned view
only forwards. `_contains` is a one-line wrapper whose value is entirely at the call site;
`find(...) != PROVEN_INDEX_NOT_FOUND` is a comparison against a sentinel that reads as noise in
an `if`.

The existing search is good and both of these should reuse it: `_find` already picks between
shift-or for short needles and Two-Way (Crochemore–Perrin, ported from musl's `memmem`) for
long ones. `_find_last` should not become a naive backwards scan that is `O(nm)`.

### 4.4 Views need a three-way compare

```text
int proven_u8str_view_cmp(proven_u8str_view_t a, proven_u8str_view_t b);  /* <0, 0, >0 */
```

Lexicographic over unsigned bytes, shorter-is-less on a common prefix. Today only `_eq` exists,
so a caller who wants to sort an array of views — the natural thing to do with the output of
§4.1 — has to write the comparison themselves, and byte-signedness is one of the two things
people get wrong when they do. `algorithm.h` has the sort (`proven_array_sort`, an introsort
with an `O(n log n)` guarantee) and it takes a `proven_compare_fn_t`; what it cannot be handed
today is a correct comparison for strings, because the library does not have one.

Note that `proven_compare_fn_t` is `int (*)(const void *a, const void *b)` and receives
*pointers to elements*, so sorting `proven_u8str_view_t` values needs a two-line adapter that
dereferences and calls `_cmp`. The adapter should ship next to `_cmp` rather than being
rediscovered by every caller.

### 4.5 A validity predicate, defined against §2.3

```text
bool proven_u8str_view_is_valid(proven_u8str_view_t s);   /* s.size == 0 || s.ptr != NULL */
```

The name is one token away from something that already exists —
`proven_u8str_is_valid(const proven_u8str_t *)`, which checks the *owning* type's internal
invariants (`u8str.h:107`). Two predicates whose names differ by `_view_` and whose meanings
differ substantially is a trap; whichever name is chosen, both headers must say what the other
one does. The alternative is to name this one for what it tests — `_view_is_well_formed` — and
that is worth deciding before it ships, not after.

This is *not* the talk's `str_valid`. It answers "is this view structurally sound" — a non-empty
view must have a pointer — and deliberately says `true` for the empty view, because §2.3 proved
that `{NULL, 0}` is a legitimate value that the library's own slicing produces. It must never be
used as a loop terminator, and its documentation should say so in those words, next to the
reproducer.

### 4.6 Mitigating the allocator hazard without storing the allocator

From §3: the caller must pair `_create`, `_reserve`, `_append_grow` and `_destroy` with the same
allocator, and nothing checks it. Rather than grow the struct for everyone, check it in debug
builds only — record a hash of `alloc.ctx` and `alloc.alloc_fn` in a field that exists only
under a new build-time switch (the library has no debug-check macro today; this item introduces
one), and assert on mismatch. Zero cost in release, catches the bug at
the call that commits it rather than at the heap corruption three seconds later. This is a
separate decision from §4.1–§4.5 and should not hold them up.

---

## 5. What is genuinely fine — stated plainly

An audit that finds everything broken has not been reading carefully. Measured against the
talk's own recommendations, this library is already the thing the talk is arguing for, and in
several places it is further along.

- **The owning/non-owning split already exists and is the talk's exact design.** `str` /
  `str_buf` is `proven_u8str_view_t` / `proven_u8str_t`, size-carrying, no null-termination
  dependency, no implicit conversions.
- **The owning half is more complete than the talk's.** Its `str_buf` offers make / append /
  insert / remove / `str`; this library has those plus `_replace_at`, `_replace_first`,
  `_reserve`, `_reset`, `_borrow`, `_append_partial` and a three-way explicit split between
  atomic-fixed, truncating and growable mutation. The talk's `void str_buf_append(...)` cannot
  even report failure.
- **The search is better than what the talk shows.** Two-Way with a shift-or fast path, not
  `strstr`.
- **Allocators are already the talk's model, done more carefully.** The talk's `allocator_t` is
  one `proc` pointer plus `user_data`; ours separates alloc / realloc / free, requires alignment
  to be passed and preserved, and specifies failure atomicity. Scratch allocators are already
  threaded through `fs.h` and `fmt.h`, which is the talk's "temporary allocator" slide.
- **The formatter already has the talk's extension point, statically.** The talk registers
  printers by string name at runtime (`logger_register_printer("cat", print_cat)`);
  `PROVEN_ARG_OF(objptr, renderfn)` does it with `_Generic` at compile time, so a typo is a
  compile error rather than a missing printer at runtime.
- **The hash map already takes views as keys, with no copy.** `map.h` supports
  `PROVEN_KEY_TYPE_U8_BORROWED` — a key that is a view into bytes the caller keeps alive — and
  hashes untrusted string keys with SipHash-2-4 under a
  per-process random key. The output of §4.1 can be used as a map key without an allocation,
  which is the whole point of a view-based split and is already true.
- **"Avoid libc" is already policy, and is checkable.** `PROVEN_FREESTANDING` is a real build
  mode (`./nob freestanding`) that compiles and tests the library without a hosted libc, not an
  aspiration in a README.
- **Value-oriented design is already pervasive.** Results are returned by value as
  `{err, value}` structs rather than through out-parameters; `memory.h` says so explicitly.
- **The library already declines the talk's weakest idea.** The talk's `defer` macro —
  `for (int i = (start, 0); !i; (i += 1), end)` — has no counterpart here, and `error.h` says
  in its own header why: no macro that hides a `return`. (This is narrower than "no control-flow
  macros"; `list.h` has `PROVEN_LIST_FOR_EACH`. See §3.)

The list of things to take from a 145-slide talk is eleven functions. That is the correct
outcome for a library that has been audited this hard, and it is worth recording that the
reading was done rather than leaving it as a question someone re-opens next year.

---

## 6. Adjacent findings, not part of this design

Surfaced by the same pass over `u8str.h`; recorded so they are not rediscovered.

- **`proven_u8str_mut_t` is declared and used by nothing.** Verified: no signature anywhere in
  `include/` or `src/` produces or consumes it — only its own `typedef` (`u8str.h:32`) and its
  alias (`alias_xcv.h:486`). A public type with no function that produces or consumes it is a
  promise the library never keeps. Delete it, or give it the one job it looks like it was meant
  for (a mutable span into a string's bytes). Note that deleting it is not a one-line change: it
  is documented in four manual files (`manual/manual-03-strings-text.md`,
  `manual/manual-07-alias-xcv-index.md` and both `manual-ko/` mirrors), and the documentation
  gates will fail the build until those go too. That is the gates working, not an obstacle.
- **Result structs disagree on their field name.** `proven_result_u8str_t`, `_size_t`, `_buf_t`
  and the `_mem_*` family use `.value`; `proven_result_i64_t`, `_u64_t`, `_f64_t` and
  `_u8str_view_t` use `.val`. Both spellings are load-bearing in existing code, so this is a
  deprecation, not an edit — but it should be one name.
- **`proven_u16str_t` has almost no vocabulary.** Eight public entry points — six out-of-line
  plus two `static inline` accessors — and among them no find, no compare, no slice, no
  conversion to or from UTF-8. That is a deliberate scope boundary today (u16 exists for the
  Windows boundary), and it should either stay deliberate and be written down as such in the
  header, or be closed. It is not this RFC's subject.

---

## 7. Implementation plan

Ordered so each step is useful on its own. Test-first per `docs/TESTING.md` §5.1 — every item
lands as a failing test and then a passing one, and §2.1's six-case table is the first test
written.

| Step | Item | Why here | Backlog |
|---|---|---|---|
| 1 | `proven_u8str_view_cmp` | Smallest possible change, no new type, unblocks sorting views. A warm-up that proves the test shape. | **B-021** |
| 2 | `_find_last`, `_contains` | Pure additions over the existing Two-Way search. No new semantics to argue about. | **B-020** |
| 3 | `_trim` / `_trim_start` / `_trim_end` / `_remove_prefix` / `_remove_suffix` | Independent of the iterator, and half the reason anyone wants the iterator is to trim what comes out of it. | **B-019** |
| 4 | `proven_u8str_split` / `_split_next` | The keystone. Lands with the `n` separators → `n + 1` fields table as its test, including `""` and `"a,,b"`. | **B-018** |
| 5 | `_is_valid` on views, plus the `{NULL,0}` ambiguity documented in `u8str.h` and manual chapter 3 | Cheap, but it must land *after* §4.1 so its documentation can point at the iterator as the thing you use instead. | **B-022** |
| 6 | Debug-only allocator-identity check on `proven_u8str_t` | Separable, and the only item here that touches the owning type. | **B-023** |
| 7 | Manual chapter 3: splitting section, the §2.1 counter-example, the `strtok`-on-a-literal counter-example | The documentation gates (`docs/DOCUMENTING.md`) will require it anyway; doing it deliberately beats doing it because a test failed. | **B-018** |

Steps 1–3 carry no design risk. Step 4 is the one with a contract worth arguing about before
it is written, and §4.1 states it so that the argument happens now rather than after callers
depend on it.

Deliberately **not** in this plan: UTF-8 codepoint iteration, case folding, `join`, string
interning, and u16 parity. Each is a real gap and none of them is this RFC's subject. A plan
that includes everything is a plan that finishes nothing.
