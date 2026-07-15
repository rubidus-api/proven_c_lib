#include "proven.h"
#include "proven/time.h"
#include "proven_test.h"
#include <stdint.h>
#include <string.h>

/*
 * Timing, not correctness. This measures the throughput of the byte-level primitives added
 * this cycle - the hashes, the encoders, and the two random generators - so that a regression
 * shows up as a number and the benchmark doc has something to report. Every loop folds its
 * output into a checksum so the optimiser cannot delete the work it is supposed to be timing;
 * a checksum drift between runs would be a correctness failure and is printed alongside the ns.
 *
 * These are ns/byte or ns/call on THIS machine, single-threaded, -O3. They are a relative
 * baseline for the same code over time, not a claim about other hardware.
 */

static uint64_t mix(uint64_t h, uint64_t x) {
    h ^= x;
    h *= 0x100000001b3ULL;
    return h;
}

/* A fixed pseudo-random payload, built once, so every backend sees identical bytes. */
static proven_byte_t g_data[4096];
static void fill_payload(void) {
    proven_xoshiro256ss_t g;
    proven_xoshiro256ss_seed(&g, 0xBEEF);
    for (size_t i = 0; i < sizeof g_data; i += 8) {
        proven_u64 v = proven_xoshiro256ss_next(&g);
        for (int b = 0; b < 8 && i + (size_t)b < sizeof g_data; ++b)
            g_data[i + b] = (proven_byte_t)(v >> (8 * b));
    }
}

static void bench_throughput(const char *label, uint64_t (*run)(proven_mem_view_t), proven_size_t nbytes, size_t rounds) {
    proven_mem_view_t v = { g_data, nbytes };
    uint64_t checksum = 0xcbf29ce484222325ULL;
    proven_time_t start = proven_time_now();
    for (size_t r = 0; r < rounds; ++r) checksum = mix(checksum, run(v));
    proven_time_t end = proven_time_now();

    proven_i64 ns = end - start; if (ns <= 0) ns = 1;
    double per_byte = (double)ns / ((double)nbytes * (double)rounds);
    double mb_s = 1000.0 / per_byte;   /* bytes/ns == GB/s; *1000 -> MB/s per ns/byte inverse */
    PROVEN_TEST_INFO("backend={} bytes={} rounds={} ns_per_byte={} MB_per_s={} checksum={}",
                     PROVEN_ARG(label), PROVEN_ARG((unsigned long long)nbytes),
                     PROVEN_ARG((unsigned long long)rounds),
                     PROVEN_ARG(per_byte), PROVEN_ARG(mb_s),
                     PROVEN_ARG((unsigned long long)checksum));
}

/* ---- hash backends ---- */
static uint64_t run_fnv(proven_mem_view_t v)   { return proven_hash_bytes(v); }
static uint64_t run_crc32(proven_mem_view_t v) { return proven_crc32(v); }
static proven_byte_t g_key[16];
static uint64_t run_siphash(proven_mem_view_t v){ return proven_hash_keyed(v, g_key); }
static uint64_t run_sha256(proven_mem_view_t v) {
    proven_byte_t d[PROVEN_SHA256_SIZE]; proven_sha256(v, d);
    uint64_t x = 0; memcpy(&x, d, 8); return x;
}

/* ---- encode backends ---- */
static proven_byte_t g_enc[8192];
static uint64_t run_hex(proven_mem_view_t v) {
    proven_size_t w = 0; (void)proven_hex_encode(v, g_enc, sizeof g_enc, &w); return w ? g_enc[w-1] : 0;
}
static uint64_t run_b64(proven_mem_view_t v) {
    proven_size_t w = 0; (void)proven_base64_encode(v, g_enc, sizeof g_enc, &w); return w ? g_enc[w-1] : 0;
}

int main(void) {
    PROVEN_TEST_SUITE("primitive throughput benchmark",
        "Timing of the hashes, encoders and generators added this cycle. ns/byte and ns/call on this machine, -O3, single-threaded - a baseline for the same code over time, not a cross-hardware claim.",
        "If a checksum drifts between runs the backend changed behaviour; if a timing regresses, inspect the module named by the backend label.");

    fill_payload();
    proven_xoshiro256ss_t kg; proven_xoshiro256ss_seed(&kg, 1);
    for (int i = 0; i < 16; ++i) g_key[i] = (proven_byte_t)proven_xoshiro256ss_next(&kg);

    const proven_size_t N = 4096;
    const size_t R = 200000;

    PROVEN_TEST_SECTION("hashes over a 4 KiB buffer", "FNV-1a, CRC-32, SipHash-2-4, SHA-256.", "");
    bench_throughput("hash_fnv1a",   run_fnv,     N, R);
    bench_throughput("hash_crc32",   run_crc32,   N, R);
    bench_throughput("hash_siphash", run_siphash, N, R);
    bench_throughput("hash_sha256",  run_sha256,  N, R);

    PROVEN_TEST_SECTION("encoders over a 4 KiB buffer", "hex and standard Base64.", "");
    bench_throughput("encode_hex",    run_hex, N, R);
    bench_throughput("encode_base64", run_b64, N, R);

    PROVEN_TEST_SECTION("random generators: bulk fill throughput",
        "xoshiro256** (fast, reproducible) vs ChaCha20 (cryptographic).", "");
    {
        proven_xoshiro256ss_t x; proven_xoshiro256ss_seed(&x, 7);
        proven_rng_t xr = proven_xoshiro256ss_rng(&x);
        uint64_t cs = 0; const size_t R2 = 100000;
        proven_time_t s = proven_time_now();
        for (size_t r = 0; r < R2; ++r) { proven_rng_fill(xr, g_data, N); cs = mix(cs, g_data[N-1]); }
        proven_time_t e = proven_time_now(); proven_i64 ns = e - s; if (ns<=0) ns=1;
        PROVEN_TEST_INFO("backend={} bytes={} rounds={} ns_per_byte={} MB_per_s={} checksum={}",
            PROVEN_ARG("random_xoshiro"), PROVEN_ARG((unsigned long long)N), PROVEN_ARG((unsigned long long)R2),
            PROVEN_ARG((double)ns/((double)N*(double)R2)), PROVEN_ARG(1000.0/((double)ns/((double)N*(double)R2))),
            PROVEN_ARG((unsigned long long)cs));

        proven_chacha_rng_t c; proven_byte_t seed[32]; memset(seed, 0x42, sizeof seed);
        proven_chacha_rng_seed(&c, seed);
        cs = 0; s = proven_time_now();
        for (size_t r = 0; r < R2; ++r) { proven_chacha_rng_fill(&c, g_data, N); cs = mix(cs, g_data[N-1]); }
        e = proven_time_now(); ns = e - s; if (ns<=0) ns=1;
        PROVEN_TEST_INFO("backend={} bytes={} rounds={} ns_per_byte={} MB_per_s={} checksum={}",
            PROVEN_ARG("random_chacha20"), PROVEN_ARG((unsigned long long)N), PROVEN_ARG((unsigned long long)R2),
            PROVEN_ARG((double)ns/((double)N*(double)R2)), PROVEN_ARG(1000.0/((double)ns/((double)N*(double)R2))),
            PROVEN_ARG((unsigned long long)cs));
    }

    PROVEN_TEST_PASS("primitive throughput measured.");
    return 0;
}
