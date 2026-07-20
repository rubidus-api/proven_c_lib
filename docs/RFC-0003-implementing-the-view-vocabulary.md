# RFC-0003 — Implementing the view vocabulary

**Status:** proposed
**Date:** 2026-07-19
**Tracks:** `docs/BACKLOG.md` B-018 … B-022, B-024

> **RFC-0002 is the why; this is the how.** RFC-0002 established that the non-owning half of the
> string API is missing nine operations, measured what that costs, and sketched signatures in
> `text` fences. Sketches do not survive contact with boundary conditions. Writing this document
> — and then attacking it — found that RFC-0002's sketch **hangs forever** on an empty separator
> (§1.1), and that two of RFC-0002's statements about the existing search are wrong (§1.2). Both
> corrections are recorded here rather than quietly patched, because RFC-0002 is committed and a
> reader who has already read it needs to know which parts moved.
>
> Nothing in this document is implemented. Where it disagrees with RFC-0002, this document wins
> on facts about the code and RFC-0002 keeps the design intent — with one exception: the split
> contract (`n` separators yield `n + 1` fields) is restated here unchanged, because it is the
> one decision that cannot be revisited after callers exist.

---

## 1. What attacking the first draft changed

Four findings, each of which invalidated something written confidently a page earlier. They are
listed first because they are the reason to trust the tables in §4 more than the tables in
RFC-0002 §4.

### 1.1 An empty separator makes RFC-0002's loop hang

`proven_u8str_view_find` matches an empty needle at *every* position, and returns the position it
was asked to start from (`src/proven/u8str.c:394`):

```c
if (needle.size == 0) return start_offset; // Empty needle always matches at offset
```

```text
find("abc", off=0, empty needle)   0
find("abc", off=1, empty needle)   1
find("abc", off=3, empty needle)   3      <- size is a legal answer
find("abc", off=4, empty needle)   NOT_FOUND
```

RFC-0002 §4.1's `_split_next` advances by `at + it->sep.size`. With an empty separator that is
`at + 0`, `rest` never shrinks, and the iterator yields empty fields forever. A caller who
computes a separator at runtime — the ordinary case, since the point is parsing input — reaches
this with a one-character mistake, and the failure mode is a hang rather than a wrong answer.

**Decision: an empty separator yields exactly one field, the whole input.** Not an error: the
iterator has no error channel, and inventing one for this case is worse than defining it.

The first draft of this RFC then claimed the guard could live in `_split` alone, "so `_split_next`
needs no branch of its own". **That was wrong**, and it was wrong in the specific way that
matters: with the empty separator left in the iterator, `_split_next` cannot distinguish
"separator not found" from "separator is empty", because `find` returns *found, at `rest`'s
start* for the empty needle rather than `NOT_FOUND`. There is no state in `{rest, sep, done}` that
removes the need for the test. So the test is explicit, it is first, and §5 states the
termination argument that depends on it.

### 1.2 The existing search is not what RFC-0002 says it is

RFC-0002 §4.3 says `_find` "already picks between shift-or for short needles and Two-Way for long
ones", and §5 calls it "Two-Way with a shift-or fast path". Both describe the *fallback* as though
it were the dispatch. The actual structure (`src/proven/u8str.c:283-300` and `:445-478`):

| condition | algorithm | worst case |
|---|---|---|
| `needle.size == 1` | `memchr` | O(n) |
| low-entropy haystack, `needle.size <= 64` | Shift-Or (bitap) | O(n) |
| low-entropy haystack, `needle.size > 64` | Two-Way (Crochemore–Perrin, from musl) | O(n) |
| **everything else — the default path** | **rarest-needle-byte anchor + `memchr` + verify** | **O(n·m)** |

"Low entropy" is not a property of the input, it is a *sampled estimate*: up to 256 bytes are
counted and the fallback engages when even the rarest needle byte appears in more than about one
eighth of the sample (`u8str.c:437-442`). The common case — ordinary text — takes the anchored
path, which is fast in practice and **quadratic in the worst case**, and the sampler is a
heuristic that an adversary who controls the haystack can plausibly dress: a haystack that samples
as high-entropy but is dense in the anchor byte outside the sampled window.

