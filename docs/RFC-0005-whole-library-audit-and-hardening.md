# RFC-0005 - Whole-library audit and hardening

**Status:** proposed
**Date:** 2026-07-23
**Related:** [RFC-0004](RFC-0004-the-manual-as-a-book.md)

> **What this changes.** The library has broad tests and unusually detailed public
> documentation, but those assets do not by themselves prove every target, release profile,
> lifetime rule, or performance claim. This RFC turns the 2026-07-23 whole-tree audit into a
> staged hardening program. It records what was actually observed, distinguishes defects from
> verification gaps and performance hypotheses, and gives every proposed change a reproducer,
> measurement, compatibility review, and exit condition.
>
> The first audit fixes are already represented in the current tree: the documentation catalog
> no longer depends on POSIX directory APIs, the build manifest covers the previously omitted
> library/test/example headers and has a gate for its declared header roots, chunk scanning
> refuses an output view it cannot keep alive, float path metrics no longer create process-global
> writes, the build and test registries are compiled from shared manifests, and the catalog
> enforces exact source membership as well as duplicate and count consistency. Public build-path
> examples are repository-relative and checked by policy. The v26.07.23d follow-up also replaces
> idle job-worker polling with retained semaphore wakes; §7.4 separates that implemented
> mechanism from its still-open latency, scaling, and native-Windows qualification. The remaining
> work is intentionally split into independently testable phases.

---

## 1. Decision

Adopt a continuing whole-library hardening cycle with three rules:

1. A **confirmed defect** needs either a deterministic reproducer or a source-level proof that
   directly contradicts a documented contract.
2. A target that was skipped, unavailable, compile-only, or exercised through a mock is a
   **verification gap**, never a pass.
3. A possible speedup is a **performance hypothesis** until a checked-in benchmark measures the
   current implementation and the candidate on representative input sizes.

Correctness and claim accuracy come first. Low-risk performance work follows only after the
measurement exists. Scheduler and data-layout changes remain last because they have the largest
concurrency and compatibility surfaces.

RFC-0004 changed the manual from a header index into a book. This RFC does not reopen that
editorial design. It hardens the code, build, tests, and measurement machinery underneath the
manual's claims.

## 2. Scope and non-goals

The audit covers:

- every installed header and implementation translation unit;
- ownership, borrowed lifetimes, failure atomicity, bounds and checked arithmetic;
- partial I/O, filesystem replacement, path handling, and platform abstraction boundaries;
- global mutable state, thread shutdown, and release-profile behavior;
- test registration, cache dependencies, documentation gates, and target claims;
- algorithmic complexity, avoidable allocations, copies, system calls, and repeated parsing;
- whether published benchmark conclusions can be reproduced from checked-in programs.

This RFC does not:

- promise that an unavailable compiler or operating system passed;
- redesign a public API merely to make an internal optimization easier;
- replace deterministic correctness tests with wall-clock thresholds;
- land map-layout, scheduler, or parser rewrites without baseline data;
- treat an analyzer's silence as proof of correctness;
- optimize the already-deferred extreme-exponent `5^q` float path without differential fuzzing
  and path-specific measurements.

## 3. Audit baseline

The audit began from the `v26.07.23b` release state. The final phase-0 tree is
`v26.07.23c`, and the inventory below includes the focused float concurrency regression added
during the audit.

| Inventory | Current count | Derivation |
|---|---:|---|
| Installed public headers | **36** | 35 under `include/proven/`, plus `include/proven.h` |
| Core implementation translation units | **26** | `src/proven/*.c` |
| Platform translation units | **8** | `platform/*.c` |
| Total implementation translation units | **34** | 26 core plus 8 platform |
| Test source files | **121** | `tests/test_*.c`, including benchmark-only sources |
| Hosted tests in `all_tests[]` | **111** | current `nob.c` registry |
| Runnable manual examples | **22** | `manual/examples/*.c` |
| Normal hosted executables | **133** | 111 tests plus 22 examples |
| Focused regression subset | **29** | current `regression_tests[]` |
| Freestanding test subset | **5** | current `freestanding_tests[]` |
| Registered benchmarks | **3** | current `benchmark_tests[]` |
| Cross-only smoke sources | **2** | 1 compile smoke plus 1 link smoke |

The first inventory estimate counted 35 public headers. That number omitted the umbrella
`include/proven.h`; 36 is the reproducible current-tree count.

The 121 test sources are exactly 111 hosted, 5 freestanding-only, 3 benchmark-only, and
2 cross-only sources. Their filename classes currently total 121:

