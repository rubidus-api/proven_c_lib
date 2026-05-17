# proven_c_lib Checklist

## Always before committing

- Update `include/proven/version.h` first.
- Sync the visible version string in `README.md`, `SPEC.md`, `TEST.md`, `manual/`, `CHANGELOG.md`, and `docs-site/index.html` when present.
- Add a `CHANGELOG.md` entry that explains the change.
- Keep public examples and help text on `/home/user/work/...` paths only.
- Do not expose private host paths, share names, user names, or SSH key names in public docs or source comments.
- Run the relevant build and test modes for the change.

## Known lessons

- Silent build drivers are hard to debug. Emit structured `BEGIN`, `ENV`, `PHASE`, `SOURCE`, `TEST`, `SUMMARY`, `PASS`, `FAIL`, and `NOTE` lines early.
- Windows/MSYS2 needs explicit diagnostics for compiler selection, executable suffixes, build-root creation, and shell mismatch problems.
- A fixed 256-byte buffer for environment keys is too small for general use. Use dynamic allocation when the public API accepts long names.
- Windows absolute path checks must include UNC and extended path forms, not just drive-letter paths.
- Windows append should preserve append semantics. Do not rely on a simple seek-to-end emulation when real append behavior is required.
- If a public structure or symbol changes, add regression coverage immediately and update the manual.
- If a bug is serious enough to repeat, add a short prevention note here with the symptom, root cause, and the first thing to inspect.
