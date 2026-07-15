# 6장: 실행, 별칭, PAL, Freestanding, 크로스 빌드

이 장은 `coro.h`, `job.h`, `alias_xcv.h`, PAL 계약, freestanding 모드, 플랫폼 빌드 관련 사항을 다룬다.

## 목차

1. [스택리스 코루틴](#1-스택리스-코루틴)
2. [Job system](#2-job-system)
3. [별칭 계층](#3-별칭-계층)
4. [PAL 계약](#4-pal-계약)
5. [Freestanding 부분집합](#5-freestanding-부분집합)
6. [크로스 컴파일](#6-크로스-컴파일)
7. [예제와 오용 사례](#7-예제와-오용-사례)

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
이 형태를 컴파일한 버전은 [7절](#7-예제와-오용-사례)의 다루어진 예제,
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
실제로 필요로 하는 원자 연산을 포함한 컴파일된 버전은 [7절](#7-예제와-오용-사례)의
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

## 3. 별칭 계층

`include/proven/alias_xcv.h`는 더 짧은 선택적 별칭 접두사를 제공한다. 이는 정규(canonical) `proven_` 및 `PROVEN_` 이름을 `xcv_` 및 `XCV_` 이름으로 매핑한다.

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

## 4. PAL 계약

플랫폼 추상화 계층(Platform Abstraction Layer)은 `platform/` 아래에 있다. 이는 이식 가능한 라이브러리 코드를 OS 서비스로 연결한다.

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

## 5. Freestanding 부분집합

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

정확한 소스 목록과 명령 예제는 `manual-freestanding.md`를 참조하라.

## 6. 크로스 컴파일

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

## 7. 예제와 오용 사례

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
