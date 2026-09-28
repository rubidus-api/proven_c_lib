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
  glibc, single-threaded benchmarks (section 3, re-measured 2026-09-28 with the checked-in
  `tests/test_bench_float_host.c`), 16-thread exhaustive sweep.

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

**Re-measured 2026-09-28 with a checked-in harness**, `tests/test_bench_float_host.c`, run by
`./nob bench-float` (release profile, GCC 14.2, glibc, x86-64, single thread). Fixed-seed corpora
of 100,000 values; one warmup pass, then the median of five samples, each in the shared row
format with its spread (raw rows: `docs/benchmarks/2026-09-28-x86_64-linux.txt`). `ratio` is
proven / host, so **< 1.0 means the library is faster**. Accuracy is checked in the same run and
a single mismatch fails the benchmark: parse results bit-for-bit against `strtod`, `%f` and `%e`
byte-for-byte against `snprintf`, shortest output round-tripping through `strtod`. On this run:
0 mismatches in 300,000 parses, 400,000 fixed/scientific formats and 200,000 shortest formats.

The June 2026 tables this replaces came from a harness kept outside the repository, on another
machine; they cannot be re-run, which is why they are gone. The one conclusion that moved: they
reported `%f`/`%e` 3-5x SLOWER than glibc at extreme magnitudes, and the checked-in harness
measures them faster there too (table 3.3). Read that as "not reproduced here", not as a
guarantee for other machines.

### 3.1 Parsing (decimal -> binary64), bit-exact vs `strtod`

| input corpus | proven | host `strtod` | ratio |
|---|---:|---:|---:|
| short human decimals (`%.6g`) | 82.8 ns | 134.2 ns | **0.62x** |
| shortest round-trip (~16 digits) | 186.2 ns | 193.7 ns | 0.96x |
| hardest 17-digit (`%.17g`) | 223.5 ns | 197.1 ns | 1.13x |

Faster than glibc on short, human-sized numbers - the common case - level at ~16 digits, and
about 1.1x slower on 17-digit inputs.

### 3.2 Formatting - normal magnitudes (1e-6 ... 1e6)

| operation | proven | host | ratio |
|---|---:|---:|---:|
| shortest | 144.6 ns | 518.2 ns (`%.17g`) | **0.28x** |
| `%f` precision 6 | 205.3 ns | 345.1 ns | **0.59x** |
| `%e` precision 16 | 308.3 ns | 529.6 ns | **0.58x** |

For shortest output glibc has no equivalent; the nearest, `%.17g`, is ~3.6x slower and not
minimal.

### 3.3 Formatting - uniform bit patterns (extreme-magnitude heavy)

Random `binary64` bit patterns are dominated by huge and tiny exponents that are rare in real
data; this corpus stresses the exact big-integer path.

| operation | proven | host | ratio |
|---|---:|---:|---:|
| shortest | 169.5 ns | 908.7 ns (`%.17g`) | **0.19x** |
| `%f` precision 6 | 1652 ns | 4663 ns | **0.35x** |
| `%e` precision 16 | 731.9 ns | 907.1 ns | **0.81x** |

### 3.4 Summary of the trade-off

- **Correctness is never traded away.** Parser bit-identical to `strtod`; `%f`/`%e`
  bit-identical to `snprintf`; shortest always round-trips and is minimal.
- **Faster than glibc** here at: parsing short numbers, and every formatting operation at every
  magnitude measured.
- **Slower than glibc** here at: parsing 17-digit numbers (~1.1x).

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

The benchmark in section 3 is checked in (`tests/test_bench_float_host.c`) and runs with
`./nob bench-float`; its raw rows are under `docs/benchmarks/`. The exhaustive sweeps are
standalone programs that link the library sources and the host C library as the oracle; their
dated raw outputs are kept in maintainer-local `docs/internal/` (outside the published repository)
(`*-f32-exhaustive-validation.md`, `*-f64-differential-validation.md`). To re-run a sweep: compile
the library sources at `-O2`, link the harness, and run - it prints the failure table above (all
zeros).