| Class | Files |
|---|---:|
| unit | 61 |
| contract | 13 |
| regression | 20 |
| differential | 4 |
| portability | 10 |
| stress | 1 |
| docs | 9 |
| bench | 3 |

The tree count and hosted registry count intentionally differ. Benchmarks have their own mode,
and a filename can belong to the tree without belonging to the normal hosted registry.

## 4. Evidence model

Every finding in this RFC carries one of these evidence levels:

| Level | Meaning | What it can establish |
|---|---|---|
| **E3 - reproduced** | A focused test failed before the change and passed after it, or a runtime tool reported the fault | A defect on the exercised configuration |
| **E2 - source proof** | A finite source path directly contradicts a contract or has a mechanically provable bound | A defect or complexity issue independent of timing |
| **E1 - target evidence** | A named compiler, linker, sanitizer, or analyzer completed a named command | Evidence only for that exact tool and configuration |
| **E0 - hypothesis** | Inspection suggests a cost, but benefit and tradeoff are unmeasured | A benchmark or experiment to run, not a reason to patch |

An E1 result does not generalize to another compiler or operating system. An E2 portability
finding can justify a fix even when the affected target is unavailable, but the target remains
unverified until it compiles and runs there. A performance mechanism can be E2 while its expected
speedup remains E0.

Severity is separate from evidence:

- **P0:** data loss, memory corruption, or security failure on an ordinary supported path;
- **P1:** undefined behavior, contract violation, or a major target/release claim that is false;
- **P2:** supported-build break, serious avoidable production cost, or verification that can
  silently give a false green result;
- **P3:** maintainability debt or a measured optimization with limited impact.

No P0 was found in this audit snapshot.

## 5. Commands actually exercised

The following table records commands run against the final audit tree: 111 hosted tests plus
22 examples, or 133 executables. A passing command establishes only the evidence boundary in
the rightmost column.

| Command or check | Observed result | Evidence boundary |
|---|---|---|
| `./nob build` | Passed, 34 sources and 133 executables | Hosted debug build on the available native compiler |
| `./nob strict-error` | Passed, 34 sources and 133 executables | Project warnings-as-errors set, native compiler only |
| `./nob release` | Passed, 34 sources and 133 executables | `-O3` profile on the native compiler; this does not close C-003 |
| `./nob asan` | Passed, 34 sources and 133 executables | AddressSanitizer on the native host |
| `./nob ubsan` | Passed, 34 sources and 133 executables | UndefinedBehaviorSanitizer on the native host |
| `./nob freestanding` | Passed, 21 sources and 5 tests | Freestanding compile/profile check; it still links a host runtime |
| `./nob tsan` | Passed, 34 sources and 133 executables | Includes the eight-thread float parser regression |
| `./nob cross` | Native GCC hosted passed; Clang, AArch64, ARM hard-float, i686, MinGW/Windows, and embedded targets skipped | A successful command with skipped targets is not a matrix pass; 1 of 11 named configurations passed |
| `scripts/project-check.sh` | Passed | Repository policy and documentation checks on this tree |
| `./nob bench-float` | All three registered benchmarks and their checksum checks passed | One timing run on one environment; not a threshold or comparative conclusion |
| Freestanding objects built with `-ffreestanding -nostdlib -fno-builtin`, then inspected with `nm -u` | Float objects retained unresolved hosted memory/string symbols | Direct proof that compile-only cross checks do not establish a no-libc link |
| GCC `-fanalyzer` over all source units | No diagnostics | Analyzer silence is supporting evidence only |
| Additional conversion/alignment warnings | Alignment casts and an mmap signed conversion were reviewed | No actionable defect was proved by these warnings |

The focused float concurrency regression reported the global-counter race against the pre-fix
implementation and passed after the counters became caller-owned. This demonstrates why
suite-level sanitizer success is not enough: TSAN cannot report a race in code that no test
invokes concurrently. The regression must remain in all future TSAN acceptance runs.

The one float benchmark run produced the following path observations in nanoseconds per parse.
They are preserved as audit evidence, not as stable performance claims:

| Corpus | proven parser | host parser |
|---|---:|---:|
| short exact | 30.54 | 78.16 |
| staged | 147.73 | 112.46 |
| exact fallback | 563.11 | 258.77 |
| boundary | 490.24 | 202.17 |
| mixed | 287.97 | 151.19 |

The primitive 4 KiB rows also completed successfully. The current harness produced one run, so
these observations have no recorded variance and cannot by themselves approve or reject an
optimization.

### 5.1 Targets not established by this audit

The audit environment did not establish any of these as passing:

