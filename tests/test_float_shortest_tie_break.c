#include "proven_test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_file_text(const char *path) {
    FILE *f = fopen(path, "rb");
    PROVEN_TEST_ASSERT(f != NULL, path, "Inspect the float corpus file if it cannot be opened.");
    PROVEN_TEST_ASSERT(fseek(f, 0, SEEK_END) == 0, path, "Inspect the float corpus file if seeking its end fails.");
    long n = ftell(f);
    PROVEN_TEST_ASSERT(n >= 0, path, "Inspect the float corpus file if its size cannot be measured.");
    PROVEN_TEST_ASSERT(fseek(f, 0, SEEK_SET) == 0, path, "Inspect the float corpus file if rewinding fails.");
    char *buf = (char *)malloc((size_t)n + 1u);
    PROVEN_TEST_ASSERT(buf != NULL, path, "Inspect the test host if the temporary buffer cannot be allocated.");
    size_t got = fread(buf, 1u, (size_t)n, f);
    PROVEN_TEST_ASSERT(got == (size_t)n, path, "Inspect the float corpus file if the test cannot read its full contents.");
    buf[got] = '\0';
    fclose(f);
    return buf;
}

static void require_text_present(const char *text, const char *needle, const char *label) {
    PROVEN_TEST_ASSERT(strstr(text, needle) != NULL, label, "Inspect the float shortest corpus if the tie-break coverage disappears.");
}

int main(void) {
    PROVEN_TEST_SUITE(
        "test_float_shortest_tie_break",
        "Verify the float shortest corpus keeps the 0.001 and 0.0001 fixed-versus-scientific tie-break cases pinned for both f64 and f32 round-trip coverage.",
        "Inspect tests/test_float_shortest_roundtrip.c and tests/test_float_upgrade_corpus.c if the fixed-versus-scientific tie-break coverage disappears."
    );

    char *roundtrip = read_file_text("tests/test_float_shortest_roundtrip.c");
    char *corpus = read_file_text("tests/test_float_upgrade_corpus.c");

    PROVEN_TEST_SECTION(
        "float shortest tie-break corpus source contract",
        "Confirm the shortest round-trip corpus records the 0.001 and 0.0001 tie-break values for both widths.",
        "Inspect the shortest corpus files if the fixed-versus-scientific tie-break cases are removed or renamed."
    );
    require_text_present(roundtrip, "check_expected(0.001, \"0.001\")", "f64 0.001 corpus should be present");
    require_text_present(roundtrip, "check_expected(-0.001, \"-0.001\")", "f64 -0.001 corpus should be present");
    require_text_present(roundtrip, "check_expected(0.0001, \"1e-04\")", "f64 0.0001 corpus should be present");
    require_text_present(roundtrip, "check_expected(-0.0001, \"-1e-04\")", "f64 -0.0001 corpus should be present");
    require_text_present(roundtrip, "check_expected_f32(0.001f, \"0.001\")", "f32 0.001 corpus should be present");
    require_text_present(roundtrip, "check_expected_f32(-0.001f, \"-0.001\")", "f32 -0.001 corpus should be present");
    require_text_present(roundtrip, "check_expected_f32(0.0001f, \"1e-04\")", "f32 0.0001 corpus should be present");
    require_text_present(roundtrip, "check_expected_f32(-0.0001f, \"-1e-04\")", "f32 -0.0001 corpus should be present");
    require_text_present(corpus, "expect_shortest_case(0.001, \"0.001\")", "f64 0.001 upgrade corpus should be present");
    require_text_present(corpus, "expect_shortest_case(-0.001, \"-0.001\")", "f64 -0.001 upgrade corpus should be present");
    require_text_present(corpus, "expect_shortest_case(0.0001, \"1e-04\")", "f64 0.0001 upgrade corpus should be present");
    require_text_present(corpus, "expect_shortest_case(-0.0001, \"-1e-04\")", "f64 -0.0001 upgrade corpus should be present");
    require_text_present(corpus, "expect_shortest_case_f32(0.001f, \"0.001\")", "f32 0.001 upgrade corpus should be present");
    require_text_present(corpus, "expect_shortest_case_f32(-0.001f, \"-0.001\")", "f32 -0.001 upgrade corpus should be present");
    require_text_present(corpus, "expect_shortest_case_f32(0.0001f, \"1e-04\")", "f32 0.0001 upgrade corpus should be present");
    require_text_present(corpus, "expect_shortest_case_f32(-0.0001f, \"-1e-04\")", "f32 -0.0001 upgrade corpus should be present");

    free(roundtrip);
    free(corpus);
    PROVEN_TEST_PASS("Float shortest tie-break corpus checks passed.");
    return 0;
}