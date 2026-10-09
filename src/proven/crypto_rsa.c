#include "proven_internal_crypto.h"
#include "proven/hash.h"
#include "proven/hmac.h"

/* RSA signature verification: PKCS #1 v1.5 and PSS. Only the public operation is here. */

#define RSA_MAX_BYTES (PROVEN_CRYPTO_RSA_MAX_BITS / 8)

/* sig^e mod n into `em`, exactly as many bytes as the modulus. Returns that length, or 0. */
static proven_size_t rsa_public(proven_mem_view_t n, proven_mem_view_t e, proven_mem_view_t sig,
                                proven_byte_t em[RSA_MAX_BYTES], proven_size_t *bits_out) {
    proven_crypto_mp_mod_t mod;
    proven_u32 s[PROVEN_CRYPTO_MP_MAX], ex[2];
    if (!n.ptr || !e.ptr || !sig.ptr) return 0;
    while (n.size > 0 && n.ptr[0] == 0) { n.ptr++; n.size--; }
    while (e.size > 0 && e.ptr[0] == 0) { e.ptr++; e.size--; }
    if (n.size > RSA_MAX_BYTES || e.size == 0 || e.size > 8) return 0;
    if (!proven_crypto_mp_mod_init(&mod, n.ptr, n.size)) return 0;
    if (mod.bits < PROVEN_CRYPTO_RSA_MIN_BITS || mod.bits > PROVEN_CRYPTO_RSA_MAX_BITS) return 0;
    if (!proven_crypto_mp_load_be(ex, 2, e.ptr, e.size)) return 0;
    if ((ex[0] & 1u) == 0 || (ex[1] == 0 && ex[0] < 3)) return 0;
    if (sig.size != n.size) return 0;
    if (!proven_crypto_mp_load_be(s, mod.limbs, sig.ptr, sig.size)) return 0;
    if (!proven_crypto_mp_lt(s, mod.n, mod.limbs)) return 0;
    proven_crypto_mp_to_mont(&mod, s, s);
    proven_crypto_mp_pow(&mod, s, s, ex, 2);
    proven_crypto_mp_from_mont(&mod, s, s);
    proven_crypto_mp_store_be(em, n.size, s, mod.limbs);
    *bits_out = mod.bits;
    return n.size;
}

static const proven_byte_t RSA_DI_SHA256[] = { 0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20 };
static const proven_byte_t RSA_DI_SHA384[] = { 0x30, 0x41, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x02, 0x05, 0x00, 0x04, 0x30 };
static const proven_byte_t RSA_DI_SHA512[] = { 0x30, 0x51, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x03, 0x05, 0x00, 0x04, 0x40 };

bool proven_crypto_rsa_verify_pkcs1(proven_mem_view_t n, proven_mem_view_t e, int hash,
                                    proven_mem_view_t digest, proven_mem_view_t sig) {
    proven_byte_t em[RSA_MAX_BYTES];
    proven_size_t bits = 0;
    const proven_byte_t *di;
    proven_size_t hlen = proven_hmac_size((proven_hmac_hash_t)hash);
    if (hash == PROVEN_HMAC_SHA256) di = RSA_DI_SHA256;
    else if (hash == PROVEN_HMAC_SHA384) di = RSA_DI_SHA384;
    else if (hash == PROVEN_HMAC_SHA512) di = RSA_DI_SHA512;
    else return false;
    if (!digest.ptr || digest.size != hlen) return false;
    proven_size_t k = rsa_public(n, e, sig, em, &bits);
    if (k == 0) return false;
    /* The whole encoding is rebuilt and compared: 00 01 FF..FF 00 DigestInfo. Parsing what the
     * signature decrypts to is how forgeries with trailing garbage have been accepted. */
    const proven_size_t di_len = 19, t_len = di_len + hlen;
    if (k < t_len + 11) return false;
    proven_byte_t diff = 0;
    proven_size_t pad_end = k - t_len - 1;
    diff |= em[0];
    diff |= (proven_byte_t)(em[1] ^ 0x01);
    for (proven_size_t i = 2; i < pad_end; ++i) diff |= (proven_byte_t)(em[i] ^ 0xff);
    diff |= em[pad_end];
    for (proven_size_t i = 0; i < di_len; ++i) diff |= (proven_byte_t)(em[pad_end + 1 + i] ^ di[i]);
    for (proven_size_t i = 0; i < hlen; ++i) diff |= (proven_byte_t)(em[pad_end + 1 + di_len + i] ^ digest.ptr[i]);
    return diff == 0;
}

