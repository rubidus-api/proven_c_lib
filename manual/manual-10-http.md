# Chapter 10: URLs and HTTP Messages

**Part V - Talking to the operating system. Prerequisites: Part II
([1](manual-01-foundation.md), [2](manual-02-allocation.md), [3](manual-03-strings-text.md)).
[Chapter 9](manual-09-networking.md) is where the bytes come from, but nothing here needs it.**
**After this chapter** you can take a URL apart, turn a request path into one that is safe to
open, parse an HTTP/1.1 request or response as it arrives, tell how long its body is, decode
that body, and write a message that nobody else's input can corrupt.

This chapter covers `url.h` and `http.h`. Both are pure text handling: no socket, no file, no
allocation. Unlike Chapter 9 they are available in a [freestanding](manual-freestanding.md)
build, and `PROVEN_NO_NET` does not remove them.

## Table of contents

1. [A codec, not a server](#1-a-codec-not-a-server)
2. [URLs](#2-urls)
3. [A path that is safe to use](#3-a-path-that-is-safe-to-use)
4. [Parsing a head](#4-parsing-a-head)
5. [How long is the body](#5-how-long-is-the-body)
6. [Decoding a body](#6-decoding-a-body)
7. [Writing a message](#7-writing-a-message)
8. [Dates](#8-dates)
9. [What is not here](#9-what-is-not-here)

## 1. A codec, not a server

`http.h` does not read from anything and does not write to anything. You hand it bytes you
already have and it tells you what they say; you hand it memory and it writes a message into it.
What carries those bytes - a blocking socket, an event loop, a TLS session, a test - is not its
business, and that is the point: the part of HTTP that is hard to get right is the same in all
of them, and here it is written once.

Two properties follow, and the rest of the chapter leans on both.

**A prefix is never an error.** Bytes arrive in whatever pieces the network makes. If what has
arrived so far is the beginning of a valid message, the parser answers `PROVEN_ERR_NEED_MORE` -
never a failure, never a premature success. So the loop around it is always the same: parse; if
it needs more, read more; otherwise act.

**Ambiguity is refused, not resolved.** An HTTP message is routinely read by two programs - a
proxy in front and a server behind. Wherever the two read the same bytes differently, one can
be shown a request the other never sees. That is *request smuggling*, and it lives entirely in
the places where a lenient parser quietly picks an interpretation. This one does not pick:

| What is refused | Why it matters |
|---|---|
| A line ending that is a bare LF, or a bare CR | One parser sees one line where another sees two |
| Whitespace between a header name and its colon | `Content-Length : 5` is a header to one parser and not to another |
| A header line starting with a space or tab (obsolete folding) | A continuation to one parser, a new field to another |
| More than one `Content-Length`, or one that is not plain digits | Two lengths means two opinions about where the body ends |
| `Transfer-Encoding` together with `Content-Length` | The classic: each parser believes a different one |
| Any transfer coding but a single `chunked` | A parser that skips an unknown coding frames the body differently |
| A chunk size with whitespace, a sign, a `0x`, or more than 16 digits | Chunk-size parsing is the other classic |

Each row is something deployed software accepts and published attacks use. A refusal is an
ordinary error value - answer 400 and close.

## 2. URLs

`proven_url_parse` splits an absolute URL into views of the text you gave it:

```text
https://user:pw@example.com:8443/docs/a%20b?q=1#top
\___/   \_____/ \_________/ \__/\_________/ \_/ \_/
scheme  userinfo    host    port   path    query fragment
```

```text
typedef struct {
    proven_u8str_view_t scheme, userinfo, host, path, query, fragment;
    proven_u16 port;
    bool has_userinfo, has_port, has_query, has_fragment;
    bool host_is_ipv6;      /* written in brackets; `host` is without them */
} proven_url_t;
```

**Parsing does not decode.** Every component comes back as written, percent signs included.
Decoding is a separate step taken on the one piece you are about to use - because a URL decoded
as a whole cannot be taken apart again. A `%2F` that was data inside a path segment becomes a
`/` that separates segments; a `%26` inside a query value becomes an `&` that ends it.

### Reference

| API | Intent | Return |
|---|---|---|
| `proven_url_parse(text, &url)` | Split `scheme://authority/path?query#fragment`. ASCII only, no spaces, every `%` followed by two hex digits. | `proven_err_t`: `INVALID_FORMAT` for anything else; `url` is then untouched. |
| `proven_url_default_port(scheme)` | 80 for `http`/`ws`, 443 for `https`/`wss`, 21 for `ftp`. | `proven_u16`; 0 for an unknown scheme. |
| `proven_url_effective_port(&url)` | The port to connect to: the one written, else the scheme's. | `proven_u16`. |
| `proven_url_scheme_is(&url, scheme)` | Compare the scheme without case. | `bool`. |
| `proven_url_split_target(target, &path, &query, &has_query)` | Split an HTTP request target: `/a?b`, `http://h/a?b`, or `*`. | `proven_err_t`: `INVALID_FORMAT` otherwise. |
| `proven_url_query_iter(query)`, `proven_url_query_next(&it, &name, &value)` | Walk `name=value&...`, yielding encoded views. Empty pairs are skipped. | iterator; `bool` (false at the end). |
| `proven_url_percent_decode(in, out, &written)` | `%XX` to bytes. `out` may be `in`. `+` stays `+`. | `proven_err_t`: `INVALID_FORMAT` for a bad escape; `OUT_OF_BOUNDS`. |
| `proven_url_form_decode(in, out, &written)` | The same, and `+` is a space: for query names and values. | `proven_err_t`. |
| `proven_url_encode_component(in, out, &written)` | Encode for ONE component: all but letters, digits and `-._~` become `%XX`. | `proven_err_t`: `OUT_OF_BOUNDS`; `OVERFLOW`. |
| `proven_url_encode_path(in, out, &written)` | The same, keeping `/`. | `proven_err_t`. |
| `proven_url_form_encode(in, out, &written)` | The same, writing a space as `+`. | `proven_err_t`. |
| `proven_url_encoded_size_max(n)` | Bytes always enough to encode `n`. | `proven_size_t`; `SIZE_MAX` if unrepresentable. |

This is RFC 3986, not the URL algorithm browsers implement. A browser repairs what it is given -
backslashes become slashes, spaces are encoded, a missing scheme is guessed. Nothing here is
repaired: text that would need it is `PROVEN_ERR_INVALID_FORMAT`. A program that accepts URLs
typed by people should say so to them rather than guess.

### Cautions, and what goes wrong

**Decode a component, never the whole URL.**

Wrong:

```text
proven_url_percent_decode(whole_url, buf, &n);      /* wrong: "a%2Fb" and "a/b" are now the same path */
proven_url_parse(decoded, &url);
```

Correct - parse first, then decode the one component you need, with the decoder that matches
it: `proven_url_form_decode` for a query name or value, `proven_url_path_resolve` for a path.

**Decoded text is bytes.** `proven_url_percent_decode` returns whatever was encoded: a NUL, a
newline, bytes that are not UTF-8. It is the sender's data. Validate it for what you are about
to do with it.

**The userinfo is not the host.** In `http://example.com@evil.test/` the host is `evil.test`.
The parser gets this right - the host begins after the *last* `@` - but a program that shows a
URL to a person, or checks it against an allow-list, must compare `url.host`, never a prefix of
the text.

Compiled and run by the test suite:

<!-- example: manual/examples/en/ex_10_url.c -->
```c
/*
 * URLs: take one apart, decode only the piece you are about to use, and never let a path
 * from the network reach the filesystem without resolving it first.
 */

static bool view_is(proven_u8str_view_t v, const char *text) {
    return proven_u8str_view_eq(v, proven_u8str_view_from_cstr(text));
}

int main(void) {
    /* Parsing gives views of what was written. Nothing is decoded and nothing is copied. */
    proven_url_t url;
    proven_err_t err = proven_url_parse(PROVEN_LIT("https://example.com:8443/docs/a%20b?q=caf%C3%A9+au+lait&page=2#top"), &url);
    EXAMPLE_REQUIRE(err == PROVEN_OK, "an absolute URL parses");
    EXAMPLE_REQUIRE(view_is(url.host, "example.com") && url.has_port && url.port == 8443, "host and port");
    EXAMPLE_REQUIRE(view_is(url.path, "/docs/a%20b"), "the path is still encoded");
    EXAMPLE_REQUIRE(url.has_query && url.has_fragment && view_is(url.fragment, "top"), "query and fragment are there");

    /* Scheme names compare without case; a URL with no port gets the scheme's own. */
    proven_url_t plain;
    EXAMPLE_REQUIRE(proven_url_parse(PROVEN_LIT("HTTP://example.com/"), &plain) == PROVEN_OK, "another URL");
    EXAMPLE_REQUIRE(proven_url_scheme_is(&plain, PROVEN_LIT("http")), "HTTP is http");
    EXAMPLE_REQUIRE(proven_url_effective_port(&plain) == 80 && proven_url_effective_port(&url) == 8443,
                    "the port to connect to: the default, or the one written");
    EXAMPLE_REQUIRE(proven_url_default_port(PROVEN_LIT("wss")) == 443, "wss is 443");

    /* What is not an absolute URL is refused, not repaired. */
    proven_url_t bad;
    EXAMPLE_REQUIRE(proven_url_parse(PROVEN_LIT("example.com/path"), &bad) == PROVEN_ERR_INVALID_FORMAT, "no scheme: refused");
    EXAMPLE_REQUIRE(proven_url_parse(PROVEN_LIT("http://example.com/a b"), &bad) == PROVEN_ERR_INVALID_FORMAT, "a raw space: refused");

    /* A query is walked pair by pair; each name and value is decoded on its own, as form data,
     * where '+' means a space. Decoding the whole query first would turn an encoded '&' inside
     * a value into a separator. */
    proven_url_query_iter_t it = proven_url_query_iter(url.query);
    proven_u8str_view_t name, value;
    proven_byte_t text[64];
    proven_size_t n = 0;
    EXAMPLE_REQUIRE(proven_url_query_next(&it, &name, &value) && view_is(name, "q"), "the first pair is q");
    EXAMPLE_REQUIRE(proven_url_form_decode(value, (proven_mem_mut_t){ text, sizeof text }, &n) == PROVEN_OK &&
                    view_is((proven_u8str_view_t){ text, n }, "caf\xc3\xa9 au lait"), "its value, decoded");
    EXAMPLE_REQUIRE(proven_url_query_next(&it, &name, &value) && view_is(name, "page") && view_is(value, "2"), "the second pair");
    EXAMPLE_REQUIRE(!proven_url_query_next(&it, &name, &value), "and no third");

    /* Plain percent-decoding leaves '+' alone, and yields BYTES - any bytes. */
    EXAMPLE_REQUIRE(proven_url_percent_decode(PROVEN_LIT("a%2Fb+c"), (proven_mem_mut_t){ text, sizeof text }, &n) == PROVEN_OK &&
                    view_is((proven_u8str_view_t){ text, n }, "a/b+c"), "%2F becomes a slash; '+' stays");

    /* Going the other way: encode a value so that it cannot end the component it goes into. */
    proven_mem_view_t file_name = proven_mem_view_from_u8(PROVEN_LIT("report 100%/final?.txt"));
    proven_byte_t enc[96];
    EXAMPLE_REQUIRE(proven_url_encoded_size_max(file_name.size) <= sizeof enc, "three bytes per byte is always enough");
    EXAMPLE_REQUIRE(proven_url_encode_component(file_name, (proven_mem_mut_t){ enc, sizeof enc }, &n) == PROVEN_OK &&
                    view_is((proven_u8str_view_t){ enc, n }, "report%20100%25%2Ffinal%3F.txt"), "one path segment: even the slash is encoded");
    EXAMPLE_REQUIRE(proven_url_encode_path(proven_mem_view_from_u8(PROVEN_LIT("/my docs/a b.txt")), (proven_mem_mut_t){ enc, sizeof enc }, &n) == PROVEN_OK &&
                    view_is((proven_u8str_view_t){ enc, n }, "/my%20docs/a%20b.txt"), "a whole path: the slashes stay");
    EXAMPLE_REQUIRE(proven_url_form_encode(proven_mem_view_from_u8(PROVEN_LIT("a b&c")), (proven_mem_mut_t){ enc, sizeof enc }, &n) == PROVEN_OK &&
                    view_is((proven_u8str_view_t){ enc, n }, "a+b%26c"), "form data: a space is '+'");

    /* A server's side of it. The request target is split first... */
    proven_u8str_view_t path, query;
    bool has_query = false;
    EXAMPLE_REQUIRE(proven_url_split_target(PROVEN_LIT("/static/css/../img/logo%20v2.png?v=3"), &path, &query, &has_query) == PROVEN_OK &&
                    has_query && view_is(query, "v=3"), "target into path and query");

    /* ...and the path is resolved before anything else looks at it: decoded once, dot segments
     * removed, and refused if it would leave the root. */
    proven_byte_t clean[128];
    EXAMPLE_REQUIRE(proven_url_path_resolve(path, (proven_mem_mut_t){ clean, sizeof clean }, &n) == PROVEN_OK &&
                    view_is((proven_u8str_view_t){ clean, n }, "/static/img/logo v2.png"), "a clean path under the root");

    /* The requests a file server exists to refuse. */
    EXAMPLE_REQUIRE(proven_url_path_resolve(PROVEN_LIT("/../etc/passwd"), (proven_mem_mut_t){ clean, sizeof clean }, &n) == PROVEN_ERR_PERMISSION,
                    "climbing above the root");
    EXAMPLE_REQUIRE(proven_url_path_resolve(PROVEN_LIT("/%2e%2e/etc/passwd"), (proven_mem_mut_t){ clean, sizeof clean }, &n) == PROVEN_ERR_PERMISSION,
                    "the same climb with the dots encoded");
    EXAMPLE_REQUIRE(proven_url_path_resolve(PROVEN_LIT("/..%2f..%2fetc/passwd"), (proven_mem_mut_t){ clean, sizeof clean }, &n) == PROVEN_ERR_INVALID_FORMAT,
                    "an encoded slash");
    EXAMPLE_REQUIRE(proven_url_path_resolve(PROVEN_LIT("/file%00.png"), (proven_mem_mut_t){ clean, sizeof clean }, &n) == PROVEN_ERR_INVALID_FORMAT,
                    "an encoded NUL");
    EXAMPLE_REQUIRE(proven_url_path_resolve(PROVEN_LIT("/%c0%ae%c0%ae/"), (proven_mem_mut_t){ clean, sizeof clean }, &n) == PROVEN_ERR_INVALID_FORMAT,
                    "an overlong encoding of a dot");

    return EXAMPLE_OK();
}
```

## 3. A path that is safe to use

A request path names a file, and the sender chooses it. `GET /../../etc/passwd` is the oldest
attack on a file server and it has many spellings. `proven_url_path_resolve` is the one function
to put between a request path and anything that opens files:

| API | Intent | Return |
|---|---|---|
| `proven_url_path_resolve(raw_path, out, &written)` | Decode once, refuse what has no place in a file name, resolve `.` and `..`. The result starts with `/`, has no dot or empty segments, and keeps a trailing `/`. `out` needs at most `raw_path.size` bytes. | `proven_err_t`: `PERMISSION` when a `..` would climb above the root; `INVALID_FORMAT` for the refusals below; `OUT_OF_BOUNDS` when `out` is too small. |

What it does, in order - and the order is the security property:

1. **Decodes once.** `%2e%2e` becomes `..` and is then treated as `..`. And `%252e%252e` becomes
   the text `%2e%2e`, which is a strange file name and nothing more - decoding again would turn
   it into a traversal, which is the *double decoding* bug.
2. **Refuses** an encoded slash (`%2F` - it would split one segment into two after the fact), a
   backslash written or encoded (a separator on Windows), any control character including NUL
   (`file%00.png` truncates the name in C), and anything that is not valid UTF-8 once decoded -
   which is what catches `%c0%ae`, an *overlong* encoding of a dot that some decoders accept.
3. **Resolves** `.` and `..` on the decoded text. A `..` with nothing left to remove is
   `PROVEN_ERR_PERMISSION`: the path is well formed and asks for what it may not have.

Wrong - checking for `..` before decoding:

```text
if (contains(raw_path, "..")) return 400;         /* wrong: "%2e%2e" sails through */
proven_url_percent_decode(raw_path, buf, &n);
open_under_root(buf, n);
```

Wrong - decoding twice "to be safe":

```text
proven_url_percent_decode(raw, a, &n);
proven_url_percent_decode(a_view, b, &m);          /* wrong: "%252e%252e" has just become ".." */
```

Correct:

```text
proven_err_t err = proven_url_path_resolve(raw_path, clean_mem, &n);
if (err == PROVEN_ERR_PERMISSION)     return 403;   /* or 400 */
if (err != PROVEN_OK)                 return 400;
open_under_root(clean, n);
```

**What it does not do.** It stops traversal by path *syntax*. It does not know your filesystem: a
symbolic link inside the root can still point outside it, and Windows gives some file names a
meaning of their own (`CON`, a trailing dot, `name:stream`). Check those where the file is
opened. The example above shows the resolver; the file-opening half belongs to
[Chapter 5](manual-05-hosted-services.md).

## 4. Parsing a head

A *head* is everything before the body: the request line or status line, the header fields, and
the empty line that ends them.

```text
typedef struct { proven_u8str_view_t name, value; } proven_http_header_t;

typedef struct {
    proven_http_method_t method;       /* GET, POST, ...; OTHER for a token the enum has no name for */
    proven_u8str_view_t method_text;
    proven_u8str_view_t target;        /* "/a?b", "http://h/a", "*", or "host:port" */
    proven_u8 version_minor;           /* 1 or 0 */
    proven_http_header_t *headers;     /* the array you supplied */
    proven_size_t header_count;
} proven_http_request_t;

typedef struct {
    proven_u16 status;
    proven_u8str_view_t reason;        /* may be empty; means nothing to a program */
    proven_u8 version_minor;
    proven_http_header_t *headers;
    proven_size_t header_count;
} proven_http_response_t;
```

Every view points into the buffer you parsed. Nothing is copied, so the buffer must outlive the
parsed head and must not move - and the array for the fields is yours, too.

### Reference

| API | Intent | Return |
|---|---|---|
| `proven_http_parse_request(data, headers, cap, max_head, &req, &head_size)` | Parse a request head from the front of `data`. `head_size` is where the body starts. `max_head` 0 means `PROVEN_HTTP_DEFAULT_MAX_HEAD` (16 KiB). | `proven_err_t`: `NEED_MORE` (read more); `OUT_OF_BOUNDS` (head over the limit, or more fields than `cap`: 431); `INVALID_FORMAT` (400); `UNSUPPORTED` (not HTTP/1.0 or 1.1: 505). |
| `proven_http_parse_response(data, headers, cap, max_head, &res, &head_size)` | The same for a response. | as above. |
| `proven_http_header_find(headers, count, name, &value)` | The first field of that name, without case. `value` may be NULL. | `bool`. |
| `proven_http_header_count(headers, count, name)` | How many fields have that name. | `proven_size_t`. |
| `proven_http_header_has_token(headers, count, name, token)` | Whether any such field lists `token` in a comma-separated value: `Connection: keep-alive, Upgrade` lists `upgrade`. | `bool`. |
| `proven_http_method_from_text(text)`, `proven_http_method_text(method)` | Between a method token and the enum. Case-sensitive: `get` is not `GET`. | enum; view (empty for `OTHER`). |
| `proven_http_reason_phrase(status)` | The standard phrase: `Not Found`. | view; `Unknown` for a code with none. |
| `proven_http_request_keep_alive(&req)`, `proven_http_response_keep_alive(&res)` | Whether the connection may carry another message: HTTP/1.1 unless `Connection: close`; HTTP/1.0 only with `Connection: keep-alive`. | `bool`. |

**The limit is checked while the head is still incomplete.** If `max_head` bytes have arrived
and hold no end of head, the answer is `PROVEN_ERR_OUT_OF_BOUNDS` at once - not `NEED_MORE`. A
client that sends a header line for ever costs you `max_head` bytes and no more.

Empty lines before a request line are skipped (clients have long sent a stray CRLF after a
body) and counted in `head_size`.

### Cautions, and what goes wrong

**`NEED_MORE` is not a failure.**

Wrong:

```text
if (proven_http_parse_request(data, h, 32, 0, &req, &n) != PROVEN_OK) return send_400();   /* wrong: a slow client gets a 400 */
```

Correct - three outcomes, three responses: `NEED_MORE` reads more (with a deadline, from
[Chapter 9](manual-09-networking.md)); `OUT_OF_BOUNDS`, `INVALID_FORMAT` and `UNSUPPORTED` answer
431, 400 and 505 and close; `PROVEN_OK` goes on.

**A field can repeat.** `proven_http_header_find` returns the first. For a field where
repetition changes the meaning, ask `proven_http_header_count` - which is exactly how this
library refuses a second `Content-Length`.

**Views die with the buffer.** If you read the next request into the same buffer, every view
from the previous head now points at something else. Copy what you need to keep.

## 5. How long is the body

After the head, the single most important question is where the message ends, because the next
message starts there. The head answers it:

```text
typedef struct {
    proven_http_body_kind_t kind;   /* NONE, LENGTH, CHUNKED, or UNTIL_CLOSE (responses only) */
    proven_u64 length;              /* for LENGTH */
} proven_http_framing_t;
```

| API | Intent | Return |
|---|---|---|
| `proven_http_request_framing(&req, &framing)` | Chunked, a length, or no body. | `proven_err_t`: `INVALID_FORMAT` (400) for the ambiguous cases of section 1; `UNSUPPORTED` (501) for a transfer coding other than `chunked`. |
| `proven_http_response_framing(&res, request_method, &framing)` | The same, knowing what was asked: a response to `HEAD`, a 1xx, 204 or 304, and a successful `CONNECT` have no body. With neither header the body runs until the peer closes. | as above. |

A response needs the request's method because the same headers mean different things: a
response to `HEAD` carries the `Content-Length` the `GET` would have had, and no body at all.

## 6. Decoding a body

```text
proven_http_body_t body;                       /* caller-owned; nothing to destroy */
proven_http_body_init(&body, framing, max_body_bytes);
proven_http_body_feed(&body, in, &consumed, &payload, &done);
```

| API | Intent | Return |
|---|---|---|
| `proven_http_body_init(&body, framing, max_body)` | Begin. `max_body` is how much you will accept - there is no default. | `proven_err_t`: `OUT_OF_BOUNDS` (413) when a `Content-Length` already exceeds it. |
| `proven_http_body_feed(&body, in, &consumed, &payload, &done)` | Consume a prefix of `in`; `payload` is the body bytes in it, as a view INTO `in`. | `proven_err_t`: `INVALID_FORMAT` (bad chunk framing); `OUT_OF_BOUNDS` (over the limit); `INVALID_STATE` after an error. |
| `proven_http_body_end(&body)` | The peer closed: was the body complete? | `PROVEN_OK`, or `PROVEN_ERR_NEED_MORE` when the message was cut short. |
| `proven_http_body_received(&body)` | Body bytes delivered so far. | `proven_u64`. |

`feed` copies nothing. For a chunked body it steps over the chunk framing and hands back the
data between it, one contiguous piece per call - so the loop is: feed what you have; use
`payload`; advance by `consumed`; when everything is consumed and `done` is still false, read
more. When `done` turns true, bytes of `in` past `consumed` belong to the **next** message.

**State your limit.** `max_body` is the answer to "how much may a stranger make me hold". A
`Content-Length` over it is refused before a byte of body is read; a chunk whose size would
cross it is refused when the size is read, not after its data has arrived.

**A close is not always an end.** A body that runs until close ends, properly, when the peer
closes. Any other body that is interrupted by a close is *truncated* - `proven_http_body_end`
returns `PROVEN_ERR_NEED_MORE` - and must not be treated as whole. Half a file that looks like a
whole file is the worst possible outcome of a download.

**After an error the connection is finished.** Once framing is wrong, where the next message
begins is unknown. Answer if you can, and close.

Compiled and run by the test suite - a request arriving in four awkward pieces:

<!-- example: manual/examples/en/ex_10_http_request.c -->
```c
/*
 * A server's view of one request: bytes arrive in pieces, the head is parsed when it is all
 * there, the framing says how long the body is, and the body is decoded as it comes.
 *
 * No socket appears here. The codec takes bytes you have; where they came from is your affair,
 * which is why this same code runs under a blocking read, an event loop, or a test like this.
 */

/* The request, as the network might deliver it: split in awkward places. */
static const char *const pieces[] = {
    "POST /upload/notes.txt?overwrite=1 HTT",
    "P/1.1\r\nHost: example.com\r\nTransfer-Encoding: chun",
    "ked\r\nConnection: keep-alive, TE\r\nX-Tag: a\r\nX-Tag: b\r\n\r\n5\r\nhel",
    "lo\r\n7;note=x\r\n, world\r\n0\r\n\r\nGET /next HTTP/1.1\r\n\r\n",
};

int main(void) {
    proven_byte_t buf[512];                 /* the read buffer: the head must fit in it */
    proven_size_t have = 0;
    proven_size_t next_piece = 0;

    proven_http_header_t fields[16];
    proven_http_request_t req;
    proven_size_t head_size = 0;

    /* Read until the head is complete. NEED_MORE is not an error: it means exactly that. */
    for (;;) {
        proven_err_t err = proven_http_parse_request((proven_mem_view_t){ buf, have }, fields, 16, sizeof buf, &req, &head_size);
        if (err == PROVEN_OK) break;
        EXAMPLE_REQUIRE(err == PROVEN_ERR_NEED_MORE, "an incomplete head asks for more; anything else is a 400, 431 or 505");
        if (err != PROVEN_ERR_NEED_MORE) return EXAMPLE_OK();
        /* "read": append the next piece. */
        proven_size_t len = proven_cstr_len(pieces[next_piece]);
        for (proven_size_t i = 0; i < len; ++i) buf[have + i] = (proven_byte_t)pieces[next_piece][i];
        have += len;
        next_piece++;
    }

    /* The head, as views into `buf`. */
    EXAMPLE_REQUIRE(req.method == PROVEN_HTTP_POST && req.version_minor == 1, "POST, HTTP/1.1");
    EXAMPLE_REQUIRE(proven_http_method_from_text(req.method_text) == PROVEN_HTTP_POST &&
                    proven_u8str_view_eq(proven_http_method_text(req.method), PROVEN_LIT("POST")), "the method, both ways");

    proven_u8str_view_t host;
    EXAMPLE_REQUIRE(proven_http_header_find(fields, req.header_count, PROVEN_LIT("host"), &host) &&
                    proven_u8str_view_eq(host, PROVEN_LIT("example.com")), "header names compare without case");
    EXAMPLE_REQUIRE(proven_http_header_count(fields, req.header_count, PROVEN_LIT("X-Tag")) == 2, "a field may repeat");
    EXAMPLE_REQUIRE(proven_http_header_has_token(fields, req.header_count, PROVEN_LIT("Connection"), PROVEN_LIT("te")),
                    "a token inside a comma-separated list");
    EXAMPLE_REQUIRE(proven_http_request_keep_alive(&req), "the connection stays open for another request");

    /* Where the request points. Split, then resolve - before the path is used for anything. */
    proven_u8str_view_t path, query;
    bool has_query = false;
    proven_byte_t clean[128];
    proven_size_t clean_len = 0;
    EXAMPLE_REQUIRE(proven_url_split_target(req.target, &path, &query, &has_query) == PROVEN_OK &&
                    proven_url_path_resolve(path, (proven_mem_mut_t){ clean, sizeof clean }, &clean_len) == PROVEN_OK &&
                    proven_u8str_view_eq((proven_u8str_view_t){ clean, clean_len }, PROVEN_LIT("/upload/notes.txt")),
                    "the target's path, resolved");

    /* How long is the body? The head decides, and an ambiguous head is an error, not a guess. */
    proven_http_framing_t framing;
    EXAMPLE_REQUIRE(proven_http_request_framing(&req, &framing) == PROVEN_OK && framing.kind == PROVEN_HTTP_BODY_CHUNKED,
                    "this body is chunked");

    /* Decode it. The limit is the caller's statement of how much a peer may send. */
    proven_http_body_t body;
    EXAMPLE_REQUIRE(proven_http_body_init(&body, framing, 1024) == PROVEN_OK, "at most 1 KiB of body");

    proven_byte_t content[64];
    proven_size_t content_len = 0;
    proven_size_t pos = head_size;          /* the body starts where the head ended */
    bool done = false;
    while (!done) {
        if (pos == have) {
            /* Everything read so far is used up: "read" more. A real server would stop here
             * with proven_http_body_end if the peer had closed instead. */
            EXAMPLE_REQUIRE(next_piece < sizeof pieces / sizeof pieces[0], "more input exists");
            proven_size_t len = proven_cstr_len(pieces[next_piece]);
            for (proven_size_t i = 0; i < len; ++i) buf[have + i] = (proven_byte_t)pieces[next_piece][i];
            have += len;
            next_piece++;
        }
        proven_size_t used = 0;
        proven_mem_view_t payload;
        proven_err_t err = proven_http_body_feed(&body, (proven_mem_view_t){ buf + pos, have - pos }, &used, &payload, &done);
        EXAMPLE_REQUIRE(err == PROVEN_OK, "the chunk framing is well formed");
        if (err != PROVEN_OK) break;
        /* `payload` is a view into `buf`: body bytes with the chunk framing stepped over. */
        for (proven_size_t i = 0; i < payload.size; ++i) content[content_len + i] = payload.ptr[i];
        content_len += payload.size;
        pos += used;
    }
    EXAMPLE_REQUIRE(proven_u8str_view_eq((proven_u8str_view_t){ content, content_len }, PROVEN_LIT("hello, world")),
                    "two chunks, joined");
    EXAMPLE_REQUIRE(proven_http_body_received(&body) == 12 && proven_http_body_end(&body) == PROVEN_OK, "twelve bytes, complete");

    /* What is left in the buffer is the next request, untouched. */
    proven_http_request_t next;
    EXAMPLE_REQUIRE(proven_http_parse_request((proven_mem_view_t){ buf + pos, have - pos }, fields, 16, 0, &next, &head_size) == PROVEN_OK &&
                    proven_u8str_view_eq(next.target, PROVEN_LIT("/next")), "the decoder stopped exactly where the next request begins");

    return EXAMPLE_OK();
}
```

## 7. Writing a message

The writers append to memory you supply. Each takes the buffer and a length, writes at that
length, and advances it; on failure the length does not move and nothing before it is touched.

| API | Appends |
|---|---|
| `proven_http_write_request_line(out, &len, method, target)` | `GET /path HTTP/1.1` CRLF |
| `proven_http_write_status_line(out, &len, status, reason)` | `HTTP/1.1 404 Not Found` CRLF; an empty `reason` sends the standard phrase |
| `proven_http_write_header(out, &len, name, value)` | `Name: value` CRLF |
| `proven_http_write_header_u64(out, &len, name, number)` | `Content-Length: 1234` CRLF |
| `proven_http_write_head_end(out, &len)` | the empty line that ends the head |
| `proven_http_write_chunk_begin(out, &len, size)` | the line that starts a chunk of `size` bytes (not 0) |
| `proven_http_write_chunk_end(out, &len)` | the CRLF after a chunk's data |
| `proven_http_write_last_chunk(out, &len)` | the zero chunk and empty trailers that end a chunked body |

All return `proven_err_t`: `PROVEN_ERR_OUT_OF_BOUNDS` when it does not fit, and
`PROVEN_ERR_INVALID_ARG` when what you asked it to write would not be the thing it is named for.

**That second error is the important one.** A header value almost always comes from somewhere
else - a file name, a redirect target, something the client sent. If it contains a CR and an LF,
writing it verbatim ends the field and starts a new one that the *sender* chose:

```text
Location: /home
Set-Cookie: session=attacker        <- this line came from inside the "value"
```

That is *response splitting*, the writing twin of request smuggling. `proven_http_write_header`
refuses any value containing CR, LF, NUL or another control character, and any name that is not
a token; `proven_http_write_request_line` refuses a target with a space or a line break. The
check is not optional and has no "raw" variant.

Wrong - one check at the end:

```text
proven_http_write_status_line(out, &len, 200, PROVEN_LIT(""));
proven_http_write_header(out, &len, name, user_value);      /* refused: len did not move */
err = proven_http_write_head_end(out, &len);                /* OK - and the header is silently missing */
```

Correct - check each call, or chain them so the first failure stops the rest, as the example
does. The writers are `[[nodiscard]]`, so the compiler objects to the first form.

A body of known length is `Content-Length` and then the bytes, which you write yourself. A body
of unknown length is `Transfer-Encoding: chunked` and then, for each piece, `chunk_begin`, the
data, `chunk_end` - and `last_chunk` at the end.

Compiled and run by the test suite:

<!-- example: manual/examples/en/ex_10_http_response.c -->
```c
/*
 * Writing a response and reading one back.
 *
 * Each writer appends at `len` and moves it. A value that could break the message - a line
 * break in a header - is refused instead of written, so a string taken from a request cannot
 * be used to add a header of the sender's choosing.
 */

int main(void) {
    proven_byte_t msg[512];
    proven_mem_mut_t out = { msg, sizeof msg };
    proven_size_t len = 0;

    /* The date header: the wall clock, in the one form HTTP writes. */
    proven_byte_t date[PROVEN_HTTP_DATE_SIZE];
    proven_time_t now = proven_time_now();
    EXAMPLE_REQUIRE(proven_http_date_format(now, date) == PROVEN_OK, "now, as an HTTP date");

    /* A response whose length is not known in advance goes out in chunks. */
    proven_err_t err = proven_http_write_status_line(out, &len, 200, PROVEN_LIT(""));     /* "" : the standard phrase */
    if (err == PROVEN_OK) err = proven_http_write_header(out, &len, PROVEN_LIT("Date"), (proven_u8str_view_t){ date, sizeof date });
    if (err == PROVEN_OK) err = proven_http_write_header(out, &len, PROVEN_LIT("Content-Type"), PROVEN_LIT("text/plain; charset=utf-8"));
    if (err == PROVEN_OK) err = proven_http_write_header(out, &len, PROVEN_LIT("Transfer-Encoding"), PROVEN_LIT("chunked"));
    if (err == PROVEN_OK) err = proven_http_write_header_u64(out, &len, PROVEN_LIT("X-Request-Id"), 4711);
    if (err == PROVEN_OK) err = proven_http_write_head_end(out, &len);
    EXAMPLE_REQUIRE(err == PROVEN_OK, "the head is written");
    proven_size_t head_len = len;

    static const char *const parts[] = { "written in ", "two chunks" };
    for (proven_size_t i = 0; i < 2 && err == PROVEN_OK; ++i) {
        proven_size_t n = proven_cstr_len(parts[i]);
        err = proven_http_write_chunk_begin(out, &len, n);
        if (err != PROVEN_OK || n > sizeof msg - len) { err = PROVEN_ERR_OUT_OF_BOUNDS; break; }
        for (proven_size_t k = 0; k < n; ++k) msg[len + k] = (proven_byte_t)parts[i][k];   /* the chunk's data */
        len += n;
        err = proven_http_write_chunk_end(out, &len);
    }
    if (err == PROVEN_OK) err = proven_http_write_last_chunk(out, &len);
    EXAMPLE_REQUIRE(err == PROVEN_OK, "the body is written");

    /* A header value with a line break in it is not written at all. */
    proven_size_t before = len;
    EXAMPLE_REQUIRE(proven_http_write_header(out, &len, PROVEN_LIT("Location"), PROVEN_LIT("/home\r\nSet-Cookie: session=stolen")) == PROVEN_ERR_INVALID_ARG &&
                    len == before, "response splitting is refused, and nothing was appended");

    /* --- the client's side: the same bytes, read back ------------------------------- */

    proven_http_header_t fields[16];
    proven_http_response_t res;
    proven_size_t head_size = 0;
    EXAMPLE_REQUIRE(proven_http_parse_response((proven_mem_view_t){ msg, len }, fields, 16, 0, &res, &head_size) == PROVEN_OK &&
                    head_size == head_len, "the head parses, and ends where it was written to end");
    EXAMPLE_REQUIRE(res.status == 200 && proven_u8str_view_eq(res.reason, proven_http_reason_phrase(200)), "200 OK");
    EXAMPLE_REQUIRE(proven_http_response_keep_alive(&res), "an HTTP/1.1 response leaves the connection open");

    /* The body's framing depends on the request it answers: a response to HEAD has none. */
    proven_http_framing_t framing;
    EXAMPLE_REQUIRE(proven_http_response_framing(&res, PROVEN_HTTP_HEAD, &framing) == PROVEN_OK && framing.kind == PROVEN_HTTP_BODY_NONE,
                    "had this answered HEAD, no body would follow");
    EXAMPLE_REQUIRE(proven_http_response_framing(&res, PROVEN_HTTP_GET, &framing) == PROVEN_OK && framing.kind == PROVEN_HTTP_BODY_CHUNKED,
                    "it answers GET, and the body is chunked");

    proven_http_body_t body;
    proven_byte_t text[64];
    proven_size_t text_len = 0;
    proven_size_t pos = head_size;
    bool done = false;
    EXAMPLE_REQUIRE(proven_http_body_init(&body, framing, sizeof text) == PROVEN_OK, "accept at most what the buffer holds");
    while (!done && pos < len) {
        proven_size_t used = 0;
        proven_mem_view_t payload;
        if (proven_http_body_feed(&body, (proven_mem_view_t){ msg + pos, len - pos }, &used, &payload, &done) != PROVEN_OK) break;
        for (proven_size_t i = 0; i < payload.size; ++i) text[text_len + i] = payload.ptr[i];
        text_len += payload.size;
        pos += used;
    }
    EXAMPLE_REQUIRE(done && pos == len && proven_u8str_view_eq((proven_u8str_view_t){ text, text_len }, PROVEN_LIT("written in two chunks")),
                    "the body, decoded");

    /* The date comes back as the time it was made from, to the second. */
    proven_u8str_view_t date_text;
    proven_time_t parsed = 0;
    EXAMPLE_REQUIRE(proven_http_header_find(fields, res.header_count, PROVEN_LIT("Date"), &date_text) &&
                    proven_http_date_parse(date_text, now, &parsed) == PROVEN_OK && parsed == now - now % 1000000000,
                    "the Date header parses back to the same second");

    /* A request is written the same way. */
    len = 0;
    err = proven_http_write_request_line(out, &len, PROVEN_LIT("GET"), PROVEN_LIT("/search?q=a%20b"));
    if (err == PROVEN_OK) err = proven_http_write_header(out, &len, PROVEN_LIT("Host"), PROVEN_LIT("example.com"));
    if (err == PROVEN_OK) err = proven_http_write_head_end(out, &len);
    EXAMPLE_REQUIRE(err == PROVEN_OK && proven_u8str_view_eq((proven_u8str_view_t){ msg, len },
                    PROVEN_LIT("GET /search?q=a%20b HTTP/1.1\r\nHost: example.com\r\n\r\n")), "a request head, byte for byte");

    return EXAMPLE_OK();
}
```

## 8. Dates

HTTP writes one date format and must read three.

| API | Intent | Return |
|---|---|---|
| `proven_http_date_format(wall_ns, out[29])` | `Sun, 06 Nov 1994 08:49:37 GMT`. Always 29 bytes (`PROVEN_HTTP_DATE_SIZE`), always GMT, no NUL. | `proven_err_t`: `INVALID_ARG` before 1970. |
| `proven_http_date_parse(text, now_ns, &wall_ns)` | Read that form, the obsolete `Sunday, 06-Nov-94 08:49:37 GMT`, and asctime's `Sun Nov  6 08:49:37 1994`. | `proven_err_t`: `INVALID_FORMAT`; `OVERFLOW`. |

Pass the **wall clock** (`proven_time_now`), never the monotonic one, which has no date
([Chapter 5 section 4](manual-05-hosted-services.md)).

The parser checks that the date exists - 30 February is refused, and so is a weekday that does
not match its date. `now_ns` is used for one rule only: a two-digit year, which the obsolete
form has, is read as the most recent year with those digits that is not more than 50 years in
the future.

`PROVEN_ERR_OVERFLOW` is a valid date that `proven_time_t` cannot hold: its nanoseconds end in
April 2262. Servers send such dates on purpose - `Expires` in the year 9999 means "never" - so
treat that answer as "far in the future", not as a malformed header.

## 9. What is not here

- **A client or a server.** The drivers that read, write, time out and keep connections alive
  are built on this codec and Chapter 9, and will have their own chapter.
- **HTTP/2 and HTTP/3.** A head that says `HTTP/2.0` is `PROVEN_ERR_UNSUPPORTED`.
- **Content codings.** `gzip` and the rest: the library has no DEFLATE yet.
- **Trailers.** After a chunked body they are checked and skipped, not returned.
- **Cookies, authentication, multipart bodies, ranges.** Headers are text to this layer.
- **Relative URLs** and resolving one against a base.
