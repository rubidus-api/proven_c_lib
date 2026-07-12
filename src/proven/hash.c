#include "proven/hash.h"

/*
 * Contract first, implementation next - docs/TESTING.md §5.1. These stubs let the header
 * compile and the known-answer test link; the test FAILS against them, on purpose, and the
 * commit that implements each algorithm makes it pass without touching what the test asserts.
 */

proven_u64 proven_hash_bytes(proven_mem_view_t data) {
    (void)data;
    return 0;
}

proven_u64 proven_hash_keyed(proven_mem_view_t data, const proven_byte_t key[16]) {
    (void)data; (void)key;
    return 0;
}

proven_u32 proven_crc32(proven_mem_view_t data) {
    (void)data;
    return 0;
}

proven_u32 proven_crc32_update(proven_u32 crc, proven_mem_view_t data) {
    (void)crc; (void)data;
    return 0;
}

void proven_sha256_init(proven_sha256_t *ctx) { (void)ctx; }
void proven_sha256_update(proven_sha256_t *ctx, proven_mem_view_t data) { (void)ctx; (void)data; }
void proven_sha256_final(proven_sha256_t *ctx, proven_byte_t out[PROVEN_SHA256_SIZE]) {
    (void)ctx;
    for (proven_size_t i = 0; i < PROVEN_SHA256_SIZE; ++i) out[i] = 0;
}
void proven_sha256(proven_mem_view_t data, proven_byte_t out[PROVEN_SHA256_SIZE]) {
    (void)data;
    for (proven_size_t i = 0; i < PROVEN_SHA256_SIZE; ++i) out[i] = 0;
}
void proven_sha256_to_hex(const proven_byte_t digest[PROVEN_SHA256_SIZE], char out[65]) {
    (void)digest;
    out[0] = 0;
}
