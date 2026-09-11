/*
 * docs/rfc-0006-runtime-check.c - a verifier meant to be RUN, by a person, on a machine
 * this project cannot reach.
 *
 * NOT part of the library, and not built by ./nob - it has its own main, and every cross
 * registry links its sources into one executable. scripts/build-rfc-0006-check.sh builds
 * it; the header comment there is the whole build.
 *
 * RFC-0006 H-005 and H-006 are Windows defects. They were fixed by inspection and they
 * cross-compile, and neither of those is a runtime result: nothing in this project has
 * ever executed a Windows binary. docs/BACKLOG.md B-033 stays open for exactly that
 * reason, and its closure conditions are things only a real run can answer.
 *
 * So this program asks them. It is built for Windows by `./nob cross`, handed to someone
 * with a Windows machine, run there, and its output comes back as the evidence. It also
 * builds and runs on POSIX - not because the defects live there, but because a verifier
 * that has never been executed at all is not a verifier. Running it here proves the
 * harness works; running it on Windows is the point.
 *
 * It writes its report to stdout AND to proven-windows-check-report.txt beside itself, so
 * the person running it can send back a file rather than a screenshot of a console window
 * that closed.
 *
 * What it cannot answer, and does not pretend to:
 *   - the 4 GiB entropy boundary. Asking for that much really would need that much memory,
 *     and a failure would not distinguish the defect from the machine. The arithmetic that
 *     the defect lived in is checked directly instead, through the same header the
 *     platform layer uses.
 *   - ACLs. RFC-0006 H-002 is about POSIX creation modes; the Windows half of that
 *     question is an ACL question and is not modelled here.
 */

#include "proven.h"
#include "../platform/proven_sys_random_chunk.h"   /* the planner the Windows entropy path uses */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdarg.h>

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#define PLATFORM_NAME "Windows"
#else
#include <unistd.h>
#include <sys/stat.h>
#define PLATFORM_NAME "POSIX"
#endif

/* ------------------------------------------------------------------ reporting */

static FILE *report_file;
static int checks_run;
static int checks_failed;
static int notes_recorded;

static void emit(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    if (report_file) {
        va_start(ap, fmt);
        vfprintf(report_file, fmt, ap);
        va_end(ap);
    }
    fflush(stdout);
}

/* A check has an expected answer and a verdict. */
static void check(const char *id, const char *what, bool ok, const char *detail) {
    checks_run++;
    if (!ok) checks_failed++;
    emit("[%-4s] %-46s %s%s%s\n", id, what, ok ? "PASS" : "FAIL",
         (detail && *detail) ? "  -- " : "", (detail && *detail) ? detail : "");
}

/* An observation has NO expected answer yet: the RFC asks for the behaviour to be
 * defined, and defining it needs someone to see what the platform actually does. These
 * never fail the run; they are the reason a person is being asked to run it. */
static void note(const char *id, const char *what, const char *observed) {
    notes_recorded++;
    emit("[%-4s] %-46s ....  %s\n", id, what, observed);
}

static const char *err_name(proven_err_t e) {
    switch (e) {
        case PROVEN_OK:                   return "OK";
        case PROVEN_ERR_NOMEM:            return "NOMEM";
        case PROVEN_ERR_OUT_OF_BOUNDS:    return "OUT_OF_BOUNDS";
        case PROVEN_ERR_INVALID_ENCODING: return "INVALID_ENCODING";
        case PROVEN_ERR_INVALID_ARG:      return "INVALID_ARG";
        case PROVEN_ERR_IO:               return "IO";
        case PROVEN_ERR_NOT_FOUND:        return "NOT_FOUND";
        case PROVEN_ERR_INVALID_STATE:    return "INVALID_STATE";
        case PROVEN_ERR_NEED_MORE:        return "NEED_MORE";
        case PROVEN_ERR_OVERFLOW:         return "OVERFLOW";
        case PROVEN_ERR_UNSUPPORTED:      return "UNSUPPORTED";
        case PROVEN_ERR_AGAIN:            return "AGAIN";
        case PROVEN_ERR_EOF:              return "EOF";
        case PROVEN_ERR_BUSY:             return "BUSY";
        case PROVEN_ERR_PERMISSION:       return "PERMISSION";
        case PROVEN_ERR_INVALID_FORMAT:   return "INVALID_FORMAT";
        default:                          return "UNKNOWN";
    }
}

