#include "proven_test.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>

static void write_text_file(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    PROVEN_TEST_ASSERT(f != NULL, "create temporary wrapper", "Check local write permissions before probing the build driver.");
    PROVEN_TEST_ASSERT(fputs(text, f) >= 0, "write temporary wrapper", "Check that the wrapper script body was written in full.");
    PROVEN_TEST_ASSERT(fclose(f) == 0, "close temporary wrapper", "Inspect the test host's temporary file handling.");
}

static char *read_all_text(const char *path) {
    FILE *f = fopen(path, "rb");
    PROVEN_TEST_ASSERT(f != NULL, "open wrapper probe log", "Check that the build driver reached the wrapper and wrote a probe log.");
    PROVEN_TEST_ASSERT(fseek(f, 0, SEEK_END) == 0, "seek probe log end", "Inspect the probe log path and file permissions.");
    long size = ftell(f);
    PROVEN_TEST_ASSERT(size >= 0, "size probe log", "Inspect the probe log path and file permissions.");
    PROVEN_TEST_ASSERT(fseek(f, 0, SEEK_SET) == 0, "rewind probe log", "Inspect the probe log path and file permissions.");
    char *buf = (char *)malloc((size_t)size + 1u);
    PROVEN_TEST_ASSERT(buf != NULL, "allocate probe log buffer", "Check the test host's memory availability.");
    size_t got = fread(buf, 1u, (size_t)size, f);
    PROVEN_TEST_ASSERT(got == (size_t)size, "read probe log", "Inspect the probe log path and file permissions.");
    buf[got] = '\0';
    fclose(f);
    return buf;
}

static bool file_contains(const char *path, const char *needle) {
    char *text = read_all_text(path);
    bool found = strstr(text, needle) != NULL;
    free(text);
    return found;
}

static int run_cmd(const char *cmd) {
    int rc = system(cmd);
    PROVEN_TEST_ASSERT(rc != -1, "launch build driver probe", "Check shell availability and command construction.");
    return rc;
}

int main(void) {
    PROVEN_TEST_SUITE(
        "test_nob_std_probe",
        "Verify the build driver probes -std=c23 first and falls back to -std=c2x when the compiler rejects the newer spelling.",
        "If this fails, inspect nob.c standard-flag selection and keep the fallback explicit, bounded, and logged."
    );

    const char *wrapper_path = "tests/test_nob_std_probe.fakecc";
    const char *log_path = "tests/test_nob_std_probe.log";
    const char *wrapper_body =
        "#!/bin/sh\n"
        "set -eu\n"
        "log=${PROVEN_STD_PROBE_LOG:-tests/test_nob_std_probe.log}\n"
        "real_cc=${PROVEN_STD_PROBE_REAL_CC:-cc}\n"
        "for arg in \"$@\"; do\n"
        "  if [ \"$arg\" = \"-std=c23\" ]; then\n"
        "    printf '%s\\n' \"reject c23: $*\" >> \"$log\"\n"
        "    exit 1\n"
        "  fi\n"
        "done\n"
        "for arg in \"$@\"; do\n"
        "  if [ \"$arg\" = \"-std=c2x\" ]; then\n"
        "    printf '%s\\n' \"accept c2x: $*\" >> \"$log\"\n"
        "    exec \"$real_cc\" \"$@\"\n"
        "  fi\n"
        "done\n"
        "printf '%s\\n' \"missing standard flag: $*\" >> \"$log\"\n"
        "exec \"$real_cc\" \"$@\"\n";

    (void)remove(wrapper_path);
    (void)remove(log_path);

    write_text_file(wrapper_path, wrapper_body);
    PROVEN_TEST_ASSERT(chmod(wrapper_path, 0755) == 0, "mark wrapper executable", "Check the test host's chmod support.");

    setenv("PROVEN_STD_PROBE_LOG", log_path, 1);
    setenv("PROVEN_STD_PROBE_REAL_CC", "cc", 1);

    int rc = run_cmd("./nob regression -build-root build/test_nob_std_probe -cc ./tests/test_nob_std_probe.fakecc");
    PROVEN_TEST_ASSERT(WIFEXITED(rc), "build driver exits normally", "Inspect the wrapper log and build-driver output for a compiler invocation failure.");
    PROVEN_TEST_ASSERT(WEXITSTATUS(rc) == 0, "build driver completes with the fallback compiler standard", "Inspect nob.c standard-flag selection and ensure -std=c2x is selected after -std=c23 fails.");

    PROVEN_TEST_ASSERT(file_contains(log_path, "reject c23"), "wrapper saw the initial -std=c23 probe", "If this is missing, the build driver may not be probing c23 first.");
    PROVEN_TEST_ASSERT(file_contains(log_path, "accept c2x"), "wrapper saw the fallback -std=c2x probe", "If this is missing, the build driver may not be falling back to c2x.");

    (void)remove(wrapper_path);
    (void)remove(log_path);

    PROVEN_TEST_PASS("Build driver standard-flag probe and fallback behavior passed.");
    return 0;
}
