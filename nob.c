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

static void append_mode_cflags(Nob_Cmd *cmd, const char *mode, const char *standard_flag, const char *sysroot) {
    nob_cmd_append(cmd, "-D_DEFAULT_SOURCE", "-D_POSIX_C_SOURCE=200809L");
    if (strcmp(mode, "release") == 0) nob_cmd_append(cmd, "-O3");
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

static bool compile_object_tmp(const char *compiler, const char *src, const char *obj_tmp, const char *mode, const char *extra_cflags, const char *standard_flag, const char *sysroot, uint32_t *out_hash) {
    Nob_Cmd cmd = {0};
    nob_cmd_append(&cmd, compiler);
    append_mode_cflags(&cmd, mode, standard_flag, sysroot);
    if (extra_cflags) nob_cmd_append(&cmd, extra_cflags);
    nob_cmd_append(&cmd, "-c", src, "-o", obj_tmp);
    
    if (out_hash) *out_hash = hash_cmd(&cmd);
    bool success = nob_cmd_run_sync(cmd);
    nob_cmd_free(cmd);
    return success;
}

static bool link_executable_tmp(const char *compiler, const char *src, Nob_File_Paths obj_files, const char *exec_tmp, const char *mode, const char *extra_cflags, const char *extra_ldflags, const char *standard_flag, const char *sysroot, uint32_t *out_hash) {
    Nob_Cmd cmd = {0};
    nob_cmd_append(&cmd, compiler);
    append_mode_cflags(&cmd, mode, standard_flag, sysroot);
    /* Mirror the user -cflags onto test compilation so config macros that affect
     * public headers stay consistent with the library objects. */
    if (extra_cflags) nob_cmd_append(&cmd, extra_cflags);
    if (strcmp(mode, "asan") == 0) nob_cmd_append(&cmd, "-fsanitize=address");
    else if (strcmp(mode, "ubsan") == 0) nob_cmd_append(&cmd, "-fsanitize=undefined");
    else if (strcmp(mode, "tsan") == 0) nob_cmd_append(&cmd, "-fsanitize=thread");
    
    if (strcmp(mode, "freestanding") != 0) {
        nob_cmd_append(&cmd, "-pthread");
    } else {
        // Enforce static linking for freestanding tests to ensure no dynamic libc bleed
        nob_cmd_append(&cmd, "-static");
    }
    
    nob_cmd_append(&cmd, src);
    for (size_t i = 0; i < obj_files.count; ++i) nob_cmd_append(&cmd, obj_files.items[i]);
    if (extra_ldflags) nob_cmd_append(&cmd, extra_ldflags);
#if !defined(_WIN32) && !defined(_WIN64)
    /* -ldl for the tests that interpose libc functions with dlsym(RTLD_NEXT, ...) to
     * inject faults an ordinary run cannot produce - a readdir() that fails mid-directory,
     * an opendir() that swaps a directory for a symlink. Harmless where unused. */
    if (strcmp(mode, "freestanding") != 0) nob_cmd_append(&cmd, "-ldl");
#endif
    nob_cmd_append(&cmd, "-o", exec_tmp);
    
    if (out_hash) *out_hash = hash_cmd(&cmd);
    bool success = nob_cmd_run_sync(cmd);
    nob_cmd_free(cmd);
    return success;
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

static void print_proven_test_begin(const Proven_Test_Case *test) {
    nob_log(NOB_INFO, "[PROVEN][TEST][BEGIN] path=%s title=%s", test->path, test->title);
    nob_log(NOB_INFO, "[PROVEN][TEST][INTENT] %s", test->intent);
    nob_log(NOB_INFO, "[PROVEN][TEST][FAIL_HINT] %s", test->failure_hint);
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
    if (strcmp(src, "src/proven/random.c") == 0) return false;
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
            if (!end) break;

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

    size_t available = 0;
    for (size_t t = 0; t < NOB_ARRAY_LEN(targets); ++t) {
        const Proven_Cross_Target *target = &targets[t];
        const char *standard_flag = NULL;
        if (!command_available(target->compiler)) {
            nob_log(NOB_WARNING, "Skipping %s: compiler not found: %s", target->name, target->compiler);
            continue;
        }
        if (!cross_target_toolchain_usable(build_root, target, sysroot, &standard_flag)) {
            continue;
        }
        available += 1;

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

        const char *smoke = target->freestanding ? "tests/test_portability_freestanding.c" : "tests/test_portability_cross_compile_smoke.c";
        char smoke_path[768];
        if (!format_path(smoke_path, sizeof(smoke_path), "%s/smoke.o", target_dir)) return false;
        Nob_Cmd cmd = {0};
        nob_cmd_append(&cmd, target->compiler);
        append_cross_cflags(&cmd, target, standard_flag, sysroot);
        nob_cmd_append(&cmd, "-c", smoke, "-o", smoke_path);
        bool ok = nob_cmd_run_sync(cmd);
        nob_cmd_free(cmd);
        if (!ok) {
            nob_log(NOB_ERROR, "[PROVEN][TEST][FAIL] path=cross/%s stage=compile-smoke source=%s", target->name, smoke);
            nob_log(NOB_ERROR, "[PROVEN][TEST][FAIL_HINT] Check public header feature guards and target-specific compiler diagnostics above.");
            return false;
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
            nob_cmd_append(&link, smoke_path, "tests/test_portability_cross_link_smoke.c");
            nob_cmd_append(&link, "-o", exe_path, "-lwinpthread");
            bool linked = nob_cmd_run_sync(link);
            nob_cmd_free(link);
            if (!linked) {
                nob_log(NOB_ERROR, "[PROVEN][TEST][FAIL] path=cross/%s stage=link", target->name);
                nob_log(NOB_ERROR, "[PROVEN][TEST][FAIL_HINT] A symbol resolved on ELF but not on this target's object format (e.g. a PE/COFF weak symbol). Inspect the undefined reference above.");
                return false;
            }
            nob_log(NOB_INFO, "[PROVEN][TEST][INFO] path=cross/%s stage=link linked smoke executable", target->name);
        }

        nob_log(NOB_INFO, "[PROVEN][TEST][PASS] path=cross/%s", target->name);
    }

    if (available == 0) {
        nob_log(NOB_WARNING, "No cross compilers were available. Install target toolchains and rerun `./nob cross`.");
    }
    return true;
}

int main(int argc, char **argv)
{
    // Shift off the command name (nob)
    nob_shift_args(&argc, &argv);

    bool force_rebuild = false;
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

    if (argc == 0) {
        show_help = true;
    }

    // Parse commands and options
    while (argc > 0) {
        if (strcmp(argv[0], "release") == 0) {
            build_mode = "release";
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
            nob_log(NOB_INFO, "Cleaning build directory...");
#if defined(_WIN32) || defined(_WIN64)
            system("rmdir /s /q build");
#else
            system("rm -rf build");
#endif
            return 0;
        } else if (strcmp(argv[0], "-f") == 0) {
            force_rebuild = true;
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
        printf("  release            Optimized build (-O3).\n");
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
        printf("  bench-float        Run the float parse benchmark only.\n");
        printf("  clean              Remove the build directory.\n");
        printf("  help, -h, --help   Show this help message.\n\n");
        printf("Options:\n");
        printf("  -f                 Force a full rebuild of the project.\n");
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
        printf("  ./nob bench-float  Build and run the float parse benchmark.\n");
        printf("  ./nob cross -build-root /home/user/work/build/proven_c_lib  Compile target matrix.\n");
        printf("  ./nob clean        Clean up all build artifacts.\n");
        return 0;
    }

    if (build_root == NULL) build_root = getenv("PROVEN_BUILD_ROOT");
    if (build_root == NULL) build_root = "build";

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
    if (strcmp(build_mode, "release") == 0) nob_sb_append_cstr(&hash_src, "-O3");
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
    
    if (!mkdir_p_safe(build_root)) return 1;
    if (!mkdir_p_safe(build_dir)) return 1;

    // Subdirs
    static const char *subdirs[] = {"src", "src/proven", "platform", "tests", "manual", "manual/examples"};
    for (size_t i = 0; i < NOB_ARRAY_LEN(subdirs); ++i) {
        char buf[256];
        if (!format_path(buf, sizeof(buf), "%s/%s", build_dir, subdirs[i])) return 1;
        if (!mkdir_p_safe(buf)) return 1;
    }

    const char *srcs[] = {
        "src/proven/stream.c", "src/proven/memory.c", "src/proven/arena.c", "src/proven/pool.c", "src/proven/buffer.c",
        "src/proven/heap.c", "src/proven/u8str.c", "src/proven/u16str.c", "src/proven/array.c",
        "src/proven/ring.c", "src/proven/map.c", "src/proven/algorithm.c", "src/proven/hash.c", "src/proven/random.c", "src/proven/fs.c",
        "src/proven/time.c", "src/proven/fmt.c", "src/proven/mmap.c", "src/proven/sysio.c",
        "src/proven/job.c", "src/proven/scan.c", "src/proven/float_decimal.c", "src/proven/float_parse.c", "src/proven/float_format.c", "src/proven/panic.c", "platform/proven_sys_mem.c",
        "platform/proven_sys_fs.c", "platform/proven_sys_time.c", "platform/proven_sys_env.c", "platform/proven_sys_random.c",
        "platform/proven_sys_thread.c", "platform/proven_sys_io.c", "platform/proven_sys_math.c"
    };

    const char *headers[] = {
        "include/proven.h", "include/proven/list.h", "include/proven/fmt.h", "include/proven/types.h",
        "include/proven/time.h", "include/proven/array.h", "include/proven/heap.h", "include/proven/arena.h",
        "include/proven/mmap.h", "include/proven/scan.h", "include/proven/algorithm.h", "include/proven/error.h",
        "include/proven/coro.h", "include/proven/ring.h", "include/proven/memory.h", "include/proven/map.h",
        "include/proven/buffer.h", "include/proven/fs.h", "include/proven/sysio.h", "include/proven/align.h",
        "include/proven/u8str.h", "include/proven/u16str.h", "include/proven/job.h", "include/proven/allocator.h",
        "include/proven/pool.h", "include/proven/version.h", "include/proven/alias_xcv.h",
        "include/proven/float_config.h", "include/proven/float_format.h", "include/proven/float_parse.h", "src/proven/float_decimal.h",
        "platform/proven_sys_mem.h", "platform/proven_sys_fs.h", "platform/proven_sys_time.h",
        "platform/proven_sys_env.h", "platform/proven_sys_thread.h", "platform/proven_sys_io.h", "platform/proven_sys_math.h"
    };

    const Proven_Test_Case all_tests[] = {
        { "tests/test_unit_memory_views", "memory byte views", "Verify immutable and mutable byte view construction keeps pointer, size, and aliasing contracts explicit.", "Inspect memory view constructors and callers that pass borrowed storage; size or pointer mismatches usually point to a broken view initialization contract." },
        { "tests/test_unit_foundation", "foundation primitives", "Verify checked arithmetic, result values, and basic public type assumptions used by the rest of the library.", "Check include/proven/types.h, include/proven/error.h, and compiler feature macros; a failure here can invalidate many higher-level tests." },
        { "tests/test_unit_memory_slicing", "memory slicing", "Verify owned memory can be exposed as read-only or mutable views and sliced without losing pointer/size accuracy.", "Review src/proven/memory.c and unchecked slice preconditions; failures often mean offset arithmetic or view aliasing changed." },
        { "tests/test_unit_error_results", "error and result primitives", "Verify error helpers and result structs carry explicit success/failure state without hidden control flow.", "Inspect error enum values and PROVEN_IS_OK/proven_is_ok semantics before debugging downstream callers." },
        { "tests/test_unit_arena", "arena allocator", "Verify arena alignment, bump allocation, exhaustion behavior, reset, and no-op free semantics.", "Check proven_arena_init, alignment rounding, and capacity checks; sanitizer failures usually indicate out-of-bounds arena math." },
        { "tests/test_unit_buffer_u8str_basics", "buffer and U8 string basics", "Verify fixed-capacity buffers, literal views, U8 string creation, append, C-string termination, and bounds defense.", "Inspect buffer/u8str capacity accounting and NUL termination; failures often come from off-by-one capacity rules." },
        { "tests/test_unit_pool", "pool allocator", "Verify fixed-size pool allocation rejects wrong sizes and recycles blocks through its bounded free bin.", "Check pool item_size, bin_len, and fallback allocator routing; leaks or wrong reuse indicate free-list corruption." },
        { "tests/test_contract_allocator_dealloc", "allocator deallocation policies", "Demonstrate that arena free is intentionally a no-op while heap free returns memory through the allocator trait.", "If this fails, verify allocator trait wiring and remember that arena lifetime is reset-based, not per-allocation free-based." },
        { "tests/test_unit_u8str_mutation", "U8 string mutation", "Verify searching, slicing, replace/insert/remove, atomic append, partial append, and growable append policies.", "Check mutation functions for failure-atomic behavior and stale-view risks; most failures are capacity or memmove boundary mistakes." },
        { "tests/test_unit_u8str_borrow", "U8 string borrow (fixed-capacity over caller memory)", "Verify proven_u8str_borrow/_reset: fixed-capacity ops and fmt work, growing ops refuse to reallocate caller memory, and destroy is a no-op.", "Inspect proven_u8str_borrow/_reset and the borrowed-flag guards in reserve/append_grow/replace_at_grow/destroy." },
        { "tests/test_unit_mem_copy", "bounded memory copy", "Verify proven_mem_copy copies within capacity, rejects overflow without writing, treats a zero-size source as a no-op, and rejects null pointers.", "Inspect proven_mem_copy in src/proven/memory.c if a copy overflows, writes on rejection, or mishandles empty/null inputs." },
        { "tests/test_unit_array", "growable array", "Verify array initialization, validation, push/pop, growth, migration, get/set, and capacity invariants.", "Review element-size arithmetic and reallocation paths; stale element pointers after growth are caller misuse and should not be preserved." },
        { "tests/test_unit_list", "intrusive list", "Verify sentinel initialization, append, reverse iteration, safe removal, and container-of access.", "Check list node linkage and removal ordering; failures usually indicate next/prev corruption or misuse of detached nodes." },
        { "tests/test_unit_ring", "bounded ring", "Verify FIFO order, wraparound, full/empty boundaries, and slot reuse in the fixed-capacity ring buffer.", "Inspect head/tail/len updates and modulo arithmetic; failures often come from off-by-one full/empty handling." },
        { "tests/test_unit_map", "hash map", "Verify open-addressing insertion, lookup, update, deletion, tombstones, and growth behavior.", "Check hash/equality callbacks, tombstone reuse, and rehash migration; failures can also mean borrowed key lifetimes are invalid." },
        { "tests/test_unit_map_owned_key", "map owned-key storage", "Verify owned U8 keys are duplicated into map storage, survive source-buffer mutation, and release their copied bytes on remove and destroy.", "Inspect the owned-key duplication, cleanup, and rehash migration paths if a key is lost, leaks, or follows a mutated source buffer." },
        { "tests/test_contract_map_hardening", "map borrowed-key hardening", "Verify borrowed U8 keys that point into internal map storage are rejected when hardening or debug validation is enabled.", "Inspect the borrowed-key range guard if an internal pointer is accepted or if external borrowed keys stop working." },
        { "tests/test_contract_pool_misuse", "pool double-free hardening", "Verify pool free detects a repeated free when debug validation or hardened validation is enabled.", "Inspect the pool free-trait gating if a repeated free is accepted silently or if the panic hook is not reached." },
        { "tests/test_unit_algorithm", "algorithms", "Verify sort and binary-search helpers over array-backed data with caller-provided comparators.", "Inspect comparator return convention and element-size usage; unstable or wrong ordering usually starts there." },
        { "tests/test_unit_fs_basic", "basic filesystem", "Verify file open/write/read/read-all/size and absolute path classification across hosted platforms.", "Check PAL filesystem open flags, read/write byte counts, cleanup of temporary files, and Windows absolute path rules." },
        { "tests/test_unit_fs_advanced", "advanced filesystem", "Verify directory creation, nested file creation, rename, directory listing, and recursive cleanup behavior.", "Inspect path joining, directory iterator ownership, and platform-specific directory APIs when entries are missing." },
        { "tests/test_unit_fs_metadata_perms", "filesystem metadata and permissions", "Verify metadata queries and permission-related behavior remain explicit and portable enough for hosted targets.", "Check platform stat wrappers, permission assumptions, and test directory cleanup; OS policy differences should be isolated in PAL code." },
        { "tests/test_unit_fs_position_and_sync", "file position, positional I/O, and durability", "Verify seek/tell/truncate/pread/pwrite/sync: pread and pwrite do not move the file position, truncate does not either, and a thing that cannot seek reports PROVEN_ERR_UNSUPPORTED rather than PROVEN_ERR_IO.", "Inspect proven_fs_seek and friends in src/proven/fs.c and the libc calls behind them in platform/proven_sys_io.c." },
        { "tests/test_unit_time_fmt", "time and formatting integration", "Verify time retrieval/conversion and formatter integration for datetime and structured arguments.", "Inspect platform time conversion, formatter datetime branch, and locale-independent assumptions." },
        { "tests/test_unit_mmap", "memory mapped files", "Verify hosted mmap open/map/view/unmap/close behavior and byte visibility over mapped file ranges.", "Check offset alignment, file-size bounds, and PAL map/unmap ownership; failures often show platform handle lifetime issues." },
        { "tests/test_unit_u16str", "U16 strings", "Verify U16 string/code-unit creation, append, view, and optional build behavior where char16_t is available.", "Inspect PROVEN_NO_U16STR guards and UTF-16 code-unit capacity accounting; do not assume one code unit is one Unicode scalar." },
        { "tests/test_unit_sysio_env", "sysio and environment", "Verify standard stream wrappers, formatted console output, environment lookup, and long environment-key handling.", "Check sysio wrappers, env C-string conversion, allocator use for long keys/values, and PAL UTF conversion on Windows." },
        { "tests/test_unit_stream", "writers and readers", "Verify one piece of code can write into a string, a fixed buffer and a file through the same interface; that a full buffer refuses rather than truncates; that a buffered writer loses nothing across auto-flushes; and that the line reader returns the final unterminated line but refuses a line too long for its buffer.", "Inspect src/proven/stream.c. Buffering uses caller-supplied memory: no hidden global state, no allocation the caller did not ask for." },
        { "tests/test_unit_coro", "stackless coroutine", "Verify coroutine macros preserve state across yields and resume to completion with caller-managed storage.", "Inspect coroutine state labels and re-entry rules; failures usually mean state was reset or a yield point was skipped." },
        { "tests/test_unit_job", "job system", "Verify worker creation, concurrent job dispatch, shutdown synchronization, and exactly-once execution of submitted jobs.", "Use TSAN for deeper diagnosis; check admission state, queue sequence counters, atomics, and worker wake/shutdown ordering." },
        { "tests/test_stress_job_concurrency", "job queue stress", "Verify the job queue tolerates a denser concurrent producer pattern and still executes each submitted job exactly once.", "Run this under TSAN first; inspect queue admission, claim, and shutdown ordering if a slot count drifts or a producer stalls." },
        { "tests/test_unit_scan", "scanner", "Verify token extraction, integer/float parsing, strings, literals, cursor movement, and invalid-input errors.", "Inspect scanner cursor advancement and overflow/invalid parsing paths; failures may leave the cursor at the wrong byte." },
        { "tests/test_contract_fmt_failure_policy", "formatter failure policy", "Verify fixed atomic formatting, truncating formatting, growable formatting, and extreme padding safety.", "Check formatter required/written counts, scratch allocation, and failure-atomic append rules before changing format internals." },
        { "tests/test_unit_fmt_f64_accuracy", "float formatter accuracy", "Verify float formatting keeps fixed-point rounding, scientific carry, and special values stable.", "Inspect PROVEN_ARG_F64 digit extraction, carry propagation, and scientific normalization if a formatted value drifts." },
        { "tests/test_portability_float", "float portability", "Verify scan and format float conversion paths stay double-only and keep target-deterministic behavior without long double dependence.", "Inspect src/proven/scan.c and src/proven/fmt.c if long double returns, casts, or target-specific float drift reappear." },
        { "tests/test_unit_float_exact_range", "float exact-range backend", "Verify representative exact-range decimal spellings keep their documented bit patterns without the host strtod fallback.", "Inspect src/proven/scan.c and the shared float decimal helper if the exact-range backend falls back to host strtod or the corpus drifts." },
        { "tests/test_unit_float_format_policy", "float format policy scaffold", "Verify the new float format policy seam preserves the current simple formatter behavior, rejects unsupported shortest-mode requests, and reports invalid inputs clearly.", "Inspect src/proven/float_format.c and include/proven/float_format.h if the policy dispatch or fixed formatter helper regresses." },
        { "tests/test_unit_float_shortest_known", "float shortest known values", "Verify the shortest float formatting policy emits the documented exact spellings for representative f64 and f32 values.", "Inspect src/proven/float_format.c if the shortest-policy output drifts or if RYU requests stop reaching the active backend." },
        { "tests/test_unit_float_shortest_roundtrip", "float shortest round-trip", "Verify shortest float formatting round-trips through host strtod for representative f64 and f32 values.", "Inspect src/proven/float_format.c if the shortest output stops round-tripping, and keep the host strtod oracle limited to tests." },
        { "tests/test_unit_float_shortest_format_roundtrip", "float shortest formatter round-trip", "Verify the shortest formatter round-trips through the parser across the documented corpora.", "Inspect src/proven/float_format.c and the shortest-digit engines if a value fails to round-trip." },
        { "tests/test_unit_float_shortest_tie_break", "float shortest tie-break corpus", "Verify the shortest corpus keeps the 0.001 fixed-versus-scientific tie-break cases pinned for both widths.", "Inspect tests/test_unit_float_shortest_roundtrip.c and tests/test_differential_float_corpus_f64.c if the tie-break corpus disappears or is renamed." },
        { "tests/test_unit_float_shortest_scientific_guard", "float shortest scientific guard", "Verify the shortest float formatter handles very small finite values by producing a valid shortest candidate instead of an invalid scientific normalization result.", "Inspect src/proven/float_decimal.c and src/proven/float_format.c if the shortest formatter rejects a tiny finite value or emits an invalid scientific spelling." },
        { "tests/test_differential_float_host_oracle_f64", "float host oracle", "Verify representative finite float parsing and simple fixed-format rendering match the platform C library on the same inputs without sharing implementation code.", "Inspect src/proven/scan.c and src/proven/float_format.c if the host oracle and library disagree on the representative finite corpus." },
        { "tests/test_differential_float_host_oracle_f32", "float host oracle float32", "Verify representative finite float32 fixed-format rendering matches the platform C library on the same inputs without sharing implementation code.", "Inspect src/proven/float_format.c if the float32 fixed formatter stops matching the host oracle corpus." },
        { "tests/test_differential_float_corpus_f64", "float upgrade corpus", "Verify the representative exact-range, subnormal-boundary, and shortest-format corpus stays pinned to the documented spellings while the float upgrade remains staged.", "Inspect src/proven/scan.c and src/proven/float_format.c if a representative corpus value changes bit pattern or shortest spelling." },
        { "tests/test_differential_float_corpus_f32", "float upgrade corpus float32 coverage", "Verify the upgrade corpus source also keeps the documented float32 shortest literals pinned alongside the existing float64 cases.", "Inspect tests/test_differential_float_corpus_f64.c if the float32 corpus section disappears or drifts from the documented literals." },
        { "tests/test_unit_float_f32_boundaries", "float32 boundary neighbors", "Verify the float32 upgrade and shortest corpora pin the ULP-adjacent neighbors around FLT_MIN and FLT_TRUE_MIN so the parser-driven backend keeps the documented boundary spellings.", "Inspect tests/test_differential_float_corpus_f64.c and tests/test_unit_float_shortest_roundtrip.c if a float32 boundary-neighbor corpus value disappears or changes spelling." },
        { "tests/test_contract_float_module_layout", "float module scaffold", "Verify the shared float helpers live in a dedicated internal translation unit instead of being copied into fmt.c and scan.c.", "Inspect src/proven/float_decimal.c, src/proven/float_decimal.h, fmt.c, scan.c, and nob.c if the shared decimal helper scaffold regresses." },
        { "tests/test_unit_float_bits", "float bit extraction", "Verify the internal float bit helpers preserve raw IEEE-754 bit patterns for f32 and f64 values, including signed zero, infinities, and NaN payloads.", "Inspect src/proven/float_decimal.c if the raw byte-copy helpers stop matching the object representation." },
        { "tests/test_unit_u128_mul", "wide multiply helper", "Verify the shared 64x64 to 128-bit multiply helper returns exact high and low halves for representative operands.", "Inspect src/proven/float_decimal.c if the wide multiply helper stops matching the reference product." },
        { "tests/test_unit_float_bigint_divmod", "bigint divmod helper", "Verify the shared big-integer division helper returns an exact quotient and remainder (q*den + r == num, r < den) across single-limb and multi-limb operands.", "Inspect proven_float_bigint_divmod (Knuth Algorithm D) in src/proven/float_decimal.c if reconstruction fails." },
        { "tests/test_unit_float_parse_api", "float parse API", "Verify the public ASCII float parser and strtod-like wrapper expose consumed-length, endptr, and range signaling over the shared exact backend.", "Inspect include/proven/float_parse.h, src/proven/float_parse.c, and src/proven/float_decimal.c if the public parser seam or wrapper contract drifts." },
        { "tests/test_unit_float_rfc_0001_cases", "RFC-0001 parse audit", "Verify the decimal-to-binary64 rewrite still satisfies the explicit named cases from docs/proposals/rfc-0001.", "Inspect docs/proposals/rfc-0001, include/proven/float_parse.h, src/proven/float_parse.c, and src/proven/float_decimal.c if a named RFC audit case fails." },
        { "tests/test_unit_fmt_fastpath", "formatter truncation comparison", "Compare truncating fixed-capacity formatting against the growable reference path for exact-fit, truncation, malformed format, and excess-argument cases.", "Inspect truncation accounting and the fixed-capacity write path if the result bytes or counts drift." },
        { "tests/test_unit_fmt_spec", "format spec grammar", "Verify precision, bases, case, alternate form, sign, char and bool - and that a spec the argument cannot honour is refused rather than ignored.", "Inspect the spec parser and render_integer / the float case in src/proven/fmt.c." },
        { "tests/test_unit_scan_f64_accuracy", "float scanner accuracy", "Verify float scanning preserves exact small values, signed zero, round-trip style decimals, exponent edges, and cursor rollback on malformed input.", "Inspect proven_scan_f64 decimal accumulation, exponent scaling, and failure-atomic cursor restore if any exact-value case drifts." },
        { "tests/test_unit_scan_f64_bounds", "float scanner boundary behavior", "Verify float scanning treats underflow as signed zero, reports overflow deterministically, and preserves cursor rollback at the true boundary cases.", "Inspect proven_scan_f64 exponent-to-value handling and final finite checks if a boundary token returns the wrong error or wrong sign." },
        { "tests/test_contract_scan_f64_overflow", "float scanner overflow", "Verify extremely large floating-point input reports PROVEN_ERR_OVERFLOW instead of silently accepting infinity.", "Inspect proven_scan_f64 exponent/range checks and math PAL behavior if this fails." },
        { "tests/test_portability_nob_std_probe", "build driver standard probe", "Verify nob probes -std=c23 first and falls back to -std=c2x when the compiler rejects c23.", "Inspect nob.c standard-flag selection and toolchain probing if the fallback does not trigger." },
        { "tests/test_contract_sysio_scan_nonseekable", "sysio scan non-seekable rejection", "Verify one-chunk sysio scanning rejects pipes/stdin-like inputs before consuming bytes.", "Inspect the seekability probe in proven_sysio_scan_chunk_impl if a non-seekable handle is accepted or partially consumed." },
        { "tests/test_unit_sysio_scanner_init", "sysio scanner init allocator validation", "Verify buffered scanner initialization rejects partial allocators and leaves the scanner zero-safe on failure.", "Inspect proven_sysio_scanner_init if a partial allocator is accepted, called, or leaves non-zero state behind." },
        { "tests/test_contract_sysio_scan_truncation", "sysio scan truncation", "Verify one-chunk scanning rejects inputs that exceed the fixed chunk and keeps the file position reusable after failure.", "Inspect proven_sysio_scan_chunk_impl truncation detection and cursor rewind if a long input is accepted or consumed." },
        { "tests/test_unit_sysio_scanner_boundary", "sysio scanner boundary refill", "Verify buffered sysio scanning resumes across a chunk boundary, refills as needed, and only reports EOF after the final token is consumed.", "Inspect proven_sysio_scanner_scan_impl staging, refill handling, and EOF transition behavior when a token reaches the end of the buffer." },
        { "tests/test_unit_sysio_scanner", "sysio-backed scanner", "Verify scanner operation over file-backed sysio data rather than only in-memory string views.", "Check file open/read wrappers, scanner buffer refill logic, and temporary file permissions." },
        { "tests/test_contract_public_structs", "public array/map/filesystem contracts", "Verify corrupted public array and map structs fail safely and filesystem append-mode requests keep write intent explicit.", "Inspect public invariant guards in array/map mutation entry points and the filesystem open-flag translation if a corrupt struct or append request slips through." },
        { "tests/test_regression_v26_05", "v26.05 regressions", "Protect previously fixed map, format, scan, array/string aliasing, and environment-value regressions.", "Read the named sub-check in TEST.md, then inspect the exact historical area before simplifying the regression." },
        { "tests/test_contract_map_hardening", "map borrowed-key hardening", "Verify borrowed U8 keys that point into internal map storage are rejected when hardening or debug validation is enabled.", "Inspect the borrowed-key range guard if an internal pointer is accepted or if external borrowed keys stop working." },
        { "tests/test_contract_pool_misuse", "pool double-free hardening", "Verify pool free detects a repeated free when debug validation or hardened validation is enabled.", "Inspect the pool free-trait gating if a repeated free is accepted silently or if the panic hook is not reached." },
        { "tests/test_regression_fs_copy_to_self", "filesystem self-copy regression", "Verify copy-to-self and copy-to-hardlink-self fail without truncating or corrupting the source file.", "Inspect same-file detection and open/truncate ordering; never open the destination for truncation before proving it is not the source." },
        { "tests/test_regression_fs_slurp", "filesystem whole-file read/write", "Verify read_all reads to EOF (so size-0-reporting sources like FIFOs and /proc are not silently returned empty), read_all_u8str is NUL-terminated, and write_file/write_file_atomic round-trip without leaving a temp file behind.", "Inspect internal_slurp_path and internal_read_to_eof in src/proven/fs.c; proven_fs_size reports 0 for non-regular files, so the reported size may only seed capacity, never bound the read." },
        { "tests/test_regression_scanner_rollback", "scanner rollback after a failed scan", "Verify a scan that fails on an oversized token restores the stream exactly, dropping no byte and duplicating none.", "Inspect scanner_fill compaction and the rollback in proven_sysio_scanner_scan_impl; a snapshot taken before compaction must be reconciled with how far the buffer moved." },
        { "tests/test_regression_v26_07", "v26.07 regressions", "Protect the fixed u8str NUL-seal (reserve and zero-output fmt growth), signed datetime-year formatting, and pool init capacity-claim defects.", "Read the failing section name; each maps to one area (u8str.c reserve, fmt.c growth and PROVEN_ARG_DATETIME, pool.c init ordering)." },
        { "tests/test_regression_sort_duplicates", "sort on duplicate keys", "Verify proven_array_sort stays sub-quadratic on all-equal, few-distinct, and degenerate orderings, and moves wide elements intact.", "Inspect the partition in src/proven/algorithm.c: a two-way partition that sends elements equal to the pivot to one side is quadratic on duplicates. The comparison-count bounds, not wall-clock time, are what this test measures." },
        { "tests/test_unit_fmt_custom", "formatting a user-defined type", "Verify PROVEN_ARG_OF renders a type the library has never heard of through a caller-supplied function, that width/fill/alignment apply to the rendered result, that a spec the library cannot interpret for that type is refused, that the renderer's own error reaches the caller, and that a non-deterministic renderer is caught instead of silently breaking an aligned column.", "Inspect render_custom and the counting/emitting sinks in src/proven/fmt.c. The renderer runs twice - once to measure, once to emit - which is what lets alignment work without allocating." },
        { "tests/test_contract_fmt_atomic", "the fixed-capacity format is atomic on failure", "Verify a failed fixed-capacity format leaves the string byte-for-byte as it was - after an overflow, after a format error discovered halfway through the output, and after an argument-count error - and that it still reports how many bytes it needed.", "Inspect the single-pass branch of proven_u8str_fmt_internal in src/proven/fmt.c. It writes as it goes now, so atomicity rests on the rollback restoring internal.len and resealing the NUL." },
        { "tests/test_regression_scanner_short_read", "the scanner over a pipe: a short read is not an end of input", "Verify a token split across two pipe writes scans whole rather than being committed truncated, that the rest of the stream stays readable, and that a failed read is PROVEN_ERR_IO rather than a clean end of input.", "Inspect scanner_fill in src/proven/sysio.c. read() on a pipe returns whatever has arrived so far; only a zero-byte read means the input ended. Regular files hide this bug entirely - a file read is short only at real EOF." },
        { "tests/test_regression_float_exact_pow5", "the exact float fallback uses an exact power of five", "Verify an exact halfway value in the 56..350 exponent window breaks to even, and that values just below and just above a rounding boundary there land on the correct double.", "Inspect proven_float_bigint_build_pow5_cached in src/proven/float_decimal.c: above the exact u128 table, 5^q must be multiplied, not taken from the rounded Eisel-Lemire table and shifted. That table is rounded and 5^q is odd, so the shift can never be exact - and this is the tier whose whole job is exactness." },
        { "tests/test_regression_map_churn", "a map with churn does not grow without bound", "Verify a bounded live set with endless insert/remove keeps the table capacity bounded, that live keys survive an in-place rehash, that removed keys stay removed, and that a genuinely growing map still grows.", "Inspect map_rehash in src/proven/map.c. `used` counts tombstones and never falls on its own, so doubling on every rehash trigger grows a steady-state cache forever - 100 live entries reached 33 MB." },
        { "tests/test_contract_sort_alignment", "the sort never hands the comparator a misaligned element", "Verify sorting over-aligned elements passes only correctly-aligned pointers to the caller's comparator, and still sorts.", "Inspect insertion_sort in src/proven/algorithm.c: its scratch is handed straight to the comparator, so it must be over-aligned, and elements aligned more strictly than the scratch must take the swap path." },
        { "tests/test_contract_allocator_trait", "the allocator trait means the same thing for every allocator", "Verify alloc(0), realloc(ptr, 0) and over-aligned allocations answer identically for the heap and the arena, and that shrinking a non-tail block in a full arena still succeeds.", "Inspect src/proven/heap.c, src/proven/arena.c and the contract in include/proven/allocator.h. A trait whose answer depends on which allocator you were handed is not a trait." },
        { "tests/test_regression_fs_walk_errors", "the walk's error branches and TOCTOU safety", "Verify a readdir() that fails mid-directory is reported with the last path component as the name and the directory's own depth (not its children's), and that a directory swapped for a symlink at the moment of descent is not followed out of the tree.", "Inspect the readdir-failure branch of proven_fs_walk_next and the fd-relative, O_NOFOLLOW descent (proven_sys_fs_dir_open_at). Both defects were found by the standing audit and are pinned here against the same fault injection." },
        { "tests/test_unit_random", "OS randomness", "Verify proven_random_bytes succeeds on a hosted platform, fills every byte (tail included), that two draws differ and are not trivially structured (all-zero, all-equal, or a counter), that len 0 is a successful no-op, and that proven_random_u64 draws distinct words.", "Inspect platform/proven_sys_random.c. There is no known-answer test for randomness; these are the properties that separate a real CSPRNG from the ways it usually breaks." },
        { "tests/test_unit_map_keyed", "map: keyed hashing for untrusted string keys", "Verify a default string-key map hashes with a keyed hash that differs from FNV (so an attacker cannot compute collisions), that proven_map_create_trusted opts into FNV, and that both kinds set/get/remove 500 string keys correctly.", "Inspect the hash selection in src/proven/map.c and the per-process key drawn from proven_random_bytes. The security property has no known-answer vector; the observable contract is via proven_map_hash." },
        { "tests/test_unit_hash", "hashing, by use case", "Verify proven_hash_bytes (FNV-1a), proven_hash_keyed (SipHash-2-4), proven_crc32, and proven_sha256 each match their algorithm's own official known-answer vectors, that the CRC and SHA streaming APIs agree with the one-shot over the concatenation, and that the SHA-256 hex spelling matches sha256sum.", "Inspect src/proven/hash.c. A mismatch means the implementation disagrees with the published standard, not merely with itself - the vectors are the differential oracle." },
        { "tests/test_unit_fs_walk", "the recursive walk", "Verify proven_fs_walk reports every entry once in pre-order with the right depth, reports a symlink cycle without descending into it, REPORTS an unreadable directory as an error rather than skipping it silently, honours max_depth while still reporting the boundary directory, and streams a wide directory rather than buffering it.", "Inspect proven_fs_walk_open/_next/_close in src/proven/fs.c. Every assertion in this test is a sentence the contract in include/proven/fs.h makes - it was written from that contract, before the implementation existed." },
        { "tests/test_regression_fs_perms_and_types", "filesystem permissions and entry types", "Verify a copy carries the source's mode, that an atomic write never exposes its contents under a wider mode (a watcher thread stats the temp while the write is in flight), that a symlink and a FIFO are PROVEN_FS_TYPE_OTHER rather than regular files, and that syncing a PRIVATE mapping is PROVEN_ERR_UNSUPPORTED rather than a silent no-op reported as success.", "Inspect proven_fs_copy and internal_write_file_atomic in src/proven/fs.c, the is_regular mapping in platform/proven_sys_fs.c, and proven_mmap_sync." },
        { "tests/test_regression_stream_partial_write", "partial writes, failed reads, and {:f}", "Verify a sink that accepts only part of a chunk receives every byte exactly once (the buffered writer used to re-send the whole buffer, duplicating the accepted prefix), that a read failure is reported as an error rather than as a clean end of file, and that {:f} forces the fixed form at any magnitude.", "Inspect writer_buffered_flush and reader_buffered_fill in src/proven/stream.c, and never_scientific in src/proven/float_format.c. All three defects looked correct against a sink that never misbehaves." },
        { "tests/test_regression_fmt_spec_silently_wrong", "formatter specs that used to be silently wrong", "Verify {:08} zero-pads instead of eating the 0 as a width digit, and that a spec the argument cannot honour (hex on a double or a string) is rejected instead of being ignored.", "Inspect the spec parser and the applicability guard in src/proven/fmt.c. A spec that is accepted and then ignored is worse than one that is rejected." },
        { "tests/test_portability_source_contracts", "source portability contracts", "Verify source-level guards for platform branches that are hard to execute on the current host.", "A failure points at a missing safety pattern or stale documentation contract; inspect the named source file and keep this narrow." },
        { "tests/test_contract_float_module_layout", "float module scaffold", "Verify the shared float helpers live in a dedicated internal translation unit instead of being copied into fmt.c and scan.c.", "Inspect src/proven/float_decimal.c, src/proven/float_decimal.h, fmt.c, scan.c, and nob.c if the shared decimal helper scaffold regresses." },
        { "tests/test_contract_arena_panic", "arena panic path", "Verify alloc-or-panic succeeds when capacity exists and invokes the panic hook on arena exhaustion.", "Check panic hook installation/restoration and arena capacity math; a failure can hide fatal OOM paths." },
        { "tests/test_docs_alias_smoke", "alias layer smoke", "Verify public XCV alias macros continue to compile and map to the intended canonical proven APIs.", "Inspect include/proven/alias_xcv.h and TEST.md alias coverage when public symbols are renamed or added." },
        { "tests/test_docs_alias_completeness", "alias layer completeness", "Verify every public proven_* function has an xcv_* alias, by parsing the public headers and the alias header.", "A listed name has no alias: add `#define xcv_<name> proven_<name>` to include/proven/alias_xcv.h, keeping the file alphabetical. A half-covered alias layer fails at the caller's call site, not here." },
        { "manual/examples/ex_01_errors", "manual example: errors as values", "Compile and run the errors-as-values example printed in manual chapter 1.", "The manual prints this program verbatim. If it fails, the chapter is now telling readers something untrue - fix the example, then re-copy it into the chapter." },
        { "manual/examples/ex_02_arena", "manual example: arena allocator", "Compile and run the arena example printed in manual chapter 2.", "The manual prints this program verbatim. If it fails, the chapter is now telling readers something untrue - fix the example, then re-copy it into the chapter." },
        { "manual/examples/ex_02_pool", "manual example: pool allocator", "Compile and run the pool example printed in manual chapter 2.", "The manual prints this program verbatim. If it fails, the chapter is now telling readers something untrue - fix the example, then re-copy it into the chapter." },
        { "manual/examples/ex_03_u8str", "manual example: owned and borrowed strings", "Compile and run the u8str example printed in manual chapter 3.", "The manual prints this program verbatim. If it fails, the chapter is now telling readers something untrue - fix the example, then re-copy it into the chapter." },
        { "manual/examples/ex_04_array", "manual example: array, sort, binary search", "Compile and run the array example printed in manual chapter 4.", "The manual prints this program verbatim. If it fails, the chapter is now telling readers something untrue - fix the example, then re-copy it into the chapter." },
        { "manual/examples/ex_04_hash", "manual example: hashing by use case", "Compile and run the hashing example printed in manual chapter 4.", "The manual prints this program verbatim. If it fails, the chapter is now telling readers something untrue - fix the example, then re-copy it into the chapter." },
        { "manual/examples/ex_04_map", "manual example: map with int and owned string keys", "Compile and run the map example printed in manual chapter 4.", "The manual prints this program verbatim. If it fails, the chapter is now telling readers something untrue - fix the example, then re-copy it into the chapter." },
        { "manual/examples/ex_05_fs_wholefile", "manual example: whole-file read and write", "Compile and run the whole-file example printed in manual chapter 5.", "The manual prints this program verbatim. If it fails, the chapter is now telling readers something untrue - fix the example, then re-copy it into the chapter." },
        { "manual/examples/ex_05_fs_stream", "manual example: open/read/write/close", "Compile and run the streaming filesystem example printed in manual chapter 5.", "The manual prints this program verbatim. If it fails, the chapter is now telling readers something untrue - fix the example, then re-copy it into the chapter." },
        { "manual/examples/ex_05_fs_walk", "manual example: walking a tree", "Compile and run the recursive-walk example printed in manual chapter 5.", "The manual prints this program verbatim. If it fails, the chapter is now telling readers something untrue - fix the example, then re-copy it into the chapter." },
        { "manual/examples/ex_05_stream", "manual example: writers and readers", "Compile and run the stream example printed in manual chapter 5.", "The manual prints this program verbatim. If it fails, the chapter is now telling readers something untrue - fix the example, then re-copy it into the chapter." },
        { "manual/examples/ex_06_job", "manual example: bounded job system", "Compile and run the job-system example printed in manual chapter 6.", "The manual prints this program verbatim. If it fails, the chapter is now telling readers something untrue - fix the example, then re-copy it into the chapter." },
        { "manual/examples/ex_06_coro", "manual example: stackless coroutine", "Compile and run the coroutine example printed in manual chapter 6.", "The manual prints this program verbatim. If it fails, the chapter is now telling readers something untrue - fix the example, then re-copy it into the chapter." },
        { "manual/examples/ex_08_fmt_scan", "manual example: formatting and scanning", "Compile and run the fmt/scan example printed in manual chapter 8.", "The manual prints this program verbatim. If it fails, the chapter is now telling readers something untrue - fix the example, then re-copy it into the chapter." },
        { "manual/examples/ex_08_fmt_custom", "manual example: formatting a user-defined type", "Compile and run the custom-argument example printed in manual chapter 8.", "The manual prints this program verbatim. If it fails, the chapter is now telling readers something untrue - fix the example, then re-copy it into the chapter." },
        { "manual/examples/ex_08_scan_recovery", "manual example: scanner error codes and recovery", "Compile and run the scanner error-recovery example printed in manual chapter 8, which provokes every scan error code on purpose.", "The manual prints this program verbatim. If it fails, chapter 8 is now telling readers something untrue about the scanner - fix the example, then re-copy it into the chapter." },
        { "tests/test_docs_manual_examples", "manual examples match the manual", "Verify every example the manual prints exists in manual/examples/, matches it verbatim, and that no example file is left unquoted.", "The example file is the source of truth: it is compiled and run. Copy its body into the chapter rather than hand-editing the chapter to look right." },
        { "tests/test_docs_manual_ch08_contracts", "manual chapter 8 scanner contracts", "Verify every behaviour manual chapter 8 states as fact about the scanner - error codes, cursor restoration, decimal-only integers, the overflow/underflow asymmetry, and the non-transactional structural scan - is actually true.", "A failing claim is named in the output. The chapter is a contract: decide which side is wrong before changing either." },
    };

    const Proven_Test_Case regression_tests[] = {
        { "tests/test_regression_v26_05", "v26.05 regressions", "Protect previously fixed map, format, scan, array/string aliasing, and environment-value regressions.", "Read the named sub-check in TEST.md, then inspect the exact historical area before simplifying the regression." },
        { "tests/test_unit_map_owned_key", "map owned-key storage", "Verify owned U8 keys are duplicated into map storage, survive source-buffer mutation, and release their copied bytes on remove and destroy.", "Inspect the owned-key duplication, cleanup, and rehash migration paths if a key is lost, leaks, or follows a mutated source buffer." },
        { "tests/test_contract_map_hardening", "map borrowed-key hardening", "Verify borrowed U8 keys that point into internal map storage are rejected when hardening or debug validation is enabled.", "Inspect the borrowed-key range guard if an internal pointer is accepted or if external borrowed keys stop working." },
        { "tests/test_contract_pool_misuse", "pool double-free hardening", "Verify pool free detects a repeated free when debug validation or hardened validation is enabled.", "Inspect the pool free-trait gating if a repeated free is accepted silently or if the panic hook is not reached." },
        { "tests/test_contract_public_structs", "public array/map/filesystem contracts", "Verify corrupted public array and map structs fail safely and filesystem append-mode requests keep write intent explicit.", "Inspect public invariant guards in array/map mutation entry points and the filesystem open-flag translation if a corrupt struct or append request slips through." },
        { "tests/test_regression_fs_copy_to_self", "filesystem self-copy regression", "Verify copy-to-self and copy-to-hardlink-self fail without truncating or corrupting the source file.", "Inspect same-file detection and open/truncate ordering; never open the destination for truncation before proving it is not the source." },
        { "tests/test_regression_fs_slurp", "filesystem whole-file read/write", "Verify read_all reads to EOF (so size-0-reporting sources like FIFOs and /proc are not silently returned empty), read_all_u8str is NUL-terminated, and write_file/write_file_atomic round-trip without leaving a temp file behind.", "Inspect internal_slurp_path and internal_read_to_eof in src/proven/fs.c; proven_fs_size reports 0 for non-regular files, so the reported size may only seed capacity, never bound the read." },
        { "tests/test_regression_scanner_rollback", "scanner rollback after a failed scan", "Verify a scan that fails on an oversized token restores the stream exactly, dropping no byte and duplicating none.", "Inspect scanner_fill compaction and the rollback in proven_sysio_scanner_scan_impl; a snapshot taken before compaction must be reconciled with how far the buffer moved." },
        { "tests/test_regression_v26_07", "v26.07 regressions", "Protect the fixed u8str NUL-seal (reserve and zero-output fmt growth), signed datetime-year formatting, and pool init capacity-claim defects.", "Read the failing section name; each maps to one area (u8str.c reserve, fmt.c growth and PROVEN_ARG_DATETIME, pool.c init ordering)." },
        { "tests/test_regression_sort_duplicates", "sort on duplicate keys", "Verify proven_array_sort stays sub-quadratic on all-equal, few-distinct, and degenerate orderings, and moves wide elements intact.", "Inspect the partition in src/proven/algorithm.c: a two-way partition that sends elements equal to the pivot to one side is quadratic on duplicates. The comparison-count bounds, not wall-clock time, are what this test measures." },
        { "tests/test_regression_scanner_short_read", "the scanner over a pipe: a short read is not an end of input", "Verify a token split across two pipe writes scans whole rather than being committed truncated, that the rest of the stream stays readable, and that a failed read is PROVEN_ERR_IO rather than a clean end of input.", "Inspect scanner_fill in src/proven/sysio.c. read() on a pipe returns whatever has arrived so far; only a zero-byte read means the input ended. Regular files hide this bug entirely - a file read is short only at real EOF." },
        { "tests/test_regression_float_exact_pow5", "the exact float fallback uses an exact power of five", "Verify an exact halfway value in the 56..350 exponent window breaks to even, and that values just below and just above a rounding boundary there land on the correct double.", "Inspect proven_float_bigint_build_pow5_cached in src/proven/float_decimal.c: above the exact u128 table, 5^q must be multiplied, not taken from the rounded Eisel-Lemire table and shifted. That table is rounded and 5^q is odd, so the shift can never be exact - and this is the tier whose whole job is exactness." },
        { "tests/test_regression_map_churn", "a map with churn does not grow without bound", "Verify a bounded live set with endless insert/remove keeps the table capacity bounded, that live keys survive an in-place rehash, that removed keys stay removed, and that a genuinely growing map still grows.", "Inspect map_rehash in src/proven/map.c. `used` counts tombstones and never falls on its own, so doubling on every rehash trigger grows a steady-state cache forever - 100 live entries reached 33 MB." },
        { "tests/test_contract_sort_alignment", "the sort never hands the comparator a misaligned element", "Verify sorting over-aligned elements passes only correctly-aligned pointers to the caller's comparator, and still sorts.", "Inspect insertion_sort in src/proven/algorithm.c: its scratch is handed straight to the comparator, so it must be over-aligned, and elements aligned more strictly than the scratch must take the swap path." },
        { "tests/test_contract_allocator_trait", "the allocator trait means the same thing for every allocator", "Verify alloc(0), realloc(ptr, 0) and over-aligned allocations answer identically for the heap and the arena, and that shrinking a non-tail block in a full arena still succeeds.", "Inspect src/proven/heap.c, src/proven/arena.c and the contract in include/proven/allocator.h. A trait whose answer depends on which allocator you were handed is not a trait." },
        { "tests/test_regression_fs_walk_errors", "the walk's error branches and TOCTOU safety", "Verify a readdir() that fails mid-directory is reported with the last path component as the name and the directory's own depth (not its children's), and that a directory swapped for a symlink at the moment of descent is not followed out of the tree.", "Inspect the readdir-failure branch of proven_fs_walk_next and the fd-relative, O_NOFOLLOW descent (proven_sys_fs_dir_open_at). Both defects were found by the standing audit and are pinned here against the same fault injection." },
        { "tests/test_unit_random", "OS randomness", "Verify proven_random_bytes succeeds on a hosted platform, fills every byte (tail included), that two draws differ and are not trivially structured (all-zero, all-equal, or a counter), that len 0 is a successful no-op, and that proven_random_u64 draws distinct words.", "Inspect platform/proven_sys_random.c. There is no known-answer test for randomness; these are the properties that separate a real CSPRNG from the ways it usually breaks." },
        { "tests/test_unit_map_keyed", "map: keyed hashing for untrusted string keys", "Verify a default string-key map hashes with a keyed hash that differs from FNV (so an attacker cannot compute collisions), that proven_map_create_trusted opts into FNV, and that both kinds set/get/remove 500 string keys correctly.", "Inspect the hash selection in src/proven/map.c and the per-process key drawn from proven_random_bytes. The security property has no known-answer vector; the observable contract is via proven_map_hash." },
        { "tests/test_unit_hash", "hashing, by use case", "Verify proven_hash_bytes (FNV-1a), proven_hash_keyed (SipHash-2-4), proven_crc32, and proven_sha256 each match their algorithm's own official known-answer vectors, that the CRC and SHA streaming APIs agree with the one-shot over the concatenation, and that the SHA-256 hex spelling matches sha256sum.", "Inspect src/proven/hash.c. A mismatch means the implementation disagrees with the published standard, not merely with itself - the vectors are the differential oracle." },
        { "tests/test_unit_fs_walk", "the recursive walk", "Verify proven_fs_walk reports every entry once in pre-order with the right depth, reports a symlink cycle without descending into it, REPORTS an unreadable directory as an error rather than skipping it silently, honours max_depth while still reporting the boundary directory, and streams a wide directory rather than buffering it.", "Inspect proven_fs_walk_open/_next/_close in src/proven/fs.c. Every assertion in this test is a sentence the contract in include/proven/fs.h makes - it was written from that contract, before the implementation existed." },
        { "tests/test_regression_fs_perms_and_types", "filesystem permissions and entry types", "Verify a copy carries the source's mode, that an atomic write never exposes its contents under a wider mode (a watcher thread stats the temp while the write is in flight), that a symlink and a FIFO are PROVEN_FS_TYPE_OTHER rather than regular files, and that syncing a PRIVATE mapping is PROVEN_ERR_UNSUPPORTED rather than a silent no-op reported as success.", "Inspect proven_fs_copy and internal_write_file_atomic in src/proven/fs.c, the is_regular mapping in platform/proven_sys_fs.c, and proven_mmap_sync." },
        { "tests/test_regression_stream_partial_write", "partial writes, failed reads, and {:f}", "Verify a sink that accepts only part of a chunk receives every byte exactly once (the buffered writer used to re-send the whole buffer, duplicating the accepted prefix), that a read failure is reported as an error rather than as a clean end of file, and that {:f} forces the fixed form at any magnitude.", "Inspect writer_buffered_flush and reader_buffered_fill in src/proven/stream.c, and never_scientific in src/proven/float_format.c. All three defects looked correct against a sink that never misbehaves." },
        { "tests/test_regression_fmt_spec_silently_wrong", "formatter specs that used to be silently wrong", "Verify {:08} zero-pads instead of eating the 0 as a width digit, and that a spec the argument cannot honour (hex on a double or a string) is rejected instead of being ignored.", "Inspect the spec parser and the applicability guard in src/proven/fmt.c. A spec that is accepted and then ignored is worse than one that is rejected." },
        { "tests/test_portability_source_contracts", "source portability contracts", "Verify source-level guards for platform branches that are hard to execute on the current host.", "A failure points at a missing safety pattern or stale documentation contract; inspect the named source file and keep this narrow." },
    };

    const Proven_Test_Case freestanding_tests[] = {
        { "tests/test_portability_freestanding_heap_stub", "freestanding heap stub", "Verify PROVEN_FREESTANDING does not expose a valid hosted heap allocator by accident.", "If this fails, inspect platform heap guards and ensure freestanding builds do not pull hosted OS allocation paths." },
        { "tests/test_portability_compile_freestanding", "freestanding compile link", "Verify the reduced freestanding core compiles and links under PROVEN_FREESTANDING.", "Check that hosted-only modules remain excluded and that public headers guard OS-dependent declarations." },
        { "tests/test_portability_compile_nofloat", "no-float formatter compile", "Verify PROVEN_FMT_NO_FLOAT removes floating-point formatting dependencies while keeping integer/text formatting usable.", "Inspect fmt feature guards and accidental references to float/double helpers." },
        { "tests/test_portability_compile_nou16str", "no-U16 compile", "Verify PROVEN_NO_U16STR removes optional U16 string support without breaking the core library.", "Check u16str guards in umbrella headers, source lists, and alias exports." },
        { "tests/test_portability_freestanding", "freestanding runtime core", "Verify allocator-backed arrays, algorithms, intrusive lists, rings, maps, strings, formatting, and scanning in the reduced core.", "Inspect only freestanding-safe modules first; any hosted PAL dependency here is a portability regression." },
    };

    const Proven_Test_Case benchmark_tests[] = {
        { "tests/test_bench_float_parse_paths", "float parse path benchmark", "Compare the shared float parser, wrapper, and host strtod on separate path-oriented decimal corpora and record dated docs output.", "Inspect src/proven/float_parse.c, src/proven/float_decimal.c, and the path-specific corpus split if the timing harness fails or any checksum drifts." },
        { "tests/test_bench_float_parse", "float parse benchmark", "Time the decimal parser against the host strtod on a mixed corpus and record the result.", "Inspect src/proven/float_parse.c and src/proven/float_decimal.c if a timing run regresses or a checksum drifts." },
    };

    print_proven_build_plan(build_mode, compiler_exe, linker_exe, archiver_exe, sysroot, build_root, build_dir,
                            NOB_ARRAY_LEN(srcs), NOB_ARRAY_LEN(all_tests), cross_check, only_regression, benchmark_mode);

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
#if defined(_WIN32) || defined(_WIN64)
    const char *exe_ext = ".exe";
#else
    const char *exe_ext = "";
#endif
    
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

        const char **inputs = nob_temp_alloc(sizeof(const char *) * (1 + NOB_ARRAY_LEN(headers)));
        inputs[0] = srcs[i];
        for (size_t j = 0; j < NOB_ARRAY_LEN(headers); ++j) inputs[j + 1] = headers[j];

        const char *obj_tmp = nob_temp_sprintf("%s.tmp", final_obj_path);
        const char *hash_path = nob_temp_sprintf("%s.cmdhash", final_obj_path);

        uint32_t current_hash = 0;
        {
            Nob_Cmd mock = {0};
            nob_cmd_append(&mock, compiler_exe);
            append_mode_cflags(&mock, build_mode, standard_flag, sysroot);
            if (user_cflags) nob_cmd_append(&mock, user_cflags);
            nob_cmd_append(&mock, "-c", srcs[i], "-o", obj_tmp);
            current_hash = hash_cmd(&mock);
            nob_cmd_free(mock);
        }

        uint32_t old_hash = 0;
        bool hash_differs = !read_cmdhash(hash_path, &old_hash) || old_hash != current_hash;
        bool rebuild = nob_needs_rebuild(final_obj_path, inputs, 1 + NOB_ARRAY_LEN(headers));
        bool should_rebuild = rebuild || hash_differs || !output_file_valid(final_obj_path, false) || force_rebuild;
        if (should_rebuild) {
            nob_log(NOB_INFO, "[PROVEN][BUILD][SOURCE][REBUILD] path=%s", srcs[i]);
            if (!compile_object_tmp(compiler_exe, srcs[i], obj_tmp, build_mode, user_cflags, standard_flag, sysroot, NULL)) {
                nob_log(NOB_ERROR, "[PROVEN][BUILD][FAIL] stage=compile-source path=%s", srcs[i]);
                nob_log(NOB_ERROR, "[PROVEN][BUILD][FAIL_HINT] Read the compiler diagnostic above and inspect the portability contract for this source file.");
                if (nob_file_exists(obj_tmp)) nob_delete_file(obj_tmp);
                return 1;
            }
            if (!nob_rename(obj_tmp, final_obj_path)) {
                nob_log(NOB_ERROR, "[PROVEN][BUILD][FAIL] stage=install-object path=%s", final_obj_path);
                nob_log(NOB_ERROR, "[PROVEN][BUILD][FAIL_HINT] Check the build-root permissions and any stale locked object file at the destination.");
                if (nob_file_exists(obj_tmp)) nob_delete_file(obj_tmp);
                return 1;
            }
            write_cmdhash(hash_path, current_hash);
            library_rebuilt += 1;
        } else {
            nob_log(NOB_INFO, "[PROVEN][BUILD][SOURCE][CACHED] path=%s", srcs[i]);
            library_cached += 1;
        }
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

        size_t input_count = 1 + NOB_ARRAY_LEN(headers) + obj_files.count;
        const char **inputs = nob_temp_alloc(sizeof(const char *) * input_count);
        size_t idx = 0;
        inputs[idx++] = src_path;
        for (size_t j = 0; j < NOB_ARRAY_LEN(headers); ++j) inputs[idx++] = headers[j];
        for (size_t j = 0; j < obj_files.count; ++j) inputs[idx++] = obj_files.items[j];

        const char *hash_path = nob_temp_sprintf("%s.cmdhash", exec_path);
        const char *exec_tmp = nob_temp_sprintf("%s.tmp", exec_path);

        uint32_t current_hash = 0;
        {
            Nob_Cmd mock = {0};
            nob_cmd_append(&mock, linker_exe);
            append_mode_cflags(&mock, build_mode, standard_flag, sysroot);
            /* Pass user -cflags to test compilation too, so config macros that
             * affect public headers (e.g. PROVEN_FLOAT_BIGINT_LIMBS) stay
             * consistent between the library objects and the test sources. */
            if (user_cflags) nob_cmd_append(&mock, user_cflags);
            if (strcmp(build_mode, "asan") == 0) nob_cmd_append(&mock, "-fsanitize=address");
            else if (strcmp(build_mode, "ubsan") == 0) nob_cmd_append(&mock, "-fsanitize=undefined");
            else if (strcmp(build_mode, "tsan") == 0) nob_cmd_append(&mock, "-fsanitize=thread");

            if (strcmp(build_mode, "freestanding") != 0) {
                nob_cmd_append(&mock, "-pthread");
            } else {
                nob_cmd_append(&mock, "-static");
            }

            nob_cmd_append(&mock, src_path);
            for (size_t j = 0; j < obj_files.count; ++j) nob_cmd_append(&mock, obj_files.items[j]);
            if (user_ldflags) nob_cmd_append(&mock, user_ldflags);
            nob_cmd_append(&mock, "-o", exec_tmp);
            current_hash = hash_cmd(&mock);
            nob_cmd_free(mock);
        }

        uint32_t old_hash = 0;
        bool hash_differs = !read_cmdhash(hash_path, &old_hash) || old_hash != current_hash;
        bool needs_link = nob_needs_rebuild(exec_path, inputs, idx) || !output_file_valid(exec_path, true);
        bool should_link = needs_link || hash_differs || force_rebuild;

        if (should_link) {
            tests_rebuilt += 1;
            nob_log(NOB_INFO, "[PROVEN][BUILD][TEST][REBUILD] path=%s", test->path);
            if (!link_executable_tmp(linker_exe, src_path, obj_files, exec_tmp, build_mode, user_cflags, user_ldflags, standard_flag, sysroot, NULL)) {
                print_proven_test_fail(test, "link", "The test executable did not link. Read the compiler diagnostics above, then check source/header/API drift.");
                if (nob_file_exists(exec_tmp)) nob_delete_file(exec_tmp);
                return 1;
            }
            if (!nob_rename(exec_tmp, exec_path)) {
                print_proven_test_fail(test, "install", "The linked executable could not be moved into place. Check build-root permissions and stale locked files.");
                if (nob_file_exists(exec_tmp)) nob_delete_file(exec_tmp);
                return 1;
            }
            write_cmdhash(hash_path, current_hash);
        } else {
            tests_cached += 1;
            nob_log(NOB_INFO, "[PROVEN][BUILD][TEST][CACHED] path=%s", test->path);
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
