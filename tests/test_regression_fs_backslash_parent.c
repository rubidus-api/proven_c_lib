/*
 * RFC-0006 H-003: a durable write synced the wrong directory when the filename contained
 * a backslash.
 *
 * internal_parent_dir treated '/' and '\' as separators on every platform. On POSIX a
 * backslash is an ordinary character in a filename, so "d/a\b" is one file called "a\b"
 * inside "d". The library wrote and renamed the right file and then tried to sync "d/a" -
 * which is not its parent.
 *
 * Two ways that goes wrong, and both are worse than not trying:
 *   - "d/a" does not exist: the call returns an I/O error AFTER the rename has already
 *     published the new contents. The caller is told the write failed and the new bytes
 *     are on disk.
 *   - "d/a" does exist as a directory: some other directory is synced and the call
 *     reports a durability it did not achieve. Checking only for PROVEN_OK cannot see
 *     this, which is why this test observes WHICH directory was synced.
 *
 * The observation seam is a definition of open() in this test, which the linker binds the
 * platform layer's call to; it forwards to the real openat syscall and records the
 * directories opened. proven_fs_sync_dir opens the directory read-only to fsync it, so the
 * record says exactly what was synced, in order.
 */

#if !defined(_WIN32) && !defined(_WIN64)
#define _GNU_SOURCE   /* syscall(); this must precede every header */
#endif

#include "proven.h"
#include "proven_test.h"

#if defined(_WIN32) || defined(_WIN64)

int main(void) {
    PROVEN_TEST_SUITE("a backslash in a POSIX filename is not a separator (RFC-0006 H-003)",
        "On Windows both characters separate components, so there is nothing here to get wrong in this direction.",
        "The Windows path rules - drive roots, UNC shares, extended paths - need a native test, not this one.");
    PROVEN_TEST_INFO("SKIP: POSIX-only test. On Windows a backslash IS a separator and proven_fs_sync_dir is PROVEN_ERR_UNSUPPORTED.");
    PROVEN_TEST_PASS("skipped on Windows.");
    return 0;
}

#else

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <errno.h>

/* Directories opened during the call under test, oldest first. */
#define SYNCED_MAX 16
static char synced[SYNCED_MAX][512];
static int synced_len;
static bool recording;

static void record_reset(void) { synced_len = 0; }

int open(const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    int fd = (int)syscall(SYS_openat, AT_FDCWD, path, flags, (int)mode);
    if (recording && fd >= 0 && synced_len < SYNCED_MAX) {
        struct stat st;
        if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
            size_t n = strlen(path);
            if (n < sizeof synced[0]) {
                memcpy(synced[synced_len], path, n + 1);
                synced_len++;
            }
        }
    }
    return fd;
}

/*
 * Large-file glibc renames open() to open64() when _FILE_OFFSET_BITS is 64, and the
 * platform layer's call would then bind to a symbol this test never defined - the
 * observation would silently record nothing and the failure would read as a library defect.
 * The build does not ask for that today; this is insurance against the day it does, and
 * against a 32-bit lane where it is the default.
 */
int open64(const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    return open(path, flags, (int)mode);
}

static char dir_buf[256];
static char path_buf[768];

static proven_u8str_view_t view_of(const char *cstr) {
    return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)cstr, .size = strlen(cstr) };
}

static proven_mem_view_t bytes_of(const char *text) {
    return (proven_mem_view_t){ .ptr = (const proven_byte_t *)text, .size = strlen(text) };
}

static bool file_holds(const char *path, const char *text) {
    char buf[256] = {0};
    int fd = (int)syscall(SYS_openat, AT_FDCWD, path, O_RDONLY, 0);
    if (fd < 0) return false;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n < 0) return false;
    buf[n] = '\0';
    return strcmp(buf, text) == 0;
}