- Clang hosted builds;
- AArch64, ARM hard-float, or i686 hosted builds;
- MinGW-w64 x86 or x64 builds;
- native MSVC or clang-cl builds;
- ARM or RISC-V embedded links;
- native Windows filesystem behavior;
- native macOS linking and runtime behavior.

An available `gcc -m32` driver could not compile against the missing multilib headers. The
separate i686 cross compiler and the AArch64, ARM hard-float, MinGW, embedded ARM, embedded
RISC-V, and Clang toolchains were unavailable. These are recorded gaps, not failures and not
passes.

## 6. Immediate findings fixed in the audit tree

### 6.1 Fixed defects and false-green gates

| ID | Severity / evidence | Finding | Containment and regression |
|---|---|---|---|
| F-001 | P1 / E3 | `proven_sysio_scan_chunk_impl` could return a string view into its local 4 KiB stack buffer. The view dangled as soon as the function returned. | The chunk API now rejects `PROVEN_SCAN_ARG_TYPE_STR_VIEW` before reading or seeking. The focused contract test proves the destination and file position are unchanged, then proves a scalar scan still succeeds. |
| F-002 | P1 / E3 | Decimal conversion path counters were process-global non-atomic writes. Concurrent public float parsing therefore had a C data race and forced unrelated cores to share one writable cache line. | Production conversion passes no metrics sink. Tests use an optional caller-owned sink through the internal observed conversion seam. The focused concurrency regression reproduced the old race under TSAN, covers Clinger, Eisel-Lemire, subnormal, and exact-fallback inputs, and passes in the final TSAN suite. |
| F-003 | P2 / E3 | `test_docs_test_catalog.c` included POSIX `dirent.h` unconditionally, so the complete suite could not compile with documented MSVC. | The test enumerates `tests/` through the library's portable filesystem iterator. The portability source-contract test forbids the POSIX dependency and requires the portable calls. |
| F-004 | P2 / E3 | The build dependency manifest omitted five internal headers. Editing one could leave stale objects or tests while the build reported success; a stale manifest path, a clean output tree, or a same-timestamp edit could also make timestamp-only checks misleading. | The manifest now includes `src/proven/float_decimal_tables.h`, `src/proven/proven_internal_memrange.h`, `tests/proven_test.h`, `tests/proven_test_fw.h`, and `manual/examples/example.h`. The build and gate include one preprocessed manifest with a compile-time sentinel. Every profile validates all declared paths before output checks, and source/header content fingerprints join the cache key, covering clean builds, coarse timestamp resolution, and restored mtimes. |
| F-005 | P2 / E3 | The catalog gate parsed raw `nob.c` text, could count disabled entries, could ignore duplicate registrations, did not prove every test source was registered, did not enforce all published totals, and left a 29-member regression subset documented as only nine paths. Its freestanding cross smoke also depended on registry position. | The build driver and gate now compile the same test registry manifest, including two explicit cross-only sources. The gate rejects duplicates, requires `regression_tests[]` to be a subset of `all_tests[]`, requires every test source to belong to the hosted, freestanding, benchmark, or cross-only registry, compares documented regression membership one-to-one, and derives all published totals from compiled registries and the filesystem. The freestanding smoke resolves a stable named path. |
| F-006 | P3 / E2 | Public examples and help text used absolute Unix home-directory paths even though the workspace privacy rule requires repository-relative public paths. The policy checker rejected only one machine-specific home path. | Public examples now use `build-out/proven_c_lib`. The repository check rejects Unix home-directory paths generically, so a different user name cannot bypass the rule. |
| F-007 | P3 / E2 | The ignore file hid arbitrary dotfiles and enumerated credential-like names and broad patterns, contrary to the workspace privacy convention and capable of hiding legitimate source. | It now ignores private material only through the standard `*_private/`, `*_internal/`, `secrets/`, and `.env` conventions. A post-change untracked scan found no newly exposed sensitive-like path. |

These changes are intentionally narrow. In particular, F-001 refuses an impossible borrowed
lifetime rather than hiding an allocation or changing the destination type, and F-002 keeps
metrics out of the production fast path rather than making every parse pay for atomics or TLS.

## 7. Confirmed unresolved issues

### 7.1 Windows atomic replacement does not replace

**ID:** C-001
**Severity / evidence:** P1 / E2
**Location:** `platform/proven_sys_fs.c`, `proven_sys_fs_rename`

The Windows branch calls `MoveFileW`. That API fails when the destination already exists. The
POSIX branch uses `rename`, and the public atomic-write behavior expects a completed write to
replace an existing destination. The Windows source therefore contradicts the cross-platform
contract.

