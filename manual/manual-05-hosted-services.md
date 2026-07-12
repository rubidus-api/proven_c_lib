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

```c
typedef struct {
    union {
        void *ptr;
        int fd;
    } internal;
} proven_fs_handle_t;

typedef proven_fs_handle_t proven_file_t;
```

Intent: represent a platform file handle without exposing POSIX or Win32 details to higher layers.

```c
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

```c
typedef enum {
    PROVEN_FS_TYPE_FILE,
    PROVEN_FS_TYPE_DIR,
    PROVEN_FS_TYPE_OTHER
} proven_fs_type_t;
```

```c
typedef struct {
    proven_u8str_t name;
    proven_fs_type_t type;
    proven_size_t size;
} proven_fs_entry_t;
```

Directory entries own `name`. Use `proven_fs_list_destroy()` for arrays returned by `proven_fs_list()`.

```c
typedef struct {
    proven_err_t err;
    proven_file_t value;
} proven_result_file_t;
```

```c
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

```c
typedef enum {
    PROVEN_FS_LOCK_SHARED,
    PROVEN_FS_LOCK_EXCLUSIVE,
    PROVEN_FS_LOCK_UNLOCK
} proven_fs_lock_type_t;
```

```c
typedef struct {
    proven_size_t size;          /* size in bytes (0 for directories on some hosts) */
    proven_fs_type_t type;       /* regular / directory / symlink / other */
    proven_fs_perms_t perms;     /* POSIX-style permission bits (see proven_fs_perms_t) */
    proven_i64 created_at;       /* creation time, seconds since the Unix epoch */
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
| `proven_fs_close(file)` | Close file handle. | void. |
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
| `proven_fs_symlink(scratch, target, linkpath)` | Create symbolic link. | `proven_err_t`. |
| `proven_fs_link(scratch, oldpath, newpath)` | Create hard link. | `proven_err_t`. |
| `proven_fs_is_absolute(path)` | Classify absolute path. | bool. |
| `proven_fs_read_all(alloc, path)` | Allocate and read a whole file, to EOF. | `proven_result_mem_mut_t`. |
| `proven_fs_read_all_u8str(alloc, path)` | Same, as a NUL-terminated owned string. | `proven_result_u8str_t`. |
| `proven_fs_write_file(scratch, path, data)` | Create-or-truncate whole-file write. | `proven_err_t`. |
| `proven_fs_write_file_atomic(scratch, path, data)` | Whole-file write via temp file + rename. | `proven_err_t`. |

Important behavior:

- `proven_fs_read()` and `proven_fs_write()` are single-operation APIs and may process fewer bytes than requested.
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
if (!proven_is_ok(of.err)) return of.err;

proven_err_t e = proven_fs_write_all(
    of.value,
    proven_mem_view_from_u8(PROVEN_LIT("hello\n"))
);
proven_fs_close(of.value);
if (!proven_is_ok(e)) return e;
```

## 2. System I/O and environment

### Standard streams

```c
proven_file_t proven_sysio_stdin(void);
proven_file_t proven_sysio_stdout(void);
proven_file_t proven_sysio_stderr(void);
void proven_sysio_flush(proven_file_t file);
```

Purpose: expose standard streams as `proven_file_t` handles and flush when needed.

### `proven_sysio_scanner_t`

```c
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

Purpose: buffered scanner for bounded file-backed input. Tokens must fit within the current loaded buffer; if a token reaches the end of that buffer before EOF, the scanner reports `PROVEN_ERR_OUT_OF_BOUNDS` instead of accepting a truncated value.

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

Environment example:

```c
proven_result_u8str_t env = proven_env_get(alloc, PROVEN_LIT("PATH"));
if (proven_is_ok(env.err)) {
    use_path(proven_u8str_as_view(&env.value));
    proven_u8str_destroy(alloc, &env.value);
}
```

## 3. Memory mapping

### Structures and enums

```c
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
if (!proven_is_ok(f.err)) return f.err;

proven_result_mmap_t mr = proven_mmap_create(
    f.value,
    0,
    0,
    PROVEN_MMAP_READ,
    PROVEN_MMAP_PRIVATE
);
if (proven_is_ok(mr.err)) {
    proven_u8str_view_t bytes = proven_mmap_as_view(mr.value);
    use_bytes(bytes.ptr, bytes.size);
    proven_mmap_destroy(&mr.value);
}
proven_fs_close(f.value);
```

## 4. Time API

### Types

```c
typedef proven_i64 proven_time_t;
```

Nanoseconds since UNIX epoch.

```c
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

```c
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
if (!proven_is_ok(r.err)) return r.err;
proven_u8str_t s = r.value;

proven_datetime_t now = proven_time_now_datetime();
proven_err_t e = proven_time_u8_fmt(
    alloc,
    &s,
    now,
    &proven_time_locale_en,
    "{year}-{month:0>2}-{day:0>2} {hour:0>2}:{min:0>2}:{sec:0>2}"
);

proven_u8str_destroy(alloc, &s);
return e;
```

## 5. Examples and misuse cases

### Single writes can be partial

Wrong:

```c
proven_result_size_t w = proven_fs_write(file, data);
/* wrong: one write may be partial */
```

Correct:

```c
proven_err_t e = proven_fs_write_all(file, data);
```

### Directory listings need special destruction

Wrong:

```c
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

```c
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

```c
proven_result_u8str_t env = proven_env_get(alloc, PROVEN_LIT("PATH"));
use_path(proven_u8str_as_view(&env.value)); /* wrong if env.err was not checked */
```

Correct:

```c
proven_result_u8str_t env = proven_env_get(alloc, PROVEN_LIT("PATH"));
if (proven_is_ok(env.err)) {
    use_path(proven_u8str_as_view(&env.value));
    proven_u8str_destroy(alloc, &env.value);
}
```
