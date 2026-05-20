# proven Agent Guide (v26.05.19j)

This file is the working rulebook for humans and AI agents editing `proven_c_lib`.

## Scope and source of truth

- Language: ISO C23.
- License: MIT.
- Repository example: `/home/user/work/proven_c_lib`.
- Shared build root example: `/home/user/work/build/proven_c_lib`.
- Build driver: `nob.c` with `nob.h`.
- Public API: `include/proven/*.h` and `include/proven.h`.
- PAL boundary: `platform/proven_sys_*.[ch]`.
- Tests: plain C executables in `tests/`.
- Version source of truth: `include/proven/version.h`.
- Current version string: `proven_c_lib-v26.05.19j`.

## Path and privacy policy

- Public docs, comments, help text, examples, and generated manuals must never expose private host paths, storage mount names, account names, SSH key names, or share names.
- Use `/home/user/work/...` examples only.
- Do not mention internal storage roots, private usernames, or other account-specific details in user-facing text.

## Repository layout

- `include/proven.h`: umbrella public include.
- `include/proven/`: public headers for types, errors, memory, allocators, containers, I/O, scanning, formatting, jobs, coroutines, and aliases.
- `src/proven/`: portable implementation files. Do not include OS headers directly here.
- `platform/`: PAL implementations for heap, filesystem, time, environment, threads, console I/O, and math.
- `tests/`: self-contained executable tests and compile checks.
- `manual/`: user manual and freestanding manual.
- `README.md`: short GitHub landing page.
- `SPEC.md`: behavior and layout contract.
- `TEST.md`: detailed test matrix and log contract.
- `CHANGELOG.md`: chronological record of public changes.
- `MEMORY.md`: durable repository facts and stable environment notes.
- `CHECKLIST.md`: recurring bug lessons and release guardrails.
- `docs/` and `docs/ai/` have been consolidated into this guide. Do not recreate their old markdown files.
- `docs-site/` and the root `package.json` wrapper were removed. Do not recreate them in this repository unless the project scope changes.
- `build/`: generated output. Do not edit by hand.

## Required commands

Compile the build driver first:

```sh
cc nob.c -o nob
```

Use explicit commands. Running `./nob` without arguments prints help.

```sh
./nob build              # debug build and full hosted test run
./nob release            # optimized build and full hosted test run
./nob strict             # warnings enabled
./nob strict-error       # warnings as errors
./nob asan               # AddressSanitizer run
./nob ubsan              # UndefinedBehaviorSanitizer run
./nob tsan               # ThreadSanitizer run
./nob regression         # regression-only debug run
./nob regression-asan    # regression-only ASan run
./nob regression-ubsan   # regression-only UBSan run
./nob freestanding       # freestanding subset compile/run checks
./nob cross              # compile-only cross-target matrix
./nob clean              # remove build output
```

Useful options:

```sh
./nob build -cc clang
./nob strict-error -cc clang
./nob build -cflags "-DNAME=value"
./nob build -ldflags "-lm"
./nob build -build-root /home/user/work/build/proven_c_lib
./nob cross -build-root /home/user/work/build/proven_c_lib
./nob build -f
```

## Versioning policy

- `include/proven/version.h` is the only source for version macros.
- Any version change must update:
  - `include/proven/version.h`
  - `CHANGELOG.md`
  - `README.md`
  - `SPEC.md`
  - `TEST.md`
  - manual files under `manual/`
  - any help text, comments, generated examples, or snippets that display the version
- Update the version first, then sync all visible docs in the same change.
- Record the change in `CHANGELOG.md` every time source, tests, build scripts, or public docs change.

## Git workflow

- This project is a Git repository. Keep history in the project-local `.git/` directory.
- Commit each verified change set.
- Use short imperative commit subjects.
- Do not commit build output, the local `nob` executable, logs, temporary test files, or environment files.
- Do not create temporary or non-distributed files inside the repository tree; use a separate workspace outside the repository for scratch artifacts and generated files that are not meant to be shipped.
- Local commits remain the source of truth until they are explicitly pushed.
- GitHub is used for distribution only in this repository; do not add Actions workflows unless the scope changes explicitly.

## C rules

- Target C23.
- Use `proven_size_t`, `proven_ptrdiff_t`, and `proven_byte_t` for sizes, signed offsets, and raw memory.
- Public typedef names end in `_t` unless the API intentionally exposes short fixed-width aliases such as `proven_u8`.
- Functions and variables use `lower_snake_case`.
- Macros use `UPPER_SNAKE_CASE`.
- Avoid VLAs in library code.
- Use checked arithmetic helpers for overflow-prone operations.
- Do not mutate public structure internals unless the header explicitly allows it.

## Architecture and dependency order

