#include "proven_test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *path;
    const char *title;
    const char *intent;
    const char *failure_hint;
} Proven_Test_Case;

#define PROVEN_BUILD_TESTS_MANIFEST_CONSUMER 1
#include "../build_tests.inc"
#ifndef PROVEN_BUILD_TESTS_MANIFEST_INCLUDED
#error "build_tests.inc must define PROVEN_BUILD_TESTS_MANIFEST_INCLUDED"
#endif
#undef PROVEN_BUILD_TESTS_MANIFEST_CONSUMER

#define ARRAY_LEN(xs) (sizeof(xs) / sizeof((xs)[0]))

typedef struct {
    const char *label;
    const Proven_Test_Case *items;
    size_t count;
} test_registry_t;

static const test_registry_t test_registries[] = {
    { "all_tests[]", all_tests, ARRAY_LEN(all_tests) },
    { "regression_tests[]", regression_tests, ARRAY_LEN(regression_tests) },
    { "freestanding_tests[]", freestanding_tests, ARRAY_LEN(freestanding_tests) },
    { "benchmark_tests[]", benchmark_tests, ARRAY_LEN(benchmark_tests) },
    { "cross_compile_tests[]", cross_compile_tests, ARRAY_LEN(cross_compile_tests) },
    { "cross_link_tests[]", cross_link_tests, ARRAY_LEN(cross_link_tests) },
};

static const test_registry_t primary_test_registries[] = {
    { "all_tests[]", all_tests, ARRAY_LEN(all_tests) },
    { "freestanding_tests[]", freestanding_tests, ARRAY_LEN(freestanding_tests) },
    { "benchmark_tests[]", benchmark_tests, ARRAY_LEN(benchmark_tests) },
    { "cross_compile_tests[]", cross_compile_tests, ARRAY_LEN(cross_compile_tests) },
    { "cross_link_tests[]", cross_link_tests, ARRAY_LEN(cross_link_tests) },
};

/*
 * TEST.md is the catalog: one entry per test, saying what the test answers and
 * what to inspect when it fails. Nothing checked that the catalog matched the
 * build, and both halves had already stopped being true - ten tests registered
 * in nob.c had no entry at all, and the headline counts named numbers that
 * matched neither nob.c nor the tree.
 *
 * A catalog nobody checks is a catalog that documents the suite as it was. So
 * this test checks five propositions:
 *
 *   1. Every test in the shared preprocessed registry has a
 *      `### `tests/NAME`` entry in TEST.md.
 *   2. TEST.md has exactly one entry per tests/test_*.c file - no test file
 *      undocumented, no entry describing a file that was deleted.
 *   3. Each registry is unique, regression_tests[] is a subset of all_tests[],
 *      and every test source belongs to exactly one primary registry.
 *   4. TEST.md's regression membership list equals regression_tests[].
 *   5. The suite, subset, tree, and filename-class counts printed in TEST.md
 *      equal the current arrays and files.
 *
 * The second is the stronger one: it fails when a test is added and when a
 * test is removed, which is when the catalog actually rots.
 */

static char *read_text_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    buf[got] = '\0';
    fclose(f);
    return buf;
}

/* Does TEST.md carry a catalog heading for this test name? */
static bool catalog_has_entry(const char *test_md, const char *name) {
    char needle[256];
    snprintf(needle, sizeof needle, "### `tests/%s`", name);
    return strstr(test_md, needle) != NULL;
}

/* Count `### `tests/test_...`` headings at the start of a line. */
static int count_catalog_entries(const char *test_md) {
    int n = 0;
    const char *p = test_md;
    const char *hit;
    while ((hit = strstr(p, "### `tests/test_")) != NULL) {
        if (hit == test_md || hit[-1] == '\n') ++n;
        p = hit + 1;
    }
    return n;
}

static bool line_contains_count(const char *text, const char *row_prefix, int count) {
    const char *row = strstr(text, row_prefix);
    if (!row) return false;
    const char *line_end = strchr(row, '\n');
    if (!line_end) line_end = row + strlen(row);

    char expected[32];
    snprintf(expected, sizeof expected, "| %d |", count);
    const char *hit = strstr(row, expected);
    return hit != NULL && hit < line_end;
}

