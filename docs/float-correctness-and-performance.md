# Floating-point conversion: correctness and performance

This document describes how `proven_c_lib` converts between decimal text and
IEEE-754 binary floating point, how those conversions are validated, and how
they compare to the host C library. It is meant to be read on its own: after
reading it you should be able to judge, without re-deriving anything, whether the
parser and formatter are trustworthy and fast enough for production use.

- **Scope:** decimal → `binary64` parsing (the scanner), and `binary64`/`binary32`
  → decimal formatting (shortest, fixed `%f`, scientific `%e`).
- **Headline result:** the formatter has been validated **exhaustively** over all
  4,278,190,080 finite `binary32` values with **zero** failures, and over
  **2,560,000,000** random `binary64` values (after that sweep found and fixed one
  real defect); the parser is **bit-for-bit identical** to the host `strtod` on
  every input tested; and on realistic data the library is **faster than glibc** at
  almost everything it does. Full numbers and methodology are below.
- **Environment for the measurements in this document:** x86-64, GCC 14.2.0,
  glibc 2.41, single-threaded benchmarks, 16-thread exhaustive sweep.

---

## 1. Algorithms

### 1.1 Parsing: decimal → binary64

The scanner (`proven_scan_f64`) is a three-tier design, the same structure used
by the fastest correctly-rounded parsers in production today (glibc, Go,
Rust/`fast_float`). Each tier is exact; the faster tiers simply handle the easy
majority and hand the hard cases down.

1. **Clinger fast path** (`proven_float_try_clinger`). When the significand fits
   in 53 bits and the power of ten is small, the value is produced with a single
   correctly-rounded double multiply/divide by an exact power of ten. Covers the
   bulk of human-written numbers.

2. **Eisel–Lemire** (`proven_float_try_eisel_lemire_*`). A 128/256-bit fixed-point
   multiply of the significand by a cached power of ten, with the
   error/round-to-even check that proves the result is correctly rounded. Covers
   most remaining inputs without any big-integer work.

3. **Exact big-integer fallback.** When the fast paths cannot *prove* the rounding
   (the value sits too close to a rounding boundary), the parser falls back to an
   exact comparison. It seeds the binary exponent from the decimal exponent
   (`proven_float_decimal_binary_exp_bounds`), then does an exact big-integer
   comparison of the decimal significand against the candidate binary value to
   decide the final bit. The seed makes this a 3–4 step decision rather than a
   53-step binary search. This tier is always correct; it is the arbiter.

Because tier 3 is exact and the fast tiers only ever return a result they have
*proven* correct, the parser is correctly rounded (round-to-nearest, ties to
even) for every input.

### 1.2 Formatting: binary → decimal

There are two formatting jobs, with different algorithms.

**Shortest** (`proven_float_format_f64_policy` / `_f32` with the *shortest*
option) — the shortest decimal string that round-trips back to the exact same
float. Two cooperating algorithms:

- **Grisu3** (`proven_float_shortest_digits_grisu`) — Loitsch's fast-dtoa. It
  uses 64-bit extended-precision (`diy_fp`) arithmetic and a cached power-of-ten
  table to produce the digits, plus a *self-check* (`round_weed`): when it cannot
  prove the digits it produced are the shortest and correctly chosen, it reports
  failure instead of emitting a possibly-wrong answer.
- **Dragon4 / Burger–Dybvig** (`proven_float_shortest_digits_core`) — the exact
  big-integer algorithm. It is used as the fallback whenever Grisu3 declines, so
  the output is always exact. It is also the independent oracle the two
  algorithms are cross-checked against.

The net effect: Grisu3 handles ~99% of values in tens of nanoseconds, Dragon4
guarantees correctness on the rest, and the two being independent implementations
that must agree is itself a strong correctness argument.

**Fixed precision** `%f` and `%e` (`proven_float_format_fixed_f_exact`,
`proven_float_format_e_exact`) — *N* digits after the point / *N* significant
digits, correctly rounded. These are computed with **exact big-integer
arithmetic** (`proven_float_scaled_round_digits`): the value is written as an
exact rational, scaled by the requested power of ten, and divided with
round-half-to-even. There is no floating-point approximation and **no `long
double`** anywhere in the formatter, so the result is correct at any magnitude
and any precision up to the big-integer capacity (subnormals, values larger than
2^64, hundreds of fractional digits — all exact).

---

## 2. Validation

### 2.1 Exhaustive binary32 sweep — the headline

Every IEEE-754 `binary32` value is enumerable (2^32 bit patterns), so the
formatter and parser were tested against **all of them**, using the host C
library as an *independent* oracle. NaN and infinity are excluded, leaving
**4,278,190,080** finite values; each was checked on five properties:

