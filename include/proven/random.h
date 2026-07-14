#ifndef PROVEN_RANDOM_H
#define PROVEN_RANDOM_H

/**
 * @file random.h
 * @brief Randomness, by use case — and the one question you have to answer first.
 *
 * There is no single "random". There are two jobs that look identical and are not, and
 * choosing the wrong one is the whole danger:
 *
 * | Your job | Use | Why |
 * |---|---|---|
 * | A key, a token, a nonce — anything an attacker must not guess. | `proven_random_bytes`, or a `proven_chacha_rng_t` seeded from it. | Only a cryptographic source is unguessable. |
 * | The same, on a target with no OS. | `proven_chacha_rng_t`, seeded from the board's real entropy. | ChaCha20 is pure arithmetic; it needs no OS. It is only as unguessable as its seed. |
 * | A simulation, a test, a game, a load generator, a sample. | `proven_xoshiro256ss_t`. | Fast, and REPRODUCIBLE: the same seed replays the same run, which is what makes a failing test debuggable. |
 * | A number in a range, a shuffle, a float in [0,1). | `proven_rng_below`, `proven_rng_shuffle`, `proven_rng_f64`, over any source. | `% n` is biased, and everyone writes it anyway. These are not. |
 *
 * The rule, stated once: **`proven_xoshiro256ss_t` is not secret-grade and must never be used
 * as one.** It is fast *because* it is predictable — a few of its outputs reveal its entire
 * state, and therefore every number it will ever produce. That is a feature for a
 * reproducible simulation and a catastrophe for a session token. The two are given names that
 * cannot be confused, so the choice is visible at the call site rather than buried in a seed.
 *
 * @note This module used to say, in this header, that it deliberately offered no
 *       pseudo-random generator at all — that a fast PRNG and a secure one are different
 *       tools and shipping one under a name suggesting the other is how insecure tokens get
 *       written. That reasoning was right and is why the split above is explicit; the
 *       conclusion (offer nothing) was wrong, because a caller who needs a reproducible
 *       sequence does not stop needing one — they write `rand()`, or a hand-rolled LCG, and
 *       get something worse than what the library declined to give them. The answer is to
 *       offer both and make the distinction impossible to miss, not to offer neither.
 *
 * ## The trait
 *
 * `proven_rng_t` is a source of random bytes, and it is **infallible**: once you hold one,
 * drawing from it cannot fail. That is not a simplification — it is where the failure went.
 * Asking an operating system for entropy CAN fail, so that failure is confined to exactly one
 * place: seeding. You check it once, at startup, and every draw downstream is total.
 *
 * ## Freestanding
 *
 * `proven_xoshiro256ss_t`, `proven_chacha_rng_t` and every helper here are pure arithmetic and
 * work on a bare-metal target. Only the OS entropy source (`proven_random_bytes`,
 * `proven_random_u64`, `proven_chacha_rng_seed_from_os`) is hosted: a board with no OS has no
 * `getrandom`, and this module will not paper over that with a clock-seeded PRNG pretending to
 * be a CSPRNG.
 */

#include "types.h"

// -------------------------------------------------------------
// The trait: an infallible source of random bytes
// -------------------------------------------------------------

typedef struct proven_rng_vtable_t {
    proven_u64 (*next_u64)(void *ctx);
    void (*fill)(void *ctx, void *buf, proven_size_t len);
} proven_rng_vtable_t;

/**
 * @brief A source of random bytes. Drawing from a valid one cannot fail.
 *
 * Two pointers; hold it by value. The context it points at is yours, and must outlive it.
 */
typedef struct {
    const proven_rng_vtable_t *vt;
    void *ctx;
} proven_rng_t;

[[nodiscard]]
static inline bool proven_rng_is_valid(proven_rng_t rng) {
    return rng.vt != NULL && rng.vt->next_u64 != NULL && rng.vt->fill != NULL && rng.ctx != NULL;
}

/** @brief One uniformly random 64-bit word. Returns 0 for an invalid source. */
[[nodiscard]]
proven_u64 proven_rng_u64(proven_rng_t rng);

