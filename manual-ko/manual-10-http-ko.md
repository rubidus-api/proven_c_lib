# 10장: URL과 HTTP 메시지

**5부 — 운영체제와 대화하기. 선행 조건: 2부
([1](manual-01-foundation-ko.md), [2](manual-02-allocation-ko.md), [3](manual-03-strings-text-ko.md)).
바이트가 오는 곳은 [9장](manual-09-networking-ko.md)이지만, 여기 있는 것은 9장 없이도 쓸 수 있다.**
**이 장을 마치면** URL을 나누고, 요청 경로를 열어도 안전한 경로로 바꾸고, HTTP/1.1 요청이나 응답을
도착하는 대로 파싱하고, 본문이 얼마나 긴지 알아내고, 그 본문을 디코딩하고, 남의 입력이 망가뜨릴 수
없는 메시지를 쓸 수 있다. 그리고 메시지 둘레에 놓이는 조각들 - 리다이렉트 풀기, 폼, 바이트 범위,
파일 올리기, 인증 응답, 쿠키 보관함, 이벤트 스트림 - 을 갖게 된다.

이 장은 `url.h`, `http.h`, `http_auth.h`, `http_cookie.h`, `sse.h`를 다룬다. 모두 소켓도 파일도 없는
텍스트 처리다. 할당하는 것은 쿠키 보관함뿐이고, 그것도 여러분이 준 할당자(allocator)에서 한다. 9장과
달리 이 헤더들은 [프리스탠딩(freestanding)](manual-freestanding-ko.md) 빌드에서 쓸 수 있고,
`PROVEN_NO_NET`으로 빠지지 않는다. 이것들을 소켓 위에서 일하게 하는 클라이언트와 서버는
[11장](manual-11-http-client-server-ko.md)에 있다.

## 목차

