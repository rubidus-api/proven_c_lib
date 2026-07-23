# proven_c_lib Checklist

## Active Task

- None.

The 2026-07-23 whole-library audit is complete through RFC-0005 phase 0. Seven immediate defects
or false-green gates were fixed and verified, including shared preprocessed build/test
manifests, content-aware cache invalidation, and privacy-safe path/ignore rules; unresolved platform, release-profile,
scheduler, and performance work is specified in
`docs/RFC-0005-whole-library-audit-and-hardening.md` and queued as B-033 through B-038 in
`docs/BACKLOG.md`. Skipped cross targets remain verification gaps rather than passes.

## Always before committing

- Update `include/proven/version.h` first.
- Sync the visible version string in `README.md` (English) and `README-ko.md` (Korean), `TEST.md`, `manual/` and `manual-ko/` (chapter headings and the `version.h` excerpt in chapter 1), and `CHANGELOG.md`.
- Add a `CHANGELOG.md` entry that explains the change.
- Keep public examples and help text on relative paths such as `build-out/proven_c_lib`.
- Do not expose private host paths, share names, user names, or SSH key names in public docs or source comments.
- Run the relevant build and test modes for the change.
- **If the change touches public API, follow `docs/DOCUMENTING.md`** (survey → plan → edit → verify).
  Most of the rules above are now enforced by the build rather than by memory: the version string
  must agree with itself (`test_docs_version_sync`), every public function must be documented, and
  the manual must not document a function that does not exist (`test_docs_manual_symbols`).

## Known lessons

- Silent build drivers are hard to debug. Emit structured `BEGIN`, `ENV`, `PHASE`, `SOURCE`, `TEST`, `SUMMARY`, `PASS`, `FAIL`, and `NOTE` lines early.
- Windows/MSYS2 needs explicit diagnostics for compiler selection, executable suffixes, build-root creation, and shell mismatch problems.
- A fixed 256-byte buffer for environment keys is too small for general use. Use dynamic allocation when the public API accepts long names.
- Windows absolute path checks must include UNC and extended path forms, not just drive-letter paths.
- Windows append should preserve append semantics. Do not rely on a simple seek-to-end emulation when real append behavior is required.
- If a public structure or symbol changes, add regression coverage immediately and update the manual.
- If a bug is serious enough to repeat, add a short prevention note here with the symptom, root cause, and the first thing to inspect.
- For decimal parsing changes, always re-check `tests/test_unit_float_rfc_0001_cases.c` and `tests/test_unit_scan.c` together. A tokenizer-level exponent guard can accidentally preserve safety while breaking `strtod`-style `endptr` behavior.
- Keep `significant_digits` consistent with the actual significand: trailing zeros are folded into `exp10` and excluded from the significand and `mantissa_u64`, so they must also be excluded from `significant_digits`. Symptom of the mismatch: long-mantissa inputs with a trailing zero (e.g. `12345678901234567890`) round to a power of two because the inflated magnitude estimate pushes `binary_exp_bounds` above the true exponent and the search clamps to its lower bound. First place to inspect: the `out->significant_digits` assignment in `proven_float_decimal_build_number`, then `proven_float_decimal_binary_exp_bounds`.
- The exact-fallback path is only exercised by long-mantissa and subnormal-tie inputs in the bundled benchmark corpora; normal-range values are caught earlier by Clinger/Eisel-Lemire. To validate fallback changes, run a large randomized differential check against host `strtod` with many significant digits AND extreme exponents — the small fixed corpus will not surface bounds/rounding regressions.
