#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include "proven_test.h"

static void require_impl(bool cond, const char *cond_text, const char *msg, const char *file, int line) {
    if (!cond) {
        fprintf(stderr, "\n[PROVEN][CHECK][FAIL] file=%s line=%d\n", file, line);
        fprintf(stderr, "[PROVEN][CHECK][COND] %s\n", cond_text);
        fprintf(stderr, "[PROVEN][CHECK][INTENT] %s\n", msg);
        fprintf(stderr, "[PROVEN][CHECK][FAIL_HINT] Inspect the float scaffold files under src/proven and keep the shared decimal helpers isolated from fmt.c and scan.c.\n");
        exit(1);
    }
}

#define require(cond, msg) require_impl((cond), #cond, (msg), __FILE__, __LINE__)

static char *read_text_file(const char *path) {
    FILE *f = fopen(path, "rb");
    require(f != NULL, path);
    require(fseek(f, 0, SEEK_END) == 0, "seek end");
    long n = ftell(f);
    require(n >= 0, "tell");
    require(fseek(f, 0, SEEK_SET) == 0, "seek set");
    char *buf = (char*)malloc((size_t)n + 1u);
    require(buf != NULL, "malloc");
    size_t got = fread(buf, 1u, (size_t)n, f);
    require(got == (size_t)n, "read full file");
    buf[got] = '\0';
    fclose(f);
    return buf;
}

static bool contains(const char *haystack, const char *needle) {
    return strstr(haystack, needle) != NULL;
}

int main(void) {
    PROVEN_TEST_SUITE(
        "test_float_module_scaffold",
        "Verify the float helper code has been split into a shared internal module and the build driver knows about the new source file.",
        "Inspect src/proven/float_decimal.c, src/proven/float_decimal.h, src/proven/fmt.c, src/proven/scan.c, and nob.c if the shared helper scaffold goes missing or is re-embedded inline."
    );

    char *mod_h = read_text_file("src/proven/float_decimal.h");
    require(contains(mod_h, "proven_float_scale_pow10"), "float_decimal.h should declare the shared pow10 scaling helper");
    require(contains(mod_h, "proven_float_convert_decimal"), "float_decimal.h should declare the shared decimal-to-double helper");
    require(contains(mod_h, "proven_float_normalize_scientific"), "float_decimal.h should declare the shared scientific normalization helper");
    free(mod_h);

    char *mod_c = read_text_file("src/proven/float_decimal.c");
    require(contains(mod_c, "proven_float_scale_pow10"), "float_decimal.c should define the shared pow10 scaling helper");
    require(contains(mod_c, "proven_float_convert_decimal"), "float_decimal.c should define the shared decimal-to-double helper");
    require(contains(mod_c, "proven_float_normalize_scientific"), "float_decimal.c should define the shared scientific normalization helper");
    require(contains(mod_c, "proven_float_shortest_literal_common"), "float_decimal.c should define the shared shortest-literal helper");
    require(contains(mod_c, "proven_float_shortest_literal_f64"), "float_decimal.c should define the shared f64 shortest-literal wrapper");
    require(contains(mod_c, "proven_float_shortest_literal_f32"), "float_decimal.c should define the shared f32 shortest-literal wrapper");
    free(mod_c);

    char *scan = read_text_file("src/proven/scan.c");
    require(contains(scan, "#include \"float_decimal.h\""), "scan.c should include the float helper header instead of carrying the shared helpers inline");
    require(!contains(scan, "static double proven_scan_scale_pow10("), "scan.c should not define the shared pow10 scaling helper inline");
    require(!contains(scan, "static double proven_scan_convert_decimal("), "scan.c should not define the shared decimal conversion helper inline");
    free(scan);

    char *fmt = read_text_file("src/proven/float_format.c");
    require(contains(fmt, "#include \"float_decimal.h\""), "float_format.c should include the float helper header instead of carrying the shared helpers inline");
    require(contains(fmt, "proven_float_shortest_literal_f64"), "float_format.c should call the shared f64 shortest-literal helper");
    require(contains(fmt, "proven_float_shortest_literal_f32"), "float_format.c should call the shared f32 shortest-literal helper");
    require(!contains(fmt, "0x7ff0000000000000ULL"), "float_format.c should not carry the f64 shortest special-case literal table inline");
    require(!contains(fmt, "0x7f800000u"), "float_format.c should not carry the f32 shortest special-case literal table inline");
    free(fmt);

    char *nob = read_text_file("nob.c");
    require(contains(nob, "src/proven/float_decimal.c"), "nob.c should compile the shared float helper translation unit");
    free(nob);

    PROVEN_TEST_PASS("Float scaffold checks passed.");
    return 0;
}
