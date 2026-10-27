#include "proven/zstd.h"
#include "proven_internal_zstd.h"

/* Zstandard decompression (RFC 8878).
 *
 * The frame and block headers are read a byte at a time as they arrive, and can stop
 * anywhere. A compressed block is decoded only when all of it is there: its Huffman streams
 * and its sequences are read from their ends. Each block is decoded into `hist`, after the
 * history it may refer to, and handed out from there. Every corner case that the format
 * leaves to the decoder is settled as the reference decoder (libzstd) settles it, or more
 * strictly: a damaged stream that libzstd reads through may be refused here - its fast
 * Huffman decoder does not check that each of four streams was read exactly, and this one
 * does - but nothing libzstd refuses is accepted here. */

#define BLOCK_MAX ZSTD_BLOCK_MAX
#define WILD 32                         /* bytes after `hist` and `lits` that copies eight at a time may touch */
#define WINDOW_MIN ((proven_u64)1024)
#define WINDOW_LIMIT ((proven_u64)1 << 31)
#define HUF_LOG_MAX 12                  /* as libzstd: RFC 8878 says 11, and nothing larger is ever written */
#define LL_MAX 35
#define ML_MAX 52
#define OF_MAX 31
#define LL_LOG 9
#define ML_LOG 9
#define OF_LOG 8

/* One state of an FSE decoding table: the symbol it stands for, and how the next state is
 * found - `next` plus that many more bits. */
typedef struct {
    proven_u16 next;
    proven_u8 symbol;
    proven_u8 bits;
} proven_fse_t;

/* One state of a sequence table: the FSE state as above, with what its symbol means made out -
 * the base value of the literal length, match length or offset, and how many more bits are
 * added to it - so that decoding a sequence looks nothing else up. */
typedef struct {
    proven_u32 base;
    proven_u16 next;
    proven_u8 bits;
    proven_u8 extra;
} proven_seq_t;

/* One entry of a Huffman table indexed by the next `log` bits: the symbol and its code's
 * length. */
typedef struct {
    proven_u8 symbol;
    proven_u8 bits;
} proven_huf_t;

enum { Z_MAGIC, Z_SKIP_SIZE, Z_SKIP, Z_FHD, Z_HEADER, Z_BLOCK, Z_RAW, Z_RLE, Z_DATA, Z_DRAIN, Z_CHECK };

struct proven_zstd_decoder {
    proven_allocator_t alloc;
    proven_u64 window_max;
    proven_err_t error;
    int state;
    int after;                          /* Z_DRAIN: where to go once everything is out */
    bool boundary;                      /* a frame has ended and nothing after it has been taken */
    proven_byte_t hdr[16];
    unsigned have, need;
    proven_u64 skip;
    /* the frame */
    proven_u64 window, fcs, total;
    bool has_fcs, has_check, last;
    proven_size_t block_max, full;      /* `full`: the most `hist` will ever need for this frame */
    unsigned block_type;
    proven_size_t block_size, block_have;
    proven_u64 rep[3];
    proven_xxh64_t xxh;
    /* buffers, grown on demand and kept */
    proven_byte_t *hist, *inbuf, *lits;
    proven_size_t hist_cap, inbuf_cap, lits_cap;
    /* proven_zstd_decode_all: a frame whose size is stated and fits is decoded straight into the
     * caller's buffer - `hist` is then lent (`lent`), its own buffer kept in `own`, and `slack`
     * is what of the caller's buffer lies past the frame (WILD at most) for the wide copies. */
    bool direct, lent;
    proven_byte_t *own;
    proven_size_t own_cap, slack;
    proven_size_t hist_pos;             /* what this frame has produced and `hist` still holds ends here */
    proven_size_t drain;                /* and what has not been handed out yet begins here */
    /* entropy tables: what the last block left, for the next to repeat */
    bool huf_ok;
    unsigned huf_log;
    const proven_seq_t *llt, *oft, *mlt;
    unsigned lll, ofl, mll;
    proven_huf_t huf[1u << HUF_LOG_MAX];
    proven_seq_t ll_dyn[1u << LL_LOG], of_dyn[1u << OF_LOG], ml_dyn[1u << ML_LOG];
    proven_seq_t ll_def[64], of_def[32], ml_def[64];
};

// -----------------------------------------------------------------------------
// Bits
// -----------------------------------------------------------------------------

/* Up to eight bytes from `at`, little-endian; bytes at or past `len` read as zero. */
static proven_u64 load_upto(const proven_byte_t *p, proven_size_t len, proven_size_t at) {
    if (at + 8 <= len) return le64(p + at);
    proven_u64 w = 0;
    for (proven_size_t i = 0; at + i < len && i < 8; ++i) w |= (proven_u64)p[at + i] << (8 * i);
    return w;
}

/* A stream read from its end towards its beginning (RFC 8878, section 4.1): the last byte's
 * highest set bit marks where it begins, and the bits below it are read highest first.
 *
 * Eight bytes of it are held in `c`, of which the top `used` bits have been taken; `reload`
 * moves `p` back and loads again, and must be called before more than 56 bits are taken. At
 * the beginning of the stream it cannot move back: reading on there reads zeros and takes
 * `used` past 64, which the callers treat as damage. The stream has been read exactly when
 * `p` is at its beginning and `used` is 64. */
typedef struct {
    const proven_byte_t *start, *p;
    proven_u64 c;
    unsigned used;
} proven_bits_t;

static bool bits_init(proven_bits_t *b, const proven_byte_t *src, proven_size_t len) {
    if (len == 0 || src[len - 1] == 0) return false;
    b->start = src;
    if (len >= 8) {
        b->p = src + len - 8;
        b->c = le64(b->p);
        b->used = 0;
    } else {
        b->p = src;
        b->c = 0;
        for (proven_size_t i = 0; i < len; ++i) b->c |= (proven_u64)src[i] << (8 * i);
        b->used = (unsigned)(8 - len) * 8;
    }
    b->used += 8 - highbit32(src[len - 1]);      /* the zeros above the marker, and the marker */
    return true;
}

/* The next `n` bits (at most 56) without taking them. */
static inline proven_u64 bits_look(const proven_bits_t *b, unsigned n) {
    /* Past 63 the stream is damaged and every caller finds out (bits_exact, bits_over); what
     * is returned meanwhile only has to stay in range - at most `n` bits - which it does. */
    return ((b->c << (b->used & 63)) >> 1) >> (63 - n);
}

static inline proven_u64 bits_read(proven_bits_t *b, unsigned n) {
    const proven_u64 v = bits_look(b, n);
    b->used += n;
    return v;
}

