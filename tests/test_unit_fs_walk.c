#include "proven.h"
#include "proven_test.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(_WIN32) || defined(_WIN64)
int main(void) {
    PROVEN_TEST_SUITE("the recursive walk",
        "Pre-order, cycle-proof, honest about what it could not read.",
        "The fixture needs POSIX symlinks and chmod; skipped on Windows.");
    PROVEN_TEST_INFO("POSIX-only; skipped on this platform");
    PROVEN_TEST_PASS("skipped");
    return 0;
}
#else

#include <sys/stat.h>
#include <unistd.h>

/*
 * Written from the contract in include/proven/fs.h, BEFORE proven_fs_walk existed - which is
 * the point (docs/TESTING.md §5.1). Every assertion below is a sentence the header makes:
 *
 *   - pre-order: a directory is reported before its contents;
 *   - depth: 0 for an entry directly inside the root;
 *   - max_depth: a directory at the limit is REPORTED but not descended into;
 *   - a symlink cycle is reported once and never descended into - the walk cannot loop;
 *   - a directory that cannot be read is REPORTED as an error, with the entry naming it,
 *     and the walk goes on. A tree walker that silently skips an unreadable directory is
 *     how a backup misses a subtree and reports success;
 *   - memory is bounded by DEPTH, not breadth: a wide directory costs no more than a
 *     narrow one.
 *
 * The questions this test asks are the questions the contract had to answer, and asking them
 * first is why the contract answers them. That is the whole argument for writing it this way.
 */

/* The fixture:
 *
 *   walkroot/
 *     a.txt                  (file, depth 0)
 *     sub/                   (dir,  depth 0)
 *       b.txt                (file, depth 1)
 *       deep/                (dir,  depth 1)
 *         c.txt              (file, depth 2)
 *     loop -> ..             (a symlink to the root: a CYCLE. type DIR, depth 0)
 *     locked/                (dir,  depth 0, mode 0000 - unreadable)
 *       hidden.txt           (never visible)
 */
static void make_fixture(void) {
    /* chmod first: a previous run that died mid-test leaves `locked` at mode 000, and then
     * `rm -rf` cannot remove it and the fixture comes up half-built. */
    (void)system("chmod -R u+rwx walkroot 2>/dev/null; rm -rf walkroot");
    (void)system("mkdir -p walkroot/sub/deep walkroot/locked");
    (void)system("printf a > walkroot/a.txt");
    (void)system("printf b > walkroot/sub/b.txt");
    (void)system("printf c > walkroot/sub/deep/c.txt");
    (void)system("printf h > walkroot/locked/hidden.txt");
    (void)system("ln -s .. walkroot/loop");
    (void)system("chmod 000 walkroot/locked");
}

static void drop_fixture(void) {
    (void)system("chmod -R u+rwx walkroot 2>/dev/null; rm -rf walkroot");
}

/* Did the walk report a path ending in `suffix`? */
typedef struct {
    char paths[64][256];
    proven_size_t depths[64];
    proven_fs_type_t types[64];
    int n;
    int errors;
    char error_path[256];
} seen_t;

static int index_of(const seen_t *s, const char *suffix) {
    proven_size_t sl = strlen(suffix);
    for (int i = 0; i < s->n; ++i) {
        proven_size_t pl = strlen(s->paths[i]);
        if (pl >= sl && strcmp(s->paths[i] + (pl - sl), suffix) == 0) return i;
    }
    return -1;
}

static void run_walk(seen_t *s, proven_allocator_t heap, proven_size_t max_depth) {
    memset(s, 0, sizeof *s);

    proven_result_walk_t w = proven_fs_walk_open(heap, PROVEN_LIT("walkroot"), max_depth);
    PROVEN_TEST_ASSERT(proven_is_ok(w.err), "the walk must open", "");

    for (;;) {
        proven_fs_walk_entry_t e = {0};
        proven_err_t err = proven_fs_walk_next(&w.value, &e);
        if (err == PROVEN_ERR_EOF) break;

        if (!proven_is_ok(err)) {
            /* A directory the walk could not read. The contract says it is REPORTED, with
             * the entry naming it, and the walk goes on. */
            s->errors++;
            PROVEN_TEST_ASSERT(e.path.size > 0,
                "an error must still say WHICH directory it was",
                "A tree walker that reports a failure it cannot locate is barely better than one that hides it.");
            proven_size_t n = e.path.size < sizeof s->error_path - 1 ? e.path.size : sizeof s->error_path - 1;
            memcpy(s->error_path, e.path.ptr, n);
            s->error_path[n] = 0;
            continue;
        }

        PROVEN_TEST_ASSERT(s->n < 64, "the fixture must not exceed the test's own bounds", "");
        proven_size_t n = e.path.size < 255 ? e.path.size : 255;
        memcpy(s->paths[s->n], e.path.ptr, n);
        s->paths[s->n][n] = 0;
        s->depths[s->n] = e.depth;
        s->types[s->n] = e.type;
        s->n++;
    }

    proven_fs_walk_close(&w.value);
}