/* ------------------------------------------------------------------ helpers */

static char work_dir[512];
static char path_a[768];
static char path_b[768];

static proven_u8str_view_t V(const char *s) {
    return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)s, .size = strlen(s) };
}

static proven_mem_view_t B(const char *s) {
    return (proven_mem_view_t){ .ptr = (const proven_byte_t *)s, .size = strlen(s) };
}

static const char *joined(char *dst, size_t cap, const char *name) {
    snprintf(dst, cap, "%s/%s", work_dir, name);
    return dst;
}

/* Reads a whole file and compares it with `expect`. Missing or unreadable counts as a
 * mismatch, which is what the caller means by asking. */
static bool file_is(proven_allocator_t a, const char *path, const char *expect) {
    proven_result_mem_mut_t r = proven_fs_read_all(a, V(path));
    if (!proven_is_ok(r.err)) return false;
    size_t n = strlen(expect);
    bool same = r.value.size == n && memcmp(r.value.ptr, expect, n) == 0;
    a.free_fn(a.ctx, r.value.ptr);
    return same;
}

/*
 * Any staging file left behind is debris: a failed replacement must clean up after itself,
 * and a successful one has renamed its temp away.
 *
 * Collect the names FIRST and act afterwards. Removing entries while a directory is being
 * iterated is not something a directory iterator promises to survive, and the first version
 * of this file also had a worse problem: it counted whatever was in the directory, so one
 * leftover file from an earlier run made every later run fail three checks at once and
 * blame the code under test. Each phase now starts from an empty directory, and a leftover
 * is named rather than merely counted.
 */
#define STAGING_MAX 32
static char staging_names[STAGING_MAX][256];
static int staging_count;
static char staging_summary[512];

static void collect_staging(proven_allocator_t a) {
    staging_count = 0;
    staging_summary[0] = '\0';
    proven_result_dir_t d = proven_fs_dir_open(a, V(work_dir));
    if (!proven_is_ok(d.err)) { staging_count = -1; return; }
    proven_fs_dir_entry_t e;
    while (proven_is_ok(proven_fs_dir_next(&d.value, &e)) && staging_count < STAGING_MAX) {
        bool is_staging = false;
        for (proven_size_t i = 0; e.name.size >= 6 && i + 6 <= e.name.size; ++i) {
            if (memcmp(e.name.ptr + i, ".pvtmp", 6) == 0) { is_staging = true; break; }
        }
        if (is_staging && e.name.size < sizeof staging_names[0]) {
            memcpy(staging_names[staging_count], e.name.ptr, e.name.size);
            staging_names[staging_count][e.name.size] = '\0';
            staging_count++;
        }
    }
    proven_fs_dir_close(&d.value);

    /* Name them. "SOME" told us nothing; a name says which operation left it. */
    size_t used = 0;
    for (int i = 0; i < staging_count && i < 3; ++i) {
        int n = snprintf(staging_summary + used, sizeof staging_summary - used,
                         "%s%s", used ? ", " : "", staging_names[i]);
        if (n < 0) break;
        used += (size_t)n;
        if (used >= sizeof staging_summary - 1) break;
    }
}

static int count_staging_files(proven_allocator_t a) {
    collect_staging(a);
    return staging_count;
}

/* Empty the directory of staging files so the next phase is judged on its own work.
 * chmod first: on Windows a read-only file cannot be deleted, which is the very defect
 * this program is here to catch, and the cleanup must not fall over the same way. */
