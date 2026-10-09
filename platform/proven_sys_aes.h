#ifndef PROVEN_SYS_AES_H
#define PROVEN_SYS_AES_H

/* The processor's AES and carry-less multiply instructions, where it has them: x86-64 (AES-NI
 * with PCLMULQDQ) and AArch64 (the ARMv8 cryptography extension). Detection is at run time.
 * Everything else, and every freestanding build, reports "none" and is never called. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* True when the three calls below may be used on this processor. */
bool proven_sys_aes_available(void);

/* Encrypt one block. `rk` is (rounds + 1) * 16 bytes of round keys in the standard order. */
void proven_sys_aes_encrypt_block(const uint8_t *rk, int rounds, const uint8_t in[16], uint8_t out[16]);

/* CTR mode: XOR `len` bytes with the keystream of counter blocks iv | be32(counter),
 * be32(counter + 1), ... `in` and `out` may be the same. */
void proven_sys_aes_ctr(const uint8_t *rk, int rounds, const uint8_t iv[12], uint32_t counter,
                        const uint8_t *in, uint8_t *out, size_t len);

/* GHASH: for each 16-byte block, y = (y ^ block) * h in GF(2^128) as GCM defines it. */
void proven_sys_aes_ghash(uint8_t y[16], const uint8_t h[16], const uint8_t *data, size_t blocks);

#endif