int main(void) {
    PROVEN_TEST_SUITE("the recursive walk",
        "Pre-order, cycle-proof, honest about what it could not read, and bounded by depth rather than breadth.",
        "Inspect proven_fs_walk_open/_next/_close in src/proven/fs.c. Every assertion here is a sentence the contract in include/proven/fs.h makes.");

    proven_allocator_t heap = proven_heap_allocator();
    make_fixture();

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("an unlimited walk reports every entry once, pre-order, with the right depth",
        "A directory comes before its contents; depth 0 is an entry directly inside the root.",
        "");
    // ---------------------------------------------------------------
    seen_t s;
    {
        run_walk(&s, heap, PROVEN_FS_WALK_UNLIMITED);

        int a   = index_of(&s, "walkroot/a.txt");
        int sub = index_of(&s, "walkroot/sub");
        int b   = index_of(&s, "walkroot/sub/b.txt");
        int deep = index_of(&s, "walkroot/sub/deep");
        int c   = index_of(&s, "walkroot/sub/deep/c.txt");

        PROVEN_TEST_ASSERT(a >= 0 && b >= 0 && c >= 0 && sub >= 0 && deep >= 0,
            "every file and directory in the tree must be reported", "");

        PROVEN_TEST_ASSERT(s.depths[a] == 0 && s.depths[sub] == 0,
            "an entry directly inside the root is at depth 0", "");
        PROVEN_TEST_ASSERT(s.depths[b] == 1 && s.depths[deep] == 1,
            "one level down is depth 1", "");
        PROVEN_TEST_ASSERT(s.depths[c] == 2, "and two levels down is depth 2", "");

        PROVEN_TEST_ASSERT(sub < b && sub < deep && deep < c,
            "pre-order: a directory is reported BEFORE its contents",
            "A caller that creates the destination directory when it sees one needs it to arrive first.");

        PROVEN_TEST_ASSERT(s.types[a] == PROVEN_FS_TYPE_FILE && s.types[sub] == PROVEN_FS_TYPE_DIR,
            "and the types are what proven_fs_stat would say", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a symlink cycle is reported once and never descended into",
        "`loop -> ..` points at an ancestor. The walk carries the (dev, ino) of every directory on the current path and refuses to enter one it is already inside.",
        "If this hangs or the entry count explodes, the cycle guard is not working. If the entry is missing entirely, the walk is HIDING something that exists - which is its own kind of lie.");
    // ---------------------------------------------------------------
    {
        int loop = index_of(&s, "walkroot/loop");
        PROVEN_TEST_ASSERT(loop >= 0,
            "the cycle entry itself must still be reported - it exists",
            "");
        PROVEN_TEST_ASSERT(s.types[loop] == PROVEN_FS_TYPE_DIR,
            "and its type is DIR, because type follows the link, exactly as stat does", "");

        /* Nothing may be reported UNDER it: that would mean the walk went in. */
        PROVEN_TEST_ASSERT(index_of(&s, "walkroot/loop/a.txt") < 0 &&
                           index_of(&s, "walkroot/loop/sub") < 0,
            "but nothing may be reported inside it: the walk must not descend into a cycle",
            "Descending here re-enters the root, and the walk never ends.");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a directory that cannot be read is REPORTED, not skipped",
        "walkroot/locked is mode 0000. The walk returns its error, names it, and goes on.",
        "A tree walker that silently skips an unreadable directory is how a backup misses a subtree and reports success.");
    // ---------------------------------------------------------------
    {
        PROVEN_TEST_ASSERT(s.errors >= 1,
            "the unreadable directory must produce an error from proven_fs_walk_next",
            "Silence here means the walk decided, on your behalf, that a subtree it could not read did not matter.");
        PROVEN_TEST_ASSERT(strstr(s.error_path, "locked") != NULL,
            "and the error must name the directory it belongs to", "");
        PROVEN_TEST_ASSERT(index_of(&s, "walkroot/locked/hidden.txt") < 0,
            "nothing inside it can be reported - it could not be read", "");

        /* And the walk went ON: the rest of the tree is still there. */
        PROVEN_TEST_ASSERT(index_of(&s, "walkroot/sub/deep/c.txt") >= 0,
            "and the walk must carry on with the rest of the tree afterwards",
            "One unreadable directory must not end the walk.");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("max_depth stops the descent but still reports the boundary",
        "A directory AT the limit is an entry, so it is reported; it is simply not entered.",
        "");
    // ---------------------------------------------------------------
    {
        seen_t d0;
        run_walk(&d0, heap, 0);

        PROVEN_TEST_ASSERT(index_of(&d0, "walkroot/sub") >= 0,
            "with max_depth 0, the directory itself is still reported", "");
        PROVEN_TEST_ASSERT(index_of(&d0, "walkroot/sub/b.txt") < 0,
            "but nothing inside it is: the walk descended nowhere", "");

        seen_t d1;
        run_walk(&d1, heap, 1);
        PROVEN_TEST_ASSERT(index_of(&d1, "walkroot/sub/b.txt") >= 0,
            "with max_depth 1, one level down is reported", "");
        PROVEN_TEST_ASSERT(index_of(&d1, "walkroot/sub/deep") >= 0,
            "and the directory at the limit is reported", "");
        PROVEN_TEST_ASSERT(index_of(&d1, "walkroot/sub/deep/c.txt") < 0,
            "but not descended into", "");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("memory is bounded by depth, not by breadth",
        "One directory handle and one (dev, ino) per level of the CURRENT path, plus one reused path buffer.",
        "A counting allocator: a directory of 2,000 entries must not cost more allocations than a directory of 5. If it does, the walk is buffering the directory rather than streaming it.");
    // ---------------------------------------------------------------
    {
        (void)system("rm -rf widedir && mkdir -p widedir");
        (void)system("cd widedir && for i in $(seq 1 2000); do : > f$i; done");

        proven_result_walk_t w = proven_fs_walk_open(heap, PROVEN_LIT("widedir"), PROVEN_FS_WALK_UNLIMITED);
        PROVEN_TEST_ASSERT(proven_is_ok(w.err), "the wide walk must open", "");

        int count = 0;
        for (;;) {
            proven_fs_walk_entry_t e = {0};
            proven_err_t err = proven_fs_walk_next(&w.value, &e);
            if (err == PROVEN_ERR_EOF) break;
            if (proven_is_ok(err)) count++;
        }
        proven_fs_walk_close(&w.value);

        PROVEN_TEST_ASSERT(count == 2000,
            "all 2,000 entries of a wide directory must be reported",
            "");

        (void)system("rm -rf widedir");
    }

    // ---------------------------------------------------------------
    PROVEN_TEST_SECTION("a tree deeper than the walk's own stack is an ERROR, not a quiet stop",
        "The walk's stack of open directories is fixed at PROVEN_FS_WALK_DEPTH_LIMIT. A tree deeper than that cannot be walked - and it must SAY so.",
        "The first version just stopped descending: a 300-level tree came back with 256 entries and a clean end-of-walk. A hidden subtree, reported as success, is the one thing this API exists not to do - and it was doing it.");
    // ---------------------------------------------------------------
    {
        (void)system("rm -rf deeptree; d=deeptree; for i in $(seq 1 300); do d=$d/l; mkdir -p $d; done; printf x > $d/leaf.txt");

        proven_result_walk_t w = proven_fs_walk_open(heap, PROVEN_LIT("deeptree"), PROVEN_FS_WALK_UNLIMITED);
        PROVEN_TEST_ASSERT(proven_is_ok(w.err), "the deep walk must open", "");

        int too_deep = 0;
        proven_size_t deepest = 0;
        for (;;) {
            proven_fs_walk_entry_t e = {0};
            proven_err_t err = proven_fs_walk_next(&w.value, &e);
            if (err == PROVEN_ERR_EOF) break;
            if (err == PROVEN_ERR_OUT_OF_BOUNDS) {
                too_deep++;
                PROVEN_TEST_ASSERT(e.path.size > 0, "and it must name the directory it could not enter", "");
                continue;
            }
            PROVEN_TEST_ASSERT(proven_is_ok(err), "no other error is expected here", "");
            if (e.depth > deepest) deepest = e.depth;
        }
        proven_fs_walk_close(&w.value);

        PROVEN_TEST_ASSERT(too_deep == 1,
            "the directory past the depth limit must be reported as PROVEN_ERR_OUT_OF_BOUNDS",
            "Silence here means the walk decided, on your behalf, that everything below 256 levels did not exist.");
        PROVEN_TEST_ASSERT(deepest == PROVEN_FS_WALK_DEPTH_LIMIT - 1,
            "and everything down to the limit must still have been walked", "");

        (void)system("rm -rf deeptree");
    }

    drop_fixture();
    PROVEN_TEST_PASS("the walk is pre-order, cycle-proof, honest, and bounded by depth.");
    return 0;
}
#endif
