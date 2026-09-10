# RFC-0006 - Security and boundary correctness follow-up

**Status:** implemented on branch `rfc-0006`, pending owner review and merge. The two Windows
rows have no runtime result.
The two decisions the RFC reserved for the owner were taken as the RFC's own first option
and are marked below; both are one-line reversals. See section 8 for the closure table.
**Date:** 2026-09-09
**Baseline:** `7d6b46729d8e501ac5935b2ac6859152d6b33ecd` (public version `0.0.1`)
**Related:** [RFC-0005](RFC-0005-whole-library-audit-and-hardening.md),
[tracked backlog](BACKLOG.md)

## 1. Purpose and decision

This is an implementation specification, not a claim that the library has been
exhaustively verified. It extends RFC-0005 with four independently actionable
boundary/security defects found in the current source and concrete closure work
for two previously recorded Windows defects. No production source or registered
test is changed by this RFC. Existing passing tests do not cover the reproductions
below. Historical RFC-0005 pass results are not fresh results for this baseline.

Approve separate test-first changes in this order: H-002, H-001, H-004, H-003,
then the native Windows work H-005/H-006. Do not combine allocator, parser, queue,
and filesystem redesigns into one patch. Keep public signatures unless this RFC
explicitly identifies a decision; get that decision before implementation.

### Severity and evidence

- P1: urgent confidentiality or bounds failure, conditional on the stated input
  or environment. None of these is a universal P0 release emergency.
- P2: correctness defect requiring a specific path, counter boundary, or target.
- E3: a focused local executable reproduced the observation.
- E2: source-level proof; affected target or full-size scenario not executed.
- Gap: neither a demonstrated failure nor a passing target.

| ID | Priority | Evidence | Work item | Relationship to RFC-0005 |
|---|---|---|---|---|
| H-001 | P1 | E3 sizing helpers; E2 encoder capacity bypass | Check encoding arithmetic before memory access | New |
| H-002 | P1 | E3 POSIX temp-file descriptor retention | Create private staging files with restrictive initial permissions | New; earlier permission fix remains incomplete |
| H-003 | P2 | E3 Linux path reproduction | Do not interpret a POSIX backslash as a separator | New |
| H-004 | P2 | E3 UBSan with seeded valid queue boundary state | Remove signed overflow in queue sequence comparisons | New |
| H-005 | P1 | E2, Windows runtime unverified | Replace existing destinations atomically on Windows | Existing C-001 / B-033 |
| H-006 | P1 | E2, Windows runtime unverified | Fill every requested entropy byte on Windows | Existing V-003 / B-033 |

## 2. Audit coverage and limits

The review sampled ownership, arithmetic, and error paths in `arena.c`, `pool.c`,
`array.c`, `buffer.c`, `ring.c`, `u8str.c`, `u16str.c`, `map.c`, `mmap.c`,
`encode.c`, `random.c`, `stream.c`, `job.c`, and `fs.c`, together with their
relevant public contracts and memory/filesystem/random PAL paths. This is not a
line-by-line proof of every listed module. Float conversion, formatting/scanning,
hash algorithms, time, and the remaining platform branches received baseline-suite
coverage or prior-RFC comparison, not a fresh exhaustive algorithm audit.

Fresh local evidence: GCC 14.2.0, Linux, 64-bit `size_t`.

| Check actually run | Result | Limit |
|---|---|---|
| `cc nob.c -o nob` then `./nob build` | Pass; 34 cached source objects, 192 cached test/example executables executed | Debug native suite, not a clean rebuild or new sanitizer matrix |
| Appendix A observer | Encoding helpers returned zero; temp was initially 0644 and retained fd read NEW; durable backslash write returned error after publishing NEW | Real local syscalls with linker interception; not a separate-user exploit test |
| Appendix B queue probe with UBSan | Fail as expected: signed subtraction overflow at `job.c:262` | Controlled queue state, not billions of submissions or a whole-suite UBSan run |

