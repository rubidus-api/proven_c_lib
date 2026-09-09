/*
 * RFC-0006 H-002: a staging file must never exist, even briefly, in a mode that lets
 * another local user open it.
 *
 * proven_fs_write_file_atomic writes the new contents to a sibling ".pvtmpNN" file and
 * renames it over the target. That temp used to be CREATED with 0666 & ~umask - so with
 * the usual umask 0022 it was 0644 for the instant between creation and the chmod that
 * narrowed it. Narrowing it afterwards does not help: a descriptor another user opened
 * during that instant stays open and stays readable, and the private payload is then
 * written through the file it still points at. chmod revokes future opens, never
 * existing ones.
 *
 * The observation seam is a definition of open() in this test, which the linker binds
 * the platform layer's call to. It forwards to the real openat syscall and records the
 * mode each created file actually got, so the check is deterministic - no watcher thread
 * and no race to win.
 *
 * Scope of what this proves: initial permissions. It does not prove anything about a
 * hostile writer in the directory, about readers who already had the old file open, or
 * about Windows ACLs.
 */

#if !defined(_WIN32) && !defined(_WIN64)
#define _GNU_SOURCE   /* syscall(); this must precede every header */
#endif

#include "proven.h"
#include "proven_test.h"

#if defined(_WIN32) || defined(_WIN64)

