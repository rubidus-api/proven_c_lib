#ifndef PROVEN_SYS_CRC_H
#define PROVEN_SYS_CRC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @file proven_sys_crc.h
 * @brief Carry-less multiplication for the reflected CRCs of src/proven/hash.c.
 *
 * Only the folding lives here; the constants, the table-driven code and the choice between the
 * two are in hash.c.
 */

/** @brief Whether this processor has the instruction (PCLMULQDQ, PMULL). */
[[nodiscard]]
bool proven_sys_crc_clmul_available(void);

/**
 * @brief Fold `n` bytes (a multiple of 16, at least 64) to 16 whose CRC, from a register of 0,
 *        is the CRC of the `n` from the register `crc`.
 *
 * `k` holds reflected x^576, x^512, x^192 and x^128 modulo the polynomial. Returns false, with
 * `out` untouched, where this build has no such instruction.
 */
bool proven_sys_crc_fold(const uint64_t k[4], uint64_t crc, const uint8_t *p, size_t n, uint8_t out[16]);

#endif // PROVEN_SYS_CRC_H