static inline void bits_reload(proven_bits_t *b) {
    if (b->used > 64) return;
    if (b->p >= b->start + 8) {
        b->p -= b->used >> 3;
        b->used &= 7;
    } else {
        if (b->p == b->start) return;
        proven_size_t k = b->used >> 3;
        if ((proven_size_t)(b->p - b->start) < k) k = (proven_size_t)(b->p - b->start);
        b->p -= k;
        b->used -= (unsigned)k * 8;
    }
    b->c = le64(b->p);
}

/* Whether the stream was read to its beginning and no further. */
static bool bits_exact(proven_bits_t *b) {
    bits_reload(b);
    return b->p == b->start && b->used == 64;
}

/* Whether it was read past its beginning. */
static bool bits_over(proven_bits_t *b) {
    bits_reload(b);
    return b->used > 64;
}

/* A stream read from its beginning, lowest bit first: the FSE table descriptions. Past the
 * end reads zeros; the caller compares how far it read with the length. */
typedef struct {
    const proven_byte_t *p;
    proven_size_t len, bit;
} proven_fwd_t;

static proven_u32 fwd_peek(const proven_fwd_t *f, unsigned n) {
    const proven_u64 w = load_upto(f->p, f->len, f->bit >> 3) >> (f->bit & 7);
    return (proven_u32)(w & (((proven_u64)1 << n) - 1));
}

// -----------------------------------------------------------------------------
// FSE tables (RFC 8878, section 4.1)
// -----------------------------------------------------------------------------

/* Read a table description: the normalised counts of up to `max_sym + 1` symbols. */
static bool fse_read(const proven_byte_t *p, proven_size_t len, unsigned max_sym, unsigned max_log,
                     proven_i16 *norm, unsigned *nsym, unsigned *log, proven_size_t *used) {
    proven_fwd_t f = { p, len, 0 };
    const unsigned al = fwd_peek(&f, 4) + 5;
    f.bit += 4;
    if (al > max_log) return false;
    int remaining = (1 << al) + 1, threshold = 1 << al;
    unsigned nbits = al + 1, s = 0;
    bool prev0 = false;
    while (remaining > 1 && s <= max_sym) {
        if (prev0) {
            unsigned n0 = s;
            for (;;) {
                const unsigned r = fwd_peek(&f, 2);
                f.bit += 2;
                n0 += r;
                if (r != 3) break;
                if (n0 > max_sym + 1) return false;
            }
            if (n0 > max_sym) return false;
            while (s < n0) norm[s++] = 0;
        }
        const int max = (2 * threshold - 1) - remaining;
        const int bits = (int)fwd_peek(&f, nbits);
        int count;
        if ((bits & (threshold - 1)) < max) {
            count = bits & (threshold - 1);
            f.bit += nbits - 1;
        } else {
            count = bits & (2 * threshold - 1);
            if (count >= threshold) count -= max;
            f.bit += nbits;
        }
        count--;
        remaining -= count < 0 ? -count : count;
        norm[s++] = (proven_i16)count;
        prev0 = count == 0;
        while (remaining < threshold) { nbits--; threshold >>= 1; }
        if (f.bit > 8 * len) return false;
    }
    if (remaining != 1) return false;
    *used = (f.bit + 7) >> 3;
    if (*used > len) return false;
    *nsym = s;
    *log = al;
    return true;
}

static bool fse_build(proven_fse_t *t, const proven_i16 *norm, unsigned nsym, unsigned al) {
    const proven_u32 size = 1u << al, mask = size - 1;
    proven_u32 high = size - 1;
    proven_u16 next[256];
    for (unsigned s = 0; s < nsym; ++s) {
        if (norm[s] == -1) {
            t[high--].symbol = (proven_u8)s;
            next[s] = 1;
        } else {
            next[s] = (proven_u16)norm[s];
        }
    }
    const proven_u32 step = (size >> 1) + (size >> 3) + 3;
    proven_u32 pos = 0;
    for (unsigned s = 0; s < nsym; ++s) {
        for (int i = 0; i < norm[s]; ++i) {
            t[pos].symbol = (proven_u8)s;
            do pos = (pos + step) & mask; while (pos > high);
        }
    }
    if (pos != 0) return false;
    for (proven_u32 u = 0; u < size; ++u) {
        const proven_u32 ns = next[t[u].symbol]++;
        const unsigned nb = al - highbit32(ns);
        t[u].bits = (proven_u8)nb;
        t[u].next = (proven_u16)((ns << nb) - size);
    }
    return true;
}

/* A sequence table from an FSE table of `1 << al` states: each symbol's base and extra bits
 * from the code tables of RFC 8878 section 3.1.1.3.2.1 (offsets: base 2^code, code bits). */
enum { SEQ_LL, SEQ_ML, SEQ_OF };

static void seq_from(proven_seq_t *dst, const proven_fse_t *src, unsigned al, int kind) {
    for (proven_u32 u = 0; u < (1u << al); ++u) {
        const unsigned c = src[u].symbol;
        dst[u].next = src[u].next;
        dst[u].bits = src[u].bits;
        if (kind == SEQ_LL) { dst[u].base = LL_BASE[c]; dst[u].extra = LL_BITS[c]; }
        else if (kind == SEQ_ML) { dst[u].base = ML_BASE[c]; dst[u].extra = ML_BITS[c]; }
        else { dst[u].base = (proven_u32)1 << c; dst[u].extra = (proven_u8)c; }
    }
}

// -----------------------------------------------------------------------------
// Making, freeing, starting over
// -----------------------------------------------------------------------------

static void start(proven_zstd_decoder_t *z) {
    z->error = PROVEN_OK;
    z->state = Z_MAGIC;
    z->boundary = false;
    z->have = 0;
    z->hist_pos = 0;
    z->drain = 0;
}