This matters here because the first draft of §5 argued that `_find_last` must have a worst-case
linear guarantee "because the forward search does". **It does not.** The real posture of this
library's substring search is *fast heuristic path, with a linear fallback for the inputs the
heuristic is known to lose on*. §5 is rewritten around what is actually there.

RFC-0002's two sentences should be corrected when it is next touched. They do not change its
conclusions — the search is still better than `strstr`, which was the claim being made — but they
are not accurate descriptions of the dispatch.

### 1.3 `_cmp` cannot get byte-signedness right "for free" without a guard

`proven_memcmp` forwards to `proven_sys_mem_cmp`, which forwards to libc `memcmp`
(`platform/proven_sys_mem.c:146`), and `memcmp` compares as `unsigned char` by definition. So the
classic bug where `"\xFF"` sorts before `"a"` under signed `char` is avoided by construction.

The first draft went one step further and claimed the NULL guard inside `proven_sys_mem_cmp` was
"unreachable through `_cmp`, because the compared length is zero exactly when one side is empty".
That is false. A view of `{NULL, 5}` is *representable* — §4.5 exists precisely because it is —
and `cmp({NULL,3}, {"abc",3})` compares three bytes with a NULL pointer:

```text
proven_memcmp(NULL, "abc", 3) = -1
```

Today that returns `-1` instead of dereferencing, which is the platform layer being defensive, not
`_cmp` being correct. **`_cmp` must guard ill-formed views itself** (§3.4) rather than inherit
safety from a layer three calls down that is under no obligation to keep providing it.

### 1.4 A public symbol costs six files, and only two of them fail the build

The first draft said "five files, three enforced". Corrected:

| Obligation | Where | Enforced by |
|---|---|---|
| Declaration + doc comment | `include/proven/u8str.h` | — |
| Implementation | `src/proven/u8str.c` | — |
| `xcv_` alias | `include/proven/alias_xcv.h` | **`tests/test_docs_alias_completeness.c`** — build fails |
| Alias index row | `manual/manual-07-alias-xcv-index.md` | — (but see below) |
| Manual coverage | somewhere under `manual/` | **`tests/test_docs_manual_symbols.c`** — build fails |
| Korean mirror | `manual-ko/…-ko.md` | house rule only |
| Test registration | `nob.c` `all_tests[]`, 4 fields incl. `failure_hint` | nothing — an unregistered test silently never runs |

Two corrections worth stating plainly, because they change what "the gates will catch it" means:

- **The manual gate does not require chapter 3.** `test_docs_manual_symbols.c` opens `manual/` and
  scans every `.md` in it. A single row added to `manual-07-alias-xcv-index.md` — which these
  symbols need anyway — satisfies the coverage check. **Prose in chapter 3 is not enforced by
  anything.** It is still required by this plan; it is just required by us, not by the build.
- **An unregistered test is not a failing test, it is an absent one.** `nob.c` does not discover
  `tests/*.c`; it reads a table. A test file that is written, correct, and not in that table is
  green by omission.

---

## 2. Naming, settled

**Every function that takes or returns a view keeps the `proven_u8str_view_` prefix**, matching
the seven that exist (`_find`, `_eq`, `_slice`, `_starts_with`, `_ends_with`, `_from_cstr`,
`_to_cstr`). So `proven_u8str_view_split_next`, not RFC-0002 §4.1's `proven_u8str_split_next`.

**The validity predicate is `proven_u8str_view_is_well_formed`, not `_is_valid`.**
`proven_u8str_is_valid(const proven_u8str_t *)` already exists (`u8str.h:107`) and checks the
*owning* type's invariants. Two predicates differing only by `_view_`, with different meanings,
one of which is the conventional loop-terminator name in other libraries, is a trap. `_is_well_formed`
says what it tests.

---

