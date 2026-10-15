#include "proven_test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32) || defined(_WIN64)
int main(void) {
    PROVEN_TEST_SUITE("test_portability_compile_nobrotli", "POSIX host only: the fixture runs the host compiler through /bin/sh.", "");
    PROVEN_TEST_PASS("Skipped on Windows: the fixture needs a POSIX shell (B-035).");
    return 0;
}
#else

/*
 * PROVEN_NO_BROTLI is a promise about size: the Brotli decoder and its 122,784-byte dictionary
 * are left out, proven.h does not include brotli.h, and an HTTP client asked to decode Brotli
 * says the build cannot (PROVEN_ERR_UNSUPPORTED).
 *
 * The suite itself is built without that macro, so nothing else here could notice the promise
 * breaking. This test asks the host compiler directly: every library source must compile with
 * -DPROVEN_NO_BROTLI, a program that includes proven.h and the alias header must link against
 * the result and run, and no object may define a Brotli symbol.
 */

static int run(const char *cmd) {
    int rc = system(cmd);
    PROVEN_TEST_ASSERT(rc != -1, "launch the host compiler", "Check that /bin/sh and a C compiler are available.");
    return rc;
}

int main(void) {
    PROVEN_TEST_SUITE("test_portability_compile_nobrotli",
        "A hosted build with -DPROVEN_NO_BROTLI compiles every source, links, runs, and contains no Brotli code or dictionary.",
        "Inspect the PROVEN_NO_BROTLI guards in include/proven.h, src/proven/brotli.c and src/proven/http_coding.c.");

    const char *dir = "build/test_portability_compile_nobrotli";
    char cmd[4096];

    snprintf(cmd, sizeof cmd, "rm -rf %s && mkdir -p %s", dir, dir);
    PROVEN_TEST_ASSERT(run(cmd) == 0, "a clean scratch directory", "");

    /* -std=c23 where the compiler has it, the transitional spelling where it does not - the
     * same fallback the build driver makes. */
    snprintf(cmd, sizeof cmd,
        "std=-std=c23; ${CC:-cc} $std -fsyntax-only -x c /dev/null 2>/dev/null || std=-std=c2x; "
        "for f in $(sed -n 's/^PROVEN_BUILD_SOURCE(\"\\([^\"]*\\)\".*/\\1/p' build_sources.inc); do "
        "  ${CC:-cc} $std -Wall -Wextra -Werror -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L -DPROVEN_NO_BROTLI "
        "    -I./include -I./platform -c \"$f\" -o %s/$(echo \"$f\" | tr '/' '_').o || exit 1; "
        "done; echo $std > %s/std.txt", dir, dir);
    PROVEN_TEST_ASSERT(run(cmd) == 0,
        "every library source compiles with -DPROVEN_NO_BROTLI",
        "A source that needs brotli.h when the macro is set: guard the use, or the include.");

    snprintf(cmd, sizeof cmd,
        "printf '%%s\\n' '#include \"proven.h\"' '#include \"proven/alias_xcv.h\"' "
        "'#ifdef PROVEN_BROTLI_H' '#error \"brotli.h must not be included under PROVEN_NO_BROTLI\"' '#endif' "
        "'int main(void) {' "
        "'    proven_http_client_config_t c = { .alloc = proven_heap_allocator(), .decompress = true, .decompress_brotli = true };' "
        "'    proven_http_client_t *client = NULL;' "
        "'    return proven_http_client_create(&c, &client) == PROVEN_ERR_UNSUPPORTED && client == NULL ? 0 : 1;' "
        "'}' > %s/main.c && "
        "${CC:-cc} $(cat %s/std.txt) -Wall -Wextra -Werror -D_DEFAULT_SOURCE -DPROVEN_NO_BROTLI -I./include -I./platform "
        "  %s/main.c %s/*.o -pthread -o %s/nobrotli && %s/nobrotli",
        dir, dir, dir, dir, dir, dir);
    PROVEN_TEST_ASSERT(run(cmd) == 0,
        "a program that includes proven.h links against that build and runs, with brotli.h not included, and a client asked to decode Brotli is PROVEN_ERR_UNSUPPORTED",
        "The umbrella header pulled brotli.h in, something outside brotli.c references a Brotli function, or proven_http_decoder_cfg_check_ lets the switch through.");

    snprintf(cmd, sizeof cmd,
        "! (nm %s/*.o 2>/dev/null | grep -E ' [TtUDdRr] (proven_brotli_|brotli_)' >/dev/null)", dir);
    PROVEN_TEST_ASSERT(run(cmd) == 0,
        "no object of that build defines or references a Brotli function or the dictionary",
        "List them with: nm build/test_portability_compile_nobrotli/*.o | grep -i brotli");

    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    (void)run(cmd);

    PROVEN_TEST_PASS("PROVEN_NO_BROTLI leaves the Brotli decoder and its dictionary out of a hosted build.");
    return 0;
}
#endif