int main(void) {
    PROVEN_TEST_SUITE("private staging permissions (RFC-0006 H-002)",
        "POSIX creation modes are the subject; Windows uses ACLs and is a separate target result.",
        "Nothing to do here. The Windows permission story needs a native test, not this one.");
    PROVEN_TEST_INFO("SKIP: POSIX-only test. Windows confidentiality depends on ACLs, which this does not model.");
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

/* Every file this process creates, with the mode it had at the moment of creation. */
#define OBS_MAX 32
static struct { char path[512]; unsigned mode; } obs[OBS_MAX];
static int obs_len;

static void obs_reset(void) { obs_len = 0; }

/* The mode the newest created file whose name contains `needle` was born with,
 * or 07777 when no such file was created. */
static unsigned obs_mode_of(const char *needle) {
    for (int i = obs_len - 1; i >= 0; --i) {
        if (strstr(obs[i].path, needle)) return obs[i].mode;
    }
    return 07777u;
}

int open(const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    int fd = (int)syscall(SYS_openat, AT_FDCWD, path, flags, (int)mode);
    /* path is declared nonnull by the platform header; do not compare it to NULL here. */
    if (fd >= 0 && (flags & O_CREAT) && obs_len < OBS_MAX) {
        struct stat st;
        if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) {
            size_t n = strlen(path);
            if (n < sizeof obs[0].path) {
                memcpy(obs[obs_len].path, path, n + 1);
                obs[obs_len].mode = (unsigned)(st.st_mode & 07777u);
                obs_len++;
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
static char path_buf[512];
static char path_buf2[512];

static const char *jp(char *dst, size_t cap, const char *name) {
    snprintf(dst, cap, "%s/%s", dir_buf, name);
    return dst;
}

static proven_u8str_view_t view_of(const char *cstr) {
    return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)cstr, .size = strlen(cstr) };
}

static unsigned mode_of(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return 07777u;
    return (unsigned)(st.st_mode & 07777u);
}

static void write_bytes(proven_allocator_t a, const char *path, const char *text) {
    proven_mem_view_t v = { .ptr = (const proven_byte_t *)text, .size = strlen(text) };
    proven_err_t e = proven_fs_write_file(a, view_of(path), v);
    PROVEN_TEST_ASSERT(proven_is_ok(e), "the fixture itself must be writable",
        "The temporary directory could not be written. Check TMPDIR and permissions before reading anything else here.");
}

/* Does this filesystem honour the mode passed to creat? A share with inherited ACLs
 * (NFSv4/ZFS) can force a wider mode on every new file no matter what is requested,
 * and on such a filesystem this test cannot distinguish a fixed library from a broken
 * one. Say so and stop rather than reporting a failure the library did not cause. */
static bool modes_are_honoured(void) {
    jp(path_buf, sizeof path_buf, "probe-mode");
    (void)remove(path_buf);
    int fd = (int)syscall(SYS_openat, AT_FDCWD, path_buf, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return false;
    struct stat st;
    bool ok = fstat(fd, &st) == 0 && (st.st_mode & 077u) == 0u;
    close(fd);
    (void)remove(path_buf);
    return ok;
}

int main(void) {
    PROVEN_TEST_SUITE("private staging permissions (RFC-0006 H-002)",
        "A file that is about to hold private bytes must be CREATED private. Narrowing it later leaves a window, and a descriptor opened in that window survives the narrowing.",
        "The mode reported is the one the file was born with. If it carries group or other bits, the creating call asked for the default mode instead of the restrictive one.");

    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    snprintf(dir_buf, sizeof dir_buf, "%s/proven-h002-%ld", tmp, (long)getpid());
    if (mkdir(dir_buf, 0700) != 0) {
        PROVEN_TEST_INFO("SKIP: could not create a private work directory under the temporary directory.");
        PROVEN_TEST_PASS("skipped: no writable temporary directory.");
        return 0;
    }

    if (!modes_are_honoured()) {
        PROVEN_TEST_INFO("SKIP: this filesystem does not honour creation modes (inherited ACLs), so initial permissions cannot be observed here.");
        (void)rmdir(dir_buf);
        PROVEN_TEST_PASS("skipped: creation modes are not honoured on this filesystem.");
        return 0;
    }

    proven_allocator_t a = proven_heap_allocator();
    mode_t saved_umask = umask(0022);

    PROVEN_TEST_SECTION("atomic replacement of a private file",
        "Rewriting a 0600 file must stage the new contents in a file that is 0600 from its first instant.",
        "A staging mode of 0644 is the defect: the payload is written into a file other users could already have opened.");
    {
        const char *target = jp(path_buf, sizeof path_buf, "secret");
        write_bytes(a, target, "OLD");
        PROVEN_TEST_ASSERT(proven_is_ok(proven_fs_chmod(a, view_of(target), (proven_fs_perms_t)0600u)),
            "the fixture must be a 0600 file", "chmod failed on the fixture; the rest of this test would be meaningless.");

        obs_reset();
        proven_mem_view_t v = { .ptr = (const proven_byte_t *)"NEW", .size = 3 };
        PROVEN_TEST_ASSERT(proven_is_ok(proven_fs_write_file_atomic(a, view_of(target), v)),
            "the atomic write must succeed", "Inspect internal_write_file_atomic in src/proven/fs.c.");

        unsigned staged = obs_mode_of(".pvtmp");
        PROVEN_TEST_ASSERT(staged != 07777u, "the staging file must have been observed",
            "No .pvtmp file was created through open(). The staging strategy changed; update this test with it.");
        PROVEN_TEST_ASSERT((staged & 077u) == 0u, "the staging file must be created without group or other access",
            "This is the defect RFC-0006 H-002 describes. The creating call must pass a restrictive mode, not chmod afterwards.");
        PROVEN_TEST_ASSERT(mode_of(target) == 0600u, "the published file keeps the target's own mode",
            "Restrictive staging must not change what the finished file looks like.");
    }

    PROVEN_TEST_SECTION("durable replacement of a private file",
        "The durable path stages the same way and must be private from creation too.",
        "If only the atomic path was fixed, the durable one still writes secrets into a world-readable temp.");
    {
        const char *target = jp(path_buf, sizeof path_buf, "secret-durable");
        write_bytes(a, target, "OLD");
        PROVEN_TEST_ASSERT(proven_is_ok(proven_fs_chmod(a, view_of(target), (proven_fs_perms_t)0600u)),
            "the fixture must be a 0600 file", "chmod failed on the fixture.");

        obs_reset();
        proven_mem_view_t v = { .ptr = (const proven_byte_t *)"NEW", .size = 3 };
        PROVEN_TEST_ASSERT(proven_is_ok(proven_fs_write_file_durable(a, view_of(target), v)),
            "the durable write must succeed", "Inspect internal_write_file_atomic in src/proven/fs.c.");

        unsigned staged = obs_mode_of(".pvtmp");
        PROVEN_TEST_ASSERT(staged != 07777u, "the staging file must have been observed", "No .pvtmp file was created through open().");
        PROVEN_TEST_ASSERT((staged & 077u) == 0u, "durable staging is created without group or other access",
            "Both whole-file replacement paths share one implementation; fix it there, not twice.");
        PROVEN_TEST_ASSERT(mode_of(target) == 0600u, "the published file keeps the target's own mode", "Restrictive staging must not change the finished mode.");
    }

    PROVEN_TEST_SECTION("copying a private file to a new name",
        "proven_fs_copy narrows the destination to 0600 for the duration of the copy - so it must CREATE it that way.",
        "Creating the destination with the default mode and chmodding it afterwards is the same window in a second place.");
    {
        const char *src = jp(path_buf, sizeof path_buf, "copy-src");
        const char *dest = jp(path_buf2, sizeof path_buf2, "copy-dest");
        write_bytes(a, src, "PRIVATE");
        PROVEN_TEST_ASSERT(proven_is_ok(proven_fs_chmod(a, view_of(src), (proven_fs_perms_t)0600u)),
            "the source must be a 0600 file", "chmod failed on the fixture.");
        (void)remove(dest);

        obs_reset();
        PROVEN_TEST_ASSERT(proven_is_ok(proven_fs_copy(a, view_of(src), view_of(dest))),
            "the copy must succeed", "Inspect proven_fs_copy in src/proven/fs.c.");

        unsigned created = obs_mode_of("copy-dest");
        PROVEN_TEST_ASSERT(created != 07777u, "the destination creation must have been observed",
            "No destination file was created through open(). The copy strategy changed; update this test with it.");
        PROVEN_TEST_ASSERT((created & 077u) == 0u, "a new copy destination is created without group or other access",
            "The copy carries the source's mode on at the end; until then the bytes must not be readable by anyone else.");
        PROVEN_TEST_ASSERT(mode_of(dest) == 0600u, "the finished copy carries the source's mode", "The final chmod is what publishes the intended mode.");
    }

    PROVEN_TEST_SECTION("a brand-new atomic target keeps the documented default mode",
        "Restrictive staging is for carrying an existing target's mode across. Creating a file that did not exist keeps the default the library always had.",
        "If this now reports 0600, the default-permissions policy changed - that is an owner decision, not a side effect.");
    {
        const char *target = jp(path_buf, sizeof path_buf, "brand-new");
        (void)remove(target);
        proven_mem_view_t v = { .ptr = (const proven_byte_t *)"NEW", .size = 3 };
        PROVEN_TEST_ASSERT(proven_is_ok(proven_fs_write_file_atomic(a, view_of(target), v)),
            "the atomic write must succeed", "Inspect internal_write_file_atomic in src/proven/fs.c.");
        PROVEN_TEST_ASSERT(mode_of(target) == 0644u, "a new atomic target is 0666 & ~umask, as before",
            "umask here is 0022, so the documented default is 0644. A different value means the default policy moved.");
    }

    PROVEN_TEST_SECTION("umask is not what makes staging private",
        "Under umask 0000 the default creation mode is 0666, so a staging file that is still private proves the mode came from the call.",
        "If staging is 0666 here, the fix was really the process umask and it protects nothing.");
    {
        (void)umask(0000);
        const char *target = jp(path_buf, sizeof path_buf, "secret-umask0");
        write_bytes(a, target, "OLD");
        PROVEN_TEST_ASSERT(proven_is_ok(proven_fs_chmod(a, view_of(target), (proven_fs_perms_t)0600u)),
            "the fixture must be a 0600 file", "chmod failed on the fixture.");

        obs_reset();
        proven_mem_view_t v = { .ptr = (const proven_byte_t *)"NEW", .size = 3 };
        PROVEN_TEST_ASSERT(proven_is_ok(proven_fs_write_file_atomic(a, view_of(target), v)),
            "the atomic write must succeed", "Inspect internal_write_file_atomic in src/proven/fs.c.");
        unsigned staged = obs_mode_of(".pvtmp");
        PROVEN_TEST_ASSERT((staged & 077u) == 0u, "staging is private even with an open umask",
            "The restrictive mode must come from the creating call. The library must not touch the process umask, which is shared mutable state.");
        (void)umask(0022);
    }

    /* Leave nothing behind. */
    {
        const char *names[] = { "secret", "secret-durable", "copy-src", "copy-dest", "brand-new", "secret-umask0" };
        for (size_t i = 0; i < sizeof names / sizeof names[0]; ++i) {
            (void)remove(jp(path_buf, sizeof path_buf, names[i]));
        }
        (void)rmdir(dir_buf);
    }
    (void)umask(saved_umask);

    PROVEN_TEST_PASS("staging files are created private, and the published modes are unchanged.");
    return 0;
}

#endif
