#include "proven_test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_file_text(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long len = ftell(f);
    if (len < 0) {
        fclose(f);
        return NULL;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    char *text = (char *)malloc((size_t)len + 1u);
    if (!text) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(text, 1, (size_t)len, f);
    fclose(f);
    if (got != (size_t)len) {
        free(text);
        return NULL;
    }
    text[len] = '\0';
    return text;
}

static void require_text_present(const char *text, const char *needle, const char *label) {
    PROVEN_TEST_ASSERT(strstr(text, needle) != NULL, label, "Inspect tests/test_float_upgrade_corpus.c if the float32 shortest corpus coverage disappears.");
}

int main(void) {
    PROVEN_TEST_SUITE(
        "test_float_upgrade_corpus_f32",
        "Verify the float upgrade corpus includes representative float32 shortest spellings in addition to the existing float64 cases.",
        "Inspect tests/test_float_upgrade_corpus.c if the float32 coverage section disappears or drifts from the documented literals."
    );

    char *src = read_file_text("tests/test_float_upgrade_corpus.c");
    PROVEN_TEST_ASSERT(src != NULL, "float upgrade corpus source should be readable", "Inspect the test file path if the corpus source cannot be opened.");

    PROVEN_TEST_SECTION(
        "float32 shortest corpus source contract",
        "Confirm the upgrade corpus source records the float32 special-case literals that the shared shortest helper expects.",
        "Inspect tests/test_float_upgrade_corpus.c if any documented float32 literal is missing from the corpus section."
    );
    require_text_present(src, "float32 shortest corpus", "float32 shortest corpus section should be present");
    require_text_present(src, "expect_shortest_case_f32(FLT_MIN, \"1.1754944e-38\")", "FLT_MIN shortest literal should be present");
    require_text_present(src, "expect_shortest_case_f32(-FLT_MIN, \"-1.1754944e-38\")", "-FLT_MIN shortest literal should be present");
    require_text_present(src, "expect_shortest_case_f32(0.2f, \"0.2\")", "0.2f shortest literal should be present");
    require_text_present(src, "expect_shortest_case_f32(-0.2f, \"-0.2\")", "-0.2f shortest literal should be present");
    require_text_present(src, "expect_shortest_case_f32(0.29999998f, \"0.29999998\")", "0.29999998f shortest literal should be present");
    require_text_present(src, "expect_shortest_case_f32(-0.29999998f, \"-0.29999998\")", "-0.29999998f shortest literal should be present");
    require_text_present(src, "expect_shortest_case_f32(1.0000002f, \"1.0000002\")", "1.0000002f shortest literal should be present");
    require_text_present(src, "expect_shortest_case_f32(-1.0000002f, \"-1.0000002\")", "-1.0000002f shortest literal should be present");
    require_text_present(src, "expect_shortest_case_f32(2.5f, \"2.5\")", "2.5f shortest literal should be present");
    require_text_present(src, "expect_shortest_case_f32(-2.5f, \"-2.5\")", "-2.5f shortest literal should be present");
    require_text_present(src, "expect_shortest_case_f32(33554432.0f, \"33554432\")", "33554432.0f shortest literal should be present");
    require_text_present(src, "expect_shortest_case_f32(-33554432.0f, \"-33554432\")", "-33554432.0f shortest literal should be present");
    require_text_present(src, "expect_shortest_case_f32(FLT_TRUE_MIN, \"1e-45\")", "FLT_TRUE_MIN shortest literal should be present");
    require_text_present(src, "expect_shortest_case_f32(-FLT_TRUE_MIN, \"-1e-45\")", "-FLT_TRUE_MIN shortest literal should be present");
    require_text_present(src, "expect_shortest_case_f32(FLT_MAX, \"3.4028235e38\")", "FLT_MAX shortest literal should be present");
    require_text_present(src, "expect_shortest_case_f32(-FLT_MAX, \"-3.4028235e38\")", "-FLT_MAX shortest literal should be present");

    free(src);
    PROVEN_TEST_PASS("Float32 upgrade corpus source contract checks passed.");
    return 0;
}