static int purge_staging(proven_allocator_t a) {
    collect_staging(a);
    int removed = 0;
    for (int i = 0; i < staging_count; ++i) {
        char full[1024];
        snprintf(full, sizeof full, "%.511s/%.255s", work_dir, staging_names[i]);
        (void)proven_fs_chmod(a, V(full), (proven_fs_perms_t)0600u);
        if (proven_is_ok(proven_fs_remove(a, V(full)))) removed++;
    }
    return removed;
}

/* ------------------------------------------------------------------ the checks */

static void section(const char *title) {
    emit("\n-- %s\n", title);
}

static void check_h005(proven_allocator_t a) {
    section("H-005  replacing a file that already exists");

    const char *target = joined(path_a, sizeof path_a, "replace-me.txt");
    (void)proven_fs_remove(a, V(target));

    proven_err_t first = proven_fs_write_file_atomic(a, V(target), B("FIRST"));
    check("A1", "atomic write to a name that does not exist", first == PROVEN_OK, err_name(first));
    check("A2", "  and the file holds what was written", file_is(a, target, "FIRST"), "");

    /* THE defect. MoveFileW refuses an existing destination, so this call used to fail on
     * Windows while the same code passed everywhere else. */
    proven_err_t second = proven_fs_write_file_atomic(a, V(target), B("SECOND"));
    check("A3", "SECOND atomic write over the same name", second == PROVEN_OK, err_name(second));
    check("A4", "  and the file now holds the new contents", file_is(a, target, "SECOND"), "");

    proven_err_t durable = proven_fs_write_file_durable(a, V(target), B("THIRD"));
    check("A5", "durable write over an existing file", durable == PROVEN_OK, err_name(durable));
    check("A6", "  and the file holds the newest contents", file_is(a, target, "THIRD"), "");

    int debris = count_staging_files(a);
    check("A7", "no staging file left behind", debris == 0,
          debris < 0 ? "could not list the directory" : staging_summary);

    /* A name that is not ASCII, and a long one. Windows stores names as UTF-16 and the
     * platform layer converts; a conversion that loses bytes shows up here. */
    const char *unicode = joined(path_b, sizeof path_b, "\xed\x95\x9c\xea\xb8\x80-name.txt");
    (void)proven_fs_remove(a, V(unicode));
    proven_err_t u1 = proven_fs_write_file_atomic(a, V(unicode), B("U1"));
    proven_err_t u2 = proven_fs_write_file_atomic(a, V(unicode), B("U2"));
    check("A8", "a non-ASCII name can be written and replaced",
          u1 == PROVEN_OK && u2 == PROVEN_OK && file_is(a, unicode, "U2"), err_name(u2));
    (void)proven_fs_remove(a, V(unicode));

    char longname[256];
    memset(longname, 'x', 200);
    longname[200] = '\0';
    char longpath[1024];
    /* The precisions are not decoration: -Werror=format-truncation wants to see that
     * neither piece can overrun, and it is right to. */
    snprintf(longpath, sizeof longpath, "%.511s/%.200s.txt", work_dir, longname);
    (void)proven_fs_remove(a, V(longpath));
    proven_err_t l1 = proven_fs_write_file_atomic(a, V(longpath), B("L1"));
    proven_err_t l2 = proven_fs_write_file_atomic(a, V(longpath), B("L2"));
    check("A9", "a 200-character name can be written and replaced",
          l1 == PROVEN_OK && l2 == PROVEN_OK && file_is(a, longpath, "L2"), err_name(l2));
    (void)proven_fs_remove(a, V(longpath));
}