`scripts/project-check.sh` passed for this documentation change, including the
documentation, example-parity, privacy, and whitespace checks. Do not infer Windows, macOS, 32-bit,
embedded, ASan, TSAN, or fresh full UBSan success from the table above.

### Explicit non-findings and deferred work

- An arena/pool realloc with a *different* alignment is not a valid regression:
  `include/proven/allocator.h` requires the original alignment. Do not broaden
  allocator semantics to fix a caller-contract violation.
- Aliased string replacement that shifts the tail is expressly forbidden in
  `include/proven/u8str.h`. Do not treat rejection of that input as a new defect.
- A mocked oversized view does not prove an attacker can allocate that view on a
  real 64-bit process. H-001 separates arithmetic proof from exploitability.
- RFC-0005 C-002/C-003, V-001/V-002/V-004..V-012 and B-034..B-038 remain independent
  follow-up work. Recheck source and target availability before claiming closure.
- File-copy pathname identity races, Windows path-conversion races, map entropy
  fallback policy, and long-running semaphore permit accumulation deserve targeted
  follow-up, but this RFC does not claim reproduced exploits for them.
- Documentation workflow mismatch observed during this task: adding an
  `Unreleased` changelog section makes `test_docs_version_sync.c:176` fail,
  although `docs/operations/README.md` requests that section. The trial
  changelog addition was removed; this RFC does not change release history.
  Before landing behavior changes, reconcile that gate with the changelog
  policy (skip Unreleased when checking the newest released version), or use
  an approved coordinated release/version update. Do not bump a release solely
  to make this proposed RFC pass.
- No benchmark-supported performance change is proposed. Retain RFC-0005's
  measurement requirements instead of guessing at optimizations.

## 3. H-001: encoding arithmetic can understate the output size

### Source and trigger

`src/proven/encode.c`: `proven_hex_encoded_size`, `proven_hex_encode`,
`proven_base64_encoded_size`, `proven_base64_decoded_size`, and
`base64_encode_impl`. The public contract in `include/proven/encode.h` promises
that insufficient capacity is rejected without a truncated write.

Let M be `SIZE_MAX`. The current helpers return zero for:

- hex encoded size at `M / 2 + 1` (`n * 2` wraps);
- Base64 encoded size at M (`n + 2` wraps);
- Base64 decoded upper bound at M (`n + 3` wraps).

The encoder itself is not protected merely by fixing the helpers: hex repeats
`data.size * 2`; Base64 computes `full * 4` and tail additions unchecked. A
wrapped `need` can pass `need > out_cap`, followed by output writes inconsistent
with the actual capacity. The encode path must reject unrepresentable output
before touching input or output. For representable real input this is a bounds
failure; large valid inputs are more plausible on 32-bit than 64-bit targets.
The helper failures alone require no allocated input and are fully reproducible.

### Proposed implementation

1. Introduce internal checked exact-size calculations, using `PROVEN_CKD_MUL`
   and `PROVEN_CKD_ADD`. Hex checks `n * 2`. Base64 checks `(n / 3) * 4` and
   then adds 0/2/3/4 according to remainder and padding mode.
2. In both encoders, compute and validate the exact size before any byte access.
   Return `PROVEN_ERR_OVERFLOW` for an unrepresentable size; preserve current
   `PROVEN_ERR_OUT_OF_BOUNDS` for a representable size exceeding capacity.
   Keep `written_out == 0` on either error and leave the output untouched.
3. Compute the decoded upper bound without overflowing the round-up addition:
   `(n / 4) * 3 + (n % 4 != 0 ? 3 : 0)`. Its maximum fits in `size_t`.
4. Public size helpers cannot return an error today. Proposed compatible
   signature policy: return `SIZE_MAX` on *encoded-size* overflow, document
   that sentinel, and ensure encoders independently reject overflow even if
   passed that capacity. Valid hex and padded Base64 output sizes cannot equal
   `SIZE_MAX`. Do not use zero as the sentinel because it also means empty input.
   Owner decision required: accept this policy or add result-returning checked
   helpers plus aliases/examples, retaining documented legacy wrappers.
