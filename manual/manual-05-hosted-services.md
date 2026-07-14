# Chapter 5: Hosted Services

This chapter covers `fs.h`, `sysio.h`, `mmap.h`, `time.h`, `stream.h`, and `random.h`.

These APIs require hosted platform support and are excluded from the current freestanding subset.

## Table of contents

1. [Filesystem API](#1-filesystem-api)
2. [System I/O and environment](#2-system-io-and-environment)
3. [Memory mapping](#3-memory-mapping)
4. [Time API](#4-time-api)
5. [Examples and misuse cases](#5-examples-and-misuse-cases)
6. [Walking a tree](#walking-a-tree)
7. [Streams: writers and readers](#streams-writers-and-readers)
8. [The standard streams](#the-standard-streams)
9. [Randomness, by use case](#randomness-by-use-case)

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

One field is narrower than it looks, and one has a sharp edge:

- `created_at` is **always 0**. Plain `stat()` has no portable birth time, and the
  PAL does not ask for one. Only `modified_at` carries a real timestamp.
- `type` is `FILE` for a regular file, `DIR` for a directory, and
  **`PROVEN_FS_TYPE_OTHER`** for everything else — a FIFO, a socket, a device, a dangling
  symlink. (Until v26.07.13a all of those stat'd as `FILE`, which told a caller it could
  open them and read bytes out of them. A dangling symlink cannot be opened at all.)

  `type` **follows symlinks**, here and in the directory walk, which is what makes the two
  agree: a symlink to a regular file is `FILE`, and a symlink to a directory is `DIR`. The
  edge that follows is real — **a recursive walker can loop**, because a symlink pointing
  at an ancestor is a cycle and its type says `DIR`. Carry a depth limit, or remember
  `(dev, ino)` pairs and refuse to descend into one you have seen.

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

There used to be a `proven_sysio_flush` here that was none of this: it claimed to flush a
buffer that did not exist. It is **deleted**. Pushing a buffered writer's bytes to the OS is
`proven_writer_flush`; pushing the OS's bytes to the disk is `proven_fs_sync`. Those are two
different operations, and one word could not honestly mean both.

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
- `proven_fs_write_file()` is not atomic: a reader can observe a partially written file, and a failure mid-write leaves the file truncated. `proven_fs_write_file_atomic()` writes a sibling temp file and renames it over the target, so a concurrent reader sees either the entire old file or the entire new one. It is atomic with respect to readers, not durable across power loss: the rename may reach the disk before the data. When you need durability, ask for it explicitly with `proven_fs_write_file_durable` (or `proven_fs_sync`), described above.

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
```

Purpose: expose the standard streams as `proven_file_t` handles. They are also writers and
readers — see [The standard streams](#the-standard-streams) below, which is what lets you read
stdin a line at a time, buffer stdout, and format straight into either.

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
     * not durable across power loss. When it must be, proven_fs_write_file_durable asks. */
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

## Walking a tree

`proven_fs_dir_*` walks ONE directory. `proven_fs_walk` walks a tree — and it is worth
saying exactly what it refuses to do, because those refusals are the feature:

| | |
|---|---|
| It cannot **loop** | It never descends *through* a symlink. |
| It cannot **escape** | Same rule: a link out of the tree is reported, not followed. |
| It cannot **lie** | A directory it cannot read comes back as an *error* naming that directory, and the walk goes on. A tree walker that silently skips an unreadable subtree is how a backup misses files and reports success. |
| It cannot **bloat** | One open handle per *level* of the current path, plus one reused path buffer. Memory is a function of depth, not of how many files there are. |

A symlinked directory is still **reported** — it exists, `type` is `DIR`, `is_symlink` is
true — it is simply not entered. Hiding it would be its own kind of lie. If you *want* to
follow it, you have the path: open a second walk on it, and you own the cycle question.

`entry.path` and `entry.name` are borrowed from the walk's one reused buffer and are valid
**until the next call**. Copy them if you need them to outlive the step; that is the price
of a walk of a million entries costing one allocation instead of a million.

Two limits, both of which say so rather than going quiet:

- `max_depth` — how far to descend. A directory *at* the limit is still reported (it is an
  entry); it is not entered.
- `PROVEN_FS_WALK_DEPTH_LIMIT` (256) — how deep the walk's own stack goes, ever. A directory
  past it comes back as `PROVEN_ERR_OUT_OF_BOUNDS`, naming the directory.

Compiled and run by the test suite:

<!-- example: manual/examples/ex_05_fs_walk.c -->
```c
/*
 * Walking a tree.
 *
 * The three things a recursive walker gets wrong, and what this one does instead:
 *
 *   It loops.       A symlink pointing at an ancestor is a cycle. This walk never descends
 *                   THROUGH a symlink - the symlinked directory is still reported, it is
 *                   simply not entered - so a cycle is impossible, and so is walking out of
 *                   the tree you asked about and into the rest of the filesystem.
 *
 *   It lies.        A directory it cannot read gets skipped, and the walk reports success.
 *                   That is how a backup misses a subtree. Here the error comes back from
 *                   proven_fs_walk_next, with the entry naming the directory, and the walk
 *                   carries on from the next sibling. You decide what to do about it.
 *
 *   It bloats.      Reading a whole directory into memory before yielding anything makes a
 *                   walk of a big tree cost a big allocation. This one holds one open handle
 *                   per LEVEL of the current path and one reused path buffer - so its memory
 *                   is a function of depth, not of how many files there are.
 */

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    /* A small tree to walk: a file, a directory, a file inside it. */
    EXAMPLE_REQUIRE(proven_is_ok(proven_fs_mkdir(alloc, PROVEN_LIT("ex_walk"))), "mkdir");
    EXAMPLE_REQUIRE(proven_is_ok(proven_fs_mkdir(alloc, PROVEN_LIT("ex_walk/inner"))), "mkdir inner");
    EXAMPLE_REQUIRE(proven_is_ok(proven_fs_write_file(alloc, PROVEN_LIT("ex_walk/top.txt"),
        proven_mem_view_from_u8(PROVEN_LIT("top")))), "write top.txt");
    EXAMPLE_REQUIRE(proven_is_ok(proven_fs_write_file(alloc, PROVEN_LIT("ex_walk/inner/deep.txt"),
        proven_mem_view_from_u8(PROVEN_LIT("deep")))), "write deep.txt");

    proven_result_walk_t walk = proven_fs_walk_open(alloc, PROVEN_LIT("ex_walk"),
                                                    PROVEN_FS_WALK_UNLIMITED);
    EXAMPLE_REQUIRE(proven_is_ok(walk.err), "the walk should open");

    proven_size_t files = 0;
    proven_size_t dirs = 0;
    proven_size_t unreadable = 0;
    proven_size_t total_bytes = 0;

    for (;;) {
        proven_fs_walk_entry_t entry = {0};
        proven_err_t err = proven_fs_walk_next(&walk.value, &entry);

        if (err == PROVEN_ERR_EOF) break;

        if (!proven_is_ok(err)) {
            /* A directory that could not be read, or one deeper than the walk's stack. It is
             * REPORTED, not skipped - `entry.path` says which one - and the walk goes on. A
             * tool that copies a tree should fail here; one that reports on a tree should
             * count it and say so. What it must not do is pretend it did not happen. */
            unreadable++;
            continue;
        }

        /* `entry.path` and `entry.name` are borrowed: they point into the walk's one reused
         * buffer and are valid until the next call. Copy them if you need them longer. */
        if (entry.type == PROVEN_FS_TYPE_DIR) {
            dirs++;
        } else if (entry.type == PROVEN_FS_TYPE_FILE) {
            files++;
            total_bytes += entry.size;
        }
    }

    proven_fs_walk_close(&walk.value);

    EXAMPLE_REQUIRE(files == 2, "two files: top.txt and inner/deep.txt");
    EXAMPLE_REQUIRE(dirs == 1, "one directory: inner");
    EXAMPLE_REQUIRE(unreadable == 0, "and nothing in this tree is unreadable");
    EXAMPLE_REQUIRE(total_bytes == 7, "three bytes plus four");

    /* Depth-limited: max_depth 0 reports what is directly inside the root and descends
     * nowhere. The directory at the limit is still an entry, so it is still reported. */
    walk = proven_fs_walk_open(alloc, PROVEN_LIT("ex_walk"), 0);
    EXAMPLE_REQUIRE(proven_is_ok(walk.err), "the shallow walk should open");

    proven_size_t shallow = 0;
    for (;;) {
        proven_fs_walk_entry_t entry = {0};
        proven_err_t err = proven_fs_walk_next(&walk.value, &entry);
        if (err == PROVEN_ERR_EOF) break;
        if (proven_is_ok(err)) shallow++;
    }
    proven_fs_walk_close(&walk.value);

    EXAMPLE_REQUIRE(shallow == 2, "top.txt and inner - but nothing inside inner");

    (void)proven_fs_remove(alloc, PROVEN_LIT("ex_walk/inner/deep.txt"));
    (void)proven_fs_remove(alloc, PROVEN_LIT("ex_walk/top.txt"));
    (void)proven_fs_remove(alloc, PROVEN_LIT("ex_walk/inner"));
    (void)proven_fs_remove(alloc, PROVEN_LIT("ex_walk"));

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

- **A writer that has failed stays failed.** Once a writer has lost bytes — a buffered
  writer whose sink died, a fixed buffer that overflowed — the stream it was producing has
  a hole in it that the receiver cannot see, so every later write and flush returns the
  error. A shorter chunk that *would* fit is refused too: writing it would put it after the
  hole, and the result would look like complete output. There is no `clear()`: if you have a
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

### The structures you hold

The two **handles** are passed by value and are cheap:

```text
typedef struct {
    void *ctx;
    proven_result_size_t (*write_fn)(void *ctx, proven_mem_view_t chunk);
    proven_err_t (*flush_fn)(void *ctx);   /* may be NULL: this sink holds nothing back */
} proven_writer_t;

typedef struct {
    void *ctx;
    proven_result_size_t (*read_fn)(void *ctx, proven_mem_mut_t dest);
} proven_reader_t;
```

`write_fn` reports **how much went out even when it then fails**, and that is not a nicety: a
write to a pipe or a full disk really does put some bytes out and then fail. A buffered writer
built on the tidier "all or nothing" lie kept its whole buffer on failure and re-sent it on the
next flush — a 6000-byte payload arrived as 10,096 bytes with the first 4096 **duplicated**.
Losing data is bad; silently doubling it is worse, because the receiver cannot tell.

Everything else is [caller-owned state](manual.md#42-caller-owned-state-no-destroy-do-not-copy) —
`proven_writer_buf_t`, `proven_writer_u8str_t`, `proven_writer_buffered_t`,
`proven_reader_view_t`, `proven_reader_buffered_t`. They allocate nothing, they have no destroy,
and **they must not be copied or moved** while a handle points into them.

### Cautions, and what goes wrong

**A buffered writer that is never flushed is output that never happened.** There is no hidden
state, so there is no destructor to flush it for you — and nothing here registers an `atexit`
handler, because a library that owns your process is a library you cannot reason about.

Wrong:

```text
proven_writer_buffered_t st;
proven_writer_t w = proven_writer_buffered(&st, inner, buf);
(void)proven_fprintln(w, "the important line");
return;                       /* wrong: the buffer dies with the frame, and so does the line */
```

Correct — flush before the buffer or the inner sink goes away:

```c
proven_byte_t buf[256];
proven_sysio_out_t out;
proven_writer_t w = proven_sysio_stdout_buffered(&out,
    (proven_mem_mut_t){ .ptr = buf, .size = sizeof buf });
(void)proven_fprintln(w, "the important line");
(void)proven_writer_flush(w);          /* now it has happened */
```

**The line a reader hands you points into its buffer.** It is valid only until the next call.
That is what makes reading a million lines cost one buffer instead of a million allocations — and
it is a dangling pointer the moment you keep it.

Wrong:

```text
proven_u8str_view_t lines[100];
for (int i = 0; i < 100; ++i) {
    proven_result_u8str_view_t ln = proven_reader_read_line(&st);
    lines[i] = ln.val;        /* wrong: every entry aliases the SAME buffer, and the
                                 next read_line overwrites what the last one returned */
}
```

Correct: copy the bytes you need to keep (into a `proven_u8str_t`, an arena, wherever), before
you call again.

**A flush is not a durability barrier.** `proven_writer_flush` pushes a buffered writer's bytes
to the thing behind it; getting a *file's* bytes onto the disk is `proven_fs_sync`. They are
different operations, and one word could not honestly mean both — which is why the old
`proven_sysio_flush`, which claimed to be both and was neither, is gone.

**A line longer than the buffer is refused, not truncated.** `PROVEN_ERR_OUT_OF_BOUNDS` — and the
reader then stays wedged on that line: there is no resync, because a line you cannot hold is not
a line you can skip past without deciding what to do with the bytes. Size the buffer for the
input you expect. (A line that *exactly fills* the buffer is fine: the newline does not have to
fit too, and neither does a final line with no newline at all.)

## The standard streams

`stream.h` has writers, readers, buffered writers and a line reader. `sysio.h` has stdin,
stdout and stderr. Until they were introduced to each other, two things were simply not
possible — and one call was a lie.

**Reading stdin a line at a time had no route.** The most common thing a program does with
stdin, and the choices were the token scanner or reading the whole of a stream that may never
end. The bridge fixes that: a standard handle is parked in caller-owned storage, so the line
reader has something stable to point at.

```c
proven_byte_t buf[4096];
proven_sysio_lines_t lines;
if (proven_is_ok(proven_sysio_stdin_lines(&lines, (proven_mem_mut_t){ .ptr = buf, .size = sizeof buf }))) {
    for (;;) {
        proven_result_u8str_view_t line = proven_sysio_read_line(&lines);
        if (line.err == PROVEN_ERR_EOF) break;
        if (!proven_is_ok(line.err)) break;   /* OUT_OF_BOUNDS: a line longer than `buf` */
        /* `line.val` points INTO `buf` and is valid only until the next call. */
        proven_println("{}", PROVEN_ARG(line.val));
    }
}
```

It inherits the line reader's properties, which is the point of not writing a second one: the
view costs no allocation, `"\r\n"` is handled, a final line with no trailing newline is still
returned, and a line longer than your buffer is `PROVEN_ERR_OUT_OF_BOUNDS` — never a silently
truncated line.

**The formatter could not be aimed at a standard stream.** `proven_fprintln` takes a writer;
stdout was not one. Now it is, and it can be a *buffered* one — so a thousand small lines cost
one syscall instead of a thousand.

| | |
|---|---|
| `proven_sysio_stdout_writer(&st)` | An unbuffered writer over stdout. Every write is a write syscall. |
| `proven_sysio_stderr_writer(&st)` | The same for stderr — which is what you want for an error: it is out before the next line of code runs. |
| `proven_sysio_stdin_reader(&st)` | A reader over stdin. |
| `proven_sysio_stdout_buffered(&out, buf)` | stdout behind a buffered writer over a buffer you own. |
| `proven_sysio_file_buffered(&out, file, buf)` | The same over any open file. |

**And `flush` means something now.** `proven_sysio_flush` used to claim to flush a buffer that
did not exist: a no-op on POSIX, a *disk sync* on Windows. It is **deleted**. Pushing a
buffered writer's bytes to the OS is `proven_writer_flush`; pushing the OS's bytes to the disk
is `proven_fs_sync`. They are different operations and now say so.

> **You must flush a buffered writer.** Nothing reaches the terminal until the buffer fills or
> you flush it, and nothing in this library registers an `atexit` handler to do it behind your
> back — a library that owns your process is a library you cannot reason about. Buffered output
> that is never flushed is output that never happened. The direct calls (`proven_print`,
> `proven_println`, `proven_eprint`) remain unbuffered for exactly this reason: what they write
> is on its way out before they return.

### The structures you hold

```text
typedef struct { proven_file_t file; } proven_sysio_std_t;
    /* Storage for a standard handle, so a writer or reader has something stable to
       point at. proven_writer_from_file takes a proven_file_t * and the file must
       outlive the writer - so it cannot be a temporary. This is that storage. */

typedef struct {
    proven_sysio_std_t       std;
    proven_writer_buffered_t buffered;
} proven_sysio_out_t;        /* a buffered writer over a standard stream or a file */

typedef struct {
    proven_sysio_std_t       std;
    proven_reader_buffered_t buffered;
} proven_sysio_lines_t;      /* a line reader over a standard stream or a file */
```

All three are [caller-owned state](manual.md#42-caller-owned-state-no-destroy-do-not-copy).

### Cautions, and what goes wrong

**These state structs contain a pointer to themselves.** The writer you get back addresses
`&st->std.file` *inside* the struct you passed. Copy the struct, or return it by value, and the
writer still points at the original — which may be a dead frame.

Wrong — and an audit reproduced exactly this as a heap-use-after-free:

```text
proven_sysio_out_t out;
proven_writer_t w = proven_sysio_stdout_buffered(&out, buf);

proven_sysio_out_t copy = out;      /* wrong: `w` still points into `out` */
/* ... `out` goes out of scope ... */
(void)proven_writer_write_str(w, PROVEN_LIT("boom"));   /* writes through dead storage */
```

The one exception is `proven_sysio_lines_t`: `proven_sysio_read_line` re-binds it on every call,
so a line reader **may** be moved. That is a deliberate courtesy, because it takes its state by
pointer — the shape that says "relocatable" — and the library should not lay a trap in the shape
of a promise.

**A zero-initialised `proven_file_t` is not an invalid handle — on POSIX it is fd 0, which is
stdin.** The library cannot tell a handle you forgot to fill in from one that legitimately refers
to fd 0.

```text
proven_sysio_out_t out;
proven_file_t f = {0};                                  /* wrong: this is stdin */
proven_writer_t w = proven_sysio_file_buffered(&out, f, buf);   /* writes to fd 0 */
```

**Buffered stdout and unbuffered stderr do not interleave in the order you wrote them.** Anything
you buffer sits in your buffer while stderr goes straight out. Flush before you print an error
that is supposed to appear after your output.

## Randomness, by use case

There is no single "random". There are two jobs that look identical and are not:

| Your job | Use | Why |
|---|---|---|
| A key, a token, a nonce — anything an attacker must not guess. | `proven_random_bytes`, or a `proven_chacha_rng_t` seeded from it. | Only a cryptographic source is unguessable. |
| The same, on a target with no OS. | `proven_chacha_rng_t`, seeded from the board's own entropy. | ChaCha20 is pure arithmetic; it needs no OS. It is only as unguessable as its seed. |
| A simulation, a test, a game, a sample. | `proven_xoshiro256ss_t`. | Fast, and **reproducible**: the same seed replays the same run, which is what makes a failing test debuggable. |
| A number in a range, a shuffle, a float in [0,1). | `proven_rng_below`, `proven_rng_range`, `proven_rng_f64`, `proven_rng_shuffle` — over any source. | `% n` is biased, and everyone writes it anyway. These are not. |

The two requirements are in direct opposition. Reproducible means predictable, and predictable
is exactly what a token must not be: a few outputs of `proven_xoshiro256ss_t` reveal its entire
state and therefore every number it will ever produce. That is a feature for a simulation you
need to replay and a catastrophe for a session token — so the two carry names that cannot be
confused, and the choice is visible at the call site rather than buried in how something was
seeded.

**The trait is infallible.** `proven_rng_t` is a source of random bytes, and drawing from a
valid one cannot fail. That is not a simplification; it is where the failure went. Asking an
operating system for entropy *can* fail, so that failure is confined to exactly one place —
seeding — which you check once, at startup. Every draw downstream is total.

| | |
|---|---|
| `proven_random_bytes(buf, len)` | The OS CSPRNG. Returns `false` on failure; do not use `buf` then. `len == 0` is a successful no-op. |
| `proven_random_u64()` | One strong word from the OS, or `0` on failure. |
| `proven_chacha_rng_seed_from_entropy(&g)` | Seed the cryptographic generator from the entropy source. **This is the call that can fail.** |
| `proven_random_set_source(fn, ctx)` | Install the entropy source. The OS is already installed on a hosted target; a bare-metal target installs its board's TRNG. |
| `proven_chacha_rng_seed(&g, seed32)` | Seed it from 32 bytes you supply — a hardware entropy source on a board. Never the clock. |
| `proven_xoshiro256ss_seed(&g, seed)` | Seed the reproducible generator. Even seed 0 is fine: it is expanded through SplitMix64. |

### Where entropy comes from

Everything above is pure arithmetic — the generators, the helpers, and `proven_random_bytes`
itself. What differs by platform is the **entropy source** behind it, because that is the one
thing a program cannot compute for itself.

- **Hosted:** the OS CSPRNG is installed for you — `getrandom` on Linux, `getentropy` on the
  BSDs and macOS, `BCryptGenRandom` on Windows, and `/dev/urandom` where none of those exist.
  You call nothing.
- **Bare metal:** there is no source until you install one. A board *has* real entropy — an
  on-chip TRNG, a ring oscillator, an ADC's noise floor — and the library cannot know where.

```text
/* On a board: hand the library its hardware entropy, once, at startup.
 * (A listing, not a fragment: it defines a function, and `hardware_rng_read` is
 * whatever your SoC calls its entropy register.) */
static bool board_trng(void *ctx, void *buf, proven_size_t len) {
    (void)ctx;
    /* read the SoC's entropy register into buf; return false if it is not ready */
    return hardware_rng_read(buf, len);
}

proven_random_set_source(board_trng, NULL);

/* From here everything above works unchanged - including the one call that turns a few
 * hundred bytes of hardware entropy into an endless cryptographic stream. */
proven_chacha_rng_t g;
if (!proven_chacha_rng_seed_from_entropy(&g)) {
    /* the TRNG was not ready. The generator is INERT - it yields zeros and an invalid
     * trait - so ignoring this does not get you plausible-looking bytes. */
}
```

With no source installed, `proven_random_bytes` returns **false**. It does not fall back to a
clock-seeded PRNG, because that looks like success and is a security hole nothing reports — a
refusal is a fact a caller can act on.

There is deliberately **no built-in `RDRAND` / `RNDR` backend.** On a hosted target the OS
already mixes the CPU's instruction into its own pool, so calling it directly buys nothing and
costs you that mixing; and a raw hardware instruction used as the *sole* source is exactly the
arrangement people have argued about for a decade. If you want it, it is four lines behind this
hook — and then the choice is visibly yours.

### The structures you hold

All three are [caller-owned state](manual.md#42-caller-owned-state-no-destroy-do-not-copy): they
allocate nothing, there is nothing to destroy, and **copying one clones its sequence**.

```text
typedef struct { const proven_rng_vtable_t *vt; void *ctx; } proven_rng_t;
    /* The trait: two pointers, held by value. `ctx` points at one of the generators
       below, which must outlive it. Drawing from a VALID one cannot fail - that is
       the whole design: the failure lives in seeding, not in drawing. */

typedef struct { proven_u64 s[4]; } proven_xoshiro256ss_t;
    /* 256 bits of state. Reproducible, and NOT secret-grade. */

typedef struct {
    proven_u32    state[16];   /* the ChaCha state: constants, key, counter, nonce */
    proven_byte_t block[64];   /* the keystream block currently being handed out */
    proven_size_t used;        /* how much of it is spent */
    proven_u32    seeded;      /* set only by seeding. A zero-initialised struct is
                                  the shape of "never seeded", and must stay inert. */
} proven_chacha_rng_t;
```

### Reference

| API | Intent | Return |
|---|---|---|
| `proven_random_bytes(buf, len)` | Fill from the entropy source (the OS by default). **The one call that can fail.** | `bool`. On `false`, `buf` is unspecified and must not be used. `len == 0` succeeds. |
| `proven_random_u64()` | One strong word from the same source. | `proven_u64`, or `0` on failure — which is also a valid draw, so use `proven_random_bytes` when you must tell them apart. |
| `proven_random_set_source(fn, ctx)` | Install the entropy source. Not needed on a hosted target; this is how a board hands over its TRNG. | void. |
| `proven_xoshiro256ss_seed(&g, seed)` | Seed the reproducible generator. Any seed is fine — even 0; it is expanded through SplitMix64. | void. |
| `proven_xoshiro256ss_next(&g)` | The next word. The hot path: call it directly, not through the trait. | `proven_u64`. |
| `proven_xoshiro256ss_rng(&g)` | View it as a `proven_rng_t`, for the helpers. | `proven_rng_t`. |
| `proven_chacha_rng_seed(&g, seed32)` | Seed the cryptographic generator from 32 bytes of **real entropy** you supply. | void. |
| `proven_chacha_rng_seed_from_entropy(&g)` | Seed it from the installed source. **Check this.** | `bool`. On `false` the generator is left INERT — it yields zeros and an invalid trait. |
| `proven_chacha_rng_next/_fill` | Draw. Cannot fail once seeded. | `proven_u64` / void. |
| `proven_chacha_rng(&g)` | View it as a `proven_rng_t`. | `proven_rng_t` — **invalid** if the generator was never successfully seeded. |
| `proven_rng_u64(rng)` / `proven_rng_fill(rng, buf, len)` | Draw through the trait, from whichever generator. | `proven_u64` / void. `0` / no-op for an invalid source. |
| `proven_rng_below(rng, bound)` | Uniform in `[0, bound)`, **unbiased**. | `proven_u64`; `0` when `bound == 0`. |
| `proven_rng_range(rng, lo, hi)` | Uniform in `[lo, hi]`, inclusive. The full `INT64_MIN..INT64_MAX` span does not overflow. | `proven_i64`; `lo` if `hi < lo`. |
| `proven_rng_f64(rng)` | Uniform in `[0, 1)`. 53 bits; never returns `1.0`. | `double`. |
| `proven_rng_shuffle(rng, base, count, elem_size)` | An unbiased Fisher-Yates permutation, in place. | void. |

### Cautions, and what goes wrong

**Never generate a secret with `proven_xoshiro256ss_t`.** It is fast *because* it is
predictable: a handful of its outputs reveal its entire 256-bit state, and from the state every
number it will ever produce. The two generators carry names that cannot be confused for exactly
this reason.

Wrong — a session token an attacker can compute after watching a few:

```text
proven_xoshiro256ss_t g;
proven_xoshiro256ss_seed(&g, 12345);
proven_u64 session_token = proven_xoshiro256ss_next(&g);   /* wrong: predictable */
```

**Never seed the cryptographic generator from the clock, a counter, or a serial number.**
ChaCha20 is exactly as unguessable as its seed. A clock-derived seed produces a stream that
looks perfectly random and is not — which is worse than an obvious failure, because nothing
reports it.

```text
proven_byte_t seed[32] = { 0 };
memcpy(seed, &now_ns, sizeof now_ns);      /* wrong: ~20 bits of real entropy, and guessable */
proven_chacha_rng_seed(&g, seed);
```

**Check the seeding.** It is the only thing here that can fail, which is precisely why ignoring
it is tempting. If you do, the generator is inert and hands you zeros — a visibly dead value,
by design, rather than a plausible one.

Wrong:

```text
proven_chacha_rng_t g;
proven_chacha_rng_seed_from_entropy(&g);   /* wrong: the bool was the point */
proven_chacha_rng_fill(&g, key, 32);       /* key is now 32 zero bytes */
```

Correct:

```c
proven_chacha_rng_t g;
if (!proven_chacha_rng_seed_from_entropy(&g)) {
    /* No entropy. There is nothing safe to do here except refuse to continue. */
} else {
    proven_byte_t key[32];
    proven_chacha_rng_fill(&g, key, sizeof key);   /* cannot fail: it is seeded */
}
```

**`% n` is biased, and everyone writes it anyway.** Unless `n` divides 2^64 the low values come
up more often — invisible in a spot check, real in a shuffle or a sample.

```text
proven_u64 die = proven_rng_u64(rng) % 6 + 1;   /* wrong: 1 and 2 are slightly likelier */
```

Correct: `proven_rng_below(rng, 6) + 1`.

**Do not copy a seeded generator** unless you mean to clone its stream. Two "independent"
generators copied from one produce identical output — which is a feature for replaying a
simulation and a catastrophe for issuing two tokens.

Compiled and run by the test suite:

<!-- example: manual/examples/ex_05_random.c -->
```c
/*
 * Randomness, by use case. There is no single "random": there are two jobs that look
 * identical and are not, and picking the wrong one is the whole danger.
 *
 *   A key, a token, a nonce - anything an attacker must not guess - needs a CRYPTOGRAPHIC
 *   source. A simulation, a test, a game needs a REPRODUCIBLE one, because a failing run you
 *   cannot replay is a failing run you cannot debug. The two requirements are in direct
 *   opposition: reproducible means predictable, and predictable is exactly what a token must
 *   not be. So the library gives them different names, and the choice is visible here at the
 *   call site rather than buried in how something was seeded.
 */

int main(void) {
    /* ---- Job 1: a secret. The OS CSPRNG - and the one place randomness can fail. ---- */
    proven_byte_t key[32];
    EXAMPLE_REQUIRE(proven_random_bytes(key, sizeof key),
                    "the OS must give us strong bytes on a hosted platform");

    /* ---- Job 2: lots of cryptographic bytes, or any at all on a board with no OS.
     * ChaCha20 is pure arithmetic: seed it once from real entropy and it needs nothing from
     * the operating system afterwards - no syscall per draw, and it works on bare metal.
     * Seeding is the ONLY step that can fail, so it is the only one you have to check. ---- */
    proven_chacha_rng_t crypto;
    EXAMPLE_REQUIRE(proven_chacha_rng_seed_from_entropy(&crypto), "seed the CSPRNG from the OS, once");

    proven_byte_t token[16];
    proven_chacha_rng_fill(&crypto, token, sizeof token);   /* cannot fail: it is seeded */

    /* ---- Job 3: a REPRODUCIBLE run. xoshiro256** is fast and replays exactly from its seed,
     * which is what makes a failing simulation debuggable. It is NOT secret-grade: a few of
     * its outputs reveal its whole state. Never hand it a token to generate. ---- */
    proven_xoshiro256ss_t sim;
    proven_xoshiro256ss_seed(&sim, 12345);

    proven_xoshiro256ss_t replay;
    proven_xoshiro256ss_seed(&replay, 12345);
    EXAMPLE_REQUIRE(proven_xoshiro256ss_next(&sim) == proven_xoshiro256ss_next(&replay),
                    "the same seed replays the same run - that is the whole point");

    /* ---- The helpers work over ANY source, through the proven_rng_t trait. ---- */
    proven_rng_t rng = proven_xoshiro256ss_rng(&sim);

    /* A number in a range. `rng_u64() % 6` is what everyone writes, and it is BIASED unless
     * the bound divides 2^64 - the low values come up more often. This one is not. */
    for (int i = 0; i < 100; ++i) {
        proven_u64 die = proven_rng_below(rng, 6) + 1;
        EXAMPLE_REQUIRE(die >= 1 && die <= 6, "a die roll is 1..6, uniformly");
    }

    proven_i64 temperature = proven_rng_range(rng, -40, 85);
    EXAMPLE_REQUIRE(temperature >= -40 && temperature <= 85, "an inclusive range, both ends");

    double p = proven_rng_f64(rng);
    EXAMPLE_REQUIRE(p >= 0.0 && p < 1.0, "a double in [0, 1) - never 1.0");

    /* An unbiased shuffle: Fisher-Yates over the unbiased index above. The `% n` version of
     * this loop measurably favours some orderings. */
    int deck[10];
    for (int i = 0; i < 10; ++i) deck[i] = i;
    proven_rng_shuffle(rng, deck, 10, sizeof deck[0]);

    int sum = 0;
    for (int i = 0; i < 10; ++i) sum += deck[i];
    EXAMPLE_REQUIRE(sum == 45, "a shuffle is a permutation: every card is still there, once");

    /* The cryptographic generator satisfies the same trait, so the same helpers work over it
     * when the choice must be unguessable rather than merely uniform. */
    proven_rng_t secure = proven_chacha_rng(&crypto);
    proven_u64 unguessable_index = proven_rng_below(secure, 1000);
    EXAMPLE_REQUIRE(unguessable_index < 1000, "the helpers do not care which source they draw from");

    (void)token;
    (void)key;
    return EXAMPLE_OK();
}
```