proven_err_t proven_zstd_decoder_create(proven_allocator_t alloc, proven_u64 window_max, proven_zstd_decoder_t **out) {
    if (!out) return PROVEN_ERR_INVALID_ARG;
    *out = NULL;
    if (window_max == 0) window_max = PROVEN_ZSTD_WINDOW_DEFAULT;
    if (!proven_alloc_is_valid(alloc) || window_max < WINDOW_MIN || window_max > WINDOW_LIMIT) return PROVEN_ERR_INVALID_ARG;
    proven_result_mem_mut_t m = alloc.alloc_fn(alloc.ctx, sizeof(proven_zstd_decoder_t), 16);
    if (m.err != PROVEN_OK) return PROVEN_ERR_NOMEM;
    proven_zstd_decoder_t *z = (proven_zstd_decoder_t *)(void *)m.value.ptr;
    *z = (proven_zstd_decoder_t){ .alloc = alloc, .window_max = window_max, .slack = WILD };
    /* The predefined distributions are valid by construction. */
    proven_fse_t t[64];
    (void)fse_build(t, LL_DEFAULT, 36, 6); seq_from(z->ll_def, t, 6, SEQ_LL);
    (void)fse_build(t, OF_DEFAULT, 29, 5); seq_from(z->of_def, t, 5, SEQ_OF);
    (void)fse_build(t, ML_DEFAULT, 53, 6); seq_from(z->ml_def, t, 6, SEQ_ML);
    start(z);
    *out = z;
    return PROVEN_OK;
}

void proven_zstd_decoder_destroy(proven_zstd_decoder_t *z) {
    if (!z) return;
    proven_byte_t *h = z->lent ? z->own : z->hist;
    if (h) z->alloc.free_fn(z->alloc.ctx, h);
    if (z->inbuf) z->alloc.free_fn(z->alloc.ctx, z->inbuf);
    if (z->lits) z->alloc.free_fn(z->alloc.ctx, z->lits);
    z->alloc.free_fn(z->alloc.ctx, z);
}

void proven_zstd_decoder_reset(proven_zstd_decoder_t *z) {
    if (z) start(z);
}

/* Make `*buf` hold at least `want` bytes, keeping what it holds, with `slack` more after them
 * (zeroed: they may be read, never used). */
static proven_err_t grow(proven_zstd_decoder_t *z, proven_byte_t **buf, proven_size_t *cap, proven_size_t want, proven_size_t slack) {
    if (*cap >= want && *buf) return PROVEN_OK;
    proven_result_mem_mut_t m = *buf ? z->alloc.realloc_fn(z->alloc.ctx, *buf, *cap + slack, want + slack, 1)
                                     : z->alloc.alloc_fn(z->alloc.ctx, want + slack, 1);
    if (m.err != PROVEN_OK) return PROVEN_ERR_NOMEM;
    *buf = m.value.ptr;
    *cap = want;
    for (proven_size_t i = 0; i < slack; ++i) (*buf)[want + i] = 0;
    return PROVEN_OK;
}

/* Room for `room` more bytes after `hist_pos`: grow `hist` by doubling, up to what the frame
 * can need, and once it is that large, keep only the window's worth of history. */
static proven_err_t make_room(proven_zstd_decoder_t *z, proven_size_t room) {
    if (z->hist_cap - z->hist_pos >= room) return PROVEN_OK;
    if (z->lent) return PROVEN_ERR_INVALID_FORMAT;     /* not reached: no block passes the stated size */
    const proven_size_t want = z->hist_pos + room;
    if (z->hist_cap < z->full) {
        proven_size_t to = z->hist_cap < z->full / 2 ? z->hist_cap * 2 : z->full;
        if (to < want) to = want;
        if (to < 4096) to = 4096;
        if (to > z->full) to = z->full;
        proven_err_t e = grow(z, &z->hist, &z->hist_cap, to, WILD);
        if (e != PROVEN_OK) return e;
        if (z->hist_cap - z->hist_pos >= room) return PROVEN_OK;
    }
    const proven_size_t keep = z->hist_pos < z->window ? z->hist_pos : (proven_size_t)z->window;
    (void)proven_mem_move(z->hist, z->hist_cap, (proven_mem_view_t){ .ptr = z->hist + z->hist_pos - keep, .size = keep });
    z->hist_pos = keep;
    z->drain = keep;
    return z->hist_cap - z->hist_pos >= room ? PROVEN_OK : PROVEN_ERR_INVALID_FORMAT;
}

// -----------------------------------------------------------------------------
// Literals (RFC 8878, section 3.1.1.3.1)
// -----------------------------------------------------------------------------

/* The Huffman table description: weights, given directly or FSE-coded, from which the code
 * follows (section 4.2.1). */
static bool huf_read(proven_zstd_decoder_t *z, const proven_byte_t *p, proven_size_t len, proven_size_t *used) {
    if (len < 1) return false;
    proven_u8 w[256];
    unsigned n = 0;
    const unsigned hb = p[0];
    if (hb >= 128) {
        n = hb - 127;
        const proven_size_t bytes = (n + 1) / 2;
        if (1 + bytes > len) return false;
        for (unsigned i = 0; i < n; ++i) w[i] = (proven_u8)(i % 2 == 0 ? p[1 + i / 2] >> 4 : p[1 + i / 2] & 15);
        *used = 1 + bytes;
    } else {
        if (hb == 0 || 1 + (proven_size_t)hb > len) return false;
        const proven_byte_t *q = p + 1;
        proven_i16 norm[256];
        unsigned nsym = 0, al = 0;
        proven_size_t hu = 0;
        if (!fse_read(q, hb, 255, 6, norm, &nsym, &al, &hu)) return false;
        proven_fse_t t[64];
        if (!fse_build(t, norm, nsym, al)) return false;
        proven_bits_t b;
        if (!bits_init(&b, q + hu, hb - hu)) return false;
        /* Two states take turns over one stream; when it runs out, the other state gives one
         * last symbol. */
        proven_u32 s1 = (proven_u32)bits_read(&b, al), s2 = (proven_u32)bits_read(&b, al);
        for (;;) {
            if (n > 253) return false;
            w[n++] = t[s1].symbol;
            s1 = t[s1].next + (proven_u32)bits_read(&b, t[s1].bits);
            if (bits_over(&b)) { w[n++] = t[s2].symbol; break; }
            if (n > 253) return false;
            w[n++] = t[s2].symbol;
            s2 = t[s2].next + (proven_u32)bits_read(&b, t[s2].bits);
            if (bits_over(&b)) { w[n++] = t[s1].symbol; break; }
        }
        *used = 1 + (proven_size_t)hb;
    }
    /* The last symbol's weight is what makes the total a power of two. */
    proven_u32 rank[HUF_LOG_MAX + 2] = { 0 }, total = 0;
    for (unsigned i = 0; i < n; ++i) {
        if (w[i] > HUF_LOG_MAX) return false;
        rank[w[i]]++;
        total += (1u << w[i]) >> 1;
    }
    if (total == 0) return false;
    const unsigned log = highbit32(total) + 1;
    if (log > HUF_LOG_MAX) return false;
    const proven_u32 rest = (1u << log) - total;
    if ((rest & (rest - 1)) != 0) return false;
    const unsigned lastw = highbit32(rest) + 1;
    w[n++] = (proven_u8)lastw;
    rank[lastw]++;
    if (rank[1] < 2 || (rank[1] & 1)) return false;
    proven_u32 startpos[HUF_LOG_MAX + 2], next = 0;
    for (unsigned k = 1; k <= log; ++k) { startpos[k] = next; next += rank[k] << (k - 1); }
    for (unsigned s = 0; s < n; ++s) {
        if (w[s] == 0) continue;
        const proven_u32 count = 1u << (w[s] - 1);
        const proven_huf_t e = { .symbol = (proven_u8)s, .bits = (proven_u8)(log + 1 - w[s]) };
        for (proven_u32 u = 0; u < count; ++u) z->huf[startpos[w[s]] + u] = e;
        startpos[w[s]] += count;
    }
    z->huf_log = log;
    z->huf_ok = true;
    return true;
}

