#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#define NOB_IMPLEMENTATION
#include "nob.h"
#include <string.h>

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

static void append_mode_cflags(Nob_Cmd *cmd, const char *mode) {
    nob_cmd_append(cmd, "-D_DEFAULT_SOURCE", "-D_POSIX_C_SOURCE=200809L");
    if (strcmp(mode, "release") == 0) nob_cmd_append(cmd, "-O3");
    else if (strcmp(mode, "asan") == 0) nob_cmd_append(cmd, "-g", "-O0", "-fsanitize=address");
    else if (strcmp(mode, "ubsan") == 0) nob_cmd_append(cmd, "-g", "-O0", "-fsanitize=undefined");
    else if (strcmp(mode, "tsan") == 0) nob_cmd_append(cmd, "-g", "-O0", "-fsanitize=thread");
    else if (strcmp(mode, "strict-error") == 0) nob_cmd_append(cmd, "-g", "-O0", "-Wall", "-Wextra", "-Werror");
    else if (strcmp(mode, "freestanding") == 0) nob_cmd_append(cmd, "-g", "-O0", "-Wall", "-Wextra", "-Werror", "-DPROVEN_FREESTANDING", "-DPROVEN_FMT_NO_FLOAT", "-DPROVEN_NO_U16STR", "-ffreestanding");
    else nob_cmd_append(cmd, "-g", "-O0");
    nob_cmd_append(cmd, "-std=c23", "-I./include", "-I./platform");
}

static bool compile_object_tmp(const char *compiler, const char *src, const char *obj_tmp, const char *mode, const char *extra_cflags, uint32_t *out_hash) {
    Nob_Cmd cmd = {0};
    nob_cmd_append(&cmd, compiler);
    append_mode_cflags(&cmd, mode);
    if (extra_cflags) nob_cmd_append(&cmd, extra_cflags);
    nob_cmd_append(&cmd, "-c", src, "-o", obj_tmp);
    
    if (out_hash) *out_hash = hash_cmd(&cmd);
    bool success = nob_cmd_run_sync(cmd);
    nob_cmd_free(cmd);
    return success;
}

static bool link_executable_tmp(const char *compiler, const char *src, Nob_File_Paths obj_files, const char *exec_tmp, const char *mode, const char *extra_ldflags, uint32_t *out_hash) {
    Nob_Cmd cmd = {0};
    nob_cmd_append(&cmd, compiler);
    append_mode_cflags(&cmd, mode);
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
    if (!shell_token_is_safe(path)) {
        nob_log(NOB_ERROR, "Unsafe directory path: %s", path ? path : "(null)");
        return false;
    }
#if defined(_WIN32) || defined(_WIN64)
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "mkdir %s >nul 2>nul", path);
#else
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", path);
#endif
    return system(cmd) == 0;
}

static bool command_available(const char *cmd_name) {
    if (!shell_token_is_safe(cmd_name)) return false;
#if defined(_WIN32) || defined(_WIN64)
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "where %s >nul 2>nul", cmd_name);
#else
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "command -v %s >/dev/null 2>&1", cmd_name);
#endif
    return system(cmd) == 0;
}

typedef struct {
    const char *name;
    const char *compiler;
    bool freestanding;
    const char *arch_flag_1;
    const char *arch_flag_2;
} Proven_Cross_Target;

static bool cross_source_is_freestanding(const char *src) {
    if (strcmp(src, "src/proven/u16str.c") == 0) return false;
    if (strcmp(src, "src/proven/fs.c") == 0) return false;
    if (strcmp(src, "src/proven/sysio.c") == 0) return false;
    if (strcmp(src, "src/proven/mmap.c") == 0) return false;
    if (strcmp(src, "src/proven/job.c") == 0) return false;
    if (strcmp(src, "platform/proven_sys_fs.c") == 0) return false;
    if (strcmp(src, "platform/proven_sys_thread.c") == 0) return false;
    if (strcmp(src, "platform/proven_sys_io.c") == 0) return false;
    if (strcmp(src, "platform/proven_sys_env.c") == 0) return false;
    if (strcmp(src, "platform/proven_sys_time.c") == 0) return false;
    if (strcmp(src, "platform/proven_sys_mem.c") == 0) return false;
    return true;
}

