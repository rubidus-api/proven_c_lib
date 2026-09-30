#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#define NOB_IMPLEMENTATION
#include "nob.h"
#include <string.h>
#include <stdarg.h>

#include <sys/stat.h>
#include <errno.h>

// Simple hash for command lines to detect flag changes
static uint32_t simple_hash(const char *str) {
    if (!str) return 0;
    uint32_t hash = 5381;
    int c;
    while ((c = *str++))
        hash = ((hash << 5) + hash) + (uint32_t)c;
    return hash;
}

static void hash_bytes(uint32_t *hash, const void *data, size_t size) {
    const unsigned char *bytes = data;
    for (size_t i = 0; i < size; ++i) {
        *hash = ((*hash << 5) + *hash) + (uint32_t)bytes[i];
    }
}

static bool hash_file_contents(uint32_t *hash, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        nob_log(NOB_ERROR,
                "[PROVEN][BUILD][FAIL] stage=dependency-content path=%s",
                path);
        return false;
    }

    hash_bytes(hash, path, strlen(path) + 1u);
    unsigned char buffer[8192];
    size_t read_count = 0;
    while ((read_count = fread(buffer, 1, sizeof(buffer), f)) != 0) {
        hash_bytes(hash, buffer, read_count);
    }
    bool ok = !ferror(f);
    if (fclose(f) != 0) ok = false;
    if (!ok) {
        nob_log(NOB_ERROR,
                "[PROVEN][BUILD][FAIL] stage=dependency-content path=%s",
                path);
    }
    return ok;
}

static bool hash_paths_contents(const char **paths, size_t count,
                                uint32_t *out_hash) {
    uint32_t hash = 5381;
    for (size_t i = 0; i < count; ++i) {
        if (!hash_file_contents(&hash, paths[i])) return false;
    }
    *out_hash = hash;
    return true;
}

static uint32_t hash_cmd(Nob_Cmd *cmd) {
    Nob_String_Builder sb = {0};
    for (size_t i = 0; i < cmd->count; ++i) {
        nob_sb_append_cstr(&sb, cmd->items[i]);
        nob_sb_append_cstr(&sb, "\n");
    }
    nob_sb_append_null(&sb);
    uint32_t hash = simple_hash(sb.items);
    nob_sb_free(sb);
    return hash;
}

static bool write_cmdhash(const char *path, uint32_t hash) {
    FILE *f = fopen(path, "w");
    if (!f) return false;
    fprintf(f, "%u\n", hash);
    fclose(f);
    return true;
}

static bool read_cmdhash(const char *path, uint32_t *out_hash) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char buf[64];
    if (fgets(buf, sizeof(buf), f)) {
        *out_hash = (uint32_t)strtoul(buf, NULL, 10);
        fclose(f);
        return true;
    }
    fclose(f);
    return false;
}

static bool output_file_valid(const char *path, bool executable) {
    struct stat st;
    if (stat(path, &st) != 0) return false;
    if (st.st_size == 0) return false;
    if (executable) {
#if !defined(_WIN32) && !defined(_WIN64)
        if ((st.st_mode & S_IXUSR) == 0) return false;
#endif
    }
    return true;
}

static bool checked_needs_rebuild(const char *output_path, const char **input_paths,
                                  size_t input_paths_count, bool *out_rebuild) {
    /*
     * nob_needs_rebuild returns "rebuild" immediately when the output is absent.
     * Validate every declared input first so a clean build cannot hide a missing
     * manifest dependency in a profile that never runs the hosted contract test.
     */
    for (size_t i = 0; i < input_paths_count; ++i) {
        struct stat input_stat;
        if (stat(input_paths[i], &input_stat) != 0) {
            nob_log(NOB_ERROR,
                    "[PROVEN][BUILD][FAIL] stage=dependency-stat path=%s",
                    input_paths[i]);
            nob_log(NOB_ERROR,
                    "[PROVEN][BUILD][FAIL_HINT] A required source or header could not be inspected. Restore the declared dependency or fix its permissions.");
            return false;
        }
    }

    int status = nob_needs_rebuild(output_path, input_paths, input_paths_count);
    if (status < 0) {
        nob_log(NOB_ERROR,
                "[PROVEN][BUILD][FAIL] stage=dependency-stat output=%s",
                output_path);
        nob_log(NOB_ERROR,
                "[PROVEN][BUILD][FAIL_HINT] A required source, header, or output path could not be inspected. Fix the missing path or filesystem error before rebuilding.");
        return false;
    }
    *out_rebuild = status > 0;
    return true;
}

static void append_mode_cflags(Nob_Cmd *cmd, const char *mode, const char *standard_flag, const char *sysroot) {
    nob_cmd_append(cmd, "-D_DEFAULT_SOURCE", "-D_POSIX_C_SOURCE=200809L");
    /* release: optimised, NDEBUG - the library's debug validation (pool double-free scan, map
     * overlap check) is compiled out; it made pool teardown quadratic (B-036).
     * hardened: optimised, NDEBUG, and PROVEN_HARDENED=1 - that validation kept in. */
    if (strcmp(mode, "release") == 0) nob_cmd_append(cmd, "-O3", "-DNDEBUG");
    else if (strcmp(mode, "hardened") == 0) nob_cmd_append(cmd, "-O2", "-DNDEBUG", "-DPROVEN_HARDENED=1");
    else if (strcmp(mode, "asan") == 0) nob_cmd_append(cmd, "-g", "-O0", "-fsanitize=address");
    else if (strcmp(mode, "ubsan") == 0) nob_cmd_append(cmd, "-g", "-O0", "-fsanitize=undefined");
    else if (strcmp(mode, "tsan") == 0) nob_cmd_append(cmd, "-g", "-O0", "-fsanitize=thread");
    else if (strcmp(mode, "strict-error") == 0) nob_cmd_append(cmd, "-g", "-O0", "-Wall", "-Wextra", "-Werror");
    else if (strcmp(mode, "freestanding") == 0) nob_cmd_append(cmd, "-g", "-O0", "-Wall", "-Wextra", "-Werror", "-DPROVEN_FREESTANDING", "-DPROVEN_FMT_NO_FLOAT", "-DPROVEN_NO_U16STR", "-ffreestanding");
    else nob_cmd_append(cmd, "-g", "-O0");
    if (standard_flag) nob_cmd_append(cmd, standard_flag);
    if (sysroot && *sysroot) nob_cmd_append(cmd, "--sysroot", sysroot);
    nob_cmd_append(cmd, "-I./include", "-I./platform", "-I./manual/examples");
}

/* The one place a library object's compile command is built: the cache hash and the compile use
 * it, so the hash is the exact command that runs. -MMD -MF writes the compiler's dependency file. */
static void build_compile_cmd(Nob_Cmd *cmd, const char *compiler, const char *src, const char *obj_tmp, const char *dep_tmp, const char *mode, const char *extra_cflags, const char *standard_flag, const char *sysroot) {
    nob_cmd_append(cmd, compiler);
    append_mode_cflags(cmd, mode, standard_flag, sysroot);
    if (extra_cflags) nob_cmd_append(cmd, extra_cflags);
    nob_cmd_append(cmd, "-MMD", "-MF", dep_tmp);
    nob_cmd_append(cmd, "-c", src, "-o", obj_tmp);
}

/*
 * Compiler dependency files (B-036). Each object and test executable is built with -MMD, so its
 * `.d` file names the source and every non-system header that translation unit actually
 * included. The cache key is the exact command plus the contents of those files: editing a
 * header rebuilds exactly the outputs that consumed it, and nothing else.
 */
static bool depfile_is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

static bool read_depfile(const char *path, Nob_File_Paths *out) {
    if (nob_file_exists(path) != 1) return false;
    Nob_String_Builder sb = {0};
    if (!nob_read_entire_file(path, &sb)) return false;
    const char *d = sb.items;
    size_t n = sb.count, i = 0;
    /* Skip the target: up to the first ':' followed by whitespace (a drive letter's ':' is not). */
    while (i < n && !(d[i] == ':' && (i + 1 == n || depfile_is_space(d[i + 1])))) ++i;
    if (i >= n) { nob_sb_free(sb); return false; }
    ++i;
    while (i < n) {
        if (depfile_is_space(d[i])) { ++i; continue; }
        if (d[i] == '\\' && i + 1 < n && (d[i + 1] == '\n' || d[i + 1] == '\r')) { ++i; continue; }
        size_t start = i;
        while (i < n && !depfile_is_space(d[i])) ++i;
        size_t len = i - start;
        if (d[start + len - 1] == ':') break;          /* a second rule (-MP phony targets) */
        nob_da_append(out, nob_temp_sprintf("%.*s", (int)len, d + start));
    }
    nob_sb_free(sb);
    return out->count > 0;
}

/* The state an output was built from: its exact command plus the contents of every file its
 * depfile names. False when that is unknown - no depfile yet, or a file it names is gone - and
 * the output must then be rebuilt. */
static bool depfile_state_hash(Nob_Cmd *cmd, const char *depfile, uint32_t *out_hash) {
    Nob_File_Paths deps = {0};
    bool ok = read_depfile(depfile, &deps);
    uint32_t h = ok ? hash_cmd(cmd) : 0;
    for (size_t i = 0; ok && i < deps.count; ++i) {
        struct stat st;
        ok = stat(deps.items[i], &st) == 0 && hash_file_contents(&h, deps.items[i]);
    }
    nob_da_free(deps);
    if (ok) *out_hash = h;
    return ok;
}

/* After a successful build: install the fresh depfile and record the hash taken from it (never
 * the pre-build one, which came from the previous depfile). No depfile means the next build
 * rebuilds this output again - correct, only slower. */
static void record_build_state(Nob_Cmd *cmd, const char *dep_tmp, const char *dep_path, const char *hash_path) {
    uint32_t h = 0;
    if (nob_file_exists(dep_tmp) == 1 && nob_rename(dep_tmp, dep_path) && depfile_state_hash(cmd, dep_path, &h)) {
        write_cmdhash(hash_path, h);
    } else {
        nob_log(NOB_WARNING, "[PROVEN][BUILD][DEPFILE][MISSING] path=%s - the compiler wrote no usable dependency file; this output will rebuild every time", dep_path);
        if (nob_file_exists(hash_path) == 1) nob_delete_file(hash_path);
    }
}

