#include "proven_test.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#if !defined(_WIN32) && !defined(_WIN64)
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

/*
 * `./nob clean` deletes a directory tree, so it is held to what it may delete (B-036): the
 * selected -build-root, never '.', '/' or a path with '..'; only when nob created it (it carries
 * .proven-build-root) or it is the default `build`; and never through a symlink into a directory
 * outside the root.
 */

#define BASE "build/test_portability_nob_clean"

#if defined(_WIN32) || defined(_WIN64)
int main(void) {
    PROVEN_TEST_SUITE("test_portability_nob_clean", "POSIX host only: the fixture drives ./nob through a POSIX shell and symlink().", "");
    PROVEN_TEST_PASS("Skipped on Windows: the fixture needs a POSIX shell and symlink() (B-035).");
    return 0;
}
#else

static int run_cmd(const char *cmd) {
    int rc = system(cmd);
    PROVEN_TEST_ASSERT(rc != -1, "launch build driver", "Check shell availability and command construction.");
    PROVEN_TEST_ASSERT(WIFEXITED(rc), "build driver exits normally", "Inspect the build-driver output above.");
    return WEXITSTATUS(rc);
}

static void touch(const char *path) {
    FILE *f = fopen(path, "wb");
    PROVEN_TEST_ASSERT(f != NULL, "create fixture file", "Check write permissions under build/.");
    fputs("x\n", f);
    PROVEN_TEST_ASSERT(fclose(f) == 0, "close fixture file", "Inspect the test host's file handling.");
}

static bool exists(const char *path) {
    struct stat st;
    return lstat(path, &st) == 0;
}

int main(void) {
    PROVEN_TEST_SUITE(
        "test_portability_nob_clean",
        "Verify ./nob clean removes the selected build root and nothing else: unsafe paths and unmarked directories are refused, and a symlink inside the root is removed without its target being entered.",
        "If this fails, inspect clean_build_root, build_root_path_is_safe and remove_tree_no_follow in nob.c before touching this test."
    );

    (void)system("rm -rf " BASE);
    PROVEN_TEST_ASSERT(system("mkdir -p " BASE "/outside " BASE "/unmarked " BASE "/root/sub") == 0,
                       "create fixture directories", "Check write permissions under build/.");
    touch(BASE "/outside/keep.txt");
    touch(BASE "/unmarked/keep.txt");
    touch(BASE "/root/sub/obj.o");
    touch(BASE "/root/.proven-build-root");
    PROVEN_TEST_ASSERT(symlink("../outside", BASE "/root/link") == 0, "create symlink fixture",
                       "Check that the test host supports symlinks under build/.");

    PROVEN_TEST_SECTION("refused paths", "Paths that could name the checkout or leave it are refused before anything is read.", "");
    PROVEN_TEST_ASSERT(run_cmd("./nob clean -build-root . >/dev/null 2>&1") != 0, "'.' is refused",
                       "clean must never remove the checkout itself.");
    PROVEN_TEST_ASSERT(run_cmd("./nob clean -build-root " BASE "/../test_portability_nob_clean >/dev/null 2>&1") != 0,
                       "a '..' component is refused", "A path with '..' can leave the checkout; refuse it by shape.");
    PROVEN_TEST_ASSERT(run_cmd("./nob clean -build-root / >/dev/null 2>&1") != 0,
                       "'/' is refused", "clean must never remove the filesystem root.");
    PROVEN_TEST_ASSERT(run_cmd("./nob clean -build-root " BASE "/unmarked >/dev/null 2>&1") != 0,
                       "an unmarked directory is refused", "Only a root nob created (it carries .proven-build-root) may be removed.");
    PROVEN_TEST_ASSERT(exists(BASE "/unmarked/keep.txt"), "the unmarked directory is intact",
                       "A refusal must not delete anything first.");

    PROVEN_TEST_SECTION("marked root", "A marked root is removed; the symlink inside it goes, its target stays.", "");
    PROVEN_TEST_ASSERT(run_cmd("./nob clean -build-root " BASE "/root >/dev/null 2>&1") == 0, "a marked root is removed",
                       "Inspect remove_tree_no_follow for a failed delete.");
    PROVEN_TEST_ASSERT(!exists(BASE "/root"), "the root is gone", "clean reported success but left the root.");
    PROVEN_TEST_ASSERT(exists(BASE "/outside/keep.txt"), "the symlink target is untouched",
                       "clean followed a symlink out of the build root; remove links, never enter them.");
    PROVEN_TEST_ASSERT(run_cmd("./nob clean -build-root " BASE "/root >/dev/null 2>&1") == 0,
                       "a missing root is nothing to do", "Cleaning twice should succeed.");

    (void)system("rm -rf " BASE);
    PROVEN_TEST_PASS("Build driver clean removes only the selected, marked build root.");
    return 0;
}
#endif