static void append_cross_cflags(Nob_Cmd *cmd, const Proven_Cross_Target *target) {
    if (target->freestanding) {
        nob_cmd_append(cmd,
                       "-std=c23", "-ffreestanding", "-nostdlib",
                       "-DPROVEN_FREESTANDING", "-DPROVEN_FMT_NO_FLOAT", "-DPROVEN_NO_U16STR",
                       "-Wall", "-Wextra", "-Werror", "-I./include", "-I./platform");
    } else {
        nob_cmd_append(cmd,
                       "-std=c23", "-D_DEFAULT_SOURCE", "-D_POSIX_C_SOURCE=200809L",
                       "-Wall", "-Wextra", "-Werror", "-I./include", "-I./platform");
    }
    if (target->arch_flag_1) nob_cmd_append(cmd, target->arch_flag_1);
    if (target->arch_flag_2) nob_cmd_append(cmd, target->arch_flag_2);
}

static bool cross_target_toolchain_usable(const char *build_root, const Proven_Cross_Target *target) {
    char probe_dir[512];
    snprintf(probe_dir, sizeof(probe_dir), "%s/_probe", build_root);
    if (!mkdir_p_safe(probe_dir)) return false;

    char src_path[768];
    char obj_path[768];
    snprintf(src_path, sizeof(src_path), "%s/%s.c", probe_dir, target->name);
    snprintf(obj_path, sizeof(obj_path), "%s/%s.o", probe_dir, target->name);

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
    append_cross_cflags(&cmd, target);
    nob_cmd_append(&cmd, "-c", src_path, "-o", obj_path);
    bool ok = nob_cmd_run_sync(cmd);
    nob_cmd_free(cmd);

    if (!ok) {
        nob_log(NOB_WARNING, "Skipping %s: compiler exists but target flags/sysroot are not usable", target->name);
    }
    return ok;
}