## 3. The API, exactly

All declarations go in `include/proven/u8str.h`. Every function is a pure function of its
arguments: no allocation, no globals, no hidden state, and all are usable in
`PROVEN_FREESTANDING` builds because none touches a hosted service.

**One rule governs all of them:** an *ill-formed* view — `ptr == NULL` with `size > 0` — is
treated as empty, deterministically, by an explicit guard in each function. Nothing in this API
dereferences a pointer it has not checked, and nothing relies on a lower layer's defensiveness
(§1.3).

**One spelling of empty:** every function here returns `{NULL, 0}` for an empty result, matching
`proven_u8str_view_slice`, which is what most of them are built on. An empty view therefore does
not carry a position. This was argued the other way in the first draft — return `{ptr, 0}` so a
trimmed-to-nothing view still points into its source — and that is rejected, because split fields
come from `_slice` and would keep returning `{NULL, 0}` regardless, leaving the new API with two
spellings of empty inside a single table (§4.2).

### 3.1 Splitting

```c
typedef struct {
    proven_u8str_view_t rest;  /* not yet yielded; points into the caller's source bytes */
    proven_u8str_view_t sep;
    bool                done;  /* set once the final field has been yielded */
} proven_u8str_view_split_t;

[[nodiscard]] proven_u8str_view_split_t
proven_u8str_view_split(proven_u8str_view_t src, proven_u8str_view_t sep);

[[nodiscard]] bool
proven_u8str_view_split_next(proven_u8str_view_split_t *it, proven_u8str_view_t *out);
```

`_split_next` writes the next field to `*out` and returns `true`, or returns `false` once the
input is exhausted. It returns `false` without writing if `it` or `out` is NULL.

`_split` normalises **at construction**: an ill-formed `src` or `sep` (`ptr == NULL` with
`size > 0`) is stored as `{NULL, 0}`. This is not optional tidiness. Without it, `find` returns
`NOT_FOUND` for an ill-formed `rest` — its own guard — and step 3 below then hands the caller the
ill-formed view *as a field*, with `size > 0` and a NULL pointer, which is a dereference waiting
in the caller's loop body. Executing this spec against §4.1's table caught exactly that: eleven
rows passed and `{NULL,5}` yielded a 5-byte field over a NULL pointer.

Required shape of `_split_next`, in order, because §5's termination argument depends on it. It
may assume `rest` and `sep` are well formed, because `_split` guaranteed it:

1. `if (!it || !out || it->done) return false;`
2. `if (it->sep.size == 0) { *out = it->rest; it->done = true; return true; }` — §1.1
3. `at = find(it->rest, 0, it->sep); if (at == NOT_FOUND) { *out = it->rest; it->done = true; return true; }`
4. `*out = slice(it->rest, 0, at); it->rest = slice(it->rest, at + sep.size, …); return true;`

Step 2 must precede step 3. With it, every path either sets `done` or shrinks `rest` by at least
`sep.size >= 1` bytes.

The iterator holds no self-referential pointers — `rest` points into the *source*, never into the
iterator — so unlike the state structs in `stream.h` it may be copied, and copying forks the
iteration. §6 tests that, because it is the kind of property that stays true until someone adds a
field.

**Apart from that ill-formed normalisation, `_split` does not rewrite `sep`.** A caller that
passes a well-formed separator — every case except `ptr == NULL` with `size > 0` — reads back
exactly what it passed. The struct is public and its fields are readable; they are not stable
state to mutate. The one exception is the safety guard above, and it is an exception rather than
a general licence to rewrite: an ill-formed view has no bytes to preserve.

### 3.2 Trimming and affix removal

```c
[[nodiscard]] proven_u8str_view_t proven_u8str_view_trim(proven_u8str_view_t s);
[[nodiscard]] proven_u8str_view_t proven_u8str_view_trim_start(proven_u8str_view_t s);
[[nodiscard]] proven_u8str_view_t proven_u8str_view_trim_end(proven_u8str_view_t s);

[[nodiscard]] proven_u8str_view_t
proven_u8str_view_remove_prefix(proven_u8str_view_t s, proven_u8str_view_t prefix);
[[nodiscard]] proven_u8str_view_t
proven_u8str_view_remove_suffix(proven_u8str_view_t s, proven_u8str_view_t suffix);
```