/**
 * @brief Fill `len` bytes of `buf`. A no-op for `len == 0`, a NULL `buf`, or an invalid source.
 * @note Every byte is written; there is no short fill.
 */
void proven_rng_fill(proven_rng_t rng, void *buf, proven_size_t len);

// -------------------------------------------------------------
// Fast and reproducible: xoshiro256** — NOT for secrets
// -------------------------------------------------------------

/**
 * @brief A fast, reproducible generator. Never use it for anything that must not be guessed.
 *
 * xoshiro256** (Blackman & Vigna, public domain): 256 bits of state, period 2^256-1, and
 * output that passes the usual statistical batteries — which says nothing about whether an
 * adversary can predict it. They can; the state is recoverable from the output.
 */
typedef struct {
    proven_u64 s[4];
} proven_xoshiro256ss_t;

/**
 * @brief Seed from one 64-bit value. The same seed always replays the same sequence.
 *
 * @note The seed is expanded through SplitMix64, so a seed of 0 — or 1, 2, 3, which is what
 *       callers actually pass — still produces a well-distributed 256-bit state. Writing a
 *       small counter straight into the state words is the classic way to get a generator
 *       whose first outputs are visibly not random, and an all-zero state it can never leave.
 */
void proven_xoshiro256ss_seed(proven_xoshiro256ss_t *g, proven_u64 seed);

/** @brief The next word. The hot path: call this directly rather than through the trait. */
proven_u64 proven_xoshiro256ss_next(proven_xoshiro256ss_t *g);

/** @brief View this generator as a `proven_rng_t`, for the helpers below. */
[[nodiscard]]
proven_rng_t proven_xoshiro256ss_rng(proven_xoshiro256ss_t *g);

// -------------------------------------------------------------
// Cryptographic, and OS-free once seeded: ChaCha20
// -------------------------------------------------------------

/** @brief The seed a ChaCha generator takes: 256 bits, and they must be real entropy. */
#define PROVEN_CHACHA_SEED_SIZE 32

/**
 * @brief A cryptographic generator that runs with no operating system.
 *
 * ChaCha20 (Bernstein; RFC 8439) run as a stream — the keystream *is* the random data. Its
 * output is unguessable without the key, so unlike xoshiro it is safe for keys and tokens;
 * and it needs nothing from the OS once seeded, which is what makes it the answer on a
 * bare-metal target and the fast answer for bulk random data on a hosted one (no syscall per
 * draw).
 *
 * @note It is exactly as unguessable as its seed. Seed it from `proven_chacha_rng_seed_from_os`
 *       on a hosted target, or from a hardware entropy source on a board. Seeding it from the
 *       clock, a serial number, or an uninitialised buffer yields something that looks random
 *       and is not — which is worse than an obvious failure, because nothing reports it.
 */
typedef struct {
    proven_u32 state[16];
    proven_byte_t block[64];
    proven_size_t used;   /* bytes of `block` already handed out; 64 == "generate the next one" */

    /* Set by seeding, and by nothing else. A zero-initialised struct - which is the shape of
     * "never seeded" - must not be able to masquerade as a generator holding a fresh block, so
     * "is this usable" cannot be inferred from `used` alone. An unseeded generator, or one
     * whose seeding failed, is INERT: its trait is invalid, its next() is 0, and its fill()
     * writes zeros rather than whatever the stack was holding. */
    proven_u32 seeded;
} proven_chacha_rng_t;

/** @brief Seed from 32 bytes of real entropy. The same seed replays the same stream. */
void proven_chacha_rng_seed(proven_chacha_rng_t *g, const proven_byte_t seed[PROVEN_CHACHA_SEED_SIZE]);

/** @brief The next word of keystream. */
proven_u64 proven_chacha_rng_next(proven_chacha_rng_t *g);

/** @brief Fill `len` bytes. Faster than `next` in a loop: it copies whole 64-byte blocks. */
void proven_chacha_rng_fill(proven_chacha_rng_t *g, void *buf, proven_size_t len);

/** @brief View this generator as a `proven_rng_t`, for the helpers below. */
[[nodiscard]]
proven_rng_t proven_chacha_rng(proven_chacha_rng_t *g);