static inline proven_byte_t huf_one(const proven_huf_t *t, unsigned log, proven_bits_t *b) {
    const proven_huf_t e = t[bits_look(b, log)];
    b->used += e.bits;
    return e.symbol;
}

/* One Huffman stream of exactly `count` symbols, read to its beginning exactly. Four symbols
 * take at most 48 bits: one reload for every four. */
static bool huf_stream(const proven_zstd_decoder_t *z, const proven_byte_t *p, proven_size_t len, proven_byte_t *out, proven_size_t count) {
    proven_bits_t b;
    if (!bits_init(&b, p, len)) return false;
    const unsigned log = z->huf_log;
    const proven_huf_t *t = z->huf;
    proven_size_t i = 0;
    for (; i + 4 <= count; i += 4) {
        bits_reload(&b);
        out[i] = huf_one(t, log, &b);
        out[i + 1] = huf_one(t, log, &b);
        out[i + 2] = huf_one(t, log, &b);
        out[i + 3] = huf_one(t, log, &b);
    }
    bits_reload(&b);
    for (; i < count; ++i) out[i] = huf_one(t, log, &b);
    return bits_exact(&b);
}

/* Four streams, the first three `seg` symbols long and the last `last`, decoded side by side:
 * the four are independent, so the processor can work on all of them at once. */
static bool huf_four(const proven_zstd_decoder_t *z, const proven_byte_t *q, const proven_size_t sz[4], proven_byte_t *out, proven_size_t seg, proven_size_t last) {
    proven_bits_t b[4];
    const proven_byte_t *at = q;
    for (int k = 0; k < 4; ++k) {
        if (!bits_init(&b[k], at, sz[k])) return false;
        at += sz[k];
    }
    const unsigned log = z->huf_log;
    const proven_huf_t *t = z->huf;
    proven_byte_t *o0 = out, *o1 = out + seg, *o2 = out + 2 * seg, *o3 = out + 3 * seg;
    proven_size_t i = 0;
    for (; i + 4 <= last; i += 4) {
        bits_reload(&b[0]); bits_reload(&b[1]); bits_reload(&b[2]); bits_reload(&b[3]);
        for (int k = 0; k < 4; ++k) {
            o0[i + k] = huf_one(t, log, &b[0]);
            o1[i + k] = huf_one(t, log, &b[1]);
            o2[i + k] = huf_one(t, log, &b[2]);
            o3[i + k] = huf_one(t, log, &b[3]);
        }
    }
    for (int k = 0; k < 4; ++k) bits_reload(&b[k]);
    for (proven_size_t j = i; j < seg; ++j) {
        if (((j - i) & 3) == 0) { bits_reload(&b[0]); bits_reload(&b[1]); bits_reload(&b[2]); bits_reload(&b[3]); }
        o0[j] = huf_one(t, log, &b[0]);
        o1[j] = huf_one(t, log, &b[1]);
        o2[j] = huf_one(t, log, &b[2]);
        if (j < last) o3[j] = huf_one(t, log, &b[3]);
    }
    return bits_exact(&b[0]) && bits_exact(&b[1]) && bits_exact(&b[2]) && bits_exact(&b[3]);
}

static proven_err_t literals(proven_zstd_decoder_t *z, const proven_byte_t *src, proven_size_t n,
                             const proven_byte_t **lit, proven_size_t *nlit, proven_size_t *used) {
    if (n < 1) return PROVEN_ERR_INVALID_FORMAT;
    const unsigned b0 = src[0], type = b0 & 3, sf = (b0 >> 2) & 3;
    if (type <= 1) {
        proven_size_t regen, hs;
        if (sf == 0 || sf == 2) { regen = b0 >> 3; hs = 1; }
        else if (sf == 1) { if (n < 2) return PROVEN_ERR_INVALID_FORMAT; regen = (b0 >> 4) + ((proven_size_t)src[1] << 4); hs = 2; }
        else { if (n < 3) return PROVEN_ERR_INVALID_FORMAT; regen = (b0 >> 4) + ((proven_size_t)src[1] << 4) + ((proven_size_t)src[2] << 12); hs = 3; }
        if (regen > z->block_max) return PROVEN_ERR_INVALID_FORMAT;
        if (type == 0) {
            if (hs + regen > n) return PROVEN_ERR_INVALID_FORMAT;
            *lit = src + hs;
            *used = hs + regen;
        } else {
            if (hs + 1 > n) return PROVEN_ERR_INVALID_FORMAT;
            proven_err_t e = grow(z, &z->lits, &z->lits_cap, regen, WILD);
            if (e != PROVEN_OK) return e;
            for (proven_size_t i = 0; i < regen; ++i) z->lits[i] = src[hs];
            *lit = z->lits;
            *used = hs + 1;
        }
        *nlit = regen;
        return PROVEN_OK;
    }
    proven_size_t hs, regen, csize;
    const bool four = sf != 0;
    if (sf <= 1) {
        if (n < 3) return PROVEN_ERR_INVALID_FORMAT;
        const proven_u32 v = (proven_u32)src[0] | ((proven_u32)src[1] << 8) | ((proven_u32)src[2] << 16);
        hs = 3; regen = (v >> 4) & 0x3FF; csize = (v >> 14) & 0x3FF;
    } else if (sf == 2) {
        if (n < 4) return PROVEN_ERR_INVALID_FORMAT;
        const proven_u32 v = le32(src);
        hs = 4; regen = (v >> 4) & 0x3FFF; csize = (v >> 18) & 0x3FFF;
    } else {
        if (n < 5) return PROVEN_ERR_INVALID_FORMAT;
        const proven_u64 v = (proven_u64)le32(src) | ((proven_u64)src[4] << 32);
        hs = 5; regen = (proven_size_t)((v >> 4) & 0x3FFFF); csize = (proven_size_t)((v >> 22) & 0x3FFFF);
    }
    if (regen > z->block_max || hs + csize > n) return PROVEN_ERR_INVALID_FORMAT;
    if (four && regen < 6) return PROVEN_ERR_INVALID_FORMAT;
    const proven_byte_t *p = src + hs;
    proven_size_t m = csize;
    if (type == 2) {
        proven_size_t tu = 0;
        if (!huf_read(z, p, m, &tu)) { z->huf_ok = false; return PROVEN_ERR_INVALID_FORMAT; }
        p += tu;
        m -= tu;
    } else if (!z->huf_ok) {
        return PROVEN_ERR_INVALID_FORMAT;
    }
    proven_err_t e = grow(z, &z->lits, &z->lits_cap, regen, WILD);
    if (e != PROVEN_OK) return e;
    if (!four) {
        if (!huf_stream(z, p, m, z->lits, regen)) return PROVEN_ERR_INVALID_FORMAT;
    } else {
        if (m < 6) return PROVEN_ERR_INVALID_FORMAT;
        const proven_size_t s1 = (proven_size_t)p[0] | ((proven_size_t)p[1] << 8);
        const proven_size_t s2 = (proven_size_t)p[2] | ((proven_size_t)p[3] << 8);
        const proven_size_t s3 = (proven_size_t)p[4] | ((proven_size_t)p[5] << 8);
        if (6 + s1 + s2 + s3 > m) return PROVEN_ERR_INVALID_FORMAT;
        const proven_size_t s4 = m - 6 - s1 - s2 - s3;
        const proven_size_t seg = (regen + 3) / 4;
        if (3 * seg > regen) return PROVEN_ERR_INVALID_FORMAT;
        const proven_size_t sz[4] = { s1, s2, s3, s4 };
        if (!huf_four(z, p + 6, sz, z->lits, seg, regen - 3 * seg)) return PROVEN_ERR_INVALID_FORMAT;
    }
    *lit = z->lits;
    *nlit = regen;
    *used = hs + csize;
    return PROVEN_OK;
}