Whitespace is exactly six ASCII bytes: `' '`, `'\t'`, `'\n'`, `'\v'`, `'\f'`, `'\r'`. Not
locale-dependent, not Unicode, and the header must say so in those words — a trim function that is
vague about its character set is how a Unicode dependency arrives by implication.

`_remove_prefix` returns `s` unchanged when the prefix is absent. That is not an error and there
is no way to ask whether it fired; a caller who needs to know calls `_starts_with` first.

### 3.3 Reverse search and containment

```c
[[nodiscard]] proven_size_t
proven_u8str_view_find_last(proven_u8str_view_t haystack, proven_u8str_view_t needle);
[[nodiscard]] bool
proven_u8str_view_contains(proven_u8str_view_t haystack, proven_u8str_view_t needle);
```

`_find_last` returns the **start position of the last occurrence**, or `PROVEN_INDEX_NOT_FOUND`.
Occurrences may overlap: `find_last("aaa", "aa")` is `1`, not `0`.

"Position", not "index", and the distinction is load-bearing: the return value is in
`[0, haystack.size]`, and it equals `haystack.size` **only for an empty needle**, which matches
`_find` returning `start_offset` for an empty needle including when that offset is `size` (§1.1).
The header must warn that `s.ptr[find_last(s, needle)]` is a read past the end when `needle` is
empty — the one case where the returned position is not a valid byte index.

`_find_last` takes no `start_offset`. `_find` has one because forward search resumes; a reverse
search would need an *end* limit, which is spelled `_slice` then `_find_last`, and a parameter
every caller passes as zero is worse than no parameter.

### 3.4 Ordering

```c
[[nodiscard]] int proven_u8str_view_cmp(proven_u8str_view_t a, proven_u8str_view_t b);
[[nodiscard]] int proven_u8str_view_cmp_ptr(const void *a, const void *b);
```

`_cmp` compares the first `min(a.size, b.size)` bytes with `proven_memcmp`; **if that comparison
returns zero**, the shorter view compares less. (The antecedent is the comparison *result*, not
the length — an ambiguity in the first draft that would have left `cmp("a","ab")` undefined.)

Returns a negative value, zero, or a positive value — **not necessarily −1/0/1**, and the header
must say so, because `== -1` is the second-most-common way to misuse a comparator.

Ill-formed views are treated as empty by an explicit guard before any comparison (§1.3, §3
preamble). Under that guard `_cmp` is a total order on views: antisymmetric, transitive, total.

`_cmp_ptr` is the adapter for `proven_array_sort`, whose `proven_compare_fn_t` is
`int (*)(const void *, const void *)` receiving *pointers to elements* (`algorithm.h:20,49`). It
reads its arguments as `const proven_u8str_view_t *` and is undefined for anything else — the
normal contract for a `qsort`-shaped comparator, and it must be documented as such. It exists so
that sorting an array of views does not require every caller to rediscover the indirection.

### 3.5 Structural validity

```c
[[nodiscard]] bool proven_u8str_view_is_well_formed(proven_u8str_view_t s);
```

True when `s.size == 0 || s.ptr != NULL`. It answers "could this view be read safely", nothing
more. Its doc comment must state in words that it is **not** an end-of-iteration test, and point
at why: `_slice` returns `{NULL, 0}` for a legitimately empty result *and* for an out-of-range
request, so it cannot distinguish them (RFC-0002 §2.3). This is the one function here whose
documentation matters more than its body.

---

## 4. Behaviour tables

These are the test tables; §6 says which file each lands in.

§4.1 is not only a table — it is executed. `docs/rfc-0003-spec-check.c` transcribes §3.1's four
steps literally and runs them against all thirteen rows below and all three properties, so the
specification can be checked before any of it is implemented:

```text
gcc -std=c2x -O2 -D_GNU_SOURCE -Iinclude -Iplatform \
    -o /tmp/rfc0003 docs/rfc-0003-spec-check.c src/proven/*.c platform/*.c && /tmp/rfc0003
```

It currently reports all thirteen rows passing, and 200,000 randomised cases with no count
mismatch, no non-termination, no copy-fork divergence and no field outside its source — over
380,000 fields and 86,000 forked iterators, printed so that a vacuous pass is visible. That is
what a specification should be able to say about itself, and it is how the `{NULL,5}` hole in
§3.1 was found rather than shipped.

Both numbers were wrong when this paragraph was first written: the harness skipped the
NULL-argument row and two of the three properties while the prose claimed full coverage. An
unchecked coverage claim is the same defect as an unchecked specification, one level up.

### 4.1 `_split` / `_split_next`

The contract, unchanged from RFC-0002 §4.1: **`n` separators yield `n + 1` fields**, where `n`
counts **non-overlapping occurrences found left to right** — the same walk `_split_next` performs.
The term is defined here because §3.3 uses "occurrence" in the *overlapping* sense for
`_find_last`, and the two senses give different answers on the same input (row 9 below).

| `src` | `sep` | fields yielded | note |
|---|---|---|---|
| `"a,b,c"` | `","` | `"a"`, `"b"`, `"c"` | the common case |
| `"a"` | `","` | `"a"` | no separator is still one field |
| `"a,"` | `","` | `"a"`, `""` | trailing separator yields a trailing empty |
| `",a"` | `","` | `""`, `"a"` | leading too |
| `"a,,b"` | `","` | `"a"`, `""`, `"b"` | empty fields preserved, unlike `strtok` |
| `""` | `","` | `""` | one empty field, not zero fields |
| `{NULL,0}` | `","` | `""` | a null view is an empty string |
| `"aXXb"` | `"XX"` | `"a"`, `"b"` | multi-byte separator |
| `"aXXXb"` | `"XX"` | `"a"`, `"Xb"` | leftmost, non-overlapping: **one** occurrence, two fields |
| `"abc"` | `""` | `"abc"` | §1.1 — empty separator, one field, terminates |
| `"abc"` | `{NULL,0}` | `"abc"` | ill-formed separator is treated as empty (§3 preamble) |
| `{NULL,5}` | `","` | `""` | ill-formed source is treated as empty |
| any | `it == NULL` or `out == NULL` | — | `_split_next` returns `false` |

Properties to test as properties, not rows:

- **For `sep.size > 0` only:** field count equals the number of non-overlapping left-to-right
  occurrences of `sep`, plus one. The scope restriction is not decoration — an empty separator
  "occurs" at all `size + 1` positions (§1.1), which for `"abc"` is 4, so an unscoped property
  predicts **5** fields where row 10 requires 1.
- A copied iterator continues independently of the original.
- Every field is a sub-range of the source, or empty.

Note that fields are produced by `_slice`, so **every** empty field is `{NULL, 0}` (§3 preamble) —
rows 3, 4, 5, 6, 7 and 12, including row 4's *leading* empty, which is the one a test author is
most likely to guess points at offset 0. The tests must assert on size, never on `ptr`.

### 4.2 Trimming and affix removal

| call | result |
|---|---|
| `trim("  a  ")` | `"a"` |
| `trim("a")` | `"a"` |
| `trim("   ")` | empty |
| `trim("")` | empty |
| `trim("\t\n\v\f\r a \r\f\v\n\t")` | `"a"` — all six bytes, both ends |
| `trim_start("  a  ")` | `"a  "` |
| `trim_end("  a  ")` | `"  a"` |
| `trim("a b")` | `"a b"` — interior whitespace untouched |
| `trim({NULL,5})` | empty — ill-formed treated as empty |
| `remove_prefix("foobar", "foo")` | `"bar"` |
| `remove_prefix("foobar", "xyz")` | `"foobar"` — unchanged, not an error |
| `remove_prefix("foo", "foo")` | empty |
| `remove_prefix("foo", "foobar")` | `"foo"` — prefix longer than input, unchanged |
| `remove_prefix("foo", "")` | `"foo"` |
| `remove_suffix("foobar", "bar")` | `"foo"` |
| `remove_suffix("foobar", "xyz")` | `"foobar"` |
| `remove_suffix("foo", "foo")` | empty |

