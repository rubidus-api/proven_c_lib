# Chapter 17: HTTP/2 - frames and header compression

**Part V - Talking to the operating system. Prerequisites: [Chapter 10](manual-10-http.md) for
what a request and a response are; [Chapter 2](manual-02-allocation.md) for the allocator the
compression tables are made with.**
**After this chapter** you can say what HTTP/2 changes and what it does not, encode and decode
header blocks, read and write frames, tell a well-formed request from one that must not be
passed on - and say which of these this library does today.

This chapter covers `hpack.h` and `h2.h`. Both are pure computation, available in a
[freestanding](manual-freestanding.md) build: HPACK allocates one table for an encoder and one
for a decoder, and the frame functions allocate nothing.

**What is here is the codec, not yet the protocol.** These two headers read and write what
travels on an HTTP/2 connection. They do not hold a connection together - which streams are
open, how much may be sent, what to answer - and the servers and clients of Chapters 11 and
15 speak HTTP/1.1. Section 5 says exactly where the line is.

## Table of contents

1. [What HTTP/2 changes, in one page](#1-what-http2-changes-in-one-page)
2. [Header compression](#2-header-compression)
3. [Frames](#3-frames)
4. [A header list as a message](#4-a-header-list-as-a-message)
5. [What is accepted, what is not here, and how this was tested](#5-what-is-accepted-what-is-not-here-and-how-this-was-tested)

## 1. What HTTP/2 changes, in one page

HTTP/2 (RFC 9113) carries the same requests and responses as HTTP/1.1 - the same methods,
status codes and header fields - in another shape. Three things are different:

- **Everything is a frame.** Nine bytes of header - a length, a type, flags, a stream number -
  and a payload. There is no text to parse and no line to end: a message is HEADERS frames
  and DATA frames.
- **Many exchanges share one connection.** Each request and its response are a *stream* with a
  number; frames of different streams are interleaved, so a slow response does not hold up
  the ones behind it. Each stream, and the connection as a whole, has a *window*: how many
  bytes of DATA the other side has said it will take.
- **Header fields are compressed**, by a scheme made for them (HPACK, RFC 7541), because the
  same fields are sent again with every request.

What HTTP/1.1 said in its first line is said by *pseudo-header fields*, whose names begin
with a colon: `:method`, `:scheme`, `:authority` and `:path` for a request, `:status` for a
response. There is no `Host` (it is `:authority`), no `Connection`, no `Transfer-Encoding`:
framing does what they did.

A connection begins with 24 fixed bytes from the client (`PROVEN_H2_PREFACE`), then a
SETTINGS frame from each side.

## 2. Header compression

```c
proven_err_t proven_hpack_encoder_create(proven_allocator_t alloc, proven_size_t max_table_bytes, proven_hpack_encoder_t **out);
proven_err_t proven_hpack_encode(proven_hpack_encoder_t *encoder, const proven_http_header_t *headers, proven_size_t count,
                                 const bool *sensitive, proven_mem_mut_t out, proven_size_t *len);

proven_err_t proven_hpack_decoder_create(proven_allocator_t alloc, proven_size_t max_table_bytes, proven_hpack_decoder_t **out);
proven_err_t proven_hpack_decode(proven_hpack_decoder_t *decoder, proven_mem_view_t block,
                                 proven_http_header_t *headers, proven_size_t header_cap,
                                 proven_mem_mut_t scratch, proven_size_t max_list_bytes, proven_size_t *count);
```

HPACK sends a field in one of three ways: as a number that names an entry of a fixed table of
61 common fields (`:method: GET` is entry 2); as a number that names an entry of the
*dynamic table*, which holds fields this connection sent before; or as a literal - which may
be added to the dynamic table as it goes, and whose text may be Huffman-coded. A request
that is 65 bytes the first time is 22 the second.

**The two ends must keep the same table.** Every block is decoded, in the order it was sent,
by the one decoder of its connection and direction. Skip a block, or decode one twice, and
every block after it means something else. That is why a decoder that meets a malformed
block refuses everything afterwards, and why HTTP/2 ends the connection over it.

**The table's size is yours to set**, and its cost is exactly that size: 4,096 bytes by
default (`PROVEN_HPACK_DEFAULT_TABLE_BYTES`), allocated when the first entry is added. An
encoder made with 0 keeps no table and is stateless; a decoder made with 0 tells the peer to
keep none.

### Example: a request sent twice, a secret kept out, and a limit

<!-- example: manual/examples/en/ex_17_hpack.c -->
```c
#include <stdio.h>
#include <string.h>

/*
 * HPACK: how HTTP/2 sends header fields.
 *
 * An encoder turns a list of fields into a block of bytes; the decoder at the other end turns
 * the block back into the list. Both keep a table of fields that went before, and the second
 * time a field is sent it is one byte. The two tables must stay the same - which is why a
 * connection has exactly one encoder and one decoder in each direction, used in order.
 */

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();
    proven_hpack_encoder_t *encoder = NULL;
    proven_hpack_decoder_t *decoder = NULL;
    /* 4096 bytes of table is the protocol's default; each side may agree on another size. */
    EXAMPLE_REQUIRE(proven_hpack_encoder_create(heap, PROVEN_HPACK_DEFAULT_TABLE_BYTES, &encoder) == PROVEN_OK &&
                    proven_hpack_decoder_create(heap, PROVEN_HPACK_DEFAULT_TABLE_BYTES, &decoder) == PROVEN_OK, "an encoder here, a decoder there");

    // ---- a request, twice ------------------------------------------------------------
    /* Names are lowercase in HTTP/2, and what HTTP/1 put in its first line is here as fields
     * whose names begin with a colon. */
    proven_http_header_t request[] = {
        { PROVEN_LIT(":method"), PROVEN_LIT("GET") },
        { PROVEN_LIT(":scheme"), PROVEN_LIT("https") },
        { PROVEN_LIT(":path"), PROVEN_LIT("/reports/2026/october") },
        { PROVEN_LIT(":authority"), PROVEN_LIT("www.example.com") },
        { PROVEN_LIT("user-agent"), PROVEN_LIT("example-client/1.0") },
        { PROVEN_LIT("authorization"), PROVEN_LIT("Bearer 5f2c1e9a7b3d") },
    };
    proven_byte_t block[512];
    EXAMPLE_REQUIRE(proven_hpack_encode_bound(request, 6) <= sizeof block, "the block will fit");
    proven_size_t first = 0;
    EXAMPLE_REQUIRE(proven_hpack_encode(encoder, request, 6, NULL, (proven_mem_mut_t){ block, sizeof block }, &first) == PROVEN_OK, "the request, encoded");

    /* The decoder gives the fields back as views: into the block, into the fixed table, or
     * into the scratch you lend it. */
    proven_http_header_t fields[16];
    proven_byte_t scratch[1024];
    proven_size_t count = 0;
    EXAMPLE_REQUIRE(proven_hpack_decode(decoder, (proven_mem_view_t){ block, first }, fields, 16, (proven_mem_mut_t){ scratch, sizeof scratch }, 4096, &count) == PROVEN_OK && count == 6,
                    "and decoded");
    EXAMPLE_REQUIRE(proven_u8str_view_eq(fields[2].name, PROVEN_LIT(":path")) && proven_u8str_view_eq(fields[2].value, PROVEN_LIT("/reports/2026/october")), "field for field");

    /* The same request again costs a fraction: both sides remember. */
    proven_size_t second = 0;
    EXAMPLE_REQUIRE(proven_hpack_encode(encoder, request, 6, NULL, (proven_mem_mut_t){ block, sizeof block }, &second) == PROVEN_OK, "the same request again");
    printf("the first time: %u bytes; the second: %u\n", (unsigned)first, (unsigned)second);
    EXAMPLE_REQUIRE(second < first / 2, "less than half");
    EXAMPLE_REQUIRE(proven_hpack_decode(decoder, (proven_mem_view_t){ block, second }, fields, 16, (proven_mem_mut_t){ scratch, sizeof scratch }, 4096, &count) == PROVEN_OK && count == 6 &&
                    proven_u8str_view_eq(fields[4].value, PROVEN_LIT("example-client/1.0")), "and the decoder still gives the whole list");

    // ---- what is not remembered --------------------------------------------------------
    /* A table is a compression context, and a secret in one can be guessed from the size of
     * later blocks. So the authorization value was never put in it - by either side: */
    proven_size_t entries = proven_hpack_decoder_table_count(decoder);
    bool secret_kept = false;
    for (proven_size_t i = 0; i < entries; ++i) {
        proven_u8str_view_t name, value;
        EXAMPLE_REQUIRE(proven_hpack_decoder_table_entry(decoder, i, &name, &value), "an entry of the table");
        if (proven_u8str_view_eq(name, PROVEN_LIT("authorization"))) secret_kept = true;
    }
    printf("the table holds %u entries, %u bytes by the protocol's count\n", (unsigned)entries, (unsigned)proven_hpack_decoder_table_bytes(decoder));
    EXAMPLE_REQUIRE(entries == 3 && !secret_kept, "the path, the authority and the user agent - not the secret");
    /* Any field can be given the same treatment. */
    proven_http_header_t more[] = { { PROVEN_LIT("x-api-key"), PROVEN_LIT("k-0123456789") } };
    bool sensitive[] = { true };
    proven_size_t n = 0;
    EXAMPLE_REQUIRE(proven_hpack_encode(encoder, more, 1, sensitive, (proven_mem_mut_t){ block, sizeof block }, &n) == PROVEN_OK &&
                    proven_hpack_decode(decoder, (proven_mem_view_t){ block, n }, fields, 16, (proven_mem_mut_t){ scratch, sizeof scratch }, 4096, &count) == PROVEN_OK &&
                    proven_hpack_decoder_table_count(decoder) == 3, "a field marked sensitive adds nothing to the table");

    // ---- a limit, and what it is for ---------------------------------------------------
    /* A block is not the size of what it says. This one is five bytes and names the same
     * remembered field five times: */
    static const proven_byte_t repeat[] = { 0xbe, 0xbe, 0xbe, 0xbe, 0xbe };       /* "entry 62" - the newest in the table */
    EXAMPLE_REQUIRE(proven_hpack_decode(decoder, (proven_mem_view_t){ repeat, sizeof repeat }, fields, 16, (proven_mem_mut_t){ scratch, sizeof scratch }, 4096, &count) == PROVEN_OK && count == 5,
                    "five bytes, five fields");
    /* So the limit is on the decoded list - names, values, and 32 a field - not on the block.
     * Over it, the fields are withheld, but the decoder has read the block to its end and is
     * still usable: refuse the one request, keep the connection. */
    EXAMPLE_REQUIRE(proven_hpack_decode(decoder, (proven_mem_view_t){ repeat, sizeof repeat }, fields, 16, (proven_mem_mut_t){ scratch, sizeof scratch }, 100, &count) == PROVEN_ERR_OUT_OF_BOUNDS && count == 0,
                    "a list limit of 100 bytes refuses it");
    EXAMPLE_REQUIRE(proven_hpack_decode(decoder, (proven_mem_view_t){ repeat, 1 }, fields, 16, (proven_mem_mut_t){ scratch, sizeof scratch }, 100, &count) == PROVEN_OK && count == 1, "and the next block is decoded as if nothing had happened");

    /* A block that is not HPACK at all is another matter: the tables can no longer be trusted
     * to match, and the decoder says so from then on. In HTTP/2 that ends the connection. */
    static const proven_byte_t nonsense[] = { 0x80 };                            /* "entry 0": there is none */
    EXAMPLE_REQUIRE(proven_hpack_decode(decoder, (proven_mem_view_t){ nonsense, 1 }, fields, 16, (proven_mem_mut_t){ scratch, sizeof scratch }, 4096, &count) == PROVEN_ERR_INVALID_FORMAT &&
                    proven_hpack_decode(decoder, (proven_mem_view_t){ repeat, 1 }, fields, 16, (proven_mem_mut_t){ scratch, sizeof scratch }, 4096, &count) == PROVEN_ERR_INVALID_FORMAT,
                    "a malformed block is refused, and so is everything after it");

    // ---- the table's size is negotiated -------------------------------------------------
    /* When the peer announces a smaller table, the encoder is told, and says at the start of
     * its next block that it followed. A decoder whose own limit was lowered insists on that. */
    proven_hpack_decoder_destroy(decoder);
    EXAMPLE_REQUIRE(proven_hpack_decoder_create(heap, PROVEN_HPACK_DEFAULT_TABLE_BYTES, &decoder) == PROVEN_OK, "a fresh decoder");
    proven_hpack_decoder_set_max_table_bytes(decoder, 0);          /* "this side will keep no table" ... */
    proven_hpack_encoder_set_peer_table_bytes(encoder, 0);         /* ... heard by the encoder */
    n = 0;
    EXAMPLE_REQUIRE(proven_hpack_encode(encoder, request, 6, NULL, (proven_mem_mut_t){ block, sizeof block }, &n) == PROVEN_OK && block[0] == 0x20, "the next block opens by saying: table size 0");
    EXAMPLE_REQUIRE(proven_hpack_decode(decoder, (proven_mem_view_t){ block, n }, fields, 16, (proven_mem_mut_t){ scratch, sizeof scratch }, 4096, &count) == PROVEN_OK && count == 6 &&
                    proven_hpack_decoder_table_count(decoder) == 0, "and nothing is remembered on either side");

    // ---- Huffman coding, by itself -------------------------------------------------------
    /* Literal strings are sent in a code that gives common characters fewer bits. */
    proven_u8str_view_t text = PROVEN_LIT("www.example.com");
    proven_byte_t coded[32], back[32];
    proven_size_t coded_len = 0, back_len = 0;
    EXAMPLE_REQUIRE(proven_hpack_huffman_size(text) == 12 && proven_hpack_huffman_encode(text, (proven_mem_mut_t){ coded, sizeof coded }, &coded_len) == PROVEN_OK && coded_len == 12, "fifteen characters in twelve bytes");
    EXAMPLE_REQUIRE(proven_hpack_huffman_decode((proven_mem_view_t){ coded, coded_len }, (proven_mem_mut_t){ back, sizeof back }, &back_len) == PROVEN_OK && back_len == 15 && memcmp(back, "www.example.com", 15) == 0, "and back");

    proven_hpack_encoder_destroy(encoder);
    proven_hpack_decoder_destroy(decoder);
    return EXAMPLE_OK();
}
```

### Reference

| API | Intent | Return |
|---|---|---|
| `proven_hpack_encoder_create(alloc, max_table_bytes, &enc)` | An encoder that uses at most this much table (0: none). | `proven_err_t`: `INVALID_ARG`, `NOMEM`. |
| `proven_hpack_encode(enc, headers, count, sensitive, out, &len)` | Append one header block for these fields. `sensitive` is optional. | `proven_err_t`: `OUT_OF_BOUNDS` when `out` has less room than the bound - nothing written, nothing changed. |
| `proven_hpack_encode_bound(headers, count)` | Room that is always enough. | `proven_size_t`. |
| `proven_hpack_encoder_set_peer_table_bytes(enc, n)` | The peer's limit changed; announced in the next block. | none. |
| `proven_hpack_decoder_create(alloc, max_table_bytes, &dec)` | A decoder that allows the peer this much table. | `proven_err_t`. |
| `proven_hpack_decode(dec, block, headers, cap, scratch, max_list_bytes, &count)` | Decode one whole block. | `proven_err_t`: `OUT_OF_BOUNDS` - too large a list, too many fields, too small a scratch; the decoder is still good. `INVALID_FORMAT` - not HPACK; the decoder is finished. |
| `proven_hpack_decoder_set_max_table_bytes(dec, n)` | This side's limit changed; if lowered, the peer's next block must say it followed. | none. |
| `proven_hpack_decoder_table_count`, `_table_bytes`, `_table_entry` | Look at the decoder's table. | counts; `bool`. |
| `proven_hpack_huffman_size`, `_huffman_encode`, `_huffman_decode` | The string code alone. | size; `proven_err_t`. |
| `proven_hpack_encoder_destroy`, `proven_hpack_decoder_destroy` | Free one. Null is accepted. | none. |

### Cautions, and what goes wrong

- **A block is not the size of what it says.** Five bytes can name a large remembered field
  five times; five thousand can name it five thousand times. Always give `max_list_bytes`:
  it bounds the decoded list - names, values and 32 bytes a field, as the RFC counts - and
  `scratch` of that size is always enough.
- **Over the limit is not the end of the connection; malformed is.** `OUT_OF_BOUNDS` means the
  block was read to its end, the table is in step, and only this message is refused.
  `INVALID_FORMAT` means the tables may now differ, and the decoder repeats it for ever.
- **Views, three kinds.** A decoded name or value points into the block, into the library's
  fixed table, or into your scratch. All three must outlive your use of the fields; copy what
  you keep.
- **Secrets stay out of the table.** A table is a compression context, and what is in one can
  be guessed, byte by byte, from the size of later blocks that an attacker influences. The
  encoder never remembers the values of `authorization` and `proxy-authorization` and sends
  them marked so that no hop will; it does not remember `cookie` and `set-cookie`; and
  `sensitive` extends that to any field you name.
- **Names are lowercase before they reach the encoder.** It encodes what it is given; the
  check that a list is fit to send is section 4's.
- **One encoder, one decoder, one direction, in order.** Not per request, not per stream.

## 3. Frames

```c
proven_err_t proven_h2_frame_parse(proven_mem_view_t data, proven_h2_frame_t *out);
proven_err_t proven_h2_frame_check(const proven_h2_frame_t *frame, proven_u32 max_frame_size, proven_h2_error_t *code, bool *stream_error);
proven_err_t proven_h2_frame_write(proven_mem_mut_t out, proven_size_t *len, const proven_h2_frame_t *frame);
```

A frame is nine bytes and a payload:

| Field | Bits | Meaning |
|---|---|---|
| Length | 24 | Bytes of payload. Every endpoint accepts 16,384; more only if it said so |
| Type | 8 | One of ten; anything else is skipped |
| Flags | 8 | Meaning depends on the type |
| Stream | 31 (and one reserved bit) | 0 is the connection itself; a client opens odd streams |

`proven_h2_frame_check` applies, before the payload is looked at, the rule each type has:

| Type | Stream | Size | Carries |
|---|---|---|---|
| `DATA` | required | any | A piece of a body. May be padded. `END_STREAM` ends the sender's side |
| `HEADERS` | required | any | A header block, or its beginning. May be padded; may carry five bytes of priority, which are skipped |
| `CONTINUATION` | required | any | More of a header block, until `END_HEADERS` |
| `RST_STREAM` | required | 4 | An error code: this stream is over |
| `SETTINGS` | must be 0 | a multiple of 6; 0 with `ACK` | Identifier and value pairs |
| `PING` | must be 0 | 8 | Eight bytes to be sent back with `ACK` |
| `GOAWAY` | must be 0 | at least 8 | The last stream acted on, an error code, text for a human |
| `WINDOW_UPDATE` | either | 4 | How many more bytes may be sent |
| `PRIORITY` | required | 5 | Deprecated; checked and ignored |
| `PUSH_PROMISE` | required | any | Server push, which this library always declines |

**An error has one of two sizes.** A *connection error* ends the connection: send GOAWAY with
the code, then close. A *stream error* ends one stream: send RST_STREAM and go on. The check
reports the code and which it is.

### Example: a connection's first bytes, read as a server reads them

<!-- example: manual/examples/en/ex_17_frames.c -->
```c
#include <stdio.h>
#include <string.h>

/*
 * HTTP/2 frames, and the header list inside them - read here from a buffer, as a server
 * reads the first bytes of a connection.
 *
 * Nothing in h2.h holds a connection together. It reads and writes frames, applies the rule
 * each frame type has, and checks that a decoded header list is a request. Which streams are
 * open and how much may be sent is the business of whatever uses these.
 */

/* What a client would send to open a connection and ask for one page. */
static proven_size_t client_bytes(proven_allocator_t heap, proven_byte_t *wire, proven_size_t cap) {
    proven_mem_mut_t out = { wire, cap };
    proven_size_t len = 0;
    memcpy(wire, PROVEN_H2_PREFACE, PROVEN_H2_PREFACE_SIZE);       /* 24 fixed bytes come first */
    len = PROVEN_H2_PREFACE_SIZE;

    /* Then the client's settings: here, "do not push" and a larger window. */
    proven_h2_setting_t settings[] = {
        { PROVEN_H2_SETTINGS_ENABLE_PUSH, 0 },
        { PROVEN_H2_SETTINGS_INITIAL_WINDOW_SIZE, 1048576 },
    };
    if (proven_h2_write_settings(out, &len, settings, 2) != PROVEN_OK) return 0;

    /* A request is a HEADERS frame on a new stream - a client's streams are odd numbers -
     * carrying an HPACK block. END_HEADERS: the block is whole. END_STREAM: no body follows. */
    proven_http_header_t request[] = {
        { PROVEN_LIT(":method"), PROVEN_LIT("GET") },
        { PROVEN_LIT(":scheme"), PROVEN_LIT("https") },
        { PROVEN_LIT(":path"), PROVEN_LIT("/") },
        { PROVEN_LIT(":authority"), PROVEN_LIT("www.example.com") },
        { PROVEN_LIT("accept"), PROVEN_LIT("text/html") },
    };
    proven_hpack_encoder_t *encoder = NULL;
    proven_byte_t block[256];
    proven_size_t block_len = 0;
    if (proven_hpack_encoder_create(heap, PROVEN_HPACK_DEFAULT_TABLE_BYTES, &encoder) != PROVEN_OK) return 0;
    proven_err_t err = proven_hpack_encode(encoder, request, 5, NULL, (proven_mem_mut_t){ block, sizeof block }, &block_len);
    proven_hpack_encoder_destroy(encoder);
    if (err != PROVEN_OK) return 0;
    proven_h2_frame_t headers = { .length = (proven_u32)block_len, .type = PROVEN_H2_HEADERS,
                                  .flags = PROVEN_H2_FLAG_END_HEADERS | PROVEN_H2_FLAG_END_STREAM, .stream_id = 1 };
    if (proven_h2_frame_write(out, &len, &headers) != PROVEN_OK) return 0;
    memcpy(wire + len, block, block_len);
    len += block_len;

    /* And, for the example's sake, one frame that breaks a rule: a PING must be eight bytes. */
    proven_h2_frame_t bad_ping = { .length = 4, .type = PROVEN_H2_PING };
    if (proven_h2_frame_write(out, &len, &bad_ping) != PROVEN_OK) return 0;
    memset(wire + len, 0, 4);
    return len + 4;
}

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();
    proven_byte_t wire[1024];
    proven_size_t size = client_bytes(heap, wire, sizeof wire);
    EXAMPLE_REQUIRE(size > 0, "the client's bytes");

    // ---- the server's side -------------------------------------------------------------
    EXAMPLE_REQUIRE(size >= PROVEN_H2_PREFACE_SIZE && memcmp(wire, PROVEN_H2_PREFACE, PROVEN_H2_PREFACE_SIZE) == 0, "the preface: this is HTTP/2");
    proven_size_t at = PROVEN_H2_PREFACE_SIZE;

    proven_hpack_decoder_t *decoder = NULL;
    EXAMPLE_REQUIRE(proven_hpack_decoder_create(heap, PROVEN_HPACK_DEFAULT_TABLE_BYTES, &decoder) == PROVEN_OK, "the connection's decoder");
    int frames = 0, requests = 0;
    proven_h2_error_t refused_with = PROVEN_H2_NO_ERROR;

    while (at < size) {
        /* Nine bytes of header say how long the frame is, what it is, and whose. */
        proven_h2_frame_t frame;
        proven_err_t err = proven_h2_frame_parse((proven_mem_view_t){ wire + at, size - at }, &frame);
        if (err == PROVEN_ERR_NEED_MORE) break;                    /* a real server would read more */
        EXAMPLE_REQUIRE(err == PROVEN_OK, "a frame header");

        /* Before the payload is looked at: is this frame allowed to be what it says? The size
         * limit is the one this side announced (the protocol's smallest, here). */
        proven_h2_error_t code;
        bool stream_error;
        if (proven_h2_frame_check(&frame, PROVEN_H2_MIN_MAX_FRAME_SIZE, &code, &stream_error) != PROVEN_OK) {
            /* A real server answers GOAWAY (or RST_STREAM when only the stream is at fault). */
            refused_with = code;
            EXAMPLE_REQUIRE(!stream_error, "a PING of the wrong size ends the connection");
            break;
        }
        proven_mem_view_t payload = { wire + at + PROVEN_H2_FRAME_HEADER_SIZE, frame.length };
        EXAMPLE_REQUIRE(PROVEN_H2_FRAME_HEADER_SIZE + frame.length <= size - at, "the whole frame is here");
        frames++;

        if (frame.type == PROVEN_H2_SETTINGS) {
            proven_size_t pos = 0;
            proven_h2_setting_t setting;
            while ((err = proven_h2_settings_next(payload, &pos, &setting, &code)) == PROVEN_OK) {
                if (setting.id == PROVEN_H2_SETTINGS_INITIAL_WINDOW_SIZE) printf("the client's window: %u bytes a stream\n", (unsigned)setting.value);
                /* A setting this side does not know is simply not acted on. */
            }
            EXAMPLE_REQUIRE(err == PROVEN_ERR_EOF, "every setting was in range");
        } else if (frame.type == PROVEN_H2_HEADERS) {
            /* The header block is what is left when padding and the old priority fields are
             * taken away. (With END_HEADERS unset, CONTINUATION frames would carry the rest.) */
            proven_mem_view_t block;
            EXAMPLE_REQUIRE(proven_h2_frame_content(&frame, payload, &block) == PROVEN_OK && (frame.flags & PROVEN_H2_FLAG_END_HEADERS) != 0, "a whole header block");
            proven_http_header_t fields[32];
            proven_byte_t scratch[2048];
            proven_size_t count = 0;
            EXAMPLE_REQUIRE(proven_hpack_decode(decoder, block, fields, 32, (proven_mem_mut_t){ scratch, sizeof scratch }, 16384, &count) == PROVEN_OK, "decoded");

            /* A list of fields is not yet a request. This is where a name in capitals, a line
             * break in a value, a missing :path or a "connection" header are caught. */
            proven_h2_request_head_t head;
            EXAMPLE_REQUIRE(proven_h2_request_head(fields, count, &head) == PROVEN_OK, "a well-formed request");
            printf("stream %u: %.*s %.*s for %.*s, %u more fields\n", (unsigned)frame.stream_id, (int)head.method.size, (const char *)head.method.ptr,
                   (int)head.path.size, (const char *)head.path.ptr, (int)head.authority.size, (const char *)head.authority.ptr, (unsigned)head.field_count);
            EXAMPLE_REQUIRE(proven_u8str_view_eq(head.path, PROVEN_LIT("/")) && head.field_count == 1 && proven_u8str_view_eq(head.fields[0].name, PROVEN_LIT("accept")), "its parts");
            requests++;
        }
        at += PROVEN_H2_FRAME_HEADER_SIZE + frame.length;
    }
    EXAMPLE_REQUIRE(frames == 2 && requests == 1 && refused_with == PROVEN_H2_FRAME_SIZE_ERROR, "two frames taken, one request, and the third frame refused with FRAME_SIZE_ERROR");
    proven_hpack_decoder_destroy(decoder);

    // ---- what the server writes back -----------------------------------------------------
    proven_byte_t reply[256];
    proven_mem_mut_t out = { reply, sizeof reply };
    proven_size_t len = 0;
    static const proven_byte_t opaque[8] = { 'p', 'r', 'o', 'v', 'e', 'n', '!', '!' };
    EXAMPLE_REQUIRE(proven_h2_write_settings(out, &len, NULL, 0) == PROVEN_OK &&       /* its own settings: all defaults */
                    proven_h2_write_settings_ack(out, &len) == PROVEN_OK &&            /* "yours are in force" */
                    proven_h2_write_window_update(out, &len, 0, 1 << 20) == PROVEN_OK &&   /* a megabyte more may be sent on the connection */
                    proven_h2_write_ping(out, &len, opaque, false) == PROVEN_OK &&     /* are you there? */
                    proven_h2_write_rst_stream(out, &len, 3, PROVEN_H2_REFUSED_STREAM) == PROVEN_OK &&   /* not that stream, not now */
                    proven_h2_write_goaway(out, &len, 1, PROVEN_H2_FRAME_SIZE_ERROR, proven_mem_view_from_u8(PROVEN_LIT("a PING is eight bytes"))) == PROVEN_OK,
                    "six frames a server might send");

    /* Read back, as the client would. */
    at = 0;
    int seen = 0;
    while (at < len) {
        proven_h2_frame_t frame;
        proven_h2_error_t code;
        bool stream_error;
        EXAMPLE_REQUIRE(proven_h2_frame_parse((proven_mem_view_t){ reply + at, len - at }, &frame) == PROVEN_OK &&
                        proven_h2_frame_check(&frame, PROVEN_H2_MIN_MAX_FRAME_SIZE, &code, &stream_error) == PROVEN_OK, "each passes its own rule");
        proven_mem_view_t payload = { reply + at + PROVEN_H2_FRAME_HEADER_SIZE, frame.length };
        proven_u32 a = 0, b = 0;
        proven_mem_view_t debug;
        if (frame.type == PROVEN_H2_WINDOW_UPDATE) EXAMPLE_REQUIRE(proven_h2_read_window_update(payload, &a) == PROVEN_OK && a == 1 << 20, "the increment");
        if (frame.type == PROVEN_H2_RST_STREAM) EXAMPLE_REQUIRE(proven_h2_read_rst_stream(payload, &a) == PROVEN_OK && a == PROVEN_H2_REFUSED_STREAM && frame.stream_id == 3, "the reset");
        if (frame.type == PROVEN_H2_GOAWAY) EXAMPLE_REQUIRE(proven_h2_read_goaway(payload, &a, &b, &debug) == PROVEN_OK && a == 1 && b == PROVEN_H2_FRAME_SIZE_ERROR && debug.size == 21, "the goodbye");
        seen++;
        at += PROVEN_H2_FRAME_HEADER_SIZE + frame.length;
    }
    EXAMPLE_REQUIRE(seen == 6, "six frames read back");

    // ---- responses and trailers ----------------------------------------------------------
    proven_http_header_t response[] = { { PROVEN_LIT(":status"), PROVEN_LIT("200") }, { PROVEN_LIT("content-type"), PROVEN_LIT("text/html") } };
    proven_u16 status = 0;
    const proven_http_header_t *rest = NULL;
    proven_size_t rest_count = 0;
    EXAMPLE_REQUIRE(proven_h2_response_head(response, 2, &status, &rest, &rest_count) == PROVEN_OK && status == 200 && rest_count == 1, "a response has one pseudo-header field: its status");
    proven_http_header_t smuggle[] = { { PROVEN_LIT(":status"), PROVEN_LIT("200") }, { PROVEN_LIT("x-note"), PROVEN_LIT("fine\r\ntransfer-encoding: chunked") } };
    EXAMPLE_REQUIRE(proven_h2_response_head(smuggle, 2, &status, &rest, &rest_count) == PROVEN_ERR_INVALID_FORMAT, "a value with a line break in it is no response at all");
    proven_http_header_t trailers[] = { { PROVEN_LIT("x-checksum"), PROVEN_LIT("9f86d081") } };
    EXAMPLE_REQUIRE(proven_h2_trailers_check(trailers, 1) == PROVEN_OK, "trailers are ordinary fields");
    return EXAMPLE_OK();
}
```

### Reference

| API | Intent | Return |
|---|---|---|
| `proven_h2_frame_parse(data, &frame)` | Read nine bytes of frame header. | `proven_err_t`: `NEED_MORE` under nine bytes. |
| `proven_h2_frame_write(out, &len, &frame)` | Append a frame header. | `proven_err_t`: `INVALID_ARG` (length above 2^24-1, stream above 2^31-1), `OUT_OF_BOUNDS`. |
| `proven_h2_frame_check(&frame, max_frame_size, &code, &stream_error)` | Apply the table above. | `proven_err_t`: `INVALID_FORMAT` with the code to send and whether the stream or the connection ends. |
| `proven_h2_frame_content(&frame, payload, &content)` | The content of DATA or HEADERS, without padding and priority. | `proven_err_t`: `INVALID_FORMAT` for padding that does not fit (a connection error, `PROTOCOL_ERROR`). |
| `proven_h2_settings_next(payload, &pos, &setting, &code)` | Read the next setting. | `proven_err_t`: `EOF` at the end; `INVALID_FORMAT` with the code for a value out of range. |
| `proven_h2_write_settings`, `_settings_ack`, `_ping`, `_window_update`, `_rst_stream`, `_goaway` | Append one whole frame. | `proven_err_t`: `OUT_OF_BOUNDS` with nothing written. |
| `proven_h2_read_window_update`, `_rst_stream`, `_goaway` | Read the payload of a checked frame. | `proven_err_t`; a window increment of 0 is `INVALID_FORMAT`. |

### Cautions, and what goes wrong

- **Check before you read.** `proven_h2_frame_parse` cannot fail on nine bytes; whether they
  describe a frame that may exist is the check's answer. A frame longer than the size you
  announced is a connection error whatever it claims to be.
- **Flow control counts the whole payload.** `proven_h2_frame_content` tells you where the
  content of a padded DATA frame is; the window is charged for the padding too.
- **A header block may span frames**, and nothing else may come between them: HEADERS without
  `END_HEADERS` is followed by CONTINUATION frames of the same stream until one has it. Bound
  what you assemble - twice your header list limit is enough for any honest block - or a
  peer can make you hold as much as it cares to send.
- **Unknown is not wrong.** A frame type or a setting this library does not know passes the
  check, as the protocol requires; skip it.
- **Settings have ranges**, and a value outside one is a connection error:
  `ENABLE_PUSH` is 0 or 1, the initial window at most 2^31-1, the frame size from 16,384 to
  2^24-1.

## 4. A header list as a message

```c
proven_err_t proven_h2_request_head(const proven_http_header_t *headers, proven_size_t count, proven_h2_request_head_t *out);
proven_err_t proven_h2_response_head(const proven_http_header_t *headers, proven_size_t count, proven_u16 *status,
                                     const proven_http_header_t **fields, proven_size_t *field_count);
proven_err_t proven_h2_trailers_check(const proven_http_header_t *headers, proven_size_t count);
```

A decoded header list is a list of byte strings. Whether it is a request is a separate
question, and the answer matters most where HTTP/2 meets HTTP/1.1: a value with a line break
in it, passed on as text, becomes two header lines - a request the receiving server never
saw arrive. So a list is *malformed*, and must be refused rather than repaired or forwarded,
when:

| Rule | Why |
|---|---|
| A field name has an uppercase letter, a space, a control byte, a byte above `0x7e`, or a colon anywhere but first | Names are lowercase tokens; a colon starts a pseudo-header field and nothing else |
| A value has NUL, CR or LF, or begins or ends with a space or a tab | These are the delimiters of HTTP/1.1 |
| A pseudo-header field comes after an ordinary one, appears twice, or is one nobody defined | They are the message's first line: once, and first |
| A request lacks `:method`, `:scheme` or `:path`; `:path` is empty for `http` or `https`; a request has `:status` | The first line must be whole. (`CONNECT` has `:method` and `:authority` only) |
| A response lacks `:status`, or it is not three digits | The same |
| `connection`, `proxy-connection`, `keep-alive`, `transfer-encoding` or `upgrade` is present; `te` says anything but `trailers` | These describe an HTTP/1.1 connection. Here they are an attempt to describe someone else's |
| Trailers contain any pseudo-header field | Trailers are ordinary fields |

`proven_h2_request_head` applies them and hands back the parts: method, scheme, authority,
path, and the ordinary fields that follow. A malformed message is a stream error of type
`PROTOCOL_ERROR`: answer `400` if a response can still be sent, reset the stream if not.

## 5. What is accepted, what is not here, and how this was tested

**What the decoder accepts is HPACK as RFC 7541 defines it.** Any number of table size
updates at the start of a block (the RFC promises at most two); integers of up to five
continuation bytes; a literal's Huffman padding of up to seven one bits and nothing else.

**What the encoder writes** is valid HPACK that makes the common choices: a table reference
where there is one, Huffman coding where it is shorter (not where it is the same length), new
fields added to the table unless they are secrets or larger than it.

**Not here:**

- **The connection.** Stream states, flow control windows, the order of frames, settings and
  their acknowledgement, what to send in answer to what: none of it. The HTTP servers and
  clients of this library speak HTTP/1.1 only.
- **Server push**: `PUSH_PROMISE` is recognised so that it can be refused.
- **Priorities** (RFC 7540's scheme, which RFC 9113 deprecated): skipped.
- **`Upgrade: h2c`**, which RFC 9113 removed; **WebSocket over HTTP/2** (RFC 8441); **HTTP/3**.
- **The never-indexed mark on a decoded field**: a proxy would pass it on; nothing here does.

**How this was tested.** The static table and the Huffman code are not typed in: a private
program reads them from the published text of RFC 7541, and stops unless there are 61 entries
and 257 codes that are prefix-free and fill the code space exactly; the registered test checks
the same of the table it is linked with. That test then replays all sixteen examples of the
RFC's Appendix C through the decoder, comparing the dynamic table with the RFC's after each;
has the encoder reproduce, byte for byte, the examples whose choices it shares; refuses
thirteen kinds of malformed block; holds each limit; and decodes every beginning and every
single-bit change of the RFC's blocks. A second test tries each frame type's rule on both
sides of its edge and thirty-one malformed requests. Outside the registered tests: this
decoder read 47,142 blocks that fourteen other implementations produced for the same header
lists (nghttp2, Go, Node, Python, Haskell and Swift encoders, including sequences in which
the table size changes); Node's HTTP/2 server, which is nghttp2, decoded every block this
encoder wrote for 1,290 requests on shared connections; and five million damaged blocks
were decoded under tools that report any read or write outside a buffer, with none reported.

**Not done:** no coverage-guided fuzzing; no measurement of speed (the Huffman decoder reads
a bit at a time); no external review.