5. Use remaining-length or full-group-count loop guards instead of `i + 3` or
   `i + 4` where addition itself could wrap. Do not change Base64 alphabet or
   padding acceptance as part of this arithmetic fix.

### Tests and exit criteria

Extend the existing encode unit tests first. Use helper-only values at 0..6,
`M/2 - 1`, `M/2`, `M/2 + 1`, `M-3` through M, and the padded/unpadded largest
representable output transitions. Derive expected results with checked arithmetic
or division, not by copying the old expressions. Add exact-capacity and one-byte
short outputs with canaries and `written_out` assertions for all three encoders.

For impossible-size refusal, a deliberately synthetic view may be used only as
an early-validation probe: label it as such, isolate it under ASan, and require no
memory access. Do not describe it as a valid allocated giant object. Prefer a
checked-size seam to test boundary arithmetic without fabricated storage.

Exit: all size calculations remain correct on native and 32-bit lanes; no
capacity decision uses a wrapped value; refusal is write-free; ordinary vectors
remain byte-identical; public errors/sentinel semantics are documented in the
headers, English/Korean manuals, aliases if added, and changelog.

## 4. H-002: chmod before writing does not revoke an already-open fd

### Source and threat model

`src/proven/fs.c:internal_write_file_atomic` opens a predictable `.pvtmpNN`
sibling through `proven_fs_open`, then chmods it to the target's mode. On POSIX,
`platform/proven_sys_fs.c:proven_sys_fs_open` creates with `0666`, filtered by
umask. With umask 0022, the temp is initially 0644. A different local user able to
traverse the directory can open it read-only before chmod and keep that descriptor.
Later chmod to 0600 does not invalidate that open descriptor. New private contents
are then readable through it, even though the file mode was narrowed before the
first payload write. An attacker does not need directory write permission.

Appendix A intercepts the real temp creation, observes mode 0644, opens a retained
reader, resumes the library, and reads NEW after the successful private rewrite.
It runs as one user: the cross-user authorization argument comes from the observed
other-readable mode and ordinary POSIX open-fd semantics, not a tested second UID.

New-destination `proven_fs_copy` follows the same create-then-chmod pattern, so
include it in the remedy rather than declaring atomic writes the only consumer.
An existing readable destination may already have readers; retroactively revoking
those readers is not a guarantee this library can provide.

### Proposed implementation

1. Add a private PAL open-with-create-mode operation or an explicit internal
   restrictive-create option. Pass 0600 *to the initial creation syscall*,
   together with `O_CREAT | O_EXCL` for the staging file. Do not toggle the
   process umask: it is shared mutable process state.
2. Preserve the public `proven_fs_open` defaults. Migrate atomic/durable staging
   and new copy destinations to the restrictive path. Audit every call site of
   the new helper; do not blanket-change public new-file permissions.
3. Apply final permissions through the opened handle (`fchmod` in the PAL),
   before final sync/rename. Avoid re-resolving the staging pathname for chmod.
   Carry the target mode for replacement. For a new atomic target retain the
   documented default-mode behavior with a concurrency-safe design, or obtain
   approval to document restrictive new-file defaults. Do not invent a
   thread-safe umask query by temporarily setting umask to zero.
4. Never proceed with private payload writing after a required permission or
   metadata lookup failure. Distinguish missing target from failed metadata
   retrieval: current boolean stat results lose that distinction. Add PAL error
   information where needed, without OS calls in `src/proven/`.
5. Preserve exclusive creation, cleanup, close-error propagation, and old-target
   preservation on failures before rename. Permission design on Windows must
   consider ACLs rather than pretending POSIX mode bits establish equivalent
   confidentiality; native ACL qualification remains a separate target result.

### Tests and exit criteria

Replace the observational hook with a regression that checks initial mode has
no group/other access before permitting execution to proceed. Cover atomic and
durable replacement of 0600 targets and copying a 0600 source to a new path,
umasks 0000/0022/0077, open/chmod/write/sync/close failures, and temp collisions.
Use syscall interception/barriers rather than a probabilistic watcher loop.
A second-user integration test may be skipped when unavailable; report the skip.

