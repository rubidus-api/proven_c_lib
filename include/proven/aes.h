#ifndef PROVEN_AES_H
#define PROVEN_AES_H

#include "proven/types.h"
#include "proven/error.h"
#include "proven/memory.h"

/**
 * @file aes.h
 * @brief AES-CBC (NIST SP 800-38A) over whole blocks, and PKCS #7 padding's removal - for reading
 *        what other tools encrypted: PBES2 in PKCS #8 key files, PKCS #12 (since v0.109.0).
 *
 * CBC is not authenticated: anyone can change the ciphertext and the plaintext changes with it.
 * Use it to open a file whose integrity something else checks (a key file's own structure, a
 * MAC), never as a new design - for that, the TLS engine's AEADs exist. The cipher is the one
 * the TLS engine uses: the processor's AES instructions where there are some, otherwise a
 * bitsliced one; neither takes a time that depends on the key or the data.
 *
 * Pure computation: no allocation, no OS; part of the freestanding profile.
 */

/**
 * @brief Decrypt `data` in place: whole 16-byte blocks under a 16-, 24- or 32-byte key.
 * @param iv the first IV; left holding the last ciphertext block, which is the next call's IV.
 * @return PROVEN_ERR_INVALID_ARG for another key length, a length that is not a multiple of 16,
 *         or null pointers. Nothing is changed on error.
 */
[[nodiscard]]
proven_err_t proven_aes_cbc_decrypt(proven_mem_view_t key, proven_byte_t iv[16], proven_mem_mut_t data);

/** @brief Encrypt `data` in place, the same way round. Errors as proven_aes_cbc_decrypt. */
[[nodiscard]]
proven_err_t proven_aes_cbc_encrypt(proven_mem_view_t key, proven_byte_t iv[16], proven_mem_mut_t data);

/**
 * @brief The length of `data` without its PKCS #7 padding (RFC 5652 section 6.3): 1 to `block`
 *        bytes, each the count. The check reads the whole last block whatever the padding says.
 * @return PROVEN_ERR_INVALID_FORMAT for padding that is not that (`*len` is then 0);
 *         PROVEN_ERR_INVALID_ARG for a `block` outside 1..255 or a length that is not a
 *         non-zero multiple of it.
 */
[[nodiscard]]
proven_err_t proven_pkcs7_unpad(proven_mem_view_t data, proven_size_t block, proven_size_t *len);

#endif /* PROVEN_AES_H */