/* True when a test interposes libc functions with dlsym(RTLD_NEXT, ...) to inject faults an
 * ordinary run cannot produce (a readdir() that fails mid-directory, an opendir() that swaps a
 * directory for a symlink). Only those tests link -ldl. */
static bool test_source_needs_libdl(const char *src) {
    Nob_String_Builder sb = {0};
    if (!nob_read_entire_file(src, &sb)) return false;
    Nob_String_View sv = nob_sb_to_sv(sb);
    bool found = false;
    static const char needle[] = "RTLD_NEXT";
    size_t n = sizeof needle - 1;
    for (size_t i = 0; i + n <= sv.count && !found; ++i) found = memcmp(sv.data + i, needle, n) == 0;
    nob_sb_free(sb);
    return found;
}

/* The one place a test link command is built: the cache hash and the link both use it, so the
 * hash is the exact command that runs. */
static void build_test_link_cmd(Nob_Cmd *cmd, const char *compiler, const char *src, Nob_File_Paths obj_files, const char *exec_tmp, const char *dep_tmp, const char *mode, const char *extra_cflags, const char *extra_ldflags, const char *standard_flag, const char *sysroot, bool target_windows) {
    nob_cmd_append(cmd, compiler);
    append_mode_cflags(cmd, mode, standard_flag, sysroot);
    /* Mirror the user -cflags onto test compilation so config macros that affect
     * public headers stay consistent with the library objects. */
    if (extra_cflags) nob_cmd_append(cmd, extra_cflags);
    if (strcmp(mode, "asan") == 0) nob_cmd_append(cmd, "-fsanitize=address");
    else if (strcmp(mode, "ubsan") == 0) nob_cmd_append(cmd, "-fsanitize=undefined");
    else if (strcmp(mode, "tsan") == 0) nob_cmd_append(cmd, "-fsanitize=thread");

    if (strcmp(mode, "freestanding") != 0) {
        nob_cmd_append(cmd, "-pthread");
        /* A Windows test runs where no mingw-w64 runtime DLL is installed (the test VM). */
        if (target_windows) nob_cmd_append(cmd, "-static");
    } else {
        // Enforce static linking for freestanding tests to ensure no dynamic libc bleed
        nob_cmd_append(cmd, "-static");
    }

    nob_cmd_append(cmd, "-MMD", "-MF", dep_tmp);
    nob_cmd_append(cmd, src);
    for (size_t i = 0; i < obj_files.count; ++i) nob_cmd_append(cmd, obj_files.items[i]);
    if (extra_ldflags) nob_cmd_append(cmd, extra_ldflags);
    if (target_windows) {
        /* The Windows CSPRNG (BCryptGenRandom): mingw-w64 ignores the #pragma comment that
         * names the import library for MSVC, as in the cross smoke link. */
        if (strcmp(mode, "freestanding") != 0) nob_cmd_append(cmd, "-lbcrypt");
    } else if (strcmp(mode, "freestanding") != 0 && test_source_needs_libdl(src)) {
        nob_cmd_append(cmd, "-ldl");
    }
    nob_cmd_append(cmd, "-o", exec_tmp);
}

/* Whether the compiler builds for Windows, whatever the host: a mingw-w64 cross compiler on
 * Linux builds Windows test executables (.exe, static, -lbcrypt, never -ldl) for a Windows
 * machine to run (-no-run here). Asked of the compiler with -dumpmachine. */
static bool compiler_targets_windows(const char *compiler, const char *probe_dir) {
    const char *out = nob_temp_sprintf("%s/dumpmachine.txt", probe_dir);
    Nob_Cmd cmd = {0};
    nob_cmd_append(&cmd, compiler, "-dumpmachine");
    bool ran = nob_cmd_run(&cmd, .stdout_path = out);
    nob_cmd_free(cmd);
    bool windows = false;
    Nob_String_Builder sb = {0};
    if (ran && nob_read_entire_file(out, &sb)) {
        nob_sb_append_null(&sb);
        windows = strstr(sb.items, "mingw") || strstr(sb.items, "windows") || strstr(sb.items, "cygwin");
    } else {
#if defined(_WIN32) || defined(_WIN64)
        windows = true;                                 /* no answer: assume the host */
#endif
    }
    nob_sb_free(sb);
    return windows;
}

