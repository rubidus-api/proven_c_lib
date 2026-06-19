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

---

## 2026-06-18 — ENHANCEMENT: no fixed-capacity `proven_u8str_t` over caller-owned memory

- **Status:** RESOLVED in `proven_c_lib-v26.06.18a`. Added `proven_u8str_borrow`
  / `proven_u8str_reset` (fixed-capacity string over caller memory; safe-by-
  default `borrowed` flag so growing ops refuse to reallocate caller memory and
  destroy is a no-op) and the bounded `proven_mem_copy`. See CHANGELOG and
  `docs/internal/proposals/rfc-0002-borrowed-fixed-capacity-u8str.md`.
- **Reported by:** prov_text_editor (RFC-0004 / Special Milestone S — adopting the
  proven string system across the editor).
- **Category:** enhancement (not a defect; existing behavior is correct).
- **Affected:** `include/proven/u8str.h`, `include/proven/buffer.h`.

### Context
prov adopted `proven_u8str_t` / `proven_u8str_view_t` widely (config parser,
buffer-set paths, filename basenames, the piece-table insert store, the prompt
accumulator, and the global status line via `proven_u8str_append_fmt_grow`).
Four call sites could **not** be moved onto the proven string system and still
rely on libc `snprintf` / `memcpy`.

### Observed limitation
`proven_u8str_t` always owns allocator-backed memory: the only constructors are
`proven_u8str_create(alloc, limit)` and `proven_u8str_create_from_view(alloc, …)`,
and `proven_buf_t` likewise only has `proven_buf_create(alloc, cap)`. There is
**no constructor that wraps caller-owned memory** (a stack/static `char buf[N]`)
as a fixed-capacity string. Consequently:

1. **Per-frame formatting cannot use proven.** prov rebuilds its command line,
   per-window status line, tab bar, and transient `message` every render frame
   into stack buffers. Using `proven_u8str_t` there would force a heap
   allocation (or a reused scratch) per line, per frame — so these stay on
   bounded `snprintf`. The type-safe `proven_*_fmt` formatter (which we *do* use
   for the one heap-backed status scratch) is unavailable here.
2. **Pure / allocator-free modules cannot use proven.** prov's command-label
   helper (`command.c`, no allocator by design) formats `"%s %s"` into a
   caller-owned buffer. With no borrow constructor, `proven_u8str_append_fmt`
   (the atomic fixed-capacity variant already in the API) has nothing to write
   into, so it stays on `snprintf`.

The fixed-capacity append family (`proven_u8str_append`,
`proven_u8str_append_fmt` — "Atomic Fixed-Capacity", returning
`PROVEN_ERR_OUT_OF_BOUNDS` on overflow) is already designed for exactly this
use; only the *constructor* over external memory is missing.

### Requested API (maintainer's call on the exact shape)
A borrow constructor that yields a fixed-capacity `proven_u8str_t` over
caller-owned bytes, where the fixed-capacity (non-growing) operations work and
`proven_u8str_destroy` is a safe no-op. For example:

```c
/* Wrap caller-owned memory [buf, buf+cap) as a fixed-capacity, initially-empty,
 * NUL-terminated string. No ownership is taken; destroy is a no-op. The
 * growing operations (*_grow) must reject it (PROVEN_ERR_OUT_OF_BOUNDS or
 * INVALID_ARG) since the memory cannot be reallocated. */
proven_u8str_t proven_u8str_borrow(proven_byte_t *buf, proven_size_t cap);
```

(or the equivalent at the `proven_buf_t` layer, e.g.
`proven_buf_borrow(ptr, cap)`, with the u8str wrapper built on top). A
companion `proven_u8str_reset(&s)` (truncate to empty, keep the buffer) would
let a borrowed or owned string be reused each frame without reallocation.

### Why this matters
It would let prov replace its remaining hot-path and allocator-free `snprintf`
sites with proven's type-safe structural formatter — removing the last libc
string dependencies and closing the "format/argument mismatch" bug class there —
with **zero per-frame allocation**.

### Secondary, minor
A bounded `proven_mem_copy(dst, dst_cap, proven_mem_view_t src)` in `memory.h`
would let byte-range *reads* (e.g. copying a document slice into a caller
buffer) drop raw `memcpy` for a bounds-checked primitive. Low priority — these
are byte moves, not string assembly.

### Downstream status
prov is **not** halted on this — it shipped Milestone S with these four sites
intentionally left on libc and documented (RFC-0004 §8, TODO "libc dependency
ledger"). It will convert them once a borrow constructor is available.

---

## 2026-06-19 — ENHANCEMENT: `proven_fs_stat` exposes no owner/group (uid/gid)

- **Status:** OPEN — feature request (not a defect).
- **Reported by:** prov_text_editor (building the file-open directory browser).
- **Affected file/symbol:** `include/proven/fs.h` — `proven_fs_stat_t`
  (fields: `size`, `type`, `perms`, `created_at`, `modified_at`, `dev`, `ino`)
  and `platform/proven_sys_fs.c` — `proven_sys_fs_stat` / `proven_sys_fs_stat_t`.

### Observed behavior
`proven_fs_stat` returns permissions and timestamps but no ownership. The POSIX
backend already does a `stat()` whose `struct stat` carries `st_uid` / `st_gid`,
and `proven_sys_fs_dir_next` likewise `fstatat()`s every entry — the ownership
fields are simply discarded.

### Expected behavior
A way to obtain an entry's owner and group. Minimal shape:

- Add `unsigned long long uid; unsigned long long gid;` to `proven_fs_stat_t`
  (and the sys-level stat struct), populated from `st_uid` / `st_gid` on POSIX.
- Windows has no uid/gid; a documented convention (e.g. leave them 0, or expose
  the owner SID / account name through a separate optional call) would let
  downstream show an "owner" column without `#ifdef`-ing the platform.
- Optional but ideal: a name-resolution helper (uid → user name, gid → group
  name) so callers need not link `getpwuid`/`getgrgid` themselves.

### Downstream status
prov is **not** halted on this. The browser shipped with the size / permissions
/ mtime / type columns (all derivable from the current API) and the owner/group
columns documented as deferred (CHANGELOG "File-open browser"). It will add the
owner/group columns once `proven_fs_stat` exposes uid/gid.
