#ifndef PROVEN_TEST_DOCS_TLS_IDENTITY_H
#define PROVEN_TEST_DOCS_TLS_IDENTITY_H

/*
 * For the two chapter 14 examples, which take a certificate and a key as PEM files (v0.78.0):
 * an Ed25519 identity for example.test and 127.0.0.1, valid an hour either side of now, issued
 * with the library's internal issuer and written to two files in the test's own directory. The
 * repository holds no key; this is how the examples stay run by the suite.
 */
#include "../src/proven/proven_internal_tls.h"

static bool tls_identity_files(const char *cert_path, const char *key_path) {
    proven_allocator_t heap = proven_heap_allocator();
    proven_u8str_view_t names[2] = { PROVEN_LIT("example.test"), PROVEN_LIT("127.0.0.1") };
    const proven_i64 now = proven_time_now() / 1000000000;
    proven_byte_t cert_pem[1024], key_pem[256];
    proven_size_t cert_len = 0, key_len = 0;
    bool ok = proven_tls_self_signed_(names, 2, now - 3600, now + 3600, NULL, NULL, (proven_mem_mut_t){ cert_pem, sizeof cert_pem }, &cert_len,
                                      (proven_mem_mut_t){ key_pem, sizeof key_pem }, &key_len) == PROVEN_OK &&
              proven_fs_write_file(heap, proven_u8str_view_from_cstr(cert_path), (proven_mem_view_t){ cert_pem, cert_len }) == PROVEN_OK &&
              proven_fs_write_file(heap, proven_u8str_view_from_cstr(key_path), (proven_mem_view_t){ key_pem, key_len }) == PROVEN_OK;
    proven_mem_wipe((proven_mem_mut_t){ key_pem, sizeof key_pem });
    return ok;
}

static void tls_identity_remove(const char *cert_path, const char *key_path) {
    proven_allocator_t heap = proven_heap_allocator();
    (void)proven_fs_remove(heap, proven_u8str_view_from_cstr(cert_path));
    (void)proven_fs_remove(heap, proven_u8str_view_from_cstr(key_path));
}

#endif