Every "empty" above is `{NULL, 0}` (§3 preamble). Assert on size.

### 4.3 `_find_last`

| haystack | needle | result |
|---|---|---|
| `"abcabc"` | `"abc"` | `3` |
| `"abcabc"` | `"z"` | `NOT_FOUND` |
| `"aaa"` | `"aa"` | `1` — overlapping occurrences count here (§3.3) |
| `"abc"` | `"abc"` | `0` — whole-string match |
| `"abc"` | `"abcd"` | `NOT_FOUND` — needle longer than haystack |
| `"a/b/c"` | `"/"` | `3` — the single-byte case, most real uses |
| `"abc"` | `""` | `3` — a position, not an index; equals `size` (§3.3) |
| `""` | `""` | `0` |
| `{NULL,0}` | `{NULL,0}` | `0` — both empty; the interaction row |
| `""` | `"a"` | `NOT_FOUND` |
| `{NULL,0}` | `"a"` | `NOT_FOUND` |
| `{NULL,5}` | `"a"` | `NOT_FOUND` — ill-formed treated as empty |

### 4.4 `_cmp`

| a | b | sign |
|---|---|---|
| `"a"` | `"b"` | negative |
| `"b"` | `"a"` | positive |
| `"a"` | `"a"` | zero |
| `"a"` | `"ab"` | negative — prefix sorts first |
| `"ab"` | `"a"` | positive |
| `""` | `""` | zero |
| `""` | `"a"` | negative |
| `{NULL,0}` | `""` | zero — matches `_eq`, which returns 1 for this pair |
| `{NULL,5}` | `""` | zero — ill-formed treated as empty (§1.3) |
| `"\xFF"` | `"a"` | **positive** — unsigned comparison; this row is the signedness pin |
| `"a\0b"` | `"a\0c"` | negative — embedded NULs are data, not terminators |

The last row matters more than it looks: it is the property that distinguishes this library's
strings from C strings, and belongs in the suite as a statement of identity.

### 4.5 `_is_well_formed`

| input | result |
|---|---|
| `{"abc", 3}` | `true` |
| `{NULL, 0}` | `true` — the empty view is well formed |
| `{"", 0}` | `true` |
| `{NULL, 5}` | `false` — the only false case |

---

## 5. Algorithms and complexity

**`_split_next` terminates**, and the argument is the ordered shape in §3.1: step 2 disposes of
`sep.size == 0` before any search, so on reaching step 4 `sep.size >= 1` and `rest` shrinks by at
least one byte; otherwise steps 2 and 3 set `done`. A full iteration costs one `_find` per field
over disjoint ranges, so it is O(total input) plus the search's own behaviour. No new algorithm.

**`_contains` is `_find(...) != PROVEN_INDEX_NOT_FOUND`** — no separate implementation, so it
cannot drift from `_find`.

**`_cmp` is one guarded `proven_memcmp` plus a size comparison.** No loop of its own.

**Trim and affix removal are byte loops with no allocation.** `_remove_prefix` must be
`_starts_with` plus a `_slice`, so there is exactly one definition of "is a prefix".

### `_find_last`, and what "matching the house standard" actually means

§1.2 removed the argument the first draft used here. The forward search does **not** guarantee
worst-case linearity; its default path is an anchored `memchr`-and-verify scan that is O(n·m) in
the worst case, with a sampled-entropy fallback to Shift-Or / Two-Way for the inputs that path is
known to lose on. So "the reverse search must be linear because the forward one is" was false.

