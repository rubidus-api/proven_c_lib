#include "proven/random.h"

#ifndef PROVEN_FREESTANDING
#include "../../platform/proven_sys_random.h"
#endif

/* ------------------------------------------------------------------
 * STUBS. The contract lives in include/proven/random.h and the test that holds it to that
 * contract is tests/test_unit_rng.c. Neither the generators nor the helpers are implemented
 * yet: this file exists so the test compiles and links, and FAILS - which is the point of the
 * commit it lands in (docs/TESTING.md §5.1).
 * ------------------------------------------------------------------ */

proven_u64 proven_rng_u64(proven_rng_t rng) {
    (void)rng;
    return 0;
}

void proven_rng_fill(proven_rng_t rng, void *buf, proven_size_t len) {
    (void)rng; (void)buf; (void)len;
}

void proven_xoshiro256ss_seed(proven_xoshiro256ss_t *g, proven_u64 seed) {
    (void)seed;
    if (g) { g->s[0] = 0; g->s[1] = 0; g->s[2] = 0; g->s[3] = 0; }
}

proven_u64 proven_xoshiro256ss_next(proven_xoshiro256ss_t *g) {
    (void)g;
    return 0;
}

proven_rng_t proven_xoshiro256ss_rng(proven_xoshiro256ss_t *g) {
    return (proven_rng_t){ .vt = NULL, .ctx = g };
}

void proven_chacha_rng_seed(proven_chacha_rng_t *g, const proven_byte_t seed[PROVEN_CHACHA_SEED_SIZE]) {
    (void)seed;
    if (g) { g->used = 64; }
}

proven_u64 proven_chacha_rng_next(proven_chacha_rng_t *g) {
    (void)g;
    return 0;
}

void proven_chacha_rng_fill(proven_chacha_rng_t *g, void *buf, proven_size_t len) {
    (void)g; (void)buf; (void)len;
}

proven_rng_t proven_chacha_rng(proven_chacha_rng_t *g) {
    return (proven_rng_t){ .vt = NULL, .ctx = g };
}

proven_u64 proven_rng_below(proven_rng_t rng, proven_u64 bound) {
    (void)rng; (void)bound;
    return 0;
}

proven_i64 proven_rng_range(proven_rng_t rng, proven_i64 lo, proven_i64 hi) {
    (void)rng; (void)hi;
    return lo;
}

double proven_rng_f64(proven_rng_t rng) {
    (void)rng;
    return 0.0;
}

void proven_rng_shuffle(proven_rng_t rng, void *base, proven_size_t count, proven_size_t elem_size) {
    (void)rng; (void)base; (void)count; (void)elem_size;
}

#ifndef PROVEN_FREESTANDING

bool proven_random_bytes(void *buf, proven_size_t len) {
    return proven_sys_random_bytes(buf, len);
}

proven_u64 proven_random_u64(void) {
    proven_u64 v = 0;
    if (!proven_random_bytes(&v, sizeof v)) return 0;
    return v;
}

bool proven_chacha_rng_seed_from_os(proven_chacha_rng_t *g) {
    (void)g;
    return false;
}

#endif /* !PROVEN_FREESTANDING */
