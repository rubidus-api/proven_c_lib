# Chapter 16: Compression

**Part III - Data structures. Prerequisites: [Chapter 2](manual-02-allocation.md) for the allocator a
compressor is made with; [Chapter 4](manual-04-containers-algorithms.md) for the checksums.**
**After this chapter** you can compress and decompress a buffer or a stream of any length,
read and write the `gzip` and `zlib` wrappers, decide how much a stranger's data may expand
to before you refuse it - and say what was tested and how fast it is.

This chapter covers `deflate.h`. It is pure computation - one allocation when a compressor or
decompressor is made, nothing from the operating system - and is available in a
[freestanding](manual-freestanding.md) build. Section 6 is the exception: it is about
compressed HTTP responses, in the servers and clients of Chapters 11 and 15.

## Table of contents

1. [What DEFLATE does, in one page](#1-what-deflate-does-in-one-page)
2. [A whole buffer](#2-a-whole-buffer)
3. [A stream](#3-a-stream)
4. [Levels, windows and what they cost](#4-levels-windows-and-what-they-cost)
5. [Data from somebody else](#5-data-from-somebody-else)
6. [Over HTTP](#6-over-http)
7. [What is accepted, what is not here, and how this was tested](#7-what-is-accepted-what-is-not-here-and-how-this-was-tested)

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

## 6. Over HTTP

```c
void proven_http_exchange_compress(proven_http_exchange_t *exchange);     /* the server of Chapter 11 */
void proven_http_stream_compress(proven_http_stream_t *stream);           /* the server of Chapter 15 */

bool proven_http_accepts_coding(const proven_http_header_t *headers, proven_size_t count, proven_http_coding_t coding);
proven_http_coding_t proven_http_content_coding(const proven_http_header_t *headers, proven_size_t count);
```

HTTP calls this a *content coding*: the server compresses the body and says so in
`Content-Encoding`, and it may only do that for a client whose `Accept-Encoding` allowed it.
The four HTTP drivers of [Chapter 11](manual-11-http-client-server.md) and
[Chapter 15](manual-15-event-loop.md) do both halves - and **do neither until you ask**. A
program that sets nothing sends the same bytes as before.

Unlike the rest of this chapter, this part needs sockets and is not in a freestanding build -
except the two helper functions, which are plain header arithmetic in `http.h`.

**A server compresses a response when its handler asks for that response.** One call, before
the response begins:

| | What happens |
|---|---|
| The request allows gzip, and the response has a body of its own | Sent with `Content-Encoding: gzip` |
| The request does not allow gzip (no `Accept-Encoding`, `gzip;q=0`, only other codings) | Sent as it is |
| Status 204, 206 or 304; or your headers already have `Content-Encoding` or `Content-Range` | Sent as it is |
| A body sent in one piece that is under 256 bytes, or does not get smaller | Sent as it is |
| No memory for the compressor | Sent as it is |
| In every one of these cases | `Vary: Accept-Encoding` is added, unless your own `Vary` names it |

`Vary` is there because the response now depends on a request header, and a cache in between
must not hand the gzip to a client that cannot read it.

A body sent in one piece is compressed first and sent with its new `Content-Length`. A body
written in pieces is compressed as it is written and sent chunked, whatever length you
announced - the length is still what you must write.

**A client asks and decodes when its configuration says `decompress`.** It then sends
`Accept-Encoding: gzip` and decodes a body that comes back as `gzip` or `deflate`, on the way
to you: `proven_http_client_read` fills your buffer with decoded bytes, and the event-driven
client's `on_body` receives decoded pieces of at most 16 KiB.

### Example: a page compressed, an account page not

<!-- example: manual/examples/en/ex_16_http_gzip.c -->
```c
#include <stdio.h>
#include <string.h>

/*
 * Compressed responses over HTTP: a server that compresses when its handler asks and the
 * client accepts, and a client that asks and decodes.
 *
 * Both are off until the program says otherwise. The server's part is one call in the
 * handler, per response; the client's part is one flag in its configuration.
 */

static proven_byte_t g_page[20000];        /* the page being served: text, so it compresses */

static void fill_page(void) {
    proven_size_t n = 0;
    for (unsigned k = 0; n < sizeof g_page; ++k) {
        char line[48];
        int len = snprintf(line, sizeof line, "row %u of a table that says much the same\n", k);
        for (int i = 0; i < len && n < sizeof g_page; ++i) g_page[n++] = (proven_byte_t)line[i];
    }
}

static void handle(void *ctx, proven_http_exchange_t *x) {
    proven_http_server_t **server = ctx;
    const proven_http_request_t *req = proven_http_exchange_request(x);
    proven_mem_view_t page = { g_page, sizeof g_page };

    if (proven_u8str_view_eq(req->target, PROVEN_LIT("/page"))) {
        /* One call, before the response begins. The server then does the rest: it reads the
         * request's Accept-Encoding, compresses if gzip is allowed, and writes
         * Content-Encoding, Vary and the new Content-Length. */
        proven_http_exchange_compress(x);
        proven_http_header_t type = { PROVEN_LIT("Content-Type"), PROVEN_LIT("text/plain") };
        (void)proven_http_exchange_respond(x, 200, &type, 1, page);

    } else if (proven_u8str_view_eq(req->target, PROVEN_LIT("/account"))) {
        /* Not asked for here, on purpose: this response would carry a secret next to text
         * the client chose, and the size of a compressed body gives the secret away a byte
         * at a time. Compression is asked for per response because only the handler knows. */
        (void)proven_http_exchange_respond(x, 200, NULL, 0, proven_mem_view_from_u8(PROVEN_LIT("token=... you searched for: ...\n")));

    } else if (proven_u8str_view_eq(req->target, PROVEN_LIT("/accepts"))) {
        /* The same question the server asks, for a handler that wants to choose for itself -
         * between a file and its precompressed twin, say. */
        bool gzip = proven_http_accepts_coding(req->headers, req->header_count, PROVEN_HTTP_CODING_GZIP);
        (void)proven_http_exchange_respond(x, 200, NULL, 0, proven_mem_view_from_u8(gzip ? PROVEN_LIT("gzip") : PROVEN_LIT("identity")));

    } else {
        (void)proven_http_exchange_respond(x, 200, NULL, 0, proven_mem_view_from_u8(PROVEN_LIT("bye\n")));
        proven_http_server_stop(*server);
    }
}

static void serve(void *arg) { (void)proven_http_server_run(*(proven_http_server_t **)arg); }

/* GET `path`; report the body's size as read, the Content-Length, and the coding named. */
static bool get(proven_http_client_t *client, proven_u16 port, const char *path,
                proven_size_t *read_size, proven_u64 *content_length, proven_http_coding_t *coding, proven_u8str_t *body) {
    char url[96];
    int n = snprintf(url, sizeof url, "http://127.0.0.1:%u%s", (unsigned)port, path);
    proven_http_client_response_t resp;
    bool ok = proven_http_client_get(client, (proven_u8str_view_t){ (const proven_byte_t *)url, (proven_size_t)n }, &resp) == PROVEN_OK && resp.status == 200;
    if (ok) {
        /* The headers are the server's, untouched: this is how to see what was sent. */
        *coding = proven_http_content_coding(resp.headers, resp.header_count);
        proven_u8str_view_t length = { 0 };
        *content_length = 0;
        if (proven_http_header_find(resp.headers, resp.header_count, PROVEN_LIT("Content-Length"), &length)) {
            for (proven_size_t i = 0; i < length.size; ++i) *content_length = *content_length * 10 + (proven_u64)(length.ptr[i] - '0');
        }
        /* The limit is on what arrives here - decoded bytes, when the client decodes. */
        (void)proven_u8str_reset(body);
        ok = proven_http_client_read_all(&resp, proven_heap_allocator(), body, 1024 * 1024) == PROVEN_OK;
        *read_size = proven_u8str_as_view(body).size;
    }
    proven_http_client_finish(&resp);
    return ok;
}

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();
    fill_page();

    static proven_http_server_t *server;
    proven_http_server_config_t config = { .alloc = heap, .handler = handle, .handler_ctx = &server };
    config.compress_level = 6;              /* 1 is fastest, 9 smallest; zero means this */
    EXAMPLE_REQUIRE(proven_http_server_create(&config, &server) == PROVEN_OK, "a server");
    proven_net_addr_t at;
    proven_err_t err = proven_http_server_listen(server, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &at);
    if (err == PROVEN_ERR_PERMISSION || err == PROVEN_ERR_UNSUPPORTED) {
        printf("no sockets in this environment; nothing to show\n");
        proven_http_server_destroy(server);
        return EXAMPLE_OK();
    }
    EXAMPLE_REQUIRE(err == PROVEN_OK, "it listens");
    proven_job_sys_t *threads = NULL;
    proven_job_group_t running;
    proven_job_group_init(&running);
    EXAMPLE_REQUIRE(proven_job_system_init(heap, 1, 4, &threads) == PROVEN_OK &&
                    proven_job_group_submit(threads, &running, serve, &server) == PROVEN_OK, "the server loop is started");

    /* Two clients: one as it always was, one that asks for compressed responses. */
    proven_http_client_config_t plain_config = { .alloc = heap, .max_idle_connections = 2 };
    proven_http_client_config_t decoding_config = { .alloc = heap, .max_idle_connections = 2, .decompress = true };
    proven_http_client_t *plain = NULL, *decoding = NULL;
    EXAMPLE_REQUIRE(proven_http_client_create(&plain_config, &plain) == PROVEN_OK &&
                    proven_http_client_create(&decoding_config, &decoding) == PROVEN_OK, "two clients");

    proven_u8str_t body = { 0 };
    proven_size_t read_size = 0;
    proven_u64 sent = 0;
    proven_http_coding_t coding = PROVEN_HTTP_CODING_IDENTITY;

    /* The client that sets nothing sends no Accept-Encoding, and nothing changes for it. */
    EXAMPLE_REQUIRE(get(plain, at.port, "/page", &read_size, &sent, &coding, &body), "the page, to a client that did not ask");
    EXAMPLE_REQUIRE(coding == PROVEN_HTTP_CODING_IDENTITY && sent == sizeof g_page && read_size == sizeof g_page, "arrives as it is");

    /* The one that asks gets the same bytes from a fraction of them. */
    EXAMPLE_REQUIRE(get(decoding, at.port, "/page", &read_size, &sent, &coding, &body), "the page, to a client that asked");
    EXAMPLE_REQUIRE(coding == PROVEN_HTTP_CODING_GZIP && read_size == sizeof g_page &&
                    memcmp(proven_u8str_as_view(&body).ptr, g_page, sizeof g_page) == 0, "was sent as gzip and read as the page");
    EXAMPLE_REQUIRE(sent < sizeof g_page / 5, "in less than a fifth of the bytes");
    printf("the page: %u bytes, sent as %u\n", (unsigned)sizeof g_page, (unsigned)sent);

    /* A response whose handler did not ask is not compressed, whoever asks for it. */
    EXAMPLE_REQUIRE(get(decoding, at.port, "/account", &read_size, &sent, &coding, &body) && coding == PROVEN_HTTP_CODING_IDENTITY, "the account page is sent as it is");

    EXAMPLE_REQUIRE(get(decoding, at.port, "/accepts", &read_size, &sent, &coding, &body) &&
                    proven_u8str_view_eq(proven_u8str_as_view(&body), PROVEN_LIT("gzip")), "the handler can see that this client accepts gzip");
    EXAMPLE_REQUIRE(get(plain, at.port, "/accepts", &read_size, &sent, &coding, &body) &&
                    proven_u8str_view_eq(proven_u8str_as_view(&body), PROVEN_LIT("identity")), "and that the other does not");

    EXAMPLE_REQUIRE(get(plain, at.port, "/quit", &read_size, &sent, &coding, &body), "stop");
    proven_job_group_wait(threads, &running);
    proven_u8str_destroy(heap, &body);
    proven_http_client_destroy(plain);
    proven_http_client_destroy(decoding);
    proven_http_server_destroy(server);
    proven_job_system_close(threads);
    proven_job_system_destroy(threads);
    return EXAMPLE_OK();
}
```

### Example: the same on a loop

<!-- example: manual/examples/en/ex_16_http_event_gzip.c -->
```c
#include <stdio.h>
#include <string.h>

/*
 * The same on a loop: the event-driven server compressing a response that is written in
 * pieces, and the event-driven client decoding it as it arrives.
 *
 * Neither holds the body. The server compresses what it is given into its output queue, under
 * the queue's limit; the client hands on decoded pieces of at most 16 KiB.
 */

#define REPORT_BYTES ((proven_size_t)300000)

typedef struct {
    proven_loop_t *loop;
    proven_http_stream_t *stream;      /* the one response being written */
    proven_size_t written;
    proven_size_t received, pieces, largest;
    unsigned long sum_sent, sum_received;
    bool gzip, intact, done;
    proven_err_t why;
} app_t;

/* The report is generated as it is written: there is no buffer holding all of it. */
static proven_byte_t report_byte(proven_size_t i) { return (proven_byte_t)("measured value \n"[i % 16]); }

// ---- the server --------------------------------------------------------------

/* Write until the server takes no more; on_writable calls this again when there is room. */
static void pump(app_t *app) {
    proven_byte_t piece[4096];
    while (app->written < REPORT_BYTES) {
        proven_size_t n = REPORT_BYTES - app->written < sizeof piece ? REPORT_BYTES - app->written : sizeof piece;
        for (proven_size_t i = 0; i < n; ++i) piece[i] = report_byte(app->written + i);
        proven_result_size_t took = proven_http_stream_write(app->stream, (proven_mem_view_t){ piece, n });
        if (took.err != PROVEN_OK) return;
        for (proven_size_t i = 0; i < took.value; ++i) app->sum_sent += piece[i];
        app->written += took.value;
        if (took.value < n) return;        /* the compressed output is at its limit: wait */
    }
    (void)proven_http_stream_end(app->stream);
}

static void on_request(void *ctx, proven_http_stream_t *stream, const proven_http_request_t *head) {
    (void)head;
    app_t *app = ctx;
    app->stream = stream;
    /* Before the response begins. From here on the body is written exactly as it would be
     * without it: the length given is still the length to write, though the client is sent
     * chunks of gzip. What is written may wait in the compressor until more follows. */
    proven_http_stream_compress(stream);
    if (proven_http_stream_begin(stream, 200, NULL, 0, REPORT_BYTES) == PROVEN_OK) pump(app);
}
static void on_writable(void *ctx, proven_http_stream_t *stream) { (void)stream; pump(ctx); }

// ---- the client --------------------------------------------------------------

static void on_response(void *ctx, proven_http_event_request_t *request, const proven_http_response_t *head) {
    (void)request;
    app_t *app = ctx;
    app->gzip = proven_http_content_coding(head->headers, head->header_count) == PROVEN_HTTP_CODING_GZIP;
}

/* Decoded pieces. A piece is a view that is good until this function returns. */
static void on_body(void *ctx, proven_http_event_request_t *request, proven_mem_view_t piece, bool last) {
    (void)request; (void)last;
    app_t *app = ctx;
    for (proven_size_t i = 0; i < piece.size; ++i) {
        if (piece.ptr[i] != report_byte(app->received + i)) app->intact = false;
        app->sum_received += piece.ptr[i];
    }
    app->received += piece.size;
    app->pieces++;
    if (piece.size > app->largest) app->largest = piece.size;
}

static void on_done(void *ctx, proven_http_event_request_t *request, proven_err_t why) {
    (void)request;
    app_t *app = ctx;
    app->why = why;
    app->done = true;
    proven_loop_stop(app->loop);
}

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();
    app_t app = { .intact = true };
    EXAMPLE_REQUIRE(proven_loop_create(heap, &app.loop) == PROVEN_OK, "a loop");

    /* The window sets what each compressed response holds while it is being written: about
     * 53 KiB at 12 bits, 325 KiB at the full 15. With many at once, that is the number to choose. */
    proven_http_event_server_config_t server_config = { .on = { .on_request = on_request, .on_writable = on_writable }, .ctx = &app, .compress_window_bits = 12 };
    proven_http_event_server_t *server = NULL;
    proven_net_addr_t at;
    EXAMPLE_REQUIRE(proven_http_event_server_create(app.loop, &server_config, &server) == PROVEN_OK, "a server");
    proven_err_t err = proven_http_event_server_listen(server, proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &at);
    if (err == PROVEN_ERR_PERMISSION || err == PROVEN_ERR_UNSUPPORTED) {
        printf("no sockets in this environment; nothing to show\n");
        proven_http_event_server_destroy(server);
        proven_loop_destroy(app.loop);
        return EXAMPLE_OK();
    }
    EXAMPLE_REQUIRE(err == PROVEN_OK, "it listens");

    /* `decompress` asks for gzip and decodes it; `max_body_bytes` then also bounds the decoded
     * size, so a small body that unfolds into a huge one is cut off at the limit. */
    proven_http_event_client_config_t client_config = { .decompress = true, .max_body_bytes = 1024 * 1024 };
    proven_http_event_client_t *client = NULL;
    EXAMPLE_REQUIRE(proven_http_event_client_create(app.loop, &client_config, &client) == PROVEN_OK, "a client that decodes");

    char url[64];
    int n = snprintf(url, sizeof url, "http://127.0.0.1:%u/report", (unsigned)at.port);
    proven_http_event_request_options_t options = {
        .url = { (const proven_byte_t *)url, (proven_size_t)n },
        .on = { on_response, on_body, NULL, on_done },
        .ctx = &app,
    };
    EXAMPLE_REQUIRE(proven_http_event_client_start(client, &options, NULL) == PROVEN_OK, "the request is started");
    EXAMPLE_REQUIRE(proven_loop_run(app.loop) == PROVEN_OK && app.done, "the loop ran until it was done");

    EXAMPLE_REQUIRE(app.why == PROVEN_OK && app.gzip, "the response came as gzip");
    EXAMPLE_REQUIRE(app.received == REPORT_BYTES && app.intact && app.sum_received == app.sum_sent, "and was delivered as the report that was written");
    EXAMPLE_REQUIRE(app.largest <= 16384 && app.pieces >= REPORT_BYTES / 16384, "in pieces of at most 16 KiB");
    printf("%u bytes written, compressed, sent, decoded and received\n", (unsigned)app.received);

    proven_http_event_client_destroy(client);
    proven_http_event_server_destroy(server);
    proven_loop_destroy(app.loop);
    return EXAMPLE_OK();
}
```

### Reference

| API | Intent | Return |
|---|---|---|
| `proven_http_exchange_compress(x)` | Ask that this response be compressed. Before `respond` or `begin`; afterwards it does nothing. | none. |
| `proven_http_stream_compress(stream)` | The same for the event-driven server: in `on_request` or later, before the response begins. | none. |
| `compress_level` (both server configurations) | 1 (fastest) to 9 (smallest); -1 stores without compressing. Zero is 6. Section 4 has the measurements. | `INVALID_ARG` from `create` outside that range. |
| `compress_window_bits` (both) | 9 to 15: the memory a streamed response holds while it is written (section 4's table). Zero is 15. | the same. |
| `decompress` (both client configurations) | Send `Accept-Encoding: gzip` and decode `gzip` and `deflate` bodies. | - |
| `proven_http_accepts_coding(headers, count, coding)` | Whether a request's `Accept-Encoding` allows a coding: listed with a weight above zero, or covered by `*`. | `bool`. Without the field only identity is accepted. |
| `proven_http_content_coding(headers, count)` | What `Content-Encoding` names. | `PROVEN_HTTP_CODING_IDENTITY`, `_GZIP` (also for `x-gzip`), `_DEFLATE`, or `_OTHER` for anything else and for more than one. |

### Cautions, and what goes wrong

- **Do not ask for a response that mixes a secret with text the client chose.** This is the
  leak of section 5 in its best-known form (BREACH): a page that carries a session token and
  also echoes a search term, compressed, tells an observer of the encrypted connection -
  through its length alone - whether the term matched part of the token. TLS does not help;
  it hides bytes, not sizes. This is why the server has no "compress everything" setting:
  the call is per response because the judgement is.
- **A streamed, compressed response does not arrive piece by piece.** What you write may sit
  in the compressor until more is written or the response ends. Do not ask for compression
  of an event stream ([Chapter 10](manual-10-http.md), section 13) or anything else a client
  must see as it happens.
- **A compressed response of a different size needs a different validator.** If you send an
  `ETag`, the compressed and the plain response are two representations; give them different
  tags, or a cache may match one against the other. The server does not rewrite yours.
- **The client's headers are the server's.** With `decompress`, `Content-Encoding: gzip` is
  still there and `Content-Length` still counts the compressed bytes - not the bytes you
  read. Count what you read, or look at `proven_http_content_coding` to know which it was.
- **The limit on what a server may send you is in decoded bytes.** `read_all`'s `max_bytes`,
  and the event-driven client's `max_body_bytes`, count what comes out of the decoder: two
  kilobytes that unfold into two megabytes are cut off at your limit, not at theirs. A plain
  `proven_http_client_read` is bounded by the buffer you give it, as always.
- **A damaged compressed body is `PROVEN_ERR_INVALID_FORMAT`**, never a short success: a
  stream that is cut off, fails its checksum, or is followed by bytes that are not another
  gzip member. Several gzip members in a row are read as one body.
- **`deflate` means two things in practice.** The standard says a zlib stream; some servers
  send raw DEFLATE. The client looks at the first two bytes and reads whichever it is. It
  never asks for `deflate` - only `gzip` - and the servers never send it.
- **Not decoded:** a `206 Partial Content` (a range of a compressed body is not a compressed
  body - and with `decompress` set, a request that carries `Range` is sent without
  `Accept-Encoding`); a coding other than these two (`br`, `zstd`); two codings applied one
  over the other. Such a body reaches you as it was sent, with its header.
- **Memory.** A response being decoded costs about 35 KiB (52 KiB in the event-driven
  client) while it is open. A streamed response being compressed costs what section 4's
  table says for the window - 325 KiB by default - until it ends; one sent in one piece
  costs that only for the call, with a window no larger than the body. Ten thousand
  streamed responses at the default window are 3 GiB: lower `compress_window_bits`, or
  compress fewer.

## 7. What is accepted, what is not here, and how this was tested

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
  (The HTTP clients of section 6 do this for a response body.)
- **Over HTTP:** compressed *request* bodies, in either direction; `br` and `zstd`; a call
  that pushes out what a streamed response has written so far; serving a precompressed file
  (a handler can do that itself: `proven_http_accepts_coding`, then its own
  `Content-Encoding`).

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

**Section 6 was tested** against bodies zlib made - gzip, zlib, raw DEFLATE, two members,
every optional header field, and each way of being cut short or damaged - through both
clients, whole and a byte a chunk, each checked against zlib's own verdict when the test's
table was generated; and with
both servers compressing at seven sizes, in one piece and streamed, plain and over TLS.
Outside the registered tests, curl and Python read what both servers send, and both clients
read what a Python server sends.

**Not done:** no coverage-guided fuzzing; no comparison with a second compressor's output
besides zlib's; no external review.