**Fix direction:** use a Windows replacement primitive with explicit replace-existing
semantics. Evaluate `MoveFileExW` with `MOVEFILE_REPLACE_EXISTING` versus `ReplaceFileW` for the
required durability and metadata behavior. Preserve same-volume temporary-file placement and
cleanup on every failure.

**Required regression:** on native Windows, begin with an existing destination, atomically write
different contents, verify the new complete contents, and verify that no temporary file remains.
Inject replacement failure and prove the old destination remains intact.

**Exit condition:** the native Windows regression and ordinary atomic-write regression pass
under the supported Windows compiler lanes.

**Compatibility risk:** medium. Windows replacement APIs differ in metadata, sharing, and
write-through behavior. The chosen contract must be documented rather than inferred from POSIX.

### 7.2 The freestanding gate still depends on hosted memory routines

**ID:** C-002
**Severity / evidence:** P1 claim gap / E2
**Locations:** `src/proven/float_decimal.c`, `src/proven/float_format.c`, freestanding build mode

The float implementation includes and calls `memcpy`, `memmove`, `memset`, and `strlen`.
Compiling with `-ffreestanding` does not prove a no-CRT link: the current freestanding tests are
linked against the host runtime. This was checked directly by compiling with freestanding,
no-standard-library, and no-builtin options and inspecting the objects with `nm -u`:
`float_decimal.o` retained unresolved `memcpy`, `memmove`, and `memset`, while
`float_format.o` retained unresolved `memcpy` and `strlen`. Embedded compile-only probes do not
expose that link failure.

**Fix direction:** first define the exact freestanding runtime contract. Either route the
required operations through the existing platform/memory layer or provide small internal
implementations that the freestanding set can link. If a minimal compiler runtime is
intentionally required, name and test that requirement.

**Required regression:** compile freestanding objects with builtins disabled, inspect unresolved
symbols against an explicit allowlist, and link a minimal smoke program without the hosted C
library. Exercise the float boundary values already covered by hosted differential tests where
the embedded target can run them.

**Exit condition:** the no-hosted-CRT smoke link succeeds with no unapproved unresolved symbols,
or the documentation narrows the claim to the runtime actually required.

**Compatibility risk:** medium. Replacing compiler-recognized memory calls can reduce code
quality; retaining them changes the stated freestanding contract.

### 7.3 The normal optimized build keeps quadratic pool validation

**ID:** C-003
**Severity / evidence:** P2 / E2
**Location:** `src/proven/pool.c`

Pool free scans every cached pointer to detect a duplicate when either `PROVEN_HARDENED` is true
or `NDEBUG` is absent. The named `release` mode uses `-O3` but does not define `NDEBUG`.
Freeing N pooled objects into the cache therefore performs a total O(N squared) duplicate
search. The final release suite passed, but no comparison-count or scaling gate exercises this
complexity, so that pass does not close the issue.

**Fix direction:** define and document explicit debug, release, and hardened profiles. Release
should remove the linear duplicate scan; hardened should keep it deliberately. Do not make
optimization level silently choose safety semantics.

**Required regression and measurement:** a source/build contract test verifies the intended
defines in each mode. A deterministic counter or benchmark records comparisons for increasing
pool sizes and demonstrates linear total release behavior while hardened mode still detects a
double free.

**Exit condition:** profile names and flags are visible in the build plan, release total free
work scales linearly, and debug/hardened diagnostics retain their tests.

**Compatibility risk:** medium. Programs accidentally relying on debug panic behavior in an
optimized build will observe a change; the release profile must be opt-in or clearly versioned.

### 7.4 Idle job parking is implemented; full qualification remains open

**ID:** C-004
**Severity / evidence:** P2 / E3 on the measured POSIX host; E2 source evidence on Windows
**Location:** `src/proven/job.c`, `platform/proven_sys_thread.c`

The audited implementation repeatedly checked an empty queue and called
`proven_sys_thread_yield`. Yielding left every idle worker runnable. v26.07.23d replaces that loop
with a PAL counting semaphore: POSIX uses a mutex/condition-variable permit count and Windows
uses a kernel semaphore.

The wake protocol is deliberately counted rather than edge-triggered. A successful submission
publishes its queue cell before posting one permit, so the permit remains available if no worker
has reached its wait. The first close stops admission, waits for all submitters that already
entered to finish committing or rejecting their work, then posts one final permit per worker.
An external `proven_job_execute_one` can consume a job before a worker consumes its permit; that
stale permit causes an empty recheck and is safe. Only the first concurrent closer posts the
final permits.

