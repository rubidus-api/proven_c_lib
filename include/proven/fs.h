#ifndef PROVEN_FS_H
#define PROVEN_FS_H

#include "proven/types.h"
#include "proven/error.h"
#include "proven/u8str.h"
#include "proven/memory.h"
#include "proven/array.h"
#include "proven/scan.h"   /* proven_result_u64_t */

/**
 * @file fs.h
 * @brief High-level File System I/O interface.
 */

// We redefine this here for internal PAL coherence or just use void*
typedef struct {
    union {
        void *ptr;
        int   fd;
    } internal;
} proven_fs_handle_t;

typedef enum {
    PROVEN_FS_READ   = 1 << 0,
    PROVEN_FS_WRITE  = 1 << 1,
    PROVEN_FS_APPEND = 1 << 2,
    PROVEN_FS_CREATE = 1 << 3,
    PROVEN_FS_TRUNC  = 1 << 4,
    PROVEN_FS_CREATE_NEW = 1 << 5
} proven_fs_mode_t;

/*
 * Raw filesystem helpers do not sanitize untrusted paths, enforce root
 * confinement, or defend against symlink-race TOCTOU. Callers handling
 * untrusted paths must validate them first.
 */

typedef enum {
    PROVEN_FS_TYPE_FILE,
    PROVEN_FS_TYPE_DIR,
    PROVEN_FS_TYPE_OTHER
} proven_fs_type_t;

/**
 * @brief Metadata for a single filesystem entry.
 */
typedef struct {
    proven_u8str_t   name; // Owned by the list/caller context
    proven_fs_type_t type;
    proven_size_t    size;
} proven_fs_entry_t;

/**
 * @brief Opaque handle representing an open file.
 */
typedef proven_fs_handle_t proven_file_t;

typedef struct {
    proven_err_t  err;
    proven_file_t value;
} proven_result_file_t;

// -------------------------------------------------------------
// Core File Operations
// -------------------------------------------------------------

/**
 * @brief Opens a file at the specified path with the given mode.
 */
[[nodiscard]]
proven_result_file_t proven_fs_open(proven_allocator_t scratch, proven_u8str_view_t path, proven_fs_mode_t mode);

/**
 * @brief Closes an open file handle.
 */
void proven_fs_close(proven_file_t file);

/**
 * @brief Reads data from an open file into a mutable slice.
 */
[[nodiscard]]
proven_result_size_t proven_fs_read(proven_file_t file, proven_mem_mut_t dest);

/**
 * @brief Writes data from a view into an open file.
 */
[[nodiscard]]
proven_result_size_t proven_fs_write(proven_file_t file, proven_mem_view_t src);

/**
 * @brief Writes the entire src view into an open file, retrying on partial writes.
 */
[[nodiscard]]
proven_err_t proven_fs_write_all(proven_file_t file, proven_mem_view_t src);

/**
 * @brief Retrieves the size of an open file in bytes.
 *
 * @note This is the file's length, not the write cursor. Use proven_fs_tell for
 *       the position.
 */
[[nodiscard]]
proven_result_size_t proven_fs_size(proven_file_t file);

// -------------------------------------------------------------
// Position, size, and durability
// -------------------------------------------------------------

typedef enum {
    PROVEN_FS_SEEK_SET = 0,   /**< from the beginning of the file */
    PROVEN_FS_SEEK_CUR = 1,   /**< from the current position */
    PROVEN_FS_SEEK_END = 2    /**< from the end; a negative offset goes backwards */
} proven_fs_whence_t;

/**
 * @brief Moves the file position. Returns the resulting absolute offset.
 *
 * @note A handle that cannot seek - a pipe, a FIFO, a terminal - returns
 *       PROVEN_ERR_UNSUPPORTED, not PROVEN_ERR_IO. Not being seekable is a
 *       property of the thing, not a failure of the call, and code that adapts to
 *       it needs to be able to tell the two apart.
 * @note Seeking past the end of a file is legal and does not extend it; a write
 *       there does, leaving a hole.
 */
[[nodiscard]]
proven_result_u64_t proven_fs_seek(proven_file_t file, proven_i64 offset, proven_fs_whence_t whence);

