/*
 * A refused directory, mode, link or lock call says which refusal it was.
 *
 * proven_fs_mkdir, proven_fs_rmdir, proven_fs_chmod, proven_fs_link and proven_fs_lock took a
 * yes-or-no answer from the platform layer and turned every "no" into PROVEN_ERR_IO, and
 * proven_fs_rename did the same for a source that is not there. A caller creating the
 * directories of a path could not tell "already there" from "the disk failed" and had to stat
 * after every failure (reported by a downstream project, 2026-10-07). The reproducer is the
 * first section: the same proven_fs_mkdir twice.
 *
 * proven_fs_mkdir_all is the function that report asked for, and its cases are here too.
 */

#include "proven.h"
#include "proven_test.h"

#include <string.h>

#if defined(_WIN32) || defined(_WIN64)
#define PLATFORM_IS_POSIX 0
#else
#define PLATFORM_IS_POSIX 1
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#define ROOT "test_refusal_codes.d"
#define V(s) PROVEN_LIT(s)

static proven_allocator_t heap;

static void put(proven_u8str_view_t path) {
    proven_err_t e = proven_fs_write_file(heap, path, proven_mem_view_from_u8(V("x")));
    PROVEN_TEST_ASSERT(proven_is_ok(e), "a fixture file is written", "");
}

static bool is_dir(proven_u8str_view_t path) {
    proven_fs_stat_t st = {0};
    return proven_is_ok(proven_fs_stat(heap, path, &st)) && st.type == PROVEN_FS_TYPE_DIR;
}

/* Best effort: whatever an earlier, failed run left behind. */
static void sweep(void) {
    static const char *const files[] = {
        ROOT "/file", ROOT "/file2", ROOT "/full/inside", ROOT "/moved", ROOT "/lock",
        ROOT "/all/blocked", ROOT "/closed/in"
    };
    static const char *const dirs[] = {
        ROOT "/all/a/b/c", ROOT "/all/a/b", ROOT "/all/a", ROOT "/all/s/t", ROOT "/all/s",
        ROOT "/all/d/e", ROOT "/all/d", ROOT "/all/one", ROOT "/all",
        ROOT "/closed/in", ROOT "/closed", ROOT "/full", ROOT "/once", ROOT
    };
    (void)proven_fs_chmod(heap, V(ROOT "/closed"), (proven_fs_perms_t)0755);
    for (size_t i = 0; i < sizeof files / sizeof files[0]; ++i) {
        (void)proven_fs_remove(heap, proven_u8str_view_from_cstr(files[i]));
    }
    for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; ++i) {
        (void)proven_fs_rmdir(heap, proven_u8str_view_from_cstr(dirs[i]));
    }
}

