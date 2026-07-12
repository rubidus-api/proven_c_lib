#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

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

int main(void) {
    char *fs = read_text_file("platform/proven_sys_fs.c");
    require(contains(fs, "if (!fd)"), "Windows directory open checks fd allocation before FindFirstFileW");
    require(contains(fs, "if (!wd)"), "Windows directory open checks win_dir allocation before dereference");
    require(contains(fs, "116444736000000000ULL"), "Windows FILETIME conversion uses the Unix epoch delta");
    require(contains(fs, "ull.QuadPart < 116444736000000000ULL"), "Windows FILETIME conversion checks pre-1970 underflow");
    require(contains(fs, "off_t mmap_offset"), "POSIX mmap stores offset in an off_t temporary");
    require(contains(fs, "(size_t)mmap_offset != offset"), "POSIX mmap rejects size_t to off_t truncation");
    require(contains(fs, "FILE_APPEND_DATA"), "Windows append opens with FILE_APPEND_DATA");
    require(!contains(fs, "SetFilePointer(h, 0, NULL, FILE_END)"), "Windows append does not emulate O_APPEND with a one-time seek");
    free(fs);

    char *env = read_text_file("platform/proven_sys_env.c");
    require(!contains(env, "wchar_t wname[256]"), "Windows env key conversion has no 255-byte fixed limit");
    require(contains(env, "MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, NULL, 0)"), "Windows env key conversion sizes its buffer first");
    free(env);

    char *sysio = read_text_file("src/proven/sysio.c");
    require(!contains(sysio, "char key_buf[256]"), "public env API has no 255-byte fixed key limit");
    require(!contains(sysio, "key.size >= 256"), "public env API does not reject oversized keys before PAL lookup");
    free(sysio);

    char *io = read_text_file("platform/proven_sys_io.c");
    require(contains(io, "uint64_t result_off = 0"), "32-bit Linux _llseek uses a 64-bit result buffer");
    require(!contains(io, "unsigned long result_off = 0"), "32-bit Linux _llseek no longer uses unsigned long result buffers");
    free(io);

    char *job = read_text_file("src/proven/job.c");
    require(contains(job, "admission_state"), "job system has a single admission state");
    require(contains(job, "proven_job_begin_submit"), "job submit claims admission before queue slot claim");
    require(contains(job, "proven_job_end_submit"), "job submit releases admission after commit");
    free(job);

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
    free(nob);

    char *test_md = read_text_file("TEST.md");
    require(contains(test_md, "Failure tip"), "TEST.md documents failure tips for test modes and individual tests");
    require(contains(test_md, "Sub-checks"), "TEST.md documents the lower-level checks covered by each test executable");
    require(contains(test_md, "Log format"), "TEST.md documents the standardized test log format");
    free(test_md);

    return 0;
}
