#include "proven_test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_file_text(const char *path) {
    FILE *f = fopen(path, "rb");
    PROVEN_TEST_ASSERT(f != NULL, path, "Inspect src/proven/float_format.c if the shortest formatter source file cannot be opened.");
    PROVEN_TEST_ASSERT(fseek(f, 0, SEEK_END) == 0, path, "Inspect the source file if seeking the end fails.");
    long len = ftell(f);
    PROVEN_TEST_ASSERT(len >= 0, path, "Inspect the source file if its size cannot be measured.");
    PROVEN_TEST_ASSERT(fseek(f, 0, SEEK_SET) == 0, path, "Inspect the source file if it cannot be rewound.");
    char *text = (char *)malloc((size_t)len + 1u);
    PROVEN_TEST_ASSERT(text != NULL, path, "Inspect the source file if allocating the inspection buffer fails.");
    size_t got = fread(text, 1u, (size_t)len, f);
    PROVEN_TEST_ASSERT(got == (size_t)len, path, "Inspect the source file if reading it returns a short count.");
    text[len] = '\0';
    fclose(f);
    return text;
}

static void require_text_present(const char *text, const char *needle, const char *label) {
    PROVEN_TEST_ASSERT(strstr(text, needle) != NULL, label, "Inspect src/proven/float_format.c if the new shared shortest helper is missing.");
}

int main(void) {
    PROVEN_TEST_SUITE(
        "test_float_shortest_common_helper",
        "Verify the shortest float-format backend keeps thin per-width policy shims that forward into one shared shortest helper.",
        "Inspect src/proven/float_format.c if the shared helper disappears or if the policy dispatch stops routing through the width-specific shims."
    );

    char *src = read_file_text("src/proven/float_format.c");
    require_text_present(src, "proven_float_format_build_shortest_common", "shared shortest helper should exist");
    require_text_present(src, "proven_float_format_build_shortest_f64", "f64 shortest wrapper should exist");
    require_text_present(src, "proven_float_format_build_shortest_f32", "f32 shortest wrapper should exist");
    require_text_present(src, "return proven_float_format_build_shortest_common(buf, buf_cap, value, false, 17, written_out);", "f64 wrapper should keep the binary64 width contract");
    require_text_present(src, "return proven_float_format_build_shortest_common(buf, buf_cap, (double)value, true, 9, written_out);", "f32 wrapper should keep the binary32 width contract");
    require_text_present(src, "return proven_float_format_build_shortest_f64(buf, buf_cap, value, written_out);", "f64 policy dispatch should call the width-specific wrapper");
    require_text_present(src, "return proven_float_format_build_shortest_f32(buf, buf_cap, value, written_out);", "f32 policy dispatch should call the width-specific wrapper");
    free(src);

    PROVEN_TEST_PASS("Shared shortest helper routing checks passed.");
    return 0;
}
