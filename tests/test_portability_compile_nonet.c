#include "proven_test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32) || defined(_WIN64)
int main(void) {
    PROVEN_TEST_SUITE("test_portability_compile_nonet", "POSIX host only: the fixture runs the host compiler through /bin/sh.", "");
    PROVEN_TEST_PASS("Skipped on Windows: the fixture needs a POSIX shell (B-035).");
    return 0;
}
#else

/*
 * PROVEN_NO_NET is a promise to a consumer who wants no sockets: the header is not pulled in by
 * proven.h, the two socket sources compile to nothing, and so no socket symbol is linked.
 *
 * The suite itself is built without that macro, so nothing else here could notice the promise
 * breaking. This test asks the host compiler directly: every library source must compile with
 * -DPROVEN_NO_NET, a program that includes proven.h and the alias header must link against the
 * result, and that program must contain no socket code.
 */

static int run(const char *cmd) {
    int rc = system(cmd);
    PROVEN_TEST_ASSERT(rc != -1, "launch the host compiler", "Check that /bin/sh and a C compiler are available.");
    return rc;
}

int main(void) {
    PROVEN_TEST_SUITE("test_portability_compile_nonet",
        "A hosted build with -DPROVEN_NO_NET compiles every source, links, and contains no socket code.",
        "Inspect the PROVEN_NO_NET guards in include/proven.h, src/proven/net.c and platform/proven_sys_net.c.");

    const char *dir = "build/test_portability_compile_nonet";
    char cmd[4096];

    snprintf(cmd, sizeof cmd, "rm -rf %s && mkdir -p %s", dir, dir);
    PROVEN_TEST_ASSERT(run(cmd) == 0, "a clean scratch directory", "");

    /* -std=c23 where the compiler has it, the transitional spelling where it does not - the
     * same fallback the build driver makes. */
    snprintf(cmd, sizeof cmd,
        "std=-std=c23; ${CC:-cc} $std -fsyntax-only -x c /dev/null 2>/dev/null || std=-std=c2x; "
        "for f in $(sed -n 's/^PROVEN_BUILD_SOURCE(\"\\([^\"]*\\)\".*/\\1/p' build_sources.inc); do "
        "  ${CC:-cc} $std -Wall -Wextra -Werror -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L -DPROVEN_NO_NET "
        "    -I./include -I./platform -c \"$f\" -o %s/$(echo \"$f\" | tr '/' '_').o || exit 1; "
        "done; echo $std > %s/std.txt", dir, dir);
    PROVEN_TEST_ASSERT(run(cmd) == 0,
        "every library source compiles with -DPROVEN_NO_NET",
        "A source that needs net.h when the macro is set: guard the use, or the include.");

    snprintf(cmd, sizeof cmd,
        "printf '%%s\\n' '#include \"proven.h\"' '#include \"proven/alias_xcv.h\"' "
        "'#ifdef PROVEN_NET_H' '#error \"net.h must not be included under PROVEN_NO_NET\"' '#endif' "
        "'int main(void) { return proven_time_monotonic_now() >= 0 ? 0 : 1; }' > %s/main.c && "
        "${CC:-cc} $(cat %s/std.txt) -Wall -Wextra -Werror -D_DEFAULT_SOURCE -DPROVEN_NO_NET -I./include -I./platform "
        "  %s/main.c %s/*.o -pthread -o %s/nonet && %s/nonet",
        dir, dir, dir, dir, dir, dir);
    PROVEN_TEST_ASSERT(run(cmd) == 0,
        "a program that includes proven.h links against that build and runs, with net.h not included",
        "The umbrella header pulled net.h in, or something outside the two socket sources references a socket function.");

    /* The object files, not the linked program: a static libc may carry getaddrinfo for its own
     * reasons, and that would say nothing about this library. */
    snprintf(cmd, sizeof cmd,
        "! (nm %s/*.o 2>/dev/null | grep -E ' [TU] (proven_net_|proven_sys_net_|proven_transport_|proven_http_client_|proven_http_server_|proven_http_exchange_|getaddrinfo|socket|connect|accept|recv|send)' >/dev/null)", dir);
    PROVEN_TEST_ASSERT(run(cmd) == 0,
        "no object of that build defines or references a socket function",
        "List them with: nm build/test_portability_compile_nonet/*.o | grep -E 'proven_net_|socket'");

    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    (void)run(cmd);

    PROVEN_TEST_PASS("PROVEN_NO_NET leaves the socket layer out of a hosted build.");
    return 0;
}
#endif
