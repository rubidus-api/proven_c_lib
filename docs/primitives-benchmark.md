# Primitive throughput — hashes, encoders, and random

This is the throughput of the byte-level primitives added in the v26.07.13 line — the four
hashes, the two encoders, and the two random generators — measured so that a regression shows
up as a number rather than a feeling. It is a companion to
[`float-correctness-and-performance.md`](float-correctness-and-performance.md), which covers the
decimal engine.

Timing, not correctness. The correctness of each of these is established elsewhere, against the
relevant standard's own vectors: the hashes against their KATs and a differential vs Python
`hashlib`/`zlib`, the encoders against RFC 4648's vectors and Python `base64`/`binascii`, ChaCha20
against RFC 8439 and OpenSSL, xoshiro against the upstream reference. This document is only about
speed.

## Method

`tests/test_bench_primitives.c`, run through `./nob bench-float` (which builds the benchmark set
at `-O3`). Each backend runs over a fixed 4 KiB pseudo-random buffer for a fixed number of
rounds, wrapped in `proven_time_now()`. Every result is folded into an FNV checksum that is
printed with the timing, for two reasons: the optimiser cannot delete work whose result is
observed, so the loop measures what it claims to; and a checksum that drifts between runs or
after a change is a **correctness** failure surfaced by the benchmark, not a timing one. (The
CRC-32 optimisation below was accepted only because its checksum was byte-identical before and
after.)

The numbers are single-threaded, on one x86-64 development machine, and they are a **relative
baseline for this code over time** — not a claim about other hardware, other buffer sizes, or a
comparison against other libraries. Read them as ratios, not absolutes.

## Results

Median of three runs, 4 KiB buffer, `-O3`:

| Primitive | ns/byte | MB/s | Notes |
|---|---:|---:|---|
| **Hashes** | | | |
| SipHash-2-4 (`proven_hash_keyed`) | 0.47 | ~2100 | Processes 8 bytes per round; the keyed default for untrusted map keys. |
| FNV-1a (`proven_hash_bytes`) | 1.15 | ~870 | Byte-serial by construction — each byte depends on the last — so it cannot pipeline the way SipHash does. Fine for a hash table; this is why it is not the choice for bulk data. |
| CRC-32 (`proven_crc32`) | 2.31 | ~432 | Table-driven (see below). |
| SHA-256 (`proven_sha256`) | 5.9 | ~170 | A portable C compression function, no SHA-NI. Cryptographic; the slowest, as expected. |
| **Encoders** | | | |
| Base64 (`proven_base64_encode`) | 0.70 | ~1420 | 3 bytes → 4 chars, table lookups. |
| hex (`proven_hex_encode`) | 0.96 | ~1040 | 1 byte → 2 chars. |
| **Random (bulk fill)** | | | |
| xoshiro256** (`proven_rng_fill`) | 0.28 | ~3500 | Fast and reproducible; not secret-grade. |
| ChaCha20 (`proven_chacha_rng_fill`) | 2.42 | ~415 | Cryptographic; ~8.5× the cost of xoshiro, which is the price of being unguessable and exactly why the two carry different names. |

Two things worth reading off the table, because they are the reason the API is shaped the way it
is:

- **SipHash is faster than FNV here, despite doing more cryptographic work.** FNV-1a is a serial
  dependency chain — byte *n*'s hash needs byte *n−1*'s — so a modern core cannot overlap the
  work; SipHash consumes a 64-bit word per round and pipelines. FNV is still the right default
  for a *trusted-key* hash table, where keys are short and the point is simplicity, but it is not
  a bulk hash, and the numbers say so.

- **ChaCha20 costs about 8.5× xoshiro.** That gap is not a defect to optimise away; it is the
  difference between "reproducible" and "unguessable". The library gives them names that cannot
  be confused precisely so that a caller who needs the cheap one does not reach for the expensive
  one, and vice versa.

## The one optimisation this benchmark drove

The first run put CRC-32 at **~104 MB/s** — an outlier, four times slower than FNV and far below
the others. The cause was visible in one read of the code: the CRC was the textbook *bitwise*
form, eight shift-and-conditional-xor iterations per byte, with no table at all. It is the
smallest correct CRC and the slowest.

Replacing it with the standard 256-entry reflected table (polynomial `0xEDB88320`) turns those
eight iterations into a single lookup:

```
c = (c >> 8) ^ CRC32_TABLE[(c ^ byte) & 0xFF];
```

That took CRC-32 from **~104 MB/s to ~432 MB/s — a 4.2× speedup** — for 1 KiB of `.rodata`. A
`slice-by-8` variant would roughly double it again, but at 8 KiB of tables; for a checksum that is
rarely the hot path in a library that values staying compact, the 1 KiB table is the right point
on that curve, and the doc records the choice rather than hiding it. The table is generated and
its `"123456789" → 0xcbf43926` check value is verified against it; the output is byte-identical to
the bitwise form (the benchmark's checksum did not move) and to `zlib.crc32` over random inputs.

## Reproducing

```sh
cc nob.c -o nob
./nob bench-float      # builds the benchmark set at -O3 and runs it
```

The `backend=… ns_per_byte=… MB_per_s=… checksum=…` lines are the raw output. A checksum that
differs from a prior run for the same backend is a behaviour change to investigate before the
timing.
