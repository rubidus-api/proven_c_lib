#include "proven.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

static const char *const build_header_manifest[] = {
#undef PROVEN_BUILD_HEADERS_MANIFEST_INCLUDED
#define PROVEN_BUILD_HEADER(path) path,
#include "../build_headers.inc"
#ifndef PROVEN_BUILD_HEADERS_MANIFEST_INCLUDED
#error "build_headers.inc must define PROVEN_BUILD_HEADERS_MANIFEST_INCLUDED"
#endif
#undef PROVEN_BUILD_HEADER
};

static void require_impl(bool cond, const char *cond_text, const char *msg, const char *file, int line) {
    if (!cond) {
        fprintf(stderr, "\n[PROVEN][CHECK][FAIL] file=%s line=%d\n", file, line);
        fprintf(stderr, "[PROVEN][CHECK][COND] %s\n", cond_text);
        fprintf(stderr, "[PROVEN][CHECK][INTENT] %s\n", msg);
        fprintf(stderr, "[PROVEN][CHECK][FAIL_HINT] This source-contract check protects a portability or documentation invariant. Inspect the named file and restore the expected safe pattern.\n");
        exit(1);
    }
}

#define require(cond, msg) require_impl((cond), #cond, (msg), __FILE__, __LINE__)

static char *read_text_file(const char *path) {
    FILE *f = fopen(path, "rb");
    require(f != NULL, path);
    require(fseek(f, 0, SEEK_END) == 0, "seek end");
    long n = ftell(f);
    require(n >= 0, "tell");
    require(fseek(f, 0, SEEK_SET) == 0, "seek set");
    char *buf = (char*)malloc((size_t)n + 1u);
    require(buf != NULL, "malloc");
    size_t got = fread(buf, 1u, (size_t)n, f);
    require(got == (size_t)n, "read full file");
    buf[got] = '\0';
    fclose(f);
    return buf;
}

static bool contains(const char *haystack, const char *needle) {
    return strstr(haystack, needle) != NULL;
}

static bool build_manifest_contains(const char *path) {
    for (size_t i = 0; i < sizeof build_header_manifest / sizeof build_header_manifest[0]; ++i) {
        if (strcmp(build_header_manifest[i], path) == 0) return true;
    }
    return false;
}

static int require_manifest_paths_exist(void) {
    int missing = 0;
    for (size_t i = 0;
         i < sizeof build_header_manifest / sizeof build_header_manifest[0];
         ++i) {
        FILE *f = fopen(build_header_manifest[i], "rb");
        if (!f) {
            fprintf(stderr,
                    "[PROVEN][TEST][INFO] dependency manifest path does not exist: %s\n",
                    build_header_manifest[i]);
            ++missing;
            continue;
        }
        fclose(f);
    }
    return missing;
}

static int require_header_dir_in_manifest(const char *dir_path) {
    proven_allocator_t heap = proven_heap_allocator();
    proven_result_dir_t opened = proven_fs_dir_open(heap, proven_u8str_view_from_cstr(dir_path));
    require(proven_is_ok(opened.err), "open header directory");

    int missing = 0;
    proven_fs_dir_entry_t entry = {0};
    proven_err_t step = PROVEN_OK;
    while (proven_is_ok(step = proven_fs_dir_next(&opened.value, &entry))) {
        proven_size_t len = entry.name.size;
        if (len < 3 || memcmp(entry.name.ptr + len - 2, ".h", 2) != 0) continue;

        char path[512];
        if (len > 255 || snprintf(path, sizeof path, "%s/%.*s", dir_path, (int)len,
                                  (const char *)entry.name.ptr) >= (int)sizeof path) {
            fprintf(stderr, "[PROVEN][TEST][INFO] header path is too long for manifest audit: %s\n",
                    dir_path);
            ++missing;
            continue;
        }
        if (!build_manifest_contains(path)) {
            fprintf(stderr, "[PROVEN][TEST][INFO] header absent from nob.c dependency manifest: %s\n",
                    path);
            ++missing;
        }
    }
    proven_fs_dir_close(&opened.value);
    require(step == PROVEN_ERR_EOF, "finish header directory scan");
    return missing;
}