// -----------------------------------------------------------------------------
// Sequences (RFC 8878, section 3.1.1.3.2) and their execution
// -----------------------------------------------------------------------------

static bool seq_table(unsigned mode, const proven_byte_t *p, proven_size_t n, proven_size_t *at,
                      const proven_seq_t **cur, unsigned *curlog, proven_seq_t *dyn, const proven_seq_t *def, unsigned deflog,
                      unsigned max_sym, unsigned max_log, int kind) {
    switch (mode) {
        case 0:
            *cur = def;
            *curlog = deflog;
            return true;
        case 1: {
            if (*at >= n || p[*at] > max_sym) return false;
            const proven_fse_t one = { .next = 0, .symbol = p[*at], .bits = 0 };
            seq_from(dyn, &one, 0, kind);
            (*at)++;
            *cur = dyn;
            *curlog = 0;
            return true; }
        case 2: {
            proven_i16 norm[64];
            proven_fse_t t[1u << LL_LOG];
            unsigned nsym = 0, al = 0;
            proven_size_t u = 0;
            if (!fse_read(p + *at, n - *at, max_sym, max_log, norm, &nsym, &al, &u)) return false;
            if (!fse_build(t, norm, nsym, al)) return false;
            seq_from(dyn, t, al, kind);
            *at += u;
            *cur = dyn;
            *curlog = al;
            return true; }
        default:
            return *cur != NULL;
    }
}

static void copy_bytes(proven_byte_t *d, const proven_byte_t *s, proven_size_t n) {
    if (n == 0) return;
    if (n >= 32) { (void)proven_mem_copy(d, n, (proven_mem_view_t){ .ptr = s, .size = n }); return; }
    for (proven_size_t i = 0; i < n; ++i) d[i] = s[i];
}

/* Eight and sixteen bytes at once; the compiler makes each one load and one store. */
static inline void copy8(proven_byte_t *d, const proven_byte_t *s) { __builtin_memcpy(d, s, 8); }
static inline void copy16(proven_byte_t *d, const proven_byte_t *s) { __builtin_memcpy(d, s, 16); }

/* `n` bytes from `s` to `d`, sixteen at a time: up to fifteen beyond the end are written, and
 * read. The caller has checked that both have that slack and that they do not overlap. */
static inline void copy_wild(proven_byte_t *d, const proven_byte_t *s, proven_size_t n) {
    copy16(d, s);                                /* most are sixteen or fewer: one copy, no loop */
    for (proven_size_t i = 16; i < n; i += 16) copy16(d + i, s + i);
}

/* A match: `off` back, `len` long. When `off` is shorter than `len` the copy reads what it
 * has just written, and that is what it means: each block copied at once must lie wholly in
 * what was written before it. So 16 at a time from 16 back, 8 at a time from 8 back, and
 * closer than 8 the first bytes one at a time, until the pattern of period `off` is written
 * out to `step` bytes - the smallest multiple of `off` that is at least 8 - and the rest eight
 * at a time from `step` back, which repeats the same pattern. `room`: bytes that may be
 * written from `d`, the slack included - the wide copies write up to fifteen beyond the match. */
static inline void copy_match(proven_byte_t *d, proven_size_t off, proven_size_t len, proven_size_t room) {
    const proven_byte_t *s = d - off;
    if (len + 16 <= room) {
        if (off >= 16) { copy_wild(d, s, len); return; }
        if (off >= 8) { for (proven_size_t i = 0; i < len; i += 8) copy8(d + i, s + i); return; }
        const proven_size_t step = off * ((8 + off - 1) / off);
        proven_size_t i = 0;
        for (; i < step && i < len; ++i) d[i] = s[i];
        for (; i < len; i += 8) copy8(d + i, d + i - step);
        return;
    }
    for (proven_size_t i = 0; i < len; ++i) d[i] = s[i];
}

