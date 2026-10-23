/*
 * manual/examples/en/ex_14_tls_engine.c, run as the suite runs every example - but with the
 * certificate and key it now takes as arguments (v0.78.0), issued here. Run with none, it prints
 * how to make them and succeeds. The Korean edition's code is the same (check-example-parity).
 */
#define main ex_14_tls_engine_main
#include "../manual/examples/en/ex_14_tls_engine.c"
#undef main
#include "proven_test.h"
#include "test_docs_tls_identity.h"

int main(void) {
    PROVEN_TEST_SUITE("chapter 14's engine example, given an identity",
        "The example reads a certificate and key from the files it is given and passes every check; given none, it says how to make them and exits 0.",
        "Check manual/examples/en/ex_14_tls_engine.c first; if it fails only here, tests/test_docs_tls_identity.h.");
    char prog[] = "ex_14_tls_engine", cert[] = "ex14_engine_cert.pem", key[] = "ex14_engine_key.pem";
    char *none[] = { prog, NULL };
    PROVEN_TEST_ASSERT(ex_14_tls_engine_main(1, none) == 0, "with no files it explains, and succeeds", "");
    PROVEN_TEST_ASSERT(tls_identity_files(cert, key), "an identity for example.test and 127.0.0.1, written to two files", "");
    char *args[] = { prog, cert, key, NULL };
    const int r = ex_14_tls_engine_main(3, args);
    tls_identity_remove(cert, key);
    PROVEN_TEST_ASSERT(r == 0, "given them, every check of the example passes", "");
    PROVEN_TEST_PASS("chapter 14's engine example runs with an identity from files.");
    return 0;
}