Verify final public modes as well as initial private modes. Failure before commit
must preserve old contents and remove only the owned temp. State explicitly that
hostile directory writers, existing authorized readers, ACL preservation, and
secure erasure require separate guarantees; restrictive creation alone does not
solve every filesystem race. Exit: no readable-before-chmod staging window and
no global-umask mutation, with all compatibility decisions documented.

## 5. H-003: POSIX durable writes sync the wrong parent for backslashes

`src/proven/fs.c:internal_parent_dir` treats both slash and backslash as separators
on every platform. POSIX allows a backslash inside a basename. A durable write to
`build/rfc-0006/a\b` creates and renames the correct file, but attempts to sync
`build/rfc-0006/a`, which is not its parent. If that path is absent, the operation
returns an I/O error *after* NEW is already visible. If it is an existing directory,
it can sync the wrong directory and falsely report durability. Appendix A
reproduces the first case. The basename-trimming loop in the same implementation
also uses this unconditional separator rule.

Fix: centralize platform-aware separator/parent selection. On POSIX only slash
separates components; on Windows handle slash/backslash, drive roots, drive-relative
paths, UNC shares, and extended paths according to the supported path contract.
Do not normalize POSIX filenames into Windows spelling. Apply the same rule to
parent extraction and staging-name basename detection. Route platform operations
through the PAL and avoid embedding OS headers in the core.

Tests: a literal backslash basename, a regular slash path, no separator, root
parent, maximum-length basename containing backslashes, and a decoy directory
matching the incorrect prefix. A PAL sync observer must record the actual parent
being synced; merely checking PROVEN_OK misses the decoy case. Record the ordering
file-sync -> rename -> correct-directory-sync. Also retain real directory-sync
failure tests: errors after a successful rename cannot roll the rename back.
Document that commit boundary rather than claiming all durable errors preserve
the old bytes. Exit: correct parent is used for both POSIX cases and native Windows
root cases have independent results, not a Linux-derived pass.

## 6. H-004: signed sequence subtraction overflows at the sign boundary

`src/proven/job.c:proven_job_submit` calculates
`(proven_ptrdiff_t)seq - (proven_ptrdiff_t)pos`; `proven_job_execute_one` does the
same with `pos + 1`. At a signed-half-range boundary, the two casts can yield
`PTRDIFF_MAX` and `PTRDIFF_MIN`. Subtracting them is signed overflow, even though
the intended queue distance is just -1. This is reachable in a valid full queue:
capacity two, enqueue position p = `PTRDIFF_MAX + 1`, dequeue position p - 2,
cell sequences p - 1 and p. Submission should reject full without modifying it.

Appendix B seeds exactly that state and UBSan reports signed overflow. This does
not establish a naturally elapsed 64-bit lifetime failure; on 32-bit, crossing
2^31 submissions is a materially shorter lifetime. No concurrency race is needed
for the arithmetic defect.

Fix: calculate sequence distance in the unsigned queue counter type, then classify
zero/ahead/behind without signed subtraction. Document the modular-distance rule
and ensure queue capacity is strictly below half the counter range; reject
impossible capacity at init before allocations. An explicit unsigned high-bit
classification avoids depending on an unsigned-to-signed out-of-range conversion.
Use one reviewed helper at both producer and consumer sites. Do not change
memory-order annotations, admission, wake permits, or drain behavior in this patch.

Tests: seeded empty, full, and nonempty states around `PTRDIFF_MAX`, the sign
transition, `SIZE_MAX`, and unsigned wrap to zero; both submit and execute paths;
accepted jobs exactly once; full rejection without overwriting cells. Keep the
seam test-only, preferably an internal distance helper plus a queue-state harness,
not a new public mutation API. Run targeted UBSan, native 32-bit where available,
and the existing close/destroy stress and TSAN tests. Exit: no signed-overflow
reports, correct modular comparisons, and no queue shutdown regression.