/* True when a second holder, asking without waiting, was told PROVEN_ERR_BUSY. */
static bool second_locker_is_busy(proven_u8str_view_t path, proven_err_t *out_seen) {
#if PLATFORM_IS_POSIX
    /* POSIX record locks belong to the PROCESS, so the first holder has to be another one. */
    int ready[2], done[2];
    if (pipe(ready) != 0 || pipe(done) != 0) return false;
    pid_t child = fork();
    if (child < 0) return false;
    if (child == 0) {
        char byte = 'n';
        proven_result_file_t f = proven_fs_open(heap, path, PROVEN_FS_READ | PROVEN_FS_WRITE);
        if (proven_is_ok(f.err) && proven_is_ok(proven_fs_lock(f.value, PROVEN_FS_LOCK_EXCLUSIVE, true))) byte = 'y';
        (void)!write(ready[1], &byte, 1);
        (void)!read(done[0], &byte, 1);
        _exit(0);
    }
    char byte = 'n';
    bool held = read(ready[0], &byte, 1) == 1 && byte == 'y';
    proven_err_t seen = PROVEN_ERR_INVALID_STATE;
    if (held) {
        proven_result_file_t f = proven_fs_open(heap, path, PROVEN_FS_READ | PROVEN_FS_WRITE);
        if (proven_is_ok(f.err)) {
            seen = proven_fs_lock(f.value, PROVEN_FS_LOCK_EXCLUSIVE, false);
            (void)proven_fs_close(f.value);
        }
    }
    (void)!write(done[1], &byte, 1);
    int status = 0;
    (void)waitpid(child, &status, 0);
    close(ready[0]); close(ready[1]); close(done[0]); close(done[1]);
    *out_seen = seen;
    return held && seen == PROVEN_ERR_BUSY;
#else
    /* Windows byte-range locks belong to the HANDLE: two handles in one process conflict. */
    proven_result_file_t a = proven_fs_open(heap, path, PROVEN_FS_READ | PROVEN_FS_WRITE);
    proven_result_file_t b = proven_fs_open(heap, path, PROVEN_FS_READ | PROVEN_FS_WRITE);
    if (!proven_is_ok(a.err) || !proven_is_ok(b.err)) return false;
    bool held = proven_is_ok(proven_fs_lock(a.value, PROVEN_FS_LOCK_EXCLUSIVE, true));
    proven_err_t seen = proven_fs_lock(b.value, PROVEN_FS_LOCK_EXCLUSIVE, false);
    if (held) (void)proven_fs_lock(a.value, PROVEN_FS_LOCK_UNLOCK, false);
    (void)proven_fs_close(a.value);
    (void)proven_fs_close(b.value);
    *out_seen = seen;
    return held && seen == PROVEN_ERR_BUSY;
#endif
}