static proven_err_t sequences(proven_zstd_decoder_t *z, const proven_byte_t *p, proven_size_t n,
                              const proven_byte_t *lit, proven_size_t nlit, proven_size_t limit) {
    if (n < 1) return PROVEN_ERR_INVALID_FORMAT;
    proven_size_t nseq, at;
    const unsigned b0 = p[0];
    if (b0 == 0) {
        if (n != 1) return PROVEN_ERR_INVALID_FORMAT;
        nseq = 0; at = 1;
    } else if (b0 < 128) {
        nseq = b0; at = 1;
    } else if (b0 < 255) {
        if (n < 2) return PROVEN_ERR_INVALID_FORMAT;
        nseq = ((proven_size_t)(b0 - 128) << 8) + p[1]; at = 2;
    } else {
        if (n < 3) return PROVEN_ERR_INVALID_FORMAT;
        nseq = (proven_size_t)p[1] + ((proven_size_t)p[2] << 8) + 0x7F00; at = 3;
    }
    if (nseq == 0 && at != n) return PROVEN_ERR_INVALID_FORMAT;
    proven_byte_t *h = z->hist;
    proven_size_t lp = 0;
    proven_size_t op = z->hist_pos;
    if (nseq > 0) {
        if (at >= n) return PROVEN_ERR_INVALID_FORMAT;
        const unsigned modes = p[at++];
        if ((modes & 3) != 0) return PROVEN_ERR_INVALID_FORMAT;
        if (!seq_table(modes >> 6, p, n, &at, &z->llt, &z->lll, z->ll_dyn, z->ll_def, 6, LL_MAX, LL_LOG, SEQ_LL) ||
            !seq_table((modes >> 4) & 3, p, n, &at, &z->oft, &z->ofl, z->of_dyn, z->of_def, 5, OF_MAX, OF_LOG, SEQ_OF) ||
            !seq_table((modes >> 2) & 3, p, n, &at, &z->mlt, &z->mll, z->ml_dyn, z->ml_def, 6, ML_MAX, ML_LOG, SEQ_ML)) {
            return PROVEN_ERR_INVALID_FORMAT;
        }
        proven_bits_t b;
        if (!bits_init(&b, p + at, n - at)) return PROVEN_ERR_INVALID_FORMAT;
        const proven_seq_t *llt = z->llt, *oft = z->oft, *mlt = z->mlt;
        proven_u32 ls = (proven_u32)bits_read(&b, z->lll), os = (proven_u32)bits_read(&b, z->ofl), ms = (proven_u32)bits_read(&b, z->mll);
        /* What may be written from `h` and read from `lit`, the slack included. */
        const proven_size_t wend = z->hist ? z->hist_cap + z->slack : 0, lend = lit && lit == z->lits ? z->lits_cap + WILD : nlit;
        lp = 0;
        /* In locals for the loop: every byte stored through `h` might, for the compiler, be a
         * store to `*z`, which would make it read these again on every sequence. */
        proven_u64 rep0 = z->rep[0], rep1 = z->rep[1], rep2 = z->rep[2];
        const proven_u64 window = z->window;
        proven_err_t verdict = PROVEN_OK;
        for (proven_size_t i = 0; i < nseq; ++i) {
            /* At most 31 bits, then 32, then 26. After a reload at least 56 are held (`used`
             * at most 7), so a second reload is needed only when what follows would run past
             * 64 - rarely: long offsets with long extra bits. */
            bits_reload(&b);
            const proven_seq_t o = oft[os], m = mlt[ms], l = llt[ls];
            const proven_u64 ofv = (proven_u64)o.base + bits_read(&b, o.extra);
            if (b.used + m.extra + l.extra > 64) bits_reload(&b);
            const proven_size_t ml = m.base + (proven_size_t)bits_read(&b, m.extra);
            const proven_size_t ll = l.base + (proven_size_t)bits_read(&b, l.extra);
            proven_u64 off;
            if (ofv > 3) {
                off = ofv - 3;
                rep2 = rep1; rep1 = rep0; rep0 = off;
            } else {
                const unsigned idx = (unsigned)ofv - 1 + (ll == 0);
                if (idx == 0) {
                    off = rep0;
                } else {
                    off = idx == 1 ? rep1 : idx == 2 ? rep2 : rep0 - 1;
                    if (idx != 1) rep2 = rep1;
                    rep1 = rep0;
                    rep0 = off;
                }
            }
            if (i + 1 < nseq) {
                if (b.used + LL_LOG + ML_LOG + OF_LOG > 64) bits_reload(&b);
                ls = l.next + (proven_u32)bits_read(&b, l.bits);
                ms = m.next + (proven_u32)bits_read(&b, m.bits);
                os = o.next + (proven_u32)bits_read(&b, o.bits);
            }
            if (ll > nlit - lp || ll > limit - op) { verdict = PROVEN_ERR_INVALID_FORMAT; break; }
            if (ll > 0) {
                if (op + ll + 16 <= wend && lp + ll + 16 <= lend) copy_wild(h + op, lit + lp, ll);
                else copy_bytes(h + op, lit + lp, ll);
            }
            op += ll;
            lp += ll;
            /* Further back than this frame has produced, or than its window reaches. */
            if (ml > limit - op || off == 0 || off > op || off > window) { verdict = PROVEN_ERR_INVALID_FORMAT; break; }
            if (ml > 0) copy_match(h + op, (proven_size_t)off, ml, wend - op);
            op += ml;
        }
        z->rep[0] = rep0; z->rep[1] = rep1; z->rep[2] = rep2;
        if (verdict != PROVEN_OK) return verdict;
        if (!bits_exact(&b)) return PROVEN_ERR_INVALID_FORMAT;
        nlit -= lp;
    } else {
        lp = 0;
    }
    if (nlit > limit - op) return PROVEN_ERR_INVALID_FORMAT;
    if (nlit > 0) copy_bytes(h + op, lit + lp, nlit);
    z->hist_pos = op + nlit;
    return PROVEN_OK;
}

/* The most this block may produce: its maximum, and what the frame's stated size leaves. */
static proven_size_t block_room(const proven_zstd_decoder_t *z) {
    proven_size_t room = z->block_max;
    if (z->has_fcs && z->fcs - z->total < room) room = (proven_size_t)(z->fcs - z->total);
    return room;
}

/* Decode a whole compressed block after `hist_pos`; `*from` is where its output begins. */
static proven_err_t compressed_block(proven_zstd_decoder_t *z, const proven_byte_t *src, proven_size_t n, proven_size_t *from) {
    const proven_size_t room = block_room(z);
    proven_err_t e = make_room(z, room);
    if (e != PROVEN_OK) return e;
    *from = z->hist_pos;
    const proven_byte_t *lit = NULL;
    proven_size_t nlit = 0, used = 0;
    e = literals(z, src, n, &lit, &nlit, &used);
    if (e != PROVEN_OK) return e;
    return sequences(z, src + used, n - used, lit, nlit, z->hist_pos + room);
}

// -----------------------------------------------------------------------------
// Frames
// -----------------------------------------------------------------------------