## 7. Previously known Windows defects: exact closure tasks

### H-005: replacement primitive (RFC-0005 C-001)

`platform/proven_sys_fs.c:proven_sys_fs_rename` still uses `MoveFileW`, which fails
when the destination already exists. Both whole-file atomic write functions
route through it. The ordinary second write therefore fails on Windows.

Do not fix by deleting the destination first: that loses atomic visibility and
can destroy the old file on subsequent failure. Select a supported replacement
primitive with an explicit contract (`MoveFileExW` replacement flags or
`ReplaceFileW` with a separately handled missing destination). Keep same-volume
semantics; do not enable a silent copy/delete fallback. Define read-only target,
ACL/metadata, sharing, symlink, and durability behavior before choosing the final
primitive. Preserve the original Windows error for diagnosis.

Required native tests: first creation, replacement, open readers with/without
sharing permission, read-only destination, replacement failure, Unicode/long
paths, no missing-name/partial-content interval, old bytes preserved on failure,
and no leaked handles/temps. A source-text test or cross link is not native
atomicity evidence. Reuse B-033 rather than opening a duplicate backlog item.

### H-006: entropy request narrowing (RFC-0005 V-003)

`platform/proven_sys_random.c:proven_sys_random_bytes` casts `size_t len` once to
`ULONG` for `BCryptGenRandom`. On 64-bit Windows, `len > ULONG_MAX` silently
narrows: only a prefix (possibly zero bytes) is requested and NTSTATUS success
is translated into success for the whole buffer. Callers can treat unchanged
bytes as fresh entropy. The small requests used by ordinary seeding do not
exercise this defect.

Fix: loop over disjoint chunks at most `ULONG_MAX`, checking every BCrypt call.
Advance the pointer and remaining count only after success. Return false on any
failed chunk; never substitute a PRNG. Specify that failure may leave a filled
prefix (matching the existing boolean API) so callers discard the entire result.
Retain true for a zero-length request and false for null with nonzero length.

Tests: factor a private chunk planner/backend seam so 0, 1, limit-1, limit,
limit+1, 2*limit+1 are testable without allocating gigabytes. Check sum, offsets,
no gaps/overlap, and failures in first/middle/final chunks. A reduced artificial
limit plus a real small output buffer tests pointer progression; arithmetic-only
large tests must not construct out-of-object pointers. Run a native BCrypt smoke
test separately. Exit: success means every requested byte was submitted to a
successful OS entropy call; fake-backend and native evidence remain separate.

## 8. Execution contract for the implementing AI

1. Read the current project rules and relevant header contracts; verify this
   baseline or rebase the findings by symbol. Inspect status/diff before changes.
   This RFC is not authorization to modify consuming projects' vendored copies.
2. Obtain approval for the size-helper overflow policy and creation/default-mode
   policy. Follow the project's T3 plan requirement for security/concurrency/API
   implementation. Do not mistake this proposed RFC for an approved new API.
3. Land a failing regression for one H-ID, demonstrate the expected failure, then
   apply its smallest fix. Do not weaken assertions merely to get a green gate.
   Extend existing test executables where appropriate. If adding a test source,
   register it in `build_tests.inc` and update the catalog/count documents
   required by the existing documentation gates. Confirm the manifest filename
   in the current tree before editing; never assume automatic test discovery.
4. Run the focused test, `./nob build`, `./nob strict-error`, `./nob asan`, and
   `./nob ubsan` for native changes. Add `./nob freestanding` for H-001 and
   `./nob tsan` plus close/destroy stress for H-004. Run cross compile/link and
   native runtime lanes for Windows changes; record pass/fail/skip separately.
5. Update headers, relevant English/Korean manual explanations and examples,
   aliases for any added public function, and CHANGELOG for behavioral changes.
   Do not hand-edit generated `docs/en` or `docs/ko` editions. Run the documented
   generators only if the manual source changes require edition updates.
6. Run `scripts/project-check.sh`; inspect the final diff for unrelated changes
   and private data. Keep reproducer artifacts under ignored `build/`. A release
   or public vulnerability post requires a separate explicit decision.