static void rsa_hash(int hash, proven_mem_view_t a, proven_mem_view_t b, proven_mem_view_t c, proven_byte_t out[PROVEN_HMAC_MAX_SIZE]) {
    if (hash == PROVEN_HMAC_SHA256) {
        proven_sha256_t h;
        proven_sha256_init(&h);
        proven_sha256_update(&h, a); proven_sha256_update(&h, b); proven_sha256_update(&h, c);
        proven_sha256_final(&h, out);
    } else {
        proven_sha512_t h;
        if (hash == PROVEN_HMAC_SHA384) proven_sha384_init(&h); else proven_sha512_init(&h);
        proven_sha512_update(&h, a); proven_sha512_update(&h, b); proven_sha512_update(&h, c);
        if (hash == PROVEN_HMAC_SHA384) proven_sha384_final(&h, out); else proven_sha512_final(&h, out);
    }
}

bool proven_crypto_rsa_verify_pss(proven_mem_view_t n, proven_mem_view_t e, int hash, proven_size_t salt_len,
                                  proven_mem_view_t digest, proven_mem_view_t sig) {
    proven_byte_t em_full[RSA_MAX_BYTES], db[RSA_MAX_BYTES], h2[PROVEN_HMAC_MAX_SIZE], block[PROVEN_HMAC_MAX_SIZE];
    proven_size_t bits = 0;
    proven_size_t hlen = proven_hmac_size((proven_hmac_hash_t)hash);
    if (hlen == 0 || !digest.ptr || digest.size != hlen || salt_len > RSA_MAX_BYTES) return false;
    proven_size_t k = rsa_public(n, e, sig, em_full, &bits);
    if (k == 0) return false;
    /* EM is emLen = ceil((bits - 1) / 8) bytes: one shorter than the modulus when bits = 1 mod 8. */
    proven_size_t em_bits = bits - 1;
    proven_size_t em_len = (em_bits + 7) / 8;
    const proven_byte_t *em = em_full + (k - em_len);
    if (em_len < k && em_full[0] != 0) return false;
    if (em_len < hlen + salt_len + 2) return false;
    if (em[em_len - 1] != 0xbc) return false;
    proven_size_t db_len = em_len - hlen - 1;
    const proven_byte_t *h = em + db_len;
    proven_byte_t top_mask = (proven_byte_t)(0xffu >> (8 * em_len - em_bits));
    if ((em[0] & (proven_byte_t)~top_mask) != 0) return false;
    /* DB = maskedDB xor MGF1(H) */
    for (proven_size_t done = 0, counter = 0; done < db_len; ++counter) {
        proven_byte_t c[4] = { (proven_byte_t)(counter >> 24), (proven_byte_t)(counter >> 16), (proven_byte_t)(counter >> 8), (proven_byte_t)counter };
        rsa_hash(hash, (proven_mem_view_t){ .ptr = h, .size = hlen }, (proven_mem_view_t){ .ptr = c, .size = 4 }, (proven_mem_view_t){ .ptr = c, .size = 0 }, block);
        for (proven_size_t i = 0; i < hlen && done < db_len; ++i, ++done) db[done] = (proven_byte_t)(em[done] ^ block[i]);
    }
    db[0] &= top_mask;
    proven_size_t ps_len = db_len - salt_len - 1;
    proven_byte_t diff = 0;
    for (proven_size_t i = 0; i < ps_len; ++i) diff |= db[i];
    diff |= (proven_byte_t)(db[ps_len] ^ 0x01);
    if (diff != 0) return false;
    static const proven_byte_t zeros[8] = { 0 };
    rsa_hash(hash, (proven_mem_view_t){ .ptr = zeros, .size = 8 }, digest, (proven_mem_view_t){ .ptr = db + ps_len + 1, .size = salt_len }, h2);
    return proven_mem_equal_ct((proven_mem_view_t){ .ptr = h2, .size = hlen }, (proven_mem_view_t){ .ptr = h, .size = hlen });
}