static void check_h005_failure_path(proven_allocator_t a) {
    section("H-005  what happens when the replacement CANNOT succeed");

    const char *target = joined(path_a, sizeof path_a, "held-open.txt");
    (void)proven_fs_remove(a, V(target));
    proven_err_t seed = proven_fs_write_file(a, V(target), B("OLD"));
    if (seed != PROVEN_OK) {
        note("B0", "could not seed the fixture", err_name(seed));
        return;
    }

#if defined(_WIN32) || defined(_WIN64)
    /* Hold the destination open with no sharing at all. A replacement cannot proceed
     * against that, which is the closest thing to an injected failure that needs no
     * special privilege. What must survive it: the OLD contents, and no debris. */
    wchar_t wpath[1024];
    int wn = MultiByteToWideChar(CP_UTF8, 0, target, -1, wpath, 1024);
    HANDLE held = INVALID_HANDLE_VALUE;
    if (wn > 0) {
        held = CreateFileW(wpath, GENERIC_READ, 0 /* no sharing */, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    }
    if (held == INVALID_HANDLE_VALUE) {
        note("B1", "could not hold the destination open", "skipped");
    } else {
        (void)purge_staging(a);   /* judge this phase on its own work */
        proven_err_t blocked = proven_fs_write_file_atomic(a, V(target), B("NEW"));
        CloseHandle(held);
        /* This WAS a note. The VM run of 2026-09-11 showed it said PERMISSION - telling the
         * caller the file was protected when it was only in use. Windows' rename does not
         * distinguish the two; the library now asks the file, and in use means BUSY. */
        check("B1", "atomic write while the target is held open is BUSY, not PERMISSION",
              blocked == PROVEN_ERR_BUSY, err_name(blocked));
        check("B2", "  the OLD contents survive a failed replacement",
              blocked != PROVEN_OK ? file_is(a, target, "OLD") : file_is(a, target, "NEW"),
              blocked == PROVEN_OK ? "it succeeded, so NEW is correct" : "");
        int debris = count_staging_files(a);
        check("B3", "  no staging file left behind", debris == 0, staging_summary);
    }

    /* The same, but held the way this library's own proven_fs_open holds a file: with
     * read, write AND delete sharing. POSIX replaces a file that a reader has open and the
     * reader keeps the old bytes; MoveFileExW refuses it. Recorded, not asserted: whether
     * Windows should do what POSIX does here is the owner's decision (RFC-0006 decision 2). */
    held = (wn > 0) ? CreateFileW(wpath, GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL)
                    : INVALID_HANDLE_VALUE;
    if (held != INVALID_HANDLE_VALUE) {
        (void)purge_staging(a);
        proven_err_t shared = proven_fs_write_file_atomic(a, V(target), B("NEW2"));
        CloseHandle(held);
        note("B7", "atomic write while a reader holds it with delete sharing", err_name(shared));
        check("B8", "  and whatever happened, the file is whole (old or new)",
              file_is(a, target, "OLD") || file_is(a, target, "NEW2"), "");
    }

    /* A read-only destination. RFC-0006 asks for this behaviour to be DEFINED, and
     * defining it needs someone to see what Windows actually does - so it is recorded,
     * not asserted. */
    const char *ro = joined(path_b, sizeof path_b, "read-only.txt");
    (void)proven_fs_remove(a, V(ro));
    (void)proven_fs_write_file(a, V(ro), B("OLD"));
    (void)proven_fs_chmod(a, V(ro), (proven_fs_perms_t)0444u);
    (void)purge_staging(a);   /* judge this phase on its own work */
    proven_err_t ro_err = proven_fs_write_file_atomic(a, V(ro), B("NEW"));
    /* This WAS an observation - the RFC asked for the behaviour to be defined and it took a
     * native run to see what there was to define. It is now a rule: a destination the caller
     * marked as not-to-be-written is refused, on every platform, by every whole-file
     * replacement, with the same error. */
    check("B4", "atomic write over a READ-ONLY destination is refused",
          ro_err == PROVEN_ERR_PERMISSION, err_name(ro_err));
    check("B5", "  and the old contents are untouched", file_is(a, ro, "OLD"),
          file_is(a, ro, "NEW") ? "it was REPLACED" : "");

    /* This one WAS an observation, and the first native run turned it into a defect: the
     * staging file was left behind. It carried the target's read-only mode, and Windows
     * will not delete a read-only file, so the cleanup failed silently. Now it is a check,
     * because the answer is no longer open to opinion - debris is debris. */
    int ro_debris = count_staging_files(a);
    check("B6", "  no staging file left behind after that failure", ro_debris == 0,
          ro_debris > 0 ? staging_summary : "");
    (void)proven_fs_chmod(a, V(ro), (proven_fs_perms_t)0644u);
    (void)proven_fs_remove(a, V(ro));
#else
    note("B1", "holding the target open blocks nothing here", "not applicable on POSIX");

    /* The same question POSIX-side, so the two platforms can be compared rather than
     * guessed at. They do NOT agree: rename here needs write permission on the DIRECTORY,
     * not on the file, so replacing a read-only file simply works. */
    const char *ro = joined(path_b, sizeof path_b, "read-only.txt");
    (void)proven_fs_remove(a, V(ro));
    (void)proven_fs_write_file(a, V(ro), B("OLD"));
    (void)proven_fs_chmod(a, V(ro), (proven_fs_perms_t)0444u);
    (void)purge_staging(a);   /* judge this phase on its own work */
    proven_err_t ro_err = proven_fs_write_file_atomic(a, V(ro), B("NEW"));
    check("B4", "atomic write over a READ-ONLY destination is refused",
          ro_err == PROVEN_ERR_PERMISSION, err_name(ro_err));
    check("B5", "  and the old contents are untouched", file_is(a, ro, "OLD"),
          file_is(a, ro, "NEW") ? "it was REPLACED" : "");
    int ro_debris = count_staging_files(a);
    check("B6", "  no staging file left behind after that call", ro_debris == 0,
          ro_debris > 0 ? staging_summary : "");
    (void)proven_fs_chmod(a, V(ro), (proven_fs_perms_t)0644u);
    (void)proven_fs_remove(a, V(ro));
#endif

    (void)proven_fs_remove(a, V(target));
}

static void check_h006(proven_allocator_t a) {
    (void)a;
    section("H-006  entropy: every byte asked for is asked for");

    /* Ordinary sizes. A generator that fills nothing, or fills a prefix and reports
     * success, shows up as an improbable run of zero bytes. */
    static proven_byte_t buf[1u << 20];
    const size_t sizes[] = { 1, 63, 64, 4096, sizeof buf };
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; ++i) {
        size_t n = sizes[i];
        memset(buf, 0, n);
        bool ok = proven_random_bytes(buf, n);
        size_t run = 0, longest = 0;
        for (size_t j = 0; j < n; ++j) {
            if (buf[j] == 0) { run++; if (run > longest) longest = run; } else run = 0;
        }
        char detail[96];
        snprintf(detail, sizeof detail, "%zu bytes, longest zero run %zu", n, longest);
        /* 16 consecutive zero bytes in real entropy is about 2^-128. A tail that was
         * never written is nothing but zero bytes. */
        check("C1", "random_bytes fills the whole buffer", ok && longest < 16, detail);
    }

    check("C2", "a zero-length request succeeds", proven_random_bytes(buf, 0), "");

    /* The arithmetic the defect actually lived in, at the boundaries, with a reduced
     * limit standing in for ULONG_MAX. Asking for 4 GiB for real would need 4 GiB. */
    const size_t L = 64;
    bool planner_ok =
        proven_sys_random_chunk(0, L) == 0 &&
        proven_sys_random_chunk(1, L) == 1 &&
        proven_sys_random_chunk(L - 1, L) == L - 1 &&
        proven_sys_random_chunk(L, L) == L &&
        proven_sys_random_chunk(L + 1, L) == L &&
        proven_sys_random_chunk(2 * L + 1, L) == L;
    check("C3", "the chunk planner is right at its boundaries", planner_ok, "");

    size_t remaining = 2 * L + 1, covered = 0, calls = 0;
    while (remaining > 0 && calls < 100) {
        size_t c = proven_sys_random_chunk(remaining, L);
        if (c == 0) break;
        covered += c;
        remaining -= c;
        calls++;
    }
    check("C4", "a whole plan covers the buffer exactly",
          remaining == 0 && covered == 2 * L + 1 && calls == 3, "");
}