7. Close each row only with the focused test name, command, compiler/target,
   before/after result, and implementing commit. If native Windows cannot run,
   label a source fix implemented-unverified and keep its runtime row open.

Closed against baseline `7d6b46729d8e501ac5935b2ac6859152d6b33ecd`, on branch `rfc-0006`,
gcc 14.2.0 / Linux / x86-64. Every row's regression was seen to FAIL before its fix.

| Work item | Registered regression | Fix | Evidence | Status |
|---|---|---|---|---|
| H-001 | `tests/test_unit_encode`, two new sections | `ffcc04b` | Before: helpers answered 0 at `M/2+1` and `M`; the encoder section died with SIGSEGV. After: `PROVEN_SIZE_MAX` and `PROVEN_ERR_OVERFLOW`. `build` · `strict-error` · `asan` · `ubsan` · `freestanding` | Closed on this target |
| H-002 | `tests/test_regression_fs_private_staging` | `7c22299` | Before: staging mode 0644. After: 0600, and the Appendix A observer agrees where modes are honoured. `build` · `strict-error` · `asan` · `ubsan` | Closed for the reproduced cases |
| H-003 | `tests/test_regression_fs_backslash_parent` | `91370a0` | Before: the durable write returned an error after publishing. After: succeeds and syncs the real parent, decoy directory untouched. `build` · `strict-error` · `asan` · `ubsan` | Closed for the reproduced cases |
| H-004 | `tests/test_regression_job_seq_wrap` | `dd6f20d` | Appendix B under UBSan: before, signed overflow at `job.c:262`; after, exit 0 with no diagnostic. `build` · `strict-error` · `asan` · `ubsan` · `tsan` | Closed on this target |
| H-005 | `tests/test_portability_source_contracts` (source contract only) | `57aecc0` | `./nob cross` compiles `windows-x86_64-winapi` and `windows-i686-winapi`. **No native run.** | Implemented, unverified |
| H-006 | `tests/test_portability_source_contracts` + planner boundaries | `57aecc0` | Planner checked at 0, 1, limit-1, limit, limit+1, 2*limit+1 and over a whole plan. `./nob cross` passes. **No native run.** | Implemented, unverified |

### The two decisions the RFC reserved, and how to reverse them

1. **Size-helper overflow policy** (section 3, proposal 4). Taken: the helpers answer
   `PROVEN_SIZE_MAX`. It is one line each in `src/proven/encode.c`; the alternative -
   result-returning helpers plus aliases, examples and legacy wrappers - is a larger public
   surface and was not built without a decision. Either way the encoders refuse
   independently, so a caller who passes the sentinel back as a capacity is refused rather
   than trusted.
2. **New-file permission policy** (section 4, proposal 3). Taken: unchanged. Restrictive
   creation carries an EXISTING target's mode across; a brand-new atomic target still gets
   `0666 & ~umask`, and `tests/test_regression_fs_private_staging` pins that so a change to
   it cannot happen by accident. Making new files restrictive by default is a policy
   decision, not a bug fix.

### Exit criteria this branch did NOT meet

The sections above ask for more than the reproduced cases. What was fixed and pinned is the
defect each section describes; these remain unwritten, and neither row should be read as
satisfying its own section 4 or 5 in full.

- **H-002.** Not covered: umask 0077; injected failures of open, chmod, write, sync and close;
  staging-name collisions; a second-user lane. Covered: initial staging mode under umasks 0022
  and 0000, atomic and durable replacement of a 0600 target, a new copy destination, the
  unchanged default for a brand-new target, and the finished public modes.
- **H-003.** Not covered: a root parent (`/name`), and the file-sync -> rename ->
  directory-sync ORDERING is not recorded - the test names which directory was synced, not
  when. Covered: a literal backslash basename, the decoy directory, a plain slash path, a
  bare filename, and a 250-character basename full of backslashes.