int main(void) {
    char *fs = read_text_file("platform/proven_sys_fs.c");
    require(contains(fs, "if (!fd)"), "Windows directory open checks fd allocation before FindFirstFileW");
    require(contains(fs, "if (!wd)"), "Windows directory open checks win_dir allocation before dereference");
    require(contains(fs, "116444736000000000ULL"), "Windows FILETIME conversion uses the Unix epoch delta");
    require(contains(fs, "ull.QuadPart < 116444736000000000ULL"), "Windows FILETIME conversion checks pre-1970 underflow");
    require(contains(fs, "off_t mmap_offset"), "POSIX mmap stores offset in an off_t temporary");
    require(contains(fs, "(size_t)mmap_offset != offset"), "POSIX mmap rejects size_t to off_t truncation");
    require(contains(fs, "FILE_APPEND_DATA"), "Windows append opens with FILE_APPEND_DATA");
    /* readdir() returns NULL for BOTH "the directory ended" and "the read failed", and
     * only errno tells them apart - so it must be cleared first. A listing cut short by
     * a failing disk or a vanished NFS mount used to look exactly like a complete one,
     * which is how a backup silently skips files. There is no way to provoke a readdir
     * failure from a test on a healthy host, so the contract lives here, in the source. */
    require(contains(fs, "int proven_sys_fs_dir_step"), "the PAL directory walk can report failure, not just end-of-directory");
    require(contains(fs, "errno = 0;"), "POSIX dir_step clears errno so a NULL readdir can be told apart from a failure");
    require(contains(fs, "return (errno != 0) ? -1 : 0;"), "POSIX dir_step reports a readdir failure as failure");
    require(contains(fs, "ERROR_NO_MORE_FILES"), "Windows dir_step tells a finished directory apart from a failed one");
    require(!contains(fs, "SetFilePointer(h, 0, NULL, FILE_END)"), "Windows append does not emulate O_APPEND with a one-time seek");
    free(fs);

    char *fs_c = read_text_file("src/proven/fs.c");
    require(contains(fs_c, "proven_sys_fs_dir_step"), "the public directory walk uses the failure-reporting PAL entry point");
    require(!contains(fs_c, "if (!proven_sys_fs_dir_next(dh, &se)) return PROVEN_ERR_EOF;"),
            "a failed directory read is not reported to the caller as end-of-directory");
    free(fs_c);

    char *env = read_text_file("platform/proven_sys_env.c");
    require(!contains(env, "wchar_t wname[256]"), "Windows env key conversion has no 255-byte fixed limit");
    require(contains(env, "MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, NULL, 0)"), "Windows env key conversion sizes its buffer first");
    free(env);

    char *sysio = read_text_file("src/proven/sysio.c");
    require(!contains(sysio, "char key_buf[256]"), "public env API has no 255-byte fixed key limit");
    require(!contains(sysio, "key.size >= 256"), "public env API does not reject oversized keys before PAL lookup");
    free(sysio);

    /* The PAL used to implement read, write and seek in inline-assembly raw
     * syscalls, one path per architecture. It bought nothing - proven_sys_fs.c in
     * the same library already calls libc's open/read/write - and it cost real
     * things: three of the four paths were unverifiable on a machine without the
     * cross-toolchains, and because the console path issued raw `syscall`
     * instructions, LD_PRELOAD tracing was BLIND to every one of this library's
     * console writes.
     *
     * It is gone. This contract keeps it gone: an architecture-specific syscall
     * path is a thing you add on purpose, not something that creeps back. */
    char *io = read_text_file("platform/proven_sys_io.c");
    require(!contains(io, "__asm__ volatile"), "the PAL uses libc, not hand-written syscall assembly");
    require(contains(io, "lseek("), "seek goes through libc lseek");
    require(contains(io, "fsync("), "durability goes through libc fsync");
    free(io);

    char *job = read_text_file("src/proven/job.c");
    require(contains(job, "admission_state"), "job system has a single admission state");
    require(contains(job, "proven_job_begin_submit"), "job submit claims admission before queue slot claim");
    require(contains(job, "proven_job_end_submit"), "job submit releases admission after commit");
    require(contains(job, "proven_sys_semaphore_wait"),
            "an idle job worker parks on the thread PAL instead of polling");
    require(contains(job, "proven_sys_semaphore_post"),
            "job publication and close wake parked workers");
    require(!contains(job, "Back-off to prevent 100% CPU starvation"),
            "the idle worker loop no longer repeatedly yields");
    free(job);

    char *thread_h = read_text_file("platform/proven_sys_thread.h");
    require(contains(thread_h, "proven_sys_semaphore_t"),
            "the thread PAL exposes an opaque counting semaphore");
    require(contains(thread_h, "proven_sys_semaphore_init"),
            "the thread PAL can initialize a counting semaphore");
    require(contains(thread_h, "proven_sys_semaphore_wait"),
            "the thread PAL can park on a counting semaphore");
    require(contains(thread_h, "proven_sys_semaphore_post"),
            "the thread PAL can wake a parked waiter");
    require(contains(thread_h, "proven_sys_semaphore_destroy"),
            "the thread PAL can release semaphore resources");
    free(thread_h);

    char *thread_c = read_text_file("platform/proven_sys_thread.c");
    require(contains(thread_c, "CreateSemaphoreW"),
            "the Windows thread PAL uses a kernel semaphore");
    require(contains(thread_c, "WaitForSingleObject"),
            "the Windows semaphore wait blocks in the kernel");
    require(contains(thread_c, "ReleaseSemaphore"),
            "the Windows semaphore post retains wake permits");
    require(contains(thread_c, "pthread_cond_wait"),
            "the POSIX semaphore wait parks on a condition variable");
    require(contains(thread_c, "pthread_cond_signal"),
            "the POSIX semaphore post wakes one waiter");
    free(thread_c);

    char *nob = read_text_file("nob.c");
    require(contains(nob, "Proven_Test_Case"), "nob.c stores structured metadata for each test executable");
    require(contains(nob, "[PROVEN][BUILD][BEGIN]"), "nob.c announces the build mode and output directory before platform-specific setup");
    require(contains(nob, "[PROVEN][BUILD][NOTE]"), "nob.c warns when it detects a Windows or MSYS2 runtime that may need extra PATH/toolchain setup");
    require(contains(nob, "[PROVEN][BUILD][PHASE] library compilation start"), "nob.c prints a build-phase banner before source compilation begins");
    require(contains(nob, "[PROVEN][BUILD][PHASE] test link-and-run start"), "nob.c prints a build-phase banner before linked test executables run");
    require(contains(nob, "[PROVEN][BUILD][PASS]"), "nob.c prints a standard build pass line after all selected tests succeed");
    require(!contains(nob, "mkdir %s >nul"), "nob.c does not use silent cmd.exe mkdir redirection on Windows");
    require(!contains(nob, "mkdir -p %s"), "nob.c does not depend on a POSIX shell for recursive directory creation");
    require(contains(nob, "[PROVEN][TEST][BEGIN]"), "nob.c prints a standard test begin line before execution");
    require(contains(nob, "[PROVEN][TEST][INTENT]"), "nob.c prints each test executable intent before execution");
    require(contains(nob, "[PROVEN][TEST][FAIL_HINT]"), "nob.c prints a failure hint before each test executable runs");
    require(contains(nob, "[PROVEN][TEST][PASS]"), "nob.c prints a standard pass line after each test executable succeeds");

    /* nob.c and this test include the same preprocessed manifest. A commented or
     * disabled entry therefore cannot masquerade as an active dependency. Enumerate
     * the library, PAL, test, and example roots consumed by the build so a new helper
     * cannot silently leave stale objects behind. */
    require(contains(nob, "#include \"build_headers.inc\""),
            "nob.c consumes the shared preprocessed dependency manifest");
    require(contains(nob, "#ifndef PROVEN_BUILD_HEADERS_MANIFEST_INCLUDED"),
            "nob.c fails compilation if the header manifest include is disabled");
    require(contains(nob, "#include \"build_tests.inc\""),
            "nob.c consumes the shared preprocessed test registry");
    require(contains(nob, "#ifndef PROVEN_BUILD_TESTS_MANIFEST_INCLUDED"),
            "nob.c fails compilation if the test manifest include is disabled");
    require(contains(nob, "checked_needs_rebuild"),
            "nob.c checks the tri-state dependency timestamp result");
    require(contains(nob, "hash_paths_contents"),
            "nob.c fingerprints shared dependency contents once per build");
    require(contains(nob, "hash_file_contents(&current_hash, srcs[i])"),
            "object cache keys include source contents, not only coarse timestamps");
    require(contains(nob, "hash_file_contents(&current_hash, src_path)"),
            "test cache keys include test-source contents, not only coarse timestamps");
    require(contains(nob, "PROVEN_FREESTANDING_CROSS_SMOKE_PATH"),
            "freestanding cross coverage resolves a stable named path");
    require(!contains(nob, "PROVEN_FREESTANDING_CROSS_SMOKE_TEST"),
            "freestanding cross coverage does not depend on a registry index");
    require(contains(nob, "if (status < 0)"),
            "nob.c stops when a source or header timestamp cannot be inspected");
    require(!contains(nob, "bool rebuild = nob_needs_rebuild"),
            "nob.c does not coerce a dependency stat error into a rebuild request");
    require(build_manifest_contains("build_headers.inc"),
            "the shared manifest invalidates cached outputs when the manifest changes");
    require(build_manifest_contains("build_tests.inc"),
            "test registry changes invalidate cached test executables");
    require(require_manifest_paths_exist() == 0,
            "every path in the dependency manifest exists");
    int missing_headers = 0;
    static const char *header_dirs[] = {
        "include/proven", "src/proven", "platform", "tests", "manual/examples"
    };
    for (size_t i = 0; i < sizeof header_dirs / sizeof header_dirs[0]; ++i) {
        missing_headers += require_header_dir_in_manifest(header_dirs[i]);
    }
    require(build_manifest_contains("include/proven.h"),
            "the umbrella header is in the dependency manifest");
    require(missing_headers == 0,
            "every header in the build-consumed roots is present in nob.c's dependency manifest");
    free(nob);

    char *test_md = read_text_file("TEST.md");
    require(contains(test_md, "Failure tip"), "TEST.md documents failure tips for test modes and individual tests");
    require(contains(test_md, "Sub-checks"), "TEST.md documents the lower-level checks covered by each test executable");
    require(contains(test_md, "Log format"), "TEST.md documents the standardized test log format");
    free(test_md);

    /* Documentation gates are part of the complete hosted suite and therefore have
     * the same target contract as every other registered test. MSVC has no dirent.h;
     * directory enumeration must go through the library API, whose PAL already has a
     * Win32 implementation. This source-level check is the executable evidence we can
     * keep on hosts where MSVC itself is unavailable. */
    char *catalog = read_text_file("tests/test_docs_test_catalog.c");
    require(!contains(catalog, "#include <dirent.h>"),
            "the catalog gate does not import the POSIX-only dirent API");
    require(contains(catalog, "proven_fs_dir_open"),
            "the catalog gate enumerates tests through the portable filesystem API");
    require(contains(catalog, "proven_fs_dir_next"),
            "the catalog gate uses the portable directory step API");
    require(contains(catalog, "proven_fs_dir_close"),
            "the catalog gate closes the portable directory iterator");
    free(catalog);

    return 0;
}
