#ifndef PROVEN_RANDOM_H
#define PROVEN_RANDOM_H

/**
 * @file random.h
 * @brief Cryptographically strong random bytes from the operating system.
 *
 * The one source of randomness a program can actually trust: the OS CSPRNG - `getrandom`
 * on Linux, `getentropy` on the BSDs and macOS, `BCryptGenRandom` on Windows. Not a PRNG
 * seeded from the clock, not `rand()`; the real thing, suitable for keys, tokens, nonces,
 * and seeding a HashDoS-resistant table.
 *
 * @note There is deliberately no user-visible pseudo-random generator here. A fast,
 *       reproducible PRNG and a secure one are different tools for different jobs, and
 *       handing out one under a name that suggests the other is how people ship insecure
 *       tokens. This module does exactly one thing: strong bytes from the OS. If you want a
 *       fast reproducible sequence for a simulation, that is a different (unmixed) concern.
 */

#include "types.h"

/**
 * @brief Fill `buf` with `len` cryptographically strong random bytes.
 *
 * @return true if `buf` was filled; false if the platform has no CSPRNG available (a
 *         freestanding target with no OS) or the OS call failed. On false, `buf`'s contents
 *         are unspecified and MUST NOT be used - a caller that ignores the return and uses
 *         the buffer anyway is exactly the bug this boolean exists to prevent.
 *
 * @note len == 0 is a successful no-op (returns true).
 * @note This can block briefly, once, very early in a system's life, if the OS entropy pool
 *       is not yet initialised. It never returns low-quality bytes to avoid blocking - it
 *       waits, because bytes that are not random are worse than bytes that are late.
 */
[[nodiscard]]
bool proven_random_bytes(void *buf, proven_size_t len);

/**
 * @brief A single cryptographically strong 64-bit value, or 0 on failure.
 *
 * A convenience over proven_random_bytes for the common case of needing one word - a hash
 * seed, a token. Because 0 is both the failure signal and a (vanishingly unlikely) valid
 * result, use proven_random_bytes directly when you must distinguish "the RNG failed" from
 * "the RNG returned zero".
 */
[[nodiscard]]
proven_u64 proven_random_u64(void);

#endif /* PROVEN_RANDOM_H */