static void sanitize_name(char *dst, size_t cap, const char *src) {
    size_t n = 0;
    for (; *src && n + 1 < cap; ++src) {
        char c = *src;
        if ((c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '_' || c == '-' || c == '.') {
            dst[n++] = c;
        } else {
            dst[n++] = '_';
        }
    }
    dst[n] = 0;
}


static bool format_path(char *dst, size_t cap, const char *fmt, ...) {
    if (!dst || cap == 0 || !fmt) return false;
    va_list ap;
    va_start(ap, fmt);
    int written = vsnprintf(dst, cap, fmt, ap);
    va_end(ap);
    return written >= 0 && (size_t)written < cap;
}

static bool shell_token_is_safe(const char *s) {
    if (!s || !*s) return false;
    for (; *s; ++s) {
        char c = *s;
        if ((c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '_' || c == '-' || c == '.' || c == '/' || c == '+') {
            continue;
        }
        return false;
    }
    return true;
}

static bool mkdir_p_safe(const char *path) {
    if (!path || !*path) return false;
    if (!shell_token_is_safe(path)) {
        nob_log(NOB_ERROR, "Unsafe directory path: %s", path ? path : "(null)");
        return false;
    }
    char tmp[1024];
    size_t len = strlen(path);
    if (len >= sizeof(tmp)) {
        nob_log(NOB_ERROR, "Directory path is too long: %s", path);
        return false;
    }
    memcpy(tmp, path, len + 1u);
    for (char *p = tmp; *p; ++p) {
        if (*p == '/' || *p == '\\') {
            char saved = *p;
            *p = '\0';
            if (tmp[0] != '\0' && strcmp(tmp, ".") != 0) {
                if (!nob_mkdir_if_not_exists(tmp)) return false;
            }
            *p = saved;
            while (p[1] == '/' || p[1] == '\\') ++p;
        }
    }
    return nob_mkdir_if_not_exists(tmp);
}

/* A build root nob created carries this marker; `clean` removes only a root that has it (or the
 * default `build`), so a mistyped -build-root or PROVEN_BUILD_ROOT cannot delete a source tree. */
#define PROVEN_BUILD_ROOT_MARKER ".proven-build-root"

static bool build_root_path_is_safe(const char *root) {
    if (!shell_token_is_safe(root)) return false;
    const char *p = root;
    bool any_component = false;
    while (*p) {
        while (*p == '/') ++p;
        const char *start = p;
        while (*p && *p != '/') ++p;
        size_t n = (size_t)(p - start);
        if (n == 0) continue;
        if (n == 2 && start[0] == '.' && start[1] == '.') return false;
        if (!(n == 1 && start[0] == '.')) any_component = true;
    }
    return any_component;                               /* ".", "./" or "/" alone is never a build root */
}

/* Delete a tree without following links: a symlink (or a Windows junction) is removed, never
 * entered, so clean cannot reach outside the build root. */
static bool remove_tree_no_follow(const char *path) {
#if defined(_WIN32) || defined(_WIN64)
    DWORD attr = GetFileAttributesA(path);
    if (attr == INVALID_FILE_ATTRIBUTES) return false;
    bool is_dir = (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
    bool is_link = (attr & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    if (attr & FILE_ATTRIBUTE_READONLY) SetFileAttributesA(path, attr & ~(DWORD)FILE_ATTRIBUTE_READONLY);
    if (is_link) return is_dir ? RemoveDirectoryA(path) != 0 : DeleteFileA(path) != 0;
#else
    Nob_File_Type type = nob_get_file_type(path);
    if (type == (Nob_File_Type)-1) return false;
    bool is_dir = type == NOB_FILE_DIRECTORY;
#endif
    if (!is_dir) return nob_delete_file(path);
    Nob_File_Paths children = {0};
    if (!nob_read_entire_dir(path, &children)) return false;
    bool ok = true;
    for (size_t i = 0; i < children.count && ok; ++i) {
        const char *name = children.items[i];
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        Nob_String_Builder child = {0};
        nob_sb_appendf(&child, "%s/%s", path, name);
        nob_sb_append_null(&child);
        ok = remove_tree_no_follow(child.items);
        nob_sb_free(child);
    }
    nob_da_free(children);
    return ok && nob_delete_file(path);
}

static bool clean_build_root(const char *root) {
    if (!build_root_path_is_safe(root)) {
        nob_log(NOB_ERROR, "[PROVEN][CLEAN][REFUSE] build_root=%s reason=unsafe-path", root);
        nob_log(NOB_ERROR, "[PROVEN][CLEAN][FAIL_HINT] clean refuses '.', '/', any '..' component, and characters outside [A-Za-z0-9_./+-].");
        return false;
    }
    int root_exists = nob_file_exists(root);
    if (root_exists < 0) return false;
    if (root_exists == 0) {
        nob_log(NOB_INFO, "[PROVEN][CLEAN][NOTHING] build_root=%s", root);
        return true;
    }
    const char *marker = nob_temp_sprintf("%s/%s", root, PROVEN_BUILD_ROOT_MARKER);
    if (strcmp(root, "build") != 0 && nob_file_exists(marker) != 1) {
        nob_log(NOB_ERROR, "[PROVEN][CLEAN][REFUSE] build_root=%s reason=no-marker", root);
        nob_log(NOB_ERROR, "[PROVEN][CLEAN][FAIL_HINT] %s has no %s file, so nob did not create it; remove it by hand if it really is a build root.", root, PROVEN_BUILD_ROOT_MARKER);
        return false;
    }
    if (!remove_tree_no_follow(root)) {
        nob_log(NOB_ERROR, "[PROVEN][CLEAN][FAIL] build_root=%s", root);
        return false;
    }
    nob_log(NOB_INFO, "[PROVEN][CLEAN][PASS] build_root=%s", root);
    return true;
}

static bool mkdir_build_root(const char *root) {
    if (!mkdir_p_safe(root)) return false;
    const char *marker = nob_temp_sprintf("%s/%s", root, PROVEN_BUILD_ROOT_MARKER);
    if (nob_file_exists(marker)) return true;
    return nob_write_entire_file(marker, "proven_c_lib build root; ./nob clean may remove this directory\n", 63);
}

static bool command_available(const char *cmd_name) {
    if (!shell_token_is_safe(cmd_name)) return false;
#if defined(_WIN32) || defined(_WIN64)
    char cmd[512];
    if (!format_path(cmd, sizeof(cmd), "where %s >nul 2>nul", cmd_name)) return false;
#else
    char cmd[512];
    if (!format_path(cmd, sizeof(cmd), "command -v %s >/dev/null 2>&1", cmd_name)) return false;
#endif
    return system(cmd) == 0;
}

/*
 * scripts/project-check.sh runs the repository checks that are not C: the documentation
 * structure rules and the privacy scan that keeps this machine's paths and any key-like
 * pattern out of a public repository.
 *
 * It was not wired into the build, and the cost was not hypothetical. check-docs failed
 * continuously from 2026-07-12 to 2026-07-20 - across six releases, v26.07.13g through
 * v26.07.13m - because `./nob build` stayed green and nothing anybody ran reported it. It
 * turned out to be a false positive, which is the worse outcome: the one signal the check
 * ever produced was noise, and it produced it where nobody was listening.
 *
 * A check outside the build is a check nobody runs. This one costs about 0.1s, so it runs
 * in every mode and a failure fails the build.
 *
 * It is skipped, loudly, when it cannot run - no POSIX shell, no python3, or no script (a
 * source tarball). A skip says so; it never passes silently.
 */
static bool command_available(const char *cmd_name);

static int run_project_check(void) {
    const char *script = "scripts/project-check.sh";

    if (!nob_file_exists(script)) {
        nob_log(NOB_INFO, "[PROVEN][PROJECT_CHECK][SKIP] path=%s reason=not-present", script);
        return 0;
    }
#if defined(_WIN32) || defined(_WIN64)
    nob_log(NOB_WARNING, "[PROVEN][PROJECT_CHECK][SKIP] path=%s reason=needs-a-posix-shell", script);
    return 0;
#else
    if (!command_available("python3")) {
        nob_log(NOB_WARNING, "[PROVEN][PROJECT_CHECK][SKIP] path=%s reason=python3-not-found", script);
        return 0;
    }
    nob_log(NOB_INFO, "[PROVEN][PROJECT_CHECK][RUN] path=%s", script);
    if (system("sh scripts/project-check.sh") != 0) {
        nob_log(NOB_ERROR, "[PROVEN][PROJECT_CHECK][FAIL] path=%s", script);
        nob_log(NOB_ERROR, "[PROVEN][PROJECT_CHECK][FAIL_HINT] The failing check names the file above. "
                           "A privacy hit is either a real leak - delete it - or an example string "
                           "that collides with a host path, in which case change the example, not "
                           "the pattern. Weakening the scan to silence it is how the leak ships.");
        return 1;
    }
    nob_log(NOB_INFO, "[PROVEN][PROJECT_CHECK][PASS] path=%s", script);
    return 0;
#endif
}

static bool compiler_accepts_standard_flag(const char *compiler, const char *standard_flag, const char *sysroot, const char *probe_dir) {
    char src_path[768];
    char obj_path[768];
    if (!format_path(src_path, sizeof(src_path), "%s/std_probe.c", probe_dir)) return false;
    if (!format_path(obj_path, sizeof(obj_path), "%s/std_probe.o", probe_dir)) return false;

    FILE *f = fopen(src_path, "w");
    if (!f) return false;
    fputs("int proven_std_probe(void) { return 0; }\n", f);
    fclose(f);

    Nob_Cmd cmd = {0};
    nob_cmd_append(&cmd, compiler, standard_flag);
    if (sysroot && *sysroot) nob_cmd_append(&cmd, "--sysroot", sysroot);
    nob_cmd_append(&cmd, "-c", src_path, "-o", obj_path);
    bool ok = nob_cmd_run_sync(cmd);
    nob_cmd_free(cmd);
    return ok;
}

static const char *select_c_standard_flag(const char *compiler, const char *sysroot, const char *probe_dir) {
    static const char *const candidates[] = { "-std=c23", "-std=c2x" };
    if (!mkdir_p_safe(probe_dir)) return NULL;

    for (size_t i = 0; i < NOB_ARRAY_LEN(candidates); ++i) {
        const char *candidate = candidates[i];
        nob_log(NOB_INFO, "[PROVEN][BUILD][NOTE] probing compiler standard support with %s", candidate);
        if (compiler_accepts_standard_flag(compiler, candidate, sysroot, probe_dir)) {
            nob_log(NOB_INFO, "[PROVEN][BUILD][NOTE] selected compiler standard flag %s", candidate);
            return candidate;
        }
        if (i == 0) {
            nob_log(NOB_INFO, "[PROVEN][BUILD][NOTE] compiler rejected -std=c23; retrying with -std=c2x");
        }
    }

    nob_log(NOB_ERROR, "[PROVEN][BUILD][FAIL] compiler %s does not accept -std=c23 or -std=c2x", compiler);
    return NULL;
}

typedef struct {
    const char *name;
    const char *compiler;
    bool freestanding;
    const char *arch_flag_1;
    const char *arch_flag_2;
} Proven_Cross_Target;

typedef struct {
    const char *path;
    const char *title;
    const char *intent;
    const char *failure_hint;
} Proven_Test_Case;

#undef PROVEN_BUILD_TESTS_MANIFEST_INCLUDED
#define PROVEN_BUILD_TESTS_MANIFEST_CONSUMER 1
#include "build_tests.inc"
#ifndef PROVEN_BUILD_TESTS_MANIFEST_INCLUDED
#error "build_tests.inc must define PROVEN_BUILD_TESTS_MANIFEST_INCLUDED"
#endif
#undef PROVEN_BUILD_TESTS_MANIFEST_CONSUMER

_Static_assert(NOB_ARRAY_LEN(all_tests) > 100, "all_tests registry is unexpectedly small");
_Static_assert(NOB_ARRAY_LEN(regression_tests) > 0, "regression_tests registry must not be empty");
_Static_assert(NOB_ARRAY_LEN(freestanding_tests) > 0, "freestanding_tests registry must not be empty");
_Static_assert(NOB_ARRAY_LEN(benchmark_tests) > 0, "benchmark_tests registry must not be empty");
_Static_assert(NOB_ARRAY_LEN(cross_compile_tests) > 0, "cross_compile_tests registry must not be empty");
_Static_assert(NOB_ARRAY_LEN(cross_link_tests) > 0, "cross_link_tests registry must not be empty");

static void print_proven_test_begin(const Proven_Test_Case *test) {
    nob_log(NOB_INFO, "[PROVEN][TEST][BEGIN] path=%s title=%s", test->path, test->title);
    nob_log(NOB_INFO, "[PROVEN][TEST][INTENT] %s", test->intent);
    nob_log(NOB_INFO, "[PROVEN][TEST][FAIL_HINT] %s", test->failure_hint);
}

static const Proven_Test_Case *find_test_case(const Proven_Test_Case *tests,
                                              size_t count,
                                              const char *path) {
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(tests[i].path, path) == 0) return &tests[i];
    }
    return NULL;
}

static void print_proven_test_fail(const Proven_Test_Case *test, const char *stage, const char *detail) {
    nob_log(NOB_ERROR, "[PROVEN][TEST][FAIL] path=%s stage=%s", test->path, stage);
    nob_log(NOB_ERROR, "[PROVEN][TEST][FAIL_HINT] %s", detail ? detail : test->failure_hint);
}

static void print_proven_test_pass(const Proven_Test_Case *test) {
    nob_log(NOB_INFO, "[PROVEN][TEST][PASS] path=%s", test->path);
}

static const char *detect_runtime_profile(void) {
#if defined(_WIN32) || defined(_WIN64)
    const char *msystem = getenv("MSYSTEM");
    if (msystem && *msystem) return msystem;
    return "Windows";
#else
    const char *ostype = getenv("OSTYPE");
    if (ostype && *ostype) return ostype;
    return "POSIX";
#endif
}

static void print_proven_build_plan(const char *build_mode, const char *compiler_exe, const char *linker_exe,
                                    const char *archiver_exe, const char *sysroot,
                                    const char *build_root, const char *build_dir, size_t source_count,
                                    size_t test_count, bool cross_check, bool only_regression, bool benchmark_mode) {
    nob_log(NOB_INFO, "[PROVEN][BUILD][BEGIN] mode=%s cc=%s ld=%s build_root=%s build_dir=%s",
            build_mode, compiler_exe, linker_exe, build_root, build_dir);
    /* Say what the profile checks, so a log is never read as more (or less) than it was (B-036). */
    const char *safety =
        strcmp(build_mode, "release") == 0  ? "optimised; NDEBUG: library misuse validation (pool double free, map key overlap) compiled OUT" :
        strcmp(build_mode, "hardened") == 0 ? "optimised; PROVEN_HARDENED: library misuse validation compiled IN" :
        strcmp(build_mode, "asan") == 0     ? "debug validation IN; AddressSanitizer" :
        strcmp(build_mode, "ubsan") == 0    ? "debug validation IN; UndefinedBehaviorSanitizer" :
        strcmp(build_mode, "tsan") == 0     ? "debug validation IN; ThreadSanitizer" :
        strcmp(build_mode, "freestanding") == 0 ? "debug validation IN; freestanding profile" :
                                               "unoptimised; debug validation IN";
    nob_log(NOB_INFO, "[PROVEN][BUILD][PROFILE] mode=%s safety=%s", build_mode, safety);
    nob_log(NOB_INFO, "[PROVEN][BUILD][ENV] runtime=%s platform=%s", detect_runtime_profile(),
#if defined(_WIN32) || defined(_WIN64)
            "windows"
#else
            "posix"
#endif
    );
    nob_log(NOB_INFO, "[PROVEN][BUILD][ENV] ar=%s sysroot=%s",
            archiver_exe && *archiver_exe ? archiver_exe : "(unset)",
            sysroot && *sysroot ? sysroot : "(unset)");
    if (cross_check) {
        nob_log(NOB_INFO, "[PROVEN][BUILD][PLAN] cross matrix selected; no hosted test executables will be linked or run.");
    } else if (only_regression) {
        nob_log(NOB_INFO, "[PROVEN][BUILD][PLAN] regression-only run selected; only the regression subset will be linked and executed.");
    } else if (benchmark_mode) {
        nob_log(NOB_INFO, "[PROVEN][BUILD][PLAN] benchmark run selected; only the benchmark executable will be linked and executed.");
    } else {
        nob_log(NOB_INFO, "[PROVEN][BUILD][PLAN] sources=%zu tests=%zu; library objects will be compiled before test executables are linked and executed.",
                source_count, test_count);
    }
#if defined(_WIN32) || defined(_WIN64)
    const char *msystem = getenv("MSYSTEM");
    if (msystem && *msystem) {
        nob_log(NOB_WARNING, "[PROVEN][BUILD][NOTE] Windows/MSYS2 runtime detected (%s). If the build appears silent, confirm the compiler is on PATH, the build-root is writable, and the shell can launch child processes.", msystem);
    } else {
        nob_log(NOB_WARNING, "[PROVEN][BUILD][NOTE] Native Windows runtime detected. If the build appears to do nothing, first verify compiler availability and writable output paths.");
    }
#endif
}

static bool cross_source_is_freestanding(const char *src) {
    if (strcmp(src, "src/proven/u16str.c") == 0) return false;
    if (strcmp(src, "src/proven/fs.c") == 0) return false;
    if (strcmp(src, "src/proven/stream.c") == 0) return false;
    if (strcmp(src, "src/proven/sysio.c") == 0) return false;
    if (strcmp(src, "src/proven/mmap.c") == 0) return false;
    if (strcmp(src, "platform/proven_sys_random.c") == 0) return false;
    if (strcmp(src, "src/proven/job.c") == 0) return false;
    if (strcmp(src, "platform/proven_sys_fs.c") == 0) return false;
    if (strcmp(src, "platform/proven_sys_thread.c") == 0) return false;
    if (strcmp(src, "platform/proven_sys_io.c") == 0) return false;
    if (strcmp(src, "platform/proven_sys_env.c") == 0) return false;
    if (strcmp(src, "platform/proven_sys_time.c") == 0) return false;
    if (strcmp(src, "platform/proven_sys_mem.c") == 0) return false;
    return true;
}

static void append_cross_cflags(Nob_Cmd *cmd, const Proven_Cross_Target *target, const char *standard_flag, const char *sysroot) {
    if (target->freestanding) {
        nob_cmd_append(cmd,
                       standard_flag, "-ffreestanding", "-nostdlib",
                       "-DPROVEN_FREESTANDING", "-DPROVEN_FMT_NO_FLOAT", "-DPROVEN_NO_U16STR",
                       "-Wall", "-Wextra", "-Werror", "-I./include", "-I./platform");
    } else {
        nob_cmd_append(cmd,
                       standard_flag, "-D_DEFAULT_SOURCE", "-D_POSIX_C_SOURCE=200809L",
                       "-Wall", "-Wextra", "-Werror", "-I./include", "-I./platform");
    }
    if (sysroot && *sysroot) nob_cmd_append(cmd, "--sysroot", sysroot);
    if (target->arch_flag_1) nob_cmd_append(cmd, target->arch_flag_1);
    if (target->arch_flag_2) nob_cmd_append(cmd, target->arch_flag_2);
}

/* Targets that also link a smoke executable instead of compiling only.
 * This catches link-time symbol resolution that differs from ELF, notably
 * PE/COFF (Windows / mingw-w64) weak-symbol handling. */
static bool target_links_smoke(const Proven_Cross_Target *target) {
    return !target->freestanding && strncmp(target->name, "windows-", 8) == 0;
}

static bool cross_target_toolchain_usable(const char *build_root, const Proven_Cross_Target *target, const char *sysroot, const char **out_standard_flag) {
    char probe_dir[512];
    if (!format_path(probe_dir, sizeof(probe_dir), "%s/_probe", build_root)) return false;
    if (!mkdir_p_safe(probe_dir)) return false;

    const char *standard_flag = select_c_standard_flag(target->compiler, sysroot, probe_dir);
    if (!standard_flag) return false;
    if (out_standard_flag) *out_standard_flag = standard_flag;

    char src_path[768];
    char obj_path[768];
    if (!format_path(src_path, sizeof(src_path), "%s/%s.c", probe_dir, target->name)) return false;
    if (!format_path(obj_path, sizeof(obj_path), "%s/%s.o", probe_dir, target->name)) return false;

    FILE *f = fopen(src_path, "w");
    if (!f) {
        nob_log(NOB_ERROR, "Could not write cross probe: %s", src_path);
        return false;
    }
    fprintf(f, "#include <stddef.h>\n");
    fprintf(f, "#include <stdint.h>\n");
    if (strcmp(target->name, "linux-i686-multilib-hosted") == 0) {
        fprintf(f, "_Static_assert(sizeof(void *) == 4, \"gcc -m32 did not select a 32-bit ABI\");\n");
        fprintf(f, "_Static_assert(sizeof(size_t) == 4, \"gcc -m32 did not select 32-bit size_t\");\n");
    }
    fprintf(f, "int proven_cross_probe(void) { return (int)sizeof(uintptr_t); }\n");
    fclose(f);

    Nob_Cmd cmd = {0};
    nob_cmd_append(&cmd, target->compiler);
    append_cross_cflags(&cmd, target, standard_flag, sysroot);
    nob_cmd_append(&cmd, "-c", src_path, "-o", obj_path);
    bool ok = nob_cmd_run_sync(cmd);
    nob_cmd_free(cmd);

    if (!ok) {
        nob_log(NOB_WARNING, "Skipping %s: compiler exists but target flags/sysroot are not usable", target->name);
    }
    return ok;
}


// -------------------------------------------------------------
// Manual code-block check
// -------------------------------------------------------------
//
// Every ```c block in the manual must be code a compiler has actually seen.
// Blocks quoted from manual/examples/ are compiled and RUN as programs; the
// remaining inline blocks are fragments, and this check wraps each one in a
// function body and syntax-checks it.
//
// Anything that is not runnable code - a signature listing, a struct listing,
// pseudo-code, a deliberate counter-example - belongs in a ```text fence. That
// distinction is the whole point: before it existed, 186 of the manual's 190
// code blocks could not be compiled, so nothing could tell anyone when they
// stopped being true.
static bool check_manual_code_blocks(const char *compiler, const char *standard_flag, const char *build_dir)
{
    static const char *chapters[] = {
        "manual/manual.md",
        "manual/manual-00-start-here.md",
        "manual/manual-01-foundation.md",
        "manual/manual-02-allocation.md",
        "manual/manual-03-strings-text.md",
        "manual/manual-04-containers-algorithms.md",
        "manual/manual-05-hosted-services.md",
        "manual/manual-06-execution-and-platform.md",
        "manual/manual-07-alias-xcv-index.md",
        "manual/manual-08-fmt-scan.md",
        "manual/manual-freestanding.md",
    };

    bool all_ok = true;
    nob_log(NOB_INFO, "[PROVEN][BUILD][PHASE] manual code-block check start chapter_count=%zu", NOB_ARRAY_LEN(chapters));

    for (size_t c = 0; c < NOB_ARRAY_LEN(chapters); ++c) {
        Nob_String_Builder md = {0};
        if (!nob_read_entire_file(chapters[c], &md)) {
            nob_log(NOB_ERROR, "[PROVEN][DOCS][FAIL] path=%s stage=read", chapters[c]);
            nob_log(NOB_ERROR, "[PROVEN][DOCS][FAIL_HINT] The manual chapter list in nob.c names a file that does not exist.");
            all_ok = false;
            continue;
        }
        nob_sb_append_null(&md);

        char out_path[768];
        char base[128];
        const char *slash = strrchr(chapters[c], '/');
        snprintf(base, sizeof(base), "%s", slash ? slash + 1 : chapters[c]);
        char *dot = strrchr(base, '.');
        if (dot) *dot = '\0';
        if (!format_path(out_path, sizeof(out_path), "%s/manual/%s_blocks.c", build_dir, base)) {
            nob_sb_free(md);
            all_ok = false;
            continue;
        }

        FILE *out = fopen(out_path, "w");
        if (!out) {
            nob_log(NOB_ERROR, "[PROVEN][DOCS][FAIL] path=%s stage=open-scratch", out_path);
            nob_sb_free(md);
            all_ok = false;
            continue;
        }
        fputs("#include \"proven.h\"\n#include \"proven/alias_xcv.h\"\n", out);

        size_t blocks = 0;
        const char *p = md.items;
        while ((p = strstr(p, "```c\n")) != NULL) {
            const char *body = p + 5;
            const char *end = strstr(body, "\n```");
            if (!end) {
                /* An opening fence with no closing fence at column 0. This used to `break`,
                 * which skipped this block AND every block after it in the chapter - the gate
                 * switching itself off without saying so. It was live: an indented ```c inside
                 * a bullet list opens a block whose indented closer this search cannot see, so
                 * a pseudo-code sketch in chapter 2 was never compiled by anything, and nobody
                 * knew until moving it to chapter 6 put a real closing fence after it. Fail
                 * loudly instead: indent a fence and you get a build error, not silence. */
                nob_log(NOB_ERROR, "[PROVEN][DOCS][FAIL] path=%s stage=unterminated-block", chapters[c]);
                nob_log(NOB_ERROR, "[PROVEN][DOCS][FAIL_HINT] A ```c fence in this chapter is never closed at column 0. "
                                   "The usual cause is an indented fence inside a list item: the opener is found and the "
                                   "indented closer is not. Move the block out of the list, or fence it as ```text if it "
                                   "is a sketch. Everything after it in this chapter was going unchecked.");
                all_ok = false;
                break;
            }

            /* Blocks quoted from manual/examples/ are whole programs: they are
             * compiled and run elsewhere, so skip them here. The marker sits
             * immediately above the fence. */
            bool is_example = false;
            const char *look = p;
            size_t back = 0;
            while (look > md.items && back < 200) { --look; ++back; if (*look == '\n' && back > 1) break; }
            if (strncmp(look, "\n<!-- example:", 14) == 0 || strncmp(look, "<!-- example:", 13) == 0) is_example = true;

            if (!is_example) {
                fprintf(out, "static void blk_%zu(proven_allocator_t alloc, proven_allocator_t scratch) {\n(void)alloc; (void)scratch;\n", blocks);
                fwrite(body, 1, (size_t)(end - body), out);
                fputs("\n}\n", out);
                ++blocks;
            }
            p = end + 4;
        }
        fclose(out);
        nob_sb_free(md);

        Nob_Cmd cmd = {0};
        nob_cmd_append(&cmd, compiler);
        if (standard_flag) nob_cmd_append(&cmd, standard_flag);
        nob_cmd_append(&cmd, "-Wall", "-Wextra", "-D_DEFAULT_SOURCE", "-D_POSIX_C_SOURCE=200809L",
                       "-I./include", "-I./platform", "-fsyntax-only", out_path);
        bool ok = nob_cmd_run_sync(cmd);
        nob_cmd_free(cmd);

        if (ok) {
            nob_log(NOB_INFO, "[PROVEN][DOCS][OK] path=%s blocks=%zu", chapters[c], blocks);
        } else {
            nob_log(NOB_ERROR, "[PROVEN][DOCS][FAIL] path=%s stage=compile-blocks blocks=%zu", chapters[c], blocks);
            nob_log(NOB_ERROR, "[PROVEN][DOCS][FAIL_HINT] A ```c block in this chapter does not compile. Either fix the code, or - if it is a signature listing, a struct listing, pseudo-code, or a deliberate counter-example - fence it as ```text. The scratch translation unit is at %s; each blk_N corresponds to the Nth non-example ```c block in the chapter.", out_path);
            all_ok = false;
        }
    }

    return all_ok;
}

/* Build one cross target: its library objects, its smoke program, and whatever link that
 * target's evidence requires. False on the first failure, after logging it. */
static bool run_cross_target(const Proven_Cross_Target *target, const char *standard_flag,
                             const char *build_root, const char *sysroot,
                             const char **srcs, size_t srcs_count) {
    char target_dir[512];
    if (!format_path(target_dir, sizeof(target_dir), "%s/%s", build_root, target->name)) return false;
    if (!mkdir_p_safe(target_dir)) return false;

    nob_log(NOB_INFO, "[PROVEN][TEST][BEGIN] path=cross/%s title=cross compile target", target->name);
    nob_log(NOB_INFO, "[PROVEN][TEST][INTENT] Compile proven sources and the smoke translation unit for target %s using %s.", target->name, target->compiler);
    nob_log(NOB_INFO, "[PROVEN][TEST][FAIL_HINT] If this target fails, check the compiler/sysroot for %s first; otherwise inspect the source file named in the compiler diagnostic.", target->name);
    nob_log(NOB_INFO, "Cross compile target: %s (%s)", target->name, target->compiler);

    for (size_t i = 0; i < srcs_count; ++i) {
        if (target->freestanding && !cross_source_is_freestanding(srcs[i])) continue;

        char obj_name[256];
        sanitize_name(obj_name, sizeof obj_name, srcs[i]);

        char obj_path[768];
        if (!format_path(obj_path, sizeof(obj_path), "%s/%s.o", target_dir, obj_name)) return false;

        Nob_Cmd cmd = {0};
        nob_cmd_append(&cmd, target->compiler);
        append_cross_cflags(&cmd, target, standard_flag, sysroot);
        nob_cmd_append(&cmd, "-c", srcs[i], "-o", obj_path);
        bool ok = nob_cmd_run_sync(cmd);
        nob_cmd_free(cmd);
        if (!ok) {
            nob_log(NOB_ERROR, "[PROVEN][TEST][FAIL] path=cross/%s stage=compile-source source=%s", target->name, srcs[i]);
            nob_log(NOB_ERROR, "[PROVEN][TEST][FAIL_HINT] Check the diagnostic above for the exact source portability issue on this target.");
            return false;
        }
    }

    const Proven_Test_Case *smoke_tests = cross_compile_tests;
    size_t smoke_count = NOB_ARRAY_LEN(cross_compile_tests);
    if (target->freestanding) {
        smoke_tests = find_test_case(
            freestanding_tests, NOB_ARRAY_LEN(freestanding_tests),
            PROVEN_FREESTANDING_CROSS_SMOKE_PATH);
        if (!smoke_tests) {
            nob_log(NOB_ERROR,
                    "[PROVEN][TEST][FAIL] path=cross/%s stage=manifest",
                    target->name);
            nob_log(NOB_ERROR,
                    "[PROVEN][TEST][FAIL_HINT] The named freestanding cross smoke path is absent from freestanding_tests[].");
            return false;
        }
        smoke_count = 1;
    }
    const char *smoke_obj_paths[NOB_ARRAY_LEN(cross_compile_tests)];
    for (size_t i = 0; i < smoke_count; ++i) {
        const char *smoke_source = nob_temp_sprintf("%s.c", smoke_tests[i].path);
        const char *smoke_obj =
            nob_temp_sprintf("%s/smoke-%zu.o", target_dir, i);
        smoke_obj_paths[i] = smoke_obj;

        Nob_Cmd cmd = {0};
        nob_cmd_append(&cmd, target->compiler);
        append_cross_cflags(&cmd, target, standard_flag, sysroot);
        nob_cmd_append(&cmd, "-c", smoke_source, "-o", smoke_obj);
        bool ok = nob_cmd_run_sync(cmd);
        nob_cmd_free(cmd);
        if (!ok) {
            nob_log(NOB_ERROR,
                    "[PROVEN][TEST][FAIL] path=cross/%s stage=compile-smoke source=%s",
                    target->name, smoke_source);
            nob_log(NOB_ERROR,
                    "[PROVEN][TEST][FAIL_HINT] Check public header feature guards and target-specific compiler diagnostics above.");
            return false;
        }
    }

    if (target->freestanding) {
        /* B-034: the no-CRT link. Every freestanding object plus a program that supplies only
         * memcpy/memmove/memset/memcmp, linked static with no C library and no startup files.
         * A static link fails on any unresolved symbol, so success is the evidence that the
         * library needs nothing else from its environment but the compiler's support library. */
        const char *nocrt_src = nob_temp_sprintf("%s.c", freestanding_link_tests[0].path);
        char elf_path[768];
        if (!format_path(elf_path, sizeof(elf_path), "%s/nocrt-link.elf", target_dir)) return false;
        Nob_Cmd link = {0};
        nob_cmd_append(&link, target->compiler);
        append_cross_cflags(&link, target, standard_flag, sysroot);
        nob_cmd_append(&link, "-nostartfiles", "-static", "-Wl,-e,proven_nocrt_entry");
        for (size_t i = 0; i < srcs_count; ++i) {
            if (!cross_source_is_freestanding(srcs[i])) continue;
            char link_obj[256];
            sanitize_name(link_obj, sizeof link_obj, srcs[i]);
            nob_cmd_append(&link, nob_temp_sprintf("%s/%s.o", target_dir, link_obj));
        }
        nob_cmd_append(&link, nocrt_src, "-lgcc", "-o", elf_path);
        bool linked = nob_cmd_run_sync(link);
        nob_cmd_free(link);
        if (!linked) {
            nob_log(NOB_ERROR, "[PROVEN][TEST][FAIL] path=cross/%s stage=nocrt-link", target->name);
            nob_log(NOB_ERROR, "[PROVEN][TEST][FAIL_HINT] %s", freestanding_link_tests[0].failure_hint);
            return false;
        }
        nob_log(NOB_INFO, "[PROVEN][TEST][INFO] path=cross/%s stage=nocrt-link linked with only memcpy, memmove, memset, memcmp and -lgcc", target->name);
    }

    if (target_links_smoke(target)) {
        char exe_path[768];
        if (!format_path(exe_path, sizeof(exe_path), "%s/link-smoke", target_dir)) return false;
        Nob_Cmd link = {0};
        nob_cmd_append(&link, target->compiler);
        append_cross_cflags(&link, target, standard_flag, sysroot);
        for (size_t i = 0; i < srcs_count; ++i) {
            char link_obj[256];
            sanitize_name(link_obj, sizeof link_obj, srcs[i]);
            char link_obj_path[768];
            if (!format_path(link_obj_path, sizeof(link_obj_path), "%s/%s.o", target_dir, link_obj)) {
                nob_cmd_free(link);
                return false;
            }
            nob_cmd_append(&link, nob_temp_sprintf("%s", link_obj_path));
        }
        for (size_t i = 0; i < smoke_count; ++i) {
            nob_cmd_append(&link, smoke_obj_paths[i]);
        }
        for (size_t i = 0; i < NOB_ARRAY_LEN(cross_link_tests); ++i) {
            nob_cmd_append(&link,
                           nob_temp_sprintf("%s.c", cross_link_tests[i].path));
        }
        /* -lbcrypt: the Windows CSPRNG (BCryptGenRandom). MSVC pulls the import
         * library in from a #pragma comment in the source; mingw-w64 does not
         * honour that pragma, so the Windows link needs the library named here. */
        nob_cmd_append(&link, "-o", exe_path, "-lwinpthread", "-lbcrypt");
        bool linked = nob_cmd_run_sync(link);
        nob_cmd_free(link);
        if (!linked) {
            nob_log(NOB_ERROR, "[PROVEN][TEST][FAIL] path=cross/%s stage=link", target->name);
            nob_log(NOB_ERROR, "[PROVEN][TEST][FAIL_HINT] A symbol resolved on ELF but not on this target's object format (e.g. a PE/COFF weak symbol). Inspect the undefined reference above.");
            return false;
        }
        nob_log(NOB_INFO, "[PROVEN][TEST][INFO] path=cross/%s stage=link linked smoke executable", target->name);
    }

    return true;
}

static bool run_cross_compile_matrix(const char *build_root, const char *sysroot,
                                     const char **srcs, size_t srcs_count,
                                     const char **headers, size_t headers_count) {
    (void)headers;
    (void)headers_count;

    const Proven_Cross_Target targets[] = {
        { "native-gcc-hosted", "gcc", false, NULL, NULL },
        { "native-clang-hosted", "clang", false, NULL, NULL },
        { "linux-aarch64-hosted", "aarch64-linux-gnu-gcc", false, NULL, NULL },
        { "linux-armhf-hosted", "arm-linux-gnueabihf-gcc", false, NULL, NULL },
        { "linux-i686-hosted", "i686-linux-gnu-gcc", false, NULL, NULL },
        { "linux-i686-multilib-hosted", "gcc", false, "-m32", NULL },
        { "windows-x86_64-winapi", "x86_64-w64-mingw32-gcc", false, NULL, NULL },
        { "windows-i686-winapi", "i686-w64-mingw32-gcc", false, NULL, NULL },
        { "freestanding-arm-cortex-m4", "arm-none-eabi-gcc", true, "-mcpu=cortex-m4", "-mthumb" },
        { "freestanding-riscv64-elf", "riscv64-elf-gcc", true, NULL, NULL },
        { "freestanding-riscv64-unknown-elf", "riscv64-unknown-elf-gcc", true, NULL, NULL },
    };

    if (!mkdir_p_safe(build_root)) return false;

    /*
     * Every target ends as PASS, FAIL or SKIP, and all three are reported (B-035). A missing
     * toolchain is a SKIP - a verification gap, not a pass - and the four targets a release
     * depends on (the native host compilers and both Windows word sizes; owner decision
     * 2026-09-28) may not be skipped: a matrix that skipped one of them does not finish green.
     */
    static const char *const mandatory[] = {
        "native-gcc-hosted", "native-clang-hosted", "windows-x86_64-winapi", "windows-i686-winapi",
    };
    enum { CROSS_PASS, CROSS_FAIL, CROSS_SKIP } result[NOB_ARRAY_LEN(targets)];
    const char *why[NOB_ARRAY_LEN(targets)];
    size_t passed = 0, failed = 0, skipped = 0;
    bool mandatory_missing = false;

    for (size_t t = 0; t < NOB_ARRAY_LEN(targets); ++t) {
        const Proven_Cross_Target *target = &targets[t];
        const char *standard_flag = NULL;
        why[t] = "";
        if (!command_available(target->compiler)) {
            nob_log(NOB_WARNING, "Skipping %s: compiler not found: %s", target->name, target->compiler);
            result[t] = CROSS_SKIP; why[t] = "compiler not found"; ++skipped;
        } else if (!cross_target_toolchain_usable(build_root, target, sysroot, &standard_flag)) {
            result[t] = CROSS_SKIP; why[t] = "target flags or sysroot not usable"; ++skipped;
        } else if (run_cross_target(target, standard_flag, build_root, sysroot, srcs, srcs_count)) {
            nob_log(NOB_INFO, "[PROVEN][TEST][PASS] path=cross/%s", target->name);
            result[t] = CROSS_PASS; ++passed;
        } else {
            result[t] = CROSS_FAIL; ++failed;
        }
        if (result[t] == CROSS_SKIP) {
            for (size_t k = 0; k < NOB_ARRAY_LEN(mandatory); ++k) {
                if (strcmp(mandatory[k], target->name) == 0) { mandatory_missing = true; why[t] = nob_temp_sprintf("%s - MANDATORY", why[t]); }
            }
        }
    }

    for (size_t t = 0; t < NOB_ARRAY_LEN(targets); ++t) {
        const char *r = result[t] == CROSS_PASS ? "PASS" : result[t] == CROSS_FAIL ? "FAIL" : "SKIP";
        nob_log(result[t] == CROSS_PASS ? NOB_INFO : NOB_WARNING, "[PROVEN][CROSS][RESULT] target=%s result=%s%s%s",
                targets[t].name, r, *why[t] ? " reason=" : "", why[t]);
    }
    nob_log(NOB_INFO, "[PROVEN][CROSS][SUMMARY] passed=%zu failed=%zu skipped=%zu", passed, failed, skipped);
    if (mandatory_missing) {
        nob_log(NOB_ERROR, "[PROVEN][CROSS][FAIL] a mandatory target was skipped: install its toolchain, or the matrix is not evidence for it");
    }
    return failed == 0 && !mandatory_missing;
}

int main(int argc, char **argv)
{
    // Shift off the command name (nob)
    nob_shift_args(&argc, &argv);

    bool force_rebuild = false;
    bool no_run = false;
    const char *build_mode = "debug";
    const char *cc = NULL;
    const char *user_ld = NULL;
    const char *user_cflags = NULL;
    const char *user_ldflags = NULL;
    bool show_help = false;
    bool only_regression = false;
    bool benchmark_mode = false;
    bool cross_check = false;
    const char *build_root = NULL;
    bool clean_requested = false;

    if (argc == 0) {
        show_help = true;
    }

    // Parse commands and options
    while (argc > 0) {
        if (strcmp(argv[0], "release") == 0) {
            build_mode = "release";
            nob_shift_args(&argc, &argv);
        } else if (strcmp(argv[0], "hardened") == 0) {
            build_mode = "hardened";
            nob_shift_args(&argc, &argv);
        } else if (strcmp(argv[0], "debug") == 0 || strcmp(argv[0], "build") == 0) {
            build_mode = "debug";
            nob_shift_args(&argc, &argv);
        } else if (strcmp(argv[0], "freestanding") == 0) {
            build_mode = "freestanding";
            nob_shift_args(&argc, &argv);
        } else if (strcmp(argv[0], "cross") == 0) {
            cross_check = true;
            nob_shift_args(&argc, &argv);
        } else if (strcmp(argv[0], "regression") == 0) {
            build_mode = "debug";
            only_regression = true;
            nob_shift_args(&argc, &argv);
        } else if (strcmp(argv[0], "regression-asan") == 0) {
            build_mode = "asan";
            only_regression = true;
            nob_shift_args(&argc, &argv);
        } else if (strcmp(argv[0], "regression-ubsan") == 0) {
            build_mode = "ubsan";
            only_regression = true;
            nob_shift_args(&argc, &argv);
        } else if (strcmp(argv[0], "bench-float") == 0 || strcmp(argv[0], "benchmark-float") == 0) {
            build_mode = "release";
            benchmark_mode = true;
            nob_shift_args(&argc, &argv);
        } else if (strcmp(argv[0], "asan") == 0) {
            build_mode = "asan";
            nob_shift_args(&argc, &argv);
        } else if (strcmp(argv[0], "ubsan") == 0) {
            build_mode = "ubsan";
            nob_shift_args(&argc, &argv);
        } else if (strcmp(argv[0], "tsan") == 0) {
            build_mode = "tsan";
            nob_shift_args(&argc, &argv);
        } else if (strcmp(argv[0], "strict") == 0) {
            build_mode = "strict";
            nob_shift_args(&argc, &argv);
        } else if (strcmp(argv[0], "strict-error") == 0) {
            build_mode = "strict-error";
            nob_shift_args(&argc, &argv);
        } else if (strcmp(argv[0], "clean") == 0) {
            clean_requested = true;
            nob_shift_args(&argc, &argv);
        } else if (strcmp(argv[0], "-f") == 0) {
            force_rebuild = true;
            nob_shift_args(&argc, &argv);
        } else if (strcmp(argv[0], "-no-run") == 0) {
            no_run = true;
            nob_shift_args(&argc, &argv);
        } else if (strcmp(argv[0], "-cc") == 0) {
            nob_shift_args(&argc, &argv);
            if (argc > 0) {
                cc = argv[0];
                nob_shift_args(&argc, &argv);
            } else {
                nob_log(NOB_ERROR, "-cc requires a compiler argument");
                return 1;
            }
        } else if (strcmp(argv[0], "-ld") == 0) {
            nob_shift_args(&argc, &argv);
            if (argc > 0) {
                user_ld = argv[0];
                nob_shift_args(&argc, &argv);
            } else {
                nob_log(NOB_ERROR, "-ld requires a linker argument");
                return 1;
            }
        } else if (strcmp(argv[0], "-cflags") == 0) {
            nob_shift_args(&argc, &argv);
            if (argc > 0) {
                user_cflags = argv[0];
                nob_shift_args(&argc, &argv);
            } else {
                nob_log(NOB_ERROR, "-cflags requires an argument");
                return 1;
            }
        } else if (strcmp(argv[0], "-ldflags") == 0) {
            nob_shift_args(&argc, &argv);
            if (argc > 0) {
                user_ldflags = argv[0];
                nob_shift_args(&argc, &argv);
            } else {
                nob_log(NOB_ERROR, "-ldflags requires an argument");
                return 1;
            }
        } else if (strcmp(argv[0], "-build-root") == 0) {
            nob_shift_args(&argc, &argv);
            if (argc > 0) {
                build_root = argv[0];
                nob_shift_args(&argc, &argv);
            } else {
                nob_log(NOB_ERROR, "-build-root requires a directory argument");
                return 1;
            }
        } else if (strcmp(argv[0], "help") == 0 || strcmp(argv[0], "-h") == 0 || strcmp(argv[0], "--help") == 0) {
            show_help = true;
            nob_shift_args(&argc, &argv);
        } else {
            nob_log(NOB_ERROR, "Unknown argument: %s", argv[0]);
            return 1;
        }
    }

    if (show_help) {
        printf("Proven Build System (nob.c)\n");
        printf("Usage: ./nob [command] [options]\n\n");
        printf("Commands:\n");
        printf("  build, debug       Default build mode (-g -O0). [Default]\n");
        printf("  release            Optimized build (-O3 -DNDEBUG): library misuse validation compiled out.\n");
        printf("  hardened           Optimized build (-O2 -DNDEBUG -DPROVEN_HARDENED=1): validation kept in.\n");
        printf("  freestanding       Bare-metal MCU build without libc/OS.\n");
        printf("  cross              Compile-only matrix for hosted, WinAPI, ARM, and freestanding targets.\n");
        printf("  asan               Build and run tests with AddressSanitizer.\n");
        printf("  ubsan              Build and run tests with UndefinedBehaviorSanitizer.\n");
        printf("  tsan               Build and run tests with ThreadSanitizer.\n");
        printf("  strict             Build with extra warnings (-Wall -Wextra).\n");
        printf("  strict-error       Build with warnings as errors (-Werror).\n");
        printf("  regression         Run only the regression test suite (debug mode).\n");
        printf("  regression-asan    Run regression tests with AddressSanitizer.\n");
        printf("  regression-ubsan   Run regression tests with UndefinedBehaviorSanitizer.\n");
        printf("  bench-float        Run the benchmarks: float parse paths plus primitive throughput.\n");
        printf("  clean              Remove the build root (-build-root or build); refuses unsafe or unmarked roots.\n");
        printf("  help, -h, --help   Show this help message.\n\n");
        printf("Options:\n");
        printf("  -f                 Force a full rebuild of the project.\n");
        printf("  -no-run            Build every test executable but run none (e.g. a mingw-w64 build for Windows).\n");
        printf("  -cc <compiler>     Specify the C compiler to use (e.g., clang, gcc).\n");
        printf("  -ld <linker>       Specify the linker to use (defaults to compiler).\n");
        printf("  -cflags <flags>    Additional compiler flags (e.g., -DDEBUG).\n");
        printf("  -ldflags <flags>   Additional linker flags (e.g., -lm).\n");
        printf("  -build-root <dir>  Build output root, defaults to build or PROVEN_BUILD_ROOT.\n\n");
        printf("Environment Variables:\n");
        printf("  CC, NOB_COMPILER   Alternative ways to specify the compiler.\n");
        printf("  LD                 Alternative way to specify the linker.\n");
        printf("  AR                 Optional archiver path recorded in the build plan.\n");
        printf("  SYSROOT            Optional compiler sysroot passed through to build probes.\n\n");
        printf("Examples:\n");
        printf("  ./nob build        Build and run all tests in debug mode.\n");
        printf("  ./nob release      Build and run all tests with optimizations.\n");
        printf("  ./nob -cc clang    Build using the Clang compiler.\n");
        printf("  ./nob -cflags \"-DDEBUG\"  Build with custom debug definition.\n");
        printf("  ./nob asan -f      Force rebuild and check for memory leaks with ASan.\n");
        printf("  ./nob bench-float  Build and run the float and primitive benchmarks.\n");
        printf("  ./nob cross -build-root build-out/proven_c_lib  Compile target matrix.\n");
        printf("  ./nob clean        Clean up all build artifacts.\n");
        return 0;
    }

    if (build_root == NULL) build_root = getenv("PROVEN_BUILD_ROOT");
    if (build_root == NULL) build_root = "build";
    if (clean_requested) return clean_build_root(build_root) ? 0 : 1;

    if (cc == NULL) cc = getenv("CC");
    if (cc == NULL) cc = getenv("NOB_COMPILER");
    if (cc == NULL) cc = "gcc";

    const char *compiler_exe = cc;

    if (user_ld == NULL) user_ld = getenv("LD");
    if (user_ld == NULL) user_ld = compiler_exe;
    const char *linker_exe = user_ld;

    const char *archiver_exe = getenv("AR");
    if (archiver_exe && *archiver_exe == '\0') archiver_exe = NULL;

    const char *sysroot = getenv("SYSROOT");
    if (sysroot && *sysroot == '\0') sysroot = NULL;

    if (!command_available(compiler_exe)) {
        nob_log(NOB_ERROR, "[PROVEN][BUILD][FAIL] compiler not found on PATH: %s", compiler_exe);
        return 1;
    }
    if (!command_available(linker_exe)) {
        nob_log(NOB_ERROR, "[PROVEN][BUILD][FAIL] linker not found on PATH: %s", linker_exe);
        return 1;
    }

    char std_probe_root[768];
    if (!format_path(std_probe_root, sizeof(std_probe_root), "%s/_std_probe", build_root)) return 1;
    const char *standard_flag = select_c_standard_flag(compiler_exe, sysroot, std_probe_root);
    if (!standard_flag) return 1;

    nob_log(NOB_INFO, "[PROVEN][BUILD][ENV] standard_flag=%s", standard_flag);

    // Calculate flag hash for directory isolation
    Nob_String_Builder hash_src = {0};
    nob_sb_append_cstr(&hash_src, compiler_exe);
    nob_sb_append_cstr(&hash_src, linker_exe);
    if (archiver_exe) nob_sb_append_cstr(&hash_src, archiver_exe);
    nob_sb_append_cstr(&hash_src, build_mode);
    nob_sb_append_cstr(&hash_src, standard_flag);
    nob_sb_append_cstr(&hash_src, "-I./include-D_DEFAULT_SOURCE-D_POSIX_C_SOURCE=200809L");
    if (sysroot) nob_sb_append_cstr(&hash_src, sysroot);
    if (user_cflags) nob_sb_append_cstr(&hash_src, user_cflags);
    if (user_ldflags) nob_sb_append_cstr(&hash_src, user_ldflags);
    
    // Include important flags in hash to detect configuration shifts
    if (strcmp(build_mode, "release") == 0) nob_sb_append_cstr(&hash_src, "-O3-DNDEBUG");
    if (strcmp(build_mode, "hardened") == 0) nob_sb_append_cstr(&hash_src, "-O2-DNDEBUG-DPROVEN_HARDENED=1");
    if (strcmp(build_mode, "strict-error") == 0) nob_sb_append_cstr(&hash_src, "-Wall-Wextra-Werror-g-O0");
    if (strcmp(build_mode, "asan") == 0) nob_sb_append_cstr(&hash_src, "-fsanitize=address-g-O0");
    if (strcmp(build_mode, "ubsan") == 0) nob_sb_append_cstr(&hash_src, "-fsanitize=undefined-g-O0");
    if (strcmp(build_mode, "tsan") == 0) nob_sb_append_cstr(&hash_src, "-fsanitize=thread-g-O0");
    if (strcmp(build_mode, "strict") == 0) nob_sb_append_cstr(&hash_src, "-Wall-Wextra-g-O0");
    if (strcmp(build_mode, "freestanding") == 0) nob_sb_append_cstr(&hash_src, "-DPROVEN_FREESTANDING-DPROVEN_FMT_NO_FLOAT-DPROVEN_NO_U16STR-ffreestanding-g-O0");
    
    nob_sb_append_null(&hash_src);
    uint32_t flag_hash = simple_hash(hash_src.items);
    nob_sb_free(hash_src);

    char compiler_name[64];
    sanitize_name(compiler_name, sizeof compiler_name, compiler_exe);

    char build_dir[512];
    if (!format_path(build_dir, sizeof(build_dir), "%s/%s-%s-%08x", build_root, compiler_name, build_mode, flag_hash)) return 1;
    
    if (!mkdir_build_root(build_root)) return 1;
    if (!mkdir_p_safe(build_dir)) return 1;

    // Subdirs
    static const char *subdirs[] = {"src", "src/proven", "platform", "tests", "manual", "manual/examples",
                                    "manual/examples/en", "manual/examples/ko"};
    for (size_t i = 0; i < NOB_ARRAY_LEN(subdirs); ++i) {
        char buf[256];
        if (!format_path(buf, sizeof(buf), "%s/%s", build_dir, subdirs[i])) return 1;
        if (!mkdir_p_safe(buf)) return 1;
    }

    const char *srcs[] = {
        "src/proven/stream.c", "src/proven/memory.c", "src/proven/arena.c", "src/proven/pool.c", "src/proven/buffer.c",
        "src/proven/heap.c", "src/proven/alloc_check.c", "src/proven/u8str.c", "src/proven/u16str.c", "src/proven/array.c",
        "src/proven/ring.c", "src/proven/map.c", "src/proven/algorithm.c", "src/proven/hash.c", "src/proven/encode.c", "src/proven/utf.c", "src/proven/random.c", "src/proven/fs.c",
        "src/proven/time.c", "src/proven/fmt.c", "src/proven/mmap.c", "src/proven/sysio.c",
        "src/proven/job.c", "src/proven/scan.c", "src/proven/float_decimal.c", "src/proven/float_parse.c", "src/proven/float_format.c", "src/proven/panic.c", "platform/proven_sys_mem.c",
        "platform/proven_sys_fs.c", "platform/proven_sys_time.c", "platform/proven_sys_env.c", "platform/proven_sys_random.c",
        "platform/proven_sys_thread.c", "platform/proven_sys_io.c", "platform/proven_sys_math.c"
    };

    const char *headers[] = {
#undef PROVEN_BUILD_HEADERS_MANIFEST_INCLUDED
#define PROVEN_BUILD_HEADER(path) path,
#include "build_headers.inc"
#ifndef PROVEN_BUILD_HEADERS_MANIFEST_INCLUDED
#error "build_headers.inc must define PROVEN_BUILD_HEADERS_MANIFEST_INCLUDED"
#endif
#undef PROVEN_BUILD_HEADER
    };
    _Static_assert(NOB_ARRAY_LEN(headers) > 1, "build header manifest is unexpectedly small");

    /*
     * Validate every declared header before any mode runs, so a missing manifest entry cannot
     * pass as a clean build. Rebuild decisions do not use the manifest: each output's own
     * compiler dependency file names the headers it consumed (B-036).
     */
    uint32_t headers_content_hash = 0;
    if (!hash_paths_contents(headers, NOB_ARRAY_LEN(headers),
                             &headers_content_hash)) {
        return 1;
    }
    (void)headers_content_hash;

    print_proven_build_plan(build_mode, compiler_exe, linker_exe, archiver_exe, sysroot, build_root, build_dir,
                            NOB_ARRAY_LEN(srcs), NOB_ARRAY_LEN(all_tests), cross_check, only_regression, benchmark_mode);

    /* Before anything is compiled: it costs 0.1s, and failing here means the real reason is the
     * first thing printed. Run it after the tests instead and a documentation failure surfaces
     * through tests/test_portability_nob_std_probe, which runs this driver as a subprocess and
     * reports "build driver completes with the fallback compiler standard" - true, useless, and
     * three steps from the cause. Skipped for regression-only and cross runs: the former is what
     * that probe invokes, so the check does not run twice per build; the latter links no hosted
     * tests and is not where repository hygiene is decided. */
    if (!only_regression && !cross_check) {
        if (run_project_check() != 0) return 1;
    }

    if (cross_check) {
        const char *cross_root = nob_temp_sprintf("%s/cross", build_root);
        return run_cross_compile_matrix(cross_root, sysroot, srcs, NOB_ARRAY_LEN(srcs), headers, NOB_ARRAY_LEN(headers)) ? 0 : 1;
    }

    const Proven_Test_Case *tests = only_regression ? regression_tests : all_tests;
    size_t tests_count = only_regression ? NOB_ARRAY_LEN(regression_tests) : NOB_ARRAY_LEN(all_tests);

    if (strcmp(build_mode, "freestanding") == 0) {
        tests = freestanding_tests;
        tests_count = NOB_ARRAY_LEN(freestanding_tests);
    } else if (benchmark_mode) {
        tests = benchmark_tests;
        tests_count = NOB_ARRAY_LEN(benchmark_tests);
    }

    const char *obj_ext = ".o";
    bool target_windows = compiler_targets_windows(compiler_exe, build_dir);
    const char *exe_ext = target_windows ? ".exe" : "";
    if (target_windows) nob_log(NOB_INFO, "[PROVEN][BUILD][TARGET] windows=1 exe_ext=.exe link=-static,-lbcrypt");
    
    Nob_Cmd cmd = {0};
    Nob_File_Paths obj_files = {0};

    // Library Compilation
    size_t library_rebuilt = 0;
    size_t library_cached = 0;
    nob_log(NOB_INFO, "[PROVEN][BUILD][PHASE] library compilation start source_count=%zu", NOB_ARRAY_LEN(srcs));
    for (size_t i = 0; i < NOB_ARRAY_LEN(srcs); ++i) {
        if (strcmp(build_mode, "freestanding") == 0) {
            if (strcmp(srcs[i], "src/proven/u16str.c") == 0) continue;
            if (strcmp(srcs[i], "src/proven/fs.c") == 0) continue;
            if (strcmp(srcs[i], "src/proven/stream.c") == 0) continue;   /* streams sit on fs */
            if (strcmp(srcs[i], "src/proven/sysio.c") == 0) continue;
            if (strcmp(srcs[i], "src/proven/mmap.c") == 0) continue;
            if (strcmp(srcs[i], "src/proven/job.c") == 0) continue;
            if (strcmp(srcs[i], "platform/proven_sys_fs.c") == 0) continue;
            if (strcmp(srcs[i], "platform/proven_sys_thread.c") == 0) continue;
            if (strcmp(srcs[i], "platform/proven_sys_io.c") == 0) continue;
            if (strcmp(srcs[i], "platform/proven_sys_env.c") == 0) continue;
            if (strcmp(srcs[i], "platform/proven_sys_time.c") == 0) continue;
            if (strcmp(srcs[i], "platform/proven_sys_random.c") == 0) continue;  /* no OS CSPRNG on bare metal */
            if (strcmp(srcs[i], "platform/proven_sys_mem.c") == 0) continue;
        }

        Nob_String_Builder op = {0};
        nob_sb_append_cstr(&op, build_dir);
        nob_sb_append_cstr(&op, "/");
        nob_sb_append_cstr(&op, srcs[i]);
        op.count -= 2; // drop .c
        nob_sb_append_cstr(&op, obj_ext);
        nob_sb_append_null(&op);
        const char *final_obj_path = op.items;

        nob_da_append(&obj_files, final_obj_path);

        const char *obj_tmp = nob_temp_sprintf("%s.tmp", final_obj_path);
        const char *hash_path = nob_temp_sprintf("%s.cmdhash", final_obj_path);
        const char *dep_path = nob_temp_sprintf("%s.d", final_obj_path);
        const char *dep_tmp = nob_temp_sprintf("%s.d.tmp", final_obj_path);

        Nob_Cmd compile = {0};
        build_compile_cmd(&compile, compiler_exe, srcs[i], obj_tmp, dep_tmp, build_mode, user_cflags, standard_flag, sysroot);
        uint32_t current_hash = 0, old_hash = 0;
        bool state_known = depfile_state_hash(&compile, dep_path, &current_hash);
        bool hash_differs = !state_known || !read_cmdhash(hash_path, &old_hash) || old_hash != current_hash;
        bool should_rebuild = hash_differs || !output_file_valid(final_obj_path, false) || force_rebuild;
        if (should_rebuild) {
            nob_log(NOB_INFO, "[PROVEN][BUILD][SOURCE][REBUILD] path=%s", srcs[i]);
            if (!nob_cmd_run_sync(compile)) {
                nob_log(NOB_ERROR, "[PROVEN][BUILD][FAIL] stage=compile-source path=%s", srcs[i]);
                nob_log(NOB_ERROR, "[PROVEN][BUILD][FAIL_HINT] Read the compiler diagnostic above and inspect the portability contract for this source file.");
                if (nob_file_exists(obj_tmp)) nob_delete_file(obj_tmp);
                if (nob_file_exists(dep_tmp)) nob_delete_file(dep_tmp);
                return 1;
            }
            if (!nob_rename(obj_tmp, final_obj_path)) {
                nob_log(NOB_ERROR, "[PROVEN][BUILD][FAIL] stage=install-object path=%s", final_obj_path);
                nob_log(NOB_ERROR, "[PROVEN][BUILD][FAIL_HINT] Check the build-root permissions and any stale locked object file at the destination.");
                if (nob_file_exists(obj_tmp)) nob_delete_file(obj_tmp);
                return 1;
            }
            record_build_state(&compile, dep_tmp, dep_path, hash_path);
            library_rebuilt += 1;
        } else {
            nob_log(NOB_INFO, "[PROVEN][BUILD][SOURCE][CACHED] path=%s", srcs[i]);
            library_cached += 1;
        }
        nob_cmd_free(compile);
    }

    // Manual code-block check (hosted builds only: the fragments use the hosted API)
    if (strcmp(build_mode, "freestanding") != 0 && !benchmark_mode) {
        if (!check_manual_code_blocks(compiler_exe, standard_flag, build_dir)) {
            nob_log(NOB_ERROR, "[PROVEN][BUILD][FAIL] stage=manual-code-blocks");
            return 1;
        }
    }

    // Tests Compilation & Execution
    size_t tests_rebuilt = 0;
    size_t tests_cached = 0;
    nob_log(NOB_INFO, "[PROVEN][BUILD][PHASE] test link-and-run start test_count=%zu", tests_count);
    for (size_t i = 0; i < tests_count; ++i) {
        const Proven_Test_Case *test = &tests[i];
        const char *src_path = nob_temp_sprintf("%s.c", test->path);
        const char *exec_path = nob_temp_sprintf("%s/%s%s", build_dir, test->path, exe_ext);

        print_proven_test_begin(test);

        const char *hash_path = nob_temp_sprintf("%s.cmdhash", exec_path);
        const char *exec_tmp = nob_temp_sprintf("%s.tmp", exec_path);
        const char *dep_path = nob_temp_sprintf("%s.d", exec_path);
        const char *dep_tmp = nob_temp_sprintf("%s.d.tmp", exec_path);

        Nob_Cmd link = {0};
        build_test_link_cmd(&link, linker_exe, src_path, obj_files, exec_tmp, dep_tmp, build_mode, user_cflags, user_ldflags, standard_flag, sysroot, target_windows);
        uint32_t current_hash = 0, old_hash = 0;
        bool state_known = depfile_state_hash(&link, dep_path, &current_hash);
        bool hash_differs = !state_known || !read_cmdhash(hash_path, &old_hash) || old_hash != current_hash;
        /* The library objects are inputs too; their contents are not in the hash, their times are. */
        bool needs_link = false;
        if (!checked_needs_rebuild(exec_path, obj_files.items, obj_files.count, &needs_link)) {
            return 1;
        }
        needs_link = needs_link || !output_file_valid(exec_path, true);
        bool should_link = needs_link || hash_differs || force_rebuild;

        if (should_link) {
            tests_rebuilt += 1;
            nob_log(NOB_INFO, "[PROVEN][BUILD][TEST][REBUILD] path=%s", test->path);
            if (!nob_cmd_run_sync(link)) {
                print_proven_test_fail(test, "link", "The test executable did not link. Read the compiler diagnostics above, then check source/header/API drift.");
                if (nob_file_exists(exec_tmp)) nob_delete_file(exec_tmp);
                if (nob_file_exists(dep_tmp)) nob_delete_file(dep_tmp);
                return 1;
            }
            if (!nob_rename(exec_tmp, exec_path)) {
                print_proven_test_fail(test, "install", "The linked executable could not be moved into place. Check build-root permissions and stale locked files.");
                if (nob_file_exists(exec_tmp)) nob_delete_file(exec_tmp);
                return 1;
            }
            record_build_state(&link, dep_tmp, dep_path, hash_path);
        } else {
            tests_cached += 1;
            nob_log(NOB_INFO, "[PROVEN][BUILD][TEST][CACHED] path=%s", test->path);
        }
        nob_cmd_free(link);

        if (no_run) {
            nob_log(NOB_INFO, "[PROVEN][BUILD][TEST][BUILT] path=%s exe=%s", test->path, exec_path);
            nob_temp_reset();
            continue;
        }
        cmd.count = 0;
        nob_cmd_append(&cmd, exec_path);
        nob_log(NOB_INFO, "[PROVEN][BUILD][TEST][RUN] path=%s", test->path);
        if (!nob_cmd_run_sync(cmd)) {
            print_proven_test_fail(test, "run", test->failure_hint);
            return 1;
        }
        print_proven_test_pass(test);
        nob_temp_reset();
    }

    nob_cmd_free(cmd);
    nob_log(NOB_INFO, "[PROVEN][BUILD][SUMMARY] mode=%s rebuilt_sources=%zu cached_sources=%zu rebuilt_tests=%zu cached_tests=%zu",
            build_mode, library_rebuilt, library_cached, tests_rebuilt, tests_cached);
    nob_log(NOB_INFO, "[PROVEN][BUILD][PASS] mode=%s build_dir=%s", build_mode, build_dir);
    return 0;
}
