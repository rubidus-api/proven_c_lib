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
        "Verify the shortest float-format backend uses parser-driven round-trip helpers without the old binary-search sweep or a separate integer-shortcut helper.",
        "Inspect src/proven/float_format.c if the shortest backend keeps a precision-search helper, reintroduces a separate integer shortcut, or stops routing through the round-trip search helper."
    );

    char *src = read_file_text("src/proven/float_format.c");
    require_text_missing(src, "proven_float_format_find_shortest_precision", "old binary-search helper should be removed");
    require_text_present(src, "proven_float_format_build_shortest_roundtrip_f64", "f64 round-trip formatter helper should exist");
    require_text_present(src, "proven_float_format_build_shortest_roundtrip_f32", "f32 round-trip formatter helper should exist");
    require_text_present(src, "proven_float_format_roundtrip_search_fixed", "shared round-trip search helper should remain present");
    require_text_missing(src, "proven_float_format_try_integer_shortest_f64", "separate f64 integer shortcut should be folded into the round-trip search path");
    require_text_missing(src, "proven_float_format_try_integer_shortest_f32", "separate f32 integer shortcut should be folded into the round-trip search path");
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