int main(void) {
    PROVEN_TEST_SUITE("a refused directory, mode, link or lock call says which refusal it was",
        "Already there, not there, not allowed and held by someone else are four different answers; PROVEN_ERR_IO is none of them.",
        "A failure names the call and the code it should have returned. PROVEN_ERR_IO in its place means the platform layer's reason was dropped again.");

    heap = proven_heap_allocator();
    sweep();
    PROVEN_TEST_ASSERT(proven_is_ok(proven_fs_mkdir(heap, V(ROOT))), "the fixture directory is created", "");
    proven_err_t e;

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("proven_fs_mkdir",
        "The reproducer: the second mkdir of one path. Then a file in the way, and a missing parent.",
        "Inspect proven_sys_fs_mkdir_checked and proven_fs_mkdir.");
    // ---------------------------------------------------------------
    PROVEN_TEST_ASSERT(proven_fs_mkdir(heap, V(ROOT "/once")) == PROVEN_OK, "the first mkdir succeeds", "");
    e = proven_fs_mkdir(heap, V(ROOT "/once"));
    PROVEN_TEST_ASSERT(e == PROVEN_ERR_EXISTS, "the second mkdir of the same path is PROVEN_ERR_EXISTS", "");
    put(V(ROOT "/file"));
    e = proven_fs_mkdir(heap, V(ROOT "/file"));
    PROVEN_TEST_ASSERT(e == PROVEN_ERR_EXISTS, "mkdir over a file is PROVEN_ERR_EXISTS", "");
    e = proven_fs_mkdir(heap, V(ROOT "/absent/child"));
    PROVEN_TEST_ASSERT(e == PROVEN_ERR_NOT_FOUND, "mkdir under a missing parent is PROVEN_ERR_NOT_FOUND", "");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("proven_fs_rmdir",
        "A name that is not there is NOT_FOUND. A directory that is not empty stays PROVEN_ERR_IO: no code names that case.",
        "Inspect proven_sys_fs_rmdir_checked and proven_fs_rmdir.");
    // ---------------------------------------------------------------
    e = proven_fs_rmdir(heap, V(ROOT "/absent"));
    PROVEN_TEST_ASSERT(e == PROVEN_ERR_NOT_FOUND, "rmdir of a missing name is PROVEN_ERR_NOT_FOUND", "");
    PROVEN_TEST_ASSERT(proven_fs_mkdir(heap, V(ROOT "/full")) == PROVEN_OK, "a second directory is created", "");
    put(V(ROOT "/full/inside"));
    e = proven_fs_rmdir(heap, V(ROOT "/full"));
    PROVEN_TEST_ASSERT(e == PROVEN_ERR_IO, "rmdir of a directory that is not empty is PROVEN_ERR_IO", "");
    PROVEN_TEST_ASSERT(is_dir(V(ROOT "/full")), "and the directory is still there", "");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("proven_fs_chmod, proven_fs_link and proven_fs_rename",
        "A missing name is NOT_FOUND for each; a hard link onto a name that is taken is EXISTS.",
        "Inspect proven_sys_fs_chmod_checked, proven_sys_fs_link_checked and proven_sys_fs_rename_checked.");
    // ---------------------------------------------------------------
    e = proven_fs_chmod(heap, V(ROOT "/absent"), (proven_fs_perms_t)0644);
    PROVEN_TEST_ASSERT(e == PROVEN_ERR_NOT_FOUND, "chmod of a missing name is PROVEN_ERR_NOT_FOUND", "");
    e = proven_fs_link(heap, V(ROOT "/absent"), V(ROOT "/file2"));
    PROVEN_TEST_ASSERT(e == PROVEN_ERR_NOT_FOUND, "a hard link to a missing name is PROVEN_ERR_NOT_FOUND", "");
    put(V(ROOT "/file2"));
    e = proven_fs_link(heap, V(ROOT "/file"), V(ROOT "/file2"));
    PROVEN_TEST_ASSERT(e == PROVEN_ERR_EXISTS, "a hard link onto a name that is taken is PROVEN_ERR_EXISTS", "");
    e = proven_fs_rename(heap, V(ROOT "/absent"), V(ROOT "/moved"));
    PROVEN_TEST_ASSERT(e == PROVEN_ERR_NOT_FOUND, "a rename of a missing name is PROVEN_ERR_NOT_FOUND", "");

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("proven_fs_lock",
        "A lock someone else holds, asked for without waiting, is BUSY: the answer is to try again later.",
        "Inspect proven_sys_fs_lock_checked. The first holder is a child process on POSIX and a second handle on Windows.");
    // ---------------------------------------------------------------
    put(V(ROOT "/lock"));
    {
        proven_err_t seen = PROVEN_OK;
        bool busy = second_locker_is_busy(V(ROOT "/lock"), &seen);
        PROVEN_TEST_INFO("the second holder was answered {}", PROVEN_ARG((int)seen));
        PROVEN_TEST_ASSERT(busy, "a held lock, asked for without waiting, is PROVEN_ERR_BUSY", "");
    }

#if PLATFORM_IS_POSIX
    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a directory that refuses writing",
        "mkdir inside a directory without the write bit is PERMISSION. POSIX only, and not as root, which modes do not stop.",
        "Inspect the EACCES branch of the platform layer's reason mapping.");
    // ---------------------------------------------------------------
    if (geteuid() == 0) {
        PROVEN_TEST_INFO("SKIP: running as root, which is not refused by directory modes.");
    } else {
        PROVEN_TEST_ASSERT(proven_fs_mkdir(heap, V(ROOT "/closed")) == PROVEN_OK, "a third directory is created", "");
        PROVEN_TEST_ASSERT(proven_fs_chmod(heap, V(ROOT "/closed"), (proven_fs_perms_t)0555) == PROVEN_OK, "and made read-only", "");
        proven_result_file_t probe = proven_fs_open(heap, V(ROOT "/closed/in"), PROVEN_FS_WRITE | PROVEN_FS_CREATE);
        if (proven_is_ok(probe.err)) {
            (void)proven_fs_close(probe.value);
            (void)proven_fs_remove(heap, V(ROOT "/closed/in"));
            PROVEN_TEST_INFO("SKIP: this filesystem did not honour a 0555 directory mode.");
        } else {
            e = proven_fs_mkdir(heap, V(ROOT "/closed/in"));
            PROVEN_TEST_ASSERT(e == PROVEN_ERR_PERMISSION, "mkdir in a read-only directory is PROVEN_ERR_PERMISSION", "");
            e = proven_fs_mkdir_all(heap, V(ROOT "/closed/in/deeper"));
            PROVEN_TEST_ASSERT(e == PROVEN_ERR_PERMISSION, "and so is proven_fs_mkdir_all through it", "");
        }
        PROVEN_TEST_ASSERT(proven_fs_chmod(heap, V(ROOT "/closed"), (proven_fs_perms_t)0755) == PROVEN_OK, "the mode is put back", "");
        PROVEN_TEST_ASSERT(proven_fs_rmdir(heap, V(ROOT "/closed")) == PROVEN_OK, "and the directory removed", "");
    }
