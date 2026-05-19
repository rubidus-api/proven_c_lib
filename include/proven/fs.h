#ifndef PROVEN_FS_H
#define PROVEN_FS_H

#include "proven/types.h"
#include "proven/error.h"
#include "proven/u8str.h"
#include "proven/memory.h"
#include "proven/array.h"

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
 */
[[nodiscard]]
proven_result_size_t proven_fs_size(proven_file_t file);

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
    proven_fs_perms_t perms;
    proven_i64 created_at;
    proven_i64 modified_at;
    unsigned long long dev;
    unsigned long long ino;
} proven_fs_stat_t;

/**
 * @brief Get detailed file/directory information.
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
 * @note If the file size changes concurrently, this reads up to the original size
 *       or until EOF is reached, returning the actual number of bytes read.
 *       If the final shrink realloc fails, this returns the original larger 
 *       allocation with `value.size` correctly set to the actual bytes read.
 */
[[nodiscard]]
proven_result_mem_mut_t proven_fs_read_all(proven_allocator_t alloc, proven_u8str_view_t path);

#endif /* PROVEN_FS_H */
