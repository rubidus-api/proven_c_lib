#include "proven_test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_file_text(const char *path) {
    FILE *f = fopen(path, "rb");
    PROVEN_TEST_ASSERT(f != NULL, path, "Inspect the float corpus source file if it cannot be opened.");
    PROVEN_TEST_ASSERT(fseek(f, 0, SEEK_END) == 0, path, "Inspect the float corpus source file if seeking the end fails.");
    long n = ftell(f);
    PROVEN_TEST_ASSERT(n >= 0, path, "Inspect the float corpus source file if its size cannot be measured.");
    PROVEN_TEST_ASSERT(fseek(f, 0, SEEK_SET) == 0, path, "Inspect the float corpus source file if rewinding fails.");
    char *buf = (char *)malloc((size_t)n + 1u);
    PROVEN_TEST_ASSERT(buf != NULL, path, "Inspect the host if the inspection buffer cannot be allocated.");
    size_t got = fread(buf, 1u, (size_t)n, f);
    PROVEN_TEST_ASSERT(got == (size_t)n, path, "Inspect the float corpus source file if reading returns a short count.");
    buf[got] = '\0';
    fclose(f);
    return buf;
}

static void require_text_present(const char *text, const char *needle, const char *label) {
    PROVEN_TEST_ASSERT(strstr(text, needle) != NULL, label, "Inspect the float corpus source if the pinned boundary-neighbor case disappears.");
}

int main(void) {
    PROVEN_TEST_SUITE(
        "test_unit_float_f32_boundaries",
        "Verify the float32 upgrade and shortest corpus pin the ULP-adjacent neighbors around FLT_MIN and FLT_TRUE_MIN.",
        "Inspect tests/test_differential_float_corpus_f64.c or tests/test_unit_float_shortest_roundtrip.c if a float32 boundary-neighbor case disappears."
    );

    char *upgrade = read_file_text("tests/test_differential_float_corpus_f64.c");
    char *roundtrip = read_file_text("tests/test_unit_float_shortest_roundtrip.c");

    PROVEN_TEST_SECTION(
        "float32 ULP-adjacent neighbors",
        "Confirm the corpus pins the values one ULP below FLT_MIN and one ULP above FLT_TRUE_MIN for both the upgrade corpus and the shortest round-trip corpus.",
        "Inspect the float32 corpus if either ULP-adjacent boundary-neighbor case is missing."
    );
    require_text_present(upgrade, "float_from_bits(0x007fffffu)", "upgrade corpus should pin the float32 neighbor below FLT_MIN");
    require_text_present(upgrade, "1.1754942e-38", "upgrade corpus should pin the float32 neighbor below FLT_MIN with its documented shortest spelling");
    require_text_present(upgrade, "float_from_bits(0x00000002u)", "upgrade corpus should pin the float32 neighbor above FLT_TRUE_MIN");
    require_text_present(upgrade, "3e-45", "upgrade corpus should pin the float32 neighbor above FLT_TRUE_MIN with its documented shortest spelling");
    require_text_present(roundtrip, "float_from_bits(0x007fffffu)", "shortest round-trip should pin the float32 neighbor below FLT_MIN");
    require_text_present(roundtrip, "1.1754942e-38", "shortest round-trip should pin the float32 neighbor below FLT_MIN with its documented shortest spelling");
    require_text_present(roundtrip, "float_from_bits(0x00000002u)", "shortest round-trip should pin the float32 neighbor above FLT_TRUE_MIN");
    require_text_present(roundtrip, "3e-45", "shortest round-trip should pin the float32 neighbor above FLT_TRUE_MIN with its documented shortest spelling");

    free(upgrade);
    free(roundtrip);
    PROVEN_TEST_PASS("Float32 boundary-neighbor source contract checks passed.");
    return 0;
}