**Implemented regression evidence:**

- a source-contract test failed against the polling implementation and now requires wait/post
  calls plus both POSIX and Windows PAL branches;
- the unit test lets workers park before first submission, closes a completely idle system, and
  deterministically creates stale permits with an external consumer;
- the stress test closes while four producers are active, then proves every accepted job ran
  exactly once and no job ran twice;
- both focused job executables completed 100 consecutive runs with per-run timeouts;
- the complete 133-executable TSAN suite passed.

A five-run local process-CPU comparison used eight workers and a 250 ms idle window:

| Implementation | Mean process CPU during the idle window |
|---|---:|
| former poll + yield loop | 2000.081 ms |
| counting-semaphore parking | 0.032 ms |

This is strong evidence for the intended effect on the measured POSIX configuration, but it is
not a portable performance result. The temporary comparison does not contain the worker-count or
wake-latency matrix required by this RFC.

**Required remaining measurement:** check in a reproducible benchmark with raw samples for
1, 2, 8, and 32 workers; record median and p99 submit-to-start latency under idle, burst, and
saturated loads plus saturated multi-producer submission throughput; define the accepted
latency and throughput budgets; and run the focused wake/close tests on native Windows.

**Exit condition:** no accepted job is lost on each supported runtime, TSAN remains clean,
steady idle CPU is close to each platform's parked-thread baseline, and the accepted
wake-latency budget is documented.

**Compatibility risk:** medium after implementation. The public function signatures did not
change, but scheduler timing did and the internal PAL grew. On POSIX each accepted submit now
briefly enters the semaphore mutex to add a permit and signal one waiter; the atomic ring remains
nonblocking with respect to queue capacity, but the complete submit operation is not wait-free.
A missed or surplus wake can become a permanent hang or excess rechecks, which is why B-038
remains open until throughput, the full target matrix, and the latency matrix are recorded.

## 8. Verification and portability gaps

The following items are not all user-visible failures on the native host, but each can make a
published support claim or green build misleading.

| ID | Gap or source concern | Required closure |
|---|---|---|
| V-001 | Five other tests still include or require POSIX-only facilities without a native MSVC implementation: `test_docs_alias_completeness.c`, `test_docs_manual_examples.c`, `test_docs_manual_symbols.c`, `test_unit_fs_position_and_sync.c`, and `test_portability_nob_std_probe.c`. | Port them through project abstractions or provide small Windows branches, then compile the complete test tree with MSVC or clang-cl. |
| V-002 | `nob.c` emits GCC/Clang-style options and does not constitute a native MSVC build driver. Source tests that inspect `_MSC_VER` branches are not a compiler lane. | Add a real MSVC or clang-cl lane with compile, archive, link, and run stages, or narrow the documented compiler support. |
| V-003 | Windows random fill narrows a `size_t` request to one `ULONG` call. A request above the Windows API count range can be short-filled while the wrapper reports success. | Chunk requests to the API's maximum count and add a fake/backend boundary test plus native smoke coverage. |
| V-004 | Windows symlink creation does not select the directory flag, and absolute normalization can change the meaning of relative link targets. | Specify file/directory and relative-target semantics, add both kinds of native tests, and preserve the caller's relative target when required. |
| V-005 | Every non-Windows hosted test link receives `-ldl`. That is Linux-oriented and was not validated on macOS. | Link `dl` only where the target and fault-injection tests require it; run a native macOS lane. |
| V-006 | The executable cache hash is derived before the real non-Windows `-ldl` addition in the link path. A link-option change can reuse a stale executable. | Hash the exact final command and add a build-driver test that changes one link option and requires relinking. |
| V-007 | Build-root validation rejects Windows drive syntax and backslashes, while `clean` does not consistently own a caller-selected build root. | Define path forms per host, test drive/UNC-style inputs without exposing machine paths, and make clean operate only on the explicitly selected safe root. |
| V-008 | Source and freestanding manifests remain duplicated by hand. The header dependency manifest is now shared with its gate, but still rebuilds broadly. | Move toward one source manifest and compiler-generated dependency files; retain a deterministic manifest-consistency gate. |
| V-009 | `include/proven.h` receives panic declarations transitively rather than directly including `proven/panic.h`. | Add the direct include and a generated completeness check so the umbrella does not depend on an unrelated header's transitive includes. |
| V-010 | The cross command can exit successfully after skipping most of its named matrix. | Print and machine-check passed, failed, and skipped target counts; release policy must require named mandatory targets. |
| V-011 | The freestanding mode is compile-profile evidence, not a no-runtime link proof. | Close C-002 and label compile-only probes explicitly in logs and documentation. |
| V-012 | The English-ASCII rule is not mechanically enforced, and historical public documents still contain non-ASCII punctuation even though audit additions were normalized. | Normalize the remaining English public text deliberately, then add a gate that excludes Korean mirrors and rejects new non-ASCII bytes in English docs and comments. |

