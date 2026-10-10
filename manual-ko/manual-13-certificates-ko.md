# 13장: 인증서와 신뢰

**5부 — 운영체제와 대화하기. 선행 조건: 다이제스트는 [4장](manual-04-containers-algorithms-ko.md),
저장소가 받는 할당자(allocator) - 예제에서는 힙(heap) 할당자 - 는 [2장](manual-02-allocation-ko.md).**
**이 장을 마치면** X.509 인증서를 읽고, 어느 이름을 위한 것인지 말하고, 믿는 인증서를 불러오고,
상대의 인증서 체인을 믿을지 결정할 수 있다 - 그리고 어떤 검사를 했고 어떤 검사를 하지 않았는지
정확히 안다.

이 장은 `cert.h`를 다룬다. 읽기와 검증은 아무것도 할당하지 않고 운영체제를 부르지 않는다.
[프리스탠딩(freestanding)](manual-freestanding-ko.md) 빌드에서 쓸 수 있다. 신뢰 앵커 저장소는 여러분이
준 할당자로 할당한다. 호출 하나, `proven_cert_store_add_system`은 운영체제의 루트를 읽으며
호스티드(hosted) 전용이다.

**이것이 무엇인가.** 이것은 TLS의 인증서 절반이며 따로도 쓸 수 있다. 파일, 메시지, 또는 다른
라이브러리의 핸드셰이크에서 건네받은 인증서를 검사하는 일이다. 프로토콜 자체 - 핸드셰이크와
암호화된 레코드 - 는 [14장](manual-14-tls-ko.md)이고, 여기 있는 모든 것을 쓴다.

**그리고 이 장에 무엇이 더해지든 남아 있을 경고.** 이것은 외부 감사를 받지 않은 새 구현이다. 다른
구현과 대조하고, 공개된 적대적 벡터로 시험했으며(어느 것인지는 8절), 그것은 증거이지 증명이 아니다.
능력 있는 공격자를 상대로 사용자가 의존하는 프로그램이라면 감사받은 것으로 인증서를 검사해야 한다.

## 목차