1. [서버가 아니라 코덱](#1-서버가-아니라-코덱)
2. [URL](#2-url)
3. [써도 안전한 경로](#3-써도-안전한-경로)
4. [헤드 파싱](#4-헤드-파싱)
5. [본문은 얼마나 긴가](#5-본문은-얼마나-긴가)
6. [본문 디코딩](#6-본문-디코딩)
7. [메시지 쓰기](#7-메시지-쓰기)
8. [날짜](#8-날짜)
9. [참조와 폼](#9-참조와-폼)
10. [범위와 multipart 본문](#10-범위와-multipart-본문)
11. [인증](#11-인증)
12. [쿠키](#12-쿠키)
13. [서버 전송 이벤트](#13-서버-전송-이벤트)
14. [여기에 없는 것](#14-여기에-없는-것)

## 1. 서버가 아니라 코덱

`http.h`는 어디에서도 읽지 않고 어디에도 쓰지 않는다. 이미 가진 바이트를 건네면 그것이 무슨 말인지
알려 주고, 메모리를 건네면 거기에 메시지를 쓴다. 그 바이트를 무엇이 실어 나르는지 - 블로킹 소켓,
이벤트 루프, TLS 세션, 테스트 - 는 이 헤더의 일이 아니고, 그것이 요점이다. HTTP에서 제대로 하기
어려운 부분은 그 모두에서 같고, 여기서 한 번만 쓰였다.

두 가지 성질이 따라 나오고, 이 장의 나머지가 둘에 기댄다.

**접두사는 결코 오류가 아니다.** 바이트는 네트워크가 나눈 조각대로 도착한다. 지금까지 온 것이 올바른
메시지의 앞부분이라면 파서는 `PROVEN_ERR_NEED_MORE`를 답한다. 실패도 아니고 섣부른 성공도 아니다.
그래서 그 둘레의 루프는 언제나 같다. 파싱하고, 더 필요하다면 더 읽고, 아니면 행동한다.

**모호함은 해소하지 않고 거절한다.** HTTP 메시지는 흔히 두 프로그램이 읽는다 - 앞단의 프록시와 뒷단의
서버. 둘이 같은 바이트를 다르게 읽는 곳마다, 하나에게는 다른 하나가 보지 못하는 요청을 보여 줄 수
있다. 그것이 *요청 밀반입(request smuggling)*이고, 너그러운 파서가 조용히 해석 하나를 고르는 자리에만
산다. 이 파서는 고르지 않는다.

| 거절하는 것 | 왜 중요한가 |
|---|---|
| LF만, 또는 CR만 있는 줄 끝 | 한 파서는 한 줄로, 다른 파서는 두 줄로 본다 |
| 헤더 이름과 콜론 사이의 공백 | `Content-Length : 5`는 한 파서에게는 헤더이고 다른 파서에게는 아니다 |
| 공백이나 탭으로 시작하는 헤더 줄(폐기된 줄 접기) | 한 파서에게는 이어지는 줄, 다른 파서에게는 새 필드 |
| 둘 이상의 `Content-Length`, 또는 순수한 숫자가 아닌 값 | 길이 둘은 본문이 끝나는 곳에 대한 의견 둘이다 |
| `Transfer-Encoding`과 `Content-Length`가 함께 | 고전적인 경우: 파서마다 다른 쪽을 믿는다 |
| 단일 `chunked`가 아닌 모든 전송 코딩 | 모르는 코딩을 건너뛰는 파서는 본문을 다르게 자른다 |
| 공백, 부호, `0x`가 붙거나 16자리를 넘는 청크 크기 | 청크 크기 파싱이 또 하나의 고전이다 |

각 행은 배포된 소프트웨어가 받아 주고, 공개된 공격이 쓰는 것이다. 거절은 평범한 오류 값이다. 400으로
답하고 닫는다.

## 2. URL

`proven_url_parse`는 절대 URL을 여러분이 준 텍스트를 가리키는 뷰(view)들로 나눈다.

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

**파싱은 디코딩하지 않는다.** 모든 구성 요소는 퍼센트 기호까지 적힌 그대로 돌아온다. 디코딩은 지금
쓰려는 조각 하나에 대해 따로 하는 걸음이다. 통째로 디코딩한 URL은 다시 나눌 수 없기 때문이다. 경로
세그먼트 안의 데이터였던 `%2F`가 세그먼트를 나누는 `/`가 되고, 쿼리 값 안의 `%26`이 그 값을 끝내는
`&`가 된다.

### 참조

| API | 의도 | 반환 |
|---|---|---|
| `proven_url_parse(text, &url)` | `scheme://authority/path?query#fragment`를 나눈다. ASCII만, 공백 없이, 모든 `%` 뒤에 hex 두 자리. | `proven_err_t`: 그 밖의 것은 `INVALID_FORMAT`. 그때 `url`은 그대로다. |
| `proven_url_default_port(scheme)` | `http`/`ws`는 80, `https`/`wss`는 443, `ftp`는 21. | `proven_u16`. 모르는 스킴은 0. |
| `proven_url_effective_port(&url)` | 연결할 포트: 적힌 것, 없으면 스킴의 것. | `proven_u16`. |
| `proven_url_scheme_is(&url, scheme)` | 스킴을 대소문자 구분 없이 비교한다. | `bool`. |
| `proven_url_split_target(target, &path, &query, &has_query)` | HTTP 요청 타깃을 나눈다: `/a?b`, `http://h/a?b`, 또는 `*`. | `proven_err_t`: 그 밖에는 `INVALID_FORMAT`. |
| `proven_url_query_iter(query)`, `proven_url_query_next(&it, &name, &value)` | `name=value&...`를 걸으며 인코딩된 뷰를 낸다. 빈 쌍은 건너뛴다. | 반복자. `bool`(끝에서 false). |
| `proven_url_percent_decode(in, out, &written)` | `%XX`를 바이트로. `out`이 `in`이어도 된다. `+`는 `+`로 남는다. | `proven_err_t`: 잘못된 이스케이프는 `INVALID_FORMAT`. `OUT_OF_BOUNDS`. |
| `proven_url_form_decode(in, out, &written)` | 같은 일을 하고, `+`를 공백으로 읽는다: 쿼리의 이름과 값에 쓴다. | `proven_err_t`. |
| `proven_url_encode_component(in, out, &written)` | 구성 요소 *하나*를 위해 인코딩한다: 글자, 숫자, `-._~` 외에는 모두 `%XX`가 된다. | `proven_err_t`: `OUT_OF_BOUNDS`. `OVERFLOW`. |
| `proven_url_encode_path(in, out, &written)` | 같은 일을 하되 `/`는 둔다. | `proven_err_t`. |
| `proven_url_form_encode(in, out, &written)` | 같은 일을 하되 공백을 `+`로 쓴다. | `proven_err_t`. |
| `proven_url_encoded_size_max(n)` | `n`바이트를 인코딩하기에 언제나 충분한 크기. | `proven_size_t`. 표현할 수 없으면 `SIZE_MAX`. |

이것은 RFC 3986이고, 브라우저가 구현하는 URL 알고리즘이 아니다. 브라우저는 받은 것을 고쳐 준다 -
역슬래시를 슬래시로, 공백을 인코딩하고, 없는 스킴을 추측한다. 여기서는 아무것도 고치지 않는다. 고쳐야
할 텍스트는 `PROVEN_ERR_INVALID_FORMAT`이다. 사람이 타이핑한 URL을 받는 프로그램은 추측하는 대신
그렇다고 알려 줘야 한다.

### 주의사항, 그리고 무엇이 잘못되는가

**구성 요소를 디코딩하라. URL 전체는 절대 아니다.**

잘못된 예:

```text
proven_url_percent_decode(whole_url, buf, &n);      /* wrong: "a%2Fb" and "a/b" are now the same path */
proven_url_parse(decoded, &url);
```

올바른 예 — 먼저 파싱하고, 필요한 구성 요소 하나를 그것에 맞는 디코더로 디코딩한다. 쿼리의 이름이나
값에는 `proven_url_form_decode`, 경로에는 `proven_url_path_resolve`.

**디코딩된 텍스트는 바이트다.** `proven_url_percent_decode`는 인코딩돼 있던 것을 그대로 돌려준다.
NUL, 줄바꿈, UTF-8이 아닌 바이트. 보낸 쪽의 데이터다. 그것으로 하려는 일에 맞게 검증하라.

**userinfo는 호스트가 아니다.** `http://example.com@evil.test/`의 호스트는 `evil.test`다. 파서는
이것을 옳게 처리한다 - 호스트는 *마지막* `@` 뒤에서 시작한다 - 하지만 URL을 사람에게 보여 주거나 허용
목록과 대조하는 프로그램은 텍스트의 앞부분이 아니라 `url.host`를 비교해야 한다.

테스트 스위트가 컴파일하고 실행한다:

<!-- example: manual/examples/ko/ex_10_url.c -->
```c
/*
 * URL: 나누고, 지금 쓰려는 조각만 디코딩하고, 네트워크에서 온 경로는 해소하기 전에는
 * 파일 시스템에 닿게 하지 않는다.
 */

static bool view_is(proven_u8str_view_t v, const char *text) {
    return proven_u8str_view_eq(v, proven_u8str_view_from_cstr(text));
}

int main(void) {
    /* 파싱은 적힌 그대로를 가리키는 뷰를 준다. 디코딩하는 것도 복사하는 것도 없다. */
    proven_url_t url;
    proven_err_t err = proven_url_parse(PROVEN_LIT("https://example.com:8443/docs/a%20b?q=caf%C3%A9+au+lait&page=2#top"), &url);
    EXAMPLE_REQUIRE(err == PROVEN_OK, "an absolute URL parses");
    EXAMPLE_REQUIRE(view_is(url.host, "example.com") && url.has_port && url.port == 8443, "host and port");
    EXAMPLE_REQUIRE(view_is(url.path, "/docs/a%20b"), "the path is still encoded");
    EXAMPLE_REQUIRE(url.has_query && url.has_fragment && view_is(url.fragment, "top"), "query and fragment are there");

    /* 스킴 이름은 대소문자 구분 없이 비교한다. 포트가 없는 URL은 스킴의 기본 포트를 얻는다. */
    proven_url_t plain;
    EXAMPLE_REQUIRE(proven_url_parse(PROVEN_LIT("HTTP://example.com/"), &plain) == PROVEN_OK, "another URL");
    EXAMPLE_REQUIRE(proven_url_scheme_is(&plain, PROVEN_LIT("http")), "HTTP is http");
    EXAMPLE_REQUIRE(proven_url_effective_port(&plain) == 80 && proven_url_effective_port(&url) == 8443,
                    "the port to connect to: the default, or the one written");
    EXAMPLE_REQUIRE(proven_url_default_port(PROVEN_LIT("wss")) == 443, "wss is 443");

    /* 절대 URL이 아닌 것은 고쳐 주지 않고 거절한다. */
    proven_url_t bad;
    EXAMPLE_REQUIRE(proven_url_parse(PROVEN_LIT("example.com/path"), &bad) == PROVEN_ERR_INVALID_FORMAT, "no scheme: refused");
    EXAMPLE_REQUIRE(proven_url_parse(PROVEN_LIT("http://example.com/a b"), &bad) == PROVEN_ERR_INVALID_FORMAT, "a raw space: refused");

    /* 쿼리는 쌍 단위로 걷는다. 이름과 값을 각각 따로, '+'가 공백을 뜻하는 폼 데이터로
     * 디코딩한다. 쿼리 전체를 먼저 디코딩하면 값 안의 인코딩된 '&'가 구분자로
     * 바뀌어 버린다. */
    proven_url_query_iter_t it = proven_url_query_iter(url.query);
    proven_u8str_view_t name, value;
    proven_byte_t text[64];
    proven_size_t n = 0;
    EXAMPLE_REQUIRE(proven_url_query_next(&it, &name, &value) && view_is(name, "q"), "the first pair is q");
    EXAMPLE_REQUIRE(proven_url_form_decode(value, (proven_mem_mut_t){ text, sizeof text }, &n) == PROVEN_OK &&
                    view_is((proven_u8str_view_t){ text, n }, "caf\xc3\xa9 au lait"), "its value, decoded");
    EXAMPLE_REQUIRE(proven_url_query_next(&it, &name, &value) && view_is(name, "page") && view_is(value, "2"), "the second pair");
    EXAMPLE_REQUIRE(!proven_url_query_next(&it, &name, &value), "and no third");

    /* 일반 퍼센트 디코딩은 '+'를 그대로 두고, *바이트*를 낸다 - 어떤 바이트든. */
    EXAMPLE_REQUIRE(proven_url_percent_decode(PROVEN_LIT("a%2Fb+c"), (proven_mem_mut_t){ text, sizeof text }, &n) == PROVEN_OK &&
                    view_is((proven_u8str_view_t){ text, n }, "a/b+c"), "%2F becomes a slash; '+' stays");

    /* 반대 방향: 값이 자기가 들어갈 구성 요소를 끝낼 수 없도록 인코딩한다. */
    proven_mem_view_t file_name = proven_mem_view_from_u8(PROVEN_LIT("report 100%/final?.txt"));
    proven_byte_t enc[96];
    EXAMPLE_REQUIRE(proven_url_encoded_size_max(file_name.size) <= sizeof enc, "three bytes per byte is always enough");
    EXAMPLE_REQUIRE(proven_url_encode_component(file_name, (proven_mem_mut_t){ enc, sizeof enc }, &n) == PROVEN_OK &&
                    view_is((proven_u8str_view_t){ enc, n }, "report%20100%25%2Ffinal%3F.txt"), "one path segment: even the slash is encoded");
    EXAMPLE_REQUIRE(proven_url_encode_path(proven_mem_view_from_u8(PROVEN_LIT("/my docs/a b.txt")), (proven_mem_mut_t){ enc, sizeof enc }, &n) == PROVEN_OK &&
                    view_is((proven_u8str_view_t){ enc, n }, "/my%20docs/a%20b.txt"), "a whole path: the slashes stay");
    EXAMPLE_REQUIRE(proven_url_form_encode(proven_mem_view_from_u8(PROVEN_LIT("a b&c")), (proven_mem_mut_t){ enc, sizeof enc }, &n) == PROVEN_OK &&
                    view_is((proven_u8str_view_t){ enc, n }, "a+b%26c"), "form data: a space is '+'");

    /* 서버 쪽에서 보면. 요청 타깃을 먼저 나누고... */
    proven_u8str_view_t path, query;
    bool has_query = false;
    EXAMPLE_REQUIRE(proven_url_split_target(PROVEN_LIT("/static/css/../img/logo%20v2.png?v=3"), &path, &query, &has_query) == PROVEN_OK &&
                    has_query && view_is(query, "v=3"), "target into path and query");

    /* ...경로는 다른 무엇이 보기 전에 해소한다: 한 번 디코딩하고, 점 세그먼트를
     * 없애고, 루트를 벗어나려 하면 거절한다. */
    proven_byte_t clean[128];
    EXAMPLE_REQUIRE(proven_url_path_resolve(path, (proven_mem_mut_t){ clean, sizeof clean }, &n) == PROVEN_OK &&
                    view_is((proven_u8str_view_t){ clean, n }, "/static/img/logo v2.png"), "a clean path under the root");

    /* 파일 서버가 거절하려고 존재하는 요청들. */
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

## 3. 써도 안전한 경로

요청 경로는 파일을 가리키고, 그것을 고르는 쪽은 보낸 쪽이다. `GET /../../etc/passwd`는 파일 서버에
대한 가장 오래된 공격이고 표기가 여럿이다. `proven_url_path_resolve`는 요청 경로와 파일을 여는 모든 것
사이에 둘 단 하나의 함수다.

| API | 의도 | 반환 |
|---|---|---|
| `proven_url_path_resolve(raw_path, out, &written)` | 한 번 디코딩하고, 파일 이름에 있어서는 안 될 것을 거절하고, `.`과 `..`을 해소한다. 결과는 `/`로 시작하고, 점 세그먼트와 빈 세그먼트가 없으며, 끝의 `/`는 유지된다. `out`은 많아야 `raw_path.size`바이트면 된다. | `proven_err_t`: `..`이 루트 위로 올라가려 하면 `PERMISSION`. 아래의 거절들은 `INVALID_FORMAT`. `out`이 작으면 `OUT_OF_BOUNDS`. |

하는 일을 순서대로 적는다. 그 순서가 곧 보안 성질이다.

1. **한 번 디코딩한다.** `%2e%2e`는 `..`이 되고, 그다음 `..`으로 취급된다. 그리고 `%252e%252e`는
   `%2e%2e`라는 텍스트가 되며, 이상한 파일 이름일 뿐 그 이상이 아니다 - 한 번 더 디코딩하면 경로
   이탈이 되는데, 그것이 *이중 디코딩* 버그다.
2. **거절한다.** 인코딩된 슬래시(`%2F` - 나중에 세그먼트 하나를 둘로 쪼갠다), 적혔든 인코딩됐든
   역슬래시(Windows에서는 구분자다), NUL을 포함한 모든 제어 문자(`file%00.png`는 C에서 이름을 자른다),
   그리고 디코딩한 뒤 올바른 UTF-8이 아닌 것 - 일부 디코더가 받아 주는 점의 *과장(overlong)* 인코딩
   `%c0%ae`가 여기서 걸린다.
3. **해소한다.** 디코딩된 텍스트에서 `.`과 `..`을. 더 지울 것이 없는데 나온 `..`은
   `PROVEN_ERR_PERMISSION`이다. 경로는 형식에 맞고, 가져서는 안 될 것을 요구한다.

잘못된 예 — 디코딩 전에 `..`을 검사하기:

```text
if (contains(raw_path, "..")) return 400;         /* wrong: "%2e%2e" sails through */
proven_url_percent_decode(raw_path, buf, &n);
open_under_root(buf, n);
```

잘못된 예 — "안전하게" 두 번 디코딩하기:

```text
proven_url_percent_decode(raw, a, &n);
proven_url_percent_decode(a_view, b, &m);          /* wrong: "%252e%252e" has just become ".." */
```

올바른 예:

```text
proven_err_t err = proven_url_path_resolve(raw_path, clean_mem, &n);
if (err == PROVEN_ERR_PERMISSION)     return 403;   /* or 400 */
if (err != PROVEN_OK)                 return 400;
open_under_root(clean, n);
```

**하지 않는 일.** 경로 *문법*으로 일어나는 이탈을 막는다. 여러분의 파일 시스템은 모른다. 루트 안의
심볼릭 링크는 여전히 밖을 가리킬 수 있고, Windows는 일부 파일 이름에 고유한 뜻을 준다(`CON`, 끝의 점,
`name:stream`). 그것들은 파일을 여는 자리에서 검사하라. 위 예제는 해소 함수를 보여 주고, 파일을 여는
절반은 [5장](manual-05-hosted-services-ko.md)의 것이다.

## 4. 헤드 파싱

*헤드*는 본문 앞의 모든 것이다. 요청 줄이나 상태 줄, 헤더 필드들, 그리고 그것을 끝내는 빈 줄.

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

모든 뷰는 여러분이 파싱한 버퍼 안을 가리킨다. 복사되는 것이 없으므로, 버퍼는 파싱된 헤드보다 오래
살아야 하고 움직여서는 안 된다. 필드를 담을 배열도 여러분의 것이다.

### 참조

| API | 의도 | 반환 |
|---|---|---|
| `proven_http_parse_request(data, headers, cap, max_head, &req, &head_size)` | `data` 앞쪽에서 요청 헤드를 파싱한다. `head_size`는 본문이 시작하는 자리다. `max_head`가 0이면 `PROVEN_HTTP_DEFAULT_MAX_HEAD`(16 KiB). | `proven_err_t`: `NEED_MORE`(더 읽어라). `OUT_OF_BOUNDS`(헤드가 한도를 넘거나 필드가 `cap`보다 많음: 431). `INVALID_FORMAT`(400). `UNSUPPORTED`(HTTP/1.0도 1.1도 아님: 505). |
| `proven_http_parse_response(data, headers, cap, max_head, &res, &head_size)` | 응답에 대해 같은 일을 한다. | 위와 같다. |
| `proven_http_header_find(headers, count, name, &value)` | 그 이름의 첫 필드. 대소문자 구분 없음. `value`는 NULL이어도 된다. | `bool`. |
| `proven_http_header_count(headers, count, name)` | 그 이름의 필드가 몇 개인가. | `proven_size_t`. |
| `proven_http_header_has_token(headers, count, name, token)` | 그런 필드 중 쉼표로 나뉜 값에 `token`이 든 것이 있는가: `Connection: keep-alive, Upgrade`에는 `upgrade`가 있다. | `bool`. |
| `proven_http_method_from_text(text)`, `proven_http_method_text(method)` | 메서드 토큰과 enum 사이를 오간다. 대소문자를 구분한다: `get`은 `GET`이 아니다. | enum. 뷰(`OTHER`면 비어 있음). |
| `proven_http_reason_phrase(status)` | 표준 문구: `Not Found`. | 뷰. 문구가 없는 코드는 `Unknown`. |
| `proven_http_request_keep_alive(&req)`, `proven_http_response_keep_alive(&res)` | 연결이 다음 메시지를 실을 수 있는가: HTTP/1.1은 `Connection: close`가 없는 한, HTTP/1.0은 `Connection: keep-alive`가 있을 때만. | `bool`. |

**한도는 헤드가 아직 미완성일 때 검사한다.** `max_head`바이트가 도착했는데 그 안에 헤드의 끝이 없으면
답은 `NEED_MORE`가 아니라 즉시 `PROVEN_ERR_OUT_OF_BOUNDS`다. 헤더 줄을 끝없이 보내는 클라이언트는
`max_head`바이트만큼만 비용을 치르게 한다.

요청 줄 앞의 빈 줄은 건너뛰고(클라이언트는 오래전부터 본문 뒤에 남는 CRLF를 보내 왔다) `head_size`에
센다.

### 주의사항, 그리고 무엇이 잘못되는가

**`NEED_MORE`는 실패가 아니다.**

잘못된 예:

```text
if (proven_http_parse_request(data, h, 32, 0, &req, &n) != PROVEN_OK) return send_400();   /* wrong: a slow client gets a 400 */
```

올바른 예 — 결과 셋에 대응 셋. `NEED_MORE`면 더 읽는다([9장](manual-09-networking-ko.md)의 기한과
함께). `OUT_OF_BOUNDS`, `INVALID_FORMAT`, `UNSUPPORTED`면 431, 400, 505로 답하고 닫는다. `PROVEN_OK`면
계속한다.

**필드는 반복될 수 있다.** `proven_http_header_find`는 첫 번째를 돌려준다. 반복이 뜻을 바꾸는 필드라면
`proven_http_header_count`에 물어라 - 이 라이브러리가 두 번째 `Content-Length`를 거절하는 방법이 바로
그것이다.

**뷰는 버퍼와 함께 죽는다.** 다음 요청을 같은 버퍼에 읽어 들이면, 앞 헤드의 모든 뷰는 이제 다른 것을
가리킨다. 남겨 둘 것은 복사하라.

## 5. 본문은 얼마나 긴가

헤드 다음으로 가장 중요한 질문은 메시지가 어디서 끝나는가다. 다음 메시지가 거기서 시작하기 때문이다.
답은 헤드가 준다.

```text
typedef struct {
    proven_http_body_kind_t kind;   /* NONE, LENGTH, CHUNKED, or UNTIL_CLOSE (responses only) */
    proven_u64 length;              /* for LENGTH */
} proven_http_framing_t;
```

| API | 의도 | 반환 |
|---|---|---|
| `proven_http_request_framing(&req, &framing)` | 청크, 길이, 또는 본문 없음. | `proven_err_t`: 1절의 모호한 경우는 `INVALID_FORMAT`(400). `chunked`가 아닌 전송 코딩은 `UNSUPPORTED`(501). |
| `proven_http_response_framing(&res, request_method, &framing)` | 같은 일을 하되 무엇을 물었는지 안다: `HEAD`에 대한 응답, 1xx, 204, 304, 성공한 `CONNECT`에는 본문이 없다. 두 헤더가 다 없으면 본문은 상대가 닫을 때까지다. | 위와 같다. |

응답에 요청의 메서드가 필요한 이유는 같은 헤더가 다른 뜻을 갖기 때문이다. `HEAD`에 대한 응답은 `GET`이었다면
가졌을 `Content-Length`를 싣고, 본문은 아예 없다.

## 6. 본문 디코딩

```text
proven_http_body_t body;                       /* caller-owned; nothing to destroy */
proven_http_body_init(&body, framing, max_body_bytes);
proven_http_body_feed(&body, in, &consumed, &payload, &done);
```

`proven_http_body_t`는 호출자 소유(owned) 상태다. 할당하는 것이 없고 destroy할 것도 없다.

| API | 의도 | 반환 |
|---|---|---|
| `proven_http_body_init(&body, framing, max_body)` | 시작한다. `max_body`는 받아들일 양이다 - 기본값이 없다. | `proven_err_t`: `Content-Length`가 이미 그것을 넘으면 `OUT_OF_BOUNDS`(413). |
| `proven_http_body_feed(&body, in, &consumed, &payload, &done)` | `in`의 앞부분을 소비한다. `payload`는 그 안의 본문 바이트이고, `in` *안을* 가리키는 뷰다. | `proven_err_t`: `INVALID_FORMAT`(잘못된 청크 프레이밍). `OUT_OF_BOUNDS`(한도 초과). 오류 뒤에는 `INVALID_STATE`. |
| `proven_http_body_end(&body)` | 상대가 닫았다: 본문은 완전했는가? | `PROVEN_OK`, 또는 메시지가 잘렸으면 `PROVEN_ERR_NEED_MORE`. |
| `proven_http_body_received(&body)` | 지금까지 전달된 본문 바이트. | `proven_u64`. |

`feed`는 아무것도 복사하지 않는다. 청크 본문이라면 청크 프레이밍을 건너뛰고 그 사이의 데이터를, 호출마다
연속된 한 조각씩 돌려준다. 그래서 루프는 이렇다. 가진 것을 먹이고, `payload`를 쓰고, `consumed`만큼
전진하고, 전부 소비했는데 `done`이 아직 false면 더 읽는다. `done`이 true가 되면, `in`에서 `consumed`
뒤의 바이트는 **다음** 메시지의 것이다.

**한도를 밝혀라.** `max_body`는 "낯선 이가 나에게 얼마나 들고 있게 할 수 있는가"에 대한 답이다. 그것을
넘는 `Content-Length`는 본문을 한 바이트도 읽기 전에 거절되고, 크기가 그것을 넘길 청크는 데이터가
도착한 뒤가 아니라 크기를 읽을 때 거절된다.

**닫힘이 언제나 끝은 아니다.** 닫을 때까지 이어지는 본문은 상대가 닫을 때 제대로 끝난다. 그 밖의 본문이
닫힘으로 중단되면 *잘린* 것이다 - `proven_http_body_end`가 `PROVEN_ERR_NEED_MORE`를 돌려준다 - 그리고
온전한 것으로 취급해서는 안 된다. 온전한 파일처럼 보이는 반쪽 파일은 다운로드의 최악의 결과다.

**오류 뒤에는 연결이 끝난다.** 프레이밍이 틀리면 다음 메시지가 어디서 시작하는지 알 수 없다. 할 수
있으면 답하고, 닫는다.

테스트 스위트가 컴파일하고 실행한다 - 어색하게 잘린 네 조각으로 도착하는 요청:

<!-- example: manual/examples/ko/ex_10_http_request.c -->
```c
/*
 * 서버가 보는 요청 하나: 바이트는 조각으로 도착하고, 헤드는 다 모였을 때 파싱하고,
 * 프레이밍이 본문 길이를 알려 주고, 본문은 오는 대로 디코딩한다.
 *
 * 여기에 소켓은 나오지 않는다. 코덱은 가진 바이트를 받을 뿐이고 그것이 어디서 왔는지는
 * 여러분의 일이다. 같은 코드가 블로킹 read 아래에서도, 이벤트 루프에서도, 이런 테스트에서도
 * 도는 이유다.
 */

/* 네트워크가 전해 줄 법한 모양의 요청: 어색한 자리에서 잘려 있다. */
static const char *const pieces[] = {
    "POST /upload/notes.txt?overwrite=1 HTT",
    "P/1.1\r\nHost: example.com\r\nTransfer-Encoding: chun",
    "ked\r\nConnection: keep-alive, TE\r\nX-Tag: a\r\nX-Tag: b\r\n\r\n5\r\nhel",
    "lo\r\n7;note=x\r\n, world\r\n0\r\n\r\nGET /next HTTP/1.1\r\n\r\n",
};

int main(void) {
    proven_byte_t buf[512];                 /* 읽기 버퍼: 헤드가 여기에 들어가야 한다 */
    proven_size_t have = 0;
    proven_size_t next_piece = 0;

    proven_http_header_t fields[16];
    proven_http_request_t req;
    proven_size_t head_size = 0;

    /* 헤드가 완성될 때까지 읽는다. NEED_MORE는 오류가 아니다. 말 그대로의 뜻이다. */
    for (;;) {
        proven_err_t err = proven_http_parse_request((proven_mem_view_t){ buf, have }, fields, 16, sizeof buf, &req, &head_size);
        if (err == PROVEN_OK) break;
        EXAMPLE_REQUIRE(err == PROVEN_ERR_NEED_MORE, "an incomplete head asks for more; anything else is a 400, 431 or 505");
        if (err != PROVEN_ERR_NEED_MORE) return EXAMPLE_OK();
        /* "read": 다음 조각을 덧붙인다. */
        proven_size_t len = proven_cstr_len(pieces[next_piece]);
        for (proven_size_t i = 0; i < len; ++i) buf[have + i] = (proven_byte_t)pieces[next_piece][i];
        have += len;
        next_piece++;
    }

    /* 헤드. `buf` 안을 가리키는 뷰들이다. */
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

    /* 요청이 가리키는 곳. 나누고, 해소한다 - 경로를 어디에든 쓰기 전에. */
    proven_u8str_view_t path, query;
    bool has_query = false;
    proven_byte_t clean[128];
    proven_size_t clean_len = 0;
    EXAMPLE_REQUIRE(proven_url_split_target(req.target, &path, &query, &has_query) == PROVEN_OK &&
                    proven_url_path_resolve(path, (proven_mem_mut_t){ clean, sizeof clean }, &clean_len) == PROVEN_OK &&
                    proven_u8str_view_eq((proven_u8str_view_t){ clean, clean_len }, PROVEN_LIT("/upload/notes.txt")),
                    "the target's path, resolved");

    /* 본문은 얼마나 긴가? 헤드가 정하고, 모호한 헤드는 추측이 아니라 오류다. */
    proven_http_framing_t framing;
    EXAMPLE_REQUIRE(proven_http_request_framing(&req, &framing) == PROVEN_OK && framing.kind == PROVEN_HTTP_BODY_CHUNKED,
                    "this body is chunked");

    /* 디코딩한다. 한도는 상대가 얼마나 보내도 되는지에 대한 호출자의 선언이다. */
    proven_http_body_t body;
    EXAMPLE_REQUIRE(proven_http_body_init(&body, framing, 1024) == PROVEN_OK, "at most 1 KiB of body");

    proven_byte_t content[64];
    proven_size_t content_len = 0;
    proven_size_t pos = head_size;          /* 본문은 헤드가 끝난 자리에서 시작한다 */
    bool done = false;
    while (!done) {
        if (pos == have) {
            /* 지금까지 읽은 것을 다 썼다: 더 "read"한다. 실제 서버라면 상대가 닫았을 때
             * 여기서 proven_http_body_end를 부르고 멈춘다. */
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
        /* `payload`는 `buf` 안을 가리키는 뷰다: 청크 프레이밍을 건너뛴 본문 바이트. */
        for (proven_size_t i = 0; i < payload.size; ++i) content[content_len + i] = payload.ptr[i];
        content_len += payload.size;
        pos += used;
    }
    EXAMPLE_REQUIRE(proven_u8str_view_eq((proven_u8str_view_t){ content, content_len }, PROVEN_LIT("hello, world")),
                    "two chunks, joined");
    EXAMPLE_REQUIRE(proven_http_body_received(&body) == 12 && proven_http_body_end(&body) == PROVEN_OK, "twelve bytes, complete");

    /* 버퍼에 남은 것은 다음 요청이고, 손대지 않았다. */
    proven_http_request_t next;
    EXAMPLE_REQUIRE(proven_http_parse_request((proven_mem_view_t){ buf + pos, have - pos }, fields, 16, 0, &next, &head_size) == PROVEN_OK &&
                    proven_u8str_view_eq(next.target, PROVEN_LIT("/next")), "the decoder stopped exactly where the next request begins");

    return EXAMPLE_OK();
}
```

## 7. 메시지 쓰기

쓰기 함수들은 여러분이 준 메모리에 덧붙인다. 각각 버퍼와 길이를 받아, 그 길이의 자리에 쓰고, 길이를
전진시킨다. 실패하면 길이는 움직이지 않고 그 앞의 것은 건드리지 않는다.

| API | 덧붙이는 것 |
|---|---|
| `proven_http_write_request_line(out, &len, method, target)` | `GET /path HTTP/1.1` CRLF |
| `proven_http_write_status_line(out, &len, status, reason)` | `HTTP/1.1 404 Not Found` CRLF. `reason`이 비어 있으면 표준 문구를 보낸다 |
| `proven_http_write_header(out, &len, name, value)` | `Name: value` CRLF |
| `proven_http_write_header_u64(out, &len, name, number)` | `Content-Length: 1234` CRLF |
| `proven_http_write_head_end(out, &len)` | 헤드를 끝내는 빈 줄 |
| `proven_http_write_chunk_begin(out, &len, size)` | `size`바이트(0이 아님) 청크를 시작하는 줄 |
| `proven_http_write_chunk_end(out, &len)` | 청크 데이터 뒤의 CRLF |
| `proven_http_write_last_chunk(out, &len)` | 청크 본문을 끝내는 0 청크와 빈 트레일러 |

모두 `proven_err_t`를 돌려준다. 들어가지 않으면 `PROVEN_ERR_OUT_OF_BOUNDS`, 쓰라고 한 것이 그 이름의
물건이 되지 못하면 `PROVEN_ERR_INVALID_ARG`다.

**중요한 것은 두 번째 오류다.** 헤더 값은 거의 언제나 다른 곳에서 온다 - 파일 이름, 리다이렉트 대상,
클라이언트가 보낸 무언가. 거기에 CR과 LF가 들어 있으면, 그대로 쓰는 순간 필드가 끝나고 *보낸 쪽*이
고른 새 필드가 시작된다.

```text
Location: /home
Set-Cookie: session=attacker        <- this line came from inside the "value"
```

그것이 *응답 분할(response splitting)*이고, 요청 밀반입의 쓰기 쪽 쌍둥이다. `proven_http_write_header`는
CR, LF, NUL, 그 밖의 제어 문자가 든 값과 토큰이 아닌 이름을 거절하고, `proven_http_write_request_line`은
공백이나 줄바꿈이 든 타깃을 거절한다. 이 검사는 선택 사항이 아니고 "raw" 변종도 없다.

잘못된 예 — 마지막에 한 번만 검사하기:

```text
proven_http_write_status_line(out, &len, 200, PROVEN_LIT(""));
proven_http_write_header(out, &len, name, user_value);      /* refused: len did not move */
err = proven_http_write_head_end(out, &len);                /* OK - and the header is silently missing */
```

올바른 예 — 호출마다 검사하거나, 예제처럼 첫 실패가 나머지를 멈추도록 잇는다. 쓰기 함수들은
`[[nodiscard]]`라서 첫 번째 형태에는 컴파일러가 이의를 제기한다.

길이를 아는 본문은 `Content-Length`를 쓰고 바이트를 쓴다. 바이트는 여러분이 직접 쓴다. 길이를 모르는
본문은 `Transfer-Encoding: chunked`를 쓰고, 조각마다 `chunk_begin`, 데이터, `chunk_end`를 쓰고, 끝에
`last_chunk`를 쓴다.

테스트 스위트가 컴파일하고 실행한다:

<!-- example: manual/examples/ko/ex_10_http_response.c -->
```c
/*
 * 응답을 쓰고, 그것을 다시 읽는다.
 *
 * 쓰기 함수는 `len` 자리에 덧붙이고 `len`을 옮긴다. 메시지를 깨뜨릴 수 있는 값 - 헤더 안의
 * 줄바꿈 - 은 쓰지 않고 거절한다. 그래서 요청에서 가져온 문자열로 보낸 쪽이 고른 헤더를
 * 끼워 넣을 수 없다.
 */

int main(void) {
    proven_byte_t msg[512];
    proven_mem_mut_t out = { msg, sizeof msg };
    proven_size_t len = 0;

    /* Date 헤더: 벽시계를, HTTP가 쓰는 단 하나의 형식으로. */
    proven_byte_t date[PROVEN_HTTP_DATE_SIZE];
    proven_time_t now = proven_time_now();
    EXAMPLE_REQUIRE(proven_http_date_format(now, date) == PROVEN_OK, "now, as an HTTP date");

    /* 길이를 미리 알 수 없는 응답은 청크로 나간다. */
    proven_err_t err = proven_http_write_status_line(out, &len, 200, PROVEN_LIT(""));     /* "" : 표준 문구 */
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
        for (proven_size_t k = 0; k < n; ++k) msg[len + k] = (proven_byte_t)parts[i][k];   /* 청크의 데이터 */
        len += n;
        err = proven_http_write_chunk_end(out, &len);
    }
    if (err == PROVEN_OK) err = proven_http_write_last_chunk(out, &len);
    EXAMPLE_REQUIRE(err == PROVEN_OK, "the body is written");

    /* 줄바꿈이 든 헤더 값은 아예 쓰이지 않는다. */
    proven_size_t before = len;
    EXAMPLE_REQUIRE(proven_http_write_header(out, &len, PROVEN_LIT("Location"), PROVEN_LIT("/home\r\nSet-Cookie: session=stolen")) == PROVEN_ERR_INVALID_ARG &&
                    len == before, "response splitting is refused, and nothing was appended");

    /* --- 클라이언트 쪽: 같은 바이트를 다시 읽는다 ----------------------------------- */

    proven_http_header_t fields[16];
    proven_http_response_t res;
    proven_size_t head_size = 0;
    EXAMPLE_REQUIRE(proven_http_parse_response((proven_mem_view_t){ msg, len }, fields, 16, 0, &res, &head_size) == PROVEN_OK &&
                    head_size == head_len, "the head parses, and ends where it was written to end");
    EXAMPLE_REQUIRE(res.status == 200 && proven_u8str_view_eq(res.reason, proven_http_reason_phrase(200)), "200 OK");
    EXAMPLE_REQUIRE(proven_http_response_keep_alive(&res), "an HTTP/1.1 response leaves the connection open");

    /* 본문의 프레이밍은 그것이 답하는 요청에 달려 있다: HEAD에 대한 응답에는 본문이 없다. */
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

    /* 날짜는 그것을 만든 시각으로, 초 단위까지 되돌아온다. */
    proven_u8str_view_t date_text;
    proven_time_t parsed = 0;
    EXAMPLE_REQUIRE(proven_http_header_find(fields, res.header_count, PROVEN_LIT("Date"), &date_text) &&
                    proven_http_date_parse(date_text, now, &parsed) == PROVEN_OK && parsed == now - now % 1000000000,
                    "the Date header parses back to the same second");

    /* 요청도 같은 방식으로 쓴다. */
    len = 0;
    err = proven_http_write_request_line(out, &len, PROVEN_LIT("GET"), PROVEN_LIT("/search?q=a%20b"));
    if (err == PROVEN_OK) err = proven_http_write_header(out, &len, PROVEN_LIT("Host"), PROVEN_LIT("example.com"));
    if (err == PROVEN_OK) err = proven_http_write_head_end(out, &len);
    EXAMPLE_REQUIRE(err == PROVEN_OK && proven_u8str_view_eq((proven_u8str_view_t){ msg, len },
                    PROVEN_LIT("GET /search?q=a%20b HTTP/1.1\r\nHost: example.com\r\n\r\n")), "a request head, byte for byte");

    return EXAMPLE_OK();
}
```

## 8. 날짜

HTTP는 날짜 형식 하나를 쓰고, 셋을 읽어야 한다.

| API | 의도 | 반환 |
|---|---|---|
| `proven_http_date_format(wall_ns, out[29])` | `Sun, 06 Nov 1994 08:49:37 GMT`. 언제나 29바이트(`PROVEN_HTTP_DATE_SIZE`), 언제나 GMT, NUL 없음. | `proven_err_t`: 1970년 이전은 `INVALID_ARG`. |
| `proven_http_date_parse(text, now_ns, &wall_ns)` | 그 형식과, 폐기된 `Sunday, 06-Nov-94 08:49:37 GMT`, asctime의 `Sun Nov  6 08:49:37 1994`를 읽는다. | `proven_err_t`: `INVALID_FORMAT`. `OVERFLOW`. |

**벽시계**(`proven_time_now`)를 넘겨라. 날짜가 없는 단조 시계는 안 된다
([5장 §4](manual-05-hosted-services-ko.md)).

파서는 그 날짜가 존재하는지 검사한다 - 2월 30일은 거절하고, 날짜와 맞지 않는 요일도 거절한다. `now_ns`는
규칙 하나에만 쓰인다. 폐기된 형식에 있는 두 자리 연도는, 그 두 자리를 가진 해 중 50년 넘게 미래가 아닌
가장 최근의 해로 읽는다.

`PROVEN_ERR_OVERFLOW`는 `proven_time_t`가 담을 수 없는 올바른 날짜다. 그 나노초는 2262년 4월에 끝난다.
서버는 그런 날짜를 일부러 보낸다 - 9999년의 `Expires`는 "영원히"라는 뜻이다 - 그러니 그 답은 잘못된
헤더가 아니라 "먼 미래"로 취급하라.

## 9. 참조와 폼

`Location` 헤더, 페이지 안의 링크, 폼의 `action`. 이것들은 모두 *참조*이고, 그것이 어디로 이어지는지는
그것이 발견된 URL에 달려 있다. `proven_url_resolve`가 그 단계이며, RFC 3986 5.2절이 정한 대로 한다.

| `http://example.com/docs/guide/intro.html?v=2`에서 발견된 참조 | 해소 결과 |
|---|---|
| `chapter2.html` | `http://example.com/docs/guide/chapter2.html` |
| `../img/logo.png` | `http://example.com/docs/img/logo.png` |
| `/login?next=%2F` | `http://example.com/login?next=%2F` |
| `?v=3` | `http://example.com/docs/guide/intro.html?v=3` |
| `//cdn.example.net/x` | `http://cdn.example.net/x` |
| `https://other.example/` | `https://other.example/` |

| API | 의도 | 반환 |
|---|---|---|
| `proven_url_resolve(base, reference, out, &written)` | `reference`를 절대 URL `base`에 대해 해소한다. 점 세그먼트는 제거된다. 그 자체로 절대 URL인 참조는 그대로 돌려준다. | `proven_err_t`: `base`가 절대 URL이 아니거나 결과에 호스트가 없게 되면 `INVALID_FORMAT`. `OUT_OF_BOUNDS`. |
| `proven_url_form_append(out, &len, name, value)` | 폼 본문의 `name=value` 한 쌍을 인코딩해서 덧붙인다. 첫 쌍이 아니면 앞에 `&`를 붙인다. | `proven_err_t`: `OUT_OF_BOUNDS`. 그때 `len`과 그 앞의 바이트는 그대로다. |

**해소된 URL은 남이 고른 목적지다.** 그 `Location`을 보낸 서버가 골랐다. 해소는 그것이 어디로
이어지는지 알려 줄 뿐이다. 거기로 갈지 말지 - 다른 호스트로, `https`에서 `http`로 내려가서, 여러분의
내부 네트워크 안의 주소로 - 는 해소 함수가 내리는 결정이 아니다. 앞의 둘은 11장의 클라이언트가
결정한다. 셋째는 여러분 몫이다.

`proven_url_form_append`는 `application/x-www-form-urlencoded`를 쓴다. 쿼리 문자열과 단순한 폼 본문의
형식이다. 공백은 `+`가 되고, 글자와 숫자와 `-._~`가 아닌 것은 모두 `%XX`가 된다. 이름과 값은
바이트이므로 주어진 그대로 인코딩된다.

## 10. 범위와 multipart 본문

범위 요청은 리소스의 일부를 요청한다. 내려받기를 이어 가거나 로그의 끝을 읽을 때 쓴다. 요청은 바이트를
지목하고, 답은 자기가 어느 바이트를 싣고 있는지 말하는 `206`이다.

| API | 의도 | 반환 |
|---|---|---|
| `proven_http_write_range(out, &len, first, last)` | `Range: bytes=first-last`를 덧붙인다. "끝까지"는 `last`에 `PROVEN_HTTP_RANGE_TO_END`를 준다. | `proven_err_t`: `last < first`이면 `INVALID_ARG`. |
| `proven_http_range_parse(value, size, &first, &last)` | 서버 쪽: `Range` 값을 `size`바이트짜리 리소스에 견주어 읽는다. `bytes=a-b`, `bytes=a-`, `bytes=-n`을 받고 리소스 크기에 맞춰 자른다. | `proven_err_t`: `OUT_OF_BOUNDS` - 416으로 답하라. 범위가 여럿이거나 다른 단위면 `UNSUPPORTED` - 리소스 전체를 보내라. `INVALID_FORMAT` - 헤더를 무시하라. |
| `proven_http_write_content_range(out, &len, first, last, total)` | 206을 위한 `Content-Range: bytes first-last/total`을 덧붙인다. | `proven_err_t`: `first <= last < total`이 아니면 `INVALID_ARG`. |
| `proven_http_content_range_parse(value, &first, &last, &total, &has_total)` | 클라이언트 쪽: 그것을 다시 읽는다. `/*`이면 `has_total`이 false다. | `proven_err_t`: `INVALID_FORMAT`. |

**`proven_http_range_parse`의 세 오류는 서로 다른 세 가지 답이며**, RFC 9110이 하나하나 이름을 붙였다.
만족시킬 수 없는 범위는 `Content-Range: bytes */size`를 단 `416`이다. 이 서버가 구현하지 않는 범위나
읽을 수 없는 헤더는 *무시*한다 - 응답은 리소스 전체를 실은 평범한 `200`이다. `Range` 헤더에 `400`으로
답하는 것은 틀렸다.

**이어받는 클라이언트는 무엇이 돌아왔는지 확인해야 한다.** 서버는 `Range`를 무시해도 된다. 5000번
바이트부터 요청했는데 답이 `200`이라면 본문은 0번 바이트에서 시작한다. 그것을 이미 가진 5000바이트
뒤에 덧붙이면 소리 없이 망가진 파일이 된다. `Content-Range`가 요청한 자리에서 시작하는 `206`에만
덧붙여라.

`multipart/form-data` 본문은 폼이 파일을 올리는 방식이다. 필드 하나하나가 파트이고, 파트들은 어느
파트 안에도 나타나서는 안 되는 경계 줄로 나뉜다.

| API | 의도 | 반환 |
|---|---|---|
| `proven_http_multipart_boundary(random, out)` | 여러분이 준 무작위 16바이트로 경계 - `PROVEN_HTTP_BOUNDARY_SIZE`(40) 글자 - 를 만든다. | 없음. |
| `proven_http_multipart_write_content_type(out, &len, boundary)` | `Content-Type: multipart/form-data; boundary=...`를 덧붙인다. | `proven_err_t`: 경계가 비었거나, 70자를 넘거나, 따옴표가 필요하면 `INVALID_ARG`. |
| `proven_http_multipart_write_part(out, &len, boundary, name, filename, content_type)` | 파트의 여는 부분을 덧붙인다. `filename`이 비면 평범한 필드다. `content_type`이 비면 쓰지 않는다. 파트의 바이트는 이어서 여러분이 쓴다. | `proven_err_t`: 이름이 비었거나 `content_type`에 제어 문자가 있으면 `INVALID_ARG`. |
| `proven_http_multipart_write_part_end(out, &len)` | 파트의 바이트를 끝내는 줄바꿈을 덧붙인다. | `proven_err_t`. |
| `proven_http_multipart_write_end(out, &len, boundary)` | 닫는 경계를 덧붙인다. | `proven_err_t`. |

**경계는 예측할 수 없어야 한다.** 올린 파일의 바이트 안에 경계 줄이 들어갈 수 있다면, 그 파일은 자기
파트를 끝내고 다른 파트 - 폼에 없던 필드 - 를 시작할 수 있다. `proven_random_bytes`에서 얻은
16바이트는 그 가능성을 키를 맞히는 것만큼 낮춘다. 카운터나 시계로 만든 경계는 그렇지 않다.

필드 이름이나 파일 이름 안의 큰따옴표와 줄바꿈은 브라우저가 하듯 퍼센트 인코딩해서 쓴다. 그래서
`a".png`라는 파일이 자기가 적힌 따옴표 문자열을 닫을 수 없다.

이 함수들은 틀만 만든다. 파트의 바이트는 `_write_part`와 `_write_part_end` 사이에 여러분이 쓰므로,
큰 파일도 통째로 쥐지 않고 조각조각 보낼 수 있다. 그리고 남이 보낸 multipart 본문을 파싱하는 것은
여기에 없다.

## 11. 인증

HTTP 자체의 인증은 요구와 응답이다. `401` 응답이 `WWW-Authenticate`를 싣고 오고, 요청을
`Authorization`과 함께 다시 보낸다.

| API | 의도 | 반환 |
|---|---|---|
| `proven_http_basic_auth(user, password, out, &written)` | `Basic <base64(user:password)>` 값을 쓴다. | `proven_err_t`: `user`에 콜론이 있거나 어느 쪽에든 제어 문자가 있으면 `INVALID_ARG`. `OUT_OF_BOUNDS`. |
| `proven_http_auth_offers(header_value, scheme)` | `WWW-Authenticate` 값이 `scheme`을 내놓는지(대소문자 무시). | `bool`. |
| `proven_http_digest_challenge_parse(header_value, &challenge)` | `WWW-Authenticate` 값에서 Digest 요구를 찾는다. 여럿이면 이 라이브러리가 가진 가장 강한 알고리즘의 것을 고른다. | `proven_err_t`: `NOT_FOUND` - Digest 요구가 없다. `UNSUPPORTED` - 없는 알고리즘뿐이거나 `qop=auth`가 없다. `INVALID_FORMAT`. |
| `proven_http_digest_auth(&challenge, user, password, method, uri, nonce_count, cnonce, out, &written)` | 그에 답하는 `Authorization` 값을 쓴다. `uri`는 보낸 그대로의 요청 타깃이고, `cnonce`는 여러분이 주는 무작위 텍스트이며, `nonce_count`는 새 nonce마다 1에서 시작한다. | `proven_err_t`: `INVALID_ARG`. `OUT_OF_BOUNDS`. |

**Basic은 비밀번호를 헤더에 실을 수 있게 인코딩한 것이지 숨긴 것이 아니다.** 요청을 읽을 수 있는
사람은 누구나 비밀번호를 읽을 수 있다. 암호화되지 않은 연결에서는 경로 위의 모든 사람이 그렇다.
TLS 위에서 쓰거나 아예 쓰지 마라.

**Digest는 비밀번호를 보내지 않는다. 하는 일은 그것뿐이다.** 응답은 비밀번호와 서버의 nonce와 요청
줄에 대한 해시라서, 엿듣는 사람이 나중에 로그인하는 데 쓸 것을 얻지 못한다. 그래도 요청과 응답은
읽을 수 있는 채로 네트워크를 건너고, 교환 한 번을 붙잡은 사람은 그것을 놓고 느긋하게 비밀번호를
추측해 볼 수 있다. 평문 HTTP 위의 Basic보다는 낫다. TLS를 대신하지는 못한다.

`MD5`, `MD5-sess`, `SHA-256`, `SHA-256-sess`가 `qop=auth`와 함께 구현되어 있다. MD5가 있는 까닭은
현장의 서버들이 아직 그것을 요구하기 때문이다 - 레거시 다이제스트는 4장을 보라. 둘 다 내놓는 요구에는
SHA-256으로 답한다. `qop=auth-int`, `SHA-512-256`, `userhash` 옵션은 없다.

**예상하지 않은 요구에는 답하지 마라.** 요구하는 서버 아무에게나 자격 증명을 보내는 클라이언트는
엉뚱한 서버에도 보낸다 - 이를테면 리다이렉트 뒤에. 그 자격 증명이 어느 호스트를 위한 것인지 정하고,
그 호스트에만 답하라.

테스트 스위트가 컴파일하고 실행한다:

<!-- example: manual/examples/ko/ex_10_http_helpers.c -->
```c
#include <string.h>

/*
 * 클라이언트와 서버가 메시지 코덱 둘레에서 필요로 하는 작은 조각들: 리다이렉트 풀기,
 * 폼 만들기, 리소스의 일부 요청하기, 파일 올리기, 인증 요구에 답하기. 모두 텍스트를 받아
 * 텍스트를 낸다 - 소켓도 할당도 없다.
 */

static bool view_is(proven_u8str_view_t v, const char *text) {
    return proven_u8str_view_eq(v, proven_u8str_view_from_cstr(text));
}

int main(void) {
    proven_byte_t buf[512];
    proven_mem_mut_t out = (proven_mem_mut_t){ buf, sizeof buf };
    proven_size_t len = 0;

    /* Location 헤더는 참조다. 그것이 어디로 이어지는지는 지금 어디에 있는지에 달려 있다. */
    proven_u8str_view_t base = PROVEN_LIT("http://example.com/docs/guide/intro.html?v=2");
    EXAMPLE_REQUIRE(proven_url_resolve(base, PROVEN_LIT("../img/logo.png"), out, &len) == PROVEN_OK &&
                    view_is((proven_u8str_view_t){ buf, len }, "http://example.com/docs/img/logo.png"), "a relative reference");
    EXAMPLE_REQUIRE(proven_url_resolve(base, PROVEN_LIT("/login?next=%2F"), out, &len) == PROVEN_OK &&
                    view_is((proven_u8str_view_t){ buf, len }, "http://example.com/login?next=%2F"), "an absolute path");
    EXAMPLE_REQUIRE(proven_url_resolve(base, PROVEN_LIT("//cdn.example.net/x"), out, &len) == PROVEN_OK &&
                    view_is((proven_u8str_view_t){ buf, len }, "http://cdn.example.net/x"), "another host, same scheme");

    /* 폼 본문을 한 쌍씩. 구분자와 인코딩은 알아서 처리된다. */
    len = 0;
    EXAMPLE_REQUIRE(proven_url_form_append(out, &len, proven_mem_view_from_u8(PROVEN_LIT("q")), proven_mem_view_from_u8(PROVEN_LIT("tea & cake"))) == PROVEN_OK &&
                    proven_url_form_append(out, &len, proven_mem_view_from_u8(PROVEN_LIT("page")), proven_mem_view_from_u8(PROVEN_LIT("2"))) == PROVEN_OK,
                    "two fields");
    EXAMPLE_REQUIRE(view_is((proven_u8str_view_t){ buf, len }, "q=tea+%26+cake&page=2"), "application/x-www-form-urlencoded");

    /* 범위. 클라이언트가 요청하고, 서버는 그 요청을 자기가 가진 것의 크기에 견주어 읽는다. */
    len = 0;
    EXAMPLE_REQUIRE(proven_http_write_range(out, &len, 100, PROVEN_HTTP_RANGE_TO_END) == PROVEN_OK &&
                    view_is((proven_u8str_view_t){ buf, len }, "Range: bytes=100-\r\n"), "from byte 100 to the end");
    proven_u64 first = 0, last = 0, total = 0;
    EXAMPLE_REQUIRE(proven_http_range_parse(PROVEN_LIT("bytes=100-"), 1000, &first, &last) == PROVEN_OK && first == 100 && last == 999, "of 1000 bytes: 100 through 999");
    EXAMPLE_REQUIRE(proven_http_range_parse(PROVEN_LIT("bytes=-50"), 1000, &first, &last) == PROVEN_OK && first == 950 && last == 999, "the last fifty");
    EXAMPLE_REQUIRE(proven_http_range_parse(PROVEN_LIT("bytes=2000-"), 1000, &first, &last) == PROVEN_ERR_OUT_OF_BOUNDS, "past the end: answer 416");
    EXAMPLE_REQUIRE(proven_http_range_parse(PROVEN_LIT("bytes=0-9,20-29"), 1000, &first, &last) == PROVEN_ERR_UNSUPPORTED, "several ranges: send the whole thing");
    len = 0;
    EXAMPLE_REQUIRE(proven_http_write_content_range(out, &len, 950, 999, 1000) == PROVEN_OK &&
                    view_is((proven_u8str_view_t){ buf, len }, "Content-Range: bytes 950-999/1000\r\n"), "the 206 says which part it is");
    bool has_total = false;
    EXAMPLE_REQUIRE(proven_http_content_range_parse(PROVEN_LIT("bytes 950-999/1000"), &first, &last, &total, &has_total) == PROVEN_OK &&
                    first == 950 && last == 999 && has_total && total == 1000, "and the client reads it back");

    /* multipart/form-data: 텍스트 필드 하나와 파일 하나. 경계는 여러분이 준 무작위 바이트에서
     * 나오므로, 미리 짐작해 파트 안에 심어 둘 수 없다. */
    proven_byte_t random[16];
    EXAMPLE_REQUIRE(proven_random_bytes(random, sizeof random), "sixteen random bytes");
    proven_byte_t boundary_bytes[PROVEN_HTTP_BOUNDARY_SIZE];
    proven_http_multipart_boundary(random, boundary_bytes);
    proven_u8str_view_t boundary = (proven_u8str_view_t){ boundary_bytes, sizeof boundary_bytes };

    proven_byte_t head[128];
    proven_size_t head_len = 0;
    EXAMPLE_REQUIRE(proven_http_multipart_write_content_type((proven_mem_mut_t){ head, sizeof head }, &head_len, boundary) == PROVEN_OK, "the header that announces it");

    len = 0;
    proven_err_t err = proven_http_multipart_write_part(out, &len, boundary, PROVEN_LIT("title"), PROVEN_LIT(""), PROVEN_LIT(""));
    if (err == PROVEN_OK) { memcpy(buf + len, "Holiday", 7); len += 7; }      /* 파트의 바이트는 여러분이 쓴다 */
    if (err == PROVEN_OK) err = proven_http_multipart_write_part_end(out, &len);
    if (err == PROVEN_OK) err = proven_http_multipart_write_part(out, &len, boundary, PROVEN_LIT("photo"), PROVEN_LIT("a \"b\".png"), PROVEN_LIT("image/png"));
    if (err == PROVEN_OK) { memcpy(buf + len, "\x89PNG", 4); len += 4; }
    if (err == PROVEN_OK) err = proven_http_multipart_write_part_end(out, &len);
    if (err == PROVEN_OK) err = proven_http_multipart_write_end(out, &len, boundary);
    EXAMPLE_REQUIRE(err == PROVEN_OK, "a body of two parts");
    buf[len] = '\0';
    EXAMPLE_REQUIRE(strstr((const char *)buf, "filename=\"a %22b%22.png\"") != NULL, "a quote in a file name cannot end the quoted string");

    /* 인증. Basic은 두 문자열을 인코딩한 것이다 - 암호화한 것이 아니다. */
    proven_size_t n = 0;
    EXAMPLE_REQUIRE(proven_http_basic_auth(PROVEN_LIT("Aladdin"), PROVEN_LIT("open sesame"), out, &n) == PROVEN_OK &&
                    view_is((proven_u8str_view_t){ buf, n }, "Basic QWxhZGRpbjpvcGVuIHNlc2FtZQ=="), "the value of an Authorization header");

    /* Digest는 요구에 답하는 방식이라 비밀번호 자체는 보내지 않는다. */
    proven_u8str_view_t www = PROVEN_LIT("Digest realm=\"http-auth@example.org\", qop=\"auth, auth-int\", algorithm=SHA-256, "
                                         "nonce=\"7ypf/xlj9XXwfDPEoM4URrv/xwf94BcCAzFZH4GiTo0v\", opaque=\"FQhe/qaU925kfnzjCev0ciny7QMkPqMAFRtzCUYo5tdS\"");
    EXAMPLE_REQUIRE(proven_http_auth_offers(www, PROVEN_LIT("digest")) && !proven_http_auth_offers(www, PROVEN_LIT("Basic")), "what the server offers");
    proven_http_digest_challenge_t challenge;
    EXAMPLE_REQUIRE(proven_http_digest_challenge_parse(www, &challenge) == PROVEN_OK && challenge.algorithm == PROVEN_HTTP_DIGEST_SHA256 && challenge.qop_auth, "the challenge");
    EXAMPLE_REQUIRE(proven_http_digest_auth(&challenge, PROVEN_LIT("Mufasa"), PROVEN_LIT("Circle of Life"), PROVEN_LIT("GET"), PROVEN_LIT("/dir/index.html"),
                                            1, PROVEN_LIT("f2/wE4q74E6zIJEtWaHKaf5wv/H5QzzpXusqGemxURZJ"), out, &n) == PROVEN_OK, "the answer");
    buf[n] = '\0';
    /* RFC 7616 3.9.1절이 바로 이 입력으로 계산해 보인 response 값. */
    EXAMPLE_REQUIRE(strstr((const char *)buf, "response=\"753927fa0e85d155564e2e272a28d1802ca10daf4496794697cf8db5856cb6c1\"") != NULL, "is the one in the RFC");

    return EXAMPLE_OK();
}
```

## 12. 쿠키

쿠키는 서버가 되돌려 보내 달라고 요청하는 이름과 값이다. `proven_http_cookie_jar_t`가 요청과 요청
사이에 그것을 보관한다. `Set-Cookie` 헤더가 심은 것을 저장하고, 요청에 실을 `Cookie` 헤더를 써 준다.

| API | 의도 | 반환 |
|---|---|---|
| `proven_http_cookie_jar_init(&jar, alloc, max_cookies)` | `max_cookies`개까지 담는 보관함을 만든다. | `proven_err_t`: 한도가 0이면 `INVALID_ARG`. `NOMEM`. |
| `proven_http_cookie_jar_store(&jar, host, path, secure, set_cookie, now)` | `host`와 `path`로 보낸 요청의 응답에 있던 `Set-Cookie` 값 하나를 처리한다. `now`는 `proven_time_now()`다. | `proven_err_t`: `INVALID_FORMAT` - 쿠키가 아니다, 무시하라. `PERMISSION` - 이 호스트가 심을 수 있는 것이 아니다. `OUT_OF_BOUNDS` - `PROVEN_HTTP_COOKIE_MAX_SIZE`보다 크다. `NOMEM`. 어느 경우든 보관함은 그대로다. |
| `proven_http_cookie_jar_header(&jar, host, path, secure, now, out, &written)` | 요청에 실을 `Cookie` 값을 쓴다: `a=1; b=2`. 보낼 것이 없으면 `written`이 0이다. | `proven_err_t`: `OUT_OF_BOUNDS`. |
| `proven_http_cookie_jar_count(&jar)` | 쿠키를 몇 개 갖고 있는지. | `proven_size_t`. |
| `proven_http_cookie_jar_clear(&jar)` | 모두 잊는다. | 없음. |
| `proven_http_cookie_jar_destroy(&jar)` | 해제한다. | 없음. |

**이 보관함은 일부러 호스트 전용이다.** 브라우저는 `www.example.com`이 `example.com` 전체를 위한
쿠키를 심게 해 주는데, 그것을 안전하게 하려면 공개 접미사 목록 - `co.uk`는 사이트가 아니고
`example.co.uk`는 사이트라고 말해 주는 표 - 이 필요하다. 이 라이브러리에는 그런 표가 없고, 그 표
없이 `Domain`을 따르는 보관함은 `evil.co.uk`가 `co.uk` 아래 모든 사이트를 위한 쿠키를 심게 내버려
둔다. 그래서 `Domain` 속성은 검사한 다음 - 그것을 보낸 호스트를 덮지 않으면 `PROVEN_ERR_PERMISSION`
이다 - 옆으로 치운다. 모든 쿠키는 그것을 심은 바로 그 호스트에게만 돌아가고 다른 곳으로는 가지
않는다. 하위 도메인끼리 쿠키를 공유하는 데 기대는 사이트는 이 보관함으로는 동작하지 않는다. 받아서는
안 될 것이 그 사이트에 닿는 일도 없다.

보관함이 RFC 6265 가운데 구현한 것: `Path`와 그 일치 규칙, `Max-Age`와 `Expires`(`Max-Age`가
이긴다), `Secure` - 암호화되지 않은 연결로는 보내지 않고, 그런 연결에서 심으려 하면 거절한다 - ,
이름과 경로가 같은 쿠키의 교체, 만료를 통한 삭제, 보관함이 찼을 때 가장 오래된 것 내보내기.
`HttpOnly`와 `SameSite`는 브라우저의 스크립트와 페이지가 무엇을 해도 되는지를 말한다. 프로그램에는
둘 다 없으므로 받아들이고 무시한다.

**보관함은 스레드 안전하지 않고**, 메모리 안에만 산다. destroy하면 쿠키는 사라진다.

## 13. 서버 전송 이벤트

이벤트 스트림은 끝나지 않는 응답이다. `Content-Type: text/event-stream`이 오고, 그다음 연결이 살아
있는 동안 줄이 계속 온다.

```text
id: 41
event: price
data: {"sym":"X","p":12.5}

data: a second event, of the default type
```

빈 줄이 이벤트를 끝낸다. `proven_sse_t`는 스트림을 도착한 조각 그대로 받아 이벤트를 돌려준다.

| API | 의도 | 반환 |
|---|---|---|
| `proven_sse_init(&sse, work)` | 시작한다. `work`는 줄과 조립 중인 이벤트를 위한 메모리이고 - 절반씩 - , 따라서 받아들일 가장 큰 이벤트의 한도가 된다. 최소 64바이트. | `proven_err_t`: `INVALID_ARG`. |
| `proven_sse_feed(&sse, in, &consumed, &event, &have_event)` | 이벤트 하나가 완성되거나 `in`을 다 쓸 때까지 `in`의 바이트를 소비한다. `have_event`가 true면 `event`를 쓰고 나머지로 다시 부른다. | `proven_err_t`: 줄이나 이벤트가 `work`보다 커지면 `OUT_OF_BOUNDS`. 그 뒤로는 `INVALID_STATE`. |
| `proven_sse_last_id(&sse)` | 마지막으로 본 `id`. 다시 연결할 때 `Last-Event-ID`로 보낸다. | `proven_u8str_view_t`. 없었으면 비어 있다. |

```text
typedef struct {
    proven_u8str_view_t event;   /* the type; empty means the default, "message" */
    proven_u8str_view_t data;    /* the data lines joined with LF */
    proven_u8str_view_t id;      /* the last id so far, from this event or an earlier one */
    proven_u32 retry_ms;         /* valid when has_retry */
    bool has_retry;
} proven_sse_event_t;
```

**이벤트의 뷰(view)는 다음 feed까지만 유효하다.** `work` 안을 가리키기 때문이다. 간직할 것은 복사하라.

**이벤트 중간에서 끝난 스트림은 그 이벤트를 전달하지 않은 것이다.** 표준은 이벤트가 빈 줄로
내보내진다고 정한다. 그 전에 끊긴 이벤트는 절반만 전달되는 것이 아니라 버려진다. 연결이 끝나면
`Last-Event-ID`를 달고 다시 연결하라. 서버가 다시 보내 줄 수 있다.

파서는 WHATWG 규칙을 따른다 - 세 가지 줄 끝, 주석 줄, 맨 앞의 바이트 순서 표시, 콜론 없는 필드,
콜론 뒤의 공백 하나 - . 다시 연결하지는 않는다. 그것은 클라이언트의 일이다. 이것은 파서이고, 연결은
11장의 것이다.

테스트 스위트가 컴파일하고 실행한다:

<!-- example: manual/examples/ko/ex_10_cookies_events.c -->
```c
/*
 * 클라이언트가 메시지와 메시지 사이에 간직하는 두 가지 상태: 서버가 심은 쿠키와,
 * 서버 전송 이벤트 스트림에서의 자기 위치. 어느 쪽도 소켓을 건드리지 않는다.
 */

static bool view_is(proven_u8str_view_t v, const char *text) {
    return proven_u8str_view_eq(v, proven_u8str_view_from_cstr(text));
}

int main(void) {
    // ---- 쿠키 보관함 ---------------------------------------------------------
    proven_http_cookie_jar_t jar;
    EXAMPLE_REQUIRE(proven_http_cookie_jar_init(&jar, proven_heap_allocator(), 32) == PROVEN_OK, "a jar for up to 32 cookies");
    proven_time_t now = proven_time_now();

    /* 응답의 Set-Cookie 헤더 하나하나를, 그것이 어디서 왔는지와 함께 저장한다. */
    proven_u8str_view_t host = PROVEN_LIT("shop.example.com");
    EXAMPLE_REQUIRE(proven_http_cookie_jar_store(&jar, host, PROVEN_LIT("/"), true, PROVEN_LIT("sid=abc123; Path=/; Secure; HttpOnly"), now) == PROVEN_OK, "a session cookie");
    EXAMPLE_REQUIRE(proven_http_cookie_jar_store(&jar, host, PROVEN_LIT("/cart/view"), true, PROVEN_LIT("items=3; Max-Age=3600"), now) == PROVEN_OK,
                    "one with no Path: it belongs to /cart");
    /* 남의 도메인을 위한 쿠키는 거절된다. 이 보관함은 브라우저보다 더 엄격해서, 모든 쿠키를
     * 그것을 심은 바로 그 호스트에만 돌려준다. */
    EXAMPLE_REQUIRE(proven_http_cookie_jar_store(&jar, host, PROVEN_LIT("/"), true, PROVEN_LIT("track=1; Domain=ads.example.net"), now) == PROVEN_ERR_PERMISSION,
                    "not this host's to set");
    EXAMPLE_REQUIRE(proven_http_cookie_jar_count(&jar) == 2, "two cookies");

    /* 요청을 보낼 때 보관함이 Cookie 헤더의 값을 써 준다. */
    proven_byte_t buf[256];
    proven_mem_mut_t out = { buf, sizeof buf };
    proven_size_t n = 0;
    EXAMPLE_REQUIRE(proven_http_cookie_jar_header(&jar, host, PROVEN_LIT("/cart/checkout"), true, now, out, &n) == PROVEN_OK &&
                    view_is((proven_u8str_view_t){ buf, n }, "items=3; sid=abc123"), "under /cart: both, the longer path first");
    EXAMPLE_REQUIRE(proven_http_cookie_jar_header(&jar, host, PROVEN_LIT("/account"), true, now, out, &n) == PROVEN_OK &&
                    view_is((proven_u8str_view_t){ buf, n }, "sid=abc123"), "elsewhere: only the one for /");
    EXAMPLE_REQUIRE(proven_http_cookie_jar_header(&jar, host, PROVEN_LIT("/account"), false, now, out, &n) == PROVEN_OK && n == 0,
                    "over plain HTTP: nothing - sid is Secure. n == 0 means send no header");
    EXAMPLE_REQUIRE(proven_http_cookie_jar_header(&jar, PROVEN_LIT("example.com"), PROVEN_LIT("/"), true, now, out, &n) == PROVEN_OK && n == 0,
                    "another host: nothing");

    /* 시간이 흐르면 Max-Age가 다한다. 서버는 쿠키를 만료시켜서 지운다. */
    proven_time_t later = now + (proven_time_t)7200 * 1000000000;
    EXAMPLE_REQUIRE(proven_http_cookie_jar_header(&jar, host, PROVEN_LIT("/cart"), true, later, out, &n) == PROVEN_OK &&
                    view_is((proven_u8str_view_t){ buf, n }, "sid=abc123"), "two hours on, items has expired");
    EXAMPLE_REQUIRE(proven_http_cookie_jar_store(&jar, host, PROVEN_LIT("/"), true, PROVEN_LIT("sid=; Path=/; Max-Age=0"), later) == PROVEN_OK &&
                    proven_http_cookie_jar_count(&jar) == 0, "logged out: the jar is empty");

    proven_http_cookie_jar_clear(&jar);       /* 또는 직접 비운다 */
    proven_http_cookie_jar_destroy(&jar);

    // ---- 서버 전송 이벤트 ---------------------------------------------------
    /* 파서에는 도착한 것을 도착한 조각 그대로 넣고, 파서는 온전한 이벤트를 돌려준다. */
    proven_byte_t work[512];
    proven_sse_t sse;
    EXAMPLE_REQUIRE(proven_sse_init(&sse, (proven_mem_mut_t){ work, sizeof work }) == PROVEN_OK, "a parser with room for 256-byte events");

    static const char *const arrived[] = {
        ": keep-alive\n\n",                              /* 주석 줄: 이벤트가 아니다 */
        "id: 41\nevent: price\ndata: {\"sym\":\"X\",",   /* 중간에서 끊긴 이벤트 */
        "\ndata:  \"p\":12.5}\n\n",                      /* ...그리고 그 끝 */
        "data: plain\n\n",
    };
    int events = 0;
    for (proven_size_t i = 0; i < 4; ++i) {
        proven_mem_view_t in = proven_mem_view_from_u8(proven_u8str_view_from_cstr(arrived[i]));
        while (in.size > 0) {
            proven_size_t used = 0;
            proven_sse_event_t ev;
            bool have = false;
            EXAMPLE_REQUIRE(proven_sse_feed(&sse, in, &used, &ev, &have) == PROVEN_OK, "the stream parses");
            in.ptr += used;
            in.size -= used;
            if (!have) continue;           /* 전부 소비했고 아직 이벤트는 없다 */
            events++;
            if (events == 1) {
                EXAMPLE_REQUIRE(view_is(ev.event, "price") && view_is(ev.id, "41"), "a typed event with an id");
                EXAMPLE_REQUIRE(view_is(ev.data, "{\"sym\":\"X\",\n \"p\":12.5}"), "two data lines, joined by a line feed");
            } else {
                EXAMPLE_REQUIRE(ev.event.size == 0 && view_is(ev.data, "plain"), "no type given: a \"message\"");
                EXAMPLE_REQUIRE(view_is(ev.id, "41"), "the id stays until another replaces it");
            }
        }
    }
    EXAMPLE_REQUIRE(events == 2, "two events from four pieces");
    /* 연결이 끊긴 뒤 이것을 Last-Event-ID로 보내면 서버가 거기서부터 이어 줄 수 있다. */
    EXAMPLE_REQUIRE(view_is(proven_sse_last_id(&sse), "41"), "where to resume");

    return EXAMPLE_OK();
}
```

## 14. 여기에 없는 것

- **클라이언트나 서버.** [11장](manual-11-http-client-server-ko.md)에 있다.
- **HTTP/2와 HTTP/3.** `HTTP/2.0`이라고 적힌 헤드는 `PROVEN_ERR_UNSUPPORTED`다.
- **코덱 안의 콘텐츠 코딩.** 파서와 본문 디코더는 본문을 보낸 그대로 건넨다. 관련 헤더를 읽는 헬퍼가
  둘 있고(`proven_http_accepts_coding`, `proven_http_content_coding`), 이 코덱 위에 지은 서버와
  클라이언트는 `gzip`을 압축하고 풀 수 있다: [16장](manual-16-compression-ko.md) 6절.
- **트레일러.** 청크 본문 뒤의 트레일러는 검사하고 건너뛸 뿐, 돌려주지 않는다.
- **multipart 본문 파싱, 그리고 다중 범위(`multipart/byteranges`) 응답.**
- **공개 접미사 목록**, 따라서 하위 도메인 사이에 공유되는 쿠키 - §12를 보라.
- **`Negotiate`, `NTLM`, bearer 토큰 흐름.** bearer 토큰은 여러분이 직접 쓰는 헤더다.