/**
 * @brief Returns the current file position without moving it.
 */
[[nodiscard]]
proven_result_u64_t proven_fs_tell(proven_file_t file);

/**
 * @brief Sets the file's length, growing it with zeros or discarding the tail.
 *
 * @note This is O(1). Truncating used to require reading the whole file and
 *       rewriting the part you wanted to keep - an O(n) copy for an O(1)
 *       operation - because there was no way to say this.
 * @note The file position is unchanged. If it now sits past the end, that is
 *       allowed; a write there extends the file again.
 */
[[nodiscard]]
proven_err_t proven_fs_truncate(proven_file_t file, proven_u64 length);

/**
 * @brief Reads at an absolute offset without moving the file position.
 *
 * @note This is what makes concurrent reads of one file handle safe: two threads
 *       sharing a handle cannot race on a cursor that neither of them moves.
 * @note Same EOF contract as proven_fs_read: end of file is PROVEN_ERR_EOF, not a
 *       zero-byte success.
 * @note Not available on a pipe: PROVEN_ERR_UNSUPPORTED.
 */
[[nodiscard]]
proven_result_size_t proven_fs_pread(proven_file_t file, proven_mem_mut_t dest, proven_u64 offset);

/**
 * @brief Writes at an absolute offset without moving the file position.
 *
 * @note May write fewer bytes than requested, like proven_fs_write.
 * @note Not available on a pipe: PROVEN_ERR_UNSUPPORTED.
 */
[[nodiscard]]
proven_result_size_t proven_fs_pwrite(proven_file_t file, proven_mem_view_t src, proven_u64 offset);

/**
 * @brief Forces this file's data to the storage device (fsync).
 *
 * Until now the library had no way to do this at any price: it imported no fsync
 * and no fdatasync, so a caller who wanted their bytes on the platter simply could
 * not ask. proven_sysio_flush was not it - that call does nothing.
 *
 * @note This is expensive, and it is meant to be. Call it when you have decided
 *       that losing the data would be worse than the wait.
 */
[[nodiscard]]
proven_err_t proven_fs_sync(proven_file_t file);

/**
 * @brief Forces a directory's own metadata to the storage device.
 *
 * A rename is only durable once the *directory* has reached the disk. So an atomic
 * rewrite that must survive a power cut needs both: sync the new file's data, then
 * rename, then sync the directory the rename happened in. Syncing the file alone
 * leaves a window in which the data is safe and the name that points at it is not.
 *
 * @note POSIX only in effect. On Windows a directory handle cannot be flushed this
 *       way, and this returns PROVEN_ERR_UNSUPPORTED rather than pretending.
 */
[[nodiscard]]
proven_err_t proven_fs_sync_dir(proven_allocator_t scratch, proven_u8str_view_t path);

// -------------------------------------------------------------
// Advanced Filesystem Management
// -------------------------------------------------------------

/**
 * @brief Renames or moves a file/directory from src to dest.
 */
[[nodiscard]]
proven_err_t proven_fs_rename(proven_allocator_t scratch, proven_u8str_view_t src, proven_u8str_view_t dest);

/**
 * @brief Deletes a file at the specified path.
 */
[[nodiscard]]
proven_err_t proven_fs_remove(proven_allocator_t scratch, proven_u8str_view_t path);

/**
 * @brief Copies a file from src to dest using custom memory buffers for efficiency.
 */
[[nodiscard]]
proven_err_t proven_fs_copy(proven_allocator_t temp_alloc, proven_u8str_view_t src, proven_u8str_view_t dest);

/**
 * @brief Creates a directory.
 */
[[nodiscard]]
proven_err_t proven_fs_mkdir(proven_allocator_t scratch, proven_u8str_view_t path);

/**
 * @brief Removes an empty directory.
 */
[[nodiscard]]
proven_err_t proven_fs_rmdir(proven_allocator_t scratch, proven_u8str_view_t path);

// -------------------------------------------------------------
// Directory Iteration
// -------------------------------------------------------------

