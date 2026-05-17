#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

static void require(bool cond, const char *msg) {
    if (!cond) {
        fprintf(stderr, "[FAIL] %s\n", msg);
        exit(1);
    }
}

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

    return 0;
}