## 9. Performance opportunities that require measurement

The mechanisms below are visible in source. None of the expected speedups is accepted as fact
until the proposed experiment is checked in and run. Correctness tests must use stable counters,
checksums, allocation records, or call counts; elapsed time remains benchmark output.

| ID | Mechanism seen in current code | Measurement before a change | Candidate direction and risk |
|---|---|---|---|
| P-001 | POSIX directory iteration can call `fstatat` twice for one ordinary entry. | Count metadata syscalls across regular files, directories, and symlinks on cold and warm trees. | Reuse the no-follow result where it fully determines the entry. Low risk, but preserve symlink-following semantics. |
| P-002 | One sysio scan allocates two heap blocks. | Count allocations and bytes for scalar, string, and mixed scans at common argument counts. | Use a small stack path or one combined allocation. Medium lifetime/alignment risk. |
| P-003 | Buffered stream scanning can reparse a token from its start after every short read. | Instrument bytes examined for one token delivered 1, 2, 4, and 64 bytes at a time. | Retain parser frontier/state or compact only the confirmed prefix. High rollback and grammar risk. |
| P-004 | Line reading can rescan the already examined prefix after short fills. | Count inspected bytes for long lines under one-byte and geometric chunking; require a linear bound. | Store a scan frontier. Low to medium boundary/CRLF risk. |
| P-005 | `proven_scan_skip_until` uses a naive O(N times M) delimiter search. | Comparison counts over delimiter lengths and adversarial repeated prefixes. | Reuse the tested `proven_u8str_view_find` machinery if semantics match. Medium incremental-state risk. |
| P-006 | The long substring-search threshold is described as benchmark-chosen, but no reproducible size/alphabet matrix is tracked. | Add short, medium, long, ASCII, UTF-8 byte, repeated-prefix, hit-position, and miss cases. | Tune the threshold or algorithm only from that matrix. Low API risk. |
| P-007 | SHA-256 update feeds input byte by byte even when complete blocks are available. | Throughput for 0..128-byte boundaries, 4 KiB, 1 MiB, aligned/unaligned, and incremental chunks. | Compress complete input blocks directly, retaining buffered prefix/suffix handling. Medium digest-boundary risk. |
| P-008 | Growable formatting renders at least twice; a custom formatter can be invoked up to four times. | Count formatter callbacks and allocations for fitting, one-growth, large, and failure cases. | Add sizing support or reserve heuristics without changing failure atomicity. Medium callback-side-effect compatibility risk. |
| P-009 | Map entries use an array-of-structures payload stride, and hardened validation walks the full structure. | Lookup/insert/remove throughput and cache misses across key/value widths, load factors, churn, and hardened/release modes. | Consider split metadata/payload or narrower validation only after profiles. High layout, ABI, and complexity risk. |
| P-010 | Filesystem path conversion allocates and copies every path; Windows performs additional heap conversions and full-path work. | Allocation count and latency by path length and operation, with short paths dominant in the corpus. | Add a bounded stack fast path with heap fallback. Medium encoding and long-path risk. |
| P-011 | File copy repeats path conversion and metadata queries. | Count conversions, opens, stats, reads, and writes for small, large, and self/hardlink cases. | Share validated handles/metadata while retaining self-copy and TOCTOU protections. Medium correctness risk. |
| P-012 | Format specification parsing makes two passes. | Bytes examined and total format time for literal-heavy, argument-heavy, invalid, and reused formats. | Fuse validation/execution or add an explicit compiled-format object. High error-offset and API risk. |
| P-013 | Ring indexing uses division remainder for every wrap. | Producer/consumer throughput across power-of-two and arbitrary capacities, with compiler assembly review. | Cache a mask only for power-of-two capacities, or rely on branch wrap if faster. Low to medium invariant risk. |
| P-014 | Large environment values require a query allocation followed by an owned result allocation. | Allocation count and bytes for missing, short, boundary, and large values. | Reuse or transfer a temporary buffer where ownership permits. Medium platform-contract risk. |
| P-015 | Extreme decimal exponents build exact powers of five in a path already deferred by the backlog. | Differential fuzzing against the host/oracle plus path-specific counts and latency for extreme exponents. | Cache or restructure `5^q` only if the path is material and exact rounding remains proved. High correctness risk; keep deferred by default. |