/**
 * @brief Lists the contents of a directory into an array of proven_fs_entry_t.
 * @note This uses the provided allocator to store strings for entry names.
 *       Entries should be sorted by name by default.
 */
[[nodiscard]]
proven_result_array_t proven_fs_list(proven_allocator_t alloc, proven_u8str_view_t path);

/**
 * @brief Destroys an array of proven_fs_entry_t, freeing all internal strings.
 */
void proven_fs_list_destroy(proven_allocator_t alloc, proven_array_t *list);

// -------------------------------------------------------------
// Permissions & Locking
// -------------------------------------------------------------

/**
 * @brief Portable file permissions (POSIX-style bitmask).
 */
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
    PROVEN_FS_PERM_DEFAULT = PROVEN_FS_PERM_OWNER_R | PROVEN_FS_PERM_OWNER_W | PROVEN_FS_PERM_GROUP_R | PROVEN_FS_PERM_OTHER_R
} proven_fs_perms_t;

/**
 * @brief Lock types for file synchronization.
 */
typedef enum {
    PROVEN_FS_LOCK_SHARED,    // Read lock (Multiple readers allowed)
    PROVEN_FS_LOCK_EXCLUSIVE, // Write lock (Only one owner allowed)
    PROVEN_FS_LOCK_UNLOCK     // Release lock
} proven_fs_lock_type_t;

/**
 * @brief Set file permissions.
 */
[[nodiscard]]
proven_err_t proven_fs_chmod(proven_allocator_t scratch, proven_u8str_view_t path, proven_fs_perms_t perms);

/**
 * @brief Apply or release a file lock.
 */
[[nodiscard]]
proven_err_t proven_fs_lock(proven_file_t file, proven_fs_lock_type_t type, bool wait);

#define PROVEN_FS_PATH_SEP '/'

/**
 * @brief Detailed file information.
 */
typedef struct {
    proven_size_t size;
    proven_fs_type_t type;
    proven_fs_perms_t perms;  /* permission bits only; the file type is in `type` */
    proven_i64 created_at;
    proven_i64 modified_at;
    unsigned long long dev;
    unsigned long long ino;
    unsigned long long uid;   /* owner id: POSIX st_uid; 0 on Windows (no uid/gid) */
    unsigned long long gid;   /* group id: POSIX st_gid; 0 on Windows */
} proven_fs_stat_t;

/**
 * @brief Get detailed file/directory information.
 *
 * @note `perms` carries only the nine permission bits, so it can be handed
 *       straight back to proven_fs_chmod. It used to carry the raw POSIX
 *       st_mode, whose file-type bits chmod rejects - which made the obvious
 *       round-trip fail for every real file. Read the file type from `type`.
 */
[[nodiscard]]
proven_err_t proven_fs_stat(proven_allocator_t scratch, proven_u8str_view_t path, proven_fs_stat_t *out_stat);

/**
 * @brief Create a symbolic link.
 */
[[nodiscard]]
proven_err_t proven_fs_symlink(proven_allocator_t scratch, proven_u8str_view_t target, proven_u8str_view_t linkpath);

/**
 * @brief Create a hard link.
 */
[[nodiscard]]
proven_err_t proven_fs_link(proven_allocator_t scratch, proven_u8str_view_t oldpath, proven_u8str_view_t newpath);

/**
 * @brief Check if a path is absolute.
 */
[[nodiscard]]
bool proven_fs_is_absolute(proven_u8str_view_t path);

// -------------------------------------------------------------
// Utility Wrappers
// -------------------------------------------------------------

/**
 * @brief Reads the entire contents of a file into a newly allocated buffer.
 *
 * @note Reads to EOF, not to a pre-measured size. The file's reported size only
 *       seeds the initial capacity, so a regular file is still read in one
 *       allocation and one pass - but a source whose size cannot be known up
 *       front (pipe, FIFO, /proc entry, character device) is read correctly
 *       rather than returning empty, and a regular file that grows mid-read is
 *       not silently truncated.
 * @note An empty source yields `{.ptr = NULL, .size = 0}` with PROVEN_OK.
 * @note A regular file of known size costs exactly one allocation: EOF is
 *       confirmed with a one-byte probe rather than by growing the buffer.
 *       The buffer only grows if the source really does outrun its reported
 *       size. If the final shrink realloc fails, the larger allocation is
 *       returned with `value.size` correctly set to the bytes read.
 */
