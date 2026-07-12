#define _GNU_SOURCE
#include "proven.h"
#include "proven_test.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(_WIN32) || defined(_WIN64)
int main(void) {
    PROVEN_TEST_SUITE("the walk's error branches, and its TOCTOU safety",
        "readdir-failure reporting and symlink-swap descent.",
        "Needs POSIX symlink interposition; skipped on Windows.");
    PROVEN_TEST_INFO("POSIX-only; skipped on this platform");
    PROVEN_TEST_PASS("skipped");
    return 0;
}
#else

#include <dirent.h>
#include <errno.h>
#include <dlfcn.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>

/*
 * Two defects the standing audit of proven_fs_walk found (docs/TESTING.md §5.2), pinned
 * against the same fault injection that found them.
 *
 *   1. When a directory's own readdir() failed mid-iteration, the error was reported with
 *      `name` set to the WHOLE path (not the last component) and `depth` one too high - the
 *      depth of that directory's children rather than of the directory itself. The two other
 *      error branches (a directory that would not open, a directory past the depth limit)
 *      described the directory correctly; this one did not, so the same directory came back
 *      as depth 1 when it was an entry and depth 2 in its error.
 *
 *   2. The walk descended by re-opening the reconstructed path STRING with opendir(), which
 *      follows symlinks. An entry that was a real directory when it was listed and a symlink
 *      when it was entered was followed straight out of the tree - so a walk of one directory
 *      could enumerate the whole filesystem. The walk's own contract said, in as many words,
 *      "it cannot escape". It could. The descent is fd-relative and O_NOFOLLOW now.
 */

/*
 * One interposer on readdir(), doing two jobs depending on how the test arms it:
 *   - fail on the Nth call (the mid-iteration EIO of defect 1);
 *   - the instant it is about to hand back an entry named `g_swap_name`, turn that entry
 *     (at `g_swap_path`) into a symlink to `g_swap_to`. This is the TOCTOU of defect 2, and
 *     triggering it from readdir - between the entry being listed and being descended into -
 *     exercises BOTH descent paths: openat(O_NOFOLLOW) must refuse it, and the old by-path
 *     opendir() would follow it.
 */
static int g_readdir_fail_on = -1;
static int g_readdir_calls = 0;
static const char *g_swap_name = NULL;   /* non-NULL arms the swap */
static const char *g_swap_full = NULL;   /* the path stat() sees at descent (relative) */
static const char *g_swap_path = NULL;   /* the directory to replace with a symlink */
static const char *g_swap_to   = NULL;   /* where the symlink points (outside the tree) */
static int g_swap_done = 0;

struct dirent *readdir(DIR *d) {
    static struct dirent *(*real)(DIR *) = NULL;
    if (!real) real = (struct dirent *(*)(DIR *))dlsym(RTLD_NEXT, "readdir");
    if (g_readdir_fail_on >= 0 && g_readdir_calls++ == g_readdir_fail_on) {
        errno = EIO;
        return NULL;
    }
    return real(d);
}

/*
 * The swap fires at the descent's OPEN - the window between "trap was classified as a real
 * directory" and "trap is opened" - whichever kind of open the code does:
 *
 *   - the shipped code opens with openat(parent_fd, "trap", O_NOFOLLOW): the swap turns trap
 *     into a symlink first, and O_NOFOLLOW then makes the open FAIL, so there is no escape;
 *   - the old by-path code opened with opendir("we_in/trap"), which follows the symlink and
 *     escapes.
 *
 * Interposing both is what lets this one test prove the fix AND fail against the code before
 * it. (Swapping earlier - in readdir - would be too early: trap would be a symlink by the
 * time it was classified, and the walk would never try to enter it, passing for the wrong
 * reason.)
 */
static void do_swap(void) {
    if (g_swap_name && !g_swap_done && g_swap_path) {
        g_swap_done = 1;
        (void)rmdir(g_swap_path);
        (void)symlink(g_swap_to, g_swap_path);
    }
}

static const char *tail_is(const char *path, const char *name) {
    size_t pl = strlen(path), nl = strlen(name);
    if (pl >= nl && strcmp(path + pl - nl, name) == 0) return path + pl - nl;
    return NULL;
}

int openat(int dirfd, const char *path, int flags, ...) {
    static int (*real)(int, const char *, int, ...) = NULL;
    if (!real) real = (int (*)(int, const char *, int, ...))dlsym(RTLD_NEXT, "openat");
    if (g_swap_name && !g_swap_done && strcmp(path, g_swap_name) == 0) do_swap();
    /* O_NOFOLLOW carries no mode; the walk never passes O_CREAT here. */
    return real(dirfd, path, flags);
}

DIR *opendir(const char *path) {
    static DIR *(*real)(const char *) = NULL;
    if (!real) real = (DIR *(*)(const char *))dlsym(RTLD_NEXT, "opendir");
    if (g_swap_name && !g_swap_done && tail_is(path, g_swap_full)) do_swap();
    return real(path);
}