- **H-001.** Not covered: a 32-bit lane run, where a real allocation large enough to reach
  these boundaries is plausible. Covered: the helper boundaries, encoder refusal, and that
  the two refusals stay distinct.
- **H-004.** Not covered: a live queue seeded at the boundary inside a registered test - only
  the RFC's own Appendix B probe does that, once, under UBSan.

### How the two Windows rows get closed

`docs/rfc-0006-runtime-check.c` is a program built to be RUN by a person on a machine this
project cannot reach. `scripts/build-rfc-0006-check.sh` builds it into `dist/` for Windows
64-bit and 32-bit (statically, so it needs nothing beside itself but `KERNEL32`, `bcrypt`
and `msvcrt`) and for the host.

It asks exactly what these rows leave unanswered: a second atomic write over an existing
name, what a failed replacement leaves behind when the destination is held open with no
sharing, what a read-only destination does (recorded, not asserted - RFC section 7 asks for
that behaviour to be DEFINED, and defining it needs someone to see it), whether entropy
buffers are filled, and whether the library works on that platform at all. It writes its
report to `proven-windows-check-report.txt` beside itself, and that file is the evidence.

It also runs on the host, and does: a verifier nobody has executed is not a verifier. On
POSIX it reports 28 checks passed. That proves the harness and nothing about Windows -
`rename` has always replaced there, so the check that matters cannot fail on this machine.

### What is still open

- Native Windows for H-005 and H-006, and for the Windows half of H-002 (ACLs, not mode
  bits). macOS, 32-bit and embedded targets are unverified as before.
- A second-user lane for H-002. The observer runs as one user; the cross-user argument
  rests on the observed mode and ordinary POSIX open-fd semantics.
- Everything section 2 lists as deferred: RFC-0005 C-002/C-003, V-001/V-002,
  V-004..V-012, B-034..B-038, the file-copy identity race, Windows path-conversion races,
  map entropy fallback policy, and semaphore permit accumulation.

## Appendix A. Reproduce three baseline observations

Run from the repository root, in a disposable project-owned test directory. This
observer writes only synthetic OLD/NEW fixtures under `build/rfc-0006`; use no
private payloads. Ensure `build/rfc-0006/a` does not exist before the wrong-parent
case. Save the following as `build/rfc-0006/probe.c` after `mkdir -p build/rfc-0006`.
The hook calls real POSIX open; it changes timing deliberately, not access modes.

```c
#define _POSIX_C_SOURCE 200809L
#include "proven.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <assert.h>
static int held = -1;
static mode_t initial_mode;
int __real_open(const char *, int, ...);
int __wrap_open(const char *p, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap; va_start(ap, flags); mode = va_arg(ap, mode_t); va_end(ap);
    }
    int fd = __real_open(p, flags, mode);
    if (fd >= 0 && (flags & O_CREAT) && strstr(p, ".pvtmp")) {
        struct stat st;
        assert(fstat(fd, &st) == 0);
        initial_mode = st.st_mode & 0777;
        held = __real_open(p, O_RDONLY);
        assert(held >= 0);
    }
    return fd;
}
int main(void) {
    printf("sizes hex=%zu b64enc=%zu b64dec=%zu\n",
      proven_hex_encoded_size(SIZE_MAX / 2 + 1),
      proven_base64_encoded_size(SIZE_MAX),
      proven_base64_decoded_size(SIZE_MAX));
    proven_allocator_t a = proven_heap_allocator();
    proven_u8str_view_t path = PROVEN_LIT("build/rfc-0006/private-data");
    umask(0022);
    assert(proven_fs_write_file(a, path, (proven_mem_view_t){(const proven_byte_t *)"OLD",3}) == PROVEN_OK);
    assert(proven_fs_chmod(a, path, 0600) == PROVEN_OK);
    proven_err_t e = proven_fs_write_file_atomic(a, path, (proven_mem_view_t){(const proven_byte_t *)"NEW",3});
    char b[4] = {0};
    assert(held >= 0 && read(held,b,3) == 3);
    printf("temp initial_mode=%03o atomic_ok=%d held_fd_payload=%s\n", (unsigned)initial_mode, e == PROVEN_OK, b);
    close(held);
    path = PROVEN_LIT("build/rfc-0006/a\\b");
    e = proven_fs_write_file_durable(a, path, (proven_mem_view_t){(const proven_byte_t *)"NEW",3});
    int fd = __real_open("build/rfc-0006/a\\b", O_RDONLY);
    memset(b,0,sizeof b);
    assert(fd >= 0 && read(fd,b,3) == 3);
    printf("backslash durable_ok=%d payload=%s\n", e == PROVEN_OK, b);
    close(fd);
    return 0;
}
```