static void check_general(proven_allocator_t a) {
    section("general: the library on this platform at all");

    const char *f = joined(path_a, sizeof path_a, "basic.txt");
    (void)proven_fs_remove(a, V(f));
    proven_err_t w = proven_fs_write_file(a, V(f), B("hello"));
    check("D1", "write a whole file", w == PROVEN_OK, err_name(w));
    check("D2", "read it back", file_is(a, f, "hello"), "");

    proven_fs_stat_t st = {0};
    proven_err_t se = proven_fs_stat(a, V(f), &st);
    check("D3", "stat says it is a regular file of 5 bytes",
          se == PROVEN_OK && st.type == PROVEN_FS_TYPE_FILE && st.size == 5, err_name(se));

    const char *c = joined(path_b, sizeof path_b, "basic-copy.txt");
    (void)proven_fs_remove(a, V(c));
    proven_err_t ce = proven_fs_copy(a, V(f), V(c));
    check("D4", "copy carries the contents", ce == PROVEN_OK && file_is(a, c, "hello"), err_name(ce));

    /* Renaming ONTO an existing name is the same primitive H-005 fixed, reached
     * through the public API rather than through an atomic write. */
    proven_err_t re = proven_fs_rename(a, V(c), V(f));
    check("D5", "rename onto an existing name", re == PROVEN_OK, err_name(re));

    proven_err_t rm = proven_fs_remove(a, V(f));
    check("D6", "remove", rm == PROVEN_OK, err_name(rm));

    /* Documented to be UNSUPPORTED on Windows rather than a silent no-op reported as
     * success. Recorded so the answer is on the record for both platforms. */
    proven_err_t sd = proven_fs_sync_dir(a, V(work_dir));
    note("D7", "sync_dir on this platform", err_name(sd));

    /* The rule is only a rule if every door obeys it. */
    {
        const char *prot = joined(path_b, sizeof path_b, "protected.txt");
        (void)proven_fs_chmod(a, V(prot), (proven_fs_perms_t)0600u);
        (void)proven_fs_remove(a, V(prot));
        (void)proven_fs_write_file(a, V(prot), B("KEEP"));
        (void)proven_fs_chmod(a, V(prot), (proven_fs_perms_t)0444u);
        check("D10", "plain write over a protected file is refused",
              proven_fs_write_file(a, V(prot), B("NEW")) == PROVEN_ERR_PERMISSION, "");
        const char *any = joined(path_a, sizeof path_a, "any-src.txt");
        (void)proven_fs_write_file(a, V(any), B("SRC"));
        check("D11", "copy over a protected file is refused",
              proven_fs_copy(a, V(any), V(prot)) == PROVEN_ERR_PERMISSION, "");
        check("D12", "  and it still holds what it held", file_is(a, prot, "KEEP"), "");
        (void)proven_fs_chmod(a, V(prot), (proven_fs_perms_t)0600u);
        (void)proven_fs_remove(a, V(prot));
        (void)proven_fs_remove(a, V(any));
    }

    proven_byte_t enc[64];
    proven_size_t written = 0;
    proven_err_t he = proven_hex_encode(B("abc"), enc, sizeof enc, &written);
    check("D8", "hex encode still works", he == PROVEN_OK && written == 6 &&
          memcmp(enc, "616263", 6) == 0, err_name(he));
    check("D9", "an impossible encoded size is refused, not wrapped",
          proven_hex_encoded_size(PROVEN_SIZE_MAX / 2 + 1) == PROVEN_SIZE_MAX, "");

    proven_job_sys_t *sys = NULL;
    proven_err_t je = proven_job_system_init(a, 2, 8, &sys);
    check("E1", "a job system starts", je == PROVEN_OK && sys != NULL, err_name(je));
    if (proven_is_ok(je) && sys) {
        int accepted = 0;
        for (int i = 0; i < 32; ++i) if (proven_job_submit(sys, NULL, NULL)) accepted++;
        check("E2", "it accepts work", accepted > 0, "");
        proven_job_system_destroy(sys);
        check("E3", "and shuts down without hanging", true, "");
    }
}

