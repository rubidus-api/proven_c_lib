#include "proven_test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_file_text(const char *path) {
    FILE *f = fopen(path, "rb");
    PROVEN_TEST_ASSERT(f != NULL, path, "Inspect src/proven/float_format.c if the shortest backend source file cannot be opened.");
    PROVEN_TEST_ASSERT(fseek(f, 0, SEEK_END) == 0, path, "Inspect the source file if seeking the end fails.");
    long n = ftell(f);
    PROVEN_TEST_ASSERT(n >= 0, path, "Inspect the source file if its size cannot be determined.");
    PROVEN_TEST_ASSERT(fseek(f, 0, SEEK_SET) == 0, path, "Inspect the source file if rewinding fails.");
    char *buf = (char *)malloc((size_t)n + 1u);
    PROVEN_TEST_ASSERT(buf != NULL, path, "Inspect the test host if the temporary buffer cannot be allocated.");
    size_t got = fread(buf, 1u, (size_t)n, f);
    PROVEN_TEST_ASSERT(got == (size_t)n, path, "Inspect the source file if the test cannot read its full contents.");
    buf[got] = '\0';
    fclose(f);
    return buf;
}

static void require_text_present(const char *text, const char *needle, const char *label) {
    PROVEN_TEST_ASSERT(strstr(text, needle) != NULL, label, "Inspect src/proven/float_format.c if the round-trip shortest helper disappears.");
}

static void require_text_missing(const char *text, const char *needle, const char *label) {
    PROVEN_TEST_ASSERT(strstr(text, needle) == NULL, label, "Inspect src/proven/float_format.c if the linear precision sweep returns.");
}

int main(void) {
    PROVEN_TEST_SUITE(
        "test_float_shortest_binary_search",
        "Verify the shortest float-format backend keeps thin per-width policy shims while routing both widths through the shared parser-driven round-trip helper.",
        "Inspect src/proven/float_format.c if the shortest backend keeps a precision-search helper, drops the shared round-trip search helper, or removes the width-specific policy shims."
    );

    char *src = read_file_text("src/proven/float_format.c");
    require_text_missing(src, "proven_float_format_find_shortest_precision", "old binary-search helper should be removed");
    require_text_present(src, "proven_float_format_build_shortest_common", "shared shortest helper should remain present");
    require_text_present(src, "proven_float_format_build_shortest_f64", "f64 shortest policy shim should remain present");
    require_text_present(src, "proven_float_format_build_shortest_f32", "f32 shortest policy shim should remain present");
    require_text_present(src, "return proven_float_format_build_shortest_common(buf, buf_cap, value, false, 17, written_out);", "f64 policy shim should keep max precision 17");
    require_text_present(src, "return proven_float_format_build_shortest_common(buf, buf_cap, (double)value, true, 9, written_out);", "f32 policy shim should keep max precision 9");
    require_text_present(src, "proven_float_format_roundtrip_search_fixed(double value, bool use_scientific, bool is_f32, proven_i32 max_precision", "round-trip search helper should keep width and precision parameters");
    require_text_present(src, "for (proven_i32 precision = max_precision; precision >= 0; --precision)", "round-trip search should walk the caller-provided precision boundary");
    require_text_present(src, "proven_float_format_candidate_roundtrips(value, is_f32", "round-trip search should validate candidates through the selected width");
    require_text_present(src, "return proven_float_format_build_shortest_f64(buf, buf_cap, value, written_out);", "f64 policy dispatch should call the width-specific shim");
    require_text_present(src, "return proven_float_format_build_shortest_f32(buf, buf_cap, value, written_out);", "f32 policy dispatch should call the width-specific shim");
    require_text_present(src, "proven_float_format_roundtrip_search_fixed", "shared round-trip search helper should remain present");

    require_text_missing(src, "proven_float_format_best_shortest_style_common", "shared shortest helper should remain removed");
    require_text_missing(
        src,
        "for (proven_i32 precision = 0; precision <= 17; ++precision)",
        "f64 shortest helper should not keep a direct linear precision sweep in the formatter source"
    );
    require_text_missing(
        src,
        "for (proven_i32 precision = 0; precision <= 9; ++precision)",
        "f32 shortest helper should not keep a direct linear precision sweep in the formatter source"
    );
    free(src);

    PROVEN_TEST_PASS("Shortest backend parser-driven checks passed.");
    return 0;
}
