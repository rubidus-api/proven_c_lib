#define NOB_IMPLEMENTATION
#include "nob.h"
#include <string.h>

int main(int argc, char **argv)
{
    NOB_GO_REBUILD_URSELF(argc, argv);

    // Shift off the command name (nob)
    nob_shift_args(&argc, &argv);

    // Read compiler environment
    const char *cc = getenv("CC");
    if (cc == NULL) cc = "gcc";
    
    const char *nob_comp = getenv("NOB_COMPILER");
    if (nob_comp == NULL) nob_comp = "gcc";

    int is_msvc = (strcmp(nob_comp, "msvc") == 0 || strcmp(nob_comp, "icx") == 0);
    int is_pocc = (strcmp(nob_comp, "pocc") == 0);

    Nob_Cmd cmd = {0};

    const char *srcs[] = {
        "src/proven/memory.c",
        "src/proven/arena.c",
        "src/proven/pool.c",
        "src/proven/buffer.c",
        "src/proven/heap.c",
        "src/proven/u8str.c",
        "src/proven/array.c",
        "src/proven/ring.c",
        "src/proven/map.c",
        "src/proven/algorithm.c",
        "src/proven/fs.c",
        "src/proven/time.c",
        "src/proven/fmt.c",
        "src/proven/mmap.c",
        "src/proven/sysio.c",
        "src/proven/job.c",
        "src/proven/scan.c",
        "platform/proven_sys_mem.c",
        "platform/proven_sys_fs.c",
        "platform/proven_sys_time.c",
        "platform/proven_sys_env.c",
        "platform/proven_sys_thread.c",
        "platform/proven_sys_io.c"
    };

    const char *headers[] = {
        "include/proven.h",
        "include/proven/list.h",
        "include/proven/fmt.h",
        "include/proven/types.h",
        "include/proven/time.h",
        "include/proven/array.h",
        "include/proven/heap.h",
        "include/proven/arena.h",
        "include/proven/mmap.h",
        "include/proven/scan.h",
        "include/proven/algorithm.h",
        "include/proven/error.h",
        "include/proven/coro.h",
        "include/proven/ring.h",
        "include/proven/memory.h",
        "include/proven/map.h",
        "include/proven/buffer.h",
        "include/proven/fs.h",
        "include/proven/sysio.h",
        "include/proven/align.h",
        "include/proven/nob.h",
        "include/proven/u8str.h",
        "include/proven/job.h",
        "include/proven/allocator.h",
        "include/proven/pool.h",
        "include/proven/version.h"
    };

    const char *tests[] = {
        "tests/test_phase1",
        "tests/test_phase2",
        "tests/test_phase3",
        "tests/test_phase4",
        "tests/test_phase5",
        "tests/test_phase6_pool",
        "tests/test_dealloc",
        "tests/test_phase7_u8str_mut",
        "tests/test_phase8_array",
        "tests/test_phase9_list",
        "tests/test_phase10_ring",
        "tests/test_phase11_map",
        "tests/test_phase12_algorithm",
        "tests/test_phase13_fs",
        "tests/test_phase14_fs_advanced",
        "tests/test_phase15_fs_security",
        "tests/test_phase16_time_fmt",
        "tests/test_phase17_mmap",
        "tests/test_phase18_sysio",
        "tests/test_phase19_coro",
        "tests/test_phase20_job",
        "tests/test_phase21_scan"
    };

    // Create build directories
    if (!nob_mkdir_if_not_exists("build")) return 1;
    if (!nob_mkdir_if_not_exists("build/src")) return 1;
    if (!nob_mkdir_if_not_exists("build/src/proven")) return 1;
    if (!nob_mkdir_if_not_exists("build/platform")) return 1;
    if (!nob_mkdir_if_not_exists("build/tests")) return 1;

    const char *obj_ext = (is_msvc || is_pocc) ? ".obj" : ".o";
    const char *exe_ext = (is_msvc || is_pocc) ? ".exe" : "";

    // Array to store the paths of generated object files
    Nob_String_Array obj_files = {0};

    // Compile each library source file into an object file
    for (size_t i = 0; i < NOB_ARRAY_LEN(srcs); ++i) {
        Nob_String_Builder obj_path = {0};
        nob_sb_append_cstr(&obj_path, "build/");
        nob_sb_append_cstr(&obj_path, srcs[i]);
        // change .c to .o / .obj
        obj_path.count -= 2; 
        nob_sb_append_cstr(&obj_path, obj_ext);
        nob_sb_append_null(&obj_path);

        nob_da_append(&obj_files, obj_path.items);

        // Check if rebuild is needed for this object
        const char **inputs = nob_temp_alloc(sizeof(const char *) * (1 + NOB_ARRAY_LEN(headers)));
        inputs[0] = srcs[i];
        for (size_t j = 0; j < NOB_ARRAY_LEN(headers); ++j) {
            inputs[j + 1] = headers[j];
        }

        int rebuild_needed = nob_needs_rebuild(obj_path.items, inputs, 1 + NOB_ARRAY_LEN(headers));
        if (rebuild_needed < 0) return 1;

        if (rebuild_needed) {
            cmd.count = 0;
            nob_cmd_append(&cmd, cc);
            for (int arg_idx = 0; arg_idx < argc; ++arg_idx) {
                nob_cmd_append(&cmd, argv[arg_idx]);
            }

            if (is_msvc) {
                nob_cmd_append(&cmd, "/std:c11"); // MSVC doesn't support -std=c2x directly yet generally
                nob_cmd_append(&cmd, "/I./include");
                nob_cmd_append(&cmd, "/c", srcs[i]);
                Nob_String_Builder out_sb = {0};
                nob_sb_append_cstr(&out_sb, "/Fo");
                nob_sb_append_cstr(&out_sb, obj_path.items);
                nob_sb_append_null(&out_sb);
                nob_cmd_append(&cmd, out_sb.items);
            } else if (is_pocc) {
                nob_cmd_append(&cmd, "/std:C17");
                nob_cmd_append(&cmd, "/I./include");
                nob_cmd_append(&cmd, "/c", srcs[i]);
                Nob_String_Builder out_sb = {0};
                nob_sb_append_cstr(&out_sb, "/Fo");
                nob_sb_append_cstr(&out_sb, obj_path.items);
                nob_sb_append_null(&out_sb);
                nob_cmd_append(&cmd, out_sb.items);
            } else {
                nob_cmd_append(&cmd, "-std=c2x");
                nob_cmd_append(&cmd, "-D_POSIX_C_SOURCE=200809L");
                nob_cmd_append(&cmd, "-D_ISOC11_SOURCE");
                nob_cmd_append(&cmd, "-I./include");
                nob_cmd_append(&cmd, "-c", srcs[i]);
                nob_cmd_append(&cmd, "-o", obj_path.items);
            }

            if (!nob_cmd_run_sync(cmd)) return 1;
        } else {
            nob_log(NOB_INFO, "%s is up-to-date", obj_path.items);
        }
    }

    // Compile and link each test
    for (size_t i = 0; i < NOB_ARRAY_LEN(tests); ++i) {
        Nob_String_Builder src_path = {0};
        nob_sb_append_cstr(&src_path, tests[i]);
        nob_sb_append_cstr(&src_path, ".c");
        nob_sb_append_null(&src_path);
        
        Nob_String_Builder exec_path = {0};
        nob_sb_append_cstr(&exec_path, "build/");
        nob_sb_append_cstr(&exec_path, tests[i]);
        nob_sb_append_cstr(&exec_path, exe_ext);
        nob_sb_append_null(&exec_path);

        // A test needs rebuild if its .c file, any header, or any object file changes
        size_t input_count = 1 + NOB_ARRAY_LEN(headers) + obj_files.count;
        const char **inputs = nob_temp_alloc(sizeof(const char *) * input_count);
        size_t input_idx = 0;
        inputs[input_idx++] = src_path.items;
        for (size_t j = 0; j < NOB_ARRAY_LEN(headers); ++j) {
            inputs[input_idx++] = headers[j];
        }
        for (size_t j = 0; j < obj_files.count; ++j) {
            inputs[input_idx++] = obj_files.items[j];
        }

        int rebuild_needed = nob_needs_rebuild(exec_path.items, inputs, input_idx);
        if (rebuild_needed < 0) return 1;
        
        if (rebuild_needed) {
            cmd.count = 0;
            nob_cmd_append(&cmd, cc);
            for (int arg_idx = 0; arg_idx < argc; ++arg_idx) {
                nob_cmd_append(&cmd, argv[arg_idx]);
            }

            if (is_msvc) {
                nob_cmd_append(&cmd, "/std:c11", "/I./include", src_path.items);
                for (size_t j = 0; j < obj_files.count; ++j) {
                    nob_cmd_append(&cmd, obj_files.items[j]);
                }
                Nob_String_Builder out_sb = {0};
                nob_sb_append_cstr(&out_sb, "/Fe");
                nob_sb_append_cstr(&out_sb, exec_path.items);
                nob_sb_append_null(&out_sb);
                nob_cmd_append(&cmd, out_sb.items);
            } else if (is_pocc) {
                nob_cmd_append(&cmd, "/std:C17", "/I./include", src_path.items);
                for (size_t j = 0; j < obj_files.count; ++j) {
                    nob_cmd_append(&cmd, obj_files.items[j]);
                }
                Nob_String_Builder out_sb = {0};
                nob_sb_append_cstr(&out_sb, "/OUT:");
                nob_sb_append_cstr(&out_sb, exec_path.items);
                nob_sb_append_null(&out_sb);
                nob_cmd_append(&cmd, out_sb.items);
            } else {
                nob_cmd_append(&cmd, "-std=c2x");
                nob_cmd_append(&cmd, "-D_POSIX_C_SOURCE=200809L");
                nob_cmd_append(&cmd, "-D_ISOC11_SOURCE");
                nob_cmd_append(&cmd, "-I./include");
                if (!is_msvc && !is_pocc) {
                    nob_cmd_append(&cmd, "-pthread");
                }
                nob_cmd_append(&cmd, src_path.items);
                for (size_t j = 0; j < obj_files.count; ++j) {
                    nob_cmd_append(&cmd, obj_files.items[j]);
                }
                nob_cmd_append(&cmd, "-o", exec_path.items);
            }

            if (!nob_cmd_run_sync(cmd)) return 1;
        } else {
            nob_log(NOB_INFO, "%s is up-to-date", exec_path.items);
        }

        // Run the freshly compiled or up-to-date test
        cmd.count = 0;
        Nob_String_Builder run_sb = {0};
        nob_sb_append_cstr(&run_sb, "./");
        nob_sb_append_cstr(&run_sb, exec_path.items);
        nob_sb_append_null(&run_sb);
        
        nob_log(NOB_INFO, "Executing test: %s", run_sb.items);
        nob_cmd_append(&cmd, run_sb.items);
        if (!nob_cmd_run_sync(cmd)) return 1;
        
        nob_temp_reset();
    }

    nob_cmd_free(cmd);
    return 0;
}
