# Proven Library - Complete Developer Manual (v26.04.24)

## Table of Contents
1. [Introduction & Design Philosophy](#1-introduction--design-philosophy)
2. [Core Types & Error Handling](#2-core-types--error-handling)
3. [Memory Architectures](#3-memory-architectures)
4. [Basic Data Structures](#4-basic-data-structures)
5. [Advanced Data Structures](#5-advanced-data-structures)
6. [Strings and Formatting](#6-strings-and-formatting)
7. [Filesystem, IO & Memory Mapping](#7-filesystem-io--memory-mapping)
8. [Multi-threading & Job System](#8-multi-threading--job-system)
9. [Stackless Coroutines](#9-stackless-coroutines)
10. [Algorithms & System Time](#10-algorithms--system-time)

---

## 1. Introduction & Design Philosophy

The `proven` library is designed from the ground up prioritizing **Strict C23 Portability**, **No-CRT Reliance**, **Zero-Overhead Memory Contexts**, and absolute **Safety (Undefined Behavior elimination)**.

### Core Principles
- **Result Pattern**: Output pointers and implicit error assumptions are strictly forbidden. Functions that can fail return a `proven_result_xxx_t` struct enclosing an `err` code and a `value`.
- **No-CRT Isolation**: The core library (`proven/`) completely avoids `<stdlib.h>` and `<stdio.h>`. External calls flow exclusively through the Platform Abstraction Layer (`platform/`).
- **Polymorphic Allocation**: No system assumes `malloc`. Everything takes a `proven_allocator_t` trait, allowing the seamless swapping of Heap, Arena, and Pool allocators.

---

## 2. Core Types & Error Handling

### 2.1 Primitive and Memory Types (`proven/types.h`)
The library shadows primitive constraints into strongly typed equivalents to guarantee environment-agnostic behaviors.

*   `proven_u8`, `proven_u16`, `proven_u32`, `proven_u64`: Unsigned integer types.
*   `proven_i8`, `proven_i16`, `proven_i32`, `proven_i64`: Signed integer types.
*   `proven_size_t`: Unsigned size/index identifier (maps to standard size).
*   `proven_ptrdiff_t`: Pointer difference calculations offset.
*   `proven_byte_t`: Essential for Strict-Aliasing safety (maps to `unsigned char`).
*   `proven_mem_t` / `proven_mem_mut_t`: Structs representing continuous memory slices. Contains `ptr` (pointer) and `size` (capacity).

### 2.2 Error Handling (`proven/error.h`)
*   `proven_err_t`: The fundamental enum for all errors.
    *   `PROVEN_OK` (0): Execution succeeded.
    *   `PROVEN_ERR_NOMEM`: Allocation failed due to capacity boundaries.
    *   `PROVEN_ERR_OUT_OF_BOUNDS`: Buffer overflow prevented.
    *   `PROVEN_ERR_INVALID_ARG`: Function invoked with contradictory parameters.
    *   `PROVEN_ERR_IO`: Platform execution (file interactions) failed.
    *   `PROVEN_ERR_NOT_FOUND`: Resource or data query empty.

**Macros:**
*   `PROVEN_IS_OK(err)`: Standard validation macro evaluating boolean `true` if `err == PROVEN_OK`.
*   `PROVEN_CKD_ADD(res, a, b)`, `PROVEN_CKD_SUB(res, a, b)`, `PROVEN_CKD_MUL(res, a, b)`: Wrap the C23 `stdckdint.h` math constraints ensuring overflow triggers an abrupt panic/error output early.

**Example: The Result Pattern**
```c
// Intended usage format strictly avoiding magic control macros.
proven_result_mem_mut_t res = alloc.alloc_fn(alloc.ctx, 128, 8);
if (!PROVEN_IS_OK(res.err)) {
    return res.err; // Explicit early error propagation
}
// Safe to utilize res.value
```

---

## 3. Memory Architectures

Deeply integrated abstractions bypassing `malloc` and preventing fragmentation via specialized Contexts.

### 3.1 The Allocator Trait (`proven/allocator.h`)
```c
typedef proven_result_mem_mut_t (*proven_alloc_fn_t)(void *ctx, proven_size_t size, proven_size_t align);
typedef proven_result_mem_mut_t (*proven_realloc_fn_t)(void *ctx, void *old_ptr, proven_size_t old_size, proven_size_t new_size, proven_size_t align);
typedef void (*proven_free_fn_t)(void *ctx, void *ptr);

typedef struct {
    void *ctx;
    proven_alloc_fn_t alloc_fn;
    proven_realloc_fn_t realloc_fn;
    proven_free_fn_t free_fn;
} proven_allocator_t;
```
Every system requiring memory (Arrays, Strings, Maps, Buffers) requests a `proven_allocator_t`.

### 3.2 Heap Allocator (`proven/heap.h`)
*   **Usage**: Maps requests directly to the OS / C Standard backing dynamically via the PAL.
*   **Function**: `proven_heap_allocator()` returns the initialized trait.

### 3.3 Bump Arena (`proven/arena.h`)
High-performance, zero-overhead allocation without System Call latency. Fragmentation is impossible. Memory is stacked continuously and released entirely simultaneously.
*   **Struct**: `proven_arena_t`
*   **Functions**: `proven_arena_create(backing)`, `proven_arena_as_allocator(&arena)`.
*   **Example**:
```c
alignas(max_align_t) proven_byte_t stack_mem[4096];
proven_mem_mut_t backing = { .ptr = stack_mem, .size = sizeof(stack_mem) };
proven_arena_t arena = proven_arena_create(backing);
proven_allocator_t alloc = proven_arena_as_allocator(&arena);
// Allocate and never perform `free`. Scope naturally drops the stack!
```

### 3.4 Pool Allocator (`proven/pool.h`)
Enforces rigid fixed-size instantiation enforcing data homogeneity. Effectively recycles pointer blocks maintaining local `bin` arrays.
*   **Struct**: `proven_pool_t`
*   **Functions**: `proven_pool_init(&pool, base_alloc, item_size, align, bin_cap)`
*   **Intended Use**: Recycling identical Game Entities, Ast Nodes, or Packet frames infinitely without degrading the parent base allocator.

---

## 4. Basic Data Structures

### 4.1 Dynamic Array (`proven/array.h`)
A generalized sequential array wrapping `realloc` operations properly handling capacities.
*   **Struct**: `proven_array_t`
*   **Macros**:
    *   `PROVEN_ARRAY_INIT(alloc, Type, init_cap, array_ptr)`
    *   `PROVEN_ARRAY_PUSH(array_ptr, Type, value)` - Doubles capacity if bounding occurs.
    *   `PROVEN_ARRAY_POP(array_ptr, Type, out_ptr)`
    *   `PROVEN_ARRAY_GET(array_ptr, Type, index)`
    *   `PROVEN_ARRAY_DESTROY(array_ptr)`

### 4.2 Generic Buffer (`proven/buffer.h`)
Manages abstract byte manipulation seamlessly tracking valid sliced regions against total allocated memory.
*   **Struct**: `proven_buf_t` containing `proven_mem_mut_t data`, `cap`, `len`.
*   **Function**: `proven_buf_create`, `proven_buf_append`.

### 4.3 Ring Buffer (`proven/ring.h`)
FIFO architecture mapping limits safely preventing overallocations. Ideal for threaded queues logic.
*   **Macros**: `PROVEN_RING_INIT`, `PROVEN_RING_PUSH`, `PROVEN_RING_POP`, `PROVEN_RING_DESTROY`.

---

## 5. Advanced Data Structures

### 5.1 Intrusive List (`proven/list.h`)
Strictly zero-allocation linked list. Nodes embed metadata physically into payload objects avoiding dereference indirection cache-misses entirely.
*   **Structs**: `proven_list_t` (Head boundary), `proven_list_node_t` (Embedded node).
*   **Macros**:
    *   `PROVEN_CONTAINER_OF(ptr, type, member)`: Backtracks physical address offset retrieving parent structures.
    *   `PROVEN_LIST_FOR_EACH(iter_node, list)`: Loop macro.
    *   `PROVEN_LIST_ENTRY(node_ptr, Type, member)`: Recovers the enclosing entity perfectly.

### 5.2 Hash Map (`proven/map.h`)
Extensively supports integer (`_INT`) and String (`_U8`) indexing through distinct explicit macro architectures ensuring collision probing is abstracted.
*   **Macros**:
    *   `PROVEN_MAP_INIT_INT`, `PROVEN_MAP_INIT_U8`
    *   `PROVEN_MAP_SET_INT`, `PROVEN_MAP_SET_U8`
    *   `PROVEN_MAP_GET_INT`, `PROVEN_MAP_GET_U8`
    *   `PROVEN_MAP_REMOVE_INT`, `PROVEN_MAP_REMOVE_U8`

---

## 6. Strings and Formatting

### 6.1 U8 Strings (`proven/u8str.h`)
Null-termination `\0` is historically hazardous. The `proven` paradigm binds views tightly against memory constraints guaranteeing size boundaries instantly without `strlen` iterations.
*   **Views**: `proven_u8str_view_t` (Immutable tracking strings strictly using `ptr` and `size`).
*   **Macro**: `PROVEN_LIT("Literal")` converts a compile-time string to a secure View.
*   **Managed**: `proven_u8str_t` handles heap lifecycle via `proven_u8str_create`.

### 6.2 No-CRT Formatter (`proven/fmt.h`, `sysio.h`)
Modern, `f-strings`/`Zig` inspired rendering pipeline evaluating arguments securely.
*   **Syntax**: `{:0>5x}` -> Pad zeros, right align, minimum width 5, Base-16 output.
    *   `{}`: General positional argument rendering.
*   **Macro Engine**:
    *   `PROVEN_ARG(var)`: Extracts payload utilizing `_Generic` automatically selecting standard primitive types mapped directly toward `proven_arg_t`.
    *   `proven_println(fmt, ...)`: Console deployment mapping explicitly bypassing `printf`.
*   **Example**:
```c
proven_println("Warning at 0x{:0>8x}: {}", PROVEN_ARG(ptr), PROVEN_ARG(PROVEN_LIT("Core Missing")));
```

---

## 7. Filesystem, IO & Memory Mapping

Abstracted heavily around isolated structures allowing Windows and POSIX logic matching underneath the exact same ABI.

### 7.1 Filesystem Operations (`proven/fs.h`)
*   `proven_fs_open(path_view, mode)`: Opens an explicit `proven_file_t` descriptor. Modes: `PROVEN_FS_MODE_READ`, `PROVEN_FS_MODE_WRITE`.
*   `proven_fs_read`, `proven_fs_write`: Uses `proven_mem_mut_t` directly allowing strict payload controls.
*   `proven_fs_read_all(alloc, path_view)`: Captures the entire file dynamically allocating a block representing size exactly matching reality.

### 7.2 Directory and Stat
*   `proven_fs_mkdir`, `proven_fs_list` (Retrieves array comprising inner files).
*   `proven_fs_stat`: Provides `proven_fs_stat_t` querying physical disc presence and size logic.

### 7.3 Memory Mapping (`proven/mmap.h`)
High-bandwith architectural loading routing pages from Disc direct to process logic.
*   **Functions**: `proven_mmap_create(fs_handle, offset, size, prot, flags)`
*   `proven_mmap_destroy` explicitly releases.

---

## 8. Multi-threading & Job System

`proven_job_t` represents a Multi-Producer Multi-Consumer (MPMC) lock-free execution environment keeping caches continuously operative without atomic suspensions.

### 8.1 Job Operations (`proven/job.h`)
*   **Initialization**: `proven_job_system_init(allocator, num_threads, queue_cap)`.
*   **Submission**: `proven_job_submit(function, arg_ptr)` allows rapid asynchronous offloading instantly appending to ring-buffers avoiding mutex bottlenecks.
*   **Shutdown**: `proven_job_system_shutdown()` performs final atomic barriers resolving threads securely stopping execution logic.
*   **Constraint**: No standard mutex integrations are provided guaranteeing jobs remain explicitly Lock-Free dependencies.

---

## 9. Stackless Coroutines

A `Duff's Device` modeled state tracking entity requiring functionally 0 allocations and absolutely minimal variable states inside (`int`).

### 9.1 Usage Model (`proven/coro.h`)
```c
struct process_state {
    proven_coro_t coro;
    int progress;
};

void run_process(struct process_state *ctx) {
    PROVEN_CORO_BEGIN(&ctx->coro);
    for (ctx->progress = 0; ctx->progress < 100; ctx->progress++) {
        // Evaluate condition safely returning control to Main.
        PROVEN_CORO_YIELD(&ctx->coro);
    }
    PROVEN_CORO_END(&ctx->coro);
}
```

---

## 10. Algorithms & System Time

### 10.1 Generic Sorting (`proven/algorithm.h`)
*   `proven_array_sort(array_ptr, compare_fn)` executes highly optimized generic Quicksort avoiding function call overhead constraints by passing explicitly mapped logic directly scaling via trait matching.

### 10.2 Strict Timing (`proven/time.h`)
*   `proven_sys_time_now_ms()` returns exact Epoch offsets.
*   `proven_time_sleep(ms)` allows thread pauses mapping universally natively suppressing conditional waits smoothly. 

---
_Documentation continuously maintained aligning strictly toward the C23 standard enforcing safe architectural implementations ecosystem-wide._
