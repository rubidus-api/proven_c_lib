# Chapter 16: Compression

**Part III - Data structures. Prerequisites: [Chapter 2](manual-02-allocation.md) for the allocator a
compressor is made with; [Chapter 4](manual-04-containers-algorithms.md) for the checksums.**
**After this chapter** you can compress and decompress a buffer or a stream of any length,
read and write the `gzip` and `zlib` wrappers, decide how much a stranger's data may expand
to before you refuse it - and say what was tested and how fast it is.

This chapter covers `deflate.h`. It is pure computation - one allocation when a compressor or
decompressor is made, nothing from the operating system - and is available in a
[freestanding](manual-freestanding.md) build.

## Table of contents

1. [What DEFLATE does, in one page](#1-what-deflate-does-in-one-page)
2. [A whole buffer](#2-a-whole-buffer)
3. [A stream](#3-a-stream)
4. [Levels, windows and what they cost](#4-levels-windows-and-what-they-cost)
5. [Data from somebody else](#5-data-from-somebody-else)
6. [What is accepted, what is not here, and how this was tested](#6-what-is-accepted-what-is-not-here-and-how-this-was-tested)

## 1. What DEFLATE does, in one page

DEFLATE (RFC 1951) is the compression inside `.gz` files, HTTP's `Content-Encoding: gzip`,
PNG images, ZIP archives and WebSocket's `permessage-deflate`. It is thirty years old, it is
not the best there is, and everything reads it.

It does two things, one after the other:

- **It replaces repeats by references.** Wherever the next few bytes have appeared before,
  within the last 32 KiB, it writes "the same as *distance* bytes ago, for *length* bytes"
  instead of the bytes. A log file, a web page or source code is mostly repeats.
- **It writes what is left in codes of different lengths**: short for what is common, long
  for what is rare.

So text shrinks to a third or a quarter, a table of numbers often to less, and **data that
is already compressed or encrypted does not shrink at all** - it grows by a few bytes.

The compressed data needs something around it to be a file or a message. There are three
choices, and **both ends must make the same one**:

| Format | Around the data | Where it is used |
|---|---|---|
| `PROVEN_DEFLATE_GZIP` | A ten-byte header and, at the end, a CRC-32 of the data and its length (RFC 1952) | `.gz` files; HTTP's `gzip` |
| `PROVEN_DEFLATE_ZLIB` | A two-byte header and an Adler-32 of the data (RFC 1950) | PNG; HTTP's `deflate` |
| `PROVEN_DEFLATE_RAW` | Nothing | Inside another format that has its own framing: ZIP entries, `permessage-deflate` |

The two with a checksum notice damage. Raw does not: a changed bit gives other data or an
error, and nothing says which.

## 2. A whole buffer

```c
proven_size_t proven_deflate_bound(proven_deflate_format_t format, proven_size_t input_size);
proven_err_t proven_deflate_all(const proven_deflate_options_t *options, proven_mem_view_t in,
                                proven_mem_mut_t out, proven_size_t *written);
proven_err_t proven_inflate_all(proven_allocator_t alloc, proven_deflate_format_t format, proven_mem_view_t in,
                                proven_mem_mut_t out, proven_size_t *written, proven_size_t *consumed);
```

When the data is in memory and so is the room for the result, these two calls are all there
is to it. Both write into a buffer of yours.

- **Compressing** needs a buffer of `proven_deflate_bound` bytes to be sure of fitting - the
  input's size plus a little, because data that does not compress grows slightly. With less,
  it may fail with `PROVEN_ERR_OUT_OF_BOUNDS`.
- **Decompressing** needs you to decide how large the result may be, because the compressed
  data does not get to. **The size of `out` is the limit**: a stream that would produce more
  is refused with `PROVEN_ERR_OUT_OF_BOUNDS`, however small it is itself. Section 5 says why
  there is no version of this that grows a buffer for you.

Compiled and run by the test suite:

<!-- example: manual/examples/en/ex_16_gzip.c -->
```c
#include <string.h>

/*
 * Compress a buffer, get it back, and refuse to be handed more than was agreed: the two
 * whole-buffer calls, and the one rule about decompressing what somebody else sent.
 */

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();

    /* Something that compresses: text repeats itself. */
    static proven_byte_t text[4000];
    static const char line[] = "GET /index.html HTTP/1.1 - 200 - text/html; charset=utf-8\n";
    for (proven_size_t i = 0; i < sizeof text; ++i) text[i] = (proven_byte_t)line[i % (sizeof line - 1)];

    /* Compressing never fails for want of room if the buffer is as large as the bound says. */
    static proven_byte_t packed[4200];
    proven_size_t packed_len = 0;
    proven_deflate_options_t options = { .alloc = heap, .format = PROVEN_DEFLATE_GZIP };
    EXAMPLE_REQUIRE(proven_deflate_bound(PROVEN_DEFLATE_GZIP, sizeof text) <= sizeof packed, "the bound for 4,000 bytes is a little more than 4,000");
    EXAMPLE_REQUIRE(proven_deflate_all(&options, (proven_mem_view_t){ text, sizeof text }, (proven_mem_mut_t){ packed, sizeof packed }, &packed_len) == PROVEN_OK,
                    "4,000 bytes of log lines compress");
    EXAMPLE_REQUIRE(packed_len < 200, "to less than 200: the same line, over and over");
    EXAMPLE_REQUIRE(packed[0] == 0x1f && packed[1] == 0x8b, "and what came out is a gzip member - a .gz file, or an HTTP body with Content-Encoding: gzip");

    /* Decompressing: the output buffer is the limit. Here it is exactly as large as the data. */
    static proven_byte_t back[4000];
    proven_size_t back_len = 0, used = 0;
    EXAMPLE_REQUIRE(proven_inflate_all(heap, PROVEN_DEFLATE_GZIP, (proven_mem_view_t){ packed, packed_len }, (proven_mem_mut_t){ back, sizeof back }, &back_len, &used) == PROVEN_OK,
                    "it decompresses");
    EXAMPLE_REQUIRE(back_len == sizeof text && memcmp(back, text, sizeof text) == 0 && used == packed_len, "to exactly what went in, using every byte of the member");

    /* The same member into a smaller buffer is refused. This is the whole defence against a
     * "decompression bomb": you say how much you are willing to receive, and that is all the
     * room there is. A few hundred bytes cannot make this program allocate a gigabyte. */
    static proven_byte_t small[1000];
    EXAMPLE_REQUIRE(proven_inflate_all(heap, PROVEN_DEFLATE_GZIP, (proven_mem_view_t){ packed, packed_len }, (proven_mem_mut_t){ small, sizeof small }, &back_len, NULL) == PROVEN_ERR_OUT_OF_BOUNDS,
                    "4,000 bytes do not fit in 1,000: PROVEN_ERR_OUT_OF_BOUNDS");

    /* Damage is noticed. gzip carries a CRC-32 of the data; one changed bit anywhere in the
     * compressed stream changes the data or breaks the format. */
    packed[packed_len / 2] ^= 0x04;
    EXAMPLE_REQUIRE(proven_inflate_all(heap, PROVEN_DEFLATE_GZIP, (proven_mem_view_t){ packed, packed_len }, (proven_mem_mut_t){ back, sizeof back }, &back_len, NULL) == PROVEN_ERR_INVALID_FORMAT,
                    "a changed bit: PROVEN_ERR_INVALID_FORMAT");
    packed[packed_len / 2] ^= 0x04;
    EXAMPLE_REQUIRE(proven_inflate_all(heap, PROVEN_DEFLATE_GZIP, (proven_mem_view_t){ packed, packed_len - 1 }, (proven_mem_mut_t){ back, sizeof back }, &back_len, NULL) == PROVEN_ERR_INVALID_FORMAT,
                    "a member cut short is not a member");

    /* The framing is a choice made at both ends. The same data as zlib is twelve bytes
     * smaller (a shorter header, a shorter checksum), and raw is smaller again - and has no
     * checksum at all, so damage to it can go unseen. */
    proven_size_t zlib_len = 0, raw_len = 0;
    static proven_byte_t other[4200];
    options.format = PROVEN_DEFLATE_ZLIB;
    EXAMPLE_REQUIRE(proven_deflate_all(&options, (proven_mem_view_t){ text, sizeof text }, (proven_mem_mut_t){ other, sizeof other }, &zlib_len) == PROVEN_OK && zlib_len == packed_len - 12,
                    "zlib framing: 12 bytes less than gzip");
    EXAMPLE_REQUIRE(proven_inflate_all(heap, PROVEN_DEFLATE_GZIP, (proven_mem_view_t){ other, zlib_len }, (proven_mem_mut_t){ back, sizeof back }, &back_len, NULL) == PROVEN_ERR_INVALID_FORMAT,
                    "and not gzip: a decompressor must be told which it is reading");
    options.format = PROVEN_DEFLATE_RAW;
    EXAMPLE_REQUIRE(proven_deflate_all(&options, (proven_mem_view_t){ text, sizeof text }, (proven_mem_mut_t){ other, sizeof other }, &raw_len) == PROVEN_OK && raw_len == zlib_len - 6,
                    "raw: no header and no checksum, 6 bytes less again");

    /* What does not compress grows a little - by the bound, at worst. */
    static proven_byte_t noise[4000], noisy[4200];
    proven_u32 x = 2463534242u;
    for (proven_size_t i = 0; i < sizeof noise; ++i) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; noise[i] = (proven_byte_t)x; }
    proven_size_t noisy_len = 0;
    options.format = PROVEN_DEFLATE_GZIP;
    EXAMPLE_REQUIRE(proven_deflate_all(&options, (proven_mem_view_t){ noise, sizeof noise }, (proven_mem_mut_t){ noisy, sizeof noisy }, &noisy_len) == PROVEN_OK &&
                    noisy_len > sizeof noise && noisy_len <= proven_deflate_bound(PROVEN_DEFLATE_GZIP, sizeof noise),
                    "random bytes come out slightly larger, and never larger than the bound");

    return EXAMPLE_OK();
}
```

## 3. A stream

```c
proven_err_t proven_deflate_create(const proven_deflate_options_t *options, proven_deflate_t **out);
proven_err_t proven_deflate(proven_deflate_t *z, proven_mem_view_t in, proven_size_t *consumed,
                            proven_mem_mut_t out, proven_size_t *produced, proven_deflate_flush_t flush, bool *done);
void proven_deflate_reset(proven_deflate_t *z);
void proven_deflate_destroy(proven_deflate_t *z);

proven_err_t proven_inflate_create(proven_allocator_t alloc, proven_deflate_format_t format, proven_inflate_t **out);
proven_err_t proven_inflate(proven_inflate_t *z, proven_mem_view_t in, proven_size_t *consumed,
                            proven_mem_mut_t out, proven_size_t *produced, bool *done);
void proven_inflate_reset(proven_inflate_t *z);
void proven_inflate_destroy(proven_inflate_t *z);
```

When the data is a file larger than memory, a response still being produced, or a connection,
both directions work the same way: an object that holds the state, and **one step function
that takes some input and fills some output**.

The contract of a step is short, and worth reading once:

- It consumes as much of `in` as it can and produces as much into `out` as it can, and tells
  you how much of each. It never allocates and never writes past `out`.
- **`PROVEN_OK` with nothing consumed and nothing produced is not an error.** It means the
  step needs more of one or the other: more input, or more room. Call it again when you have
  some. A stream that simply stops arriving looks exactly like this, which is why a caller
  that knows no more input is coming must treat it as a truncated stream.
- `*done` becomes true when the stream has ended and everything has been produced - also
  when the data exactly filled the room you gave. Input after the end is not consumed:
  `*consumed` tells you where the stream ended.

**When compressing, `flush` says how urgent the output is.**

| `flush` | Meaning | Use it |
|---|---|---|
| `PROVEN_DEFLATE_FLUSH_NONE` | More is coming. Output appears when the compressor has a block's worth - possibly not for a long time | For everything except the two cases below |
| `PROVEN_DEFLATE_FLUSH_SYNC` | Everything given so far must be decodable now. The stream goes on | When somebody is waiting for what you have written: a message on a connection, a line of a live log. It costs a few bytes and some compression each time |
| `PROVEN_DEFLATE_FLUSH_FINISH` | That was the last input: end the stream | Once, at the end. Keep calling with it, and with more room, until `*done` |

A sync flush ends on a byte boundary, and the last four bytes it produces are always
`00 00 ff ff`. WebSocket's `permessage-deflate` sends each message as a sync flush with
those four bytes left off, and the receiver puts them back. That is also why a raw stream
need never end: the decompressor is fed piece after piece, `*done` never becomes true, and
what it remembers of earlier pieces is what makes later ones small.

`proven_deflate_reset` and `proven_inflate_reset` start a new stream in the same object,
which is cheaper than making another. After `*done`, a decompressor that should read a
second gzip member following the first is reset and fed from where the first ended.

Compiled and run by the test suite:

<!-- example: manual/examples/en/ex_16_stream.c -->
```c
#include <string.h>

/*
 * Compression as a stream: data that arrives in pieces and leaves in pieces, through buffers
 * of whatever size you have. The same loop works for a file of any length, a response that
 * is still being produced, or a connection.
 */

/* Where the compressed bytes go. A real program writes them to a file or a socket. */
static proven_byte_t g_wire[8192];
static proven_size_t g_wire_len;

/* Give the compressor some input and take whatever output it has, 64 bytes at a time, until
 * it has taken all of the input - and, when `flush` asks for more than that, until it has
 * produced all of that too. */
static bool feed(proven_deflate_t *z, const char *text, proven_deflate_flush_t flush, bool *done) {
    proven_mem_view_t in = { (const proven_byte_t *)text, strlen(text) };
    for (;;) {
        proven_byte_t out[64];
        proven_size_t consumed = 0, produced = 0;
        if (proven_deflate(z, in, &consumed, (proven_mem_mut_t){ out, sizeof out }, &produced, flush, done) != PROVEN_OK) return false;
        if (g_wire_len + produced > sizeof g_wire) return false;
        memcpy(g_wire + g_wire_len, out, produced);
        g_wire_len += produced;
        in.ptr += consumed; in.size -= consumed;
        /* Nothing left to give, and it had less than a bufferful to say: it is up to date. */
        if (in.size == 0 && produced < sizeof out) return true;
    }
}

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();

    /* A compressor is made once. Zero in the options means the defaults: level 6, a 32 KiB
     * window. Level 1 is faster and compresses a little less; 9 is slower and a little more. */
    proven_deflate_options_t options = { .alloc = heap, .format = PROVEN_DEFLATE_ZLIB, .level = 6 };
    proven_deflate_t *z = NULL;
    EXAMPLE_REQUIRE(proven_deflate_create(&options, &z) == PROVEN_OK, "a compressor");

    /* Pieces go in as they come. With FLUSH_NONE the compressor keeps what it is given until
     * it has a block's worth - nothing may come out for a long time, and that is how it
     * compresses well. */
    bool done = false;
    EXAMPLE_REQUIRE(feed(z, "first piece of the stream; ", PROVEN_DEFLATE_FLUSH_NONE, &done), "a piece goes in");
    const proven_size_t after_first = g_wire_len;
    EXAMPLE_REQUIRE(after_first <= 2 && !done, "and nothing but the two-byte header has come out yet");

    /* A sync flush says: the other side must be able to read everything up to here, now. It
     * costs a few bytes each time - use it when somebody is waiting, not after every write. */
    EXAMPLE_REQUIRE(feed(z, "second piece of the stream; ", PROVEN_DEFLATE_FLUSH_SYNC, &done) && g_wire_len > after_first + 20 && !done,
                    "after a sync flush both pieces are on the wire");
    const proven_size_t at_flush = g_wire_len;

    /* FINISH ends the stream: the last block, and for zlib and gzip the checksum. */
    EXAMPLE_REQUIRE(feed(z, "third and last piece.", PROVEN_DEFLATE_FLUSH_FINISH, &done) && done, "the last piece, and the stream is finished");

    /* The receiving side: a decompressor, fed 10 bytes at a time into a 16-byte buffer. */
    proven_inflate_t *u = NULL;
    EXAMPLE_REQUIRE(proven_inflate_create(heap, PROVEN_DEFLATE_ZLIB, &u) == PROVEN_OK, "a decompressor");
    char text[200];
    proven_size_t text_len = 0, at = 0, seen_at_flush = 0;
    bool ended = false;
    /* First only what had been written when the sync flush returned; then the rest. */
    for (proven_size_t limit = at_flush; !ended; limit = g_wire_len) {
        for (;;) {
            proven_byte_t out[16];
            proven_size_t consumed = 0, produced = 0;
            const proven_size_t give = limit - at < 10 ? limit - at : 10;
            if (proven_inflate(u, (proven_mem_view_t){ g_wire + at, give }, &consumed, (proven_mem_mut_t){ out, sizeof out }, &produced, &ended) != PROVEN_OK) break;
            if (text_len + produced > sizeof text) break;
            memcpy(text + text_len, out, produced);
            text_len += produced;
            at += consumed;
            /* PROVEN_OK with nothing consumed and nothing produced: it has used everything
             * it was offered and wants more. That is not an error - it is how a stream waits. */
            if (ended || (consumed == 0 && produced == 0)) break;
        }
        if (limit == at_flush) seen_at_flush = text_len;
        else break;
    }
    static const char whole[] = "first piece of the stream; second piece of the stream; third and last piece.";
    EXAMPLE_REQUIRE(ended && text_len == sizeof whole - 1 && memcmp(text, whole, text_len) == 0, "the three pieces come out as one text");
    EXAMPLE_REQUIRE(at == g_wire_len, "and the stream ended exactly where the compressor stopped writing");
    EXAMPLE_REQUIRE(seen_at_flush >= 55, "at the flush point the receiver already had both of the first two pieces");

    /* Both objects are reused for the next stream rather than made again. */
    proven_deflate_reset(z);
    proven_inflate_reset(u);
    g_wire_len = 0;
    EXAMPLE_REQUIRE(feed(z, "another stream", PROVEN_DEFLATE_FLUSH_FINISH, &done) && done, "the same compressor, a new stream");
    proven_byte_t again[32];
    proven_size_t consumed = 0, produced = 0;
    EXAMPLE_REQUIRE(proven_inflate(u, (proven_mem_view_t){ g_wire, g_wire_len }, &consumed, (proven_mem_mut_t){ again, sizeof again }, &produced, &ended) == PROVEN_OK &&
                    ended && produced == 14 && memcmp(again, "another stream", 14) == 0, "and the same decompressor reads it");

    proven_deflate_destroy(z);
    proven_inflate_destroy(u);
    return EXAMPLE_OK();
}
```

## 4. Levels, windows and what they cost

A compressor is made from a `proven_deflate_options_t`. Zero-initialise it, set `alloc`, and
set what you want changed:

| Field | Meaning |
|---|---|
| `alloc` | Required. The compressor is one allocation, made here and freed by `proven_deflate_destroy` |
| `format` | The framing of section 1. Zero is `PROVEN_DEFLATE_RAW` |
| `level` | 1 to 9: how hard to look for repeats. **0 is the default, 6.** -1 does not compress at all - the data is only wrapped |
| `window_bits` | How far back a repeat may be, as a power of two: 9 (512 bytes) to 15 (32 KiB). **0 is the default, 15** |

**The level trades time for size, and the trade is lopsided.** On the development machine,
compressing this library's own sources and manual (3.2 MiB of text):

| Level | Compressed to | Compressing | Decompressing |
|---|---|---|---|
| 1 | 30.2% | 33 MiB/s | 66 MiB/s |
| 6 (default) | 25.9% | 16 MiB/s | 84 MiB/s |
| 9 | 25.7% | 9 MiB/s | 83 MiB/s |

From 1 to 6 buys a seventh off the size for half the speed. From 6 to 9 buys almost nothing
for nearly half the speed again. Decompression does not care what level was used.

zlib, the implementation everybody else uses, compresses the same data to the same sizes -
within a quarter of a percent at levels 6 and 9, and it is a little larger at level 1 - and is
about twice as fast at compressing and three times as fast at decompressing. This library's
code was written to be read; if these figures are too slow for a program, that program wants
zlib.

**The window is mostly a question of memory.** A compressor holds twice its window and
tables beside it; a decompressor always holds 32 KiB, because it cannot know what window the
other side used:

| | Memory |
|---|---|
| A decompressor | about 35 KiB |
| A compressor, `window_bits` 15 (default) | about 325 KiB |
| A compressor, `window_bits` 12 | about 53 KiB |
| A compressor, `window_bits` 9 | about 11 KiB |

A smaller window compresses less well - repeats further apart than the window are not found -
and makes no difference to speed worth planning around. Use it where there are many
compressors at once (one per connection) or little memory.

## 5. Data from somebody else

Compressed data that arrives from outside the program is the dangerous case, for one reason:

**It can be far larger than it looks.** DEFLATE can say "the last byte again, 258 times" in
two bytes, again and again. A hundred and seventeen bytes in this library's tests expand to
25,801; a megabyte can become a gigabyte. A program that decompresses whatever it is sent
into a buffer that grows as needed has given every sender the ability to exhaust its memory.

So nothing here grows. `proven_inflate` writes only into the space you pass it, and when that
is full it stops and says so; `proven_inflate_all` takes the limit as the size of its output.
Decide the largest result that makes sense for what you are reading - a configuration file,
a request body, a message - and refuse anything beyond it. The decompressor's own memory is
fixed at about 35 KiB whatever it is fed.

**And it can simply be wrong**, by accident or on purpose:

| Returns | When |
|---|---|
| `PROVEN_ERR_INVALID_FORMAT` | The bytes are not a valid stream of the format the decompressor was made for; or - for gzip and zlib - the checksum at the end does not match the data; or, from `proven_inflate_all`, the input ended before the stream did |
| `PROVEN_ERR_UNSUPPORTED` | A zlib stream that needs a preset dictionary |
| `PROVEN_ERR_OUT_OF_BOUNDS` | From `proven_inflate_all`: the data does not fit in the buffer you gave |

After an error a decompressor returns that error from every call until it is reset. Data it
produced before the error is not to be trusted as complete: with gzip and zlib the checksum
is only known to match at the very end.

**Two more things worth knowing.**

- **Compression can leak what it compresses.** If a secret and text chosen by an attacker are
  compressed together and the attacker can see the size of the result, the size says whether
  the chosen text repeated part of the secret, and the secret can be guessed a character at a
  time. This has broken HTTPS in practice (CRIME, BREACH). The rule is the application's to
  keep: do not compress a secret together with anything a stranger controls.
- **Raw DEFLATE has no checksum.** If the bytes may have been damaged on the way and the
  format around them does not check, use zlib or gzip.

## 6. What is accepted, what is not here, and how this was tested

**What the decompressor accepts is what zlib accepts.** Where RFC 1951 leaves a corner open,
each case was tried on zlib first and its verdict followed, because a real producer may emit
what zlib tolerates. So: a block that uses a single code is accepted, as is a length written
with the longest form of the code before the last; codes that describe more symbols than can
exist, lengths and distances outside the format, a distance reaching back before the first
byte, and a stream whose code tables do not add up are all refused. A gzip header may carry
an extra field, a file name, a comment and its own checksum, all of which are read past (the
checksum is checked); the name and the time are not reported.

**What the compressor writes is valid DEFLATE that any decompressor reads.** It is not, byte
for byte, what zlib would have written for the same input, and it does not need to be.

**Not here:**

- **Preset dictionaries** (zlib's `FDICT`), in either direction.
- **ZIP archives, tar files**: those are containers. This is the codec inside them.
- **Deflate64**, and every other compression format: Brotli, Zstandard, LZ4, bzip2, xz.
- **The gzip header's fields** - name, time, comment - cannot be set or read.
- **Several gzip members read as one stream** automatically: reset after each (section 3).

**How this was tested.** The registered test decompresses 93 streams that zlib 1.3.1 made -
every level and strategy, small windows, flushes in the middle, all three formats - a byte
at a time and in one call, and gets the data zlib was given; accepts ten hand-built streams
that are unusual and that zlib accepts; refuses thirty hand-built streams that zlib refuses,
one for each way a stream can be wrong; gives "more input, please" for every proper prefix
of the short streams and an answer of some kind - never a write outside its buffer - for
every single changed bit of them; and compresses eleven kinds of data at every level and
three window sizes and gets it back. Outside the registered tests: zlib decompressed
everything this compressor produced for a larger corpus, at every level, window and format,
with flushes and in small pieces (2,856 streams); this decompressor was compared with zlib
on 5,838 more; and six million randomly damaged streams were decompressed under tools that
report any read or write outside a buffer, with none reported and no stream that made the
decoder do work without progress.

**Not done:** no coverage-guided fuzzing; no comparison with a second compressor's output
besides zlib's; no external review.