| # | Property | Oracle |
|---|---|---|
| B | the shortest string parses back to the exact value | host `strtof(S) == v` |
| C | minimality: the correctly-rounded (D−1)-digit decimal does **not** round-trip | host `strtof` |
| D | the parser matches the host on the shortest string | `scan_f64(S)` bits == `strtod(S)` |
| E | the parser matches the host on a canonical `%.9e` rendering | `scan_f64` bits == `strtod` |

B and C together prove that the formatter's output is a *shortest* round-tripping
decimal — exactly the property the Ryu, Grisu, and double-conversion test suites
assert. D and E prove the parser is correctly rounded (bit-identical to glibc) on
these inputs.

**Result — all 4,278,190,080 finite values, zero failures:**

```
finite values checked: 4278190080
formatter error          : 0
B round-trip (strtof)    : 0
C minimality             : 0
D parser on S (vs strtod): 0
E parser canonical       : 0
TOTAL FAILURES           : 0
```

(16 threads, ~28 minutes, ~2.5 M values/s. Average shortest length: 7.65
significant digits.)

This is exhaustive: there is no `binary32` value for which the shortest formatter
produces a wrong or non-minimal string, and none for which the parser disagrees
with glibc on these decimals.

#### A note on validation rigour (why the oracle itself was double-checked)

An earlier version of this sweep reported 8 "failures" out of 4.28 billion. Every
one turned out to be a **flaw in the test oracle, not in the library** — and
chasing them down is worth recording, because it is exactly the kind of subtlety
that makes naive float testing untrustworthy:

- **6 cases were powers of two** (e.g. `0x0f800000` = 2⁻⁹⁶). At a power of two the
  float's rounding interval is *asymmetric* (the next value below is half an ULP
  away, the next above is a full ULP). The library correctly emitted the unique
  shortest decimal inside that interval (`1.2621775e-29`), but the first oracle
  compared against `printf("%.7e")`, which simply rounds the real number to 8
  digits (`1.2621774e-29`) — a value that does **not** round-trip. Confirmed with
  the system `strtof`: only the library's digits round-trip. Both independent
  internal algorithms (Grisu3 and Dragon4) agreed, which was the tell.
- **2 cases were double-rounding** in the test harness: it parsed with the f64
  scanner and then cast to `float`, i.e. decimal → f64 → f32, which can differ by
  one ULP from decimal → f32. The library only offers a decimal → `binary64`
  parser; narrowing to `float` is the caller's cast. The host `strtof` (a direct
  decimal → f32 conversion) round-trips, and so does the library's f64 result —
  the discrepancy was purely the harness's extra rounding step.

The numbers above are from the corrected oracle (`strtof` for the formatter's
round-trip, bit-exact `strtod` for the parser), which respects IEEE-754
semantics. The lesson baked into the harness: validate float text the way the
target type actually rounds, not the way a decimal printf rounds.

### 2.2 binary64 — differential fuzzing

`binary64` has 2^64 values and cannot be enumerated, so it is validated with a
large-scale randomized differential sweep against the host — the same four checks
as the `binary32` sweep, with `strtod` (a direct decimal → `binary64` conversion)
as the round-trip oracle, so there is no double-rounding subtlety.

**2,560,000,000 random finite doubles**, drawn from a mix of generators (uniform
64-bit patterns, subnormals, and `mantissa × 10^exp` decimals spanning the full
exponent range) so both the fast paths and the exact fallback are exercised:

```
random finite values     : 2560000000
avg shortest digits       : 16.2459

formatter error          : 0
B round-trip (strtod)    : 0
C minimality             : 0
D parser on S (vs strtod): 0
E parser canonical       : 0
TOTAL FAILURES           : 0
```

(16 threads, ~19.5 minutes.) B and C prove every shortest string round-trips and
is minimal; D and E prove the parser is bit-identical to `strtod` on both the
shortest strings and canonical `%.17e` renderings.

This sweep also did its job as a bug-finder. An earlier run flagged 93 values
(all just below a power of ten, e.g. `9.995442674871462e-265`) as non-minimal:
both digit generators were emitting a spurious leading zero with the decimal
exponent one too high (`0.9995442674871462e-264`). The output still round-tripped,
but it was non-canonical and one digit too long. The shortest wrappers now strip
the leading zero and lower the exponent; the trailing digits were already correct,
so the value and minimal length are unchanged. After the fix the sweep above
reports zero, and the exhaustive `binary32` sweep — which had no such cases — still
reports zero. This is a concrete example of the validation catching a real defect
that round-trip testing alone would have missed.

Together with the exhaustive `binary32` result, this gives strong evidence for the
`binary64` paths: the shortest formatter, the exact `%f`/`%e` engine, and the
parser share the same big-integer core and the same Grisu3/Dragon4 code,
parameterised only by the significand width and exponent range.

---

## 3. Performance vs the host C library