### 9.1 Existing benchmark trust gaps

The tree has three registered benchmarks, but their coverage is narrower than some prose claims:

- the primitive benchmark uses a fixed 4 KiB buffer and does not cover short inputs,
  block boundaries, incremental updates, or size scaling;
- benchmark documentation describes a median-of-three style conclusion while the current
  checked-in harness does not retain three raw samples and their variance;
- no checked-in CSV or JSON artifact preserves raw runs, toolchain identity, spread, or warmup;
- the CRC slice-by-8 statement is a projection, not a checked-in implementation comparison;
- the statement that ChaCha20's observed 4 KiB ratio is the general "price" of security extends
  one machine and one size too far;
- the public float document includes formatter performance from a separate harness, while
  `./nob bench-float` currently registers parse and primitive programs, not a formatter
  benchmark;
- `job.h` describes low overhead without an idle-CPU or wake-latency benchmark.

The remedy is not to delete useful historical numbers. Label each result with its scope, retain
raw samples in a compact text format, and make every current headline reproducible by a
checked-in command.

## 10. Phased implementation plan

Each phase must leave the normal build green and may be committed independently. A phase is not
complete merely because a command was attempted.

### Phase 0 - close and verify the audit's immediate fixes (completed in v26.07.23c)

**Work**

- retained F-001 through F-007 and their focused tests;
- ran the new concurrent float regression under TSAN;
- reconciled the 121-file, 111-hosted, 22-example, 133-executable, and 29-regression counts;
- updated the change log, version surfaces, and RFC-0004 implementation status.

**Regression/measurement completed:** focused sysio lifetime test, float path-observation unit
test, concurrent float regression, compiled-registry catalog test, dependency-manifest
portability test, cache-content checks, and repository privacy/ignore-policy checks.

**Exit evidence:** the library implementation passed `build`, `strict-error`, `release`, ASan,
UBSan, TSAN, and freestanding. The later cache/manifest and documentation hardening changed no
library implementation; the resulting tree then passed a full `strict-error` run, freestanding,
the project policy check, and the available native-GCC cross lane. Every unavailable target
remains named as a gap.

**Compatibility risk:** low. The only user-visible behavior is that chunk scanning now returns
`PROVEN_ERR_UNSUPPORTED` before consuming input when asked for a borrowed string view whose
lifetime cannot be honored.

### Phase 1 - close target-specific correctness defects

**Work**

- implement and test Windows atomic replacement (C-001);
- chunk Windows random requests (V-003);
- define and correct Windows symlink file/directory and relative-target behavior (V-004).

**Regression/measurement:** fake-backend boundary tests where useful, followed by native Windows
compile and runtime tests for replacement, failure atomicity, random-fill boundaries, file
symlinks, directory symlinks, and relative targets.

**Exit:** supported Windows lanes compile and run the complete focused set; existing destination
replacement is atomic at the documented level; no short random fill is reported as complete.

**Compatibility risk:** medium. Filesystem metadata and symlink semantics must be specified
before choosing API flags.

### Phase 2 - make portability claims mechanically true

**Work**

- close the no-hosted-CRT link gap (C-002);
- port or branch the five remaining POSIX-only tests (V-001);
- add a real MSVC or clang-cl lane, or narrow the support statement (V-002);
- make `-ldl` target- and test-specific (V-005);
- make cross output and release requirements distinguish pass, fail, and skip (V-010).

**Regression/measurement:** no-CRT unresolved-symbol and smoke-link gates; complete test-tree
compile on the Windows compiler; native macOS link/run; named cross-target result assertions.

**Exit:** each published target has a named compile/link/run level, and release output cannot be
green when a mandatory target was skipped.

**Compatibility risk:** medium. This phase can narrow a claim if infrastructure cannot support
it, but must not silently broaden one.

### Phase 3 - make build and release behavior explicit

**Work**

- make the existing debug, release, and sanitizer profile semantics explicit, and add a
  separately named hardened profile;
- remove quadratic pool validation from the intended release profile (C-003);
- hash the exact final link command (V-006);
- correct build-root and clean ownership rules (V-007);
- consolidate manifests and adopt compiler dependency files where available (V-008);
- make the umbrella header include every promised public module directly (V-009).

**Regression/measurement:** profile-definition tests, pool comparison counts, relink-on-option
change, safe build-root fixtures, incremental header-touch tests, and standalone umbrella
compilation.

**Exit:** a build log identifies its safety semantics; touching any consumed header rebuilds the
right outputs; changing a link flag relinks; clean never removes an unselected path.