[[nodiscard]]
proven_result_mem_mut_t proven_fs_read_all(proven_allocator_t alloc, proven_u8str_view_t path);

/**
 * @brief Reads the entire contents of a file into a newly allocated owned string.
 *
 * The whole-file read most callers actually want: the result is a
 * NUL-terminated `proven_u8str_t`, so `proven_u8str_as_view` and
 * `proven_u8str_as_cstr` work on it without a second copy. The terminator is
 * reserved up front, so this costs no extra allocation over proven_fs_read_all.
 *
 * @note Contents are not validated as UTF-8; the bytes are returned as they are.
 * @note Same EOF and allocator semantics as proven_fs_read_all.
 * @note Destroy the result with proven_u8str_destroy.
 */
[[nodiscard]]
proven_result_u8str_t proven_fs_read_all_u8str(proven_allocator_t alloc, proven_u8str_view_t path);

/**
 * @brief Writes a buffer to a path in one call, creating or truncating the file.
 *
 * @note Not atomic: a reader can observe a partially written file, and a failure
 *       mid-write leaves the file truncated. Use proven_fs_write_file_atomic
 *       when a concurrent reader must never see a half-written file.
 */
[[nodiscard]]
proven_err_t proven_fs_write_file(proven_allocator_t scratch, proven_u8str_view_t path, proven_mem_view_t data);

/**
 * @brief Writes a buffer to a path via a sibling temp file and a rename.
 *
 * A concurrent reader sees either the entire old file or the entire new one,
 * never a partial write. On any failure the temp file is removed and the
 * original file is left untouched.
 *
 * @note Permissions are preserved. If the target exists, its mode is copied onto
 *       the temp file before the rename, so rewriting a 0600 file does not
 *       republish it as 0644. A new file gets the process default, as with
 *       proven_fs_write_file.
 * @note Atomic with respect to readers, not durable across power loss: proven
 *       exposes no fsync, so the rename may reach the disk before the data.
 * @note Replaces, rather than follows, a symbolic link at `path`: the rename
 *       leaves a regular file where the link was. proven_fs_write_file writes
 *       *through* the link instead. Pick accordingly.
 * @note Needs write permission on the containing directory, and a filesystem
 *       where the temp sibling and the target share a mount (they always do,
 *       since the temp file is created next to the target).
 */
[[nodiscard]]
proven_err_t proven_fs_write_file_atomic(proven_allocator_t scratch, proven_u8str_view_t path, proven_mem_view_t data);

/**
 * @brief Like proven_fs_write_file_atomic, but the bytes are on the disk before it
 *        returns.
 *
 * Atomicity and durability are different promises, and conflating them is how data
 * gets lost. `write_file_atomic` guarantees that a *reader* never sees a half-written
 * file. It does not guarantee that the file survives a power cut - the kernel may
 * still be holding the bytes, and the rename may reach the disk before the data it
 * points at.
 *
 * This call closes that window, in the only order that works:
 *
 *   1. write the temp file, then **fsync it** - the data is on the disk;
 *   2. rename it over the target - readers now see the new file, atomically;
 *   3. **fsync the directory** - the rename itself is now on the disk.
 *
 * Syncing the file but not the directory leaves a crash window in which the bytes are
 * safe and the name that points at them is not, which is the corruption an atomic
 * write exists to prevent.
 *
 * @note This is slow, and it is meant to be: it waits for the storage device, twice.
 *       Use it when losing the write would be worse than the wait, and use
 *       proven_fs_write_file_atomic when it would not.
 * @note On a platform with no directory sync (Windows), step 3 is skipped rather than
 *       failing the write: the write did happen and is atomic; only the
 *       crash-durability of the rename is unavailable.
 */
[[nodiscard]]
proven_err_t proven_fs_write_file_durable(proven_allocator_t scratch, proven_u8str_view_t path, proven_mem_view_t data);

#endif /* PROVEN_FS_H */
