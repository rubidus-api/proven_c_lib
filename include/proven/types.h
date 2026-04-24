#ifndef PROVEN_TYPES_H
#define PROVEN_TYPES_H

/**
 * @file types.h
 * @brief Fundamental type definitions for the proven library.
 * Strictly adheres to C23 standards and the SPEC v1.12.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Fixed-width integer types (No _t suffix as per SPEC) */
typedef int8_t         proven_i8;
typedef int16_t        proven_i16;
typedef int32_t        proven_i32;
typedef int64_t        proven_i64;

typedef uint8_t        proven_u8;
typedef uint16_t       proven_u16;
typedef uint32_t       proven_u32;
typedef uint64_t       proven_u64;

/**
 * @brief byte type for memory access.
 * The only type allowed to alias any object representation in C.
 */
typedef unsigned char  proven_byte_t;

/* Semantic types (With _t suffix) */

/**
 * @brief Type for sizes and indices. 
 * Matches standard size_t for optimal platform alignment.
 */
typedef size_t         proven_size_t;

/**
 * @brief Type for pointer differences and offsets.
 * Matches standard ptrdiff_t.
 */
typedef ptrdiff_t      proven_ptrdiff_t;

/**
 * @brief Types for holding pointers as integers.
 */
typedef intptr_t       proven_intptr_t;
typedef uintptr_t      proven_uintptr_t;

/**
 * @brief Checked integer arithmetic from C23.
 * Wrapped with PROVEN_ prefix to avoid naming conflicts and maintain isolation.
 */
#include <stdckdint.h>
#define PROVEN_CKD_ADD(res, a, b) ckd_add(res, a, b)
#define PROVEN_CKD_SUB(res, a, b) ckd_sub(res, a, b)
#define PROVEN_CKD_MUL(res, a, b) ckd_mul(res, a, b)

#endif /* PROVEN_TYPES_H */