Compile against the debug objects just exercised by `./nob build`. Set D to the
build directory reported by that command, not an old sanitizer/profile directory.
The example below is the path reported for the audited baseline:

```sh
D=build/gcc-debug-c3625330
cc -std=c2x -Iinclude build/rfc-0006/probe.c \
  $(find "$D/src" "$D/platform" -name '*.o') \
  -pthread -lm -Wl,--wrap=open -o build/rfc-0006/probe
build/rfc-0006/probe
```

Observed (observer exit status 0 means it collected evidence, NOT that fixes pass):

```text
sizes hex=0 b64enc=0 b64dec=0
temp initial_mode=644 atomic_ok=1 held_fd_payload=NEW
backslash durable_ok=0 payload=NEW
```

After the fixes, on a filesystem that honours creation modes:

```text
sizes hex=18446744073709551615 b64enc=18446744073709551615 b64dec=13835058055282163712
temp initial_mode=600 atomic_ok=1 held_fd_payload=NEW
backslash durable_ok=1 payload=NEW
```

The retained descriptor still reads the payload, exactly as this section predicted: it
belongs to the same user and was opened before the mode was ever narrow. What changed is
that a DIFFERENT user can no longer open it at all. Run the observer somewhere modes are
honoured - a share with inherited ACLs can force a wider mode on every new file whatever
the creating call asks for, and this workstation's own checkout is such a share.

This is deliberately a before-fix observer. In particular, its same-user held-fd
read can still succeed after restrictive creation; the post-fix security test
must assert restrictive *initial permissions* or use a separately authorized
second-user lane. Do not require the observer output verbatim after fixing bugs.

## Appendix B. Reproduce queue arithmetic UB without a long soak

Save as `build/rfc-0006/job-wrap.c`. Including the implementation is a test-only
seam; do not ship it or link a second copy of `job.o` into this executable.

```c
#include "../../src/proven/job.c"
#include <stdint.h>
int main(void) {
    proven_job_cell_t cells[2] = {0};
    proven_job_sys_t sys = {0};
    sys.queue.buffer = cells;
    sys.queue.buffer_mask = 1;
    atomic_init(&sys.admission_state, 0);
    proven_size_t p = (proven_size_t)PTRDIFF_MAX + 1;
    atomic_init(&sys.queue.enqueue_pos, p);
    atomic_init(&sys.queue.dequeue_pos, p - 2);
    atomic_init(&cells[0].sequence, p - 1);
    atomic_init(&cells[1].sequence, p);
    return proven_job_submit(&sys, NULL, NULL) ? 1 : 0;
}
```

```sh
D=build/gcc-debug-c3625330
cc -std=c2x -fsanitize=undefined -fno-sanitize-recover=undefined -Iinclude \
  build/rfc-0006/job-wrap.c \
  $(find "$D/src" "$D/platform" -name '*.o' ! -name job.o) \
  -pthread -lm -o build/rfc-0006/job-wrap
build/rfc-0006/job-wrap
```

Observed: exit 1 with signed integer overflow in `proven_job_submit`, subtracting
-9223372036854775808 from 9223372036854775807. Only the included job implementation
and this probe are UBSan-instrumented; the linked debug PAL objects are not.
After the fix this full-queue probe must exit 0 without diagnostics. Expand the
registered regression to consumer and wraparound cases from H-004 rather than
keeping only this one boundary value.