static bool run_cross_compile_matrix(const char *build_root,
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
        if (!command_available(target->compiler)) {
            nob_log(NOB_WARNING, "Skipping %s: compiler not found: %s", target->name, target->compiler);
            continue;
        }
        if (!cross_target_toolchain_usable(build_root, target)) {
            continue;
        }
        available += 1;

        char target_dir[512];
        snprintf(target_dir, sizeof(target_dir), "%s/%s", build_root, target->name);
        if (!mkdir_p_safe(target_dir)) return false;

        nob_log(NOB_INFO, "Cross compile target: %s (%s)", target->name, target->compiler);

        for (size_t i = 0; i < srcs_count; ++i) {
            if (target->freestanding && !cross_source_is_freestanding(srcs[i])) continue;

            char obj_name[256];
            sanitize_name(obj_name, sizeof obj_name, srcs[i]);

            char obj_path[768];
            snprintf(obj_path, sizeof(obj_path), "%s/%s.o", target_dir, obj_name);

            Nob_Cmd cmd = {0};
            nob_cmd_append(&cmd, target->compiler);
            append_cross_cflags(&cmd, target);
            nob_cmd_append(&cmd, "-c", srcs[i], "-o", obj_path);
            bool ok = nob_cmd_run_sync(cmd);
            nob_cmd_free(cmd);
            if (!ok) return false;
        }

        const char *smoke = target->freestanding ? "tests/test_freestanding.c" : "tests/test_cross_compile_smoke.c";
        char smoke_path[768];
        snprintf(smoke_path, sizeof(smoke_path), "%s/smoke.o", target_dir);
        Nob_Cmd cmd = {0};
        nob_cmd_append(&cmd, target->compiler);
        append_cross_cflags(&cmd, target);
        nob_cmd_append(&cmd, "-c", smoke, "-o", smoke_path);
        bool ok = nob_cmd_run_sync(cmd);
        nob_cmd_free(cmd);
        if (!ok) return false;
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
        printf("  LD                 Alternative way to specify the linker.\n\n");
        printf("Examples:\n");
        printf("  ./nob build        Build and run all tests in debug mode.\n");
        printf("  ./nob release      Build and run all tests with optimizations.\n");
        printf("  ./nob -cc clang    Build using the Clang compiler.\n");
        printf("  ./nob -cflags \"-DDEBUG\"  Build with custom debug definition.\n");
        printf("  ./nob asan -f      Force rebuild and check for memory leaks with ASan.\n");
        printf("  ./nob cross -build-root /mnt/ai-share/build/proven_c_lib  Compile target matrix.\n");
        printf("  ./nob clean        Clean up all build artifacts.\n");
        return 0;
    }

    if (cc == NULL) cc = getenv("CC");
    if (cc == NULL) cc = getenv("NOB_COMPILER");
    if (cc == NULL) cc = "gcc";

    const char *compiler_exe = cc;

    if (user_ld == NULL) user_ld = getenv("LD");
    if (user_ld == NULL) user_ld = compiler_exe;
    const char *linker_exe = user_ld;

    // Calculate flag hash for directory isolation
    Nob_String_Builder hash_src = {0};
    nob_sb_append_cstr(&hash_src, compiler_exe);
    nob_sb_append_cstr(&hash_src, linker_exe);
    nob_sb_append_cstr(&hash_src, build_mode);
    nob_sb_append_cstr(&hash_src, "-std=c23-I./include-D_DEFAULT_SOURCE-D_POSIX_C_SOURCE=200809L");
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

    if (build_root == NULL) build_root = getenv("PROVEN_BUILD_ROOT");
    if (build_root == NULL) build_root = "build";

    char build_dir[512];
    snprintf(build_dir, sizeof(build_dir), "%s/%s-%s-%08x", build_root, compiler_name, build_mode, flag_hash);
    
    if (!mkdir_p_safe(build_root)) return 1;
    if (!mkdir_p_safe(build_dir)) return 1;

    // Subdirs
    static const char *subdirs[] = {"src", "src/proven", "platform", "tests"};
    for (size_t i = 0; i < NOB_ARRAY_LEN(subdirs); ++i) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%s/%s", build_dir, subdirs[i]);
        if (!mkdir_p_safe(buf)) return 1;
    }

    const char *srcs[] = {
        "src/proven/memory.c", "src/proven/arena.c", "src/proven/pool.c", "src/proven/buffer.c",
        "src/proven/heap.c", "src/proven/u8str.c", "src/proven/u16str.c", "src/proven/array.c",
        "src/proven/ring.c", "src/proven/map.c", "src/proven/algorithm.c", "src/proven/fs.c",
        "src/proven/time.c", "src/proven/fmt.c", "src/proven/mmap.c", "src/proven/sysio.c",
        "src/proven/job.c", "src/proven/scan.c", "src/proven/panic.c", "platform/proven_sys_mem.c",
        "platform/proven_sys_fs.c", "platform/proven_sys_time.c", "platform/proven_sys_env.c",
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
        "platform/proven_sys_mem.h", "platform/proven_sys_fs.h", "platform/proven_sys_time.h",
        "platform/proven_sys_env.h", "platform/proven_sys_thread.h", "platform/proven_sys_io.h", "platform/proven_sys_math.h"
    };

    const char *all_tests[] = {
        "tests/test_phase1", "tests/test_foundation", "tests/test_phase2", "tests/test_phase3",
        "tests/test_phase4", "tests/test_phase5", "tests/test_phase6_pool", "tests/test_dealloc",
        "tests/test_phase7_u8str_mut", "tests/test_phase8_array", "tests/test_phase9_list",
        "tests/test_phase10_ring", "tests/test_phase11_map", "tests/test_phase12_algorithm",
        "tests/test_phase13_fs", "tests/test_phase14_fs_advanced", "tests/test_phase15_fs_security",
        "tests/test_phase16_time_fmt", "tests/test_phase17_mmap", "tests/test_phase17_u16str",
        "tests/test_phase18_sysio", "tests/test_phase19_coro", "tests/test_phase20_job",
        "tests/test_phase21_scan", "tests/test_phase22_fmt_best_effort", "tests/test_scan_overflow_f64", 
        "tests/test_sysio_scanner", "tests/test_regression_v26_05",
        "tests/test_regression_fs_copy_to_self", "tests/test_regression_source_contracts",
        "tests/test_arena_panic", "tests/test_alias_smoke"
    };

    const char *regression_tests[] = {
        "tests/test_regression_v26_05",
        "tests/test_regression_fs_copy_to_self",
        "tests/test_regression_source_contracts"
    };

    const char *freestanding_tests[] = {
        "tests/test_freestanding_heap_stub",
        "tests/test_compile_freestanding",
        "tests/test_compile_nofloat",
        "tests/test_compile_nou16str",
        "tests/test_freestanding"
    };

    if (build_root == NULL) build_root = getenv("PROVEN_BUILD_ROOT");
    if (build_root == NULL) build_root = "build";

    if (cross_check) {
        const char *cross_root = nob_temp_sprintf("%s/cross", build_root);
        return run_cross_compile_matrix(cross_root, srcs, NOB_ARRAY_LEN(srcs), headers, NOB_ARRAY_LEN(headers)) ? 0 : 1;
    }

    const char **tests = only_regression ? regression_tests : all_tests;
    size_t tests_count = only_regression ? NOB_ARRAY_LEN(regression_tests) : NOB_ARRAY_LEN(all_tests);

    if (strcmp(build_mode, "freestanding") == 0) {
        tests = freestanding_tests;
        tests_count = NOB_ARRAY_LEN(freestanding_tests);
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
    for (size_t i = 0; i < NOB_ARRAY_LEN(srcs); ++i) {
        if (strcmp(build_mode, "freestanding") == 0) {
            if (strcmp(srcs[i], "src/proven/u16str.c") == 0) continue;
            if (strcmp(srcs[i], "src/proven/fs.c") == 0) continue;
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

        // We need to calculate the hash to see if flags changed
        uint32_t current_hash = 0;
        // Mock command to get hash without running yet
        {
            Nob_Cmd mock = {0};
            nob_cmd_append(&mock, compiler_exe);
            append_mode_cflags(&mock, build_mode);
            if (user_cflags) nob_cmd_append(&mock, user_cflags);
            nob_cmd_append(&mock, "-c", srcs[i], "-o", obj_tmp);
            current_hash = hash_cmd(&mock);
            nob_cmd_free(mock);
        }

        uint32_t old_hash = 0;
        bool hash_differs = !read_cmdhash(hash_path, &old_hash) || old_hash != current_hash;
        bool rebuild = nob_needs_rebuild(final_obj_path, inputs, 1 + NOB_ARRAY_LEN(headers));

        if (rebuild || hash_differs || !output_file_valid(final_obj_path, false) || force_rebuild) {
            if (!compile_object_tmp(compiler_exe, srcs[i], obj_tmp, build_mode, user_cflags, NULL)) {
                if (nob_file_exists(obj_tmp)) nob_delete_file(obj_tmp);
                return 1;
            }
            if (!nob_rename(obj_tmp, final_obj_path)) {
                if (nob_file_exists(obj_tmp)) nob_delete_file(obj_tmp);
                return 1;
            }
            write_cmdhash(hash_path, current_hash);
        }
    }

    // Tests Compilation & Execution
    for (size_t i = 0; i < tests_count; ++i) {
        const char *src_path = nob_temp_sprintf("%s.c", tests[i]);
        const char *exec_path = nob_temp_sprintf("%s/%s%s", build_dir, tests[i], exe_ext);

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
            append_mode_cflags(&mock, build_mode);
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
        
        if (needs_link || hash_differs || force_rebuild) {
            if (!link_executable_tmp(linker_exe, src_path, obj_files, exec_tmp, build_mode, user_ldflags, NULL)) {
                if (nob_file_exists(exec_tmp)) nob_delete_file(exec_tmp);
                return 1;
            }
            if (!nob_rename(exec_tmp, exec_path)) {
                if (nob_file_exists(exec_tmp)) nob_delete_file(exec_tmp);
                return 1;
            }
            write_cmdhash(hash_path, current_hash);
        }
        
        cmd.count = 0;
        nob_cmd_append(&cmd, exec_path);
        if (!nob_cmd_run_sync(cmd)) return 1;
        nob_temp_reset();
    }

    nob_cmd_free(cmd);
    return 0;
}