// -------------------------------------------------------------
// Uniform helpers, over any source
// -------------------------------------------------------------

/**
 * @brief A uniformly random value in [0, bound). Returns 0 when `bound` is 0.
 *
 * `proven_rng_u64(rng) % bound` is what everyone writes, and it is BIASED unless `bound`
 * divides 2^64: the low values come up more often, by a margin invisible in a spot check and
 * real in a shuffle or a sample. This is Lemire's multiply-and-reject method — unbiased, and
 * in the overwhelmingly common case it rejects nothing at all.
 */
[[nodiscard]]
proven_u64 proven_rng_below(proven_rng_t rng, proven_u64 bound);

/**
 * @brief A uniformly random value in [lo, hi], inclusive at both ends.
 * @note Returns `lo` when `hi < lo`. The full span is representable: `lo == INT64_MIN` with
 *       `hi == INT64_MAX` does not overflow (the span is 2^64-1, computed in unsigned).
 */
[[nodiscard]]
proven_i64 proven_rng_range(proven_rng_t rng, proven_i64 lo, proven_i64 hi);

/**
 * @brief A uniformly random double in [0, 1).
 * @note 53 bits — the most a double's mantissa holds — so every representable value in the
 *       range is reachable and equally likely. It never returns 1.0.
 */
[[nodiscard]]
double proven_rng_f64(proven_rng_t rng);

/**
 * @brief Shuffle `count` elements of `elem_size` bytes into a uniformly random permutation.
 *
 * Fisher-Yates, drawing each index with `proven_rng_below`, so the permutation is unbiased —
 * unlike the `% n` shuffle, which measurably favours some orderings. Allocates nothing; swaps
 * in place. A `count` of 0 or 1 is a no-op.
 */
void proven_rng_shuffle(proven_rng_t rng, void *base, proven_size_t count, proven_size_t elem_size);

// -------------------------------------------------------------
// The OS entropy source (hosted)
// -------------------------------------------------------------

#ifndef PROVEN_FREESTANDING

/**
 * @brief Fill `buf` with `len` cryptographically strong random bytes from the OS CSPRNG.
 *
 * `getrandom` on Linux, `getentropy` on the BSDs and macOS, `BCryptGenRandom` on Windows.
 * Not a PRNG seeded from the clock, not `rand()`; the real thing, suitable for keys, tokens,
 * nonces, and for seeding the generators above.
 *
 * @return true if `buf` was filled; false if the OS call failed. On false, `buf`'s contents
 *         are unspecified and MUST NOT be used - a caller that ignores the return and uses
 *         the buffer anyway is exactly the bug this boolean exists to prevent.
 *
 * @note len == 0 is a successful no-op (returns true).
 * @note This can block briefly, once, very early in a system's life, if the OS entropy pool
 *       is not yet initialised. It never returns low-quality bytes to avoid blocking - it
 *       waits, because bytes that are not random are worse than bytes that are late.
 * @note Drawing a large buffer straight from the OS costs a syscall's worth of work per call.
 *       When you want bulk random data rather than a secret to seed with, seed a
 *       `proven_chacha_rng_t` from this once and draw from that.
 */
[[nodiscard]]
bool proven_random_bytes(void *buf, proven_size_t len);

/**
 * @brief A single cryptographically strong 64-bit value from the OS, or 0 on failure.
 *
 * A convenience over proven_random_bytes for the common case of needing one word - a hash
 * seed, a token. Because 0 is both the failure signal and a (vanishingly unlikely) valid
 * result, use proven_random_bytes directly when you must distinguish "the RNG failed" from
 * "the RNG returned zero".
 */
[[nodiscard]]
proven_u64 proven_random_u64(void);

/**
 * @brief Seed a ChaCha generator from the OS CSPRNG.
 *
 * @return false if the OS entropy call failed. The generator is left unseeded and you must
 *         not draw from it. This is the one place randomness is allowed to fail; check it
 *         here, once, and every draw afterwards is infallible.
 */
[[nodiscard]]
bool proven_chacha_rng_seed_from_os(proven_chacha_rng_t *g);

#endif /* !PROVEN_FREESTANDING */

#endif /* PROVEN_RANDOM_H */