**Compatibility risk:** medium for profile semantics, low for cache and manifest fixes.

### Phase 4 - establish trustworthy performance baselines

**Work**

- extend the primitive corpus across sizes and update boundaries;
- add formatter, stream short-read, filesystem allocation/syscall, pool, and job benchmarks;
- record warmup, at least three raw samples, median, spread, compiler, mode, and corpus checksum;
- add deterministic counters where algorithmic complexity can be tested without timing.

**Regression/measurement:** the benchmark harness itself receives tests for sample count,
checksum stability, and machine-readable output. Historical prose is relabeled where its scope
is narrower than its wording.

**Exit:** every active performance headline maps to a checked-in program and reproducible row;
every proposed optimization below has a baseline.

**Compatibility risk:** low. Benchmark output format changes should be versioned if external
automation consumes it.

### Phase 5 - land low-risk measured improvements

**Candidate order**

1. line-reader scan frontier (P-004);
2. reused POSIX directory metadata (P-001);
3. `skip_until` search reuse (P-005);
4. SHA-256 complete-block update (P-007);
5. short-path stack conversions (P-010);
6. reduced sysio scan allocation (P-002);
7. file-copy metadata/path reuse (P-011);
8. environment allocation reduction (P-014);
9. ring wrap specialization (P-013).

**Regression/measurement:** each patch lands with its boundary/differential test, deterministic
work counter where possible, before/after raw benchmark rows, and a code-size check for table or
fast-path growth.

**Exit:** correctness gates remain unchanged or stronger; the target corpus improves outside
noise; no important corpus regresses without an explicit accepted tradeoff.

**Compatibility risk:** low to medium per item. Revert an optimization independently if its
benefit does not survive another supported compiler.

### Phase 6 - evaluate high-risk concurrency and layout work

**Work**

- finish latency, scaling, and native-Windows qualification for the implemented
  idle-worker parking protocol (C-004);
- evaluate incremental scanner state (P-003);
- evaluate formatter parsing/caching and callback count (P-008, P-012);
- evaluate map metadata/payload layout only from representative profiles (P-009);
- tune substring search only from the tracked matrix (P-006);
- revisit exact `5^q` work only if P-015 shows material cost.

**Regression/measurement:** TSAN stress and accepted-job accounting for jobs; byte/callback
counters and rollback suites for scanners/formatters; differential and allocator-failure tests
for layout changes; ABI review for any public structure.

**Exit:** each design has a separate implementation note or RFC when it changes public layout or
semantics, wins its target workload, and passes all failure-injection and sanitizer modes.

**Compatibility risk:** high. No items in this phase are bundled together.

## 11. Release acceptance matrix

Before declaring this RFC implemented, record one line for every required cell:

| Area | Required evidence |
|---|---|
| Native hosted | debug, strict-error, release, and hardened full suites |
| Sanitizers | ASan, UBSan, and TSAN with the focused concurrent regressions |
| Freestanding | compile profile plus no-hosted-CRT smoke link |
| Windows | supported compiler build plus filesystem/runtime focused tests |
| macOS | native compile, link, and hosted smoke run |
| Cross targets | explicit pass/fail/skip table; all mandatory release targets pass |
| Documentation | symbol, example, claim, catalog, version, and manual gates |
| Performance | raw before/after samples, checksum, spread, compiler, and build mode |
| Privacy/release | final diff and tracked-file scan contain no private environment data |

If infrastructure is unavailable, the corresponding row remains open. The release note may say
"not verified"; it may not translate a skip into support evidence.

## 12. Completion criteria

RFC-0005 can move from `proposed` to `implemented` only when:

1. C-001 through C-004 are fixed or the affected public claim is explicitly narrowed.
2. Every P0-P2 item has a focused regression, compile proof, or measurable acceptance test.
3. The Windows, macOS, MSVC/clang-cl, cross, and freestanding rows report exact evidence rather
   than a single aggregate green status.
4. Release pool behavior is explicit and no longer accidentally quadratic.
5. Idle job workers park, or the API explicitly documents and justifies a measured polling
   design.
6. Every accepted performance change includes reproducible before/after data; unmeasured items
   remain labeled hypotheses.
7. RFC-0004 records its actual first-pass outcome and points here for post-rewrite hardening.
8. The final source, test, manual, backlog, change log, and visible version counts agree.

The expected result is not a claim that the library has no bugs. It is a system in which a
correctness claim has an executable witness, a target claim names the target that ran, and a
performance claim can be reproduced before it drives code.