enum {
    REGISTRY_PATH_CAP = 400,
    REGISTRY_PATH_SIZE = 200
};

typedef struct {
    char items[REGISTRY_PATH_CAP][REGISTRY_PATH_SIZE];
    int count;
    int malformed;
} registry_path_list_t;

static registry_path_list_t collect_markdown_paths(const char *begin, const char *end) {
    registry_path_list_t out = {0};
    const char *p = begin;

    while (p < end) {
        const char *line_end = strchr(p, '\n');
        if (!line_end || line_end > end) line_end = end;
        const char *q = p;
        while (q < line_end && (*q == ' ' || *q == '\t')) ++q;

        if ((size_t)(line_end - q) >= 4u && q[0] == '-' && q[1] == ' ' && q[2] == '`') {
            const char *path_begin = q + 3;
            const char *path_end = path_begin;
            while (path_end < line_end && *path_end != '`') ++path_end;
            size_t len = (size_t)(path_end - path_begin);
            if (path_end >= line_end || len == 0 || len >= REGISTRY_PATH_SIZE ||
                out.count >= REGISTRY_PATH_CAP) {
                ++out.malformed;
            } else {
                memcpy(out.items[out.count], path_begin, len);
                out.items[out.count][len] = '\0';
                ++out.count;
            }
        }
        p = line_end < end ? line_end + 1 : end;
    }
    return out;
}

static bool path_list_contains(const registry_path_list_t *list, const char *path) {
    for (int i = 0; i < list->count; ++i) {
        if (strcmp(list->items[i], path) == 0) return true;
    }
    return false;
}

static int report_path_list_duplicates(const registry_path_list_t *list,
                                       const char *label) {
    int duplicates = 0;
    for (int i = 0; i < list->count; ++i) {
        for (int prior = 0; prior < i; ++prior) {
            if (strcmp(list->items[i], list->items[prior]) == 0) {
                printf("[PROVEN][TEST][INFO] duplicate %s entry: %s\n",
                       label, list->items[i]);
                ++duplicates;
                break;
            }
        }
    }
    return duplicates;
}

static bool registry_contains(const Proven_Test_Case *items, size_t count,
                              const char *path) {
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(items[i].path, path) == 0) return true;
    }
    return false;
}

static int registry_count_prefix(const Proven_Test_Case *items, size_t count,
                                 const char *prefix) {
    int matched = 0;
    size_t prefix_len = strlen(prefix);
    for (size_t i = 0; i < count; ++i) {
        if (strncmp(items[i].path, prefix, prefix_len) == 0) ++matched;
    }
    return matched;
}

static int report_registry_duplicates(const test_registry_t *registry) {
    int duplicates = 0;
    for (size_t i = 0; i < registry->count; ++i) {
        for (size_t prior = 0; prior < i; ++prior) {
            if (strcmp(registry->items[i].path,
                       registry->items[prior].path) == 0) {
                printf("[PROVEN][TEST][INFO] duplicate %s entry: %s\n",
                       registry->label, registry->items[i].path);
                ++duplicates;
                break;
            }
        }
    }
    return duplicates;
}

static int report_registry_paths_missing_from(
    const Proven_Test_Case *subset, size_t subset_count,
    const Proven_Test_Case *superset, size_t superset_count,
    const char *subset_label, const char *superset_label) {
    int missing = 0;
    for (size_t i = 0; i < subset_count; ++i) {
        if (!registry_contains(superset, superset_count, subset[i].path)) {
            printf("[PROVEN][TEST][INFO] %s entry absent from %s: %s\n",
                   subset_label, superset_label, subset[i].path);
            ++missing;
        }
    }
    return missing;
}

static int report_registry_paths_missing_from_list(
    const Proven_Test_Case *subset, size_t subset_count,
    const registry_path_list_t *superset,
    const char *subset_label, const char *superset_label) {
    int missing = 0;
    for (size_t i = 0; i < subset_count; ++i) {
        if (!path_list_contains(superset, subset[i].path)) {
            printf("[PROVEN][TEST][INFO] %s entry absent from %s: %s\n",
                   subset_label, superset_label, subset[i].path);
            ++missing;
        }
    }
    return missing;
}