What the forward search actually establishes is a *shape*: **be fast on ordinary input, and detect
the inputs where the fast strategy degrades**. The staged plan below matches that shape as far as
is reasonable in a first implementation, and is explicit about where it does not.

1. **`needle.size == 1`: backward byte scan.** O(n), trivially correct, and this is what almost
   every real caller wants — the last `/`, the last `.`, the last newline.
2. **`needle.size <= 64`: backward Shift-Or (bitap).** Bitap is direction-agnostic: running the
   same recurrence right-to-left with masks built from the reversed needle detects a match ending
   where the forward version would detect one starting, needs no buffer and no allocation, and is
   O(n) unconditionally. It reuses the *technique* in the file, not the code —
   `proven_u8_find_shiftor` is forward-only.
3. **`needle.size > 64`: loop over `_find`, advancing one byte past each match, keeping the last.**
   Correct, and O(n + k·(m + C)) for k occurrences of an m-byte needle, where C is the per-call
   constant of `_find` — up to a 256-entry mask build and a 256-byte entropy sample. It degrades
   to quadratic on periodic input.

Two consequences to write in the header rather than discover later:

- **Path 2 has a better worst case and a worse average case than the forward search.** On ordinary
  text, forward `_find` uses the anchored scan and skips most of the haystack; backward bitap
  touches every byte. `find_last` will therefore be slower than `find` on the same input for
  multi-byte needles. That is an accepted trade for a first implementation — simpler, and never
  quadratic — and **B-024** covers revisiting it with an anchored backward fast path if profiling
  justifies the second code path.
- **Path 3 is quadratic and says so.** It is bounded by a needle length the *caller* chooses, so
  it is not reachable by an attacker who controls only the haystack — but it is reachable by one
  who controls both. B-024 covers replacing it with a backward Two-Way.

Correctness for all three paths is established the way this repository establishes it elsewhere:
**a brute-force oracle**. A twenty-line reference `find_last` that tries every start position with
`memcmp` is obviously correct and hopelessly slow; the test drives it and the real implementation
over randomised haystacks and needles — including the periodicity cases that break path 3's
complexity, runs of a single byte and of `"aab"` — and requires identical answers. That is the
discipline that caught the transcribed-from-memory KAT vectors, and it is the only reason to trust
path 2, which is the one piece of genuinely new algorithm work in this RFC.

---

## 6. Test plan

Test-first per `docs/TESTING.md` §5.1, whose exact requirement shapes §7: commit one contains
**the header with its documentation, a stub that compiles and does nothing useful, and the failing
test**. Not the test alone — a test calling an undeclared function is a compile error, not a red
test.

| File | Covers |
|---|---|
| `tests/test_unit_u8str_split.c` | §4.1 in full, plus the scoped field-count property and iterator copyability |
| `tests/test_regression_split_empty_sep.c` | §1.1. Must assert on a **bounded** field count — a test that reproduces a hang by hanging is not a test |
| `tests/test_unit_u8str_view_ops.c` | §4.2, §4.3, §4.5 |
| `tests/test_unit_u8str_view_cmp.c` | §4.4, including the `"\xFF"` signedness row and the embedded-NUL row |
| `tests/test_differential_find_last_oracle.c` | §5's oracle, randomised with a fixed seed so a failure reproduces |

The differential name follows the existing pair `tests/test_differential_float_host_oracle_f32.c`
and `_f64.c`, which check the float parser against the host `strtod`. Same idea, different module.

Every file needs its `nob.c` `all_tests[]` entry — path, name, intent, `failure_hint` — or it
never runs (§1.4).

Once `_split` is real, `docs/rfc-0002-benchmark.c` should be re-run with its local prototype
replaced by the shipped function, to confirm RFC-0002 §2.2's 20.2 ns/field survives contact with
the implementation. If it does not, that table gets a correction note rather than a quiet edit.

---

## 7. Commit sequence