#define MAGIC ZSTD_MAGIC
#define SKIP_MAGIC 0x184D2A50u

static proven_err_t frame_header(proven_zstd_decoder_t *z) {
    const unsigned fhd = z->hdr[0];
    const unsigned fcs_flag = fhd >> 6, did_flag = fhd & 3;
    const bool single = (fhd >> 5) & 1;
    if (fhd & 0x08) return PROVEN_ERR_INVALID_FORMAT;           /* the reserved bit */
    unsigned at = 1;
    proven_u64 window = 0;
    if (!single) {
        const unsigned wd = z->hdr[at++];
        const unsigned wlog = 10 + (wd >> 3);
        const proven_u64 base = (proven_u64)1 << wlog;
        window = base + (base >> 3) * (wd & 7);
    }
    static const unsigned DID_SIZE[4] = { 0, 1, 2, 4 };
    proven_u32 did = 0;
    for (unsigned i = 0; i < DID_SIZE[did_flag]; ++i) did |= (proven_u32)z->hdr[at + i] << (8 * i);
    at += DID_SIZE[did_flag];
    const unsigned fcs_size = fcs_flag == 0 ? (single ? 1 : 0) : 1u << fcs_flag;
    proven_u64 fcs = 0;
    for (unsigned i = 0; i < fcs_size; ++i) fcs |= (proven_u64)z->hdr[at + i] << (8 * i);
    if (fcs_size == 2) fcs += 256;
    if (single) window = fcs;
    if (did != 0) return PROVEN_ERR_UNSUPPORTED;
    if (window > z->window_max) return PROVEN_ERR_OUT_OF_BOUNDS;
    z->window = window;
    z->has_fcs = fcs_size > 0;
    z->fcs = fcs;
    z->has_check = (fhd >> 2) & 1;
    z->block_max = window < BLOCK_MAX ? (proven_size_t)window : BLOCK_MAX;
    z->full = (proven_size_t)window + z->block_max;
    if (z->has_fcs && fcs < z->full) z->full = (proven_size_t)fcs;
    z->total = 0;
    z->hist_pos = 0;
    z->drain = 0;
    z->rep[0] = 1; z->rep[1] = 4; z->rep[2] = 8;
    z->huf_ok = false;
    z->llt = z->oft = z->mlt = NULL;
    xxh_init(&z->xxh);
    return PROVEN_OK;
}

/* Gather header bytes into `hdr` until there are `need`. */
static bool gather(proven_zstd_decoder_t *z, proven_mem_view_t in, proven_size_t *ia) {
    while (z->have < z->need && *ia < in.size) z->hdr[z->have++] = in.ptr[(*ia)++];
    return z->have == z->need;
}

/* Bytes [from, hist_pos) are new output of this frame. */
static void produced_now(proven_zstd_decoder_t *z, proven_size_t from) {
    const proven_size_t n = z->hist_pos - from;
    if (n == 0) return;
    if (z->has_check) xxh_update(&z->xxh, z->hist + from, n);
    z->total += n;
}

static proven_err_t bad(proven_zstd_decoder_t *z, proven_err_t e) {
    z->error = e;
    return e;
}