int main(void) {
    PROVEN_TEST_SUITE("a backslash in a POSIX filename is not a separator (RFC-0006 H-003)",
        "A durable write has to sync the directory the file is actually in. Which directory that is depends on what separates path components, and on POSIX only '/' does.",
        "A failure naming the wrong directory means the separator rule is still unconditional. A durable write that fails on a legal filename means the same thing from the other side.");

    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    snprintf(dir_buf, sizeof dir_buf, "%s/proven-h003-%ld", tmp, (long)getpid());
    if (mkdir(dir_buf, 0700) != 0) {
        PROVEN_TEST_INFO("SKIP: could not create a private work directory under the temporary directory.");
        PROVEN_TEST_PASS("skipped: no writable temporary directory.");
        return 0;
    }

    proven_allocator_t a = proven_heap_allocator();

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a durable write to a name containing a backslash succeeds",
        "The file is created, renamed into place, and the call reports what actually happened.",
        "This used to return an I/O error - after the new contents were already visible. A caller told the write failed, looking at a file that had been replaced.");
    // ---------------------------------------------------------------
    {
        snprintf(path_buf, sizeof path_buf, "%s/a\\b", dir_buf);
        (void)remove(path_buf);
        record_reset();
        recording = true;
        proven_err_t e = proven_fs_write_file_durable(a, view_of(path_buf), bytes_of("NEW"));
        recording = false;

        PROVEN_TEST_ASSERT(e == PROVEN_OK, "the durable write must succeed",
            "The parent of \"<dir>/a\\\\b\" is <dir>. Syncing \"<dir>/a\", which does not exist, is what produced the error.");
        PROVEN_TEST_ASSERT(file_holds(path_buf, "NEW"), "and the file holds the new contents", "");
        PROVEN_TEST_ASSERT(synced_len >= 1, "a directory must have been synced", "A durable write that syncs no directory is not durable.");
        if (synced_len >= 1) {
            PROVEN_TEST_ASSERT(strcmp(synced[synced_len - 1], dir_buf) == 0,
                "the directory synced is the file's real parent", "");
        }
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a decoy directory matching the wrong prefix is not synced instead",
        "When \"<dir>/a\" happens to exist, the old rule synced it and reported success. Checking only the return value cannot tell that apart from a correct write.",
        "This is the case a PROVEN_OK assertion misses entirely, which is why the directory that was opened is recorded and named here.");
    // ---------------------------------------------------------------
    {
        char decoy[768];
        snprintf(decoy, sizeof decoy, "%s/a", dir_buf);
        PROVEN_TEST_ASSERT(mkdir(decoy, 0700) == 0 || errno == EEXIST, "the decoy directory is created", "");

        snprintf(path_buf, sizeof path_buf, "%s/a\\b", dir_buf);
        record_reset();
        recording = true;
        proven_err_t e = proven_fs_write_file_durable(a, view_of(path_buf), bytes_of("AGAIN"));
        recording = false;

        PROVEN_TEST_ASSERT(e == PROVEN_OK, "the durable write still succeeds", "");
        PROVEN_TEST_ASSERT(file_holds(path_buf, "AGAIN"), "and the file holds the new contents", "");
        bool decoy_synced = false;
        for (int i = 0; i < synced_len; ++i) {
            if (strcmp(synced[i], decoy) == 0) decoy_synced = true;
        }
        PROVEN_TEST_ASSERT(!decoy_synced, "the decoy directory is never synced",
            "It is not the file's parent. Syncing it and returning PROVEN_OK is a durability claim about the wrong directory.");
        PROVEN_TEST_ASSERT(synced_len >= 1 && strcmp(synced[synced_len - 1], dir_buf) == 0,
            "the real parent is what gets synced", "");
        (void)rmdir(decoy);
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("ordinary paths still resolve to the parent they always did",
        "The separator rule is used by every durable write, not only by the odd ones.",
        "A fix that only handles the backslash case and breaks a plain slash path has traded one defect for a worse one.");
    // ---------------------------------------------------------------
    {
        snprintf(path_buf, sizeof path_buf, "%s/plain", dir_buf);
        record_reset();
        recording = true;
        proven_err_t e = proven_fs_write_file_durable(a, view_of(path_buf), bytes_of("PLAIN"));
        recording = false;
        PROVEN_TEST_ASSERT(e == PROVEN_OK && file_holds(path_buf, "PLAIN"), "a plain slash path is written durably", "");
        PROVEN_TEST_ASSERT(synced_len >= 1 && strcmp(synced[synced_len - 1], dir_buf) == 0,
            "and its parent is the directory before the last slash", "");
        (void)remove(path_buf);

        /* A path with no separator at all: the parent is the working directory. */
        record_reset();
        recording = true;
        e = proven_fs_write_file_durable(a, PROVEN_LIT("proven-h003-cwd"), bytes_of("CWD"));
        recording = false;
        PROVEN_TEST_ASSERT(e == PROVEN_OK, "a bare filename is written durably", "");
        PROVEN_TEST_ASSERT(synced_len >= 1 && strcmp(synced[synced_len - 1], ".") == 0,
            "and its parent is \".\"", "");
        (void)remove("proven-h003-cwd");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a long basename full of backslashes is measured as one name",
        "The staging file is \"<path>.pvtmpNN\", and the basename is trimmed so that name still fits in NAME_MAX. The trim counts from the last separator.",
        "Under the old rule the basename was measured from the last BACKSLASH, so a 250-character name was measured as a short one, no trim happened, and the staging name the filesystem was asked for was too long.");
    // ---------------------------------------------------------------
    {
        char name[300];
        size_t n = 0;
        for (; n < 250; ++n) name[n] = (n % 10 == 9) ? '\\' : 'x';
        name[n] = '\0';
        snprintf(path_buf, sizeof path_buf, "%s/%s", dir_buf, name);
        proven_err_t e = proven_fs_write_file_atomic(a, view_of(path_buf), bytes_of("LONG"));
        PROVEN_TEST_ASSERT(e == PROVEN_OK, "a 250-character basename containing backslashes is written",
            "The staging name has to be trimmed to fit NAME_MAX, and it can only be trimmed if the basename was measured correctly.");
        PROVEN_TEST_ASSERT(file_holds(path_buf, "LONG"), "and holds what was written", "");
        (void)remove(path_buf);
    }

    {
        snprintf(path_buf, sizeof path_buf, "%s/a\\b", dir_buf);
        (void)remove(path_buf);
        (void)rmdir(dir_buf);
    }

    PROVEN_TEST_PASS("a POSIX backslash is part of the name, and the directory synced is the real parent.");
    return 0;
}

#endif