#endif

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("proven_fs_mkdir_all",
        "Every missing directory of the path is created; one that is already a directory is not an error; a file in the way is.",
        "Inspect proven_fs_mkdir_all in src/proven/fs.c. PROVEN_OK over a file means the stat after EXISTS was dropped.");
    // ---------------------------------------------------------------
    e = proven_fs_mkdir_all(heap, V(ROOT "/all/a/b/c"));
    PROVEN_TEST_ASSERT(e == PROVEN_OK, "four missing levels are created in one call", "");
    PROVEN_TEST_ASSERT(is_dir(V(ROOT "/all")) && is_dir(V(ROOT "/all/a")) && is_dir(V(ROOT "/all/a/b")) && is_dir(V(ROOT "/all/a/b/c")),
        "and each one is a directory", "");
    e = proven_fs_mkdir_all(heap, V(ROOT "/all/a/b/c"));
    PROVEN_TEST_ASSERT(e == PROVEN_OK, "the same call again is PROVEN_OK", "");
    e = proven_fs_mkdir_all(heap, V(ROOT "/all/one"));
    PROVEN_TEST_ASSERT(e == PROVEN_OK && is_dir(V(ROOT "/all/one")), "one missing level under an existing parent", "");
    e = proven_fs_mkdir_all(heap, V(ROOT "/all/s/t/"));
    PROVEN_TEST_ASSERT(e == PROVEN_OK && is_dir(V(ROOT "/all/s/t")), "a trailing separator is accepted", "");
    e = proven_fs_mkdir_all(heap, V(ROOT "/all//d//e"));
    PROVEN_TEST_ASSERT(e == PROVEN_OK && is_dir(V(ROOT "/all/d/e")), "a doubled separator is accepted", "");
    e = proven_fs_mkdir_all(heap, V("."));
    PROVEN_TEST_ASSERT(e == PROVEN_OK, "the current directory is already there", "");

    put(V(ROOT "/all/blocked"));
    e = proven_fs_mkdir_all(heap, V(ROOT "/all/blocked"));
    PROVEN_TEST_ASSERT(e == PROVEN_ERR_EXISTS, "a file at the last name is PROVEN_ERR_EXISTS, not PROVEN_OK", "");
    e = proven_fs_mkdir_all(heap, V(ROOT "/all/blocked/x/y"));
    PROVEN_TEST_ASSERT(e == PROVEN_ERR_EXISTS, "a file in the middle of the path is PROVEN_ERR_EXISTS too", "");
    PROVEN_TEST_ASSERT(!is_dir(V(ROOT "/all/blocked")), "and the file is still a file", "");
    e = proven_fs_mkdir_all(heap, V(""));
    PROVEN_TEST_ASSERT(e == PROVEN_ERR_INVALID_ARG, "the empty path is PROVEN_ERR_INVALID_ARG", "");

    sweep();
    PROVEN_TEST_ASSERT(!is_dir(V(ROOT)), "the fixture directory is gone at the end", "");
    PROVEN_TEST_PASS("refusal codes for mkdir, rmdir, chmod, link, rename and lock, and proven_fs_mkdir_all");
    return 0;
}