1. [인증서가 무엇인지, 한 쪽으로](#1-인증서가-무엇인지-한-쪽으로)
2. [하나 읽기](#2-하나-읽기)
3. [PEM](#3-pem)
4. [어느 이름을 위한 것인가](#4-어느-이름을-위한-것인가)
5. [신뢰 앵커](#5-신뢰-앵커)
6. [체인 검증](#6-체인-검증)
7. [피닝](#7-피닝)
8. [무엇을 검사하고, 무엇을 받아들이고, 무엇이 없는가](#8-무엇을-검사하고-무엇을-받아들이고-무엇이-없는가)

## 1. 인증서가 무엇인지, 한 쪽으로

공개 키가 있으면 어떤 메시지가 짝이 되는 개인 키를 가진 쪽에서 왔는지 확인할 수 있다. 그러나 그쪽이
*누구인지*는 알려 주지 않는다. **인증서**는 그 빠진 문장이다. "이 공개 키는 `example.com`의 것이다",
그리고 **발급자**의 서명. 발급자의 공개 키가 있으면 그 서명을 확인할 수 있는데, 그 키는 발급자 자신의
인증서에 들어 있고, 그 인증서는 *그것의* 발급자가 서명했다. 문장들은 **체인**을 이루고, 체인은
어디선가 끝나야 한다. 누가 서명해서가 아니라 이미 여러분의 기계에 있었기 때문에 믿는 인증서에서
끝난다. 그것이 **신뢰 앵커**, 곧 루트다.

그러니 상대를 믿는다는 것은 질문 넷이고, `proven_cert_verify`는 넷을 모두 묻는다.

| 질문 | 건너뛰면 잘못되는 것 |
|---|---|
| 이 인증서에서 내가 가진 앵커까지 유효한 서명의 체인이 있는가? | 누구나 어떤 이름으로든 인증서를 만들 수 있다. 체인만이 여러분이 믿는 누군가가 보증했다고 말해 준다. |
| 체인의 모든 인증서가 이 순간에 유효했는가? | 몇 해 전에 유출된 키도 여전히 서명한다. 날짜는 유출이 문제가 되는 기간을 제한한다. |
| 체인의 모든 발급자가 발급할 *자격*이 있었는가? | 평범한 웹 사이트의 인증서도 진짜 기관이 서명한 것이다. 그것이 다른 인증서에 서명할 수 있다면 사이트 주인 누구나 어떤 이름의 인증서든 찍어 낼 수 있다. |
| 인증서의 이름이 내가 닿으려던 이름인가? | `attacker.example`의 완벽하게 유효한 인증서는 `bank.example`에 대해 아무것도 증명하지 않는다. |

형식: 인증서는 **DER** - 압축된 이진 인코딩 - 이고, 보통 **PEM**으로 다닌다. PEM은 그 DER를
Base64로 바꿔 `-----BEGIN CERTIFICATE-----` 줄과 `END` 줄 사이에 넣은 것으로, 텍스트 파일에 붙여
넣어도 살아남는다.

## 2. 하나 읽기

```c
proven_err_t proven_cert_parse(proven_mem_view_t der, proven_cert_t *out);
```

`proven_cert_parse`는 DER 인증서 하나를 `proven_cert_t`로 읽는다. **아무것도 복사하지 않는다.**
구조체의 모든 필드는 여러분이 넘긴 바이트를 가리키는 뷰(view)이며, 구조체를 쓰는 동안 그 바이트가
살아 있어야 한다. 해제할 것은 없다.

읽기는 일부러 엄격하다. 인증서는 낯선 이가 보낸 입력이고, 두 프로그램이 다르게 읽을 수 있는
인코딩은 둘 중 하나가 속는 길이다. 그래서: 확정 길이만, 각각 가장 짧은 형태로. 쓸데없는 앞자리 0이
없는 정수. 정확히 `FF`인 `TRUE`. 서명된 부분의 안과 밖에 똑같이 적힌 서명 알고리즘. 두 번 나오지
않는 확장. 그리고 끝 뒤에는 한 바이트도 없다. 그 밖의 것은 모두 `PROVEN_ERR_INVALID_FORMAT`이다.

가장 자주 쓰게 될 필드:

| 필드 | 무엇인가 |
|---|---|
| `der`, `tbs` | 인증서 전체와, 발급자가 서명한 부분 |
| `subject`, `issuer` | 두 이름, 각각 DER. 바이트로 비교하라. 호스트 이름을 찾으려고 파싱하지 마라(4절) |
| `not_before`, `not_after` | 유효 기간, 1970-01-01 UTC부터의 초 |
| `public_key_info` | 알고리즘이 붙은 공개 키, DER - 핀이 해시하는 것(7절) |
| `key_kind`, `key`, `rsa_n`, `rsa_e` | 키 자체: RSA, P-256, P-384 또는 Ed25519. 그 밖은 `PROVEN_CERT_KEY_UNKNOWN` |
| `sig_kind`, `sig_hash`, `signature` | *발급자*가 이 인증서에 어떻게 서명했는가 |
| `is_ca`, `has_path_len`, `path_len` | 인증서를 발급할 수 있는지, 그 아래 체인이 얼마나 깊어도 되는지 |
| `key_usage`, `ext_key_usage` | 키를 무엇에 써도 되는지, `PROVEN_CERT_KU_*`와 `PROVEN_CERT_EKU_*` 플래그 |
| `alt_names` | 이 인증서가 위하는 이름들. `proven_cert_alt_name_next`로 훑는다 |
| `unknown_critical` | "이해하지 못하면 받아들이지 말라"고 표시된 확장을 담고 있는데, 이 라이브러리가 이해하지 못한다 |

이 라이브러리가 모르는 알고리즘 - 다른 곡선의 키, SHA-1 서명 - 은 파싱 에러가 **아니다**. 인증서는
종류가 `UNKNOWN`인 채로 파싱된다. 시스템 루트 저장소에는 그런 인증서가 몇 개 들어 있고, 그 하나
때문에 나머지를 잃어서는 안 되기 때문이다. 체인이 실제로 그것을 필요로 하면 나중에 검증에서
실패한다.

```c
bool proven_cert_alt_name_next(const proven_cert_t *cert, proven_size_t *pos,
                               proven_cert_name_kind_t *kind, proven_mem_view_t *value);
```

`proven_cert_alt_name_next`는 subjectAltName 항목을 훑는다. `*pos = 0`으로 시작해 `false`를 돌려줄
때까지 부른다. `PROVEN_CERT_NAME_DNS` 값은 이름의 ASCII 텍스트이고, `PROVEN_CERT_NAME_IP` 값은 주소
바이트 4개 또는 16개이며, 그 밖의 것(이메일 주소, URI)은 원래 내용 그대로
`PROVEN_CERT_NAME_OTHER`로 나온다.

## 3. PEM

```c
proven_err_t proven_pem_next(proven_mem_view_t pem, proven_size_t *pos, proven_u8str_view_t *label,
                             proven_mem_mut_t out, proven_size_t *written);
```

`proven_pem_next`는 `*pos` 이후의 다음 블록을 찾아 그 Base64를 `out`에 디코딩하고, `label`을 `BEGIN`
뒤의 단어(인증서라면 `CERTIFICATE`)로 맞추고, `*pos`를 블록 뒤로 옮긴다. 여러 개가 든 파일은 반복해서
부른다. 블록 사이의 텍스트 - 주석, 어떤 도구가 덧붙이는 사람이 읽는 덤프 - 는 건너뛴다.

| 반환 | 언제 |
|---|---|
| `PROVEN_OK` | 블록 하나를 디코딩했다. `out`에 `*written`바이트가 있다 |
| `PROVEN_ERR_NOT_FOUND` | 블록이 더 없다 |
| `PROVEN_ERR_OUT_OF_BOUNDS` | `out`이 작다. `*written`은 필요한 크기이고 `*pos`는 움직이지 않았다. 키워서 다시 부른다 |
| `PROVEN_ERR_INVALID_ENCODING` | 짝이 맞는 `END` 줄이 없는 블록, 또는 Base64가 아닌 본문 |

인증서만을 위한 것이 아니다. 키 파일도 라벨이 다른 PEM이다. 이 호출은 안에 무엇이 들었는지
상관하지 않는다.

## 4. 어느 이름을 위한 것인가

```c
bool proven_cert_matches_host(const proven_cert_t *cert, proven_u8str_view_t host);
```

`proven_cert_matches_host`는 인증서가 `host`를 위한 것인지 답한다. `host`는 DNS 이름이거나 텍스트로
적은 IP 주소다. 규칙은 브라우저들이 정착시킨 것이고, 하나하나가 한때 열려 있던 구멍을 막는다.

- **subjectAltName만 본다.** subject의 "common name"은 인증서들이 한때 호스트 이름을 적어 넣던 자유
  텍스트다. 호스트 이름이 아니며, 들여다보지 않는다.
- **와일드카드는 맨 왼쪽 라벨 전체이고 정확히 라벨 하나를 뜻한다.** `*.example.com`은
  `www.example.com`과 맞는다. `example.com`과도, `a.b.example.com`과도 맞지 않으며,
  `w*.example.com`은 아무 뜻도 없다. 별표 뒤에 라벨이 둘보다 적은 패턴(`*.com`)은 아무것과도 맞지
  않는다.
- **IP 주소는 주소로서, IP 항목과만 비교한다.** `192.0.2.1`은 우연히 `192.0.2.1`이라고 적힌 DNS
  항목과 절대 맞지 않고, `2001:db8::1`은 주소를 어떻게 적었든 맞는다.
- **대소문자는 상관없고, 호스트 끝의 점 하나는 무시한다.**

이름은 ASCII로 비교한다. 국제화된 이름은 인증서에 나타나는 형태인 `xn--` 꼴로 줘야 한다.

## 5. 신뢰 앵커

```c
proven_err_t proven_cert_store_create(proven_allocator_t alloc, proven_cert_store_t **out);
void proven_cert_store_destroy(proven_cert_store_t *store);
proven_err_t proven_cert_store_add_der(proven_cert_store_t *store, proven_mem_view_t der);
proven_err_t proven_cert_store_add_pem(proven_cert_store_t *store, proven_mem_view_t pem, proven_size_t *added);
proven_err_t proven_cert_store_add_system(proven_cert_store_t *store, proven_size_t *added);   /* hosted */
proven_size_t proven_cert_store_count(const proven_cert_store_t *store);
```

`proven_cert_store_t`는 여러분이 믿는 인증서의 집합이다. 더한 것의 **사본을 직접 갖는다.** 그래서
더할 때 쓴 바이트는 곧바로 해제해도 된다. `proven_cert_store_destroy`가 사본과 저장소를 해제한다.

**라이브러리는 루트를 싣고 다니지 않는다.** 라이브러리에 컴파일해 넣은 신뢰 기관 목록은 나가는 날
낡고, 사용자 절반에게는 틀리다. 여러분이 고른다.

- `proven_cert_store_add_system`은 운영체제의 루트를 가져온다 - 공개 인터넷과 대화할 때 맞는
  집합이다. Windows에서는 `ROOT` 시스템 저장소를 읽는다. 그 밖에서는 환경 변수 `SSL_CERT_FILE`의
  경로와 흔한 번들 위치 가운데 읽을 수 있는 첫 파일을 읽는다. 기계에 읽을 수 있는 것이 없으면 -
  최소한의 컨테이너에는 흔히 없다 - `PROVEN_ERR_NOT_FOUND`를 돌려준다. 그것은 보고할 일이지, 모든
  것을 믿는 것으로 피해 갈 일이 아니다. **Windows에서는 답이 기대보다 작을 수 있다:** Windows는
  지금까지 필요했던 루트만 갖고 있다가, 자신의 TLS가 다른 루트를 만나면 그때 받아 온다. 이 호출은
  설치된 것을 읽을 뿐 받아 오기를 일으키지 않으므로, 그 기계가 한 번도 쓴 적 없는 루트로 가는
  체인은 거기서 `PROVEN_ERR_UNTRUSTED`다. Windows에서 임의의 서버에 닿아야 하는 프로그램은 번들을
  함께 싣고 `proven_cert_store_add_pem`을 써야 한다.
- `proven_cert_store_add_pem`은 여러분이 공급하는 번들을 받는다. 조직의 기관, 또는 프로그램이
  대화하는 서버 하나의 인증서 하나. 인증서가 아닌 블록과 파싱되지 않는 인증서는 건너뛰고,
  `*added`가 받아들인 수를 센다. 인증서가 하나도 없는 번들은 `PROVEN_ERR_NOT_FOUND`다.
- `proven_cert_store_add_der`는 하나를 더한다.

저장소는 검증으로 바뀌지 않으므로, 검증만 하는 스레드라면 몇 개든 저장소 하나를 함께 써도 된다.
다른 스레드가 검증하는 동안 저장소에 더하는 것은 경쟁이다.

## 6. 체인 검증

```c
proven_err_t proven_cert_verify(const proven_mem_view_t *chain, proven_size_t count,
                                const proven_cert_verify_options_t *options,
                                proven_cert_verify_result_t *result);
```

`chain[0]`은 상대 자신의 인증서다. 나머지는 상대가 보낸 그 밖의 것들이며 **순서는 아무래도 좋다** -
서버는 순서에 무심하고, 필요 없는 인증서를 보내기도 한다. `proven_cert_verify`는 나머지를 재료로
삼아 `chain[0]`에서 앵커까지의 경로를 찾는다. 그 가운데 파싱되지 않는 인증서는 그냥 쓰이지 않는다.

옵션:

- `anchors` - 저장소. 필수.
- `host` - 닿으려던 이름. 비워 두면 이름 검사를 건너뛴다. 신원을 다른 방법(핀, 계정에 대응시킨
  클라이언트 인증서)으로 확인할 때에만 옳다.
- `now` - 시각, 1970-01-01 UTC부터의 초. **여러분이 공급한다.** 스스로 시계를 읽는 라이브러리는
  만료된 인증서를 기다리지 않고는 시험할 수 없고, 시계가 없는 곳에서는 돌 수 없다. 호스티드
  시스템에서는 벽시계 시각을 넘겨라. 배터리로 유지되는 시계가 없는 장치라면 어떤 시각을 가정할지
  정하고, 날짜 검사가 그 가정만큼만 믿을 만하다는 것을 알아 둬라.
- `use` - 상대가 서버이면 `PROVEN_CERT_USE_SERVER`, 여러분에게 인증서를 내미는 클라이언트이면
  `PROVEN_CERT_USE_CLIENT`.

답은 두 수준으로 온다. **에러 코드**는 프로그램이 분기하는 기준이다.

| 반환 | 뜻 | 합리적인 대응 |
|---|---|---|
| `PROVEN_OK` | 경로가 있고 모든 검사를 통과했다 | 진행 |
| `PROVEN_ERR_UNTRUSTED` | 앵커까지 받아들일 만한 경로가 없다 | 거부. 이 인증서를 보증하는 것이 없다 |
| `PROVEN_ERR_EXPIRED` | 경로는 있지만 그 위의 인증서가 `not_after`를 지났다 | 거부. 만료되었다고 사용자에게 알린다. *모든* 사이트가 이렇게 실패하면 시계를 의심하라 |
| `PROVEN_ERR_NOT_YET_VALID` | 경로는 있지만 그 위의 인증서가 `not_before` 이전이다 | 거부. 대개 시계가 범인이다 |
| `PROVEN_ERR_NAME_MISMATCH` | 체인은 좋은데 인증서가 다른 이름을 위한 것이다 | 거부. 잘못 이끌렸거나 가로채인 연결이 이렇게 보인다 |
| `PROVEN_ERR_INVALID_FORMAT` | `chain[0]`이 인증서가 아니다 | 거부 |
| `PROVEN_ERR_INVALID_ARG` | 체인, 옵션 또는 저장소가 없다 | 호출을 고친다 |

`result`의 **fault**(선택 - 원하지 않으면 널 포인터를 넘긴다)는 로그 한 줄이나 에러 메시지를 위한
정확한 이유다. `PROVEN_CERT_FAULT_NO_ISSUER`, `_BAD_SIGNATURE`, `_ALGORITHM`, `_NOT_A_CA`,
`_PATH_LENGTH`, `_NAME_CONSTRAINT`, `_CRITICAL_EXTENSION`, `_USAGE`, `_EXPIRED`, `_NOT_YET_VALID`,
`_NAME_MISMATCH`, `_TOO_DEEP`, `_MALFORMED`. `result->depth`는 받아들인 경로의 인증서 수이며 앵커를
포함한다.

여러 가지가 한꺼번에 틀렸을 때 답은 성공에 가장 가까운 것이다. 앵커까지 서명된 경로가 있지만
만료되었다면, 아무것도 찾지 못했다는 답이 아니라 만료되었다는 답을 받는다. 그것이 "인증서를
갱신하라"와 "당신은 누구인가?"의 차이다.

테스트 스위트가 컴파일하고 실행한다:

<!-- example: manual/examples/ko/ex_13_cert.c -->
```c
#include <string.h>

/*
 * 인증서는 어떤 키가 어떤 이름의 것이라는 서명된 진술이다. 여기서는 하나를 읽고, 어느 이름을 위한
 * 것인지 묻고, 믿을지 결정한다: 신뢰 앵커까지의 체인, 날짜, 그리고 이름 - 무엇이 실패했는지
 * 말해 주는 에러와 함께.
 */

/* 이 예제를 위해 만든 루트와, 그 루트가 example.test에 발급한 인증서. Ed25519라서 짧다.
 * 2026년부터 2036년까지 유효하다. */
static const char ROOT_PEM[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIBAjCBtaADAgECAhQxTiZoH2JgH7bPWBktoxUyd0d3ZzAFBgMrZXAwFzEVMBMG\n"
    "A1UEAwwMRXhhbXBsZSBSb290MB4XDTI2MDEwMTAwMDAwMFoXDTM2MDEwMTAwMDAw\n"
    "MFowFzEVMBMGA1UEAwwMRXhhbXBsZSBSb290MCowBQYDK2VwAyEAJcpfTb5GYzM+\n"
    "bgKssEvuWEC6urqf1Znqz9I8CY2RdLujEzARMA8GA1UdEwEB/wQFMAMBAf8wBQYD\n"
    "K2VwA0EARcXC/GhPDj4gAU3zpeSAhJ033zALpz2P91wtEqbf2R4Z7drW+RSRQGp2\n"
    "Ky/CDeEyx//WjylXEoHVNvixwKrxAw==\n"
    "-----END CERTIFICATE-----\n"
    ;

static const char LEAF_PEM[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIBPTCB8KADAgECAhRBU6wAvGvpDo28++Tq/BfZ/8DkNzAFBgMrZXAwFzEVMBMG\n"
    "A1UEAwwMRXhhbXBsZSBSb290MB4XDTI2MDEwMTAwMDAwMFoXDTM2MDEwMTAwMDAw\n"
    "MFowFzEVMBMGA1UEAwwMZXhhbXBsZS50ZXN0MCowBQYDK2VwAyEA3dq7G1ndiCbb\n"
    "IAS6O7cRIlxdACAdLSsPSRROKyglnTOjTjBMMAwGA1UdEwEB/wQCMAAwJwYDVR0R\n"
    "BCAwHoIMZXhhbXBsZS50ZXN0gg4qLmV4YW1wbGUudGVzdDATBgNVHSUEDDAKBggr\n"
    "BgEFBQcDATAFBgMrZXADQQBVDpl9ZAqf4j2R3NTG8JuKfPJuh+PD99QVdpjnqPAR\n"
    "dgg3sGIps+UTADlifcSOVlqJF6T9ya5lHpO5FwIpLpMI\n"
    "-----END CERTIFICATE-----\n"
    ;

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();

    // ---- PEM: 바이트를 감싼 텍스트 ------------------------------------------------
    /* 인증서는 PEM으로 다닌다: BEGIN과 END 줄 사이의 Base64. proven_pem_next는 다음 블록을 찾아
     * 디코딩한다. 안의 바이트는 DER이고, 나머지 모든 호출이 받는 것이 그것이다. */
    proven_byte_t leaf_der[1024];
    proven_size_t pos = 0, leaf_len = 0;
    proven_u8str_view_t label;
    proven_mem_view_t leaf_pem = { (const proven_byte_t *)LEAF_PEM, sizeof LEAF_PEM - 1 };
    EXAMPLE_REQUIRE(proven_pem_next(leaf_pem, &pos, &label, (proven_mem_mut_t){ leaf_der, sizeof leaf_der }, &leaf_len) == PROVEN_OK,
                    "블록이 디코딩된다");
    EXAMPLE_REQUIRE(proven_u8str_view_eq(label, PROVEN_LIT("CERTIFICATE")), "그리고 무엇인지 말한다");
    EXAMPLE_REQUIRE(proven_pem_next(leaf_pem, &pos, &label, (proven_mem_mut_t){ leaf_der, 0 }, &(proven_size_t){ 0 }) == PROVEN_ERR_NOT_FOUND,
                    "두 번째 블록은 없다");

    // ---- 인증서 하나 읽기 ---------------------------------------------------------
    /* 파싱은 아무것도 복사하지 않는다: proven_cert_t의 모든 필드는 leaf_der를 가리키는 뷰이고,
     * 구조체를 쓰는 동안 leaf_der가 살아 있어야 한다. */
    proven_cert_t leaf;
    EXAMPLE_REQUIRE(proven_cert_parse((proven_mem_view_t){ leaf_der, leaf_len }, &leaf) == PROVEN_OK, "파싱된다");
    EXAMPLE_REQUIRE(leaf.version == 3 && leaf.key_kind == PROVEN_CERT_KEY_ED25519 && leaf.key.size == 32 && !leaf.is_ca,
                    "버전 3, Ed25519 키, CA 아님");
    EXAMPLE_REQUIRE(leaf.not_before < 1811808000 && leaf.not_after > 1811808000, "2027년 6월에 유효");

    proven_size_t at = 0, names = 0;
    proven_cert_name_kind_t kind;
    proven_mem_view_t name;
    while (proven_cert_alt_name_next(&leaf, &at, &kind, &name)) {
        if (kind == PROVEN_CERT_NAME_DNS) names++;
    }
    EXAMPLE_REQUIRE(names == 2, "subjectAltName에 DNS 이름 둘");

    /* 어느 호스트를 위한 것인가? subjectAltName만 센다. 와일드카드는 정확히 라벨 하나를 뜻한다. */
    EXAMPLE_REQUIRE(proven_cert_matches_host(&leaf, PROVEN_LIT("example.test")), "이름 그 자체");
    EXAMPLE_REQUIRE(proven_cert_matches_host(&leaf, PROVEN_LIT("www.example.test")), "와일드카드 아래 라벨 하나");
    EXAMPLE_REQUIRE(!proven_cert_matches_host(&leaf, PROVEN_LIT("a.b.example.test")), "둘은 아니다");
    EXAMPLE_REQUIRE(!proven_cert_matches_host(&leaf, PROVEN_LIT("example.org")), "다른 이름도 아니다");

    // ---- 신뢰 앵커 ----------------------------------------------------------------
    /* 저장소는 이미 믿는 인증서를 담는다. 받은 것을 복사하므로 ROOT_PEM은 이 뒤에 해제해도 된다.
     * 라이브러리는 루트를 싣고 다니지 않는다: 직접 공급한다. */
    proven_cert_store_t *anchors = NULL;
    proven_size_t added = 0;
    EXAMPLE_REQUIRE(proven_cert_store_create(heap, &anchors) == PROVEN_OK, "저장소");
    EXAMPLE_REQUIRE(proven_cert_store_add_pem(anchors, (proven_mem_view_t){ (const proven_byte_t *)ROOT_PEM, sizeof ROOT_PEM - 1 }, &added) == PROVEN_OK &&
                    added == 1 && proven_cert_store_count(anchors) == 1, "번들에서 앵커 하나");

    // ---- 검증 ---------------------------------------------------------------------
    /* chain[0]은 상대의 인증서이고, 상대가 보낸 나머지가 순서 없이 뒤따른다. 시각은 호출자가
     * 공급한다: 스스로 시계를 읽는 라이브러리는 테스트할 수 없고, 시계가 없는 곳에서는 돌 수
     * 없다. */
    proven_mem_view_t chain[1] = { { leaf_der, leaf_len } };
    proven_cert_verify_options_t options = {
        .anchors = anchors,
        .host = PROVEN_LIT("www.example.test"),
        .now = 1811808000,                       /* 2027-06-01 */
        .use = PROVEN_CERT_USE_SERVER,
    };
    proven_cert_verify_result_t result;
    EXAMPLE_REQUIRE(proven_cert_verify(chain, 1, &options, &result) == PROVEN_OK && result.depth == 2, "길이 2의 경로: 리프와 앵커");

    /* 틀리는 방식마다 답이 따로 있다. 에러 코드는 거친 답이고, 결과의 fault는 로그 한 줄을
     * 위한 것이다. */
    options.host = PROVEN_LIT("example.org");
    EXAMPLE_REQUIRE(proven_cert_verify(chain, 1, &options, &result) == PROVEN_ERR_NAME_MISMATCH &&
                    result.fault == PROVEN_CERT_FAULT_NAME_MISMATCH, "체인은 좋고 이름은 아니다");
    options.host = PROVEN_LIT("www.example.test");
    options.now = 2127427200;                    /* 2037-06-01 */
    EXAMPLE_REQUIRE(proven_cert_verify(chain, 1, &options, &result) == PROVEN_ERR_EXPIRED, "십 년 뒤에는 만료되었다");
    options.now = 1811808000;
    leaf_der[leaf_len - 1] ^= 1;
    EXAMPLE_REQUIRE(proven_cert_verify(chain, 1, &options, &result) == PROVEN_ERR_UNTRUSTED &&
                    result.fault == PROVEN_CERT_FAULT_BAD_SIGNATURE, "비트 하나가 바뀌면 서명이 더는 검증되지 않는다");
    leaf_der[leaf_len - 1] ^= 1;

    proven_cert_store_t *empty = NULL;
    EXAMPLE_REQUIRE(proven_cert_store_create(heap, &empty) == PROVEN_OK, "저장소");
    options.anchors = empty;
    EXAMPLE_REQUIRE(proven_cert_verify(chain, 1, &options, &result) == PROVEN_ERR_UNTRUSTED &&
                    result.fault == PROVEN_CERT_FAULT_NO_ISSUER, "앵커가 없으면 아무것도 믿지 않는다");

    // ---- 핀 ------------------------------------------------------------------------
    /* 핀은 인증서 공개 키(SubjectPublicKeyInfo)의 SHA-256이다. 어떤 키를 기대하는지 아는
     * 프로그램은 체인에 더해, 또는 체인 대신 이것을 비교한다. */
    proven_byte_t pin[32], again[32];
    EXAMPLE_REQUIRE(proven_cert_key_sha256(&leaf, pin) == PROVEN_OK, "핀");
    proven_sha256(leaf.public_key_info, again);
    EXAMPLE_REQUIRE(proven_mem_equal_ct((proven_mem_view_t){ pin, 32 }, (proven_mem_view_t){ again, 32 }), "키 인코딩의 해시다");

    // ---- 시스템의 루트 ------------------------------------------------------------
    /* 공개 인터넷과 대화할 때 앵커는 운영체제의 것이다. 기계에 그것이 있는지는 기계의 사정이므로
     * 이 예제는 두 답을 모두 받아들인다. */
    proven_cert_store_t *system_roots = NULL;
    EXAMPLE_REQUIRE(proven_cert_store_create(heap, &system_roots) == PROVEN_OK, "저장소");
    proven_err_t sys = proven_cert_store_add_system(system_roots, &added);
    EXAMPLE_REQUIRE(sys == PROVEN_OK || sys == PROVEN_ERR_NOT_FOUND, "시스템 저장소를 읽었거나, 없다");
    EXAMPLE_REQUIRE((sys == PROVEN_OK) == (proven_cert_store_count(system_roots) > 0), "읽었다면 비어 있지 않다");

    /* DER 인증서 하나도 더할 수 있다. */
    proven_byte_t root_der[1024];
    proven_size_t root_len = 0;
    pos = 0;
    EXAMPLE_REQUIRE(proven_pem_next((proven_mem_view_t){ (const proven_byte_t *)ROOT_PEM, sizeof ROOT_PEM - 1 }, &pos, &label,
                                    (proven_mem_mut_t){ root_der, sizeof root_der }, &root_len) == PROVEN_OK, "블록이 디코딩된다");
    EXAMPLE_REQUIRE(proven_cert_store_add_der(empty, (proven_mem_view_t){ root_der, root_len }) == PROVEN_OK, "DER로 된 루트");
    EXAMPLE_REQUIRE(proven_cert_verify(chain, 1, &options, &result) == PROVEN_OK, "이제 비어 있던 저장소가 리프를 믿는다");

    proven_cert_store_destroy(system_roots);
    proven_cert_store_destroy(empty);
    proven_cert_store_destroy(anchors);
    return EXAMPLE_OK();
}
```

## 7. 피닝

```c
proven_err_t proven_cert_key_sha256(const proven_cert_t *cert, proven_byte_t out[32]);
```

`proven_cert_key_sha256`은 인증서 공개 키의 SHA-256을, SubjectPublicKeyInfo 인코딩에 대해 준다.
그것이 **핀**이다. 서버가 어떤 키를 갖고 있는지 미리 아는 프로그램은 이 값을 비교해, 어떤 기관이
뭐라고 하든 나머지를 모두 거부할 수 있다.

인증서가 아니라 키를 핀으로 삼아라. 인증서는 몇 달마다 다시 발급되고 그때마다 해시가 바뀐다. 그
밑의 키는 그대로 둘 수 있다. `proven_mem_equal_ct`로 비교하고, 핀을 적어도 둘 - 쓰고 있는 키와
오프라인에 둔 예비 키 - 실어 보내라. 핀으로 삼은 유일한 키를 잃은 프로그램은 업데이트 없이는 고칠
수 없다.

핀은 검증에 **더해서**(체인도 좋아야 *하고* 키도 기대한 것이어야 한다) 쓸 수도 있고 검증
**대신**(상대의 인증서를 파싱하고, 핀을 비교하고, 체인은 만들지 않는다) 쓸 수도 있다. 뒤쪽은 직접
운영하는 자체 서명 서버에 맞다. 그때는 인증서의 날짜와 이름을 전혀 검사하지 않으므로 핀이 보안의
전부다.

## 8. 무엇을 검사하고, 무엇을 받아들이고, 무엇이 없는가

**경로 위의 모든 인증서에 대해 검사하는 것:**

- 그 위에 놓인 발급자의 서명.
- 여러분이 공급한 시각에 대한 유효 기간. 앵커도 포함한다.
- 발급자가 CA라는 것: `basicConstraints`가 그렇게 말하고, `keyUsage`가 있다면 인증서 서명을
  허용한다. 확장이 전혀 없는(버전 1) 앵커는 여러분이 저장소에 넣었으므로 CA로 받아들인다. CA라고
  말하지 않는 버전 3 인증서는 받아들이지 않는다.
- 발급자가 자기 아래에 허용하는 경로 길이.
- 발급자가 거는 이름 제약, DNS 이름과 IP 주소에 대해 - 이 라이브러리가 주장하는 이름은 그것뿐이다.
  다른 이름 형태에 대한 제약은 적용하지 않는다.
- 확장 키 용도, 있다면: 리프와 모든 중간 인증서가 여러분이 요청한 용도를 허용해야 한다.
- 읽지 못한 채 남은 critical 확장이 없다는 것.

**받아들이는 알고리즘.** 서명: 2048~8192비트 키에 대한 SHA-256, SHA-384 또는 SHA-512의 RSA
PKCS #1 v1.5와 RSA-PSS. 같은 세 해시와 P-256, P-384의 ECDSA. Ed25519. SHA-1이나 MD5 서명, 2048비트
미만의 RSA 키, 그 밖의 곡선은 fault `PROVEN_CERT_FAULT_ALGORITHM` 또는 `_BAD_SIGNATURE`로
거부한다. 앵커 *자신의* 서명은 검사하지 않는다 - 앵커는 저장소에 있어서 믿는 것이다 - 그래서
SHA-1로 서명된 오래된 루트도 여전히 체인의 닻이 된다.

**한계.** 경로는 최대 `PROVEN_CERT_MAX_DEPTH`(10)개의 인증서다. 리프 외에 최대 16개의 인증서를
고려하고, 서명 검사 64번 뒤에는 탐색을 멈춘다. 상대가 뒤엉킨 묶음을 보내 검증을 비싸게 만들 수
없다.

**여기에 없으며, 감안해서 계획해야 하는 것:**

- **폐기.** CRL도, OCSP도, 스테이플된 응답의 검사도 없다. 유효했다가 그 뒤 폐기된 인증서는
  검증된다. 수명이 짧은 인증서가 실질적인 방어다.
- **Certificate Transparency.** 인증서가 공개 로그에 기록되었는지 검사하지 않는다.
- **인증서 정책.** 정책 확장은 그것을 담은 인증서가 거부되지 않도록 인식하되, 강제하지 않는다.
- **국제화된 이름.** ASCII로 비교한다. `xn--` 꼴로 줘라.
- **개인 키.** 여기 있는 어떤 것도 개인 키를 읽거나 갖고 있지 않다.

**어떻게 시험했는가.** 다른 구현이 만든 인증서 체인으로, 위에 적은 수락과 거부 하나하나에 대해.
공개 서버에서 저장한 체인을 시스템의 루트에 대해. 그리고 그 밑의 서명 알고리즘은 Project
Wycheproof로 - 구현이 깨진다고 알려진 방식으로 구현을 깨도록 만든 입력 모음이다. `TEST.md`의
카탈로그가 등록된 테스트 각각이 무엇을 덮는지 말한다.
