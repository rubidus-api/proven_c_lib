#include "proven/hash.h"

/*
 * Four hashes, one per use case, each implemented from its specification and checked in
 * tests/test_unit_hash against that specification's own known-answer vectors. Written to
 * the contract in include/proven/hash.h, whose test landed red one commit earlier.
 *
 * Everything here is endianness-independent by construction: bytes go in and come out in a
 * fixed order regardless of the host, because a fingerprint that changed with the machine
 * would not be a fingerprint of the content.
 */

// =============================================================================
// FNV-1a 64 - fast table hash, trusted input
// =============================================================================

proven_u64 proven_hash_bytes(proven_mem_view_t data) {
    /* 64-bit offset basis and prime. xor the byte in, THEN multiply (that ordering is what
     * makes it FNV-1a rather than FNV-1, and it avalanches better). */
    proven_u64 h = 0xcbf29ce484222325ull;
    if (data.size > 0 && !data.ptr) return h;   /* a size>0 view with no pointer: consistent with the SHA path */
    for (proven_size_t i = 0; i < data.size; ++i) {
        h ^= (proven_u64)data.ptr[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

// =============================================================================
// SipHash-2-4 - keyed table hash, untrusted input
// =============================================================================

static proven_u64 sip_rotl(proven_u64 x, int b) {
    return (x << b) | (x >> (64 - b));
}

static proven_u64 sip_read_le64(const proven_byte_t *p) {
    proven_u64 v = 0;
    for (int i = 0; i < 8; ++i) v |= (proven_u64)p[i] << (8 * i);
    return v;
}

#define SIP_ROUND(a, b, c, d)             \
    do {                                  \
        (a) += (b); (b) = sip_rotl(b, 13); (b) ^= (a); (a) = sip_rotl(a, 32); \
        (c) += (d); (d) = sip_rotl(d, 16); (d) ^= (c);                        \
        (a) += (d); (d) = sip_rotl(d, 21); (d) ^= (a);                        \
        (c) += (b); (b) = sip_rotl(b, 17); (b) ^= (c); (c) = sip_rotl(c, 32); \
    } while (0)

proven_u64 proven_hash_keyed(proven_mem_view_t data, const proven_byte_t key[16]) {
    proven_u64 k0 = sip_read_le64(key);
    proven_u64 k1 = sip_read_le64(key + 8);

    proven_u64 v0 = 0x736f6d6570736575ull ^ k0;
    proven_u64 v1 = 0x646f72616e646f6dull ^ k1;
    proven_u64 v2 = 0x6c7967656e657261ull ^ k0;
    proven_u64 v3 = 0x7465646279746573ull ^ k1;

    if (data.size > 0 && !data.ptr) { data.size = 0; }   /* consistent with the SHA path */
    const proven_byte_t *in = data.ptr;
    proven_size_t len = data.size;
    proven_size_t left = len & 7u;
    const proven_byte_t *end = in + (len - left);

    /* 2 compression rounds ("2") per 8-byte word. */
    for (; in != end; in += 8) {
        proven_u64 m = sip_read_le64(in);
        v3 ^= m;
        SIP_ROUND(v0, v1, v2, v3);
        SIP_ROUND(v0, v1, v2, v3);
        v0 ^= m;
    }

    /* Final word: the remaining 0..7 bytes, with the total length in the top byte. */
    proven_u64 b = (proven_u64)(len & 0xffu) << 56;
    for (proven_size_t i = 0; i < left; ++i) {
        b |= (proven_u64)in[i] << (8 * i);
    }
    v3 ^= b;
    SIP_ROUND(v0, v1, v2, v3);
    SIP_ROUND(v0, v1, v2, v3);
    v0 ^= b;

    /* 4 finalization rounds ("4"). */
    v2 ^= 0xff;
    SIP_ROUND(v0, v1, v2, v3);
    SIP_ROUND(v0, v1, v2, v3);
    SIP_ROUND(v0, v1, v2, v3);
    SIP_ROUND(v0, v1, v2, v3);

    return v0 ^ v1 ^ v2 ^ v3;
}

// =============================================================================
// CRC-32 (IEEE 802.3, reflected) - corruption checksum
// =============================================================================

proven_u32 proven_crc32_update(proven_u32 crc, proven_mem_view_t data) {
    /* Reflected form: init/xorout 0xFFFFFFFF, polynomial 0xEDB88320. The running value the
     * caller holds is the already-inverted CRC, so we invert on the way in and out and the
     * middle stays as a plain register - that is what makes chaining chunks work. */
    proven_u32 c = ~crc;
    if (data.size > 0 && !data.ptr) return crc;   /* consistent with the SHA path */
    for (proven_size_t i = 0; i < data.size; ++i) {
        c ^= (proven_u32)data.ptr[i];
        for (int k = 0; k < 8; ++k) {
            /* Branchless bit-reflected division step. */
            c = (c >> 1) ^ (0xEDB88320u & (~(c & 1u) + 1u));
        }
    }
    return ~c;
}

proven_u32 proven_crc32(proven_mem_view_t data) {
    return proven_crc32_update(0, data);
}

// =============================================================================
// SHA-256 (FIPS 180-4) - cryptographic digest
// =============================================================================

static const proven_u32 sha256_k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static proven_u32 sha_rotr(proven_u32 x, int n) {
    return (x >> n) | (x << (32 - n));
}

static void sha256_compress(proven_u32 state[8], const proven_byte_t block[64]) {
    proven_u32 w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = ((proven_u32)block[i * 4] << 24) | ((proven_u32)block[i * 4 + 1] << 16) |
               ((proven_u32)block[i * 4 + 2] << 8) | (proven_u32)block[i * 4 + 3];
    }
    for (int i = 16; i < 64; ++i) {
        proven_u32 s0 = sha_rotr(w[i - 15], 7) ^ sha_rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        proven_u32 s1 = sha_rotr(w[i - 2], 17) ^ sha_rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    proven_u32 a = state[0], b = state[1], c = state[2], d = state[3];
    proven_u32 e = state[4], f = state[5], g = state[6], h = state[7];

    for (int i = 0; i < 64; ++i) {
        proven_u32 S1 = sha_rotr(e, 6) ^ sha_rotr(e, 11) ^ sha_rotr(e, 25);
        proven_u32 ch = (e & f) ^ (~e & g);
        proven_u32 t1 = h + S1 + ch + sha256_k[i] + w[i];
        proven_u32 S0 = sha_rotr(a, 2) ^ sha_rotr(a, 13) ^ sha_rotr(a, 22);
        proven_u32 maj = (a & b) ^ (a & c) ^ (b & c);
        proven_u32 t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

void proven_sha256_init(proven_sha256_t *ctx) {
    if (!ctx) return;
    ctx->state[0] = 0x6a09e667u; ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u; ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu; ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu; ctx->state[7] = 0x5be0cd19u;
    ctx->length = 0;
    ctx->block_len = 0;
}

void proven_sha256_update(proven_sha256_t *ctx, proven_mem_view_t data) {
    if (!ctx || (data.size > 0 && !data.ptr)) return;
    ctx->length += data.size;
    for (proven_size_t i = 0; i < data.size; ++i) {
        ctx->block[ctx->block_len++] = data.ptr[i];
        if (ctx->block_len == 64) {
            sha256_compress(ctx->state, ctx->block);
            ctx->block_len = 0;
        }
    }
}

void proven_sha256_final(proven_sha256_t *ctx, proven_byte_t out[PROVEN_SHA256_SIZE]) {
    if (!ctx || !out) return;

    /* The length in BITS, captured before the padding is appended. */
    proven_u64 bits = ctx->length * 8u;

    /* Append 0x80, then zeros, until the block is 56 bytes - leaving 8 for the length. If
     * there is not room, finish this block first and pad in a fresh one (the case the
     * 56-byte and million-'a' vectors exist to catch). */
    proven_byte_t one = 0x80;
    proven_sha256_update(ctx, (proven_mem_view_t){ .ptr = &one, .size = 1 });
    /* proven_sha256_update bumped length; it does not matter, `bits` was already taken. */
    proven_byte_t zero = 0;
    while (ctx->block_len != 56) {
        proven_sha256_update(ctx, (proven_mem_view_t){ .ptr = &zero, .size = 1 });
    }

    proven_byte_t lenbytes[8];
    for (int i = 0; i < 8; ++i) lenbytes[i] = (proven_byte_t)(bits >> (56 - 8 * i));
    proven_sha256_update(ctx, (proven_mem_view_t){ .ptr = lenbytes, .size = 8 });
    /* block_len is now 0: the last block was compressed by the final update. */

    for (int i = 0; i < 8; ++i) {
        out[i * 4]     = (proven_byte_t)(ctx->state[i] >> 24);
        out[i * 4 + 1] = (proven_byte_t)(ctx->state[i] >> 16);
        out[i * 4 + 2] = (proven_byte_t)(ctx->state[i] >> 8);
        out[i * 4 + 3] = (proven_byte_t)(ctx->state[i]);
    }
}

void proven_sha256(proven_mem_view_t data, proven_byte_t out[PROVEN_SHA256_SIZE]) {
    proven_sha256_t ctx;
    proven_sha256_init(&ctx);
    proven_sha256_update(&ctx, data);
    proven_sha256_final(&ctx, out);
}

void proven_sha256_to_hex(const proven_byte_t digest[PROVEN_SHA256_SIZE], char out[65]) {
    static const char d[] = "0123456789abcdef";
    if (!digest || !out) return;
    for (proven_size_t i = 0; i < PROVEN_SHA256_SIZE; ++i) {
        out[i * 2]     = d[digest[i] >> 4];
        out[i * 2 + 1] = d[digest[i] & 0xf];
    }
    out[64] = 0;
}
