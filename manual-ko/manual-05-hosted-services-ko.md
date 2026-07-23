# Chapter 5: Hosted Services

**5부 — 운영체제와 대화하기. 선행 조건: 2부
([1](manual-01-foundation-ko.md), [2](manual-02-allocation-ko.md), [3](manual-03-strings-text-ko.md)).**
**이 장을 마치면** 충돌 시에도 데이터를 잃지 않고 파일을 읽고 쓰며, 입력을 한 줄씩 읽고,
작업에 맞는 무작위성을 생성하고, 벽시계 시간과 경과 시간을 구별할 수 있다.

이 장에서는 `fs.h`, `sysio.h`, `mmap.h`, `time.h`, `stream.h`, `random.h`를 다룬다. 이 안의 모든
것은 운영체제를 필요로 한다: 이 장은 [freestanding](manual-freestanding-ko.md) 빌드에 적용되지
않는 매뉴얼의 유일한 부분이다.

이 API들은 호스티드 플랫폼 지원이 필요하며, 현재의 freestanding 하위 집합에서는 제외된다.

## 목차

1. [파일시스템 API](#1-filesystem-api)
2. [시스템 I/O와 환경](#2-system-io-and-environment)
3. [메모리 매핑](#3-memory-mapping)
4. [Time API](#4-time-api)
5. [예제와 오용 사례](#5-examples-and-misuse-cases)
6. [트리 순회](#walking-a-tree)
7. [스트림: writer와 reader](#streams-writers-and-readers)
8. [표준 스트림](#the-standard-streams)
9. [용도별 무작위성](#randomness-by-use-case)

## 1. Filesystem API

파일시스템 계층은 플랫폼 파일 핸들, 경로, 디렉터리 목록, 메타데이터, 권한, 링크, 잠금을 감싼다.

원시 파일시스템 헬퍼는 신뢰할 수 없는 경로를 정제하지 않고, 루트 감금(root confinement)을 강제하지 않으며, symlink-race TOCTOU를 방어하지 않는다. 신뢰할 수 없는 경로를 받아들이는 호출자는 API를 사용하기 전에 그것을 검증해야 한다.

### 구조체와 열거형

```text
typedef struct {
    union {
        void *ptr;
        int fd;
    } internal;
} proven_fs_handle_t;

typedef proven_fs_handle_t proven_file_t;
```

의도: POSIX나 Win32 세부사항을 상위 계층에 노출하지 않으면서 플랫폼 파일 핸들을 표현한다.

```text
typedef enum {
    PROVEN_FS_READ   = 1 << 0,
    PROVEN_FS_WRITE  = 1 << 1,
    PROVEN_FS_APPEND = 1 << 2,
    PROVEN_FS_CREATE = 1 << 3,
    PROVEN_FS_TRUNC  = 1 << 4,
    PROVEN_FS_CREATE_NEW = 1 << 5
} proven_fs_mode_t;
```

파일 모드 플래그는 조합할 수 있다. 예:

- `PROVEN_FS_READ`
- `PROVEN_FS_WRITE | PROVEN_FS_CREATE | PROVEN_FS_TRUNC`
- `PROVEN_FS_WRITE | PROVEN_FS_APPEND | PROVEN_FS_CREATE`

```text
typedef enum {
    PROVEN_FS_TYPE_FILE,
    PROVEN_FS_TYPE_DIR,
    PROVEN_FS_TYPE_OTHER
} proven_fs_type_t;
```

```text
typedef struct {
    proven_u8str_t name;
    proven_fs_type_t type;
    proven_size_t size;
} proven_fs_entry_t;
```

디렉터리 엔트리는 `name`을 owned로 소유한다. `proven_fs_list()`가 반환한 배열에는 `proven_fs_list_destroy()`를 사용하라.

```text
typedef struct {
    proven_err_t err;
    proven_file_t value;
} proven_result_file_t;
```

```text
typedef enum {
    PROVEN_FS_PERM_OWNER_R = 1 << 8,
    PROVEN_FS_PERM_OWNER_W = 1 << 7,
    PROVEN_FS_PERM_OWNER_X = 1 << 6,
    PROVEN_FS_PERM_GROUP_R = 1 << 5,
    PROVEN_FS_PERM_GROUP_W = 1 << 4,
    PROVEN_FS_PERM_GROUP_X = 1 << 3,
    PROVEN_FS_PERM_OTHER_R = 1 << 2,
    PROVEN_FS_PERM_OTHER_W = 1 << 1,
    PROVEN_FS_PERM_OTHER_X = 1 << 0,
    PROVEN_FS_PERM_DEFAULT = PROVEN_FS_PERM_OWNER_R | PROVEN_FS_PERM_OWNER_W |
                             PROVEN_FS_PERM_GROUP_R | PROVEN_FS_PERM_OTHER_R
} proven_fs_perms_t;
```

```text
typedef enum {
    PROVEN_FS_LOCK_SHARED,
    PROVEN_FS_LOCK_EXCLUSIVE,
    PROVEN_FS_LOCK_UNLOCK
} proven_fs_lock_type_t;
```

```text
typedef struct {
    proven_size_t size;          /* size in bytes (0 for directories on some hosts) */
    proven_fs_type_t type;       /* PROVEN_FS_TYPE_FILE / _DIR - there is no symlink type */
    proven_fs_perms_t perms;     /* the nine permission bits only; the file type is in `type` */
    proven_i64 created_at;       /* always 0: no birth time is queried - see below */
    proven_i64 modified_at;      /* last-modification time, seconds since the Unix epoch */
    unsigned long long dev;      /* device id (POSIX st_dev; 0 where unavailable) */
    unsigned long long ino;      /* inode number (POSIX st_ino; 0 where unavailable) */
    unsigned long long uid;      /* owner id  (POSIX st_uid; 0 on Windows — no uid/gid) */
    unsigned long long gid;      /* group id  (POSIX st_gid; 0 on Windows) */
} proven_fs_stat_t;
```

`uid`/`gid`(v26.06.22a에서 추가됨)는 POSIX 소유자 및 그룹 id를 담는다. 둘 다 Windows에서는 `0`인데,
Windows에는 `uid`/`gid` 개념이 없기 때문이다. `ls -l` 스타일의 소유자/그룹 열을 표시할 때는
호스트의 `getpwuid`/`getgrgid`로 이들을 이름으로 해석하라.

한 필드는 보이는 것보다 좁고, 다른 한 필드에는 날카로운 모서리가 있다:

- `created_at`은 **항상 0**이다. 평범한 `stat()`에는 이식 가능한 birth time이 없으며,
  PAL은 그것을 요청하지 않는다. `modified_at`만이 실제 타임스탬프를 담는다.
- `type`은 일반 파일이면 `FILE`, 디렉터리면 `DIR`, 그 밖의 모든 것—FIFO, 소켓, 디바이스,
  끊어진(dangling) symlink—에 대해서는 **`PROVEN_FS_TYPE_OTHER`**이다. (v26.07.13a 이전까지는
  이 모든 것이 `FILE`로 stat되어, 호출자에게 그것들을 열어 바이트를 읽을 수 있다고 알려주었다.
  끊어진 symlink는 애초에 열 수 없다.)

  `type`은 여기서도, 그리고 디렉터리 순회에서도 **symlink를 따라간다**. 그것이 두 값을
  일치시키는 원리다: 일반 파일을 가리키는 symlink는 `FILE`이고, 디렉터리를 가리키는 symlink는
  `DIR`이다. 이어지는 모서리는 실재한다—**재귀 순회기는 루프에 빠질 수 있다**. 조상을 가리키는
  symlink는 사이클이고 그 type이 `DIR`이라고 말하기 때문이다. 깊이 제한을 두거나, `(dev, ino)`
  쌍을 기억해 두고 이미 본 것으로는 내려가기를 거부하라.

```c
proven_fs_stat_t st;
if (proven_is_ok(proven_fs_stat(scratch, PROVEN_LIT("/etc/hosts"), &st))) {
    proven_println("size={} uid={} gid={}",
        PROVEN_ARG(st.size), PROVEN_ARG(st.uid), PROVEN_ARG(st.gid));
}
```

### 매크로

| 매크로 | 의도 |
|---|---|
| `PROVEN_FS_PATH_SEP` | 라이브러리 수준에서 선호되는 구분 문자, 현재는 `'/'`. |

### 파일 함수

| API | 의도 | 반환 |
|---|---|---|
| `proven_fs_open(scratch, path, mode)` | 파일 경로를 연다. `scratch`는 경로 변환에 사용된다. | `proven_result_file_t`. |
| `proven_fs_close(file)` | 파일 핸들을 닫는다. | **에러를 반환한다.** 쓰기를 한 파일에서는 close *자체*가 쓰기의 일부다: NFS, CIFS 및 쿼터를 강제하는 파일시스템은 write-back 실패를 오직 이곳에서만 보고한다. 읽기만 한 파일에서는 `(void)`로 무시해도 괜찮다. |
| `proven_fs_read(file, dest)` | 가변 슬라이스로의 단일 읽기 시도. | `proven_result_size_t`. |
| `proven_fs_write(file, src)` | 바이트 view로부터의 단일 쓰기 시도. | `proven_result_size_t`. |
| `proven_fs_write_all(file, src)` | 모든 바이트가 쓰이거나 에러가 발생할 때까지 재시도. | `proven_err_t`. |
| `proven_fs_size(file)` | 열린 파일의 크기를 조회. | `proven_result_size_t`. |
| `proven_fs_rename(scratch, src, dest)` | 경로 이름 변경 또는 이동. | `proven_err_t`. |
| `proven_fs_remove(scratch, path)` | 파일 제거. | `proven_err_t`. |
| `proven_fs_copy(temp_alloc, src, dest)` | 임시 버퍼 할당을 사용해 파일 복사. | `proven_err_t`. |
| `proven_fs_mkdir(scratch, path)` | 디렉터리 생성. | `proven_err_t`. |
| `proven_fs_rmdir(scratch, path)` | 빈 디렉터리 제거. | `proven_err_t`. |
| `proven_fs_list(alloc, path)` | 디렉터리를 `proven_fs_entry_t`의 `proven_array_t`로 나열. | `proven_result_array_t`. |
| `proven_fs_list_destroy(alloc, list)` | 디렉터리 목록과 엔트리 이름을 파괴. | void. |
| `proven_fs_chmod(scratch, path, perms)` | 권한 설정. | `proven_err_t`. |
| `proven_fs_lock(file, type, wait)` | 파일 잠금 획득/해제. | `proven_err_t`. |
| `proven_fs_stat(scratch, path, out_stat)` | 메타데이터 채우기. | `proven_err_t`. |

`proven_fs_stat()`은 `perms`에 아홉 개의 권한 비트만 보고하므로, stat의 `perms`를 그대로 `proven_fs_chmod()`에 다시 넘길 수 있다. 예전에는 원시 POSIX `st_mode`를 담았는데, `chmod`가 거부하는 파일 타입 비트가 포함되어 있어서—이 필드의 자명한 용도인 그 왕복(round-trip)이 실제 파일마다 `PROVEN_ERR_INVALID_ARG`로 실패했다. 파일 타입은 `type`에서 읽어라.

| `proven_fs_symlink(scratch, target, linkpath)` | 심볼릭 링크 생성. | `proven_err_t`. |
| `proven_fs_link(scratch, oldpath, newpath)` | 하드 링크 생성. | `proven_err_t`. |
| `proven_fs_is_absolute(path)` | 절대 경로 분류. | bool. |
| `proven_fs_read_all(alloc, path)` | 파일 전체를 EOF까지 할당하여 읽기. | `proven_result_mem_mut_t`. |
| `proven_fs_read_all_u8str(alloc, path)` | 같지만, NUL로 종료되는 owned 문자열로. | `proven_result_u8str_t`. |
| `proven_fs_write_file(scratch, path, data)` | 생성-또는-절단(create-or-truncate) 방식의 파일 전체 쓰기. | `proven_err_t`. |
| `proven_fs_write_file_atomic(scratch, path, data)` | 임시 파일 + rename을 통한 파일 전체 쓰기. | `proven_err_t`. |
| `proven_fs_write_file_durable(scratch, path, data)` | 같지만, 반환 전에 디스크에 기록됨. | `proven_err_t`. |
| `proven_fs_seek(file, offset, whence)` | 파일 위치를 이동. | `proven_result_u64_t`(새 오프셋). |
| `proven_fs_tell(file)` | 현재 위치. | `proven_result_u64_t`. |
| `proven_fs_truncate(file, length)` | 파일의 길이를 설정. O(1). | `proven_err_t`. |
| `proven_fs_pread(file, dest, offset)` | 오프셋에서 읽기; 위치를 이동하지 않는다. | `proven_result_size_t`. |
| `proven_fs_pwrite(file, src, offset)` | 오프셋에서 쓰기; 위치를 이동하지 않는다. | `proven_result_size_t`. |
| `proven_fs_sync(file)` | 이 파일의 데이터를 디스크로 강제(fsync). | `proven_err_t`. |
| `proven_fs_sync_dir(scratch, path)` | 디렉터리의 메타데이터를 디스크로 강제. | `proven_err_t`. |

### 위치, 그리고 atomic과 durable의 차이

**seek할 수 없는 핸들은 그렇다고 말한다.** 파이프, FIFO, 터미널은 `proven_fs_seek`에서
`PROVEN_ERR_IO`가 아니라 `PROVEN_ERR_UNSUPPORTED`를 반환한다. seek 불가능은 호출의 실패가
아니라 그 대상의 속성이며, 그것에 적응하는 코드—스캐너가 그렇다—는 둘을 구별할 수 있어야 한다.

**`pread`와 `pwrite`는 위치를 이동하지 않는다.** 그것이 바로 그들의 목적이다: 하나의 핸들을
공유하는 두 reader는 어느 쪽도 이동시키지 않는 커서를 두고 경합할 수 없다.

**atomic과 durable은 서로 다른 약속이며, 이 둘을 혼동하는 것이 데이터가 손실되는 방식이다.**

- `proven_fs_write_file_atomic`은 *reader*가 절반만 쓰인 파일을 결코 보지 않도록 보장한다.
  전원 차단에 대해서는 아무것도 말하지 않는다: 커널이 여전히 당신의 바이트를 붙들고 있을 수 있고,
  rename이 그것이 가리키는 데이터보다 먼저 디스크에 도달할 수 있다.
- `proven_fs_write_file_durable`은 그 창(window)을 닫는다. 오직 작동하는 하나의 순서로:
  임시 파일을 fsync하고, **그 다음** rename하고, **그 다음** 디렉터리를 fsync한다. 파일은
  sync하되 디렉터리는 하지 않으면, 바이트는 안전하지만 그것을 가리키는 이름은 안전하지 않은
  크래시 창이 남는다—그것이 바로 atomic 쓰기가 막고자 존재하는 그 손상이다.

durable 형태는 저장 장치를 두 번 기다린다. 쓰기를 잃는 것이 기다림보다 나쁠 때는 이것을,
그렇지 않을 때는 atomic 형태를 사용하라.

예전에는 여기에 `proven_sysio_flush`가 있었는데, 그것은 이 어느 것도 아니었다: 존재하지 않는
버퍼를 flush한다고 주장했다. 그것은 **삭제되었다**. 버퍼드 writer의 바이트를 OS로 밀어 넣는 것은
`proven_writer_flush`이고, OS의 바이트를 디스크로 밀어 넣는 것은 `proven_fs_sync`이다. 이 둘은 서로
다른 연산이며, 한 단어가 정직하게 둘 모두를 뜻할 수는 없었다.

중요한 동작:

- `proven_fs_read()`와 `proven_fs_write()`는 단일 연산 API이며 요청된 것보다 적은 바이트를 처리할 수 있다.
- **파일 끝에서의 읽기는 0바이트 성공이 아니라 `PROVEN_ERR_EOF`를 반환한다.** 자명한 방식으로 작성된 루프—
  `if (r.value == 0) break;`—는 결코 그 분기를 타지 않으며, 파일의 끝을 I/O 실패로 취급한다.
  `PROVEN_ERR_EOF`를 명시적으로 검사하라. 이 장 끝의 완성 예제가 그 형태를 보여준다.
- 모든 바이트를 반드시 써야 할 때는 `proven_fs_write_all()`을 사용하라.
- 크기 0의 읽기/쓰기 요청은 non-null 버퍼를 요구하지 않고 0바이트를 처리한 채로 성공해야 한다.
- `proven_fs_is_absolute()`는 POSIX 절대 경로, Windows 드라이브-루트 경로, UNC 경로, 확장된 Windows 경로 형식을 인식한다.
- `proven_fs_read_all()`은 EOF까지 읽는다. 미리 측정된 크기까지 읽는 것이 아니다. 파일이 보고한 크기는 초기 용량의 씨앗(seed)으로만 쓰이므로, 일반 파일은 여전히 한 번의 할당과 한 번의 패스로 읽힌다. 이것이 중요한 이유는 `proven_fs_size()`가 일반 파일이 아닌 모든 것에 대해 0을 보고하기 때문이다: FIFO, 캐릭터 디바이스, `/proc` 엔트리에는 미리 알 수 있는 크기가 없으며, EOF까지 읽는 것만이 그 내용을 얻는 유일한 방법이다. 또한 이는 읽히는 도중 커지는 파일이 조용히 절단되지 않음을 뜻한다. `value.size`는 언제나 실제 바이트 수이며, 빈 소스는 `PROVEN_OK`와 함께 `{ .ptr = NULL, .size = 0 }`을 낳는다.
- `proven_fs_read_all()`과 `proven_fs_read_all_u8str()`은 소스가 보고된 크기를 초과해 커질 경우 `realloc_fn`을 가진 allocator를 필요로 한다. 그렇지 않은(non-growing) allocator는 그 경우 `PROVEN_ERR_UNSUPPORTED`를 반환한다.
- `proven_fs_read_all_u8str()`은 대부분의 호출자가 원하는 파일 전체 읽기다: 결과가 NUL로 종료되므로 `proven_u8str_as_view()`와 `proven_u8str_as_cstr()`가 두 번째 복사 없이 그 위에서 동작한다. 종료 슬롯은 미리 예약되므로 추가 할당 비용이 들지 않는다. 내용은 UTF-8로 검증되지 않는다. `proven_u8str_destroy()`로 해제하라.
- `proven_fs_write_file()`은 atomic이 아니다: reader가 부분적으로 쓰인 파일을 관찰할 수 있고, 쓰기 도중의 실패는 파일을 절단된 채로 남긴다. `proven_fs_write_file_atomic()`은 형제 임시 파일을 쓰고 그것을 대상 위로 rename하므로, 동시 reader는 전체 이전 파일 또는 전체 새 파일 둘 중 하나를 본다. 이는 reader에 대해 atomic이지만 전원 손실에 대해 durable하지는 않다: rename이 데이터보다 먼저 디스크에 도달할 수 있다. durability가 필요할 때는 위에서 설명한 `proven_fs_write_file_durable`(또는 `proven_fs_sync`)로 명시적으로 요청하라.

예:

```c
proven_result_file_t of = proven_fs_open(
    alloc,
    PROVEN_LIT("out.txt"),
    PROVEN_FS_WRITE | PROVEN_FS_CREATE | PROVEN_FS_TRUNC
);
if (proven_is_ok(of.err)) {
    proven_err_t e = proven_fs_write_all(
        of.value,
        proven_mem_view_from_u8(PROVEN_LIT("hello\n"))
    );
    /* The close is part of the write. Close on the failure path too - the handle is
     * ours either way - but do not throw the answer away: on a network filesystem or
     * over quota, close() is the only place the failure appears. */
    proven_err_t ce = proven_fs_close(of.value);
    if (proven_is_ok(e)) e = ce;
    if (!proven_is_ok(e)) {
        proven_eprintln("writing out.txt failed");
    }
}
```

## 2. System I/O and environment

### 표준 스트림

```text
proven_file_t proven_sysio_stdin(void);
proven_file_t proven_sysio_stdout(void);
proven_file_t proven_sysio_stderr(void);
```

목적: 표준 스트림을 `proven_file_t` 핸들로 노출한다. 이들은 또한 writer와 reader이기도 하다—
아래 [표준 스트림](#the-standard-streams)을 보라. 그것이 stdin을 한 줄씩 읽고, stdout을 버퍼링하며,
둘 중 어느 쪽으로든 직접 포매팅할 수 있게 해준다.

### `proven_sysio_scanner_t`

```text
typedef struct {
    proven_file_t file;
    proven_allocator_t alloc;
    proven_u8 *buffer;
    proven_size_t capacity;
    proven_size_t cursor;
    proven_size_t length;
    bool eof;
} proven_sysio_scanner_t;
```

목적: 경계가 있는 스트림 입력을 위한 버퍼드 스캐너로, seek 가능한 파일뿐 아니라 파이프와 stdin에도 안전하다. 토큰이 EOF 전에 현재 로드된 조각의 끝에 도달하면, 스캐너는 버퍼를 다시 채우고 재시도한다. 다시 채운 후에도 버퍼 전체에 들어갈 수 없는 토큰만이 절단되어 받아들여지는 대신 `PROVEN_ERR_OUT_OF_BOUNDS`로 거부된다.

### Sysio 함수와 매크로

| API | 의도 | 반환 |
|---|---|---|
| `proven_sysio_scanner_init(scanner, file, alloc, buffer_capacity)` | 스캐너 버퍼를 할당하고 스트림을 바인딩. | `proven_err_t`. |
| `proven_sysio_scanner_deinit(scanner)` | 스캐너 버퍼를 해제. | void. |
| `proven_sysio_scanner_scan_impl(scanner, fmt, args, args_count)` | 내부 스캐너 엔진. | `proven_err_t`. |
| `proven_sysio_scanner_scan(scanner, fmt, ...)` | 타입 안전 버퍼드 스캔 매크로. | `proven_err_t`. |
| `proven_sysio_print_impl(handle, fmt, args, args_count)` | 내부 출력 엔진. | `proven_err_t`. |
| `proven_sysio_scan_chunk_impl(handle, fmt, args, args_count)` | 한-청크 스캔 엔진. | `proven_err_t`. |
| `proven_print(fmt, ...)` | stdout으로 출력. | `proven_err_t`. |
| `proven_println(fmt, ...)` | 개행과 함께 stdout으로 출력. | `proven_err_t`. |
| `proven_eprint(fmt, ...)` | stderr로 출력. | `proven_err_t`. |
| `proven_eprintln(fmt, ...)` | 개행과 함께 stderr로 출력. | `proven_err_t`. |
| `proven_scan_fmt_from_file(file, fmt, ...)` | 파일의 고정 크기 청크 하나에서 스캔. | `proven_err_t`. |
| `proven_scan_fmt_from_stdin(fmt, ...)` | stdin에서 고정 크기 청크 하나를 스캔. | `proven_err_t`. |
| `proven_env_get(alloc, key)` | 환경 변수를 owned U8 문자열로 읽음. | `proven_result_u8str_t`. |

`proven_sysio_scan_chunk_impl()`은 seek 가능한 파일 입력을 위한 것이다. 최대 하나의 고정 크기 청크를 읽는다. 핸들을 되감을 수 없으면, 읽기 전에 `PROVEN_ERR_UNSUPPORTED`를 반환한다. 완전한 토큰이 준비되기 전에 청크가 가득 차면, `PROVEN_ERR_OUT_OF_BOUNDS`를 반환하고 파일 커서를 청크의 시작으로 되감는다. 문자열 view 목적지도 읽기 전에 `PROVEN_ERR_UNSUPPORTED`로 거부한다. 그런 view는 이 함수의 지역 청크 버퍼를 빌리므로 반환 즉시 무효가 되기 때문이다. 반복적인 버퍼드 스캔이나 빌린 문자열 결과에는 호출자가 버퍼를 소유하는 `proven_sysio_scanner_t`를 사용하라. 그 스캐너가 반환한 문자열 view는 다음 scan 호출이나 스캐너 해제 전까지만 유효하다. 어느 쪽이든 공유 버퍼를 다시 채우거나 압축하거나 해제할 수 있기 때문이다.

예:

```c
proven_println("answer={}", PROVEN_ARG(42));
proven_eprintln("warning: {}", PROVEN_ARG(PROVEN_LIT("low memory")));
```

환경 변수 예. `proven_env_get`은 owned 문자열을 돌려주므로 반드시 파괴해야 한다:

```c
proven_result_u8str_t env = proven_env_get(alloc, PROVEN_LIT("PATH"));
if (proven_is_ok(env.err)) {
    proven_u8str_view_t path = proven_u8str_as_view(&env.value);
    proven_println("PATH is {} bytes", PROVEN_ARG(path.size));
    proven_u8str_destroy(alloc, &env.value);
}
```

## 3. Memory mapping

### 문제: 복사하고 싶지 않은 파일 읽기

`proven_fs_read_all_u8str`는 파일을 당신이 소유하는 메모리로 읽어 들인다. 설정 파일이라면 그것이
정확히 옳다. 하지만 4 GB짜리 데이터베이스, 메모리 매핑된 인덱스, 또는 두 프로세스가 동시에 봐야
하는 파일이라면 세 가지 면에서 틀렸다: 4 GB의 RAM이 필요하고, 읽든 읽지 않든 모든 바이트를
복사하며, 그 복사본은 오직 당신만의 것이다.

메모리 매핑은 대신 운영체제에게 파일의 내용이 어떤 주소에 *나타나게* 해 달라고 요청한다. 미리
복사되는 것은 없다. 당신이 건드리는 페이지만 필요할 때 디스크에서 읽히고, 결코 건드리지 않는
페이지는 결코 읽히지 않는다. 두 프로세스가 같은 파일을 `PROVEN_MMAP_SHARED`로 매핑하면 하나의
페이지 집합을 보게 되므로, 한쪽에서의 쓰기가 다른 쪽에서 보인다.

### 이것의 비용, 그리고 쓰지 말아야 할 때

실패 양상이 오류처럼 보이지 않기 때문에, 이 절은 매뉴얼에서 득실이 가장 첨예한 부분이다:

- **읽기가 fault를 일으킬 수 있다.** 파일이 메모리에 있으면 I/O 오류는 반환값이다. 매핑에서는,
  디스크 읽기가 실패하는 페이지를 건드리면 오류 코드가 아니라 **시그널**(`SIGBUS`)이 전달된다.
  이를 우회하도록 작성할 수 있는 `if`는 없다.
- **잘림(truncation)은 지뢰다.** 매핑한 뒤에 다른 프로세스가 파일을 줄이면, 새 끝을 넘어선
  페이지를 건드리는 것 역시 `SIGBUS`다. 다른 무언가가 잘라낼 수 있는 파일을 매핑하는 것은 어떤
  API도 대신 고쳐 줄 수 없는 방식으로 안전하지 않다.
- **공짜가 아니다.** 매핑을 설정하는 것은 syscall이자 페이지 테이블 작업이며, 첫 접촉마다 페이지
  fault가 난다. 작은 파일이라면 `proven_fs_read_all_u8str`가 그냥 더 빠르다.
- **`proven_mmap_sync`가 지속성의 지점이다.** 공유 매핑에 대한 쓰기는 결국 파일에 도달한다.
  *지금* 디스크에 있어야 한다면, 요청하라.

큰 파일을 드문드문 읽을 때, 프로세스 간에 공유하는 읽기 전용 데이터에, 그리고 큰 파일에 대한
임의 접근에 매핑을 쓰라. 그 밖의 모든 것에는 평범한 읽기를 쓰라.

매핑은 호출자가 소유하는 상태다: `proven_mmap_as_view`는 **매핑 안을 가리키는** view를 건네주므로,
그 view는 `proven_mmap_destroy`가 실행되는 순간 죽는다.

잘못된 예 — 매핑을 파괴한 뒤에 view를 사용하기:

```text
proven_u8str_view_t data = proven_mmap_as_view(m);
proven_err_t e = proven_mmap_destroy(&m);
(void)e;
parse(data);                 /* wrong: those addresses are no longer mapped - SIGSEGV */
```

잘못된 예 — 매핑을 키울 수 있는 버퍼처럼 취급하기:

```text
/* wrong: a mapping is a window onto a file of a fixed size at map time.
   Appending means changing the file and mapping it again. */
```

### 구조체와 열거형

```text
typedef enum {
    PROVEN_MMAP_READ  = 0x01,
    PROVEN_MMAP_WRITE = 0x02,
    PROVEN_MMAP_EXEC  = 0x04
} proven_mmap_prot_t;

typedef enum {
    PROVEN_MMAP_PRIVATE = 0x01,
    PROVEN_MMAP_SHARED  = 0x02
} proven_mmap_flags_t;

typedef struct {
    void *ptr;
    proven_size_t size;
    proven_fs_handle_t file;
    void *internal_handle;
} proven_mmap_t;

typedef struct {
    proven_err_t err;
    proven_mmap_t value;
} proven_result_mmap_t;
```

### 함수

| API | 의도 | 반환 |
|---|---|---|
| `proven_mmap_create(file, offset, size, prot, flags)` | 파일 영역을 매핑. `size == 0`은 EOF까지 매핑한다. 오프셋은 플랫폼의 매핑 세분성(granularity)과 일치해야 한다. | `proven_result_mmap_t`. |
| `proven_mmap_destroy(mmap)` | 영역을 언매핑하고 상태를 정리. | `proven_err_t`. |
| `proven_mmap_sync(mmap)` | 공유된 쓰기 가능 변경분을 저장소로 flush. | `proven_err_t`. |
| `proven_mmap_as_view(mmap)` | 매핑된 메모리를 U8 view로 빌림(borrow). | `proven_u8str_view_t`. |

예:

```c
proven_result_file_t f = proven_fs_open(alloc, PROVEN_LIT("data.bin"), PROVEN_FS_READ);
if (proven_is_ok(f.err)) {
    proven_result_mmap_t mr = proven_mmap_create(
        f.value,
        0,
        0,
        PROVEN_MMAP_READ,
        PROVEN_MMAP_PRIVATE
    );
    if (proven_is_ok(mr.err)) {
        /* The view borrows the mapping: it dies when the mapping is destroyed. */
        proven_u8str_view_t bytes = proven_mmap_as_view(mr.value);
        proven_println("mapped {} bytes", PROVEN_ARG(bytes.size));
        (void)proven_mmap_destroy(&mr.value);
    }
    (void)proven_fs_close(f.value);
}
```

## 4. Time API

### 문제: 서로 다른 두 질문, 하나의 단어

"시간"은 서로 닮았지만 하는 짓은 전혀 다른 두 가지를 뜻한다.

**벽시계(wall clock)**는 *지금 몇 시인가?*에 답한다 — 사용자가 읽는 그것이다. 이 시계는 튀어도
된다: NTP가 보정하고, 서머타임이 옮기고, 관리자가 설정한다. 이것으로 경과 시간을 재면 음수가 나올
수 있는데, 이는 윤초가 있는 날에 나타나고 테스트 중인 누구의 노트북에서도 나타나지 않는 버그다.

**단조(monotonic)** 시계는 *언제부터 얼마나 지났는가?*에 답한다. 앞으로만 움직이며 어떤 달력과도
관계가 없다. 연산의 시간을 잴 때 쓰는 것이 이것이다.

libc는 이 구분을 흐린다. `time()`은 벽시계의 초 단위 정수를 준다 — 무언가를 재기에는 너무 거칠다.
`clock()`은 **CPU** 시간을 재므로, 소켓을 기다리는 프로그램은 시간이 전혀 걸리지 않은 것처럼
보인다. 어느 이름도 자신이 어느 질문에 답하는지 말해 주지 않으며, 둘 다 서로의 용도로 흔히
쓰인다.

### 이 라이브러리는 대신 무엇을 하는가

`proven_time_now()`는 **Unix epoch 이후의 나노초**를 부호 있는 64비트 값으로 반환한다. 빼면 경과
시간이 되고, 분해하면 달력 날짜가 되는 하나의 숫자다 — 실제 작업의 시간을 잴 만큼 충분히 고운
해상도로.

`proven_time_breakdown()`은 그 숫자를 `proven_datetime_t`로 바꾸며, 그 필드는 사람이 쓰는 것들이다:
`month`는 libc의 0-11이 아니라 **1-12**이고, `year`는 1900년 이후의 햇수가 아니라 실제 연도다.
`struct tm`의 그 두 가지 off-by-N 관례는 의도적으로 갈라설 만큼 충분히 많은 버그를 낳았다.

포맷팅은 [3장](manual-03-strings-text-ko.md)과 같은 `{}` 자리표시자를 쓴다: 이름이 필드를 고르고
스펙이 자리를 채우므로, `{month:0>2}`는 너비 2로 0을 채운다. 월과 요일 이름은
`proven_time_locale_t`에서 오므로, 다른 언어로 렌더링하는 것은 전역을 설정하는 것이 아니라 다른
locale을 넘기는 일이다.

잘못된 예 — 벽시계로 시간을 재고 부호를 믿기:

```text
proven_time_t t0 = proven_time_now();
do_work();
proven_time_t t1 = proven_time_now();
proven_u64 ns = (proven_u64)(t1 - t0);   /* wrong: an NTP step back makes this enormous */
```

뺄셈은 괜찮다. 캐스트가 문제다. 차이를 부호 있는 값으로 유지하고, 음수 경과 시간은 지속 시간이
아니라 "시계가 움직였다"로 취급하라.

잘못된 예 — sleep이 정확하다고 가정하기:

```text
proven_time_sleep(15);
/* wrong to assume exactly 15ms have passed: sleep guarantees AT LEAST that long,
   and the scheduler decides when you actually run again. */
```

### 완성 예제: 경과 시간, 날짜, 그리고 포맷된 타임스탬프

테스트 스위트가 컴파일하고 실행한다. 이 예제가 무엇을 단언하지 *않는지*에 주목하라 — sleep의
상한이다. 그것을 단언하면 바쁜 기계에서 실패하는 테스트가 되기 때문이다.

<!-- example: manual/examples/ex_05_time.c -->
```c
/*
 * Time comes in two flavours that look identical and are not, and picking the
 * wrong one is the classic timing bug.
 *
 *   - A WALL CLOCK answers "what time is it?". It is what a user wants to see,
 *     and it is allowed to jump: NTP corrects it, daylight saving shifts it, an
 *     administrator sets it. Measuring a duration with it can produce a negative
 *     elapsed time, and did, famously, on leap-second days.
 *
 *   - A MONOTONIC clock answers "how long since?". It only moves forward, at a
 *     steady rate, and has no relationship to any calendar. It is what you time
 *     an operation with.
 *
 * libc blurs this. time() is wall clock in whole seconds - useless for
 * measurement. clock() measures CPU time, not elapsed time, so a program that
 * sleeps looks instantaneous. Neither name tells you which of the two questions
 * it is answering.
 *
 * proven_time_now() is nanoseconds since the Unix epoch: one number that both
 * formats as a date and subtracts as a duration, at a resolution fine enough to
 * time real work.
 */

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    /* --- as a duration ------------------------------------------------- */
    proven_time_t start = proven_time_now();
    proven_time_sleep(15);                 /* milliseconds */
    proven_time_t end = proven_time_now();

    proven_i64 elapsed_ns = end - start;
    EXAMPLE_REQUIRE(elapsed_ns > 0, "time must move forward across a sleep");
    /* Sleep guarantees AT LEAST the requested time, never at most: the scheduler
     * decides when you actually run again. Asserting an upper bound here would
     * be a test that fails on a busy machine, which is why this one does not. */
    EXAMPLE_REQUIRE(elapsed_ns >= 10 * 1000 * 1000,
                    "sleeping 15ms must take at least ~10ms of wall time");

    /* --- as a date ------------------------------------------------------ */
    proven_datetime_t dt = proven_time_breakdown(start);
    EXAMPLE_REQUIRE(dt.year >= 2020 && dt.year < 3000, "the epoch breakdown gives a plausible year");
    EXAMPLE_REQUIRE(dt.month >= 1 && dt.month <= 12, "month is 1-12, not 0-11 as in libc's tm");
    EXAMPLE_REQUIRE(dt.day >= 1 && dt.day <= 31, "day is 1-31");
    EXAMPLE_REQUIRE(dt.hour <= 23 && dt.min <= 59 && dt.sec <= 60, "sec allows 60 for leap seconds");
    EXAMPLE_REQUIRE(dt.weekday <= 6, "weekday is 0-6 with 0 = Sunday");

    /* proven_time_now_datetime() is the two calls above in one, for when you
     * only want the calendar form. */
    proven_datetime_t now = proven_time_now_datetime();
    EXAMPLE_REQUIRE(now.year == dt.year, "both routes read the same clock");

    /* --- formatting a timestamp ---------------------------------------- */
    proven_result_u8str_t s = proven_u8str_create(alloc, 64);
    EXAMPLE_REQUIRE(proven_is_ok(s.err), "a 64-byte string is enough for a timestamp");

    /* The locale supplies month and weekday names; proven_time_locale_en is the
     * built-in English one. Pass your own to render other languages. */
    proven_err_t err = proven_time_u8_fmt(alloc, &s.value, dt, &proven_time_locale_en,
                                          "{year}-{month:0>2}-{day:0>2} {hour:0>2}:{min:0>2}:{sec:0>2}");
    EXAMPLE_REQUIRE(proven_is_ok(err), "formatting a datetime should succeed");

    proven_u8str_view_t out = proven_u8str_as_view(&s.value);
    EXAMPLE_REQUIRE(out.size == 19, "year-month-day hour:min:sec is exactly 19 characters");
    EXAMPLE_REQUIRE(out.ptr[4] == '-' && out.ptr[7] == '-' && out.ptr[13] == ':',
                    "the separators land where the pattern put them");

    proven_println("formatted: {}", PROVEN_ARG(out));

    proven_u8str_destroy(alloc, &s.value);
    return EXAMPLE_OK();
}
```

### 타입

```text
typedef proven_i64 proven_time_t;
```

UNIX epoch 이후 나노초.

```text
typedef struct {
    proven_i32 year;
    proven_u8 month;
    proven_u8 day;
    proven_u8 hour;
    proven_u8 min;
    proven_u8 sec;
    proven_u32 ms;
    proven_u8 weekday;
} proven_datetime_t;
```

필드 범위:

- `month`: 1 ~ 12.
- `day`: 1 ~ 31.
- `hour`: 0 ~ 23.
- `min`: 0 ~ 59.
- `sec`: 0 ~ 60.
- `ms`: 0 ~ 999.
- `weekday`: 0 ~ 6, 일요일이 0.

```text
typedef struct {
    const proven_u8str_view_t *month_names;
    const proven_u8str_view_t *month_short_names;
    const proven_u8str_view_t *weekday_names;
    const proven_u8str_view_t *weekday_short_names;
} proven_time_locale_t;
```

`proven_time_locale_en`은 기본 영어 로케일이다.

### 함수

| API | 의도 | 반환 |
|---|---|---|
| `proven_time_u8_fmt(alloc, str, dt, locale, fmt)` | 포맷된 날짜시간을 U8 문자열에 덧붙임. | `proven_err_t`. |
| `proven_time_u16_fmt(alloc, str, dt, locale, fmt)` | U16이 비활성화되지 않은 한 포맷된 날짜시간을 U16 문자열에 덧붙임. | `proven_err_t`. |
| `proven_time_now()` | 현재 타임스탬프(나노초). | `proven_time_t`. |
| `proven_time_breakdown(time_ns)` | epoch 나노초를 분해된 UTC 시간으로 변환. | `proven_datetime_t`. |
| `proven_time_now_datetime()` | 현재 로컬 분해 시간. | `proven_datetime_t`. |
| `proven_time_sleep(ms)` | 밀리초 동안 sleep. | void. |

날짜시간 포맷 키:

- `{year}`, `{month}`, `{day}`, `{hour}`, `{min}`, `{sec}`, `{ms}`, `{wday_num}`
- 로케일을 사용하는 `{Month}`, `{mon}`
- 로케일을 사용하는 `{Weekday}`, `{wday}`

예:

```c
proven_result_u8str_t r = proven_u8str_create(alloc, 32);
if (proven_is_ok(r.err)) {
    proven_u8str_t s = r.value;

    proven_datetime_t now = proven_time_now_datetime();
    proven_err_t e = proven_time_u8_fmt(
        alloc,
        &s,
        now,
        &proven_time_locale_en,
        "{year}-{month:0>2}-{day:0>2} {hour:0>2}:{min:0>2}:{sec:0>2}"
    );
    if (proven_is_ok(e)) {
        proven_println("{}", PROVEN_ARG(proven_u8str_as_view(&s)));
    }

    proven_u8str_destroy(alloc, &s);
}
```

## 5. Examples and misuse cases

### 단일 쓰기는 부분적일 수 있다

잘못됨(Wrong):

```text
proven_result_size_t w = proven_fs_write(file, data);
/* wrong: one write may be partial, and w.value is never looked at */
```

올바름(Correct):

```c
proven_result_file_t f = proven_fs_open(alloc, PROVEN_LIT("out.txt"),
                                        PROVEN_FS_WRITE | PROVEN_FS_CREATE | PROVEN_FS_TRUNC);
if (proven_is_ok(f.err)) {
    proven_mem_view_t data = proven_mem_view_from_u8(PROVEN_LIT("payload"));
    proven_err_t e = proven_fs_write_all(f.value, data);   /* loops until done */
    proven_err_t ce = proven_fs_close(f.value);            /* and the close can fail too */
    if (proven_is_ok(e)) e = ce;
    (void)e;
}
```

### 디렉터리 목록에는 특별한 파괴가 필요하다

잘못됨(Wrong):

```text
proven_result_array_t r = proven_fs_list(alloc, PROVEN_LIT("."));
PROVEN_ARRAY_DESTROY(&r.value); /* wrong: entry names leak */
```

올바름(Correct):

```c
proven_result_array_t r = proven_fs_list(alloc, PROVEN_LIT("."));
if (proven_is_ok(r.err)) {
    proven_fs_list_destroy(alloc, &r.value);
}
```

### destroy 이후에 매핑을 사용하지 말라

잘못됨(Wrong):

```text
proven_mmap_destroy(&map);
use_bytes(map.ptr, map.size); /* wrong: mapping has been released */
```

### 스트림에는 버퍼드 sysio 스캔을 사용하라

stdin이나 파이프에서 반복적으로 읽을 때는 다음을 선호하라:

```c
proven_sysio_scanner_t scanner = {0};
proven_err_t e = proven_sysio_scanner_init(&scanner, proven_sysio_stdin(), alloc, 4096);
if (proven_is_ok(e)) {
    int value = 0;
    e = proven_sysio_scanner_scan(&scanner, "{}", PROVEN_SCAN_ARG(&value));
    proven_sysio_scanner_deinit(&scanner);
}
```

### 환경 값은 owned 문자열이다

잘못됨(Wrong):

```text
proven_result_u8str_t env = proven_env_get(alloc, PROVEN_LIT("PATH"));
use_path(proven_u8str_as_view(&env.value)); /* wrong if env.err was not checked, and it leaks */
```

올바름(Correct):

```c
proven_result_u8str_t env = proven_env_get(alloc, PROVEN_LIT("HOME"));
if (proven_is_ok(env.err)) {
    proven_u8str_view_t home = proven_u8str_as_view(&env.value);
    proven_println("HOME={}", PROVEN_ARG(home));
    proven_u8str_destroy(alloc, &env.value);
}
```

### 완성 예제: 파일 전체를 읽고 쓰기

테스트 스위트에 의해 컴파일되고 실행됨. 이것은 대부분의 호출자가 실제로 원하는 파일 전체 API이며, 대상의 권한을 보존하는 atomic 재작성을 포함한다.

<!-- example: manual/examples/ex_05_fs_wholefile.c -->
```c
/*
 * The whole-file API: one call in, one call out. It exists because the
 * open/read-loop/close dance is where most file-handling bugs live - a forgotten
 * close, a partial read treated as EOF, a truncated file left behind by a failed
 * write. If you are reading or writing a file in its entirety, this is the API.
 */

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    /* A relative path in the current directory: the example must not depend on a
     * writable /tmp, and it removes what it creates before returning. */
    proven_u8str_view_t path = PROVEN_LIT("proven_example_wholefile.tmp");
    proven_u8str_view_t text = PROVEN_LIT("first line\nsecond line\n");

    /* --- write it in one call ---------------------------------------------- */
    /* Not atomic: a concurrent reader can see this file half-written. Fine here,
     * because nobody else is looking at it yet. */
    proven_err_t err = proven_fs_write_file(alloc, path, proven_mem_view_from_u8(text));
    EXAMPLE_REQUIRE(proven_is_ok(err), "writing the whole file should succeed");
    if (!proven_is_ok(err)) return 1;

    /* --- read it back as raw bytes ----------------------------------------- */
    /* proven_fs_read_all reads to EOF rather than to a pre-measured size, so it
     * also works on a pipe or a /proc entry, whose size cannot be known up front. */
    proven_result_mem_mut_t raw = proven_fs_read_all(alloc, path);
    EXAMPLE_REQUIRE(proven_is_ok(raw.err), "reading the whole file should succeed");
    if (proven_is_ok(raw.err)) {
        EXAMPLE_REQUIRE(raw.value.size == text.size, "read_all should return every byte written");
        /* The block is plain allocator memory - hand it back to the allocator that
         * produced it. There is no proven_fs_read_all_destroy. */
        alloc.free_fn(alloc.ctx, raw.value.ptr);
    }

    /* --- read it back as a string ------------------------------------------ */
    /* This is the one most callers want: the result is NUL-terminated, so it can
     * be handed to a view, to as_cstr, or to the scanner with no second copy. The
     * terminator slot is reserved up front, so it costs no extra allocation. */
    proven_result_u8str_t s = proven_fs_read_all_u8str(alloc, path);
    EXAMPLE_REQUIRE(proven_is_ok(s.err), "reading the whole file as a string should succeed");
    if (!proven_is_ok(s.err)) {
        (void)proven_fs_remove(alloc, path);
        return 1;
    }
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_as_view(&s.value), text),
                    "the file's contents should come back unchanged");
    printf("read back %zu bytes: %s", (size_t)proven_u8str_as_view(&s.value).size,
           proven_u8str_as_cstr(&s.value));
    proven_u8str_destroy(alloc, &s.value);

    /* --- stat, and the perms round-trip ------------------------------------ */
    proven_fs_stat_t st = {0};
    err = proven_fs_stat(alloc, path, &st);
    EXAMPLE_REQUIRE(proven_is_ok(err), "stat on a file we just wrote should succeed");
    EXAMPLE_REQUIRE(st.type == PROVEN_FS_TYPE_FILE, "a regular file should stat as a FILE");
    EXAMPLE_REQUIRE(st.size == text.size, "stat should report the size we wrote");

    /* `perms` carries the nine permission bits and nothing else, so it can be fed
     * straight back to chmod. That is the whole point of the field: read a file's
     * mode, and later restore it. (It used to carry the raw POSIX st_mode, whose
     * file-type bits chmod rejects - so this obvious round-trip failed.) */
    err = proven_fs_chmod(alloc, path, st.perms);
    EXAMPLE_REQUIRE(proven_is_ok(err), "a stat's perms must be accepted back by chmod");

    /* Now make the file owner-only, so the next check has something to prove. */
    proven_fs_perms_t private_perms = PROVEN_FS_PERM_OWNER_R | PROVEN_FS_PERM_OWNER_W;
    err = proven_fs_chmod(alloc, path, private_perms);
    EXAMPLE_REQUIRE(proven_is_ok(err), "restricting the file to its owner should succeed");

    /* --- rewrite it atomically --------------------------------------------- */
    /* A sibling temp file plus a rename: a concurrent reader sees either the whole
     * old file or the whole new one, never a half-written mix. Atomic for readers,
     * not durable across power loss. When it must be, proven_fs_write_file_durable asks. */
    proven_u8str_view_t text2 = PROVEN_LIT("replacement\n");
    err = proven_fs_write_file_atomic(alloc, path, proven_mem_view_from_u8(text2));
    EXAMPLE_REQUIRE(proven_is_ok(err), "the atomic rewrite should succeed");

    proven_fs_stat_t st2 = {0};
    err = proven_fs_stat(alloc, path, &st2);
    EXAMPLE_REQUIRE(proven_is_ok(err), "stat after the atomic rewrite should succeed");
    EXAMPLE_REQUIRE(st2.size == text2.size, "the file should now hold the replacement text");
    /* The rename writes a *new* inode over the old name, so the permissions would
     * be lost unless they were copied across. They are: rewriting a 0600 file does
     * not republish it as 0644. */
    EXAMPLE_REQUIRE(st2.perms == private_perms,
                    "the atomic rewrite must preserve the target's permissions");

    /* --- clean up ----------------------------------------------------------- */
    err = proven_fs_remove(alloc, path);
    EXAMPLE_REQUIRE(proven_is_ok(err), "removing the temp file should succeed");

    return EXAMPLE_OK();
}
```

### 완성 예제: open, read, write, close

테스트 스위트에 의해 컴파일되고 실행됨. read 루프에 주목하라: 파일 끝에서의 읽기는 0바이트 성공이 아니라 `PROVEN_ERR_EOF`를 반환하므로, 0바이트만 검사하는 루프는 작성자가 기대한 방식으로 결코 종료되지 않는다.

<!-- example: manual/examples/ex_05_fs_stream.c -->
```c
/*
 * The open/read/write/close path, for when the whole-file API (ex_05_fs_wholefile)
 * is not enough: you are streaming, or you want to own the buffer.
 *
 * The one thing to get right here: a single read or write moves *up to* the
 * requested number of bytes, not exactly that many. Treating one short read as
 * end-of-file is the classic way to silently lose the tail of a file.
 */

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    proven_u8str_view_t path = PROVEN_LIT("proven_example_stream.tmp");
    proven_u8str_view_t text = PROVEN_LIT("streamed bytes, read back in chunks\n");

    /* --- write ------------------------------------------------------------- */
    /* CREATE makes the file if it is absent; TRUNC empties it if it is not. The
     * allocator is only used to convert the path for the platform call. */
    proven_result_file_t out = proven_fs_open(alloc, path,
                                              PROVEN_FS_WRITE | PROVEN_FS_CREATE | PROVEN_FS_TRUNC);
    EXAMPLE_REQUIRE(proven_is_ok(out.err), "opening the file for writing should succeed");
    if (!proven_is_ok(out.err)) return 1;

    /* write_all loops for us. proven_fs_write does one attempt and may write less,
     * which is almost never what a caller means. */
    proven_err_t err = proven_fs_write_all(out.value, proven_mem_view_from_u8(text));

    /* The close is part of the write, and on a network or quota-enforced filesystem it is
     * the ONLY place the failure appears: the bytes were buffered, write() said yes, and
     * close() is where the disk finally says no. Close on the failure path too - the
     * handle is ours either way - but do not throw the answer away. */
    proven_err_t cerr = proven_fs_close(out.value);
    if (proven_is_ok(err)) err = cerr;

    EXAMPLE_REQUIRE(proven_is_ok(err), "writing the whole buffer should succeed");
    if (!proven_is_ok(err)) {
        (void)proven_fs_remove(alloc, path);
        return 1;
    }

    /* --- read -------------------------------------------------------------- */
    proven_result_file_t in = proven_fs_open(alloc, path, PROVEN_FS_READ);
    EXAMPLE_REQUIRE(proven_is_ok(in.err), "opening the file for reading should succeed");
    if (!proven_is_ok(in.err)) {
        (void)proven_fs_remove(alloc, path);
        return 1;
    }

    /* size is a hint for sizing the buffer, not a promise about how many bytes any
     * one read will hand over - and it is 0 for anything that is not a regular
     * file (a pipe, a device, a /proc entry). The loop below does not rely on it. */
    proven_result_size_t sz = proven_fs_size(in.value);
    EXAMPLE_REQUIRE(proven_is_ok(sz.err), "querying the size of an open file should succeed");
    EXAMPLE_REQUIRE(sz.value == text.size, "the file should be as long as what we wrote");

    proven_byte_t buf[128];
    proven_size_t total = 0;

    /* The partial-read loop. Each pass asks for whatever is left of the buffer and
     * advances by however much actually arrived: a short read is normal, not the
     * end of the file. The end of the file is a distinct status - PROVEN_ERR_EOF
     * with zero bytes - so the loop terminates on that, and on nothing else. The
     * loop also stops if the source outgrows the buffer; noticing that is the
     * caller's business (here it cannot happen, but a growing file could). */
    for (;;) {
        if (total == sizeof buf) break;   /* buffer full: caller decides what to do */

        proven_mem_mut_t dest = { .ptr = buf + total, .size = sizeof buf - total };
        proven_result_size_t r = proven_fs_read(in.value, dest);
        if (r.err == PROVEN_ERR_EOF) break;
        if (!proven_is_ok(r.err)) {
            (void)proven_fs_close(in.value);
            (void)proven_fs_remove(alloc, path);
            EXAMPLE_REQUIRE(false, "reading from the open file should not fail");
            return 1;
        }
        total += r.value;
    }

    (void)proven_fs_close(in.value);

    EXAMPLE_REQUIRE(total == text.size, "the loop should have read every byte in the file");
    proven_u8str_view_t got = { .ptr = buf, .size = total };
    EXAMPLE_REQUIRE(proven_u8str_view_eq(got, text), "the bytes should come back unchanged");

    printf("read %zu bytes in chunks: %.*s", (size_t)total, (int)total, (const char *)buf);

    /* --- clean up ----------------------------------------------------------- */
    err = proven_fs_remove(alloc, path);
    EXAMPLE_REQUIRE(proven_is_ok(err), "removing the temp file should succeed");

    return EXAMPLE_OK();
}
```

## 디렉터리를 한 번에 한 엔트리씩 읽기

`proven_fs_list`는 당신이 그 중 어느 것을 보기 전에 디렉터리 **전체**를 읽으며, 모든 이름마다
문자열을 할당한다. 50,000개 엔트리로 측정: **189 ms, +4.2 MB 상주(resident), 50,008회
할당**, 마지막 엔트리가 읽힐 때까지 아무것도 보이지 않는다. 이는 설정 디렉터리에는 괜찮고
메일 스풀에는 무용하다.

`proven_fs_dir_open` / `_next` / `_close`는 같은 디렉터리를 한 번에 한 엔트리씩 순회하며
**엔트리당 아무것도 할당하지 않는다**.

| API | 의도 | 반환 |
|---|---|---|
| `proven_fs_dir_open(scratch, path)` | 스트리밍 반복자를 연다. `scratch`는 경로 변환에만 쓰인다. | `proven_result_dir_t`. |
| `proven_fs_dir_next(&dir, &entry)` | 다음 엔트리. 더 이상 없으면 `PROVEN_ERR_EOF`. | `proven_err_t`. |
| `proven_fs_dir_close(&dir)` | 반복자를 해제. | void. |

```text
typedef struct {
    proven_u8str_view_t name;   /* BORROWED: points into the iterator's own storage,
                                   valid only until the next proven_fs_dir_next */
    proven_fs_type_t    type;   /* FILE / DIR / OTHER - and it follows symlinks */
} proven_fs_dir_entry_t;
```

이렇게 사용하라:

```c
proven_result_dir_t d = proven_fs_dir_open(alloc, PROVEN_LIT("."));
if (proven_is_ok(d.err)) {
    proven_fs_dir_t dir = d.value;
    proven_fs_dir_entry_t entry;
    for (;;) {
        proven_err_t e = proven_fs_dir_next(&dir, &entry);
        if (e == PROVEN_ERR_EOF) break;
        if (!proven_is_ok(e)) break;               /* a real error: report it */
        proven_println("{}", PROVEN_ARG(entry.name));
    }
    proven_fs_dir_close(&dir);
}
```

**이름은 borrowed이며, 다음 호출에서 소멸한다.** 이것이 전체를 무할당으로 만드는 것—그리고 당신이
그것을 붙들어 두는 순간 dangling 포인터로 만드는 것이다.

잘못됨(Wrong):

```text
proven_u8str_view_t names[100];
int n = 0;
while (proven_is_ok(proven_fs_dir_next(&dir, &entry)))
    names[n++] = entry.name;    /* wrong: every entry aliases the same storage, and the
                                   next _next overwrites it. All 100 end up equal - to
                                   whatever the last entry happened to be. */
```

올바름(Correct): 보관해야 하는 것들에 대해 바이트를 복사하라(`proven_u8str_create_from_view`)—또는
`proven_fs_list`를 사용하라. 그것은 모든 엔트리에 대해 정확히 그렇게 하고 그 대가를 청구한다.

**`PROVEN_ERR_EOF`가 끝이며, 그 밖의 것은 실패다.** "OK 아님"에서 멈추는 루프는 권한 에러를
완전한 목록으로 취급한다.

```text
while (proven_is_ok(proven_fs_dir_next(&dir, &entry))) { ... }
/* wrong: an I/O error ends the loop exactly like the end of the directory does,
   and you cannot tell whether you saw everything. */
```

## Walking a tree

`proven_fs_dir_*`는 디렉터리 하나를 순회한다. `proven_fs_walk`는 트리를 순회한다—그리고
그것이 하기를 *거부하는* 것을 정확히 말할 가치가 있다. 그 거부들이 곧 기능이기 때문이다:

| | |
|---|---|
| **루프**에 빠질 수 없다 | 결코 symlink를 *통과해* 내려가지 않는다. |
| **탈출**할 수 없다 | 같은 규칙: 트리 밖으로 나가는 링크는 보고되되 따라가지 않는다. |
| **거짓말**할 수 없다 | 읽을 수 없는 디렉터리는 그 디렉터리를 명명하는 *에러*로 돌아오고, 순회는 계속된다. 읽을 수 없는 하위 트리를 조용히 건너뛰는 트리 순회기는 백업이 파일을 놓치고도 성공을 보고하는 방식이다. |
| **비대해질** 수 없다 | 현재 경로의 *레벨*당 하나의 열린 핸들, 그리고 재사용되는 하나의 경로 버퍼. 메모리는 파일 수가 아니라 깊이의 함수다. |

symlink된 디렉터리는 여전히 **보고된다**—그것은 존재하고, `type`은 `DIR`이며, `is_symlink`는
true다—단지 진입되지 않을 뿐이다. 그것을 숨기는 것은 그 자체로 일종의 거짓말이 될 것이다. 그것을
*따라가고* 싶다면, 당신에게는 경로가 있다: 그것에 대해 두 번째 순회를 열면, 사이클 문제는 당신의
소유가 된다.

### 당신이 받는 구조체

```text
typedef struct {
    proven_u8str_view_t path;   /* the whole path, from the root you passed in.
                                   BORROWED: it points into the walk's one reused
                                   buffer and is valid only until the next call. */
    proven_u8str_view_t name;   /* the last component of `path`. Same lifetime. */
    proven_fs_type_t    type;   /* FILE / DIR / OTHER - and it FOLLOWS symlinks,
                                   exactly as proven_fs_stat does. */
    proven_size_t       size;   /* bytes, for a regular file; 0 otherwise. */
    proven_size_t       depth;  /* 0 for an entry directly inside the root. */
    bool                is_symlink;  /* reached through a symlink. `type` describes
                                        the TARGET; the walk does not enter it. */
} proven_fs_walk_entry_t;
```

`entry.path`와 `entry.name`은 순회의 재사용되는 하나의 버퍼에서 빌린 것이며 **다음 호출 전까지만**
유효하다. 이 단계보다 오래 살아남게 하려면 복사하라. 그것이 백만 엔트리의 순회가 백만 번이 아니라
한 번의 할당으로 드는 것의 대가다.

잘못됨(Wrong)—디렉터리 반복자가 가진 것과 같은 함정, 같은 이유로:

```text
proven_u8str_view_t found[100];
int n = 0;
while (proven_is_ok(proven_fs_walk_next(&walk, &entry)))
    found[n++] = entry.path;   /* wrong: every entry aliases ONE buffer. When the loop
                                  ends, all 100 point at whatever the last path was. */
```

두 개의 제한, 둘 다 조용해지는 대신 그렇다고 말한다:

- `max_depth`—얼마나 깊이 내려갈지. 제한 *에 있는* 디렉터리는 여전히 보고된다(그것은 하나의
  엔트리다). 진입되지 않을 뿐이다.
- `PROVEN_FS_WALK_DEPTH_LIMIT`(256)—순회 자체의 스택이 결코 넘지 않는 깊이. 그것을 지나친
  디렉터리는 그 디렉터리를 명명하며 `PROVEN_ERR_OUT_OF_BOUNDS`로 돌아온다.

테스트 스위트에 의해 컴파일되고 실행됨:

<!-- example: manual/examples/ex_05_fs_walk.c -->
```c
/*
 * Walking a tree.
 *
 * The three things a recursive walker gets wrong, and what this one does instead:
 *
 *   It loops.       A symlink pointing at an ancestor is a cycle. This walk never descends
 *                   THROUGH a symlink - the symlinked directory is still reported, it is
 *                   simply not entered - so a cycle is impossible, and so is walking out of
 *                   the tree you asked about and into the rest of the filesystem.
 *
 *   It lies.        A directory it cannot read gets skipped, and the walk reports success.
 *                   That is how a backup misses a subtree. Here the error comes back from
 *                   proven_fs_walk_next, with the entry naming the directory, and the walk
 *                   carries on from the next sibling. You decide what to do about it.
 *
 *   It bloats.      Reading a whole directory into memory before yielding anything makes a
 *                   walk of a big tree cost a big allocation. This one holds one open handle
 *                   per LEVEL of the current path and one reused path buffer - so its memory
 *                   is a function of depth, not of how many files there are.
 */

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    /* A small tree to walk: a file, a directory, a file inside it. */
    EXAMPLE_REQUIRE(proven_is_ok(proven_fs_mkdir(alloc, PROVEN_LIT("ex_walk"))), "mkdir");
    EXAMPLE_REQUIRE(proven_is_ok(proven_fs_mkdir(alloc, PROVEN_LIT("ex_walk/inner"))), "mkdir inner");
    EXAMPLE_REQUIRE(proven_is_ok(proven_fs_write_file(alloc, PROVEN_LIT("ex_walk/top.txt"),
        proven_mem_view_from_u8(PROVEN_LIT("top")))), "write top.txt");
    EXAMPLE_REQUIRE(proven_is_ok(proven_fs_write_file(alloc, PROVEN_LIT("ex_walk/inner/deep.txt"),
        proven_mem_view_from_u8(PROVEN_LIT("deep")))), "write deep.txt");

    proven_result_walk_t walk = proven_fs_walk_open(alloc, PROVEN_LIT("ex_walk"),
                                                    PROVEN_FS_WALK_UNLIMITED);
    EXAMPLE_REQUIRE(proven_is_ok(walk.err), "the walk should open");

    proven_size_t files = 0;
    proven_size_t dirs = 0;
    proven_size_t unreadable = 0;
    proven_size_t total_bytes = 0;

    for (;;) {
        proven_fs_walk_entry_t entry = {0};
        proven_err_t err = proven_fs_walk_next(&walk.value, &entry);

        if (err == PROVEN_ERR_EOF) break;

        if (!proven_is_ok(err)) {
            /* A directory that could not be read, or one deeper than the walk's stack. It is
             * REPORTED, not skipped - `entry.path` says which one - and the walk goes on. A
             * tool that copies a tree should fail here; one that reports on a tree should
             * count it and say so. What it must not do is pretend it did not happen. */
            unreadable++;
            continue;
        }

        /* `entry.path` and `entry.name` are borrowed: they point into the walk's one reused
         * buffer and are valid until the next call. Copy them if you need them longer. */
        if (entry.type == PROVEN_FS_TYPE_DIR) {
            dirs++;
        } else if (entry.type == PROVEN_FS_TYPE_FILE) {
            files++;
            total_bytes += entry.size;
        }
    }

    proven_fs_walk_close(&walk.value);

    EXAMPLE_REQUIRE(files == 2, "two files: top.txt and inner/deep.txt");
    EXAMPLE_REQUIRE(dirs == 1, "one directory: inner");
    EXAMPLE_REQUIRE(unreadable == 0, "and nothing in this tree is unreadable");
    EXAMPLE_REQUIRE(total_bytes == 7, "three bytes plus four");

    /* Depth-limited: max_depth 0 reports what is directly inside the root and descends
     * nowhere. The directory at the limit is still an entry, so it is still reported. */
    walk = proven_fs_walk_open(alloc, PROVEN_LIT("ex_walk"), 0);
    EXAMPLE_REQUIRE(proven_is_ok(walk.err), "the shallow walk should open");

    proven_size_t shallow = 0;
    for (;;) {
        proven_fs_walk_entry_t entry = {0};
        proven_err_t err = proven_fs_walk_next(&walk.value, &entry);
        if (err == PROVEN_ERR_EOF) break;
        if (proven_is_ok(err)) shallow++;
    }
    proven_fs_walk_close(&walk.value);

    EXAMPLE_REQUIRE(shallow == 2, "top.txt and inner - but nothing inside inner");

    (void)proven_fs_remove(alloc, PROVEN_LIT("ex_walk/inner/deep.txt"));
    (void)proven_fs_remove(alloc, PROVEN_LIT("ex_walk/top.txt"));
    (void)proven_fs_remove(alloc, PROVEN_LIT("ex_walk/inner"));
    (void)proven_fs_remove(alloc, PROVEN_LIT("ex_walk"));

    return EXAMPLE_OK();
}
```

## Streams: writers and readers

포매터의 유일한 sink는 예전에 `proven_u8str_t`였다. 파일은 `proven_file_t`였다. 인메모리
스캐너는 view를 읽었고, 파일 스캐너는 또 다른 무언가를 읽었다. **네 개의 타입, 네 개의 함수
계열, 공통 인터페이스 없음**—그래서 메모리와 파일 양쪽에서 동작하는 하나의
`serialize(sink, value)`를 쓸 수 없었고, 파일로 포매팅하는 것 자체가 불가능했으며, 파일을
한 줄씩 읽을 방법이 전혀 없었다.

**writer**는 바이트 sink다. **reader**는 바이트 source다. 둘 다 값으로 전달되는 작은 vtable로,
정확히 `proven_allocator_t`처럼, 같은 이유로 그렇다: 호출자가 바이트가 어디로 가는지 결정하며,
아무것도 숨겨지지 않는다.

| API | 의도 |
|---|---|
| `proven_writer_from_file(&file)` | 열린 파일 위의 언버퍼드 sink. |
| `proven_writer_from_u8str(&state, &str, alloc)` | owned 문자열에 덧붙인다. |
| `proven_writer_from_buffer(&state)` | 고정된 호출자 메모리. 절대로 아무것도 할당하지 않는다. |
| `proven_writer_buffered(&state, inner, buf)` | 작은 쓰기들을 누적한다. 버퍼는 **당신이** 공급한다. |
| `proven_fprint(w, fmt, ...)` / `proven_fprintln` | writer로 직접 포매팅. 무할당. |
| `proven_reader_from_file(&file)` / `_from_view(&state, view)` | 바이트 source. |
| `proven_reader_buffered(&state, inner, buf)` | 버퍼드 source; 라인 읽기에 필수. |
| `proven_reader_read_line(&state)` | 개행 없는 한 줄. |
| `proven_writer_is_valid(w)` / `proven_reader_is_valid(r)` | 생성자가 성공했는가? 0으로 채워진 핸들은 invalid이고, 모든 생성자는 잘못된 인자에 대해 그런 것을 반환한다—따라서 NULL 검사가 아니라 이것이 그 검사다. |
| `proven_fwrite_fmt(w, scratch, fmt, ...)` | **당신이** 크기를 정한 scratch 버퍼를 쓰는 `proven_fprint`. `proven_fprint`는 512바이트 스택 버퍼를 사용하고 더 긴 줄에 대해 `OUT_OF_BOUNDS`를 반환한다. 이것이 그런 줄을 포매팅하는 방법이다. |
| `proven_fmt_to_writer_impl(w, scratch, fmt, args, n)` | 위의 두 매크로가 확장되는 함수. 자신만의 가변 인자 래퍼를 만드는 경우에만 직접 호출하라. 매크로는 당신이 인자를 세지 않아도 되도록 존재한다. |

명확히 언급할 가치가 있는 네 가지 규칙. 각각은 이것이 잘못 설계될 수 있었던 방식이기 때문이다:

- **버퍼링은 당신이 공급하는 메모리를 사용한다.** `proven_writer_buffered`는
  `proven_arena_create`가 그러하듯 `proven_mem_mut_t`를 받는다. 숨겨진 전역 버퍼는 없으며,
  이는 곧 당신을 위해 그것을 flush해 줄 소멸자도 없다는 뜻이다—**버퍼가 스코프를 벗어나기 전에
  당신이 flush해야 한다.** 그 대가로, 당신의 로깅 경로는 결코 할당하지 않으며, out-of-memory
  상태에서 빠져나오며 로그를 남기는 프로그램이 여전히 로그를 남길 수 있다.
- **가득 찬 sink는 거부한다. 절단하지 않는다.** 가득 찬 고정 버퍼는 `PROVEN_ERR_OUT_OF_BOUNDS`를
  반환하고 `overflowed`를 기록한다. 당신의 데이터 끝을 조용히 버리는 sink는 받을 수 없다고
  말하는 sink보다 나쁘다.
- **reader의 버퍼에 비해 너무 긴 줄은 에러다**, 절단된 줄이 아니다. 온전한 것처럼 돌려받은 절단된
  줄은 호출자가 탐지할 방법이 없는 손상이다. 버퍼는 당신 것이다; 예상하는 입력에 맞춰 크기를
  정하라.
- **부분 쓰기는 얼버무릴 실패가 아니라 하나의 사실이다.** `write_fn`은 `proven_result_size_t`를
  반환한다: sink가 얼마나 받았는지, *그리고* 무엇이 잘못되었는지. 파이프, 소켓, 또는 차오르는
  디스크는 정말로 당신의 6000바이트 중 4096바이트를 받아들이고 나서 실패한다. 그리고 "전부
  소비하거나 실패한다"고 말하는 트레이트는 그런 sink를 올바르게 작성하는 것을 그저 불가능하게
  만든다. `proven_writer_write`는 여전히 전부-아니면-전무를 뜻한다(루프를 돈다);
  `proven_writer_write_partial`은 당신이 얼마나 진행했는지 봐야 할 때 있다. 버퍼드 writer는
  sink가 받지 **않은** 꼬리만 보관한다—첫 버전은 버퍼 전체를 보관하고 다시 보냈으며, 그래서
  실패하는 sink가 받아들인 접두부를 두 번 받았다.

- **한 번 실패한 writer는 실패한 채로 남는다.** writer가 바이트를 잃은 뒤에는—sink가 죽은 버퍼드
  writer, 오버플로한 고정 버퍼—그것이 만들어내던 스트림에 수신자가 볼 수 없는 구멍이 생기므로,
  이후의 모든 쓰기와 flush는 에러를 반환한다. *들어갈* 수 있는 더 짧은 청크조차 거부된다: 그것을
  쓰면 구멍 뒤에 놓여, 결과가 완전한 출력처럼 보일 것이기 때문이다. `clear()`는 없다: 복구
  스토리가 있다면 그것은 이 writer가 괜찮은 척하는 것이 아니라 새 sink 위의 새 writer를 수반한다.
  (이전에는 실패한 쓰기 이후 `flush`가 `PROVEN_OK`를 답했다—버퍼가 비어 있어서 실패할 것이 남지
  않았기 때문이다—그리고 "쓰고, 쓰고, 쓰고, flush를 검사한다"는, 거의 모두가 버퍼드 writer를
  쓰는 방식이 가득 찬 디스크에서 성공을 보고했다.)

reader의 규칙은 그 거울상이다: **실패한 읽기는 에러이지, 결코 파일의 끝이 아니다.**
`proven_reader_read`는 source가 깨질 때 깨끗한 0바이트 EOF가 아니라 `PROVEN_ERR_IO`를 반환한다—
디스크 에러로 잘려나간 파일과 그냥 끝난 파일은, 둘을 구별할 수 없는 호출자에게는 같은 것이며,
그 중 하나만 안전하게 처리할 수 있기 때문이다.

10,000줄을 stdout으로 내보내며 측정한 비용:

| | `write()` 시스템 콜 | `malloc()` |
|---|---|---|
| `proven_println` | 10,000 | **0** (이전엔 10,000) |
| 버퍼드 writer, 호출자 메모리 8 KiB | **24** | **0** |

`malloc()` 열은 512바이트 스택 버퍼에 들어가는 줄, 즉 일반적인 경우에 대한 값이다. 그보다 긴 줄은
거부되는 대신 그 한 번의 호출에 한해 heap으로 물러난다.

`proven_println`은 의도적으로 여전히 줄당 하나의 시스템 콜이다: 그것을 버퍼링하려면 숨겨진 전역
상태가 필요할 것이다. 24를 원하는 호출자는 버퍼드 writer를 만들고 그렇게 하겠다고 말한다.

### 완성 예제: 하나의 직렬화기, 세 개의 목적지, 그리고 되읽기

테스트 스위트에 의해 컴파일되고 실행됨. `render_row`가 자신의 바이트가 어디로 가는지 모른다는
점에 주목하라—그것이 바로 요점 전부다.

<!-- example: manual/examples/ex_05_stream.c -->
```c
/*
 * Writers and readers: one interface for "where bytes go" and one for "where bytes
 * come from".
 *
 * The point is that the code below - render_row - does not know and does not care
 * whether it is writing to a string, to a fixed buffer, or to a file. That was
 * impossible before: the formatter's only sink was a proven_u8str_t.
 */

/* One serializer. It takes a sink, not a destination. */
static proven_err_t render_row(proven_writer_t w, int id, const char *name) {
    proven_fmt_result_t r = proven_fprintln(w, "{:>4} | {}", PROVEN_ARG(id), PROVEN_ARG(name));
    return r.err;
}

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    /* --- the same code, into a growing string ----------------------------- */
    proven_result_u8str_t s = proven_u8str_create(alloc, 16);
    EXAMPLE_REQUIRE(proven_is_ok(s.err), "string create");

    proven_writer_u8str_t s_state;
    proven_writer_t to_string = proven_writer_from_u8str(&s_state, &s.value, alloc);
    EXAMPLE_REQUIRE(proven_is_ok(render_row(to_string, 7, "ada")), "render into a string");

    printf("into a string:\n%s", proven_u8str_as_cstr(&s.value));
    proven_u8str_destroy(alloc, &s.value);

    /* --- the same code, into memory you own: zero allocations ------------- */
    proven_byte_t fixed[64];
    proven_writer_buf_t b_state = { .buf = { .ptr = fixed, .size = sizeof fixed } };
    proven_writer_t to_buffer = proven_writer_from_buffer(&b_state);
    EXAMPLE_REQUIRE(proven_is_ok(render_row(to_buffer, 8, "grace")), "render into a buffer");
    EXAMPLE_REQUIRE(b_state.len > 0, "the buffer received the row");

    /* A full buffer REFUSES; it does not truncate. A sink that silently drops the
     * end of your data is worse than one that says it cannot take it. */
    proven_byte_t tiny[4];
    proven_writer_buf_t t_state = { .buf = { .ptr = tiny, .size = sizeof tiny } };
    proven_writer_t to_tiny = proven_writer_from_buffer(&t_state);
    EXAMPLE_REQUIRE(proven_writer_write_str(to_tiny, PROVEN_LIT("far too long")) == PROVEN_ERR_OUT_OF_BOUNDS,
                    "a full buffer refuses rather than truncating");
    EXAMPLE_REQUIRE(t_state.overflowed, "and it records that it did");

    /* --- the same code, into a file, buffered ------------------------------ */
    /*
     * The buffer is memory YOU supply, exactly like an arena's. This library has no
     * hidden global state, so it cannot flush for you at exit - which is why you must
     * flush before the buffer goes out of scope. In exchange, your logging path never
     * allocates: ten thousand lines here cost 0 mallocs and a couple of dozen write
     * syscalls, where ten thousand proven_println calls cost 10,000 syscalls.
     */
    proven_u8str_view_t path = PROVEN_LIT("example_stream_rows.txt");
    proven_result_file_t f = proven_fs_open(alloc, path,
        (proven_fs_mode_t)(PROVEN_FS_WRITE | PROVEN_FS_CREATE | PROVEN_FS_TRUNC));
    EXAMPLE_REQUIRE(proven_is_ok(f.err), "open the output file");
    proven_file_t file = f.value;

    proven_byte_t out_buf[4096];
    proven_writer_buffered_t w_state;
    proven_writer_t to_file = proven_writer_buffered(&w_state,
        proven_writer_from_file(&file),
        (proven_mem_mut_t){ .ptr = out_buf, .size = sizeof out_buf });

    for (int i = 0; i < 3; ++i) {
        EXAMPLE_REQUIRE(proven_is_ok(render_row(to_file, i, "row")), "render into the file");
    }
    EXAMPLE_REQUIRE(proven_is_ok(proven_writer_flush(to_file)),
                    "flush: nothing is written until you say so");
    /* And the close, which is the last thing that can tell you the write did not land. */
    EXAMPLE_REQUIRE(proven_is_ok(proven_fs_close(file)), "closing the written file");

    /* --- reading it back, a line at a time -------------------------------- */
    /* Reading a file line by line was simply not possible before: the only route was
     * loading the entire file into memory and splitting it by hand. */
    proven_result_file_t rf = proven_fs_open(alloc, path, PROVEN_FS_READ);
    EXAMPLE_REQUIRE(proven_is_ok(rf.err), "reopen for reading");
    proven_file_t rfile = rf.value;

    proven_byte_t in_buf[128];
    proven_reader_buffered_t r_state;
    (void)proven_reader_buffered(&r_state, proven_reader_from_file(&rfile),
                                 (proven_mem_mut_t){ .ptr = in_buf, .size = sizeof in_buf });

    int lines = 0;
    for (;;) {
        proven_result_u8str_view_t line = proven_reader_read_line(&r_state);
        if (line.err == PROVEN_ERR_EOF) break;
        EXAMPLE_REQUIRE(proven_is_ok(line.err), "read a line");
        /* The view points INTO the reader's buffer, and is valid only until the next
         * call. Copy it if it has to outlive that. */
        printf("line %d: %.*s\n", lines, (int)line.val.size, (const char *)line.val.ptr);
        ++lines;
    }
    EXAMPLE_REQUIRE(lines == 3, "three rows in, three lines out");

    (void)proven_fs_close(rfile);
    (void)proven_fs_remove(alloc, path);

    return EXAMPLE_OK();
}
```

### 당신이 보유하는 구조체

두 **핸들**은 값으로 전달되며 저렴하다:

```text
typedef struct {
    void *ctx;
    proven_result_size_t (*write_fn)(void *ctx, proven_mem_view_t chunk);
    proven_err_t (*flush_fn)(void *ctx);   /* may be NULL: this sink holds nothing back */
} proven_writer_t;

typedef struct {
    void *ctx;
    proven_result_size_t (*read_fn)(void *ctx, proven_mem_mut_t dest);
} proven_reader_t;
```

`write_fn`은 **그 뒤에 실패하더라도 얼마나 나갔는지 보고한다**. 그리고 그것은 사소한 배려가
아니다: 파이프나 가득 찬 디스크로의 쓰기는 정말로 일부 바이트를 내보내고 나서 실패한다. 더 깔끔한
"전부 아니면 전무"라는 거짓말 위에 세워진 버퍼드 writer는 실패 시 버퍼 전체를 보관하고 다음
flush에 다시 보냈다—6000바이트 페이로드가 처음 4096바이트가 **중복된** 채로 10,096바이트로
도착했다. 데이터를 잃는 것은 나쁘다; 조용히 두 배로 만드는 것은 더 나쁘다. 수신자가 알 수 없기
때문이다.

그 밖의 모든 것은 [호출자 소유 상태](manual-ko.md#42-caller-owned-state--destroy-없음-복사-금지)다—
`proven_writer_buf_t`, `proven_writer_u8str_t`, `proven_writer_buffered_t`,
`proven_reader_view_t`, `proven_reader_buffered_t`. 이들은 아무것도 할당하지 않고, destroy가
없으며, 핸들이 그 안을 가리키고 있는 동안 **복사되거나 이동되어서는 안 된다**.

### 주의사항, 그리고 무엇이 잘못되는가

**결코 flush되지 않는 버퍼드 writer는 결코 일어나지 않은 출력이다.** 숨겨진 상태가 없으므로,
당신을 위해 그것을 flush해 줄 소멸자도 없다—그리고 여기 있는 어떤 것도 `atexit` 핸들러를 등록하지
않는다. 당신의 프로세스를 소유하는 라이브러리는 당신이 추론할 수 없는 라이브러리이기 때문이다.

잘못됨(Wrong):

```text
proven_writer_buffered_t st;
proven_writer_t w = proven_writer_buffered(&st, inner, buf);
(void)proven_fprintln(w, "the important line");
return;                       /* wrong: the buffer dies with the frame, and so does the line */
```

올바름(Correct)—버퍼나 내부 sink가 사라지기 전에 flush하라:

```c
proven_byte_t buf[256];
proven_sysio_out_t out;
proven_writer_t w = proven_sysio_stdout_buffered(&out,
    (proven_mem_mut_t){ .ptr = buf, .size = sizeof buf });
(void)proven_fprintln(w, "the important line");
(void)proven_writer_flush(w);          /* now it has happened */
```

**reader가 당신에게 건네는 줄은 그 버퍼 안을 가리킨다.** 그것은 다음 호출 전까지만 유효하다. 그것이
백만 줄을 읽는 데 백만 번의 할당이 아니라 하나의 버퍼가 들게 하는 원리다—그리고 당신이 그것을
붙들어 두는 순간 dangling 포인터다.

잘못됨(Wrong):

```text
proven_u8str_view_t lines[100];
for (int i = 0; i < 100; ++i) {
    proven_result_u8str_view_t ln = proven_reader_read_line(&st);
    lines[i] = ln.val;        /* wrong: every entry aliases the SAME buffer, and the
                                 next read_line overwrites what the last one returned */
}
```

올바름(Correct): 다시 호출하기 전에, 보관해야 하는 바이트를 복사하라(`proven_u8str_t`로, 아레나로,
어디로든).

**flush는 durability 장벽이 아니다.** `proven_writer_flush`는 버퍼드 writer의 바이트를 그 뒤의
것으로 밀어 넣는다; *파일의* 바이트를 디스크에 올리는 것은 `proven_fs_sync`다. 이 둘은 서로 다른
연산이며, 한 단어가 정직하게 둘 모두를 뜻할 수는 없었다—그것이 바로 둘 다라고 주장하면서 어느
쪽도 아니었던 옛 `proven_sysio_flush`가 사라진 이유다.

**버퍼보다 긴 줄은 절단이 아니라 거부된다.** `PROVEN_ERR_OUT_OF_BOUNDS`—그리고 reader는 그 줄에서
멈춘 채로 남는다: 재동기화(resync)는 없다. 담을 수 없는 줄은 그 바이트를 어떻게 할지 결정하지
않고서는 건너뛸 수 있는 줄이 아니기 때문이다. 예상하는 입력에 맞춰 버퍼의 크기를 정하라. (버퍼를
*정확히 채우는* 줄은 괜찮다: 개행이 함께 들어가지 않아도 되고, 개행이 전혀 없는 마지막 줄도
마찬가지다.)

## The standard streams

`stream.h`에는 writer, reader, 버퍼드 writer, 그리고 라인 reader가 있다. `sysio.h`에는 stdin,
stdout, stderr가 있다. 이 둘이 서로 소개되기 전까지는, 두 가지가 그저 불가능했다—그리고 한
호출은 거짓말이었다.

**stdin을 한 줄씩 읽는 방법이 없었다.** 프로그램이 stdin으로 하는 가장 흔한 일인데, 선택지는
토큰 스캐너이거나, 결코 끝나지 않을 수도 있는 스트림 전체를 읽는 것이었다. 그 다리(bridge)가
그것을 고친다: 표준 핸들이 호출자 소유 저장소에 주차되므로, 라인 reader에게 가리킬 안정적인
무언가가 생긴다.

```c
proven_byte_t buf[4096];
proven_sysio_lines_t lines;
if (proven_is_ok(proven_sysio_stdin_lines(&lines, (proven_mem_mut_t){ .ptr = buf, .size = sizeof buf }))) {
    for (;;) {
        proven_result_u8str_view_t line = proven_sysio_read_line(&lines);
        if (line.err == PROVEN_ERR_EOF) break;
        if (!proven_is_ok(line.err)) break;   /* OUT_OF_BOUNDS: a line longer than `buf` */
        /* `line.val` points INTO `buf` and is valid only until the next call. */
        proven_println("{}", PROVEN_ARG(line.val));
    }
}
```

이것은 라인 reader의 속성을 물려받는데, 이는 두 번째 것을 쓰지 않는 것의 요점이다: view는 무할당,
`"\r\n"`이 처리되고, 후행 개행이 없는 마지막 줄도 여전히 반환되며, 당신의 버퍼보다 긴 줄은
`PROVEN_ERR_OUT_OF_BOUNDS`다—결코 조용히 절단된 줄이 아니다.

**포매터를 표준 스트림으로 겨눌 수 없었다.** `proven_fprintln`은 writer를 받는데; stdout은 그것이
아니었다. 이제는 그렇고, *버퍼드* writer일 수도 있다—그래서 천 개의 작은 줄이 천 번이 아니라 한
번의 시스템 콜로 든다.

| | |
|---|---|
| `proven_sysio_stdout_writer(&st)` | stdout 위의 언버퍼드 writer. 모든 쓰기가 하나의 write 시스템 콜이다. |
| `proven_sysio_stderr_writer(&st)` | stderr에 대해 같은 것—에러에 대해 당신이 원하는 것이다: 다음 코드 줄이 실행되기 전에 그것이 나간다. |
| `proven_sysio_stdin_reader(&st)` | stdin 위의 reader. |
| `proven_sysio_stdout_buffered(&out, buf)` | 당신이 소유하는 버퍼 위의 버퍼드 writer 뒤에 놓인 stdout. |
| `proven_sysio_file_buffered(&out, file, buf)` | 임의의 열린 파일에 대해 같은 것. |

**그리고 이제 `flush`는 무언가를 뜻한다.** `proven_sysio_flush`는 예전에 존재하지 않는 버퍼를
flush한다고 주장했다: POSIX에서는 no-op, Windows에서는 *디스크 sync*. 그것은 **삭제되었다**.
버퍼드 writer의 바이트를 OS로 밀어 넣는 것은 `proven_writer_flush`이고, OS의 바이트를 디스크로 밀어
넣는 것은 `proven_fs_sync`다. 이 둘은 서로 다른 연산이며 이제는 그렇다고 말한다.

> **당신은 버퍼드 writer를 flush해야 한다.** 버퍼가 차거나 당신이 flush하기 전까지는 아무것도
> 터미널에 도달하지 않으며, 이 라이브러리의 어떤 것도 당신 몰래 그것을 하기 위해 `atexit` 핸들러를
> 등록하지 않는다—당신의 프로세스를 소유하는 라이브러리는 당신이 추론할 수 없는 라이브러리이기
> 때문이다. 결코 flush되지 않는 버퍼드 출력은 결코 일어나지 않은 출력이다. 직접 호출들
> (`proven_print`, `proven_println`, `proven_eprint`)은 바로 이 이유로 언버퍼드로 남는다: 그것들이
> 쓰는 것은 반환되기 전에 이미 나가는 중이다.

### 당신이 보유하는 구조체

```text
typedef struct { proven_file_t file; } proven_sysio_std_t;
    /* Storage for a standard handle, so a writer or reader has something stable to
       point at. proven_writer_from_file takes a proven_file_t * and the file must
       outlive the writer - so it cannot be a temporary. This is that storage. */

typedef struct {
    proven_sysio_std_t       std;
    proven_writer_buffered_t buffered;
} proven_sysio_out_t;        /* a buffered writer over a standard stream or a file */

typedef struct {
    proven_sysio_std_t       std;
    proven_reader_buffered_t buffered;
} proven_sysio_lines_t;      /* a line reader over a standard stream or a file */
```

셋 모두 [호출자 소유 상태](manual-ko.md#42-caller-owned-state--destroy-없음-복사-금지)다.

### 주의사항, 그리고 무엇이 잘못되는가

**이 상태 구조체들은 자기 자신을 가리키는 포인터를 담는다.** 당신이 돌려받는 writer는 당신이 넘긴
구조체 *안의* `&st->std.file`을 주소로 삼는다. 구조체를 복사하거나 값으로 반환하면, writer는 여전히
원본을 가리킨다—그것은 죽은 프레임일 수 있다.

잘못됨(Wrong)—그리고 한 감사에서 정확히 이것이 heap-use-after-free로 재현되었다:

```text
proven_sysio_out_t out;
proven_writer_t w = proven_sysio_stdout_buffered(&out, buf);

proven_sysio_out_t copy = out;      /* wrong: `w` still points into `out` */
/* ... `out` goes out of scope ... */
(void)proven_writer_write_str(w, PROVEN_LIT("boom"));   /* writes through dead storage */
```

유일한 예외는 `proven_sysio_lines_t`다: `proven_sysio_read_line`은 매 호출마다 그것을 다시
바인딩하므로, 라인 reader는 이동**될 수 있다**. 그것은 의도된 배려인데, 그것이 상태를 포인터로
받기 때문이다—"재배치 가능"을 뜻하는 형태—그리고 라이브러리는 약속의 형태를 한 함정을 놓아서는 안
되기 때문이다.

**0으로 초기화된 `proven_file_t`는 invalid 핸들이 아니다—POSIX에서 그것은 fd 0, 즉 stdin이다.**
라이브러리는 당신이 채워 넣기를 잊은 핸들과 정당하게 fd 0을 가리키는 핸들을 구별할 수 없다.

```text
proven_sysio_out_t out;
proven_file_t f = {0};                                  /* wrong: this is stdin */
proven_writer_t w = proven_sysio_file_buffered(&out, f, buf);   /* writes to fd 0 */
```

**버퍼드 stdout과 언버퍼드 stderr는 당신이 쓴 순서대로 뒤섞이지 않는다.** 당신이 버퍼링하는 것은
당신의 버퍼에 앉아 있는 동안 stderr는 곧장 나간다. 당신의 출력 뒤에 나타나야 하는 에러를
출력하기 전에 flush하라.

## Randomness, by use case

단일한 "random"은 없다. 동일해 보이지만 그렇지 않은 두 가지 작업이 있다:

| 당신의 작업 | 사용 | 이유 |
|---|---|---|
| 키, 토큰, nonce—공격자가 추측해서는 안 되는 모든 것. | `proven_random_bytes`, 또는 그것으로 시드된 `proven_chacha_rng_t`. | 오직 암호학적 source만이 추측 불가능하다. |
| 같지만, OS가 없는 타깃에서. | 보드 자체의 엔트로피로 시드된 `proven_chacha_rng_t`. | ChaCha20은 순수 산술이다; OS가 필요 없다. 그것은 오직 그 시드만큼만 추측 불가능하다. |
| 시뮬레이션, 테스트, 게임, 표본추출. | `proven_xoshiro256ss_t`. | 빠르고, **재현 가능하다**: 같은 시드가 같은 실행을 재생하며, 그것이 실패하는 테스트를 디버깅 가능하게 만든다. |
| 범위 내의 수, 셔플, [0,1)의 float. | `proven_rng_below`, `proven_rng_range`, `proven_rng_f64`, `proven_rng_shuffle`—어떤 source 위에서든. | `% n`은 편향되어 있고, 모두가 그럼에도 그것을 쓴다. 이것들은 그렇지 않다. |

두 요구사항은 정면으로 대립한다. 재현 가능이란 예측 가능을 뜻하고, 예측 가능은 정확히 토큰이
그래서는 안 되는 것이다: `proven_xoshiro256ss_t`의 몇 개 출력이 그 전체 상태를, 따라서 그것이
앞으로 낳을 모든 수를 드러낸다. 그것은 재생해야 하는 시뮬레이션에는 기능이고 세션 토큰에는
재앙이다—그래서 둘은 혼동될 수 없는 이름을 지니며, 선택은 무언가가 어떻게 시드되었는가에 묻히는
대신 호출 지점에서 가시적이다.

**트레이트는 실패하지 않는다(infallible).** `proven_rng_t`는 무작위 바이트의 source이며, valid한
것에서 뽑는 것은 실패할 수 없다. 그것은 단순화가 아니라, 실패가 어디로 갔는지다. 운영체제에
엔트로피를 요청하는 것은 *실패할 수 있으므로*, 그 실패는 정확히 한 곳—시딩—에 국한되며, 당신은
그것을 시작 시점에 한 번 검사한다. 하류의 모든 draw는 전역적(total)이다.

| | |
|---|---|
| `proven_random_bytes(buf, len)` | OS CSPRNG. 실패 시 `false`를 반환한다; 그러면 `buf`를 사용하지 말라. `len == 0`은 성공적인 no-op이다. |
| `proven_random_u64()` | OS로부터의 강한 한 워드, 실패 시 `0`. |
| `proven_chacha_rng_seed_from_entropy(&g)` | 엔트로피 source로부터 암호학적 생성기를 시드. **이것이 실패할 수 있는 호출이다.** |
| `proven_random_set_source(fn, ctx)` | 엔트로피 source를 설치. 호스티드 타깃에는 OS가 이미 설치되어 있다; 베어메탈 타깃은 보드의 TRNG를 설치한다. |
| `proven_chacha_rng_seed(&g, seed32)` | 당신이 공급하는 32바이트로부터 시드—보드의 하드웨어 엔트로피 source. 결코 시계로부터는 아니다. |
| `proven_xoshiro256ss_seed(&g, seed)` | 재현 가능 생성기를 시드. 시드 0도 괜찮다: SplitMix64를 통해 확장된다. |

### 엔트로피는 어디서 오는가

위의 모든 것은 순수 산술이다—생성기들, 헬퍼들, 그리고 `proven_random_bytes` 자체까지. 플랫폼에
따라 다른 것은 그 뒤의 **엔트로피 source**인데, 그것이 프로그램이 스스로 계산할 수 없는 유일한
것이기 때문이다.

- **호스티드:** OS CSPRNG가 당신을 위해 설치되어 있다—Linux의 `getrandom`, BSD와 macOS의
  `getentropy`, Windows의 `BCryptGenRandom`, 그리고 이들 중 어느 것도 없는 곳의 `/dev/urandom`.
  당신은 아무것도 호출하지 않는다.
- **베어메탈:** 당신이 하나를 설치하기 전까지는 source가 없다. 보드는 실제 엔트로피를 *가지고*
  있다—온칩 TRNG, 링 오실레이터, ADC의 노이즈 플로어—그리고 라이브러리는 그것이 어디 있는지 알 수
  없다.

```text
/* On a board: hand the library its hardware entropy, once, at startup.
 * (A listing, not a fragment: it defines a function, and `hardware_rng_read` is
 * whatever your SoC calls its entropy register.) */
static bool board_trng(void *ctx, void *buf, proven_size_t len) {
    (void)ctx;
    /* read the SoC's entropy register into buf; return false if it is not ready */
    return hardware_rng_read(buf, len);
}

proven_random_set_source(board_trng, NULL);

/* From here everything above works unchanged - including the one call that turns a few
 * hundred bytes of hardware entropy into an endless cryptographic stream. */
proven_chacha_rng_t g;
if (!proven_chacha_rng_seed_from_entropy(&g)) {
    /* the TRNG was not ready. The generator is INERT - it yields zeros and an invalid
     * trait - so ignoring this does not get you plausible-looking bytes. */
}
```

설치된 source가 없으면 `proven_random_bytes`는 **false**를 반환한다. 그것은 시계로 시드된 PRNG로
폴백하지 않는다. 그것은 성공처럼 보이지만 아무것도 보고하지 않는 보안 구멍이기 때문이다—거부는
호출자가 처리할 수 있는 하나의 사실이다.

의도적으로 내장된 **`RDRAND` / `RNDR` 백엔드는 없다.** 호스티드 타깃에서는 OS가 이미 CPU의 명령을
자신의 풀에 섞어 넣으므로, 그것을 직접 호출하는 것은 아무것도 얻지 못하면서 그 섞임을 잃게 하고;
*유일한* source로 쓰이는 원시 하드웨어 명령은 사람들이 10년간 논쟁해 온 바로 그 방식이다. 원한다면,
그것은 이 훅 뒤로 네 줄이다—그러면 그 선택은 가시적으로 당신 것이다.

### 당신이 보유하는 구조체

셋 모두 [호출자 소유 상태](manual-ko.md#42-caller-owned-state--destroy-없음-복사-금지)다: 아무것도
할당하지 않고, 파괴할 것이 없으며, **하나를 복사하면 그 시퀀스를 복제한다**.

```text
typedef struct { const proven_rng_vtable_t *vt; void *ctx; } proven_rng_t;
    /* The trait: two pointers, held by value. `ctx` points at one of the generators
       below, which must outlive it. Drawing from a VALID one cannot fail - that is
       the whole design: the failure lives in seeding, not in drawing. */

typedef struct { proven_u64 s[4]; } proven_xoshiro256ss_t;
    /* 256 bits of state. Reproducible, and NOT secret-grade. */

typedef struct {
    proven_u32    state[16];   /* the ChaCha state: constants, key, counter, nonce */
    proven_byte_t block[64];   /* the keystream block currently being handed out */
    proven_size_t used;        /* how much of it is spent */
    proven_u32    seeded;      /* set only by seeding. A zero-initialised struct is
                                  the shape of "never seeded", and must stay inert. */
} proven_chacha_rng_t;
```

### 레퍼런스

| API | 의도 | 반환 |
|---|---|---|
| `proven_random_bytes(buf, len)` | 엔트로피 source로부터 채움(기본은 OS). **실패할 수 있는 유일한 호출.** | `bool`. `false`이면 `buf`는 미지정이며 사용해서는 안 된다. `len == 0`은 성공한다. |
| `proven_random_u64()` | 같은 source로부터의 강한 한 워드. | `proven_u64`, 실패 시 `0`—그것도 valid한 draw이므로, 둘을 구별해야 할 때는 `proven_random_bytes`를 사용하라. |
| `proven_random_set_source(fn, ctx)` | 엔트로피 source를 설치. 호스티드 타깃에는 불필요; 이것이 보드가 자신의 TRNG를 넘겨주는 방법이다. | void. |
| `proven_xoshiro256ss_seed(&g, seed)` | 재현 가능 생성기를 시드. 어떤 시드든 괜찮다—0조차; SplitMix64를 통해 확장된다. | void. |
| `proven_xoshiro256ss_next(&g)` | 다음 워드. 핫 패스: 트레이트를 통해서가 아니라 직접 호출하라. | `proven_u64`. |
| `proven_xoshiro256ss_rng(&g)` | 헬퍼를 위해 `proven_rng_t`로 본다. | `proven_rng_t`. |
| `proven_chacha_rng_seed(&g, seed32)` | 당신이 공급하는 32바이트의 **실제 엔트로피**로부터 암호학적 생성기를 시드. | void. |
| `proven_chacha_rng_seed_from_entropy(&g)` | 설치된 source로부터 시드. **이것을 검사하라.** | `bool`. `false`이면 생성기는 INERT 상태로 남는다—0을 낳고 invalid한 트레이트를 낳는다. |
| `proven_chacha_rng_next/_fill` | draw. 일단 시드되면 실패할 수 없다. | `proven_u64` / void. |
| `proven_chacha_rng(&g)` | `proven_rng_t`로 본다. | `proven_rng_t`—생성기가 성공적으로 시드된 적이 없으면 **invalid**. |
| `proven_rng_u64(rng)` / `proven_rng_fill(rng, buf, len)` | 어느 생성기로부터든 트레이트를 통해 draw. | `proven_u64` / void. invalid source에는 `0` / no-op. |
| `proven_rng_below(rng, bound)` | `[0, bound)`에서 균일, **무편향**. | `proven_u64`; `bound == 0`일 때 `0`. |
| `proven_rng_range(rng, lo, hi)` | `[lo, hi]`에서 균일, 포함적. 전체 `INT64_MIN..INT64_MAX` 범위도 오버플로하지 않는다. | `proven_i64`; `hi < lo`이면 `lo`. |
| `proven_rng_f64(rng)` | `[0, 1)`에서 균일. 53비트; 결코 `1.0`을 반환하지 않는다. | `double`. |
| `proven_rng_shuffle(rng, base, count, elem_size)` | 무편향 Fisher-Yates 순열, 제자리(in place). | void. |

### 주의사항, 그리고 무엇이 잘못되는가

**`proven_xoshiro256ss_t`로 결코 비밀을 생성하지 말라.** 그것은 예측 가능하기 *때문에* 빠르다:
그 출력 몇 개가 256비트 상태 전체를, 그리고 그 상태로부터 그것이 앞으로 낳을 모든 수를 드러낸다.
두 생성기는 정확히 이 이유로 혼동될 수 없는 이름을 지닌다.

잘못됨(Wrong)—공격자가 몇 개를 지켜본 뒤 계산할 수 있는 세션 토큰:

```text
proven_xoshiro256ss_t g;
proven_xoshiro256ss_seed(&g, 12345);
proven_u64 session_token = proven_xoshiro256ss_next(&g);   /* wrong: predictable */
```

**암호학적 생성기를 결코 시계, 카운터, 또는 시리얼 번호로 시드하지 말라.** ChaCha20은 정확히 그
시드만큼만 추측 불가능하다. 시계에서 유도된 시드는 완벽하게 무작위처럼 보이지만 그렇지 않은
스트림을 낳는다—그것은 명백한 실패보다 나쁘다. 아무것도 그것을 보고하지 않기 때문이다.

```text
proven_byte_t seed[32] = { 0 };
memcpy(seed, &now_ns, sizeof now_ns);      /* wrong: ~20 bits of real entropy, and guessable */
proven_chacha_rng_seed(&g, seed);
```

**시딩을 검사하라.** 그것은 여기서 실패할 수 있는 유일한 것이며, 바로 그래서 그것을 무시하는 것이
솔깃하다. 무시하면 생성기는 inert 상태가 되어 당신에게 0을 건넨다—설계상 그럴듯한 값이 아니라
가시적으로 죽은 값이다.

잘못됨(Wrong):

```text
proven_chacha_rng_t g;
proven_chacha_rng_seed_from_entropy(&g);   /* wrong: the bool was the point */
proven_chacha_rng_fill(&g, key, 32);       /* key is now 32 zero bytes */
```

올바름(Correct):

```c
proven_chacha_rng_t g;
if (!proven_chacha_rng_seed_from_entropy(&g)) {
    /* No entropy. There is nothing safe to do here except refuse to continue. */
} else {
    proven_byte_t key[32];
    proven_chacha_rng_fill(&g, key, sizeof key);   /* cannot fail: it is seeded */
}
```

**`% n`은 편향되어 있고, 모두가 그럼에도 그것을 쓴다.** `n`이 2^64를 나누어떨어지게 하지 않는 한
낮은 값들이 더 자주 나온다—어쩌다 하는 점검에서는 보이지 않지만, 셔플이나 표본추출에서는 실재한다.

```text
proven_u64 die = proven_rng_u64(rng) % 6 + 1;   /* wrong: 1 and 2 are slightly likelier */
```

올바름(Correct): `proven_rng_below(rng, 6) + 1`.

**시드된 생성기를 복사하지 말라**—그 스트림을 복제하려는 의도가 아니라면. 하나에서 복사된 두 개의
"독립적인" 생성기는 동일한 출력을 낳는다—그것은 시뮬레이션 재생에는 기능이고 두 개의 토큰을
발급하는 데는 재앙이다.

테스트 스위트에 의해 컴파일되고 실행됨:

<!-- example: manual/examples/ex_05_random.c -->
```c
/*
 * Randomness, by use case. There is no single "random": there are two jobs that look
 * identical and are not, and picking the wrong one is the whole danger.
 *
 *   A key, a token, a nonce - anything an attacker must not guess - needs a CRYPTOGRAPHIC
 *   source. A simulation, a test, a game needs a REPRODUCIBLE one, because a failing run you
 *   cannot replay is a failing run you cannot debug. The two requirements are in direct
 *   opposition: reproducible means predictable, and predictable is exactly what a token must
 *   not be. So the library gives them different names, and the choice is visible here at the
 *   call site rather than buried in how something was seeded.
 */

int main(void) {
    /* ---- Job 1: a secret. The OS CSPRNG - and the one place randomness can fail. ---- */
    proven_byte_t key[32];
    EXAMPLE_REQUIRE(proven_random_bytes(key, sizeof key),
                    "the OS must give us strong bytes on a hosted platform");

    /* ---- Job 2: lots of cryptographic bytes, or any at all on a board with no OS.
     * ChaCha20 is pure arithmetic: seed it once from real entropy and it needs nothing from
     * the operating system afterwards - no syscall per draw, and it works on bare metal.
     * Seeding is the ONLY step that can fail, so it is the only one you have to check. ---- */
    proven_chacha_rng_t crypto;
    EXAMPLE_REQUIRE(proven_chacha_rng_seed_from_entropy(&crypto), "seed the CSPRNG from the OS, once");

    proven_byte_t token[16];
    proven_chacha_rng_fill(&crypto, token, sizeof token);   /* cannot fail: it is seeded */

    /* ---- Job 3: a REPRODUCIBLE run. xoshiro256** is fast and replays exactly from its seed,
     * which is what makes a failing simulation debuggable. It is NOT secret-grade: a few of
     * its outputs reveal its whole state. Never hand it a token to generate. ---- */
    proven_xoshiro256ss_t sim;
    proven_xoshiro256ss_seed(&sim, 12345);

    proven_xoshiro256ss_t replay;
    proven_xoshiro256ss_seed(&replay, 12345);
    EXAMPLE_REQUIRE(proven_xoshiro256ss_next(&sim) == proven_xoshiro256ss_next(&replay),
                    "the same seed replays the same run - that is the whole point");

    /* ---- The helpers work over ANY source, through the proven_rng_t trait. ---- */
    proven_rng_t rng = proven_xoshiro256ss_rng(&sim);

    /* A number in a range. `rng_u64() % 6` is what everyone writes, and it is BIASED unless
     * the bound divides 2^64 - the low values come up more often. This one is not. */
    for (int i = 0; i < 100; ++i) {
        proven_u64 die = proven_rng_below(rng, 6) + 1;
        EXAMPLE_REQUIRE(die >= 1 && die <= 6, "a die roll is 1..6, uniformly");
    }

    proven_i64 temperature = proven_rng_range(rng, -40, 85);
    EXAMPLE_REQUIRE(temperature >= -40 && temperature <= 85, "an inclusive range, both ends");

    double p = proven_rng_f64(rng);
    EXAMPLE_REQUIRE(p >= 0.0 && p < 1.0, "a double in [0, 1) - never 1.0");

    /* An unbiased shuffle: Fisher-Yates over the unbiased index above. The `% n` version of
     * this loop measurably favours some orderings. */
    int deck[10];
    for (int i = 0; i < 10; ++i) deck[i] = i;
    proven_rng_shuffle(rng, deck, 10, sizeof deck[0]);

    int sum = 0;
    for (int i = 0; i < 10; ++i) sum += deck[i];
    EXAMPLE_REQUIRE(sum == 45, "a shuffle is a permutation: every card is still there, once");

    /* The cryptographic generator satisfies the same trait, so the same helpers work over it
     * when the choice must be unguessable rather than merely uniform. */
    proven_rng_t secure = proven_chacha_rng(&crypto);
    proven_u64 unguessable_index = proven_rng_below(secure, 1000);
    EXAMPLE_REQUIRE(unguessable_index < 1000, "the helpers do not care which source they draw from");

    (void)token;
    (void)key;
    return EXAMPLE_OK();
}
```
