# 9장: 네트워킹

**5부 — 운영체제와 대화하기. 선행 조건: 2부
([1](manual-01-foundation-ko.md), [2](manual-02-allocation-ko.md), [3](manual-03-strings-text-ko.md)),
그리고 [5장](manual-05-hosted-services-ko.md)의 §4(두 시계)와 스트림 절.**
**이 장을 마치면** TCP나 UDP 소켓을 열고, 기다리는 모든 것에 시간 제한을 걸고, 거절된 연결과
끊어진 연결을 구별하고, 스레드 하나로 여러 소켓을 처리하고, 무엇이 실어 나르는지 상관하지 않는
프로토콜을 쓸 수 있다.

이 장은 `net.h`를 다룬다. 5장처럼 운영체제가 필요하며 [프리스탠딩](manual-freestanding-ko.md) 빌드에는
들어가지 않는다. 호스티드 빌드에서는 `PROVEN_NO_NET`으로 뺄 수 있다.

## 목차

1. [세 가지 규칙](#1-세-가지-규칙)
2. [주소](#2-주소)
3. [기한](#3-기한)
4. [TCP](#4-tcp)
5. [UDP](#5-udp)
6. [소켓 여럿, 스레드 하나](#6-소켓-여럿-스레드-하나)
7. [전송](#7-전송)
8. [짝, 그리고 루프 깨우기](#8-짝-그리고-루프-깨우기)
9. [selector: 소켓 수천 개](#9-selector-소켓-수천-개)
10. [플랫폼마다 다른 것](#10-플랫폼마다-다른-것)
11. [여기에 없는 것](#11-여기에-없는-것)

## 1. 세 가지 규칙

BSD 소켓은 마흔 살이고, 그 날카로운 모서리마다 이름이 붙어 있다. `net.h`는 그중 셋을 치우는 것이
일의 전부인 얇은 계층이다.

**기다릴 수 있는 모든 호출은 기한을 받는다.** 맨 소켓의 `recv`는 상대가 기다리게 하는 만큼 기다린다.
그렇게 지은 프로그램은 책상 위에서는 돌고, 운영 환경에서는 클라이언트 하나가 연결만 열고 아무 말도
하지 않는 첫 순간에 멈춘다. 연결을 하나씩 처리하는 서버라면 그런 클라이언트 하나로 충분하다. 여기서는
그런 프로그램을 쓸 방법이 없다. read, write, accept, connect 모두 기한 인자를 받고, 무엇이든 말해야
한다.

**실패는 어느 실패인지 말한다.** "호출이 실패했다"만으로는 다음에 무엇을 할지 정할 수 없다. 상대가
거절했는지, 사라졌는지, 제대로 끝냈는지는 서로 다른 세 상황이고 대응도 셋이다.

| 일어난 일 | 코드 | 합당한 대응 |
|---|---|---|
| 상대가 보내기를 끝냈다 | `PROVEN_ERR_EOF` | 읽기를 멈춘다. 정상적인 끝이다 |
| 기한이 지났다 | `PROVEN_ERR_TIMEOUT` | 재시도하거나, 포기하거나, 닫는다 — 망가진 것은 없다 |
| 그 주소에서 듣는 것이 없다 | `PROVEN_ERR_REFUSED` | 다른 주소를 시도하거나 보고한다. 재시도하면 같은 답이 온다 |
| 상대가 사라졌거나 연결이 중단됐다 | `PROVEN_ERR_RESET` | 닫는다. 전송 중이던 것은 사라졌을 수 있다 |
| 호스트나 네트워크로 가는 경로가 없다 | `PROVEN_ERR_UNREACHABLE` | 보고하거나 나중에 재시도한다 |
| 바인드할 주소가 이미 쓰이고 있거나, 시스템에 소켓이 바닥났다 | `PROVEN_ERR_BUSY` | 물러나거나 다른 포트를 고른다 |
| 그 주소는 바인드할 수 없다 | `PROVEN_ERR_PERMISSION` | 보고한다. 재시도해도 소용없다 |
| 호스트 이름에 주소가 없다. 유닉스 도메인 경로가 없다 | `PROVEN_ERR_NOT_FOUND` | 보고한다 |
| 데이터그램이 버퍼에 들어가지 않았거나, 보내기에 너무 크다 | `PROVEN_ERR_OUT_OF_BOUNDS` | 버퍼를 키운다. 덜 보낸다 |
| 소켓이 열려 있지 않다 | `PROVEN_ERR_INVALID_STATE` | 호출 순서를 고친다 |

이 중 어느 것도 맞지 않을 때 남는 것이 `PROVEN_ERR_IO`다.

**숨기는 것이 없다.** 이 헤더의 어떤 호출도 할당하지 않는다. 소켓은 여러분이 소유하고 닫아야 하는
작은 값이다. 소켓 위의 읽기 스트림(reader)이나 쓰기 스트림(writer)은 여러분이 선언한 상태를 쓴다. 프로세스 전체에 걸친 상태는
하나뿐이고 - Windows에서 Winsock을 시작하는 일 - 첫 사용 때 한 번 이뤄지며 여러분이 관리할 것이
아니다.

## 2. 주소

`proven_net_addr_t`는 소켓이 어디 있는지, 또는 어디로 가야 하는지를 말한다. 평범한 값이다. 복사하고,
비교하고, 표에 넣어도 된다.

```text
typedef struct {
    proven_net_family_t family;   /* IPV4, IPV6 or UNIX; NONE when unset */
    proven_u16 port;              /* host byte order: the number you would write down */
    proven_u32 scope_id;          /* IPv6 zone for a link-local address; 0 otherwise */
    proven_byte_t ip[16];         /* network order: 4 bytes for IPv4, 16 for IPv6 */
    proven_u8 path_len;           /* Unix-domain only */
    char path[104];               /* Unix-domain only */
} proven_net_addr_t;
```

### 참조

| API | 의도 | 반환 |
|---|---|---|
| `proven_net_addr_ipv4(a, b, c, d, port)` | 숫자 넷으로 만드는 IPv4 주소. | `proven_net_addr_t`. |
| `proven_net_addr_ipv6(ip[16], port, scope_id)` | 네트워크 순서의 바이트 열여섯 개로 만드는 IPv6 주소. | `proven_net_addr_t`. |
| `proven_net_addr_loopback(family, port)` | 이 기계만: `127.0.0.1` 또는 `::1`. | `proven_net_addr_t`. `family`에 루프백이 없으면 family가 `NONE`. |
| `proven_net_addr_any(family, port)` | 모든 인터페이스: `0.0.0.0` 또는 `::`. | `proven_net_addr_t`. |
| `proven_net_addr_unix(path, &out)` | 파일 시스템 경로로 만드는 유닉스 도메인 주소. | `proven_err_t`: 빈 경로나 NUL이 든 경로는 `INVALID_ARG`, `PROVEN_NET_UNIX_PATH_MAX`를 넘으면 `OUT_OF_BOUNDS`. |
| `proven_net_addr_parse(text, port, &out)` | 리터럴을 파싱한다: `192.0.2.1`, `2001:db8::1`, `[2001:db8::1]`, `fe80::1%3`. 네트워크를 건드리지 않는다. | `proven_err_t`: `text`가 주소가 아니면 `INVALID_FORMAT`. 그때 `out`은 그대로다. |
| `proven_net_addr_format(&addr, out, &written)` | `192.0.2.1:80`, `[2001:db8::1]:80`, 또는 경로를 쓴다. NUL은 쓰지 않는다. `PROVEN_NET_ADDR_TEXT_MAX`바이트면 언제나 충분하다. | `proven_err_t`: `out`이 작으면 `OUT_OF_BOUNDS`. |
| `proven_net_addr_eq(&a, &b)` | family, 주소, 포트, zone이 같은가 - 또는 경로가 같은가. | `bool`. |
| `proven_net_resolve(host, port, out, cap, &count)` | 시스템 리졸버로 이름을 찾는다. 리터럴은 질의 없이 답한다. | `proven_err_t`: `NOT_FOUND`, `TIMEOUT`(네임 서버가 답하지 않음), 빈 이름이나 너무 긴 이름은 `INVALID_ARG`. |

파서의 두 가지 결정은 의도한 것이다. IPv4 리터럴은 정확히 십진수 넷이다. 오래된 파서들이 받아 주는
`127.1`과 `2130706433`은 거절하고, 그 파서들이 팔진수 *8*로 읽는 `010.0.0.1`도 거절한다. 그리고
IPv6는 들어올 때의 표기는 여럿이지만 나갈 때는 하나다 - 소문자, 앞자리 0 없음, 가장 긴 0 그룹의
연속을 `::`로(RFC 5952). 그래서 같은 주소 둘은 같은 텍스트로 찍히고, 그 텍스트를 map 키나 검색할
로그 필드로 쓸 수 있다.

### 주의사항, 그리고 무엇이 잘못되는가

**`proven_net_resolve`는 블록하고 기한이 없다.** 시스템 리졸버이고, 리졸버가 기한을 제공하지 않으며,
망가진 네트워크에서는 수십 초가 걸릴 수 있다. 이 헤더에서 한도 없이 기다리는 호출은 이것 하나이고,
헤더는 아닌 척하지 않고 그렇게 적는다.

잘못된 예 — 다른 모든 연결을 처리하는 루프 안에서 이름을 해석하기:

```text
for (;;) {
    wait_for_events();
    if (client_wants_upstream)
        proven_net_resolve(name, 443, addrs, 4, &n);   /* wrong: every client waits on DNS */
}
```

올바른 예 — 기다려도 되는 자리에서 해석한다. 시작할 때, 또는 job에서
([6장](manual-06-execution-and-platform-ko.md)). 그리고 주소를 루프에 넘긴다.

**"any"에 바인드하면 서비스를 공개하는 것이다.** `proven_net_addr_any`는 네트워크를 향한 것까지 포함해
기계의 모든 인터페이스에서 듣는다. 자기 기계에서만 쓸 도구는 `proven_net_addr_loopback`에 바인드한다.

테스트 스위트가 컴파일하고 실행한다:

<!-- example: manual/examples/ko/ex_09_net_addr.c -->
```c
/*
 * 주소는 평범한 값이다. 만들고, 텍스트에서 파싱하고, 찍고, 둘을 비교한다.
 * 이 프로그램의 어느 줄도 소켓을 열거나 네트워크에 묻지 않는다.
 */

static bool prints_as(const proven_net_addr_t *addr, proven_u8str_view_t want) {
    proven_byte_t text[PROVEN_NET_ADDR_TEXT_MAX];
    proven_size_t n = 0;
    if (proven_net_addr_format(addr, (proven_mem_mut_t){ text, sizeof text }, &n) != PROVEN_OK) return false;
    return proven_u8str_view_eq((proven_u8str_view_t){ text, n }, want);
}

int main(void) {
    /* 부품으로 만든다. 포트는 종이에 적는 그 숫자다 - 호스트 바이트 순서. */
    proven_net_addr_t web = proven_net_addr_ipv4(192, 0, 2, 10, 80);
    EXAMPLE_REQUIRE(prints_as(&web, PROVEN_LIT("192.0.2.10:80")), "an IPv4 address prints as address:port");

    /* 프로그램에 가장 자주 필요한 고정 주소 둘: 이 기계만, 그리고 모든 인터페이스. */
    proven_net_addr_t local = proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 8080);
    proven_net_addr_t everywhere = proven_net_addr_any(PROVEN_NET_FAMILY_IPV6, 8080);
    EXAMPLE_REQUIRE(prints_as(&local, PROVEN_LIT("127.0.0.1:8080")), "loopback is 127.0.0.1");
    EXAMPLE_REQUIRE(prints_as(&everywhere, PROVEN_LIT("[::]:8080")), "any is ::, and IPv6 is bracketed before a port");

    /* 텍스트에서 파싱한다. IPv6 주소 하나의 여러 표기가 같은 바이트로 파싱되고, 나갈 때의
     * 표기는 정확히 하나다. 그래서 텍스트를 비교하고 키로 쓸 수 있다. */
    proven_net_addr_t a, b;
    EXAMPLE_REQUIRE(proven_net_addr_parse(PROVEN_LIT("2001:DB8:0:0:0:0:0:1"), 443, &a) == PROVEN_OK, "the long form parses");
    EXAMPLE_REQUIRE(proven_net_addr_parse(PROVEN_LIT("[2001:db8::1]"), 443, &b) == PROVEN_OK, "the short, bracketed form parses");
    EXAMPLE_REQUIRE(proven_net_addr_eq(&a, &b), "and they are the same address");
    EXAMPLE_REQUIRE(prints_as(&a, PROVEN_LIT("[2001:db8::1]:443")), "printed in the one canonical form");

    /* 주소가 아닌 것은 거절한다 - 다른 파서들이 조용히 받아 주는 것까지. */
    proven_net_addr_t bad;
    EXAMPLE_REQUIRE(proven_net_addr_parse(PROVEN_LIT("010.0.0.1"), 80, &bad) == PROVEN_ERR_INVALID_FORMAT,
                    "a leading zero is refused, not read as octal");
    EXAMPLE_REQUIRE(proven_net_addr_parse(PROVEN_LIT("example.com"), 80, &bad) == PROVEN_ERR_INVALID_FORMAT,
                    "a name is not a literal: that is proven_net_resolve's job");

    /* 주소가 패킷이나 파일에서 올 때는 네트워크 순서의 원시 바이트 열여섯 개로. */
    proven_byte_t raw[16] = { 0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
    proven_net_addr_t link_local = proven_net_addr_ipv6(raw, 22, 3);
    EXAMPLE_REQUIRE(prints_as(&link_local, PROVEN_LIT("[fe80::1%3]:22")), "a link-local address carries its zone");

    /* 유닉스 도메인 주소는 파일 시스템 경로다. */
    proven_net_addr_t sock;
    EXAMPLE_REQUIRE(proven_net_addr_unix(PROVEN_LIT("/run/app.sock"), &sock) == PROVEN_OK, "a path makes an address");
    EXAMPLE_REQUIRE(prints_as(&sock, PROVEN_LIT("/run/app.sock")), "and prints as the path");

    /* 리터럴의 해석은 질의 없이 답하므로, 이 줄은 네트워크가 없어도 동작한다.
     * 진짜 이름은 시스템 리졸버로 가고, 블록하며, 기한이 없다. 그래도 되는 자리에서
     * 불러라. */
    proven_net_addr_t found[4];
    proven_size_t count = 0;
    EXAMPLE_REQUIRE(proven_net_resolve(PROVEN_LIT("192.0.2.10"), 80, found, 4, &count) == PROVEN_OK && count == 1,
                    "a literal resolves to itself");
    EXAMPLE_REQUIRE(proven_net_addr_eq(&found[0], &web), "the same address as the one built by hand");

    return EXAMPLE_OK();
}
```

## 3. 기한

기한은 길이가 아니라 시점이다. 호출이 포기해야 하는, 단조 시계
([5장 §4](manual-05-hosted-services-ko.md))의 한 값이다.

| | 뜻 |
|---|---|
| `proven_net_deadline_in(ms)` | 지금부터 `ms`밀리초 뒤의 기한. |
| `PROVEN_NET_NO_DEADLINE` | 걸리는 만큼 기다린다. 그러면 말 없는 상대와 함께 기다리게 된다. |
| `PROVEN_NET_DONT_WAIT` | 지금 당장 할 수 있는 것만 한다. 나머지는 `PROVEN_ERR_TIMEOUT`이다. |

절대 시점인 이유는 기한 하나로 교환 전체를 묶기 위해서다. "이 요청에 5초"는 connect에도, 각 write에도,
각 read에도 넘기는 값 하나다. 호출별 타임아웃이었다면 걸음마다 같은 5초를 다시 계산해야 하고, 대개는
그러지 않는다.

단조 시계 위에 있는 이유는 시스템 시간을 맞춰도 기한이 움직이지 않게 하기 위해서다.

그리고 지나간 기한은 아무것도 망가뜨리지 않는다. `PROVEN_ERR_TIMEOUT`은 "아직"이라는 뜻이고, 소켓은
전과 똑같이 쓸 수 있다. 다시 시도할지, 포기할지, 닫을지는 여러분이 정한다.

잘못된 예 — 타임아웃을 죽은 연결로 취급하기:

```text
proven_result_size_t r = proven_net_read(&conn, buf, proven_net_deadline_in(100));
if (r.err != PROVEN_OK) proven_net_close(&conn);   /* wrong: TIMEOUT is not a failure of the connection */
```

올바른 예 — 이 프로토콜에서 침묵이 무엇을 뜻하는지 정한다:

```text
if (r.err == PROVEN_ERR_TIMEOUT) { send_keepalive(&conn); continue; }
if (r.err == PROVEN_ERR_EOF)     { finish(&conn); break; }
if (r.err != PROVEN_OK)          { proven_net_close(&conn); break; }
```

## 4. TCP

TCP 연결은 방향마다 하나씩의 바이트 스트림이다. 메시지 경계가 없다. 한쪽이 write 한 번으로 보낸 것을
다른 쪽은 read 세 번으로 받을 수 있고, write 둘을 read 하나로 받을 수도 있다.

### 참조

| API | 의도 | 반환 |
|---|---|---|
| `proven_net_listen(at, backlog, &listener, &bound)` | `at`에서 듣는다. 포트 0은 OS가 고른다. `bound`(선택)는 실제로 바인드된 주소다. | `proven_err_t`: `BUSY`(사용 중), `PERMISSION`, `INVALID_ARG`(이 기계의 주소가 아님), `UNSUPPORTED`. |
| `proven_net_accept(&listener, until, &conn, &peer)` | 다음 연결을 받는다. `peer`는 선택이다. | `proven_err_t`: 오지 않았으면 `TIMEOUT`. |
| `proven_net_listener_addr(&listener, &out)` | 리스너가 바인드된 곳. | `proven_err_t`. |
| `proven_net_listener_close(&listener)` | 듣기를 멈춘다. 이미 받은 연결에는 영향이 없다. | `proven_err_t`. |
| `proven_net_connect(to, until, &conn)` | 연결한다. 실패하면 닫을 것이 없다. | `proven_err_t`: `REFUSED`, `TIMEOUT`, `UNREACHABLE`, `NOT_FOUND`(유닉스 도메인 경로). |
| `proven_net_connect_start(to, &conn)` | 연결을 시작하고 곧바로 돌아온다. 여러 소켓을 함께 기다리는 프로그램을 위한 것이다(9절, 또는 [15장](manual-15-event-loop-ko.md)의 루프). | `proven_err_t`: 이미 연결되었으면 `PROVEN_OK`. 진행 중이면 `AGAIN` - 그때 `conn`은 쓸 수 있다고 보고될 때까지 지켜보고 닫는 것만 할 수 있다. 그 밖에는 실패이고 열린 것이 없다. |
| `proven_net_connect_finish(&conn)` | 진행 중이던 연결이, 그 소켓이 쓸 수 있다거나 실패했다고 보고된 뒤 어떻게 끝났는지. | `proven_err_t`: 연결되었으면 `PROVEN_OK`. `AGAIN`이면 다시 기다린다. 그 밖에는 실패이고 `conn`은 닫혔다. 열려 있지 않으면 `INVALID_STATE`. |
| `proven_net_read(&conn, dest, until)` | 도착한 것을 `dest.size`까지 읽는다. | `proven_result_size_t`: 끝에서는 `EOF` - 0바이트 성공은 없다. `TIMEOUT`. `RESET`. |
| `proven_net_write(&conn, src, until)` | 지금 들어가는 만큼 쓴다. `.value`는 실패했을 때도 나간 양이다. | `proven_result_size_t`. |
| `proven_net_write_all(&conn, src, until)` | `src` 전부를 쓰거나, 어디까지 갔는지 말하며 실패한다. | `proven_result_size_t`: `.err`가 OK일 때에만 `.value == src.size`. |
| `proven_net_shutdown_write(&conn)` | "더 보낼 것이 없다." 상대는 `EOF`를 읽고, 이쪽은 계속 읽을 수 있다. | `proven_err_t`. |
| `proven_net_close(&conn)` | 닫는다. 열려 있지 않은 연결에 불러도 안전하다. | `proven_err_t`. |
| `proven_net_conn_local_addr(&conn, &out)`, `proven_net_conn_peer_addr(&conn, &out)` | 연결의 두 끝. | `proven_err_t`. |
| `proven_net_conn_set_nodelay(&conn, on)` | 작은 write를 모으지 않고 바로 보낸다. | `proven_err_t`: 유닉스 도메인 연결에서는 `UNSUPPORTED`. |
| `proven_net_listener_is_open(&l)`, `proven_net_conn_is_open(&c)` | 값이 열린 소켓을 들고 있는가. | `bool`. |

`proven_net_listener_t`와 `proven_net_conn_t`는 여러분이 소유하는 작은 값이다. 연 것은 하나하나 닫고,
열려 있는 것을 복사하지 마라. 복사본 둘은 소켓 하나의 주인 둘이다.

**두 번에 나눠 하는 connect.** `proven_net_connect`는 기다린다. 많은 소켓을 한꺼번에 기다리는
프로그램은 그럴 수 없으므로, 같은 connect가 호출 둘로도 제공된다:

```c
proven_err_t proven_net_connect_start(proven_net_addr_t to, proven_net_conn_t *out);
proven_err_t proven_net_connect_finish(proven_net_conn_t *conn);
```

`proven_net_connect_start`는 곧바로 돌아온다: 연결이 이미 되었으면 `PROVEN_OK`(루프백 인터페이스에서는
흔히 그렇다), 진행 중이면 `PROVEN_ERR_AGAIN`. 뒤의 경우 그 소켓은 지켜보고 닫는 것만 할 수 있다:
`proven_net_poll`이나 셀렉터나 [15장](manual-15-event-loop-ko.md)의 루프로 쓸 수 있다고 보고될 때까지
기다린 다음, `proven_net_connect_finish`에게 어떻게 끝났는지 묻는다. 어느 호출에서든 실패하면 열린 것이
남지 않는다. 15장의 이벤트 구동 클라이언트가 이 쌍 위에 만들어져 있다.

같은 호출들이 **유닉스 도메인** 스트림 소켓에도 쓰인다. `proven_net_addr_unix`로 만든 주소를 넘기면
된다. listen은 경로를 만들고, 리스너를 닫아도 그 경로는 지워지지 않는다. 그래서 서버는 듣기 전에 남아
있는 경로를 지운다.

### 주의사항, 그리고 무엇이 잘못되는가

**read 한 번이 메시지 하나가 아니다.** 스트림은 바이트를 순서대로, 네트워크가 나눈 조각대로 전한다.

잘못된 예 — 요청이 read 한 번에 다 온다고 가정하기:

```text
proven_result_size_t r = proven_net_read(&conn, buf, until);
handle_request(buf, r.value);     /* wrong: this may be half a request, or one and a half */
```

올바른 예 — 프로토콜이 메시지가 완성됐다고 말할 때까지 읽는다. 알려 준 길이, 약속된 구분자, 또는
`PROVEN_ERR_EOF`. 아래 예제는 끝까지 읽는다.

**write 한 번이 전부를 보내지 않을 수 있다.** `proven_net_write`는 얼마나 나갔는지 보고한다. 그 개수를
무시하면 버퍼가 마침 차 있을 때마다 - 곧, 부하가 걸린 운영 환경에서 - 메시지가 조용히 잘린다. 끝까지
보내는 호출은 `proven_net_write_all`이고, 그것이 실패했을 때 다시 보내면 데이터가 중복되는지를 알려 주는
것이 그 개수다.

**reset은 끝이 아니다.** `PROVEN_ERR_EOF`는 상대가 끝났다고 말했고 보낸 것을 모두 읽었다는 뜻이다.
`PROVEN_ERR_RESET`은 상대가 말없이 사라졌고, 전송 중이던 것의 일부가 - 어느 방향이든 - 없어졌을 수
있다는 뜻이다. `EOF`로 끝난 파일 전송은 완전하다. `RESET`으로 끝난 것은 몇 바이트가 도착했든 완전하지
않다.

**닫는 것과 끝내는 것은 다르다.** 상대가 전부 받았는지 확실히 하려면, `proven_net_shutdown_write`로
끝났다고 말하고, `EOF`까지 읽고, 그다음에 닫는다. 읽지 않은 데이터가 남은 연결을 닫는 것이 바로
*상대편*이 `PROVEN_ERR_RESET`을 받게 되는 경로다.

테스트 스위트가 컴파일하고 실행한다:

<!-- example: manual/examples/ko/ex_09_net_tcp.c -->
```c
/*
 * 처음부터 끝까지의 TCP 교환. 기다릴 수 있는 모든 호출에 기한이 붙는다.
 *
 * 양쪽 끝이 이 프로그램 하나에 산다. 커널은 리스너의 백로그가 연결을 받는 순간 - accept가
 * 불리기 전에 - 연결을 완성하므로 이것이 가능하다. 실제 클라이언트와 서버는 같은 호출들을
 * 두 프로세스에 나눠 놓은 것이다.
 */

int main(void) {
    /* 포트 0: OS가 빈 포트를 고르고, `at`이 어느 것인지 알려 준다. 루프백: 이 기계만. */
    proven_net_listener_t listener;
    proven_net_addr_t at;
    proven_err_t err = proven_net_listen(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), 16, &listener, &at);
    if (err == PROVEN_ERR_PERMISSION || err == PROVEN_ERR_UNSUPPORTED) {
        printf("no sockets in this environment; nothing to show\n");
        return EXAMPLE_OK();
    }
    EXAMPLE_REQUIRE(err == PROVEN_OK && proven_net_listener_is_open(&listener), "the listener opens");
    EXAMPLE_REQUIRE(at.port != 0, "and reports the port it was given");

    /* 교환 전체에 기한 하나: 호출이 몇 번이 되든 지금부터 5초. */
    proven_net_deadline_t until = proven_net_deadline_in(5000);

    proven_net_conn_t client, server;
    proven_net_addr_t peer;
    EXAMPLE_REQUIRE(proven_net_connect(at, until, &client) == PROVEN_OK, "the client connects");
    EXAMPLE_REQUIRE(proven_net_accept(&listener, until, &server, &peer) == PROVEN_OK, "the server accepts");
    EXAMPLE_REQUIRE(proven_net_conn_is_open(&client) && proven_net_conn_is_open(&server), "two open ends");

    /* 양쪽 끝이 두 주소를 다 안다. accept가 준 상대 주소는 클라이언트 자신의 로컬 주소다. */
    proven_net_addr_t client_local, client_peer, listening;
    EXAMPLE_REQUIRE(proven_net_conn_local_addr(&client, &client_local) == PROVEN_OK &&
                    proven_net_conn_peer_addr(&client, &client_peer) == PROVEN_OK, "the client's two addresses");
    EXAMPLE_REQUIRE(proven_net_addr_eq(&client_local, &peer), "the server saw the client's address");
    EXAMPLE_REQUIRE(proven_net_listener_addr(&listener, &listening) == PROVEN_OK &&
                    proven_net_addr_eq(&client_peer, &listening), "the client's peer is where the server listens");

    /* 작은 조각 둘로 쓰는 요청: 이것이 없으면 둘이 함께 나가려고 기다릴 수 있다. */
    EXAMPLE_REQUIRE(proven_net_conn_set_nodelay(&client, true) == PROVEN_OK, "small writes go at once");

    /* write_all은 전부 보내거나, 어디까지 갔는지 말한다. */
    proven_mem_view_t request = proven_mem_view_from_u8(PROVEN_LIT("what time is it?"));
    proven_result_size_t sent = proven_net_write_all(&client, request, until);
    EXAMPLE_REQUIRE(sent.err == PROVEN_OK && sent.value == request.size, "the whole request went");

    /* "더 보낼 것이 없다." 서버는 요청을 읽고, 그다음 입력의 끝을 읽는다. */
    EXAMPLE_REQUIRE(proven_net_shutdown_write(&client) == PROVEN_OK, "the client half-closes");

    /* 서버는 끝까지 읽는다. read는 도착한 만큼을 돌려준다 - 스트림에는 메시지 경계가
     * 없다 - 그러니 "전부"를 읽는 올바른 방법은 루프뿐이다. */
    proven_byte_t buf[64];
    proven_size_t got = 0;
    for (;;) {
        proven_result_size_t r = proven_net_read(&server, (proven_mem_mut_t){ buf + got, sizeof buf - got }, until);
        if (r.err == PROVEN_ERR_EOF) break;                  /* 클라이언트가 끝냈다 */
        EXAMPLE_REQUIRE(r.err == PROVEN_OK, "a read that is not the end succeeds");
        if (r.err != PROVEN_OK) break;
        got += r.value;
    }
    EXAMPLE_REQUIRE(got == request.size, "the server received the whole request");

    /* write 한 번은 일부만 보낼 수 있고, 개수가 얼마인지 말한다. 여기서는 전부 들어간다. */
    proven_mem_view_t reply = proven_mem_view_from_u8(PROVEN_LIT("time to close"));
    proven_result_size_t w = proven_net_write(&server, reply, until);
    EXAMPLE_REQUIRE(w.err == PROVEN_OK && w.value == reply.size, "a short reply goes in one write");

    proven_result_size_t r = proven_net_read(&client, (proven_mem_mut_t){ buf, sizeof buf }, until);
    EXAMPLE_REQUIRE(r.err == PROVEN_OK && r.value == reply.size, "the client reads the reply after its half-close");

    /* 더 오는 것이 없고, 클라이언트는 그것을 알아내려고 영원히 기다리지 않는다. 타임아웃은
     * 손상이 아니다. 연결은 전과 똑같이 쓸 수 있다. */
    r = proven_net_read(&client, (proven_mem_mut_t){ buf, sizeof buf }, proven_net_deadline_in(50));
    EXAMPLE_REQUIRE(r.err == PROVEN_ERR_TIMEOUT, "silence is a timeout, not a hang");

    /* 모든 소켓은 그 주인이 닫는다. */
    EXAMPLE_REQUIRE(proven_net_close(&server) == PROVEN_OK, "the server closes its end");
    r = proven_net_read(&client, (proven_mem_mut_t){ buf, sizeof buf }, until);
    EXAMPLE_REQUIRE(r.err == PROVEN_ERR_EOF, "which the client reads as the end");
    EXAMPLE_REQUIRE(proven_net_close(&client) == PROVEN_OK, "the client closes");
    EXAMPLE_REQUIRE(proven_net_listener_close(&listener) == PROVEN_OK, "the listener closes");

    /* 리스너가 사라지면 같은 주소가 거절한다 - 타임아웃과는 다른 답이다. */
    proven_net_conn_t nobody;
    EXAMPLE_REQUIRE(proven_net_connect(at, proven_net_deadline_in(15000), &nobody) == PROVEN_ERR_REFUSED,
                    "nothing listening is PROVEN_ERR_REFUSED");

    return EXAMPLE_OK();
}
```

## 5. UDP

UDP는 메시지 - 데이터그램 - 를 하나씩, 각각 따로 보낸다. 데이터그램은 온전히 도착하거나 아예 도착하지
않는다. 순서가 바뀌거나, 두 번 오거나, 영영 오지 않을 수 있고, 어느 쪽인지 보낸 쪽에 알려 주는 것은
없다.

### 참조

| API | 의도 | 반환 |
|---|---|---|
| `proven_net_udp_open(at, &udp, &bound)` | `at`에 바인드된 소켓을 연다. 클라이언트는 `proven_net_addr_any(family, 0)`에 바인드한다. | `proven_err_t`: `BUSY`, `PERMISSION`, `UNSUPPORTED`(유닉스 도메인 주소). |
| `proven_net_udp_send_to(&udp, to, data, until)` | 데이터그램 하나를 보낸다: `data` 전부 아니면 아무것도. | `proven_err_t`: `data`가 데이터그램이 될 수 있는 크기를 넘으면 `OUT_OF_BOUNDS`. |
| `proven_net_udp_recv_from(&udp, dest, &from, until)` | 데이터그램 하나를 받는다. `from`은 선택이다. | `proven_result_size_t`: 잘렸으면 `OUT_OF_BOUNDS`와 `.value == dest.size`. `TIMEOUT`. |
| `proven_net_udp_addr(&udp, &out)` | 소켓이 바인드된 곳. | `proven_err_t`. |
| `proven_net_udp_close(&udp)` | 닫는다. 열려 있지 않은 소켓에 불러도 안전하다. | `proven_err_t`. |
| `proven_net_udp_is_open(&udp)` | 값이 열린 소켓을 들고 있는가. | `bool`. |

### 주의사항, 그리고 무엇이 잘못되는가

**"보냈다"는 떠났다는 뜻이지 도착했다는 뜻이 아니다.** `proven_net_udp_send_to`의 성공은 데이터그램을
네트워크에 넘겼다는 것이다. 답이 필요한 프로토콜은 기한을 두고 답을 기다리고, 오지 않으면 다시 보낸다.

**길이 0인 데이터그램도 메시지다.** 스트림에서 0바이트는 끝을 뜻할 것이고, 그래서 `proven_net_read`는
그것을 돌려주지 않는다. 데이터그램은 정말로 비어 있을 수 있으므로, `proven_net_udp_recv_from`이
`.value` 0으로 성공하는 것은 끝도 아니고 오류도 아니다.

**너무 작은 버퍼는 나머지를 잃는다.** 데이터그램에서 들어가지 않은 부분은 다음 받기를 위해 남겨지지
않는다. 사라진다.

잘못된 예 — 어림으로 잡은 버퍼, 그리고 무시한 오류:

```text
proven_byte_t buf[64];
proven_result_size_t r = proven_net_udp_recv_from(&udp, buf_mem, &from, until);
parse(buf, r.value);      /* wrong: on OUT_OF_BOUNDS this is the first 64 bytes of something longer */
```

올바른 예 — 프로토콜이 허용하는 가장 큰 메시지에 맞춰 버퍼를 잡고, `PROVEN_ERR_OUT_OF_BOUNDS`는 잘못된
패킷으로 취급한다.

테스트 스위트가 컴파일하고 실행한다:

<!-- example: manual/examples/ko/ex_09_net_udp.c -->
```c
/*
 * UDP: 스트림이 아니라 메시지다. 보내기 한 번이 데이터그램 하나이고 받기 한 번이 데이터그램
 * 하나이며, 보낸 쪽 주소가 붙어 온다 - 그리고 도착한다는 약속은 없다.
 */

int main(void) {
    proven_net_udp_t server, client;
    proven_net_addr_t server_at, client_at;
    proven_err_t err = proven_net_udp_open(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &server, &server_at);
    if (err == PROVEN_ERR_PERMISSION || err == PROVEN_ERR_UNSUPPORTED) {
        printf("no sockets in this environment; nothing to show\n");
        return EXAMPLE_OK();
    }
    EXAMPLE_REQUIRE(err == PROVEN_OK && proven_net_udp_is_open(&server), "the server's socket opens");
    EXAMPLE_REQUIRE(proven_net_udp_open(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &client, NULL) == PROVEN_OK,
                    "and the client's");
    EXAMPLE_REQUIRE(proven_net_udp_addr(&client, &client_at) == PROVEN_OK && client_at.port != 0,
                    "a socket can be asked where it is bound");

    proven_net_deadline_t until = proven_net_deadline_in(2000);

    /* 보내기 둘은 데이터그램 둘이다. 성공은 각각이 이 기계를 떠났다는 뜻이다. */
    EXAMPLE_REQUIRE(proven_net_udp_send_to(&client, server_at, proven_mem_view_from_u8(PROVEN_LIT("first")), until) == PROVEN_OK &&
                    proven_net_udp_send_to(&client, server_at, proven_mem_view_from_u8(PROVEN_LIT("second message")), until) == PROVEN_OK,
                    "two datagrams leave");

    /* 받기 한 번은 온전한 데이터그램 하나이고, 누가 보냈는지 말해 준다. */
    proven_byte_t buf[32];
    proven_net_addr_t from;
    proven_result_size_t r = proven_net_udp_recv_from(&server, (proven_mem_mut_t){ buf, sizeof buf }, &from, until);
    EXAMPLE_REQUIRE(r.err == PROVEN_OK && r.value == 5, "the first datagram, alone: five bytes");
    EXAMPLE_REQUIRE(proven_net_addr_eq(&from, &client_at), "from the client's address");

    /* 너무 작은 버퍼는 데이터그램을 자르고, 잘랐다고 말한다. 나머지는 사라진다 - 다음
     * 받기는 다음 데이터그램이다 - 그러니 버퍼는 가장 큰 메시지에 맞춰라. */
    proven_byte_t small[6];
    r = proven_net_udp_recv_from(&server, (proven_mem_mut_t){ small, sizeof small }, &from, until);
    EXAMPLE_REQUIRE(r.err == PROVEN_ERR_OUT_OF_BOUNDS && r.value == sizeof small,
                    "fourteen bytes into six: PROVEN_ERR_OUT_OF_BOUNDS, six kept");

    /* 답장은 데이터그램이 온 주소로 간다. */
    EXAMPLE_REQUIRE(proven_net_udp_send_to(&server, from, proven_mem_view_from_u8(PROVEN_LIT("ack")), until) == PROVEN_OK,
                    "the server answers the sender");
    r = proven_net_udp_recv_from(&client, (proven_mem_mut_t){ buf, sizeof buf }, NULL, until);
    EXAMPLE_REQUIRE(r.err == PROVEN_OK && r.value == 3, "and the client receives it");

    /* 더 오는 것이 없다. 잃어버린 데이터그램도 정확히 이렇게 보인다. 모든 받기에 기한이
     * 있고 모든 UDP 프로토콜에 재시도가 있는 이유다. */
    r = proven_net_udp_recv_from(&client, (proven_mem_mut_t){ buf, sizeof buf }, NULL, proven_net_deadline_in(50));
    EXAMPLE_REQUIRE(r.err == PROVEN_ERR_TIMEOUT, "silence is a timeout");

    EXAMPLE_REQUIRE(proven_net_udp_close(&client) == PROVEN_OK && proven_net_udp_close(&server) == PROVEN_OK,
                    "both sockets are closed by their owner");
    return EXAMPLE_OK();
}
```

## 6. 소켓 여럿, 스레드 하나

기한은 소켓 하나에 대한 기다림을 묶는다. 서버는 자기 소켓 전부를 한꺼번에 기다리다가 손이 필요한 것을
처리한다. 그것이 `proven_net_poll`이다.

### 참조

| API | 의도 | 반환 |
|---|---|---|
| `proven_net_listener_handle(&l)`, `proven_net_conn_handle(&c)`, `proven_net_udp_handle(&u)` | 종류가 무엇이든, poll이 보는 소켓. | `proven_net_handle_t`. 열려 있지 않은 소켓이면 `valid`가 아니다. |
| `proven_net_poll(items, count, until, &ready)` | 적어도 하나가 준비될 때까지 기다린다. `PROVEN_NET_POLL_INLINE_MAX`(64)개까지. | `proven_err_t`: 아무것도 준비되지 않았으면 `TIMEOUT`(`ready`는 0). 항목이 너무 많으면 `OUT_OF_BOUNDS`. 핸들이 유효하지 않으면 `INVALID_ARG`. |
| `proven_net_poll_with(scratch, items, count, until, &ready)` | 여러분이 준 메모리로, 개수 제한 없이 같은 일을 한다. | `proven_err_t`. `scratch`가 작으면 `OUT_OF_BOUNDS`. |
| `proven_net_poll_scratch_size(count)` | 항목 `count`개에 필요한 임시 작업용(scratch) 메모리 크기. | `proven_size_t`. 표현할 수 없으면 `SIZE_MAX`. |

```text
typedef struct {
    proven_net_handle_t handle;
    proven_u8 want;   /* PROVEN_NET_READABLE and/or PROVEN_NET_WRITABLE */
    proven_u8 got;    /* filled in: zero, or what is ready - which may include PROVEN_NET_FAILED */
} proven_net_poll_item_t;
```

**준비됨은 "기다리지 않는다"는 뜻이지 "성공한다"는 뜻이 아니다.** 읽을 수 있는 연결에는 데이터가, 또는
끝이, 또는 오류가 있을 수 있다. 읽을 수 있는 리스너에는 받을 연결이 있다. 그래서 준비 보고 다음의
호출은 `PROVEN_NET_DONT_WAIT`를 받고, 그 결과에 따라 행동한다. `PROVEN_NET_FAILED`로 보고된 항목은
오류 상태이거나 끊긴 것이고, 다음 read나 write가 어느 쪽인지 말해 준다.

**쓸 것이 있을 때에만 쓰기 가능을 물어라.** 한가한 연결은 거의 언제나 쓸 수 있으므로, 언제나 그것을 묻는
루프는 매번 즉시 깨어나 아무 일도 하지 않으며 프로세서를 돌린다.

잘못된 예:

```text
items[i].want = PROVEN_NET_READABLE | PROVEN_NET_WRITABLE;   /* wrong when nothing is queued: never sleeps */
```

올바른 예 — `PROVEN_NET_WRITABLE`은 마지막 write가 요청보다 적게 보냈거나 타임아웃된 연결에만, 그리고
그 데이터가 나갈 때까지만 건다.

`proven_net_poll_with`가 있는 이유는 운영체제가 자기 배열을 원하고 이 라이브러리는 여러분 몰래 그것을
할당하지 않기 때문이다. `proven_net_poll_scratch_size`로 크기를 잰 작업 메모리를 주고, 다음 호출에 다시
쓴다. 여기서의 준비 상태는 POSIX의 `poll`과 Windows의 `WSAPoll`이다. 소켓 수십 개나 수백 개에 맞는
도구다. 한 번에 바쁜 것이 적은 수천 개에는 §9의 selector를 써라.

테스트 스위트가 컴파일하고 실행한다:

<!-- example: manual/examples/ko/ex_09_net_poll.c -->
```c
/*
 * 스레드 하나, 소켓 여럿: 그중 하나라도 손이 필요해질 때까지 기다렸다가, 필요한 것들을
 * 처리한다. 모든 단일 스레드 서버 안에 있는 루프다.
 *
 * "준비됨"은 그 종류의 다음 호출이 기다리지 않는다는 뜻이다. 그래서 준비됐다고 보고된
 * 소켓은 PROVEN_NET_DONT_WAIT로 다루고, 루프는 poll 말고는 어디서도 블록하지 않는다.
 */

int main(void) {
    proven_net_listener_t listener;
    proven_net_addr_t at;
    proven_err_t err = proven_net_listen(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), 16, &listener, &at);
    if (err == PROVEN_ERR_PERMISSION || err == PROVEN_ERR_UNSUPPORTED) {
        printf("no sockets in this environment; nothing to show\n");
        return EXAMPLE_OK();
    }
    EXAMPLE_REQUIRE(err == PROVEN_OK, "the listener opens");

    /* 클라이언트 둘이 연결하고 각자 메시지 하나를 보낸다. */
    proven_net_conn_t clients[2];
    for (int i = 0; i < 2; ++i) {
        EXAMPLE_REQUIRE(proven_net_connect(at, proven_net_deadline_in(5000), &clients[i]) == PROVEN_OK, "a client connects");
    }
    EXAMPLE_REQUIRE(proven_net_write_all(&clients[0], proven_mem_view_from_u8(PROVEN_LIT("from the first")), proven_net_deadline_in(5000)).err == PROVEN_OK &&
                    proven_net_write_all(&clients[1], proven_mem_view_from_u8(PROVEN_LIT("from the second")), proven_net_deadline_in(5000)).err == PROVEN_OK,
                    "each sends a message");

    /* 서버: 리스너 하나와 연결 최대 둘을 목록 하나에. */
    proven_net_conn_t served[2] = {0};
    proven_size_t served_count = 0;
    proven_size_t bytes_seen = 0;

    proven_net_deadline_t until = proven_net_deadline_in(5000);
    while (bytes_seen < 14 + 15) {
        proven_net_poll_item_t items[3];
        proven_size_t count = 0;
        items[count++] = (proven_net_poll_item_t){ .handle = proven_net_listener_handle(&listener), .want = PROVEN_NET_READABLE };
        for (proven_size_t i = 0; i < served_count; ++i) {
            items[count++] = (proven_net_poll_item_t){ .handle = proven_net_conn_handle(&served[i]), .want = PROVEN_NET_READABLE };
        }

        proven_size_t ready = 0;
        err = proven_net_poll(items, count, until, &ready);
        EXAMPLE_REQUIRE(err == PROVEN_OK && ready > 0, "something becomes ready before the deadline");
        if (err != PROVEN_OK) break;

        /* 읽을 수 있는 리스너에는 기다리는 연결이 있다. */
        if (items[0].got & PROVEN_NET_READABLE) {
            EXAMPLE_REQUIRE(served_count < 2 &&
                            proven_net_accept(&listener, PROVEN_NET_DONT_WAIT, &served[served_count], NULL) == PROVEN_OK,
                            "the accept that poll predicted does not wait");
            served_count++;
        }
        /* 읽을 수 있는 연결에는 데이터가, 아니면 끝이 있다. `items[1 + i]`는 `served[i]`에서 만들었다. */
        for (proven_size_t i = 1; i < count; ++i) {
            if (!items[i].got) continue;
            proven_byte_t buf[64];
            proven_result_size_t r = proven_net_read(&served[i - 1], (proven_mem_mut_t){ buf, sizeof buf }, PROVEN_NET_DONT_WAIT);
            EXAMPLE_REQUIRE(r.err == PROVEN_OK, "the read that poll predicted does not wait");
            bytes_seen += r.value;
        }
    }
    EXAMPLE_REQUIRE(served_count == 2 && bytes_seen == 29, "both clients were accepted and both messages read, in one thread");

    /* proven_net_poll은 PROVEN_NET_POLL_INLINE_MAX개까지 받는다. 그보다 많은 서버는 자기
     * 작업 메모리를 넘긴다. OS는 배열을 원하고, 이 라이브러리는 그것을 할당하지 않는다. */
    proven_net_poll_item_t one = { .handle = proven_net_conn_handle(&served[0]), .want = PROVEN_NET_WRITABLE };
    proven_byte_t scratch[256];
    proven_size_t ready = 0;
    EXAMPLE_REQUIRE(proven_net_poll_scratch_size(1) <= sizeof scratch, "the scratch for one item is small");
    EXAMPLE_REQUIRE(proven_net_poll_with((proven_mem_mut_t){ scratch, sizeof scratch }, &one, 1, PROVEN_NET_DONT_WAIT, &ready) == PROVEN_OK &&
                    (one.got & PROVEN_NET_WRITABLE), "an idle connection is writable at once");

    /* UDP 소켓도 다른 것들과 같은 목록에서 기다린다. */
    proven_net_udp_t udp;
    EXAMPLE_REQUIRE(proven_net_udp_open(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), &udp, NULL) == PROVEN_OK, "a UDP socket");
    proven_net_poll_item_t quiet = { .handle = proven_net_udp_handle(&udp), .want = PROVEN_NET_READABLE };
    EXAMPLE_REQUIRE(proven_net_poll(&quiet, 1, proven_net_deadline_in(30), &ready) == PROVEN_ERR_TIMEOUT && ready == 0,
                    "nothing arrives on it: PROVEN_ERR_TIMEOUT, the idle tick of an event loop");
    (void)proven_net_udp_close(&udp);

    for (int i = 0; i < 2; ++i) {
        (void)proven_net_close(&clients[i]);
        (void)proven_net_close(&served[i]);
    }
    (void)proven_net_listener_close(&listener);
    return EXAMPLE_OK();
}
```

## 7. 전송

프로토콜 - 줄 단위 명령 집합, HTTP 교환 - 은 한 번만 쓰여야 하고, 자기 바이트를 무엇이 실어 나르는지
몰라야 한다. 오늘 그것은 TCP 연결이다. TLS 아래에서는 그 위의 암호화된 세션이다. 테스트에서는 메모리
버퍼 둘이고 네트워크는 없다. `proven_transport_t`가 그 이음매다.

```text
typedef struct {
    void *ctx;
    proven_result_size_t (*read_fn)(void *ctx, proven_mem_mut_t dest, proven_net_deadline_t until);
    proven_result_size_t (*write_fn)(void *ctx, proven_mem_view_t src, proven_net_deadline_t until);
    proven_err_t (*shutdown_fn)(void *ctx);   /* may be NULL */
    proven_err_t (*close_fn)(void *ctx);      /* may be NULL */
} proven_transport_t;
```

값으로 넘기는 작은 vtable로, `proven_allocator_t`([2장](manual-02-allocation-ko.md))와 `proven_writer_t`와
같은 모양이고 이유도 같다. 호출자가 정하고, 숨기는 것이 없다. 직접 만들려면 구조체를 채우면 된다.
계약은 TCP 호출들의 것이다. read는 0바이트로 성공하지 않고, write는 실패하더라도 나간 바이트를
보고한다.

### 참조

| API | 의도 | 반환 |
|---|---|---|
| `proven_net_conn_transport(&conn)` | 전송으로서의 연결. 전송을 닫으면 연결이 닫힌다. | `proven_transport_t`. |
| `proven_transport_is_valid(t)` | read와 write 함수가 있는가. | `bool`. |
| `proven_transport_read(t, dest, until)`, `proven_transport_write(t, src, until)` | `proven_net_read`, `proven_net_write`와 같다. | `proven_result_size_t`. 유효하지 않은 전송이면 `INVALID_ARG`. |
| `proven_transport_write_all(t, src, until)` | 전부 쓰거나, 개수와 함께 실패한다. 아무것도 받지 않으면서 오류도 내지 않는 전송은 끝없는 루프가 아니라 `PROVEN_ERR_IO`다. | `proven_result_size_t`. |
| `proven_transport_shutdown(t)`, `proven_transport_close(t)` | 보내는 쪽을 끝낸다. 닫는다. 전송에 할 일이 없으면 `PROVEN_OK`. | `proven_err_t`. |
| `proven_transport_reader(&state, t, timeout_ms)` | 전송 위의 `proven_reader_t`. 줄 reader를 비롯해 스트림을 읽는 모든 것에 쓴다. | `proven_reader_t`. `state`가 NULL이거나 `t`가 유효하지 않으면 유효하지 않다. |
| `proven_transport_writer(&state, t, timeout_ms)` | 전송 위의 `proven_writer_t`. `proven_fprint`가 보낼 수 있게 된다. | `proven_writer_t`. |

reader와 writer 어댑터는 소켓을
[5장의 스트림 절](manual-05-hosted-services-ko.md#스트림-writer와-reader)에 있는 모든 것과 잇는다. reader에는
기한을 넘길 자리가 없으므로, 어댑터를 거치는 read나 write는 각자의 기한을 받는다. 시작한 순간부터
`timeout_ms`, 그 값이 0이면 기한 없음이다. `proven_transport_stream_t` 상태는
[호출자 소유](manual-00-start-here-ko.md#92-caller-owned-state--destroy-없음-복사-금지)다. reader나 writer보다
오래 살아야 하고 움직여서는 안 된다.

writer는 조각을 받는 대로 보낸다. 메시지를 여러 작은 조각으로 포맷한다면 `proven_writer_buffered`로
감싸서 적은 수의 패킷으로 나가게 하고, 답을 기다리기 전에 flush하라.

테스트 스위트가 컴파일하고 실행한다:

<!-- example: manual/examples/ko/ex_09_net_transport.c -->
```c
/*
 * proven_transport_t에 대고 한 번 쓴 프로토콜을 TCP 연결 위에서 돌린다.
 *
 * 아래 함수는 자기 바이트를 무엇이 실어 나르는지 모른다. 오늘은 소켓이고, TLS 아래에서는
 * 소켓 위의 암호화된 세션이 되며, 테스트에서는 메모리 버퍼 둘일 수 있다. 이 인터페이스가
 * 있는 이유가 그것이다.
 */

/* 요청 한 줄을 보내고 답 한 줄을 읽는다. 걸음마다 5초 제한. */
static proven_err_t ask(proven_transport_t t, const char *question, proven_u8str_view_t expected) {
    if (!proven_transport_is_valid(t)) return PROVEN_ERR_INVALID_ARG;

    /* 포매터가 전송에 곧바로 쓴다. */
    proven_transport_stream_t out_state;
    proven_writer_t out = proven_transport_writer(&out_state, t, 5000);
    proven_err_t err = proven_fprintln(out, "ASK {}", PROVEN_ARG(question)).err;
    if (err != PROVEN_OK) return err;

    /* 줄 reader가 거기서 읽어, 이 함수가 가진 버퍼에 담는다. */
    proven_transport_stream_t in_state;
    proven_byte_t line_buf[128];
    proven_reader_buffered_t lines;
    (void)proven_reader_buffered(&lines, proven_transport_reader(&in_state, t, 5000),
                                 (proven_mem_mut_t){ line_buf, sizeof line_buf });
    proven_result_u8str_view_t reply = proven_reader_read_line(&lines);
    if (reply.err != PROVEN_OK) return reply.err;
    return proven_u8str_view_eq(reply.val, expected) ? PROVEN_OK : PROVEN_ERR_INVALID_FORMAT;
}

int main(void) {
    proven_net_listener_t listener;
    proven_net_addr_t at;
    proven_err_t err = proven_net_listen(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), 4, &listener, &at);
    if (err == PROVEN_ERR_PERMISSION || err == PROVEN_ERR_UNSUPPORTED) {
        printf("no sockets in this environment; nothing to show\n");
        return EXAMPLE_OK();
    }
    EXAMPLE_REQUIRE(err == PROVEN_OK, "the listener opens");

    proven_net_conn_t client_conn, server_conn;
    EXAMPLE_REQUIRE(proven_net_connect(at, proven_net_deadline_in(5000), &client_conn) == PROVEN_OK &&
                    proven_net_accept(&listener, proven_net_deadline_in(5000), &server_conn, NULL) == PROVEN_OK,
                    "a connected pair");

    /* 전송으로서의 연결. 여기서부터는 어느 쪽도 소켓을 입에 올리지 않는다. */
    proven_transport_t client = proven_net_conn_transport(&client_conn);
    proven_transport_t server = proven_net_conn_transport(&server_conn);
    proven_net_deadline_t until = proven_net_deadline_in(5000);

    /* 서버 쪽 절반을 손으로 한다. 답을 클라이언트가 묻기 전에 써 둔다. 스레드 하나에서는
     * 괜찮다. 바이트가 연결 안에서 기다릴 뿐이다. */
    proven_result_size_t w = proven_transport_write_all(server, proven_mem_view_from_u8(PROVEN_LIT("ANSWER 42\n")), until);
    EXAMPLE_REQUIRE(w.err == PROVEN_OK && w.value == 10, "the server's reply is on its way");

    EXAMPLE_REQUIRE(ask(client, "the question", PROVEN_LIT("ANSWER 42")) == PROVEN_OK,
                    "the protocol function sends its line and reads the reply");

    /* 그리고 서버는 클라이언트가 쓴 것을 원시 호출로 읽는다. */
    proven_byte_t buf[64];
    proven_result_size_t r = proven_transport_read(server, (proven_mem_mut_t){ buf, sizeof buf }, until);
    EXAMPLE_REQUIRE(r.err == PROVEN_OK &&
                    proven_u8str_view_eq((proven_u8str_view_t){ buf, r.value }, PROVEN_LIT("ASK the question\n")),
                    "the request line arrived as written");

    /* write 한 번은 얼마나 갔는지 보고한다. 여기서는 전부다. */
    w = proven_transport_write(server, proven_mem_view_from_u8(PROVEN_LIT("BYE\n")), until);
    EXAMPLE_REQUIRE(w.err == PROVEN_OK && w.value == 4, "one more line");

    /* 끝내기와 닫기도 같은 인터페이스를 거친다. */
    EXAMPLE_REQUIRE(proven_transport_shutdown(server) == PROVEN_OK, "the server ends its sending side");
    r = proven_transport_read(client, (proven_mem_mut_t){ buf, sizeof buf }, until);
    EXAMPLE_REQUIRE(r.err == PROVEN_OK && r.value == 4, "the client reads the last line");
    r = proven_transport_read(client, (proven_mem_mut_t){ buf, sizeof buf }, until);
    EXAMPLE_REQUIRE(r.err == PROVEN_ERR_EOF, "and then the end");

    EXAMPLE_REQUIRE(proven_transport_close(client) == PROVEN_OK && proven_transport_close(server) == PROVEN_OK,
                    "closing a transport closes what carries it");
    EXAMPLE_REQUIRE(!proven_net_conn_is_open(&client_conn), "the connection underneath is closed");
    (void)proven_net_listener_close(&listener);
    return EXAMPLE_OK();
}
```

## 8. 짝, 그리고 루프 깨우기

`proven_net_poll`에 막혀 있는 루프에게는 소켓만 보이고 다른 것은 보이지 않는다. 그 루프를 위해 어떤
작업을 끝낸 스레드나 그 루프를 멈추고 싶은 스레드는 변수로는 루프에 닿을 수 없다. 루프는 운영체제
안에서 잠들어 있기 때문이다. 필요한 것은 다른 스레드가 말할 때 읽을 수 있게 되는 소켓이다. 그것이
깨우개(waker)다.

### 참조

| API | 의도 | 반환 |
|---|---|---|
| `proven_net_pair(&a, &b)` | 주소 없이 서로 이어진 두 연결. 한쪽에 쓴 것이 다른 쪽에서 읽힌다. | `proven_err_t`: 시스템에 소켓이 모자라면 `BUSY`. 둘 다 닫아야 한다. |
| `proven_net_waker_open(&waker)` | 깨우개를 연다. | `proven_err_t`: `BUSY`. |
| `proven_net_waker_handle(&waker)` | poll 목록에 `PROVEN_NET_READABLE`을 원한다고 넣을 핸들. | `proven_net_handle_t`. |
| `proven_net_waker_wake(&waker)` | 읽을 수 있게 만든다. 어느 스레드에서 불러도, 여러 스레드가 동시에 불러도 안전하다. | 없음. 호출자가 대응할 수 있는 방식으로는 실패하지 않는다. |
| `proven_net_waker_drain(&waker)` | 지금까지의 깨우기를 읽어 없앤다. poll하는 스레드가 깨어난 뒤에 부른다. | 없음. |
| `proven_net_waker_close(&waker)` | 닫는다. | 없음. |

**깨어난 뒤에는 비워라.** 깨우개는 비울 때까지 읽을 수 있는 상태로 남는다. 이것을 잊은 루프는 poll하고,
곧장 돌아오고, poll하고, 곧장 돌아오며 프로세서를 헛돌린다.

**깨우기는 합쳐진다.** 비우기 한 번 전에 `proven_net_waker_wake`를 열 번 불러도 깨우기는 한 번이다.
깨우개는 "보라"고 말할 뿐 몇 번인지는 말하지 않는다. 그러니 루프가 봐야 할 것은 다른 곳 - 큐, 플래그 -
에 두고, 루프는 깨어날 때마다 그것을 확인한다.

**플래그를 세운 다음 깨워라. 그리고 비운 다음에 보라. 비우기 전이 아니다.** 순서가 반대면 보는 것과
비우는 것 사이에 깨우기가 끼어들어, 보이지 않은 채로 비워질 수 있다.

잘못된 예:

```text
if (work_pending()) do_work();        /* wrong: work queued after this line... */
proven_net_waker_drain(&waker);       /* ...has its wake drained here, and waits for the next poll to time out */
```

올바른 예:

```text
proven_net_waker_drain(&waker);
if (work_pending()) do_work();
```

**맨 나중에 닫아라.** 다른 스레드가 아직 `proven_net_waker_wake`를 부를 수 있는 동안에는
`proven_net_waker_close`가 안전하지 않다. 그 스레드들을 먼저 멈추거나 join하라.

`proven_net_pair`는 깨우개를 이루는 재료이고, 테스트나 프로그램이 네트워크 없이 연결 하나를 원하는
곳이면 어디서나 혼자서도 쓸모가 있다. POSIX에서는 유닉스 도메인 짝이고, Windows에서는 loopback TCP
연결이다. 지원하는 모든 Windows 버전에 있는 것이 그것뿐이기 때문이다.

테스트 스위트가 컴파일하고 실행한다:

<!-- example: manual/examples/ko/ex_09_net_waker.c -->
```c
/*
 * proven_net_poll에 막혀 있는 루프에게는 소켓만 보인다. 다른 스레드에서 그 루프에게
 * 무언가를 알리려면 - "할 일이 생겼다", "멈춰라" - 여러분이 말할 때 읽을 수 있게 되는
 * 소켓이 필요하다. 그것이 깨우개(waker)다.
 *
 * proven_net_pair는 깨우개를 이루는 재료이고 그 자체로도 쓸모가 있다: 주소 없이 서로
 * 이어진 두 끝으로, 프로그램의 두 부분 사이에 바이트를 건넬 때 쓴다.
 */

static void finish_work(void *arg) {
    proven_net_waker_t *waker = arg;
    proven_time_sleep(20);              /* 어떤 작업을 대신하는 자리 */
    proven_net_waker_wake(waker);       /* 어느 스레드에서 불러도 된다 */
}

int main(void) {
    /* 짝: 한쪽 끝에 넣은 것이 다른 쪽 끝에서 나온다. */
    proven_net_conn_t a, b;
    proven_err_t err = proven_net_pair(&a, &b);
    if (err == PROVEN_ERR_PERMISSION || err == PROVEN_ERR_UNSUPPORTED) {
        printf("no sockets in this environment; nothing to show\n");
        return EXAMPLE_OK();
    }
    EXAMPLE_REQUIRE(err == PROVEN_OK, "a pair opens");
    EXAMPLE_REQUIRE(proven_net_write_all(&a, proven_mem_view_from_u8(PROVEN_LIT("across")), proven_net_deadline_in(1000)).err == PROVEN_OK, "write to one end");
    proven_byte_t buf[16];
    proven_result_size_t got = proven_net_read(&b, (proven_mem_mut_t){ buf, sizeof buf }, proven_net_deadline_in(1000));
    EXAMPLE_REQUIRE(got.err == PROVEN_OK && got.value == 6, "read from the other");
    (void)proven_net_close(&a);
    (void)proven_net_close(&b);

    /* poll 목록에 든 깨우개. 읽을 것이 없으니 poll은 1초를 꼬박 기다릴 터인데... */
    proven_net_waker_t waker;
    EXAMPLE_REQUIRE(proven_net_waker_open(&waker) == PROVEN_OK, "a waker opens");
    proven_job_sys_t *jobs = NULL;
    EXAMPLE_REQUIRE(proven_job_system_init(proven_heap_allocator(), 1, 4, &jobs) == PROVEN_OK, "a worker thread");
    EXAMPLE_REQUIRE(proven_job_submit_ex(jobs, finish_work, &waker) == PROVEN_OK, "work is handed to it");

    proven_net_poll_item_t item = { .handle = proven_net_waker_handle(&waker), .want = PROVEN_NET_READABLE };
    proven_size_t ready = 0;
    proven_time_t start = proven_time_monotonic_now();
    err = proven_net_poll(&item, 1, proven_net_deadline_in(1000), &ready);
    proven_i64 waited_ms = (proven_time_monotonic_now() - start) / 1000000;
    /* ...작업 스레드가 깨웠다. */
    EXAMPLE_REQUIRE(err == PROVEN_OK && ready == 1 && waited_ms < 900, "the poll returns when the worker says so, not at its deadline");

    /* 비워야 한다. 그러지 않으면 계속 읽을 수 있는 상태로 남아 다음 poll이 아무 일 없이 곧장 돌아온다. */
    proven_net_waker_drain(&waker);
    EXAMPLE_REQUIRE(proven_net_poll(&item, 1, proven_net_deadline_in(20), &ready) == PROVEN_ERR_TIMEOUT, "drained: quiet again");

    /* 비우기 전의 여러 번 깨우기는 한 번의 깨우기다. */
    proven_net_waker_wake(&waker);
    proven_net_waker_wake(&waker);
    proven_net_waker_drain(&waker);
    EXAMPLE_REQUIRE(proven_net_poll(&item, 1, proven_net_deadline_in(20), &ready) == PROVEN_ERR_TIMEOUT, "two wakes, one drain: quiet");

    /* 깨우개는 어떤 스레드도 더는 깨울 수 없을 때에만 닫는다: 작업 스레드를 먼저 멈춘다. */
    proven_job_system_close(jobs);
    proven_job_system_destroy(jobs);
    proven_net_waker_close(&waker);
    return EXAMPLE_OK();
}
```

## 9. selector: 소켓 수천 개

`proven_net_poll`은 호출할 때마다 목록 전체를 건네받고, 시스템은 호출할 때마다 모든 항목을 살핀다. 소켓
수십 개라면 아무것도 아니다. 연결 만 개 가운데 다섯 개가 할 말이 있을 때에는, 그 다섯을 찾으려고 만 개
항목을 복사하고 살핀다 - 루프가 한 바퀴 돌 때마다.

selector는 같은 질문을 거꾸로 묻는다. 소켓을 한 번 등록하면 시스템이 기억하고, 기다리면 준비된 소켓만
돌아오며 나머지는 들여다보지 않는다.

### 참조

| API | 의도 | 반환 |
|---|---|---|
| `proven_net_selector_create(alloc, &selector)` | selector를 만든다. | `proven_err_t`: `INVALID_ARG`. `NOMEM`. 디스크립터가 모자라면 `BUSY`. |
| `proven_net_selector_create_poll(alloc, &selector)` | 시스템이 무엇을 제공하든 이식형 종류로 만든다. | 같다. |
| `proven_net_selector_kind(selector)` | 무엇 위에 지어졌는지. | `PROVEN_NET_SELECTOR_EPOLL`, `_KQUEUE` 또는 `_POLL`. |
| `proven_net_selector_add(selector, handle, want, tag)` | 소켓을 등록한다: 무엇을 지켜볼지, 그리고 이벤트와 함께 돌아올 포인터. | `proven_err_t`: 이미 등록되어 있으면 `EXISTS`. `INVALID_ARG`. `NOMEM`. `BUSY`. |
| `proven_net_selector_modify(selector, handle, want, tag)` | 그 소켓에 묻는 것과 tag를 바꾼다. | `proven_err_t`: `NOT_FOUND`. |
| `proven_net_selector_remove(selector, handle)` | 꺼낸다. | `proven_err_t`: `NOT_FOUND`. |
| `proven_net_selector_wait(selector, events, cap, until, &count)` | 무언가 준비되거나 `until`이 될 때까지 기다리고, `cap`개까지 보고한다. | `proven_err_t`: 아무것도 없었으면 `TIMEOUT`(`count`는 0). |
| `proven_net_selector_count(selector)` | 등록된 소켓 수. | `proven_size_t`. |
| `proven_net_selector_destroy(selector)` | 해제한다. 소켓은 닫지 않는다. | 없음. |

```text
typedef struct {
    void *tag;        /* what you registered with the socket */
    proven_u8 got;    /* PROVEN_NET_READABLE, PROVEN_NET_WRITABLE and/or PROVEN_NET_FAILED */
} proven_net_ready_t;
```

**무엇을 얻는지는 시스템에 달려 있고, selector가 어느 쪽인지 말해 준다.** Linux에서는 `epoll`이고 BSD와
macOS에서는 `kqueue`다. 거기서는 기다림의 비용이 준비된 소켓 수에 비례한다. Windows와 그 밖의 곳에서는
이식형 종류다: 같은 인터페이스를 `proven_net_poll`이 쓰는 호출 위에 얹은 것으로, 기다릴 때마다 여전히
모든 소켓을 살핀다. 동작은 같고 확장성은 같지 않다. `proven_net_selector_kind`가 알려 준다. 수만 연결을
쥘 작정인 서버는 확인해야 한다.

**소켓을 닫기 전에 꺼내라.** 닫힌 소켓의 번호는 다음에 열리는 소켓에게 주어진다. 옛 등록을 아직 쥐고 있는
selector는 여러분이 등록한 것이 아닌 소켓의 이벤트를 - 옛 tag와 함께 - 보고하거나, 이식형 종류에서는
기다릴 때마다 실패를 보고한다.

잘못된 예:

```text
proven_net_close(&conn);                                  /* wrong: the selector still holds its number */
proven_net_selector_remove(selector, proven_net_conn_handle(&conn));   /* too late: the handle is no longer valid */
```

올바른 예 — `proven_net_selector_remove`를 먼저, 그다음 `proven_net_close`.

**준비 상태는 레벨 트리거다.** 소켓은 준비되어 있는 동안 계속 보고된다. `proven_net_poll`과 똑같다:
처리하지 않은 이벤트는 다음 기다림에 다시 온다. 그래서 §6의 규칙이 그대로 통한다 - 이벤트 뒤에는
`PROVEN_NET_DONT_WAIT`를 받는 호출을 하고, 쓸 것이 있는 동안에만 쓰기 가능을 물어라.

**tag는 여러분 것이고, 그 수명도 그렇다.** selector는 받은 포인터를 돌려줄 뿐 그 안을 들여다보지 않는다.
그것이 가리키는 기록이, 그에 대한 이벤트가 이미 selector에서 꺼내졌을 수 있는 동안에 해제되면, 루프의
다음 반복이 해제된 메모리를 읽는다. 이것을 피하는 방식: 이벤트 묶음을 처리하는 도중에 연결을 닫을 때에는
그 기록에 표시만 하고, 묶음이 끝난 뒤에 해제한다.

**스레드 하나.** selector는 여러 스레드가 쓰기에 안전하지 않다. 기다리는 루프가 다른 스레드가 한 일에
반응하게 하려면 깨우개(§8)를 등록하고 깨워라.

**양쪽으로 준비된 소켓은 이벤트 하나일 수도 둘일 수도 있다.** `kqueue`는 읽기 가능과 쓰기 가능을 따로
보고한다. 소켓마다 하나를 기대하지 말고 이벤트 하나하나를 그것이 말하는 대로 처리하라.

테스트 스위트가 컴파일하고 실행한다:

<!-- example: manual/examples/ko/ex_09_net_selector.c -->
```c
/*
 * selector: 소켓을 한 번씩 등록해 두고, 준비된 것에 대해서만 통지받는다.
 *
 * 루프는 poll 예제의 그것이다 - 기다리고, 준비된 것을 기다리지 않고 처리한다 - . 다만 기다림과
 * 기다림 사이에 다시 짓는 것이 없고, 한가한 연결 천 개가 기다림에 아무 비용도 지우지 않는다.
 */

typedef struct {
    proven_net_conn_t conn;
    int number;
    bool open;
} client_t;

int main(void) {
    proven_net_listener_t listener;
    proven_net_addr_t at;
    proven_err_t err = proven_net_listen(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), 16, &listener, &at);
    if (err == PROVEN_ERR_PERMISSION || err == PROVEN_ERR_UNSUPPORTED) {
        printf("no sockets in this environment; nothing to show\n");
        return EXAMPLE_OK();
    }
    EXAMPLE_REQUIRE(err == PROVEN_OK, "the listener opens");

    proven_net_selector_t *selector = NULL;
    EXAMPLE_REQUIRE(proven_net_selector_create(proven_heap_allocator(), &selector) == PROVEN_OK, "a selector");
    /* Linux에서는 epoll, BSD와 macOS에서는 kqueue. 그 밖에서는 poll 위의 같은 인터페이스. */
    proven_net_selector_kind_t kind = proven_net_selector_kind(selector);
    printf("selector kind: %s\n", kind == PROVEN_NET_SELECTOR_EPOLL ? "epoll" : kind == PROVEN_NET_SELECTOR_KQUEUE ? "kqueue" : "poll");

    /* tag는 이벤트와 함께 돌아오는 것이다: 그 소켓에 대한 여러분 자신의 기록을 가리키는 포인터.
     * 여기서 리스너의 tag는 리스너 자신이다. */
    EXAMPLE_REQUIRE(proven_net_selector_add(selector, proven_net_listener_handle(&listener), PROVEN_NET_READABLE, &listener) == PROVEN_OK,
                    "the listener is registered, once");

    /* 클라이언트 셋이 연결하고, 둘째와 셋째가 무언가를 말한다. */
    proven_net_conn_t far[3];
    for (int i = 0; i < 3; ++i) EXAMPLE_REQUIRE(proven_net_connect(at, proven_net_deadline_in(5000), &far[i]) == PROVEN_OK, "a client connects");
    EXAMPLE_REQUIRE(proven_net_write_all(&far[1], proven_mem_view_from_u8(PROVEN_LIT("from one")), proven_net_deadline_in(5000)).err == PROVEN_OK &&
                    proven_net_write_all(&far[2], proven_mem_view_from_u8(PROVEN_LIT("from two")), proven_net_deadline_in(5000)).err == PROVEN_OK, "two of them send");

    client_t clients[3] = {0};
    int accepted = 0, messages = 0;
    for (int round = 0; round < 20 && messages < 2; ++round) {
        proven_net_ready_t ready[8];
        proven_size_t count = 0;
        err = proven_net_selector_wait(selector, ready, 8, proven_net_deadline_in(1000), &count);
        if (err == PROVEN_ERR_TIMEOUT) continue;        /* 루프의 한가한 한 박자 */
        EXAMPLE_REQUIRE(err == PROVEN_OK, "something is ready");
        for (proven_size_t i = 0; i < count; ++i) {
            if (ready[i].tag == &listener) {
                /* 준비됨은 "기다리지 않는다"는 뜻이다: 대기 중인 연결을 기다리지 않고 모두 받는다. */
                while (accepted < 3 && proven_net_accept(&listener, PROVEN_NET_DONT_WAIT, &clients[accepted].conn, NULL) == PROVEN_OK) {
                    client_t *c = &clients[accepted];
                    c->number = accepted++;
                    c->open = true;
                    EXAMPLE_REQUIRE(proven_net_selector_add(selector, proven_net_conn_handle(&c->conn), PROVEN_NET_READABLE, c) == PROVEN_OK,
                                    "each accepted connection is registered, with its record as the tag");
                }
            } else {
                client_t *c = ready[i].tag;
                proven_byte_t buf[32];
                proven_result_size_t got = proven_net_read(&c->conn, (proven_mem_mut_t){ buf, sizeof buf }, PROVEN_NET_DONT_WAIT);
                if (got.err == PROVEN_OK) messages++;
            }
        }
    }
    EXAMPLE_REQUIRE(accepted == 3 && messages == 2, "three connections accepted, two messages read");
    EXAMPLE_REQUIRE(proven_net_selector_count(selector) == 4, "four sockets registered: the listener and three connections");

    /* 첫 연결은 말한 적이 없어서 보고된 적도 없다: 해 줄 일이 없었다. 이제 그 연결에 다른 것을
     * 묻는다 - 써도 되는가? - 그러면 곧장 보고된다. */
    proven_net_ready_t one[4];
    proven_size_t count = 0;
    EXAMPLE_REQUIRE(proven_net_selector_wait(selector, one, 4, proven_net_deadline_in(30), &count) == PROVEN_ERR_TIMEOUT, "all quiet: nothing is reported");
    EXAMPLE_REQUIRE(proven_net_selector_modify(selector, proven_net_conn_handle(&clients[0].conn), PROVEN_NET_WRITABLE, &clients[0]) == PROVEN_OK, "ask about writing instead");
    EXAMPLE_REQUIRE(proven_net_selector_wait(selector, one, 4, proven_net_deadline_in(1000), &count) == PROVEN_OK && count == 1 &&
                    one[0].tag == &clients[0] && (one[0].got & PROVEN_NET_WRITABLE), "an idle connection is writable");
    /* 쓸 것이 있는 동안에만 쓰기 가능을 물어라. 그러지 않으면 모든 기다림이 곧장 돌아온다. */
    EXAMPLE_REQUIRE(proven_net_selector_modify(selector, proven_net_conn_handle(&clients[0].conn), PROVEN_NET_READABLE, &clients[0]) == PROVEN_OK, "back to reading");

    /* 소켓은 닫기 "전에" 꺼낸다. */
    for (int i = 0; i < 3; ++i) {
        EXAMPLE_REQUIRE(proven_net_selector_remove(selector, proven_net_conn_handle(&clients[i].conn)) == PROVEN_OK, "removed");
        (void)proven_net_close(&clients[i].conn);
        (void)proven_net_close(&far[i]);
    }
    EXAMPLE_REQUIRE(proven_net_selector_remove(selector, proven_net_listener_handle(&listener)) == PROVEN_OK &&
                    proven_net_selector_count(selector) == 0, "and the listener: the selector is empty");
    (void)proven_net_listener_close(&listener);

    /* 이식형 종류를 이름으로 요청할 수 있다 - 둘을 비교하거나, Windows가 쓰는 코드 경로를
     * Linux에서 돌려 보려고. */
    proven_net_selector_t *portable = NULL;
    EXAMPLE_REQUIRE(proven_net_selector_create_poll(proven_heap_allocator(), &portable) == PROVEN_OK &&
                    proven_net_selector_kind(portable) == PROVEN_NET_SELECTOR_POLL, "a selector of the poll kind");
    proven_net_selector_destroy(portable);
    proven_net_selector_destroy(selector);
    return EXAMPLE_OK();
}
```

## 10. 플랫폼마다 다른 것

이 호출들은 Linux, BSD, macOS, Windows에서 똑같이 동작한다. 아래는 라이브러리가 흡수하는 차이들이다.
소켓을 직접 쓸 때 발목을 잡는 것들이라 적어 둔다.

| 함정 | `net.h`가 하는 일 |
|---|---|
| 닫힌 연결에 쓰면 `SIGPIPE`가 발생해 프로세스가 죽는다(POSIX). | 모든 send가 그 시그널을 억제한다. write는 `PROVEN_ERR_RESET`을 돌려준다. 여러분 대신 시그널 핸들러를 설치하지 않는다. |
| Winsock은 이름 해석을 포함해 어떤 호출보다도 먼저 시작돼야 한다. | 첫 사용 때 한 번 시작한다. |
| Windows에서 `SO_REUSEADDR`은 다른 프로세스가 여러분의 포트에 바인드해 연결을 가져가게 한다. | 리스너는 POSIX에서 `SO_REUSEADDR`을, Windows에서 `SO_EXCLUSIVEADDRUSE`를 쓴다. 한 주소의 두 번째 리스너는 어디서나 `PROVEN_ERR_BUSY`다. |
| IPv6 리스너가 어떤 시스템에서는 IPv4도 받고 어떤 시스템에서는 받지 않는다. | IPv6 소켓은 어디서나 IPv6 전용이다. 두 프로토콜을 다 받는 서버는 리스너를 둘 연다. |
| Windows의 논블로킹 send는 어떤 크기의 버퍼든 받아서 전부 큐에 넣는다. | send는 한정된 조각으로 내놓는다. 그래서 가득 찬 버퍼가 다른 곳에서처럼 밀어내고, 기한이 작동할 수 있다. |
| Windows는 상대의 정상 종료를 `WSAPoll`에 읽기 가능이 아니라 끊김으로 보고한다. | POSIX처럼 읽기 가능으로 보고한다. 뒤따르는 read는 `PROVEN_ERR_EOF`를 돌려준다. |
| 닫힌 포트로 보낸 UDP가 같은 Windows 소켓의 *나중* 받기를 실패하게 만든다. | 그 보고를 끈다. 받기는 자기가 받은 데이터그램에 대해서만 답한다. |
| 자식 프로세스가 열린 소켓을 물려받는다. | 모든 소켓은 상속되지 않게(close-on-exec) 만든다. |
| Winsock은 없는 경로에 대한 유닉스 도메인 connect를 "거절"로 답한다. | POSIX처럼 `PROVEN_ERR_NOT_FOUND`다. |

두 가지는 다르고, 다른 채로 남는다. Windows는 거절된 루프백 연결을 보고하기까지 약 2초를 기다리고,
POSIX는 즉시 답한다 - 기한에 그만큼을 감안하라. 그리고 유닉스 도메인 소켓에는 Windows 10 버전 1803
이상이 필요하다. 그 family가 없는 곳에서 `proven_net_listen`과 `proven_net_connect`는
`PROVEN_ERR_UNSUPPORTED`를 돌려준다.

## 11. 여기에 없는 것

- **암호화.** 여기서 만든 연결은 암호화되지 않는다. TLS는 전송 인터페이스 위의 한 층이다: [14장](manual-14-tls-ko.md).
- **이름 해석의 기한.** §2를 보라.
- **Windows에서의 규모.** selector는 `epoll`이나 `kqueue`가 있는 곳에서는 그것이고, Windows에서는 `WSAPoll` 위에 지은 것이다. completion port 경로는 없다.
- **유닉스 도메인 데이터그램, raw 소켓, 멀티캐스트, `TCP_NODELAY` 외의 소켓 옵션.**
- **HTTP.** 메시지 코덱은 [10장](manual-10-http-ko.md)에 있다. 그것을 이 소켓 위에서 구동하는 클라이언트와 서버는 [11장](manual-11-http-client-server-ko.md)에 있다.