Single-threaded, GCC 14.2.0 `-O2`, glibc 2.41, 200,000-value corpora, 8
repetitions. `ratio` is proven ÷ host, so **< 1.0 means the library is faster**.
Accuracy is measured in the same run.

### 3.1 Parsing (decimal → binary64), bit-exact vs `strtod`

| input corpus | proven | host `strtod` | ratio | mismatches |
|---|---:|---:|---:|---:|
| short human decimals (`%.6g`) | 95.2 ns | 135.2 ns | **0.70×** | 0 |
| shortest round-trip (~16 dig) | 372.3 ns | 281.8 ns | 1.32× | 0 |
| hardest 17-digit (`%.17g`) | 365.4 ns | 283.9 ns | 1.29× | 0 |

The parser is **bit-for-bit identical to glibc** on every corpus. It is *faster*
than glibc on short, human-sized numbers (the common case) and about 1.3× slower
on adversarial 16–17 significant-digit inputs, where glibc's hand-tuned big-integer
path is still ahead. There is no accuracy cost anywhere.

### 3.2 Formatting — normal magnitudes (1e-6 … 1e6)

This is the corpus that reflects ordinary application data.

| operation | proven | host | ratio | accuracy |
|---|---:|---:|---:|---|
| shortest | 142.5 ns | 542.7 ns (`%.17g`) | **0.26×** | 0 round-trip failures; avg 16.93 vs 17.66 digits |
| `%f` precision 6 | 282.9 ns | 380.7 ns | **0.74×** | 0 / 200000 mismatch |
| `%e` precision 16 | 462.5 ns | 596.0 ns | **0.78×** | 0 / 200000 mismatch |

On realistic data the library **beats glibc on every formatting operation**, while
being exact. For shortest output glibc has no real equivalent — the closest is
`%.17g`, which is ~3.8× slower, produces *longer* strings on average, and is not
minimal.

### 3.3 Formatting — uniform bit patterns (extreme-magnitude heavy)

Random `binary64` bit patterns are dominated by huge/tiny exponents that are rare
in real data; this corpus stresses the exact big-integer path.

| operation | proven | host | ratio | accuracy |
|---|---:|---:|---:|---|
| shortest | 164.4 ns | 878.6 ns (`%.17g`) | **0.19×** | 0 round-trip failures; avg 16.42 vs 16.90 digits |
| `%f` precision 6 | 2695.1 ns | 635.1 ns | 4.24× | 0 / 200000 mismatch |
| `%e` precision 16 | 2828.4 ns | 881.9 ns | 3.21× | 0 / 200000 mismatch |

Shortest stays ~5× faster than glibc even here (Grisu3 is table-driven, not
magnitude-sensitive). Fixed `%f`/`%e` at extreme magnitudes is the one place the
library is slower: it does genuine arbitrary-precision arithmetic to be *exact*,
where glibc takes approximating shortcuts. The output is still bit-identical to
glibc on these cases, and such magnitudes are uncommon in practice.

### 3.4 Summary of the trade-off

- **Correctness is never traded away.** Parser bit-identical to `strtod`;
  `%f`/`%e` bit-identical to `snprintf`; shortest always round-trips and is minimal.
- **Faster than glibc** at: parsing typical numbers, shortest formatting (by 4–5×),
  and `%f`/`%e` at normal magnitudes.
- **Slower than glibc** at: parsing 16–17-digit numbers (~1.3×) and `%f`/`%e` at
  extreme magnitudes (3–4×), the price of being exact with no `long double`.

---

## 4. Portability and footprint

- The whole engine is integer-based — **no `long double` anywhere**, in the
  formatter or the parser, so it behaves identically on platforms where
  `long double == double` (MSVC, many ARM targets). The parser's exponent-bounds
  estimate used to go through `long double`; it now uses an integer fixed-point
  `floor(k * log2(10))` that is bit-identical to the exact value over the whole
  input range. That also means no libgcc soft-float routines get pulled in on a
  target without an FPU.
- No global state; thread-safe and reentrant (the exhaustive sweep ran 16 threads
  through the same code).
- Freestanding-friendly: no libm dependency in the conversion paths; the
  big-integer capacity (and hence the exact-fallback stack footprint) is tunable
  via `PROVEN_FLOAT_BIGINT_LIMBS` for embedded targets.
- Validated under `strict-error`, `freestanding`, ASan, and UBSan build gates.

---

## 5. Reproducing the results

The exhaustive sweep and the benchmark are standalone C programs that link the
library sources and the host C library as the oracle. The dated raw outputs are
kept in maintainer-local `docs/internal/` (kept outside the published repository)
(`*-f32-exhaustive-validation.md`, `*-f64-differential-validation.md`, and
`*-float-vs-host-benchmark.md`). To
re-run: compile the library sources at `-O2`, link the harness, and run — the
exhaustive sweep prints the failure table above (all zeros), and the benchmark
prints the tables in §3.