static int report_list_paths_missing_from_registry(
    const registry_path_list_t *subset,
    const Proven_Test_Case *superset, size_t superset_count,
    const char *subset_label, const char *superset_label) {
    int missing = 0;
    for (int i = 0; i < subset->count; ++i) {
        if (!registry_contains(superset, superset_count, subset->items[i])) {
            printf("[PROVEN][TEST][INFO] %s entry absent from %s: %s\n",
                   subset_label, superset_label, subset->items[i]);
            ++missing;
        }
    }
    return missing;
}

static int primary_registry_memberships(const char *path) {
    int memberships = 0;
    for (size_t i = 0; i < ARRAY_LEN(primary_test_registries); ++i) {
        const test_registry_t *registry = &primary_test_registries[i];
        if (registry_contains(registry->items, registry->count, path)) {
            ++memberships;
        }
    }
    return memberships;
}

int main(void) {
    PROVEN_TEST_SUITE("the test catalog matches the build",
        "Every registered test must have one catalog entry, each registry must contain unique paths, regression membership must match TEST.md, and every published suite and filename-class count must match the current tree.",
        "TEST.md is the catalog. Reconcile it with the registry arrays and tests/test_*.c when a missing path, duplicate, or stale count is reported.");

    char *test_md = read_text_file("TEST.md");
    PROVEN_TEST_ASSERT(test_md != NULL,
        "could not read TEST.md",
        "This test must run from the repository root.");

    const int hosted_test_count =
        registry_count_prefix(all_tests, ARRAY_LEN(all_tests), "tests/test_");
    const int manual_example_count =
        registry_count_prefix(all_tests, ARRAY_LEN(all_tests), "manual/examples/");
    const int regression_test_count =
        registry_count_prefix(regression_tests, ARRAY_LEN(regression_tests),
                              "tests/test_");
    const int freestanding_test_count =
        registry_count_prefix(freestanding_tests, ARRAY_LEN(freestanding_tests),
                              "tests/test_");
    const int benchmark_test_count =
        registry_count_prefix(benchmark_tests, ARRAY_LEN(benchmark_tests),
                              "tests/test_");
    const int cross_compile_test_count =
        registry_count_prefix(cross_compile_tests,
                              ARRAY_LEN(cross_compile_tests), "tests/test_");
    const int cross_link_test_count =
        registry_count_prefix(cross_link_tests, ARRAY_LEN(cross_link_tests),
                              "tests/test_");
    PROVEN_TEST_ASSERT(
        hosted_test_count + manual_example_count == (int)ARRAY_LEN(all_tests) &&
        regression_test_count == (int)ARRAY_LEN(regression_tests) &&
        freestanding_test_count == (int)ARRAY_LEN(freestanding_tests) &&
        benchmark_test_count == (int)ARRAY_LEN(benchmark_tests) &&
        cross_compile_test_count == (int)ARRAY_LEN(cross_compile_tests) &&
        cross_link_test_count == (int)ARRAY_LEN(cross_link_tests),
        "a registry contains an executable outside its documented path classes",
        "Use tests/test_* paths for test registries and tests/test_* or manual/examples/* in all_tests[].");
    PROVEN_TEST_ASSERT(
        registry_contains(freestanding_tests, ARRAY_LEN(freestanding_tests),
                          PROVEN_FREESTANDING_CROSS_SMOKE_PATH),
        "the named freestanding cross smoke path is absent from freestanding_tests[]",
        "Keep the cross smoke path stable by name rather than by registry index.");

    PROVEN_TEST_SECTION("every registry has unique paths and regression is a hosted subset",
        "A duplicate executable wastes work and inflates published counts; a regression-only path that is absent from the full suite silently weakens normal coverage.",
        "Remove repeated paths within each array and keep every regression_tests[] path in all_tests[].");
    {
        int duplicates = 0;
        for (size_t i = 0; i < ARRAY_LEN(test_registries); ++i) {
            duplicates += report_registry_duplicates(&test_registries[i]);
        }
        PROVEN_TEST_ASSERT(duplicates == 0,
            "one or more test registry arrays contain duplicate executable paths (listed above)",
            "Keep each executable path exactly once within its own registry.");
        PROVEN_TEST_ASSERT(
            report_registry_paths_missing_from(
                regression_tests, ARRAY_LEN(regression_tests),
                all_tests, ARRAY_LEN(all_tests),
                "regression_tests[]", "all_tests[]") == 0,
            "regression_tests[] contains a path absent from all_tests[] (listed above)",
            "A focused regression is also part of the normal hosted suite.");
    }

    PROVEN_TEST_SECTION("the documented regression membership matches regression_tests[]",
        "A correct subset count can still hide a stale list that sends maintainers to the wrong tests.",
        "Regenerate the bullet list under TEST.md's Regression subset heading from regression_tests[].");
    {
        const char *doc_heading = strstr(test_md, "## Regression subset");
        const char *doc_list_end = doc_heading ? strstr(doc_heading, "\nIntent:") : NULL;
        PROVEN_TEST_ASSERT(doc_heading && doc_list_end,
            "could not locate TEST.md's regression subset membership list",
            "Keep the Regression subset heading, its path bullets, and the following Intent paragraph.");
        registry_path_list_t documented_regressions =
            collect_markdown_paths(doc_heading, doc_list_end);
        PROVEN_TEST_ASSERT(documented_regressions.malformed == 0,
            "could not parse one or more TEST.md regression subset bullets",
            "Write each member as `- `tests/test_name`` on its own line.");
        PROVEN_TEST_ASSERT(
            report_path_list_duplicates(&documented_regressions,
                                        "TEST.md regression subset") == 0,
            "TEST.md's regression subset list contains duplicate paths (listed above)",
            "List each regression_tests[] path exactly once.");
        int missing_from_docs =
            report_registry_paths_missing_from_list(
                regression_tests, ARRAY_LEN(regression_tests),
                &documented_regressions,
                "regression_tests[]", "TEST.md regression subset");
        int stale_in_docs =
            report_list_paths_missing_from_registry(
                &documented_regressions,
                regression_tests, ARRAY_LEN(regression_tests),
                "TEST.md regression subset", "regression_tests[]");
        int order_mismatches = 0;
        if (documented_regressions.count == (int)ARRAY_LEN(regression_tests)) {
            for (size_t i = 0; i < ARRAY_LEN(regression_tests); ++i) {
                if (strcmp(documented_regressions.items[i],
                           regression_tests[i].path) != 0) {
                    printf("[PROVEN][TEST][INFO] regression order mismatch at index %d: registry=%s documentation=%s\n",
                           (int)i, regression_tests[i].path,
                           documented_regressions.items[i]);
                    ++order_mismatches;
                }
            }
        }
        PROVEN_TEST_ASSERT(missing_from_docs == 0 && stale_in_docs == 0 &&
                           order_mismatches == 0,
            "TEST.md's regression subset membership differs from regression_tests[] (listed above)",
            "Replace the bullet list with the exact registry paths in registry order.");
    }

    PROVEN_TEST_SECTION("every shared registry test is in the catalog",
        "A registered test with no entry is a test nobody can look up when it fails.",
        "Add a `### `tests/NAME`` section under the matching class heading in TEST.md.");
    {
        int registered = 0;
        int missing = 0;
        for (size_t registry_index = 0;
             registry_index < ARRAY_LEN(primary_test_registries);
             ++registry_index) {
            const test_registry_t *registry =
                &primary_test_registries[registry_index];
            for (size_t i = 0; i < registry->count; ++i) {
                const char *path = registry->items[i].path;
                if (strncmp(path, "tests/", strlen("tests/")) != 0) {
                    continue;
                }
                ++registered;
                if (!catalog_has_entry(test_md, path + strlen("tests/"))) {
                    printf("[PROVEN][TEST][INFO] registered in %s but absent from TEST.md: %s\n",
                           registry->label, path);
                    ++missing;
                }
            }
        }

        PROVEN_TEST_ASSERT(registered > 100,
            "found suspiciously few tests in the shared registries",
            "The shared manifest is incomplete - the project has well over a hundred tests.");
        PROVEN_TEST_INFO("shared primary registries contain {} tests",
            PROVEN_ARG(registered));
        PROVEN_TEST_ASSERT(missing == 0,
            "tests are registered with no TEST.md entry (listed above)",
            "Add each one to TEST.md under its class heading, with an Intent line and a Failure tip.");
    }

    PROVEN_TEST_SECTION("the catalog has one entry per test file",
        "An entry per file, both directions: a new test that nobody documented, and an entry describing a file that was deleted, are the two ways this file rots.",
        "Compare `ls tests/test_*.c` with the `### `tests/...`` headings in TEST.md.");
    {
        proven_allocator_t heap = proven_heap_allocator();
        proven_result_dir_t opened = proven_fs_dir_open(heap, PROVEN_LIT("tests"));
        PROVEN_TEST_ASSERT(proven_is_ok(opened.err),
            "could not open tests/",
            "This test must run from the repository root.");

        static const char *class_names[] = {
            "unit", "contract", "regression", "differential",
            "portability", "stress", "docs", "bench"
        };
        int class_counts[sizeof class_names / sizeof class_names[0]] = {0};
        int files = 0;
        int unclassified = 0;
        int undocumented = 0;
        int primary_membership_errors = 0;
        registry_path_list_t tree_paths = {0};
        proven_fs_dir_entry_t entry = {0};
        proven_err_t step = PROVEN_OK;
        while (proven_is_ok(step = proven_fs_dir_next(&opened.value, &entry))) {
            const proven_byte_t *n = entry.name.ptr;
            size_t len = entry.name.size;
            if (len < 8) continue;
            if (memcmp(n, "test_", 5) != 0) continue;
            if (memcmp(n + len - 2, ".c", 2) != 0) continue;
            char name[200];
            if (len - 2 >= sizeof name) {
                printf("[PROVEN][TEST][INFO] test filename exceeds catalog capacity\n");
                ++files;
                ++unclassified;
                continue;
            }
            memcpy(name, n, len - 2);
            name[len - 2] = '\0';
            ++files;

            char path[REGISTRY_PATH_SIZE];
            int path_len = snprintf(path, sizeof path, "tests/%s", name);
            if (path_len < 0 || path_len >= (int)sizeof path ||
                tree_paths.count >= REGISTRY_PATH_CAP) {
                printf("[PROVEN][TEST][INFO] test path exceeds registry audit capacity: %s\n",
                       name);
                ++tree_paths.malformed;
            } else {
                memcpy(tree_paths.items[tree_paths.count], path,
                       (size_t)path_len + 1u);
                ++tree_paths.count;
                int memberships = primary_registry_memberships(path);
                if (memberships != 1) {
                    printf("[PROVEN][TEST][INFO] test source has %d primary registry memberships: %s\n",
                           memberships, path);
                    ++primary_membership_errors;
                }
            }

            bool classified = false;
            for (size_t class_index = 0;
                 class_index < sizeof class_names / sizeof class_names[0];
                 ++class_index) {
                char prefix[64];
                snprintf(prefix, sizeof prefix, "test_%s_", class_names[class_index]);
                size_t prefix_len = strlen(prefix);
                if (len >= prefix_len && memcmp(n, prefix, prefix_len) == 0) {
                    ++class_counts[class_index];
                    classified = true;
                    break;
                }
            }
            if (!classified) {
                printf("[PROVEN][TEST][INFO] test file has no recognized filename class: %s\n", name);
                ++unclassified;
            }
            if (!catalog_has_entry(test_md, name)) {
                printf("[PROVEN][TEST][INFO] test file with no TEST.md entry: %s\n", name);
                ++undocumented;
            }
        }
        proven_fs_dir_close(&opened.value);
        PROVEN_TEST_ASSERT(step == PROVEN_ERR_EOF,
            "could not finish reading tests/",
            "Inspect the portable filesystem directory iterator and the tests directory.");
        PROVEN_TEST_ASSERT(tree_paths.malformed == 0,
            "one or more test paths exceeded the catalog gate's checked capacity",
            "Increase the explicit capacity before adding more than 400 tests or a path of 200 bytes.");

        int registry_paths_without_sources = 0;
        for (size_t registry_index = 0;
             registry_index < ARRAY_LEN(primary_test_registries);
             ++registry_index) {
            const test_registry_t *registry =
                &primary_test_registries[registry_index];
            for (size_t i = 0; i < registry->count; ++i) {
                const char *path = registry->items[i].path;
                if (strncmp(path, "tests/test_", strlen("tests/test_")) != 0) {
                    continue;
                }
                if (!path_list_contains(&tree_paths, path)) {
                    printf("[PROVEN][TEST][INFO] %s entry has no test source: %s.c\n",
                           registry->label, path);
                    ++registry_paths_without_sources;
                }
            }
        }

        int entries = count_catalog_entries(test_md);
        PROVEN_TEST_INFO("tests/test_*.c files: {}, TEST.md entries: {}",
            PROVEN_ARG(files), PROVEN_ARG(entries));

        PROVEN_TEST_ASSERT(undocumented == 0,
            "test files exist with no TEST.md entry (listed above)",
            "Every test file gets a catalog entry. If the test is not worth describing, it is not worth keeping.");
        PROVEN_TEST_ASSERT(primary_membership_errors == 0,
            "test files do not belong to exactly one all/freestanding/benchmark/cross registry (listed above)",
            "Register each tests/test_*.c source in exactly one primary registry; regression_tests[] is only a subset view.");
        PROVEN_TEST_ASSERT(registry_paths_without_sources == 0,
            "primary registry entries exist with no tests/test_*.c source (listed above)",
            "Remove the stale entry or restore its source file.");
        PROVEN_TEST_ASSERT(entries == files,
            "TEST.md entry count does not match the number of test files",
            "Either a test was deleted and its entry left behind, or an entry was duplicated. The catalog must be one-to-one with tests/test_*.c.");
        PROVEN_TEST_ASSERT(unclassified == 0,
            "test filenames exist outside the documented class taxonomy (listed above)",
            "Use one of the classes in TEST.md's Naming table or update the taxonomy and this gate together.");

        PROVEN_TEST_SECTION("the published suite counts match the registries and tree",
            "A count is useful only if the build fails when it drifts.",
            "Update TEST.md's headline and class table from the registry arrays and tests/ filenames.");
        {
            char expected[256];
            snprintf(expected, sizeof expected,
                "builds and executes %d registered tests plus the %d runnable manual examples",
                hosted_test_count, manual_example_count);
            PROVEN_TEST_ASSERT(strstr(test_md, expected) != NULL,
                "TEST.md's hosted test or manual example count is stale",
                "Update the Test catalog headline from all_tests[].");
            snprintf(expected, sizeof expected, "%d executables in all",
                hosted_test_count + manual_example_count);
            PROVEN_TEST_ASSERT(strstr(test_md, expected) != NULL,
                "TEST.md's full executable total is stale",
                "Update the Test catalog headline from all_tests[].");
            snprintf(expected, sizeof expected, "re-runs a %d-test subset",
                regression_test_count);
            PROVEN_TEST_ASSERT(strstr(test_md, expected) != NULL,
                "TEST.md's regression subset count is stale",
                "Update the Test catalog headline from regression_tests[].");
            snprintf(expected, sizeof expected, "`./nob freestanding` a %d-test subset",
                freestanding_test_count);
            PROVEN_TEST_ASSERT(strstr(test_md, expected) != NULL,
                "TEST.md's freestanding subset count is stale",
                "Update the Test catalog headline from freestanding_tests[].");
            snprintf(expected, sizeof expected, "`./nob bench-float` %d benchmarks",
                benchmark_test_count);
            PROVEN_TEST_ASSERT(strstr(test_md, expected) != NULL,
                "TEST.md's benchmark count is stale",
                "Update the Test catalog headline from benchmark_tests[].");
            snprintf(expected, sizeof expected, "The tree holds %d test files", files);
            PROVEN_TEST_ASSERT(strstr(test_md, expected) != NULL,
                "TEST.md's test-file count is stale",
                "Update the Test catalog headline from tests/test_*.c.");
        }

        for (size_t class_index = 0;
             class_index < sizeof class_names / sizeof class_names[0];
             ++class_index) {
            char row_prefix[64];
            snprintf(row_prefix, sizeof row_prefix, "| `%s` |", class_names[class_index]);
            if (!line_contains_count(test_md, row_prefix, class_counts[class_index])) {
                printf("[PROVEN][TEST][INFO] stale class count: %s should be %d\n",
                    class_names[class_index], class_counts[class_index]);
                PROVEN_TEST_ASSERT(false,
                    "TEST.md's filename-class count is stale (listed above)",
                    "Update the Naming table from tests/test_<class>_*.c.");
            }
        }
    }

    free(test_md);
    PROVEN_TEST_PASS("the catalog matches the shared registries and the tests/ tree.");
    return 0;
}