- Foundation: types, errors, alignment, memory.
- Allocation: allocator, heap, arena, pool.
- Text and buffers: buffer, u8str, u16str.
- Containers: array, list, ring, map, algorithm.
- Hosted services: fs, time, mmap, sysio.
- Execution helpers: coro, job.
- Text processing: fmt, scan.
- Optional aliases: alias_xcv.
- Lower layers must not depend on higher layers.
- OS and C runtime interactions belong under `platform/`.
- `src/proven/` must not include OS headers directly.
- `PROVEN_FREESTANDING` removes hosted services and builds the reduced core.

## Memory and ownership

- Ownership is explicit. Callers pass allocators and destroy owned objects.
- Errors are values. Fallible APIs return `proven_err_t` or `proven_result_*_t`.
- Reallocation-style operations should remain failure-atomic if documented that way.
- Borrowed views must remain valid for the documented lifetime.
- Raw memory inspection uses byte views, not type-punning through unrelated pointers.
- Arena frees are no-ops by design.
- Prefer explicit scratch allocation when temporary buffers are needed during persistent arena mutations.

## Testing policy

- Add or update tests before behavior changes.
- Keep tests as plain C executables.
- Run the smallest relevant test first, then the broader matrix.
- For memory-sensitive changes, use ASan and UBSan.
- For atomic or scheduler changes, use TSan when supported.
- For public header or configuration changes, run freestanding checks when relevant.
- Test output must be structured.
- Build driver should emit BEGIN, ENV, PHASE, SOURCE, TEST, SUMMARY, PASS, FAIL, and NOTE lines as appropriate.
- Each test executable should print BEGIN, INTENT, FAIL_HINT, PASS, and failure lines in the standardized format.
- If a build appears silent on Windows/MSYS2, emit a diagnostic note early enough that the user can tell the build driver did something.

## Documentation rules

- Keep all public docs and comments in English ASCII.
- README.md is bilingual: present the same substantive content in English first and Korean second.
- Keep README concise and front-page friendly.
- Keep manual files detailed, with intent, parameters, return values, examples, and misuse cases.
- Update docs alongside code and tests.

## Rule 11 - restrained technical language

- Prefer bounded, testable wording over absolute claims.
- Avoid language that implies guarantees, elimination, permanence, or proof unless the code or test can actually support that claim.
- Use terms such as designed to, intended to, validates, checks, reduces, or preserves when they are accurate.
- Keep changelog prose factual and compact.
- If a phrase sounds like marketing copy, rewrite it before publishing.

## Troubleshooting policy

- `cc nob.c -o nob` is the required first step for local build-driver use.
- `./nob` without arguments prints help.
- If `nob` fails to start a build, check compiler selection, build-root creation, and path permissions first.
- On Windows/MSYS2, check shell flavor, executable suffix, `PATH`, and directory creation behavior.
- If a PAL symbol is unresolved, make sure the corresponding `platform/` unit is compiled.

## Release checklist

- Update `include/proven/version.h`.
- Update `CHANGELOG.md` with a dated entry that explains what changed.
- Sync visible version strings everywhere else in the same patch.
- Run `./nob clean` followed by the relevant verification commands.
- Confirm generated `build/` output is not treated as source.

## Public API synchronization

- When adding, renaming, or removing a public symbol, update the header, implementation, tests, alias layer, alias smoke test, and user-facing docs.
- If a public structure layout changes, update the manual and any tests that rely on size or layout.
- If path behavior changes, add regression coverage.

## Long-lived notes

- Put durable, repository-wide facts in `MEMORY.md`.
- Put recurring bug lessons and prevention rules in `CHECKLIST.md`.
- Keep both files short and stable.
- Move a rule there when it is clearly worth remembering across sessions.

## TODO.md policy

- `TODO.md` tracks only unresolved review follow-ups and repository-wide maintenance items.
- Each TODO entry should name the scope, the risk or open decision, and the next verification hook.
- When a TODO item is implemented and verified, remove it from `TODO.md` and record the closure in `CHANGELOG.md`.
- Do not use `TODO.md` for ephemeral task planning; use the session `todo` tool for temporary work lists.

## Consolidated guidance from archived docs

### Requirements

- The library is intended to be lightweight, explicit, and predictable.
- It is designed for code that wants small C components, clear ownership, and a firm platform boundary.
- The build system is intentionally minimal and self-hosted through `nob.c`.

### Glossary

- CRT: C runtime, isolated behind PAL wrappers.
- UB: Undefined behavior.
- OOB: Out-of-bounds error.
- VTable allocator: a function-pointer based allocator trait.
- MPMC: multi-producer multi-consumer.

### Security

- The library reduces common safety risks but does not sanitize every external input for the caller.
- Callers remain responsible for path validation, symlink policy, and process-level trust decisions.
- Public APIs should preserve explicit error returns instead of hiding failure.

### Decisions and workflow

- Use test-first changes where practical.
- Keep the platform abstraction explicit and narrow.
- Prefer checked, visible behavior over clever implicit behavior.
- If a change reveals a recurring mistake, record the prevention rule in `CHECKLIST.md`.
