# proven — Downstream Defect Reports

Issues found in `proven` by downstream projects. Each entry is a request for an
upstream fix; downstream projects must not patch `proven` directly.

---

## 2026-06-16 — `proven_panic_handler` weak symbol fails to link on Windows (mingw-w64 / PE-COFF)

- **Status:** RESOLVED in `proven_c_lib-v26.06.16x` (panic migrated to the
  `proven_panic()` / `proven_set_panic_handler()` registration model; see
  CHANGELOG). The Windows x64 link was verified: proven now links to a `PE32+`
  executable.
- **Reported by:** prov_text_editor (adding a Windows x64 cross-compile target).
- **Affected file/symbol:** `src/proven/panic.c` — `proven_panic_handler`,
  defined with `__attribute__((weak))` (panic.c:4-6). Declared in
  `include/proven/panic.h:23`; the header notes users may override it.
- **Toolchain:** `x86_64-w64-mingw32-gcc (GCC) 16.1.0`, cross-compiling on
  Arch Linux to Windows x64 (PE-COFF).

### Observed behavior
All proven translation units compile cleanly for `x86_64-w64-mingw32`, but
linking the final executable fails:

```
ld: src/proven/pool.c:81: undefined reference to `proven_panic_handler'
ld: src/proven/pool.c:87: undefined reference to `proven_panic_handler'
collect2: error: ld returned 1 exit status
```

The reference comes from `proven_pool_free_trait` (pool.c) and is not resolved
to the weak default in panic.c.

### Root cause
`__attribute__((weak))` does not work the same way on PE-COFF as on ELF. On ELF
(native Linux) the weak default in panic.c resolves and the program links
(verified: `nm` shows `proven_panic_handler` as `W`). With the mingw-w64 / PE
linker, the weak definition in a separate object does not satisfy the reference,
so the symbol is reported undefined. This blocks any Windows build of proven,
even though every proven source otherwise compiles for `_WIN32`.

### Expected behavior
proven should link on its supported Windows toolchain (mingw-w64) without the
downstream project having to supply `proven_panic_handler`.

### Suggested directions (maintainer's call)
- Provide a **strong** default `proven_panic_handler` on PE targets (e.g.
  `#ifdef _WIN32` define it non-weak), keeping the weak default on ELF; or
- Replace the weak-override mechanism with a portable one (e.g. a settable
  function pointer / registration call) that does not rely on weak linkage; or
- Document that Windows consumers must define `proven_panic_handler` themselves,
  and confirm that is the intended contract.

### Minimal reproduction
```
x86_64-w64-mingw32-gcc -std=c23 -Iinclude -Iplatform -c src/proven/*.c \
    platform/proven_sys_*.c -odir build/win
x86_64-w64-mingw32-gcc -o test.exe build/win/*.o   # undefined reference
```

### Downstream status
prov_text_editor has **halted** its Windows x64 build pending this fix (per its
AGENTS §10.1 halt-and-report policy). The native Linux build is unaffected.