proven_err_t proven_zstd_decode(proven_zstd_decoder_t *z, proven_mem_view_t in, proven_size_t *consumed,
                                proven_mem_mut_t out, proven_size_t *produced, bool *done) {
    if (consumed) *consumed = 0;
    if (produced) *produced = 0;
    if (done) *done = false;
    if (!z || !consumed || !produced || !done || (in.size > 0 && !in.ptr) || (out.size > 0 && !out.ptr)) return PROVEN_ERR_INVALID_ARG;
    if (z->error != PROVEN_OK) return z->error;
    proven_size_t ia = 0, oa = 0;
    proven_err_t result = PROVEN_OK;
    bool going = true;
    while (going) {
        switch (z->state) {
            case Z_MAGIC:
                if (z->boundary && ia == in.size) { going = false; break; }
                z->need = 4;
                if (!gather(z, in, &ia)) { if (z->have > 0) z->boundary = false; going = false; break; }
                z->boundary = false;
                z->have = 0;
                {
                    const proven_u32 magic = le32(z->hdr);
                    if (magic == MAGIC) {
                        z->state = Z_FHD;
                    } else if ((magic & 0xFFFFFFF0u) == SKIP_MAGIC) {
                        z->state = Z_SKIP_SIZE;
                    } else {
                        result = bad(z, PROVEN_ERR_INVALID_FORMAT); going = false;
                    }
                }
                break;
            case Z_SKIP_SIZE:
                z->need = 4;
                if (!gather(z, in, &ia)) { going = false; break; }
                z->have = 0;
                z->skip = le32(z->hdr);
                z->state = Z_SKIP;
                break;
            case Z_SKIP: {
                const proven_size_t avail = in.size - ia;
                const proven_size_t k = z->skip < avail ? (proven_size_t)z->skip : avail;
                ia += k;
                z->skip -= k;
                if (z->skip > 0) { going = false; break; }
                z->state = Z_MAGIC;
                z->boundary = true;
                going = false;
                break; }
            case Z_FHD: {
                z->need = 1;
                if (!gather(z, in, &ia)) { going = false; break; }
                const unsigned fhd = z->hdr[0], fcs_flag = fhd >> 6;
                const bool single = (fhd >> 5) & 1;
                static const unsigned DID_SIZE[4] = { 0, 1, 2, 4 };
                z->need = 1 + (single ? 0 : 1) + DID_SIZE[fhd & 3] + (fcs_flag == 0 ? (single ? 1 : 0) : 1u << fcs_flag);
                z->state = Z_HEADER;
                break; }
            case Z_HEADER: {
                if (!gather(z, in, &ia)) { going = false; break; }
                z->have = 0;
                proven_err_t e = frame_header(z);
                if (e != PROVEN_OK) { result = bad(z, e); going = false; break; }
                if (z->direct && out.ptr && z->has_fcs && z->fcs <= out.size - oa) {
                    if (!z->lent) { z->own = z->hist; z->own_cap = z->hist_cap; z->lent = true; }
                    z->hist = out.ptr + oa;
                    z->hist_cap = (proven_size_t)z->fcs;
                    const proven_size_t past = out.size - oa - z->hist_cap;
                    z->slack = past < WILD ? past : WILD;
                } else if (z->lent) {
                    z->hist = z->own; z->hist_cap = z->own_cap; z->lent = false; z->slack = WILD;
                }
                z->state = Z_BLOCK;
                break; }
            case Z_BLOCK: {
                z->need = 3;
                if (!gather(z, in, &ia)) { going = false; break; }
                z->have = 0;
                const proven_u32 v = (proven_u32)z->hdr[0] | ((proven_u32)z->hdr[1] << 8) | ((proven_u32)z->hdr[2] << 16);
                z->last = v & 1;
                z->block_type = (v >> 1) & 3;
                z->block_size = v >> 3;
                z->block_have = 0;
                if (z->block_type == 3 || z->block_size > z->block_max ||
                    (z->block_type != 2 && z->block_size > block_room(z))) {
                    result = bad(z, PROVEN_ERR_INVALID_FORMAT); going = false; break;
                }
                if (z->block_type == 2) {
                    /* A compressed block of no bytes falls through to the literals, which
                     * refuse it: RFC 8878 gives it two section headers. libzstd's streaming
                     * decoder takes it as empty, its one-call decoder refuses it. */
                    if (in.size - ia >= z->block_size) {
                        /* All of it is here: decoded where it lies. */
                        proven_size_t from = 0;
                        proven_err_t e = compressed_block(z, in.ptr ? in.ptr + ia : in.ptr, z->block_size, &from);
                        if (e != PROVEN_OK) { result = bad(z, e); going = false; break; }
                        ia += z->block_size;
                        produced_now(z, from);
                        z->state = Z_DRAIN;
                        z->after = z->last ? Z_CHECK : Z_BLOCK;
                    } else {
                        proven_err_t e = grow(z, &z->inbuf, &z->inbuf_cap, z->block_size, 0);
                        if (e != PROVEN_OK) { result = bad(z, e); going = false; break; }
                        z->state = Z_DATA;
                    }
                } else {
                    proven_err_t e = make_room(z, z->block_size);
                    if (e != PROVEN_OK) { result = bad(z, e); going = false; break; }
                    z->state = z->block_type == 0 ? Z_RAW : Z_RLE;
                }
                break; }
            case Z_DATA: {
                const proven_size_t want = z->block_size - z->block_have, avail = in.size - ia;
                const proven_size_t k = want < avail ? want : avail;
                copy_bytes(z->inbuf + z->block_have, in.ptr + ia, k);
                z->block_have += k;
                ia += k;
                if (z->block_have < z->block_size) { going = false; break; }
                proven_size_t from = 0;
                proven_err_t e = compressed_block(z, z->inbuf, z->block_size, &from);
                if (e != PROVEN_OK) { result = bad(z, e); going = false; break; }
                produced_now(z, from);
                z->state = Z_DRAIN;
                z->after = z->last ? Z_CHECK : Z_BLOCK;
                break; }
            case Z_RAW: {
                const proven_size_t want = z->block_size - z->block_have, avail = in.size - ia;
                const proven_size_t k = want < avail ? want : avail;
                const proven_size_t from = z->hist_pos;
                if (k > 0) copy_bytes(z->hist + z->hist_pos, in.ptr + ia, k);
                z->hist_pos += k;
                z->block_have += k;
                ia += k;
                produced_now(z, from);
                z->state = Z_DRAIN;
                z->after = z->block_have < z->block_size ? Z_RAW : z->last ? Z_CHECK : Z_BLOCK;
                if (k == 0 && z->block_have < z->block_size) going = false;
                break; }
            case Z_RLE: {
                if (ia == in.size) { going = false; break; }
                const proven_byte_t b = in.ptr[ia++];
                const proven_size_t from = z->hist_pos;
                for (proven_size_t i = 0; i < z->block_size; ++i) z->hist[z->hist_pos + i] = b;
                z->hist_pos += z->block_size;
                produced_now(z, from);
                z->state = Z_DRAIN;
                z->after = z->last ? Z_CHECK : Z_BLOCK;
                break; }
            case Z_DRAIN: {
                const proven_size_t pending = z->hist_pos - z->drain, space = out.size - oa;
                const proven_size_t k = pending < space ? pending : space;
                if (k > 0 && out.ptr + oa != z->hist + z->drain) copy_bytes(out.ptr + oa, z->hist + z->drain, k);
                oa += k;
                z->drain += k;
                if (z->drain < z->hist_pos) { going = false; break; }
                z->state = z->after;
                if (z->state == Z_CHECK) {
                    if (z->has_fcs && z->total != z->fcs) { result = bad(z, PROVEN_ERR_INVALID_FORMAT); going = false; break; }
                    if (!z->has_check) { z->state = Z_MAGIC; z->boundary = true; going = false; }
                }
                break; }
            case Z_CHECK: {
                z->need = 4;
                if (!gather(z, in, &ia)) { going = false; break; }
                z->have = 0;
                if (le32(z->hdr) != (proven_u32)xxh_digest(&z->xxh)) { result = bad(z, PROVEN_ERR_INVALID_FORMAT); going = false; break; }
                z->state = Z_MAGIC;
                z->boundary = true;
                going = false;
                break; }
            default:
                going = false;
                break;
        }
    }
    *consumed = ia;
    *produced = oa;
    *done = z->state == Z_MAGIC && z->boundary && z->have == 0;
    return result;
}

proven_err_t proven_zstd_decode_all(proven_allocator_t alloc, proven_mem_view_t in, proven_mem_mut_t out, proven_size_t *written) {
    if (written) *written = 0;
    if (!written || (in.size > 0 && !in.ptr) || (out.size > 0 && !out.ptr)) return PROVEN_ERR_INVALID_ARG;
    proven_zstd_decoder_t *z = NULL;
    proven_err_t e = proven_zstd_decoder_create(alloc, 0, &z);
    if (e != PROVEN_OK) return e;
    z->direct = true;
    proven_size_t ia = 0, oa = 0;
    for (;;) {
        proven_size_t used = 0, made = 0;
        bool done = false;
        e = proven_zstd_decode(z, (proven_mem_view_t){ .ptr = in.ptr ? in.ptr + ia : in.ptr, .size = in.size - ia }, &used,
                               (proven_mem_mut_t){ .ptr = out.ptr ? out.ptr + oa : out.ptr, .size = out.size - oa }, &made, &done);
        ia += used;
        oa += made;
        if (e != PROVEN_OK) break;
        if (done && ia == in.size) break;
        if (used == 0 && made == 0) {
            /* No progress: output waiting for room, or input that ended inside a frame. */
            e = z->drain < z->hist_pos ? PROVEN_ERR_OUT_OF_BOUNDS : PROVEN_ERR_INVALID_FORMAT;
            break;
        }
    }
    proven_zstd_decoder_destroy(z);
    if (e == PROVEN_OK) *written = oa;
    return e;
}