/* ------------------------------------------------------------------ main */

int main(void) {
    proven_allocator_t a = proven_heap_allocator();

    report_file = fopen("proven-windows-check-report.txt", "w");

    emit("proven_c_lib - RFC-0006 runtime check\n");
    emit("platform reported by this build: %s\n", PLATFORM_NAME);
    emit("pointer size: %d bytes\n", (int)sizeof(void *));
    emit("library version: %s\n", PROVEN_VERSION_STRING);
    emit("\nThis answers RFC-0006 H-005 and H-006, which were fixed by inspection and\n");
    emit("have never been run on Windows. Send the whole output back.\n");

    /* Work somewhere writable and disposable, beside the executable. */
    snprintf(work_dir, sizeof work_dir, "proven-check-work");
    (void)proven_fs_mkdir(a, V(work_dir));

    proven_err_t probe = proven_fs_write_file(a, V(joined(path_a, sizeof path_a, "probe")), B("x"));
    if (probe != PROVEN_OK) {
        emit("\nSTOP: cannot write into %s (%s).\n", work_dir, err_name(probe));
        emit("Run this from a folder you can write to, then try again.\n");
        if (report_file) fclose(report_file);
        return 2;
    }
    (void)proven_fs_remove(a, V(path_a));

    /*
     * Start from an empty directory, and SAY SO if it was not empty.
     *
     * The first version did not do this, and the second run of it failed three checks that
     * had passed the first time - all of them counting one file the first run had left
     * behind. A verifier that blames the code under test for its own leftovers is worse
     * than no verifier, because it is believed.
     */
    int stale = purge_staging(a);
    if (stale > 0) {
        char detail[128];
        snprintf(detail, sizeof detail, "%d removed before starting", stale);
        note("S1", "staging files left by an EARLIER run", detail);
        emit("       (that is this program's own leftover, not a failure of the library.\n");
        emit("        The checks below start from an empty directory.)\n");
    }

    check_h005(a);
    check_h005_failure_path(a);
    check_h006(a);
    check_general(a);

    /*
     * Leave nothing behind except the report - and check that it worked, because the
     * failure to do this is what made the second run of the first version lie.
     */
    {
        const char *names[] = { "replace-me.txt", "basic.txt", "basic-copy.txt",
                                "held-open.txt", "read-only.txt", "probe" };
        for (size_t i = 0; i < sizeof names / sizeof names[0]; ++i) {
            const char *f = joined(path_a, sizeof path_a, names[i]);
            (void)proven_fs_chmod(a, V(f), (proven_fs_perms_t)0600u);
            (void)proven_fs_remove(a, V(f));
        }
        (void)purge_staging(a);
        proven_err_t rd = proven_fs_rmdir(a, V(work_dir));
        if (!proven_is_ok(rd)) {
            collect_staging(a);
            note("S2", "the work directory could not be removed",
                 staging_count > 0 ? staging_summary : "something else is in it");
            emit("       (remove the folder \"%s\" by hand before running this again,\n", work_dir);
            emit("        or the next run will start with leftovers.)\n");
        }
    }

    emit("\n");
    emit("=================================================================\n");
    emit("checks: %d   failed: %d   observations (no expected answer): %d\n",
         checks_run, checks_failed, notes_recorded);
    emit("VERDICT: %s\n", checks_failed == 0 ? "ALL CHECKS PASSED" : "SOMETHING FAILED");
    emit("=================================================================\n");
    emit("\nThe report was also written to proven-windows-check-report.txt\n");
    emit("in the folder you ran this from. Send that file back.\n");

    if (report_file) fclose(report_file);

#if defined(_WIN32) || defined(_WIN64)
    /* A double-clicked console window closes the instant main returns, taking the
     * report with it. */
    printf("\nPress Enter to close.\n");
    (void)getchar();
#endif

    return checks_failed == 0 ? 0 : 1;
}