int main(void) {
    PROVEN_TEST_SUITE("the walk's error branches, and its TOCTOU safety",
        "A readdir failure must name the directory correctly and give its own depth; a directory that becomes a symlink under the walk must not be followed out of the tree.",
        "Inspect the readdir-failure branch in proven_fs_walk_next and the fd-relative descent (proven_sys_fs_dir_open_at) in src/proven/fs.c and platform/proven_sys_fs.c.");

    proven_allocator_t heap = proven_heap_allocator();

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a readdir failure names the directory, not its whole path, at its own depth",
        "The bug reported `name` as the full path and `depth` one too high - unlike the other two error branches, which got it right.",
        "The fix makes this branch narrow `name` to the last component and use the directory's own depth, exactly as the open-failure branch does.");
    // ---------------------------------------------------------------
    {
        (void)system("chmod -R u+rwx we_rd 2>/dev/null; rm -rf we_rd");
        (void)system("mkdir -p we_rd/sub/inner; printf a > we_rd/sub/inner/x; printf a > we_rd/sub/inner/y; printf a > we_rd/after");

        /* Fail a readdir somewhere mid-walk. The exact index does not matter; what matters
         * is that WHEN a readdir fails, the entry describing it is well-formed. */
        g_readdir_calls = 0;
        g_readdir_fail_on = 5;

        proven_result_walk_t w = proven_fs_walk_open(heap, PROVEN_LIT("we_rd"), PROVEN_FS_WALK_UNLIMITED);
        PROVEN_TEST_ASSERT(proven_is_ok(w.err), "the walk must open", "");

        int saw_error = 0;
        for (;;) {
            proven_fs_walk_entry_t e = {0};
            proven_err_t err = proven_fs_walk_next(&w.value, &e);
            if (err == PROVEN_ERR_EOF) break;
            if (proven_is_ok(err)) continue;

            saw_error = 1;

            /* name must be the last component - no slash in it, and it must be the tail
             * of path. */
            bool has_slash = false;
            for (proven_size_t i = 0; i < e.name.size; ++i) {
                if (e.name.ptr[i] == (proven_u8)'/') has_slash = true;
            }
            PROVEN_TEST_ASSERT(!has_slash && e.name.size > 0,
                "a readdir-failure error must name the last path component, not the whole path",
                "It used to set name to the entire path.");
            PROVEN_TEST_ASSERT(e.path.size >= e.name.size &&
                memcmp(e.path.ptr + e.path.size - e.name.size, e.name.ptr, e.name.size) == 0,
                "and that name must be the tail of the path", "");

            /* depth must be the directory's OWN depth. we_rd/sub is depth 0; we_rd/sub/inner
             * is depth 1. Whichever one failed, its error depth must match where it was
             * reported as an entry - never one deeper. */
            PROVEN_TEST_ASSERT(e.depth <= 1,
                "the error depth must be the directory's own, not its children's",
                "The bug reported depth 2 for a directory that was an entry at depth 1.");
        }
        proven_fs_walk_close(&w.value);
        g_readdir_fail_on = -1;

        PROVEN_TEST_ASSERT(saw_error, "the injected readdir failure must have produced an error", "");

        (void)system("chmod -R u+rwx we_rd 2>/dev/null; rm -rf we_rd");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a directory that becomes a symlink under the walk is not followed out",
        "The walk descended by re-opening the path string, which follows symlinks. Swap a real directory for a symlink-to-outside at the moment of descent and the old walk enumerated the target.",
        "The descent is openat(parent_fd, name, O_NOFOLLOW) now: if the entry is a symlink when it is entered, the open fails and the walk reports it rather than following it.");
    // ---------------------------------------------------------------
    {
        (void)system("chmod -R u+rwx we_in we_out 2>/dev/null; rm -rf we_in we_out");
        (void)system("mkdir -p we_in/trap; printf x > we_in/normal.txt");
        (void)system("mkdir -p we_out; printf SECRET > we_out/LEAK");

        char cwd[1024];
        PROVEN_TEST_ASSERT(getcwd(cwd, sizeof cwd) != NULL, "setup: cwd", "");
        static char trap[1200], out[1200];
        snprintf(trap, sizeof trap, "%s/we_in/trap", cwd);
        snprintf(out, sizeof out, "%s/we_out", cwd);

        g_swap_name = "trap";        /* arm the swap */
        g_swap_full = "we_in/trap";  /* the relative path the descent will stat() */
        g_swap_path = trap;          /* absolute, for rmdir/symlink */
        g_swap_to = out;
        g_swap_done = 0;

        proven_result_walk_t w = proven_fs_walk_open(heap, PROVEN_LIT("we_in"), PROVEN_FS_WALK_UNLIMITED);
        PROVEN_TEST_ASSERT(proven_is_ok(w.err), "the walk must open", "");

        int escaped = 0;
        for (;;) {
            proven_fs_walk_entry_t e = {0};
            proven_err_t err = proven_fs_walk_next(&w.value, &e);
            if (err == PROVEN_ERR_EOF) break;
            if (!proven_is_ok(err)) continue;
            if (e.name.size == 4 && memcmp(e.name.ptr, "LEAK", 4) == 0) escaped = 1;
        }
        proven_fs_walk_close(&w.value);
        g_swap_name = NULL;

        PROVEN_TEST_ASSERT(g_swap_done,
            "the swap must actually have fired (the fixture must have listed \"trap\")", "");
        PROVEN_TEST_ASSERT(!escaped,
            "the walk must not enumerate a file outside the tree it was given",
            "The descent followed a symlink swapped in between listing and entering. openat(..., O_NOFOLLOW) cannot be fooled that way.");

        (void)system("chmod -R u+rwx we_in we_out 2>/dev/null; rm -rf we_in we_out");
    }

    PROVEN_TEST_PASS("the walk names its errors correctly and cannot be tricked out of its tree.");
    return 0;
}
#endif
