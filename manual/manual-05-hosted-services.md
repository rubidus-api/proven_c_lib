# Chapter 5: Hosted Services

This chapter covers `fs.h`, `sysio.h`, `mmap.h`, and `time.h`.

These APIs require hosted platform support and are excluded from the current freestanding subset.

## Table of contents

1. [Filesystem API](#1-filesystem-api)
2. [System I/O and environment](#2-system-io-and-environment)
3. [Memory mapping](#3-memory-mapping)
4. [Time API](#4-time-api)
5. [Examples and misuse cases](#5-examples-and-misuse-cases)

## 1. Filesystem API

The filesystem layer wraps platform file handles, paths, directory listing, metadata, permissions, links, and locks.

Raw filesystem helpers do not sanitize untrusted paths, enforce root confinement, or defend against symlink-race TOCTOU. Callers that accept untrusted paths must validate them before using the API.

### Structures and enums

```text
typedef struct {
    union {
        void *ptr;
        int fd;
    } internal;
} proven_fs_handle_t;

typedef proven_fs_handle_t proven_file_t;
```

Intent: represent a platform file handle without exposing POSIX or Win32 details to higher layers.

```text
typedef enum {
    PROVEN_FS_READ   = 1 << 0,
    PROVEN_FS_WRITE  = 1 << 1,
    PROVEN_FS_APPEND = 1 << 2,
    PROVEN_FS_CREATE = 1 << 3,
    PROVEN_FS_TRUNC  = 1 << 4,
    PROVEN_FS_CREATE_NEW = 1 << 5
} proven_fs_mode_t;
```

File mode flags can be combined. Examples:

- `PROVEN_FS_READ`
- `PROVEN_FS_WRITE | PROVEN_FS_CREATE | PROVEN_FS_TRUNC`
- `PROVEN_FS_WRITE | PROVEN_FS_APPEND | PROVEN_FS_CREATE`

```text
typedef enum {
    PROVEN_FS_TYPE_FILE,
    PROVEN_FS_TYPE_DIR,
    PROVEN_FS_TYPE_OTHER
} proven_fs_type_t;
```

```text
typedef struct {
    proven_u8str_t name;
    proven_fs_type_t type;
    proven_size_t size;
} proven_fs_entry_t;
```

Directory entries own `name`. Use `proven_fs_list_destroy()` for arrays returned by `proven_fs_list()`.

```text
typedef struct {
    proven_err_t err;
    proven_file_t value;
} proven_result_file_t;
```

```text
typedef enum {
    PROVEN_FS_PERM_OWNER_R = 1 << 8,
    PROVEN_FS_PERM_OWNER_W = 1 << 7,
    PROVEN_FS_PERM_OWNER_X = 1 << 6,
    PROVEN_FS_PERM_GROUP_R = 1 << 5,
    PROVEN_FS_PERM_GROUP_W = 1 << 4,
    PROVEN_FS_PERM_GROUP_X = 1 << 3,
    PROVEN_FS_PERM_OTHER_R = 1 << 2,
    PROVEN_FS_PERM_OTHER_W = 1 << 1,
    PROVEN_FS_PERM_OTHER_X = 1 << 0,
    PROVEN_FS_PERM_DEFAULT = PROVEN_FS_PERM_OWNER_R | PROVEN_FS_PERM_OWNER_W |
                             PROVEN_FS_PERM_GROUP_R | PROVEN_FS_PERM_OTHER_R
} proven_fs_perms_t;
```

```text
typedef enum {
    PROVEN_FS_LOCK_SHARED,
    PROVEN_FS_LOCK_EXCLUSIVE,
    PROVEN_FS_LOCK_UNLOCK
} proven_fs_lock_type_t;
```

```text
typedef struct {
    proven_size_t size;          /* size in bytes (0 for directories on some hosts) */
    proven_fs_type_t type;       /* PROVEN_FS_TYPE_FILE / _DIR - there is no symlink type */
    proven_fs_perms_t perms;     /* the nine permission bits only; the file type is in `type` */
    proven_i64 created_at;       /* always 0: no birth time is queried - see below */
    proven_i64 modified_at;      /* last-modification time, seconds since the Unix epoch */
    unsigned long long dev;      /* device id (POSIX st_dev; 0 where unavailable) */
    unsigned long long ino;      /* inode number (POSIX st_ino; 0 where unavailable) */
    unsigned long long uid;      /* owner id  (POSIX st_uid; 0 on Windows — no uid/gid) */
    unsigned long long gid;      /* group id  (POSIX st_gid; 0 on Windows) */
} proven_fs_stat_t;
```

`uid`/`gid` (added in v26.06.22a) hold the POSIX owner and group ids; both are `0`
on Windows, which has no `uid`/`gid` concept. Resolve them to names with the
host's `getpwuid`/`getgrgid` when displaying an `ls -l`-style owner/group column.

Two fields are narrower than they look:

- `created_at` is **always 0**. Plain `stat()` has no portable birth time, and the
  PAL does not ask for one. Only `modified_at` carries a real timestamp.
- `type` is only ever `PROVEN_FS_TYPE_FILE` or `PROVEN_FS_TYPE_DIR`. The PAL
  classifies by "is it a directory", so a FIFO, a socket, or a device stats as
  `FILE`. `PROVEN_FS_TYPE_OTHER` exists in the enum but nothing produces it today.

```c
proven_fs_stat_t st;
if (proven_is_ok(proven_fs_stat(scratch, PROVEN_LIT("/etc/hosts"), &st))) {
    proven_println("size={} uid={} gid={}",
        PROVEN_ARG(st.size), PROVEN_ARG(st.uid), PROVEN_ARG(st.gid));
}
```

### Macro

| Macro | Intent |
|---|---|
| `PROVEN_FS_PATH_SEP` | Preferred library-level separator character, currently `'/'`. |

### File functions

| API | Intent | Return |
|---|---|---|
| `proven_fs_open(scratch, path, mode)` | Open a file path. `scratch` is used for path conversion. | `proven_result_file_t`. |
| `proven_fs_close(file)` | Close file handle. | **Returns an error.** On a file you wrote to, the close *is* part of the write: NFS, CIFS and quota-enforcing filesystems report a failed write-back here and nowhere else. On a file you only read, `(void)`-ing it is fine. |
| `proven_fs_read(file, dest)` | Single read attempt into mutable slice. | `proven_result_size_t`. |
| `proven_fs_write(file, src)` | Single write attempt from byte view. | `proven_result_size_t`. |
| `proven_fs_write_all(file, src)` | Retry until all bytes are written or an error occurs. | `proven_err_t`. |
| `proven_fs_size(file)` | Query open file size. | `proven_result_size_t`. |
| `proven_fs_rename(scratch, src, dest)` | Rename or move path. | `proven_err_t`. |
| `proven_fs_remove(scratch, path)` | Remove file. | `proven_err_t`. |
| `proven_fs_copy(temp_alloc, src, dest)` | Copy file using temporary buffer allocation. | `proven_err_t`. |
| `proven_fs_mkdir(scratch, path)` | Create directory. | `proven_err_t`. |
| `proven_fs_rmdir(scratch, path)` | Remove empty directory. | `proven_err_t`. |
| `proven_fs_list(alloc, path)` | List directory into `proven_array_t` of `proven_fs_entry_t`. | `proven_result_array_t`. |
| `proven_fs_list_destroy(alloc, list)` | Destroy directory listing and entry names. | void. |
| `proven_fs_chmod(scratch, path, perms)` | Set permissions. | `proven_err_t`. |
| `proven_fs_lock(file, type, wait)` | Acquire/release file lock. | `proven_err_t`. |
| `proven_fs_stat(scratch, path, out_stat)` | Fill metadata. | `proven_err_t`. |

`proven_fs_stat()` reports only the nine permission bits in `perms`, so a stat's `perms` can be handed straight back to `proven_fs_chmod()`. It used to carry the raw POSIX `st_mode`, whose file-type bits `chmod` rejects - which made that round-trip, the obvious use of the field, fail with `PROVEN_ERR_INVALID_ARG` for every real file. Read the file type from `type`.

| `proven_fs_symlink(scratch, target, linkpath)` | Create symbolic link. | `proven_err_t`. |
| `proven_fs_link(scratch, oldpath, newpath)` | Create hard link. | `proven_err_t`. |
| `proven_fs_is_absolute(path)` | Classify absolute path. | bool. |
| `proven_fs_read_all(alloc, path)` | Allocate and read a whole file, to EOF. | `proven_result_mem_mut_t`. |
| `proven_fs_read_all_u8str(alloc, path)` | Same, as a NUL-terminated owned string. | `proven_result_u8str_t`. |
| `proven_fs_write_file(scratch, path, data)` | Create-or-truncate whole-file write. | `proven_err_t`. |
| `proven_fs_write_file_atomic(scratch, path, data)` | Whole-file write via temp file + rename. | `proven_err_t`. |
| `proven_fs_write_file_durable(scratch, path, data)` | Same, and on the disk before it returns. | `proven_err_t`. |
| `proven_fs_seek(file, offset, whence)` | Move the file position. | `proven_result_u64_t` (new offset). |
| `proven_fs_tell(file)` | Current position. | `proven_result_u64_t`. |
| `proven_fs_truncate(file, length)` | Set the file's length. O(1). | `proven_err_t`. |
| `proven_fs_pread(file, dest, offset)` | Read at an offset; does not move the position. | `proven_result_size_t`. |
| `proven_fs_pwrite(file, src, offset)` | Write at an offset; does not move the position. | `proven_result_size_t`. |
| `proven_fs_sync(file)` | Force this file's data to the disk (fsync). | `proven_err_t`. |
| `proven_fs_sync_dir(scratch, path)` | Force a directory's metadata to the disk. | `proven_err_t`. |

### Position, and the difference between atomic and durable

**A handle that cannot seek says so.** A pipe, a FIFO or a terminal returns
`PROVEN_ERR_UNSUPPORTED` from `proven_fs_seek`, not `PROVEN_ERR_IO`. Not being seekable
is a property of the thing, not a failure of the call, and code that adapts to it — the
scanner does — has to be able to tell them apart.

**`pread` and `pwrite` do not move the position.** That is what they are for: two
readers sharing one handle cannot race on a cursor that neither of them moves.

**Atomic and durable are different promises, and conflating them is how data gets
lost.**

- `proven_fs_write_file_atomic` guarantees that a *reader* never sees a half-written
  file. It says nothing about a power cut: the kernel may still be holding your bytes,
  and the rename may reach the disk before the data it points at.
- `proven_fs_write_file_durable` closes that window, in the only order that works:
  fsync the temp file, **then** rename, **then** fsync the directory. Syncing the file
  but not the directory leaves a crash window in which the bytes are safe and the name
  that points at them is not — which is exactly the corruption an atomic write exists
  to prevent.

The durable form waits for the storage device twice. Use it when losing the write would
be worse than the wait, and the atomic form when it would not.

`proven_sysio_flush` is none of this. It does nothing.

Important behavior:

- `proven_fs_read()` and `proven_fs_write()` are single-operation APIs and may process fewer bytes than requested.
- **A read at end-of-file returns `PROVEN_ERR_EOF`, not a zero-byte success.** A loop written the obvious way -
  `if (r.value == 0) break;` - never takes that branch, and treats the end of the file as an I/O failure instead.
  Check for `PROVEN_ERR_EOF` explicitly; the worked example at the end of this chapter shows the shape.
- Use `proven_fs_write_all()` when all bytes must be written.
- Zero-size read/write requests should succeed with zero bytes processed without requiring a non-null buffer.
- `proven_fs_is_absolute()` recognizes POSIX absolute paths, Windows drive-root paths, UNC paths, and extended Windows path forms.
- `proven_fs_read_all()` reads to EOF; it does not read to a pre-measured size. The file's reported size only seeds the initial capacity, so a regular file is still read in one allocation and one pass. This matters because `proven_fs_size()` reports 0 for anything that is not a regular file: a FIFO, a character device, or a `/proc` entry has no size that can be known up front, and reading to EOF is the only way to get their contents. It also means a file that grows while it is being read is not silently truncated. `value.size` is always the actual byte count, and an empty source yields `{ .ptr = NULL, .size = 0 }` with `PROVEN_OK`.
- `proven_fs_read_all()` and `proven_fs_read_all_u8str()` need an allocator with a `realloc_fn` if the source outgrows its reported size; a non-growing allocator returns `PROVEN_ERR_UNSUPPORTED` in that case.
- `proven_fs_read_all_u8str()` is the whole-file read most callers want: the result is NUL-terminated, so `proven_u8str_as_view()` and `proven_u8str_as_cstr()` work on it with no second copy. The terminator slot is reserved up front, so it costs no extra allocation. Contents are not validated as UTF-8. Release it with `proven_u8str_destroy()`.
- `proven_fs_write_file()` is not atomic: a reader can observe a partially written file, and a failure mid-write leaves the file truncated. `proven_fs_write_file_atomic()` writes a sibling temp file and renames it over the target, so a concurrent reader sees either the entire old file or the entire new one. It is atomic with respect to readers, not durable across power loss - `proven` exposes no fsync, so the rename may reach the disk before the data.

Example:

```c
proven_result_file_t of = proven_fs_open(
    alloc,
    PROVEN_LIT("out.txt"),
    PROVEN_FS_WRITE | PROVEN_FS_CREATE | PROVEN_FS_TRUNC
);
if (proven_is_ok(of.err)) {
    proven_err_t e = proven_fs_write_all(
        of.value,
        proven_mem_view_from_u8(PROVEN_LIT("hello\n"))
    );
    /* The close is part of the write. Close on the failure path too - the handle is
     * ours either way - but do not throw the answer away: on a network filesystem or
     * over quota, close() is the only place the failure appears. */
    proven_err_t ce = proven_fs_close(of.value);
    if (proven_is_ok(e)) e = ce;
    if (!proven_is_ok(e)) {
        proven_eprintln("writing out.txt failed");
    }
}
```

## 2. System I/O and environment

### Standard streams

```text
proven_file_t proven_sysio_stdin(void);
proven_file_t proven_sysio_stdout(void);
proven_file_t proven_sysio_stderr(void);
void proven_sysio_flush(proven_file_t file);
```

Purpose: expose standard streams as `proven_file_t` handles and flush when needed.

### `proven_sysio_scanner_t`

```text
typedef struct {
    proven_file_t file;
    proven_allocator_t alloc;
    proven_u8 *buffer;
    proven_size_t capacity;
    proven_size_t cursor;
    proven_size_t length;
    bool eof;
} proven_sysio_scanner_t;
```

Purpose: buffered scanner for bounded stream input, safe for pipes and stdin as well as seekable files. When a token reaches the end of the currently loaded fragment before EOF, the scanner refills the buffer and retries; only a token that cannot fit inside the whole buffer even after a refill is rejected, with `PROVEN_ERR_OUT_OF_BOUNDS`, instead of being accepted truncated.

### Sysio functions and macros

| API | Intent | Return |
|---|---|---|
| `proven_sysio_scanner_init(scanner, file, alloc, buffer_capacity)` | Allocate scanner buffer and bind stream. | `proven_err_t`. |
| `proven_sysio_scanner_deinit(scanner)` | Free scanner buffer. | void. |
| `proven_sysio_scanner_scan_impl(scanner, fmt, args, args_count)` | Internal scanner engine. | `proven_err_t`. |
| `proven_sysio_scanner_scan(scanner, fmt, ...)` | Type-safe buffered scan macro. | `proven_err_t`. |
| `proven_sysio_print_impl(handle, fmt, args, args_count)` | Internal printing engine. | `proven_err_t`. |
| `proven_sysio_scan_chunk_impl(handle, fmt, args, args_count)` | One-chunk scan engine. | `proven_err_t`. |
| `proven_print(fmt, ...)` | Print to stdout. | `proven_err_t`. |
| `proven_println(fmt, ...)` | Print to stdout with newline. | `proven_err_t`. |
| `proven_eprint(fmt, ...)` | Print to stderr. | `proven_err_t`. |
| `proven_eprintln(fmt, ...)` | Print to stderr with newline. | `proven_err_t`. |
| `proven_scan_fmt_from_file(file, fmt, ...)` | Scan from one fixed-size chunk of file. | `proven_err_t`. |
| `proven_scan_fmt_from_stdin(fmt, ...)` | Scan one fixed-size chunk from stdin. | `proven_err_t`. |
| `proven_env_get(alloc, key)` | Read environment variable into owned U8 string. | `proven_result_u8str_t`. |

`proven_sysio_scan_chunk_impl()` is intended for seekable file inputs. It reads at most one fixed-size chunk. If the handle cannot be rewound, it returns `PROVEN_ERR_UNSUPPORTED` before reading. If the chunk fills before a complete token is available, it returns `PROVEN_ERR_OUT_OF_BOUNDS` and rewinds the file cursor to the start of the chunk. Use `proven_sysio_scanner_t` for repeated buffered scanning.

Example:

```c
proven_println("answer={}", PROVEN_ARG(42));
proven_eprintln("warning: {}", PROVEN_ARG(PROVEN_LIT("low memory")));
```

Environment example. `proven_env_get` hands back an owned string, so it has to be destroyed:

```c
proven_result_u8str_t env = proven_env_get(alloc, PROVEN_LIT("PATH"));
if (proven_is_ok(env.err)) {
    proven_u8str_view_t path = proven_u8str_as_view(&env.value);
    proven_println("PATH is {} bytes", PROVEN_ARG(path.size));
    proven_u8str_destroy(alloc, &env.value);
}
```

## 3. Memory mapping

### Structures and enums

```text
typedef enum {
    PROVEN_MMAP_READ  = 0x01,
    PROVEN_MMAP_WRITE = 0x02,
    PROVEN_MMAP_EXEC  = 0x04
} proven_mmap_prot_t;

typedef enum {
    PROVEN_MMAP_PRIVATE = 0x01,
    PROVEN_MMAP_SHARED  = 0x02
} proven_mmap_flags_t;

typedef struct {
    void *ptr;
    proven_size_t size;
    proven_fs_handle_t file;
    void *internal_handle;
} proven_mmap_t;

typedef struct {
    proven_err_t err;
    proven_mmap_t value;
} proven_result_mmap_t;
```

### Functions

| API | Intent | Return |
|---|---|---|
| `proven_mmap_create(file, offset, size, prot, flags)` | Map a file region. `size == 0` maps until EOF. Offset must match the platform mapping granularity. | `proven_result_mmap_t`. |
| `proven_mmap_destroy(mmap)` | Unmap region and clear state. | `proven_err_t`. |
| `proven_mmap_sync(mmap)` | Flush shared writable changes to storage. | `proven_err_t`. |
| `proven_mmap_as_view(mmap)` | Borrow mapped memory as U8 view. | `proven_u8str_view_t`. |

Example:

```c
proven_result_file_t f = proven_fs_open(alloc, PROVEN_LIT("data.bin"), PROVEN_FS_READ);
if (proven_is_ok(f.err)) {
    proven_result_mmap_t mr = proven_mmap_create(
        f.value,
        0,
        0,
        PROVEN_MMAP_READ,
        PROVEN_MMAP_PRIVATE
    );
    if (proven_is_ok(mr.err)) {
        /* The view borrows the mapping: it dies when the mapping is destroyed. */
        proven_u8str_view_t bytes = proven_mmap_as_view(mr.value);
        proven_println("mapped {} bytes", PROVEN_ARG(bytes.size));
        (void)proven_mmap_destroy(&mr.value);
    }
    (void)proven_fs_close(f.value);
}
```

## 4. Time API

### Types

```text
typedef proven_i64 proven_time_t;
```

Nanoseconds since UNIX epoch.

```text
typedef struct {
    proven_i32 year;
    proven_u8 month;
    proven_u8 day;
    proven_u8 hour;
    proven_u8 min;
    proven_u8 sec;
    proven_u32 ms;
    proven_u8 weekday;
} proven_datetime_t;
```

Field ranges:

- `month`: 1 to 12.
- `day`: 1 to 31.
- `hour`: 0 to 23.
- `min`: 0 to 59.
- `sec`: 0 to 60.
- `ms`: 0 to 999.
- `weekday`: 0 to 6, Sunday is 0.

```text
typedef struct {
    const proven_u8str_view_t *month_names;
    const proven_u8str_view_t *month_short_names;
    const proven_u8str_view_t *weekday_names;
    const proven_u8str_view_t *weekday_short_names;
} proven_time_locale_t;
```

`proven_time_locale_en` is the default English locale.

### Functions

| API | Intent | Return |
|---|---|---|
| `proven_time_u8_fmt(alloc, str, dt, locale, fmt)` | Append formatted datetime to U8 string. | `proven_err_t`. |
| `proven_time_u16_fmt(alloc, str, dt, locale, fmt)` | Append formatted datetime to U16 string unless U16 is disabled. | `proven_err_t`. |
| `proven_time_now()` | Current timestamp in nanoseconds. | `proven_time_t`. |
| `proven_time_breakdown(time_ns)` | Convert epoch nanoseconds to broken-down UTC time. | `proven_datetime_t`. |
| `proven_time_now_datetime()` | Current local broken-down time. | `proven_datetime_t`. |
| `proven_time_sleep(ms)` | Sleep for milliseconds. | void. |

Datetime format keys:

- `{year}`, `{month}`, `{day}`, `{hour}`, `{min}`, `{sec}`, `{ms}`, `{wday_num}`
- `{Month}`, `{mon}` using locale
- `{Weekday}`, `{wday}` using locale

Example:

```c
proven_result_u8str_t r = proven_u8str_create(alloc, 32);
if (proven_is_ok(r.err)) {
    proven_u8str_t s = r.value;

    proven_datetime_t now = proven_time_now_datetime();
    proven_err_t e = proven_time_u8_fmt(
        alloc,
        &s,
        now,
        &proven_time_locale_en,
        "{year}-{month:0>2}-{day:0>2} {hour:0>2}:{min:0>2}:{sec:0>2}"
    );
    if (proven_is_ok(e)) {
        proven_println("{}", PROVEN_ARG(proven_u8str_as_view(&s)));
    }

    proven_u8str_destroy(alloc, &s);
}
```

## 5. Examples and misuse cases

### Single writes can be partial

Wrong:

```text
proven_result_size_t w = proven_fs_write(file, data);
/* wrong: one write may be partial, and w.value is never looked at */
```

Correct:

```c
proven_result_file_t f = proven_fs_open(alloc, PROVEN_LIT("out.txt"),
                                        PROVEN_FS_WRITE | PROVEN_FS_CREATE | PROVEN_FS_TRUNC);
if (proven_is_ok(f.err)) {
    proven_mem_view_t data = proven_mem_view_from_u8(PROVEN_LIT("payload"));
    proven_err_t e = proven_fs_write_all(f.value, data);   /* loops until done */
    proven_err_t ce = proven_fs_close(f.value);            /* and the close can fail too */
    if (proven_is_ok(e)) e = ce;
    (void)e;
}
```

### Directory listings need special destruction

Wrong:

```text
proven_result_array_t r = proven_fs_list(alloc, PROVEN_LIT("."));
PROVEN_ARRAY_DESTROY(&r.value); /* wrong: entry names leak */
```

Correct:

```c
proven_result_array_t r = proven_fs_list(alloc, PROVEN_LIT("."));
if (proven_is_ok(r.err)) {
    proven_fs_list_destroy(alloc, &r.value);
}
```

### Do not use a mapping after destroy

Wrong:

```text
proven_mmap_destroy(&map);
use_bytes(map.ptr, map.size); /* wrong: mapping has been released */
```

### Use buffered sysio scanning for streams

For repeated reading from stdin or pipes, prefer:

```c
proven_sysio_scanner_t scanner = {0};
proven_err_t e = proven_sysio_scanner_init(&scanner, proven_sysio_stdin(), alloc, 4096);
if (proven_is_ok(e)) {
    int value = 0;
    e = proven_sysio_scanner_scan(&scanner, "{}", PROVEN_SCAN_ARG(&value));
    proven_sysio_scanner_deinit(&scanner);
}
```

### Environment values are owned strings

Wrong:

```text
proven_result_u8str_t env = proven_env_get(alloc, PROVEN_LIT("PATH"));
use_path(proven_u8str_as_view(&env.value)); /* wrong if env.err was not checked, and it leaks */
```

Correct:

```c
proven_result_u8str_t env = proven_env_get(alloc, PROVEN_LIT("HOME"));
if (proven_is_ok(env.err)) {
    proven_u8str_view_t home = proven_u8str_as_view(&env.value);
    proven_println("HOME={}", PROVEN_ARG(home));
    proven_u8str_destroy(alloc, &env.value);
}
```

### Worked example: reading and writing whole files

Compiled and run by the test suite. This is the whole-file API most callers actually want, including the atomic rewrite that preserves the target's permissions.

<!-- example: manual/examples/ex_05_fs_wholefile.c -->
```c
/*
 * The whole-file API: one call in, one call out. It exists because the
 * open/read-loop/close dance is where most file-handling bugs live - a forgotten
 * close, a partial read treated as EOF, a truncated file left behind by a failed
 * write. If you are reading or writing a file in its entirety, this is the API.
 */

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    /* A relative path in the current directory: the example must not depend on a
     * writable /tmp, and it removes what it creates before returning. */
    proven_u8str_view_t path = PROVEN_LIT("proven_example_wholefile.tmp");
    proven_u8str_view_t text = PROVEN_LIT("first line\nsecond line\n");

    /* --- write it in one call ---------------------------------------------- */
    /* Not atomic: a concurrent reader can see this file half-written. Fine here,
     * because nobody else is looking at it yet. */
    proven_err_t err = proven_fs_write_file(alloc, path, proven_mem_view_from_u8(text));
    EXAMPLE_REQUIRE(proven_is_ok(err), "writing the whole file should succeed");
    if (!proven_is_ok(err)) return 1;

    /* --- read it back as raw bytes ----------------------------------------- */
    /* proven_fs_read_all reads to EOF rather than to a pre-measured size, so it
     * also works on a pipe or a /proc entry, whose size cannot be known up front. */
    proven_result_mem_mut_t raw = proven_fs_read_all(alloc, path);
    EXAMPLE_REQUIRE(proven_is_ok(raw.err), "reading the whole file should succeed");
    if (proven_is_ok(raw.err)) {
        EXAMPLE_REQUIRE(raw.value.size == text.size, "read_all should return every byte written");
        /* The block is plain allocator memory - hand it back to the allocator that
         * produced it. There is no proven_fs_read_all_destroy. */
        alloc.free_fn(alloc.ctx, raw.value.ptr);
    }

    /* --- read it back as a string ------------------------------------------ */
    /* This is the one most callers want: the result is NUL-terminated, so it can
     * be handed to a view, to as_cstr, or to the scanner with no second copy. The
     * terminator slot is reserved up front, so it costs no extra allocation. */
    proven_result_u8str_t s = proven_fs_read_all_u8str(alloc, path);
    EXAMPLE_REQUIRE(proven_is_ok(s.err), "reading the whole file as a string should succeed");
    if (!proven_is_ok(s.err)) {
        (void)proven_fs_remove(alloc, path);
        return 1;
    }
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_as_view(&s.value), text),
                    "the file's contents should come back unchanged");
    printf("read back %zu bytes: %s", (size_t)proven_u8str_as_view(&s.value).size,
           proven_u8str_as_cstr(&s.value));
    proven_u8str_destroy(alloc, &s.value);

    /* --- stat, and the perms round-trip ------------------------------------ */
    proven_fs_stat_t st = {0};
    err = proven_fs_stat(alloc, path, &st);
    EXAMPLE_REQUIRE(proven_is_ok(err), "stat on a file we just wrote should succeed");
    EXAMPLE_REQUIRE(st.type == PROVEN_FS_TYPE_FILE, "a regular file should stat as a FILE");
    EXAMPLE_REQUIRE(st.size == text.size, "stat should report the size we wrote");

    /* `perms` carries the nine permission bits and nothing else, so it can be fed
     * straight back to chmod. That is the whole point of the field: read a file's
     * mode, and later restore it. (It used to carry the raw POSIX st_mode, whose
     * file-type bits chmod rejects - so this obvious round-trip failed.) */
    err = proven_fs_chmod(alloc, path, st.perms);
    EXAMPLE_REQUIRE(proven_is_ok(err), "a stat's perms must be accepted back by chmod");

    /* Now make the file owner-only, so the next check has something to prove. */
    proven_fs_perms_t private_perms = PROVEN_FS_PERM_OWNER_R | PROVEN_FS_PERM_OWNER_W;
    err = proven_fs_chmod(alloc, path, private_perms);
    EXAMPLE_REQUIRE(proven_is_ok(err), "restricting the file to its owner should succeed");

    /* --- rewrite it atomically --------------------------------------------- */
    /* A sibling temp file plus a rename: a concurrent reader sees either the whole
     * old file or the whole new one, never a half-written mix. Atomic for readers,
     * not durable across power loss - proven exposes no fsync. */
    proven_u8str_view_t text2 = PROVEN_LIT("replacement\n");
    err = proven_fs_write_file_atomic(alloc, path, proven_mem_view_from_u8(text2));
    EXAMPLE_REQUIRE(proven_is_ok(err), "the atomic rewrite should succeed");

    proven_fs_stat_t st2 = {0};
    err = proven_fs_stat(alloc, path, &st2);
    EXAMPLE_REQUIRE(proven_is_ok(err), "stat after the atomic rewrite should succeed");
    EXAMPLE_REQUIRE(st2.size == text2.size, "the file should now hold the replacement text");
    /* The rename writes a *new* inode over the old name, so the permissions would
     * be lost unless they were copied across. They are: rewriting a 0600 file does
     * not republish it as 0644. */
    EXAMPLE_REQUIRE(st2.perms == private_perms,
                    "the atomic rewrite must preserve the target's permissions");

    /* --- clean up ----------------------------------------------------------- */
    err = proven_fs_remove(alloc, path);
    EXAMPLE_REQUIRE(proven_is_ok(err), "removing the temp file should succeed");

    return EXAMPLE_OK();
}
```

### Worked example: open, read, write, close

Compiled and run by the test suite. Note the read loop: a read at end-of-file returns `PROVEN_ERR_EOF`, not a zero-byte success, so a loop that only checks for zero bytes never terminates the way its author expected.

<!-- example: manual/examples/ex_05_fs_stream.c -->
```c
/*
 * The open/read/write/close path, for when the whole-file API (ex_05_fs_wholefile)
 * is not enough: you are streaming, or you want to own the buffer.
 *
 * The one thing to get right here: a single read or write moves *up to* the
 * requested number of bytes, not exactly that many. Treating one short read as
 * end-of-file is the classic way to silently lose the tail of a file.
 */

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    proven_u8str_view_t path = PROVEN_LIT("proven_example_stream.tmp");
    proven_u8str_view_t text = PROVEN_LIT("streamed bytes, read back in chunks\n");

    /* --- write ------------------------------------------------------------- */
    /* CREATE makes the file if it is absent; TRUNC empties it if it is not. The
     * allocator is only used to convert the path for the platform call. */
    proven_result_file_t out = proven_fs_open(alloc, path,
                                              PROVEN_FS_WRITE | PROVEN_FS_CREATE | PROVEN_FS_TRUNC);
    EXAMPLE_REQUIRE(proven_is_ok(out.err), "opening the file for writing should succeed");
    if (!proven_is_ok(out.err)) return 1;

    /* write_all loops for us. proven_fs_write does one attempt and may write less,
     * which is almost never what a caller means. */
    proven_err_t err = proven_fs_write_all(out.value, proven_mem_view_from_u8(text));

    /* The close is part of the write, and on a network or quota-enforced filesystem it is
     * the ONLY place the failure appears: the bytes were buffered, write() said yes, and
     * close() is where the disk finally says no. Close on the failure path too - the
     * handle is ours either way - but do not throw the answer away. */
    proven_err_t cerr = proven_fs_close(out.value);
    if (proven_is_ok(err)) err = cerr;

    EXAMPLE_REQUIRE(proven_is_ok(err), "writing the whole buffer should succeed");
    if (!proven_is_ok(err)) {
        (void)proven_fs_remove(alloc, path);
        return 1;
    }

    /* --- read -------------------------------------------------------------- */
    proven_result_file_t in = proven_fs_open(alloc, path, PROVEN_FS_READ);
    EXAMPLE_REQUIRE(proven_is_ok(in.err), "opening the file for reading should succeed");
    if (!proven_is_ok(in.err)) {
        (void)proven_fs_remove(alloc, path);
        return 1;
    }

    /* size is a hint for sizing the buffer, not a promise about how many bytes any
     * one read will hand over - and it is 0 for anything that is not a regular
     * file (a pipe, a device, a /proc entry). The loop below does not rely on it. */
    proven_result_size_t sz = proven_fs_size(in.value);
    EXAMPLE_REQUIRE(proven_is_ok(sz.err), "querying the size of an open file should succeed");
    EXAMPLE_REQUIRE(sz.value == text.size, "the file should be as long as what we wrote");

    proven_byte_t buf[128];
    proven_size_t total = 0;

    /* The partial-read loop. Each pass asks for whatever is left of the buffer and
     * advances by however much actually arrived: a short read is normal, not the
     * end of the file. The end of the file is a distinct status - PROVEN_ERR_EOF
     * with zero bytes - so the loop terminates on that, and on nothing else. The
     * loop also stops if the source outgrows the buffer; noticing that is the
     * caller's business (here it cannot happen, but a growing file could). */
    for (;;) {
        if (total == sizeof buf) break;   /* buffer full: caller decides what to do */

        proven_mem_mut_t dest = { .ptr = buf + total, .size = sizeof buf - total };
        proven_result_size_t r = proven_fs_read(in.value, dest);
        if (r.err == PROVEN_ERR_EOF) break;
        if (!proven_is_ok(r.err)) {
            (void)proven_fs_close(in.value);
            (void)proven_fs_remove(alloc, path);
            EXAMPLE_REQUIRE(false, "reading from the open file should not fail");
            return 1;
        }
        total += r.value;
    }

    (void)proven_fs_close(in.value);

    EXAMPLE_REQUIRE(total == text.size, "the loop should have read every byte in the file");
    proven_u8str_view_t got = { .ptr = buf, .size = total };
    EXAMPLE_REQUIRE(proven_u8str_view_eq(got, text), "the bytes should come back unchanged");

    printf("read %zu bytes in chunks: %.*s", (size_t)total, (int)total, (const char *)buf);

    /* --- clean up ----------------------------------------------------------- */
    err = proven_fs_remove(alloc, path);
    EXAMPLE_REQUIRE(proven_is_ok(err), "removing the temp file should succeed");

    return EXAMPLE_OK();
}
```

## Streams: writers and readers

The formatter's only sink used to be a `proven_u8str_t`. A file was a
`proven_file_t`. The in-memory scanner read a view; the file scanner read something
else again. **Four types, four function families, no common interface** — so you
could not write one `serialize(sink, value)` that worked over both memory and a
file, you could not format into a file at all, and there was no way to read a file
line by line.

A **writer** is a byte sink. A **reader** is a byte source. Both are small vtables
passed by value, exactly like `proven_allocator_t`, and for the same reason: the
caller decides where the bytes go, and nothing is hidden.

| API | Intent |
|---|---|
| `proven_writer_from_file(&file)` | Unbuffered sink over an open file. |
| `proven_writer_from_u8str(&state, &str, alloc)` | Appends to an owned string. |
| `proven_writer_from_buffer(&state)` | Fixed caller memory. Allocates nothing, ever. |
| `proven_writer_buffered(&state, inner, buf)` | Accumulates small writes; **you** supply the buffer. |
| `proven_fprint(w, fmt, ...)` / `proven_fprintln` | Format straight into a writer. No allocation. |
| `proven_reader_from_file(&file)` / `_from_view(&state, view)` | Byte sources. |
| `proven_reader_buffered(&state, inner, buf)` | Buffered source; required for line reading. |
| `proven_reader_read_line(&state)` | One line, without the newline. |

Four rules worth stating plainly, because each of them is a way this could have
been designed badly:

- **Buffering uses memory you supply.** `proven_writer_buffered` takes a
  `proven_mem_mut_t`, the way `proven_arena_create` does. There is no hidden global
  buffer, which means there is also no destructor to flush it for you — **you must
  flush before the buffer goes out of scope.** In exchange, your logging path never
  allocates, and a program logging its way out of an out-of-memory condition can
  still log.
- **A full sink refuses; it does not truncate.** A fixed buffer that fills up
  returns `PROVEN_ERR_OUT_OF_BOUNDS` and records `overflowed`. A sink that silently
  drops the end of your data is worse than one that says it cannot take it.
- **A line too long for the reader's buffer is an error**, not a truncated line. A
  truncated line handed back as if it were whole is a corruption the caller has no
  way to detect. The buffer is yours; size it for the input you expect.
- **A partial write is a fact, not a failure to be papered over.** A `write_fn`
  returns a `proven_result_size_t`: how many bytes the sink took, *and* what went
  wrong. A pipe, a socket, or a filling disk really does accept 4096 of your 6000
  bytes and then fail, and a trait that says "consume it all or fail" simply makes
  such a sink impossible to write correctly. `proven_writer_write` still means
  all-or-nothing (it loops); `proven_writer_write_partial` is there when you need to
  see how far you got. The buffered writer keeps only the tail the sink did **not**
  take — the first version kept the whole buffer and re-sent it, so a failing sink
  received the accepted prefix twice.

- **A writer that has failed stays failed.** Once a buffered writer has lost bytes, the
  stream it was producing has a hole in it that the receiver cannot see, so every later
  write and flush returns the original error. There is no `clear()`: if you have a
  recovery story it involves a new writer over a new sink, not pretending this one is
  fine. (Before this, `flush` answered `PROVEN_OK` after a failed write — the buffer was
  empty, so there was nothing left to fail on — and "write, write, write, check the
  flush", which is how almost everyone uses a buffered writer, reported success on a
  full disk.)

A reader's rule is the mirror image: **a read that fails is an error, never an end of
file.** `proven_reader_read` returns `PROVEN_ERR_IO`, not a clean zero-byte EOF, when
the source breaks — because a file cut short by a disk error and a file that simply
ended are the same thing to a caller who cannot tell them apart, and only one of them
is safe to act on.

What it costs, measured over 10,000 lines to stdout:

| | `write()` syscalls | `malloc()` |
|---|---|---|
| `proven_println` | 10,000 | **0** (was 10,000) |
| buffered writer, 8 KiB of caller memory | **24** | **0** |

`proven_println` is deliberately still one syscall per line: buffering it would need
hidden global state. A caller who wants the 24 builds a buffered writer and says so.

### Worked example: one serializer, three destinations, and reading it back

Compiled and run by the test suite. Note that `render_row` does not know where its
bytes are going — that is the entire point.

<!-- example: manual/examples/ex_05_stream.c -->
```c
/*
 * Writers and readers: one interface for "where bytes go" and one for "where bytes
 * come from".
 *
 * The point is that the code below - render_row - does not know and does not care
 * whether it is writing to a string, to a fixed buffer, or to a file. That was
 * impossible before: the formatter's only sink was a proven_u8str_t.
 */

/* One serializer. It takes a sink, not a destination. */
static proven_err_t render_row(proven_writer_t w, int id, const char *name) {
    proven_fmt_result_t r = proven_fprintln(w, "{:>4} | {}", PROVEN_ARG(id), PROVEN_ARG(name));
    return r.err;
}

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    /* --- the same code, into a growing string ----------------------------- */
    proven_result_u8str_t s = proven_u8str_create(alloc, 16);
    EXAMPLE_REQUIRE(proven_is_ok(s.err), "string create");

    proven_writer_u8str_t s_state;
    proven_writer_t to_string = proven_writer_from_u8str(&s_state, &s.value, alloc);
    EXAMPLE_REQUIRE(proven_is_ok(render_row(to_string, 7, "ada")), "render into a string");

    printf("into a string:\n%s", proven_u8str_as_cstr(&s.value));
    proven_u8str_destroy(alloc, &s.value);

    /* --- the same code, into memory you own: zero allocations ------------- */
    proven_byte_t fixed[64];
    proven_writer_buf_t b_state = { .buf = { .ptr = fixed, .size = sizeof fixed } };
    proven_writer_t to_buffer = proven_writer_from_buffer(&b_state);
    EXAMPLE_REQUIRE(proven_is_ok(render_row(to_buffer, 8, "grace")), "render into a buffer");
    EXAMPLE_REQUIRE(b_state.len > 0, "the buffer received the row");

    /* A full buffer REFUSES; it does not truncate. A sink that silently drops the
     * end of your data is worse than one that says it cannot take it. */
    proven_byte_t tiny[4];
    proven_writer_buf_t t_state = { .buf = { .ptr = tiny, .size = sizeof tiny } };
    proven_writer_t to_tiny = proven_writer_from_buffer(&t_state);
    EXAMPLE_REQUIRE(proven_writer_write_str(to_tiny, PROVEN_LIT("far too long")) == PROVEN_ERR_OUT_OF_BOUNDS,
                    "a full buffer refuses rather than truncating");
    EXAMPLE_REQUIRE(t_state.overflowed, "and it records that it did");

    /* --- the same code, into a file, buffered ------------------------------ */
    /*
     * The buffer is memory YOU supply, exactly like an arena's. This library has no
     * hidden global state, so it cannot flush for you at exit - which is why you must
     * flush before the buffer goes out of scope. In exchange, your logging path never
     * allocates: ten thousand lines here cost 0 mallocs and a couple of dozen write
     * syscalls, where ten thousand proven_println calls cost 10,000 syscalls.
     */
    proven_u8str_view_t path = PROVEN_LIT("example_stream_rows.txt");
    proven_result_file_t f = proven_fs_open(alloc, path,
        (proven_fs_mode_t)(PROVEN_FS_WRITE | PROVEN_FS_CREATE | PROVEN_FS_TRUNC));
    EXAMPLE_REQUIRE(proven_is_ok(f.err), "open the output file");
    proven_file_t file = f.value;

    proven_byte_t out_buf[4096];
    proven_writer_buffered_t w_state;
    proven_writer_t to_file = proven_writer_buffered(&w_state,
        proven_writer_from_file(&file),
        (proven_mem_mut_t){ .ptr = out_buf, .size = sizeof out_buf });

    for (int i = 0; i < 3; ++i) {
        EXAMPLE_REQUIRE(proven_is_ok(render_row(to_file, i, "row")), "render into the file");
    }
    EXAMPLE_REQUIRE(proven_is_ok(proven_writer_flush(to_file)),
                    "flush: nothing is written until you say so");
    /* And the close, which is the last thing that can tell you the write did not land. */
    EXAMPLE_REQUIRE(proven_is_ok(proven_fs_close(file)), "closing the written file");

    /* --- reading it back, a line at a time -------------------------------- */
    /* Reading a file line by line was simply not possible before: the only route was
     * loading the entire file into memory and splitting it by hand. */
    proven_result_file_t rf = proven_fs_open(alloc, path, PROVEN_FS_READ);
    EXAMPLE_REQUIRE(proven_is_ok(rf.err), "reopen for reading");
    proven_file_t rfile = rf.value;

    proven_byte_t in_buf[128];
    proven_reader_buffered_t r_state;
    (void)proven_reader_buffered(&r_state, proven_reader_from_file(&rfile),
                                 (proven_mem_mut_t){ .ptr = in_buf, .size = sizeof in_buf });

    int lines = 0;
    for (;;) {
        proven_result_u8str_view_t line = proven_reader_read_line(&r_state);
        if (line.err == PROVEN_ERR_EOF) break;
        EXAMPLE_REQUIRE(proven_is_ok(line.err), "read a line");
        /* The view points INTO the reader's buffer, and is valid only until the next
         * call. Copy it if it has to outlive that. */
        printf("line %d: %.*s\n", lines, (int)line.val.size, (const char *)line.val.ptr);
        ++lines;
    }
    EXAMPLE_REQUIRE(lines == 3, "three rows in, three lines out");

    (void)proven_fs_close(rfile);
    (void)proven_fs_remove(alloc, path);

    return EXAMPLE_OK();
}
```

