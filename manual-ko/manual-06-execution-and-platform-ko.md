# 6장: 실행, 별칭, PAL, Freestanding, 크로스 빌드

**6부 — 더 나아가기. 선행 조건: 2–5부.**
**이 장을 마치면** 메모리 모델에 놀라는 일 없이 둘 이상의 스레드에서 작업을 실행할 수 있고,
루프처럼 읽히는 상태 기계를 작성할 수 있으며, 운영체제가 없는 대상용으로 빌드할 수 있다.

이 장은 `coro.h`, `job.h`, `alias_xcv.h`, 스레드 안전성과 포인터 프로버넌스, PAL
계약, freestanding 모드, 플랫폼 빌드 관련 사항을 다룬다. 이것은 이 매뉴얼에서 가장 어려운
내용이며, 의도적으로 맨 마지막에 놓았다 — 1–5부의 어떤 것도 여기에 의존하지 않는다.

## 목차

1. [스택리스 코루틴](#1-스택리스-코루틴)
2. [Job system](#2-job-system)
3. [스레드, allocator, 포인터 프로버넌스](#3-스레드-allocator-포인터-프로버넌스)
4. [별칭 계층](#4-별칭-계층)
5. [PAL 계약](#5-pal-계약)
6. [Freestanding 부분집합](#6-freestanding-부분집합)
7. [크로스 컴파일](#7-크로스-컴파일)
8. [예제와 오용 사례](#8-예제와-오용-사례)

## 1. 스택리스 코루틴

코루틴 매크로는 switch 안에서 `__LINE__` 라벨을 사용하여 스택리스 상태 기계를 구현한다. 코루틴 함수는 `proven_i32`를 반환해야 한다: `0`은 yield 또는 대기 중을 의미하고, `1`은 완료를 의미한다.

### 구조체

```text
typedef struct {
proven_i32 state;
} proven_coro_t;
```

### 매크로

| 매크로 | 의도 |
|---|---|
| `PROVEN_CORO_INIT(c)` | 첫 사용 전에 state를 0으로 설정한다. |
| `PROVEN_CORO_BEGIN(c)` | 코루틴 switch 블록을 시작한다. |
| `PROVEN_CORO_YIELD(c)` | 재개 지점을 저장하고 0을 반환한다. |
| `PROVEN_CORO_AWAIT(c, cond)` | `cond`가 참이 될 때까지 0을 반환하고, 참이 되면 계속 진행한다. |
| `PROVEN_CORO_END(c)` | 완료로 표시하고 1을 반환한다. |
| `PROVEN_CORO_IS_DONE(c)` | state가 -1인지 확인한다. |

### 어떻게 전개되는가 (그리고 유일하게 중요한 규칙 하나)

이 매크로들은 고전적인 Protothreads/Duff's-device 기법이다. `BEGIN`은
`switch (state)`를 열고, 각 `YIELD`/`AWAIT`는 `__LINE__`을 재개 라벨로 기록한 뒤
`return`하며, `END`는 switch를 닫는다. 개념적으로는 다음과 같다:

```text
proven_i32 f(Ctx *c) {
    switch (c->coro.state) {       /* PROVEN_CORO_BEGIN */
    case 0:
        /* ... code before the first yield ... */
        c->coro.state = 42; return 0; case 42:;   /* PROVEN_CORO_YIELD on line 42 */
        /* ... code after the yield ... */
    }
    c->coro.state = -1; return 1;  /* PROVEN_CORO_END */
}
```

함수는 각 yield 지점에서 **반환**되고, **맨 위에서 다시 진입**하여
재개 `case`로 곧장 점프한다. 그 결과로 기억해야 할 단 하나의 규칙:

> **지역(스택) 변수는 yield를 넘어 살아남지 못한다.** `YIELD`/`AWAIT`를 넘어
> 값이 유지되어야 하는 것은 코루틴 자신의 구조체 안에(또는 `static`으로)
> 있어야 한다. 지역 변수는 호출할 때마다 다시 생성되기 때문이다.

전개에서 따라 나오는 다른 제약들: 두 코루틴 매크로는 같은 소스 라인을 공유해서는
안 되고(`__LINE__`이 충돌한다), `YIELD`/`AWAIT`는 여러분 자신의 `switch` 안에
있으면 안 된다(잘못된 `case`에 착지하게 된다). 코루틴은 스택이 없으므로 자신이
호출하는 헬퍼 함수에서는 yield할 수 없다 — 오직 자기 본문에서만 가능하다.

#### 반례 — yield를 넘는 지역 변수

```text
static proven_i32 bad(Ctx *c) {
    PROVEN_CORO_BEGIN(&c->coro);
    int i = 0;                       /* WRONG: a local */
    while (i < 3) {
        i += 1;
        PROVEN_CORO_YIELD(&c->coro); /* on re-entry `i` is re-initialized to 0 */
    }
    PROVEN_CORO_END(&c->coro);       /* loop never ends: infinite yields */
}
/* RIGHT: put `i` in Ctx (like `value` in the Counter example below). */
```

예제(스케치: 코루틴은 함수 전체이므로 문장 조각으로는 보여줄 수 없다 —
이 형태를 컴파일한 버전은 [8절](#8-예제와-오용-사례)의 다루어진 예제,
`manual/examples/ex_06_coro.c`이다):

```text
typedef struct Counter {
proven_coro_t coro;
int value;
} Counter;

static proven_i32 counter_next(Counter *c) {
PROVEN_CORO_BEGIN(&c->coro);
c->value = 0;
while (c->value < 3) {
c->value += 1;
PROVEN_CORO_YIELD(&c->coro);
}
PROVEN_CORO_END(&c->coro);
}

Counter c = {0};
PROVEN_CORO_INIT(&c.coro);
while (!counter_next(&c)) {
use_value(c.value);
}
```

## 2. Job system

job system은 워커 스레드와 경계가 있는 MPMC 방식 큐를 소유한다. 생산자는 `routine(arg)` 작업 항목을 제출한다. 워커는 시스템이 닫히고 비워질 때까지 큐에 들어온 작업을 실행한다.

### 구조체

```text
typedef struct {
void (*routine)(void *arg);
void *arg;
} proven_job_t;

typedef struct proven_job_sys proven_job_sys_t;
```

`proven_job_sys_t`는 불투명(opaque) 타입이다.

### 함수

| API | 의도 | 반환 |
|---|---|---|
| `proven_job_system_init(alloc, num_workers, max_queue_capacity, out_sys)` | job system을 할당하고 시작한다. 큐 용량은 2의 거듭제곱이어야 한다. | `proven_err_t`. |
| `proven_job_system_close(sys)` | 새 작업 수락을 중단한다. | void. |
| `proven_job_system_destroy(sys)` | 필요하면 닫고, 큐를 비우고, 워커를 join하고, 자원을 해제한다. | void. |
| `proven_job_submit(sys, routine, arg)` | 작업 하나를 제출한다. 다른 제출자와 스레드 안전하다. | 큐에 들어가면 true, 가득 차거나 닫혀 있으면 false. |
| `proven_job_execute_one(sys)` | 호출 스레드가 사용 가능한 작업 하나를 실행하게 한다. | 작업이 실행되었으면 true. |

중요한 제약:

- `proven_job_system_destroy()`는 여전히 `proven_job_submit()`을 호출하는 생산자 스레드와 경쟁(race)해서는 안 된다.
- 호출자는 생산자 종료를 외부에서 동기화한 다음 close/destroy해야 한다.
- 큐 시퀀스 카운터는 하나의 job-system 수명 동안 부호 있는 포인터 차이(signed pointer-difference) 범위를 넘어 랩(wrap)하지 않는다고 가정한다.

### 동시성 모델

큐는 `max_queue_capacity`개의 슬롯을 가진 고정 크기 링 버퍼이다(2의 거듭제곱이어야
하며, 그래서 슬롯 인덱스는 `seq & (capacity - 1)`이다). 생산자와 소비자는 큐 전체를
잠그지 않고 **원자적 시퀀스 카운터**로 협조한다:

- 생산자(`proven_job_submit`)는 다음 enqueue 위치를 원자적으로 확보한다. 확보가
  아직 소비되지 않은 가장 느린 슬롯을 덮어쓰게 되는 경우, 큐가 가득 찬 것이며 submit은
  `false`를 반환한다 — 절대 블록하지 않고 대기 중인 작업을 덮어쓰지 않는다.
- 소비자(워커 스레드, 또는 `proven_job_execute_one`을 통한 여러분 자신의 스레드)는
  다음 dequeue 위치를 원자적으로 확보하고, `routine`/`arg`를 읽고, 슬롯을
  재사용 가능으로 표시한 뒤, 확보 구간 바깥에서 routine을 실행한다.

submit과 execute가 MPMC-안전하므로, *여러* 스레드가 동시에 제출할 수 있고 *여러*
스레드가 동시에 소비할 수 있다. 라이브러리가 여러분을 위해 **하지 않는** 것: 그것은
*작업이 건드리는 데이터*를 동기화하지 않는다. 같은 변수를 쓰는 두 작업은 여전히
자체 잠금/원자 연산이 필요하다 — 큐는 인계(handoff)만 순서 짓고 작업 자체는 아니다.
그 작업이 사용하는 allocator의 스레드 안전성, 할당을 공유할 때의 포인터 프로버넌스
위험, 그리고 락프리 도구 상자(CAS, ABA, 태그 포인터, hazard pointer, 에폭 기반 회수)에
대해서는 2장 §7 "Allocator 스레드 안전성과 프로버넌스"를 참조하라.

수명 주기 상태 기계(이 순서로 구동하라):

```
init  --->  running  --(close)-->  closed  --(destroy: drain + join)-->  freed
```

- `init`은 `num_workers`개의 OS 스레드와 슬롯 버퍼를 미리 예약한다.
- `close`는 플래그를 뒤집어 이후의 모든 `submit`이 `false`를 반환하게 한다. 이미
  큐에 들어갔거나 진행 중인 작업은 여전히 실행된다.
- `destroy`는 필요하면 닫은 다음, **큐가 비고 모든 워커가 join될 때까지 호출
  스레드를 블록**한 뒤, 모든 것을 해제한다.

**메모리 가시성.** `destroy` 내부에서 워커가 join되는 것은 동기화 지점이다:
모든 작업의 모든 메모리 효과는 *`destroy`가 반환된 후* `destroy`를 호출한 스레드에
가시적이다. 따라서 결과를 수집하는 안전한 패턴은 "모두 submit → close → destroy →
결과 읽기"이다. 그 join *이전에* 다른 스레드에서 작업의 출력을 읽으려면 여러분
자신의 동기화가 필요하다.

#### 반례

```text
/* WRONG: capacity must be a power of two. */
proven_job_system_init(alloc, 4, 100, &sys);   /* 100 is not 2^n */

/* WRONG: reading a result before the join makes it visible. */
int total = 0;
(void)proven_job_submit(sys, accumulate, &total);
proven_println("total = {}", PROVEN_ARG(total));  /* data race: job may still be running */
/* RIGHT: */
proven_job_system_close(sys);
proven_job_system_destroy(sys);                   /* joins workers -> writes now visible */
proven_println("total = {}", PROVEN_ARG(total));

/* WRONG: ignoring the submit result drops work silently when the ring is full. */
proven_job_submit(sys, work, arg);   /* [[nodiscard]]: a false return means NOT queued */
```

예제(스케치인 이유는 job routine이 자체 함수여야 하기 때문이다. 공유 카운터가
실제로 필요로 하는 원자 연산을 포함한 컴파일된 버전은 [8절](#8-예제와-오용-사례)의
다루어진 예제, `manual/examples/ex_06_job.c`이다):

```text
static void increment(void *arg) {
int *p = arg;
*p += 1;
}

proven_job_sys_t *sys = NULL;
proven_err_t e = proven_job_system_init(alloc, 2, 64, &sys);
if (!proven_is_ok(e)) return e;

int value = 0;
if (!proven_job_submit(sys, increment, &value)) {
proven_job_system_close(sys);
proven_job_system_destroy(sys);
return PROVEN_ERR_BUSY;
}

proven_job_system_close(sys);
proven_job_system_destroy(sys);
```

routine이 전혀 보이지 않는, 수명 주기 그 자체는 평범한 문장 코드이다:

```c
proven_job_sys_t *sys = NULL;
proven_err_t e = proven_job_system_init(alloc, 2, 64, &sys);  /* capacity: power of two */
if (proven_is_ok(e)) {
    /* ... submit work here, from this thread or from producers you own ... */
    proven_job_system_close(sys);     /* every later submit now returns false */
    proven_job_system_destroy(sys);   /* drains the queue, joins the workers */
}
```

## 3. 스레드, allocator, 포인터 프로버넌스

**이 절은 2장에서 여기로 옮겨 왔다.** 매뉴얼에서 가장 어려운 내용이 독자가 가장 먼저 여는 장
중 하나에 놓여 있었고, 1–5장의 어떤 것도 여기에 의존하지 않는다. `proven` 객체를 스레드 간에
공유하고 있거나 그 위에 락프리 구조를 만들고 있다면 이 절이 필요한 절이다. 그렇지 않다면 통째로
건너뛰어도 되고, 이후에 아무 지장도 없다.

`proven_allocator_t` 트레이트는 `ctx` 하나에 함수 포인터 셋일 뿐이며, **자체적인
동기화를 전혀 담고 있지 않다**. 동시 사용이 안전한지는 전적으로 트레이트 뒤에 있는
구체 allocator에, 그리고 allocator가 만들어 낸 *객체*를 어떻게 공유하는지에 달려
있다. 이 절은 먼저 보장 사항을 밝히고, 그다음 더 깊은 포인터 프로버넌스 위험과 그
위에 동시성 구조를 만들 때 필요한 락프리 개념들을 설명한다.

### 3.1 무엇이 스레드 안전하고 무엇이 아닌가

| Allocator | 한 인스턴스의 동시 사용 | 이유 |
|---|---|---|
| `proven_heap_allocator()` | 동시 `alloc`/`realloc`/`free`에 **안전** | 트레이트는 상태가 없다(`ctx == NULL`). 플랫폼의 `aligned_alloc`/`posix_memalign`/`_aligned_malloc`+`free`로 넘기며, 이들은 C11 이래 스레드 안전하다. |
| `proven_arena_t` | **안전하지 않음** | `proven_arena_alloc_*`는 `arena->offset`을 비원자적으로 읽고-수정하고-쓴다. |
| `proven_pool_t` | **안전하지 않음** | `proven_pool_*`는 프리리스트(`bin`, `bin_len`)를 비원자적으로 pop/push한다. |

여기서 두 가지 규칙이 따라 나온다:

1. **"allocator 안전"은 "객체 안전"이 아니다.** 힙 allocator를 쓰더라도 그 위에
   세워진 컨테이너들(`proven_u8str_t`, `proven_array_t`, `proven_map_t`, …)은
   내부 잠금을 전혀 추가하지 않는다. 한 스레드가 append하는 동안 다른 스레드가
   읽는 `proven_u8str_t`는 allocator가 얼마나 스레드 안전하든 데이터 경쟁이다.
   공유 가변 객체에는 언제나 *여러분의* 동기화가 필요하다.
2. **Arena와 pool은 공유해서는 안 된다.** 동시 `proven_arena_alloc`은 `offset`을
   찢어 놓아 *같은 바이트*를 두 스레드에 넘기거나 할당 하나를 통째로 잃어버릴 수
   있고, 동시 pool pop/push는 같은 슬롯을 두 번 내주거나 `bin_len`을 언더플로시킬
   수 있다. 데이터 경쟁은 그 자체로 미정의 동작이다 — 그 결과가 왜 "숫자가 하나
   틀린 것"보다 나쁜지는 §7.3을 보라.

### 3.2 한 문단으로 보는 포인터 프로버넌스

C의 추상 기계에서 모든 포인터는 **프로버넌스**를 지닌다: 그 포인터가 유래한 저장소
인스턴스의 정체성이다. 여기서 중요한 규칙은 둘이다: (a) 포인터를 그 프로버넌스
객체의 수명이나 경계 바깥에서 사용하는 것은 미정의이며, (b) 최적화기는 **프로버넌스가
다른 포인터끼리는 앨리어싱하지 않는다**고 가정하고 그 근거로 메모리 접근을 재정렬하거나
제거해도 된다. 할당, 재할당, 해제는 모두 프로버넌스를 만들고 없앤다 — 바로 그래서
동기화되지 않은 공유와 나쁘게 상호작용한다.

### 3.3 스레드에서 할당 + 프로버넌스가 물어뜯는 지점

- **여기서 `realloc`은 항상 재배치한다.** 플랫폼의 `realloc`은 *새로 할당 + 복사 +
  옛것 해제*이므로(정렬된 블록은 이식성 있게 제자리에서 크기를 바꿀 수 없다), 성공한
  성장은 **항상** 새 프로버넌스를 가진 새 객체를 반환하고 옛 객체를 끝낸다. 옛
  포인터를 남겨 둔 복사본은 무엇이든 — 캐시해 둔 원소 포인터, 빌려온
  `proven_mem_view_t`/`proven_u8str_view_t` — 이제 댕글링이다. 단일 스레드에서는
  성장을 넘어 뷰를 들고 있지 않음으로써 이를 피하지만, 스레드가 여럿이면 다른
  스레드가 *아무 순간에나* 공유 컨테이너를 성장시킬 수 있고, 여러분의 뷰는 해제된
  저장소를 가리키게 된다(규칙 (a): UB, 게다가 데이터 경쟁).
- **주소 재사용 / ABA.** `free` 후 `alloc`(특히 pool의 프리리스트 재활용)은 *같은
  주소*를 *다른* 객체에 *새* 프로버넌스로 돌려줄 수 있다. 옛 포인터를 여전히 들고
  있는 스레드는 옛 프로버넌스를 가정한다. 규칙 (b)에 따라 컴파일러는 바이트가
  일치하더라도 둘을 앨리어싱하지 않는 것으로 취급할 수 있고, happens-before 간선이
  없으면 새 객체에 대한 쓰기가 가시적일 필요도 없다. 같은 주소, 다른 프로버넌스 —
  여전히 UB다.
- **`uintptr_t` 왕복 + 찢어진 읽기.** arena는 정렬 계산을 위해 `backing.ptr`을
  정수로 변환한다. arena는 정수로부터 포인터를 날조하는 대신 원래 `backing.ptr`을
  오프셋하여 *결과* 포인터를 유도하도록(그 프로버넌스를 보존하도록) 조심한다 —
  프로버넌스 관점에서 올바른 기법이다. 하지만 `offset`이 경쟁 상황에서 찢어진 채로
  읽히면, 계산된 포인터가 backing 객체 *바깥*에 착지할 수 있다. 즉 그 위치에 대한
  유효한 프로버넌스가 없는 접근이 된다. 경쟁은 단지 틀린 오프셋이 아니라 그곳을
  가리킬 권리가 없는 포인터를 만들어 낸다.
- **데이터 경쟁 × 프로버넌스 추론 = 잘못된 값이 아니라 미스컴파일.** 컴파일러는 각
  스레드 안에서 단일 스레드적, 프로버넌스 기반의 비앨리어싱 추론을 적용하므로,
  경쟁하는 allocator는 최적화기가 런타임에는 성립하지 않는 비앨리어싱을 "증명"하게
  만들 수 있다 — 찢어진 포인터, 컴파일러가 겹칠 수 없다고 믿는 이중 할당, 사라진
  저장이 그 결과다. 실패 양상은 어쩌다 나오는 값이 아니라 구조적이다.

### 3.4 락프리 도구 상자 (그 위에 동시성을 만든다면, 개념 정리)

`proven`은 락프리 allocator나 안전 메모리 회수(safe-memory-reclamation) 기법을
**전혀** 구현하지 않는다. 이 allocator들 위에 동시 자료구조를 만든다면 다음은
여러분이 직접 공급한다(`<stdatomic.h>`는 사용 가능하다 — 6장의 job system이 큐
인덱스에 이를 쓴다). 아래는 표준적인 조각들과 그것들이 위의 프로버넌스 위험과 어떻게
관계되는지다.

- **CAS (compare-and-swap).** 거의 모든 락프리 코드의 바탕이 되는 원자 기본 연산이다:
  `atomic_compare_exchange_strong(&p, &expected, desired)`는 `p == expected`일 때만
  원자적으로 `p = desired`로 설정하고, 아니면 현재 값을 보고한다. 다른 스레드가
  그 워드를 먼저 바꾸지 않았을 때만 한 스레드가 변경을 공표하게 해 준다.

  ```text
  /* Treiber-stack style push (sketch): node_t and n are illustrative, so this
     is a listing rather than a compiled block. */
  _Atomic(node_t *) head;
  node_t *old = atomic_load(&head);
  do { n->next = old; } while (!atomic_compare_exchange_weak(&head, &old, n));
  ```

- **ABA 문제.** CAS는 *값*을 비교하지 이력을 비교하지 않는다. 락프리 pop이
  `head == A`를 읽고 `A->next`를 설치할 계획을 세운 뒤 CAS를 한다. 그 사이에 다른
  스레드들이 `A`를 pop하고, 그 후속을 pop하고, 그것들을 해제한 뒤, `A`를 다시
  push하면(주소가 재사용되어), CAS는 여전히 `head == A`를 보고 *성공한다* — 해제된
  메모리를 가리키는 포인터를 설치하면서. 값은 일치했지만(A→B→A) 세상은 바뀌었다.
  pool의 프리리스트는 순진하게 락프리로 만들면 교과서적인 ABA 후보다.
- **태그 포인터 / 버전 카운터.** 단조 증가하는 태그를 포인터 옆에 담아 함께
  CAS한다(더블 폭 CAS를 쓰거나, 낮은 정렬 비트나 상위 비트를 훔쳐서). 성공한 갱신마다
  태그가 올라가므로 A→B→A 시퀀스는 *다른* 태그로 돌아오고 CAS는 실패한다 — ABA가
  탐지된다. 비용/한계: 비트 훔치기는 정렬이 보장되어야 하고 쓸 수 있는 주소 범위를
  줄인다. 전체 폭 태그는 하드웨어 더블 워드 CAS(예: `cmpxchg16b`)를 필요로 한다.
  태그는 원리상 랩할 수 있다. **결정적으로, 태그는 공유 워드에서의 ABA *탐지*를
  해결할 뿐 프로버넌스를 복원하지는 않는다.** 재사용된 주소는 여전히 새 객체이며,
  태그는 낡은 관점에 따라 *행동하는* 것을 막아 줄 뿐이다.
- **Hazard pointer (안전 메모리 회수).** 각 스레드가 단일 기록자/다중 독자 "hazard"
  슬롯을 몇 개 소유한다. 공유 포인터를 역참조하기 전에 그 포인터를 슬롯에 공표하고
  다시 검증한다. 객체를 해제하려는 스레드는 먼저 모든 hazard 슬롯을 훑고, 누군가
  보호 중이면 해제를 **미룬다**(retire 목록). 이는 메모리를 유계로 유지하고
  use-after-free와 회수-ABA를 막아 주며, 대가는 보호되는 접근마다 저장 한 번 + 펜스
  하나, 그리고 스레드당 동시에 보호할 수 있는 포인터 개수가 고정된다는 점이다.
- **에폭 기반 회수(EBR).** 전역 에폭 카운터를 둔다. 스레드는 공유 구조를 건드리는
  동안 현재 에폭을 "핀"하고, 은퇴시킨 메모리에는 은퇴한 에폭을 태그한다. 에폭 *e*에서
  은퇴한 메모리는 모든 스레드가 *e*를 지난 것이 관찰된 뒤에야 해제된다(에폭 두어 개
  분량의 유예 기간). 연산당 비용은 hazard pointer보다 싸지만(핀 플래그 하나뿐), 핀한
  채로 멈춰 버린 스레드는 모든 회수를 막는다 — 무한한 메모리 증가. 변종: 정지 상태
  기반(QSBR)과 구간 기반 회수.
- **이것들이 프로버넌스와 어떻게 이어지는가.** 회수 기법(hazard pointer, EBR)은
  정확히 해제된 객체의 *저장소를 살려 두기* 위해 — 그 프로버넌스를 유효하게 유지하기
  위해 — 존재한다. 어떤 스레드도 더는 참조할 수 없을 때까지 말이다. CAS + 태그는 공유
  *워드*를 일관되게 유지하지만 그것이 가리키는 대상의 수명에 대해서는 아무 말도 하지
  않는다. 그래서 올바른 락프리 스택은 보통 **둘 다** 필요하다: 태그(head 워드의 ABA용)
  *그리고* SMR 기법(pop된 노드의 안전한 회수용). `proven`의 pool이나 arena는 둘 중
  아무것도 주지 않으므로, 동시성은 그것들 위에 층으로 쌓아야지 가정해서는 안 된다.

### 3.5 `proven`에서의 안전한 패턴

- **스레드별 arena/pool.** 각 스레드에 자신의 `proven_arena_t` /
  `proven_pool_t`를 준다. 공유가 없으면 경쟁도 없고 스레드 간 프로버넌스도 없다 —
  가장 단순하게 올바른 설계이며, 보통 가장 빠르기도 하다.
- **스레드 간 alloc/free에는 힙을 쓰되, 객체는 동기화하라.** 스레드 A가 할당하고
  스레드 B가 `proven_heap_allocator()`로 해제하는 것은 괜찮다. 하지만 둘이 잠금 없이
  같은 `proven_array_t`/`proven_u8str_t`를 변경하는 것은 괜찮지 *않다*.
- **happens-before 간선과 함께 소유권을 넘겨라.** 포인터를 다른 스레드로 넘길 때는
  뮤텍스, release/acquire 순서를 가진 원자 연산, 또는 (job system 같은) 큐를 통해서
  넘겨, 한 번에 한 스레드만 그것을 관찰하게 하라. 이렇게 하면 각 할당의 프로버넌스가
  한 스레드 안에 갇히고, 생산자의 쓰기가 소비자에게 가시화된다.
- **빌려온 뷰를 스레드 간에 넘기지 말라.** 빌림이 지속되는 내내 소유자가 성장/이동/해제
  하지 않는다고 보장되지 않는 한 그렇다 — 그리고 여기서 성장은 항상 재배치임을 기억하라.
- **꼭 arena/pool을 공유해야 한다면 감싸라.** 여러분 자신의 뮤텍스로 감싸거나(또는
  §7.4의 도구로 진짜 락프리 allocator를 만들거나) 하라. 내장된 것들은 한 번에 한
  소유자를 가정한다.

## 4. 별칭 계층

### 왜 두 번째 이름 집합이 존재하는가

`proven_u8str_view_slice`는 26자다. 그런 함수를 스무 번 호출하는 파일에서 접두사는 한 줄의
3분의 1을 차지하고, 정작 달라지는 부분 — 여러분이 실제로 읽는 부분 — 은 남은 자리에 눌려 들어간다.

C에서 접두사는 장식이 아니다. 이 언어에는 함수를 위한 전역 이름 공간이 하나뿐이므로, `slice`나
`create`를 내보내는 라이브러리는 언젠가 프로그램이 링크하는 다른 무언가와 충돌하게 된다. 접두사는
C 라이브러리를 다른 것들과 안전하게 조합할 수 있게 해 주는 장치이며, 진지한 C 라이브러리마다
접두사가 있는 이유다.

`alias_xcv.h`는 그 타협안이다: 모든 공개 심벌에 대한 더 짧은 `xcv_` 표기를, 여러분이 선택해서
포함하는 헤더에 담았다. 포함하면 `xcv_u8str_view_slice`가 동작하고, 포함하지 않으면 짧은 이름은
아예 존재하지 않으므로 무엇과도 충돌하지 않는다.

**정규(canonical) 이름은 여전히 진실의 원천으로 남는다** — ABI에 대해서도, 이 매뉴얼에 대해서도,
테스트에 대해서도 그렇다. 별칭 계층은 표기이지 API가 아니다: 긴 이름으로 내보내지 않은 것은 짧은
이름으로도 내보내지 않으며, 완전성 게이트(`tests/test_docs_alias_completeness.c`)는 공개 함수가
별칭 없이 추가되면 빌드를 실패시킨다. 구멍 난 별칭 계층은 아예 없느니만 못하기 때문이다 — 그것을
채택한 호출자는 컴파일 오류를 하나씩 만나며 그 구멍들을 발견하게 된다.

잘못됨 — 헤더를 포함하지 않고 별칭을 기대하기:

```text
#include "proven.h"
xcv_u8str_view_t v;   /* wrong: proven.h does not pull in the alias layer */
```

잘못됨 — 한 파일에서 같은 개념에 두 표기를 섞기:

```text
proven_result_u8str_t s = xcv_u8str_create(alloc, 32);   /* wrong: pick one and stay with it */
```

둘 다 컴파일된다. 둘 다 좋은 생각은 아니다: 이제 독자는 함수 하나를 따라가기 위해 두 어휘를 모두
알아야 한다.

정규 헤더 다음에 포함하라:

```c
#include "proven.h"
#include "proven/alias_xcv.h"

xcv_allocator_t heap = xcv_heap_allocator();
(void)heap;
xcv_println("{}", XCV_ARG(123));
```

별칭 설계 규칙:

- 별칭은 전처리기 편의 기능이지 별도의 ABI가 아니다.
- 정규 API가 진실의 원천으로 남는다.
- 프로젝트가 자체적인 더 짧은 로컬 접두사를 원한다면 `alias_xcv.h`를 템플릿으로 취급하라.
- 공개 API 이름이 추가되거나 제거될 때 별칭 테스트를 동기화 상태로 유지하라.

별칭 매크로 계열:

- 포매팅: `XCV_ARG`, `XCV_ARG_CSTR`, `XCV_ARG_CSTR_N`, `XCV_FMT_IS_OK`.
- 컨테이너: `XCV_ARRAY_*`, `XCV_RING_*`, `XCV_MAP_*`, `XCV_LIST_*`.
- 오류 및 산술: `XCV_ERR_*`, `XCV_IS_OK`, `XCV_CKD_*`.
- 코루틴: `XCV_CORO_*`.
- 파일시스템 및 sysio: `xcv_fs_*`, `xcv_print*`, `xcv_scan*` 별칭.
- 타입 및 allocator: `xcv_*_t` 타입 별칭과 allocator 별칭.

소스에 근거한 완전한 별칭 테이블은 [7장: 별칭 인덱스](manual-07-alias-xcv-index-ko.md)를 참조하라.

## 5. PAL 계약

### 왜 syscall을 격리했는가

운영체제를 건드리는 모든 라이브러리는 OS별 코드를 어디에 둘지 결정해야 한다. 흔한 답은 "필요한
곳마다"이고, 그러면 소스 곳곳에 `#ifdef _WIN32`가 흩뿌려진다. 그것도 동작은 하지만, 누적되는 두
가지 비용이 있다: 이 라이브러리가 OS에 실제로 무엇을 요구하는지 아무도 알 수 없고, 이식은 모든
파일을 감사한다는 뜻이 된다.

이 라이브러리는 그 전부를 한 디렉터리에 넣는다. **`platform/`은 syscall을 하는 유일한
장소다.** `src/proven/` 안의 모든 것은 PAL을 통해 호출하는 이식 가능한 C이며, 그 결과:

- **요구 사항 목록이 곧 파일 목록이다.** 이 라이브러리가 운영체제에 요구하는 것은 정확히
  `proven_sys_*` 함수들의 집합이다 — 숨은 것도 없고, 뒤늦게 발견될 것도 없다.
- **이식의 범위가 유계다.** 새 대상은 `platform/`을 다시 구현한다. `src/proven/` 안의 것은
  아무것도 바뀌지 않고, 이식 가능한 절반을 검사하는 테스트들은 계속 통과한다.
- **Freestanding은 특수 사례가 아니라 같은 메커니즘이다.** 호스티드 PAL 파일 없이 빌드하면
  남는 것이 이식 가능한 코어다. [freestanding 가이드](manual-freestanding-ko.md)가 별도의
  이식 작업이 아니라 어떤 파일을 빼는지의 목록인 이유가 그것이다.

PAL은 **내부**다. `proven_sys_*` 함수는 공개 API가 아니며 시그니처가 바뀔 수 있다. 여러분이
호출할 것은 `fs.h`, `sysio.h`, `time.h`, `random.h`의 공개 래퍼다. 심벌 게이트도 이를 알고
있어서 PAL을 "문서화되어야 한다"는 요구에서 면제한다. 바로 그것이 여러분이 대상으로 프로그래밍할
표면이 아니기 때문이다.

잘못됨 — 애플리케이션 코드에서 PAL을 직접 호출하기:

```text
proven_sys_fs_open(path, flags);   /* wrong: internal. Use proven_fs_open. */
```

오류 변환, 인자 검증, 그리고 다음 릴리스에서도 그 호출이 계속 동작한다는 보장을 모두 잃는다.

PAL 영역:

- `proven_sys_mem`: 힙 할당 백엔드.
- `proven_sys_fs`: 파일, 디렉터리, 경로, 링크, 권한, 잠금.
- `proven_sys_time`: 클럭, 슬립, 시간 분해.
- `proven_sys_env`: 환경 변수.
- `proven_sys_thread`: job system을 위한 스레드와 동기화.
- `proven_sys_io`: 표준 스트림 I/O.
- `proven_sys_math`: 필요한 경우의 수학 헬퍼.

공개 애플리케이션 코드는 고수준 API를 선호해야 한다:

- 파일시스템 연산에는 `proven_fs_*`.
- 콘솔 I/O에는 `proven_sysio_*`와 print/scan 매크로.
- 시간에는 `proven_time_*`.
- 스레드 작업에는 `proven_job_*`.

PAL 내부 예외: 스레드 수명 주기 코드는 불투명한 OS 메타데이터를 진입점을 넘어 살려 두기 위해 내부 플랫폼 할당이 필요할 수 있다. 이 예외는 PAL 코드 안에 머물러야 하며 핵심 컨테이너 API로 새어 나가서는 안 된다.

## 6. Freestanding 부분집합

`PROVEN_FREESTANDING`는 OS가 없거나 libc가 최소인 대상을 위한 축소된 부분집합을 빌드한다. 현재 freestanding 빌드는 또한 다음을 정의한다:

```sh
-DPROVEN_FREESTANDING -DPROVEN_FMT_NO_FLOAT -DPROVEN_NO_U16STR -ffreestanding
```

현재 freestanding 구성에서 사용 가능한 모듈:

- `types`
- `error`
- `memory`
- `align`
- `allocator`
- `arena`
- `pool`
- `buffer`
- `array`
- `list`
- `ring`
- `map`
- `algorithm`
- `u8str`
- `coro`
- `scan`
- 부동소수점 포매팅이 없는 `fmt`
- `panic`

제외되거나 스텁 처리된 모듈:

- `heap`: 컴파일되지만, `proven_heap_allocator()`는 유효하지 않은 0 allocator를 반환한다.
- `u16str`: 현재 freestanding 빌드에서 `PROVEN_NO_U16STR`에 의해 제외됨.
- `time`: 부분적. `src/proven/time.c`는 컴파일되므로 `proven_time_breakdown()`과
  datetime 포매터는 사용 가능하지만, 클럭 백엔드
  (`platform/proven_sys_time.c`)는 포함되지 않는다 — 따라서 `proven_time_now()`,
  `proven_time_now_datetime()`, `proven_time_sleep()`은 링크할 구현이
  없다.
- `fs`, `sysio`, `mmap`, `job`: 호스티드/PAL 서비스로, 현재 freestanding 부분집합에서 제외됨.

정확한 소스 목록과 명령 예제는 `manual-freestanding-ko.md`를 참조하라.

## 7. 크로스 컴파일

### 왜 실행할 수도 없는 대상용으로 빌드하는가

이식 가능하다고 주장하는 라이브러리는 조용히 썩어 가는 주장을 하고 있는 것이다. 포인터가
64비트라고, `char`가 부호 있다고, 정렬되지 않은 로드가 괜찮다고 가정한 코드는 그것이 작성된
기계에서는 완벽하게 컴파일되고 여섯 달 뒤 ARM 보드에서 깨진다 — 그리고 그것을 깨뜨린 커밋은
무해해 보였다.

`./nob cross`는 아무것도 실행하지 않고 매트릭스의 모든 대상에 대해 라이브러리를 컴파일한다.
약해 보이지만 그렇지 않다: **대부분의 이식성 실패는 컴파일 타임 실패다.** 가정한 폭이 아닌 타입,
없는 인트린식, 대상이 실제로 강제하는 정렬 요구, 거기에는 존재하지 않는 헤더 — 전부 컴파일러에서
실패한다. 이 기계에서, 몇 초 만에, 그것을 도입한 커밋에서.

컴파일 전용 검사가 잡을 수 없는 것은 동작이다: 엔디안 버그, 런타임에만 일어나는 정렬 폴트, 그리고
타이밍에 의존하는 모든 것. 그것들에는 실제 하드웨어나 에뮬레이터가 필요하고, 이 매트릭스는 그런
척하지 않는다. 이것은 이식성 테스트의 값싼 절반이며, 비싼 절반을 결코 하지 않는 대신 모든 빌드에서
돌아간다.

크로스 빌드는 또한 freestanding 프로파일이 실제 대상 — 그중에도 Cortex-M과 RISC-V — 에 대해
실전으로 검증되는 곳이다. 그래서 "운영체제 없음"이라는 주장은 가이드의 한 문단이 아니라 컴파일러가
검사한다.

잘못됨 — 크로스 매트릭스가 초록색이라고 해서 라이브러리가 그 대상에서 동작한다는 증거로 취급하기:

```text
/* wrong: ./nob cross compiles and links objects. It does not execute anything.
   Endianness, alignment faults and real timing still need the hardware. */
```

빌드 드라이버 명령은 다음과 같다:

```sh
./nob cross -build-root /home/user/work/build/proven_c_lib
```

현재 대상 범주:

- 네이티브 GCC 호스티드.
- 사용 가능한 경우 네이티브 Clang 호스티드.
- Linux AArch64 호스티드 컴파일 전용.
- Linux ARM 하드플로트 호스티드 컴파일 전용.
- `i686-linux-gnu-gcc` 또는 `gcc -m32`를 통한 Linux i686 호스티드 컴파일 전용.
- MinGW를 통한 Windows x86_64 및 i686 WinAPI 컴파일 전용.
- ARM Cortex-M freestanding 컴파일 전용.
- RISC-V ELF freestanding 컴파일 전용.

규칙:

- 없는 선택적 컴파일러는 건너뛴다.
- 실제 컴파일 오류는 명령을 실패시킨다.
- 크로스 컴파일은 컴파일 전용이다. 런타임 검증이 아니다.
- 런타임 동작은 여전히 대상 러너, 에뮬레이터, 장치, 또는 OS 환경이 필요하다.

## 8. 예제와 오용 사례

### 코루틴 매크로는 하나의 소스 라인을 공유해서는 안 된다

잘못됨:

```text
PROVEN_CORO_YIELD(&c->coro); PROVEN_CORO_YIELD(&c->coro);
/* wrong: both use the same __LINE__ value */
```

올바름:

```text
PROVEN_CORO_YIELD(&c->coro);
PROVEN_CORO_YIELD(&c->coro);
```

### 코루틴 지역 변수는 평범한 지역 변수이다

스택리스 코루틴은 호출자에게 반환된다. yield를 넘어 값이 유지되어야 하는 지역 변수는 코루틴 함수 안의 자동 지역 변수가 아니라 코루틴 상태 객체 안에 있어야 한다.

잘못됨:

```text
static proven_i32 next(Task *t) {
int temporary = 0;
PROVEN_CORO_BEGIN(&t->coro);
temporary = 42;
PROVEN_CORO_YIELD(&t->coro);
use_int(temporary); /* wrong assumption: ordinary local lifetime/state is not persistent */
PROVEN_CORO_END(&t->coro);
}
```

올바름:

```text
typedef struct Task {
proven_coro_t coro;
int temporary;
} Task;
```

### Job destroy는 제출자와 경쟁해서는 안 된다

잘못됨:

```text
/* thread A */
proven_job_system_destroy(sys);

/* thread B at the same time */
proven_job_submit(sys, work, arg); /* wrong: external synchronization missing */
```

올바름:

```text
stop_producer_threads();
join_producer_threads();
proven_job_system_close(sys);
proven_job_system_destroy(sys);
```

### Job 인자 수명

잘못됨:

```text
void submit_bad(proven_job_sys_t *sys) {
int value = 10;
proven_job_submit(sys, work, &value);
} /* wrong: value may be dead before work runs */
```

올바름:

```text
JobData *data = allocate_job_data();
proven_job_submit(sys, work_and_free_data, data);
```

### 별칭 계층은 정규 문서를 가려서는 안 된다

잘못됨:

```text
/* Document only xcv_map_t and forget proven_map_t. */
```

올바름: 정규 `proven_` API를 먼저 문서화한 다음, 별칭을 선택적 로컬 표기로 언급하라.

### 다루어진 예제: 경계가 있는 job system

테스트 스위트가 컴파일하고 실행한다. 계약이 요구하는 순서에 주목하라: submit, 그다음 close, 그다음 destroy. `proven_job_system_destroy`는 `proven_job_submit`과 경쟁해서는 안 된다.

<!-- example: manual/examples/ex_06_job.c -->
```c
#include <stdatomic.h>

/*
 * The job system: worker threads plus a bounded lock-free queue. It orders the
 * *handoff* of work - it does not synchronize the data the work touches. That is
 * why the counter below is an atomic and not a plain int: two jobs incrementing
 * the same variable is a data race unless the caller says otherwise.
 *
 * The lifecycle is a straight line, and it is not optional:
 *
 *     init -> submit... -> close -> destroy
 *
 * destroy must not race with submit. Nothing in the library enforces that; the
 * caller has to stop its producers first. Here there is only one producer - this
 * thread - so "stop the producers" means "finish the submit loop before closing".
 */

#define JOB_COUNT 64

static void increment(void *arg) {
    atomic_int *counter = arg;
    /* relaxed is enough: we only need the total to be right, not to order anything
     * against it. The join inside destroy is what publishes the result to us. */
    atomic_fetch_add_explicit(counter, 1, memory_order_relaxed);
}

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    proven_job_sys_t *sys = NULL;
    /* Queue capacity must be a power of two - the ring maps a sequence number to a
     * slot with a mask. Sized above JOB_COUNT so a submit cannot find the ring
     * full even if every worker is still starting up. */
    proven_err_t err = proven_job_system_init(alloc, 4, 128, &sys);
    EXAMPLE_REQUIRE(proven_is_ok(err), "starting a job system with 4 workers should succeed");
    if (!proven_is_ok(err)) return 1;

    /* Lives until after destroy: a job's arg must outlive the job, and jobs run
     * until destroy has drained the queue. */
    atomic_int counter = 0;

    proven_size_t submitted = 0;
    for (proven_size_t i = 0; i < JOB_COUNT; ++i) {
        /* submit returns false when the ring is full or the system is closed - it
         * never blocks and never drops work silently. Ignoring the answer is how
         * you lose jobs, which is why it is [[nodiscard]]. */
        if (!proven_job_submit(sys, increment, &counter)) {
            /* A real caller would back off and retry, or run the job inline with
             * proven_job_execute_one. Here a full ring means the sizing above is
             * wrong, so say so rather than paper over it. */
            EXAMPLE_REQUIRE(false, "the queue was sized to hold every job");
            break;
        }
        ++submitted;
    }

    /* This thread is the only producer, and it is done submitting - so it is safe
     * to close. close makes every later submit fail; jobs already queued still run. */
    proven_job_system_close(sys);

    /* destroy blocks until the queue is empty and every worker has been joined.
     * That join is the synchronization point: after destroy returns, every memory
     * effect of every job is visible to this thread. Reading `counter` before this
     * line would be reading a value the workers are still writing. */
    proven_job_system_destroy(sys);

    int ran = atomic_load(&counter);
    EXAMPLE_REQUIRE(submitted == JOB_COUNT, "every job should have been accepted");
    EXAMPLE_REQUIRE(ran == (int)submitted, "every submitted job should have run exactly once");

    printf("submitted %zu jobs, %d ran\n", (size_t)submitted, ran);

    return EXAMPLE_OK();
}
```

### 다루어진 예제: 스택리스 코루틴

테스트 스위트가 컴파일하고 실행한다. 모두를 잡는 규칙: 지역 변수는 yield를 넘어 살아남지 못하므로, 코루틴이 서스펜션을 넘어 필요로 하는 모든 상태는 자신의 구조체 안에 있어야 한다.

<!-- example: manual/examples/ex_06_coro.c -->
```c
/*
 * A stackless coroutine is a switch statement in disguise: BEGIN opens a
 * switch on the saved state, each YIELD records __LINE__ as a resume label and
 * *returns*, and the next call re-enters the function from the top and jumps
 * straight back to that label.
 *
 * Everything that follows comes from that one fact:
 *
 *   - Locals do NOT survive a yield. The function returned; its stack frame is
 *     gone. Anything that must persist lives in the coroutine's own struct - which
 *     is what `value` and `remaining` are doing below.
 *   - Two coroutine macros must not share a source line (they would collide on
 *     __LINE__).
 *   - It cannot yield from a helper it calls: there is no stack to suspend.
 *
 * The payoff is that a suspended coroutine costs exactly its struct - four bytes
 * of state plus whatever you put next to it - and no thread, no stack, no context
 * switch.
 */

typedef struct {
    proven_coro_t coro;
    /* The generator's state. These would be `int i` locals in a normal loop; here
     * they have to be fields, or they would be reset to their initial values on
     * every resume and the loop would never end. */
    int value;
    int remaining;
} squares_t;

/* A coroutine returns proven_i32: 0 = suspended (call me again), 1 = done. */
static proven_i32 squares_next(squares_t *g) {
    PROVEN_CORO_BEGIN(&g->coro);

    g->remaining = 5;
    g->value = 1;

    while (g->remaining > 0) {
        g->value = g->value * g->value;
        PROVEN_CORO_YIELD(&g->coro);      /* the caller reads g->value here */
        g->value = g->value + 1;          /* resumes exactly on this line */
        g->remaining -= 1;
    }

    PROVEN_CORO_END(&g->coro);
}

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    /* The coroutine owns no memory, so there is nothing to destroy - but the values
     * it produces have to go somewhere, and that string does have an owner. */
    proven_result_u8str_t out = proven_u8str_create(alloc, 32);
    EXAMPLE_REQUIRE(proven_is_ok(out.err), "creating the output string should succeed");
    if (!proven_is_ok(out.err)) return 1;

    squares_t gen = {0};
    PROVEN_CORO_INIT(&gen.coro);   /* unconditional, exactly once, before the first call */

    int produced = 0;
    int last = 0;

    /* Drive it to completion. squares_next returns 1 on the call that runs off the
     * end of the body - that call produces no value, so the loop body only runs
     * while it returned 0. */
    while (!squares_next(&gen)) {
        proven_fmt_result_t r = proven_u8str_append_fmt_grow(alloc, &out.value, "{} ",
                                                             PROVEN_ARG(gen.value));
        EXAMPLE_REQUIRE(PROVEN_FMT_IS_OK(r), "appending a generated value should succeed");
        last = gen.value;
        ++produced;
    }

    /* Done is sticky: the state is -1 and stays there. Calling it again would just
     * return 1 without re-running the body. */
    EXAMPLE_REQUIRE(PROVEN_CORO_IS_DONE(&gen.coro), "the generator should have finished");
    EXAMPLE_REQUIRE(squares_next(&gen) == 1, "a finished coroutine stays finished");

    /* 1, then (1+1)^2 = 4, then (4+1)^2 = 25, then 676, then 458329. */
    EXAMPLE_REQUIRE(produced == 5, "the generator yields once per iteration");
    EXAMPLE_REQUIRE(last == 458329, "the state carried across every yield");
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_as_view(&out.value),
                                         PROVEN_LIT("1 4 25 676 458329 ")),
                    "the generated sequence should be exactly this");

    printf("squares: %s\n", proven_u8str_as_cstr(&out.value));

    proven_u8str_destroy(alloc, &out.value);
    return EXAMPLE_OK();
}
```
