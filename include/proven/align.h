#ifndef PROVEN_ALIGN_H
#define PROVEN_ALIGN_H

#include "types.h"

/**
 * @file align.h
 * @brief Memory alignment utilities.
 */

/**
 * @brief Aligns an address up to the nearest multiple of align.
 * 
 * @param addr The address or size to align.
 * @param align The alignment boundary. MUST be a power of 2.
 * @return The aligned address, or 0 if overflow occurs.
 */
static inline proven_size_t proven_mem_align_up(proven_size_t addr, proven_size_t align) {
    proven_size_t mask = align - 1;
    proven_size_t res;
    if (PROVEN_CKD_ADD(&res, addr, mask)) {
        return 0; // Overflow
    }
    return res & ~mask;
}

#endif /* PROVEN_ALIGN_H */