The first draft's sequence put the test in commit 1 and the header in commit 2. That does not
compile, and it violates the rule §6 cites. Corrected: **commit 1 of each pair carries the header,
the doc comment, a stub, the alias, the manual row, the `nob.c` entry, and the failing test.**
Everything the gates check lands with the declaration, because that is when the gates fire.

| # | Commit | Contents | Backlog |
|---|---|---|---|
| 1 | test: view comparison contract | header decls + doc comments for `_cmp`/`_cmp_ptr`, stubs returning `0`, alias entries, `manual-07` rows, chapter 3 prose, ko mirror, `nob.c` entry, `test_unit_u8str_view_cmp.c` **failing** | B-021 |
| 2 | feat: view comparison | the implementation; the same test, now green, with no assertion weakened | B-021 |
| 3 | test: trim and affix removal | same shape, five stubs returning `s` | B-019 |
| 4 | feat: trim and affix removal | implementation | B-019 |
| 5 | test: reverse search | stubs returning `NOT_FOUND`/`false`; `test_unit_u8str_view_ops.c` rows + the oracle test | B-020 |
| 6 | feat: reverse search | the three paths of §5; header documents path 2's average-case trade and path 3's quadratic | B-020, B-024 |
| 7 | test: splitting | the iterator type, `_split` stub, `_split_next` stub returning `false`; both split tests | B-018 |
| 8 | feat: splitting | the ordered implementation of §3.1, in that order; chapter 3 gains the RFC-0002 §2.1 wrong-loop counter-example | B-018 |
| 9 | feat: structural validity | `_is_well_formed` plus the documentation that is its real payload | B-022 |
| 10 | docs: re-run the benchmark | swap the prototype in `docs/rfc-0002-benchmark.c`; correct RFC-0002 §2.2 if the number moved | B-018 |

`_cmp` is first deliberately: it is the smallest change that exercises the entire obligation chain
in §1.4 — header, stub, alias, alias index, manual, Korean mirror, `nob.c`, two build-failing
gates — so the process is proven on one function before it is repeated eleven times.

B-023 (the debug-only allocator-identity check) is not in this sequence: it touches the owning
type, shares nothing with these eleven functions, and RFC-0002 §4.6 already separated it.

---

## 8. What could go wrong

- **The `n + 1` contract is irreversible.** Once callers depend on `"a,"` yielding two fields, a
  library that later "fixes" it to one breaks data-dependent behaviour in code that will not
  notice. If this contract is wrong it must change before commit 8. It is the single decision here
  worth arguing about now.
- **Step 2 of §3.1 will look redundant and get "cleaned up".** It is the only thing standing
  between the library and an infinite loop, and its redundancy is exactly what makes it a tempting
  deletion. The regression test in §6 is what makes deleting it fail; the comment above it should
  name §1.1.
- **Backward bitap is new code, and new code is where bugs are.** It is the only item that is not
  a rearrangement of existing primitives, which is why §5 requires an oracle rather than a table
  of hand-picked cases. The standing audit rule applies at full force: this is the function to
  re-audit after it passes.
- **`_is_well_formed` will be used as a loop terminator anyway**, because that is what the name
  pattern means everywhere else. The mitigation is documentation, which is weaker than a compiler.
  Accepted, and the reason the name is not `_is_valid` (§2).
- **`find_last` returning `size` for an empty needle is a real edge.** It is consistent with
  `_find` and it is the one return value that is not a valid byte index. If a caller reports
  reading past the end, this is the first place to look.
- **Chapter 3 prose is not enforced** (§1.4). The plan requires it; only we require it. A future
  commit that adds a view function and satisfies the gate with an alias-index row alone will pass
  the build with the chapter left behind.

---

## 9. Deliberately not in this plan

UTF-8 codepoint iteration, case folding and case-insensitive comparison, `join`, string interning,
splitting on a *set* of separator bytes (`find_first_of`), a `for_str_split` iteration macro
(RFC-0002 §3 defers it until the plain iterator has been used in anger), u16 parity, and B-023.

Each is a real gap. A plan that includes everything is a plan that finishes nothing.
