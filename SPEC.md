# `proven` C-Based Library Architecture and Design Specification (v26.04.24)

## 1. Project Overview
`proven` is a modern C-based library designed with strict adherence to **Provenance** rules and **Strict Aliasing** as defined in the latest C standards (C23+). It completely eliminates dependencies on the existing C Standard Library (no-CRT) and provides a memory-safety-oriented infrastructure that operates in a minimum footprint (freestanding).

---

## 2. Core Architecture Principles
1.  **Strict Bottom-Up Dependencies:** Foundation -> Allocator -> Memory Core -> Collections -> Modules (FS/FMT/CORO/JOB).
2.  **Naming Convention:** `snake_case` (identifiers), `_t` (types), `UPPER_SNAKE_CASE` (macros).
3.  **Explicit Error and Resource Management:** Elimination of `errno`, utilization of C23 `[[nodiscard]]` attributes.
4.  **Provenance & Alias Awareness:** Pointers are explicitly treated at the code level as not just simple addresses, but as having 'provenance' and 'valid scope'.
5.  **Polymorphic Allocator Injection (DI):** All systems depend only on the `proven_allocator_t` interface, not on specific allocator implementations.

---

## 3. Layered Detailed Specification

### 3.1. Foundation (Base Layer)
Definition of a dedicated type system that serves as the basis for all data.

*   **Integer Types (fixed-width, no `_t` suffix):**
    *   Unsigned: `proven_u8`, `proven_u16`, `proven_u32`, `proven_u64`.
    *   Signed: `proven_i8`, `proven_i16`, `proven_i32`, `proven_i64`.

*   **Byte Type:**
    *   **`proven_byte_t`:** typedef of `unsigned char`. The unique type in the C standard allowed to access all object representations, ensuring byte-level access.

*   **Memory Core Types:**
    *   **`proven_mem_view_t` (Read-only):** Non-owning read-only slice.
    *   **`proven_mem_mut_t` (Read-write):** Non-owning editable slice.

### 3.2. Error Handling & Resource Management
*   **`proven_err_t`:** Error code enumeration. Includes `PROVEN_OK`, `PROVEN_ERR_NOMEM`, `PROVEN_ERR_IO`, etc.
*   **Result Pattern:** Functions capable of failing return a `Result` structure by value.
*   **SESE (Single-Entry Single-Exit):** System resource release uses the `goto cleanup_xxx;` pattern.

### 3.3. Allocator Interface (`proven_allocator_t`)
*   VTable structure including `alloc_fn`, `realloc_fn`, and `free_fn`.
*   **`proven_arena_t`**: High-speed bump allocator. `realloc` acts as `alloc` when the pointer argument is `NULL`.
*   **`proven_heap_allocator`**: OS heap memory access via PAL (`platform/`).

---

## 4. Advanced Modules Specification

### 4.1. Modern Formatting & SysIO (`fmt.h`, `sysio.h`)
*   **Type-Safe Macros:** Automatic recognition of `int`, `string`, `datetime`, `pointer`, etc., via the `PROVEN_ARG` macro using C11 `_Generic`.
*   **Modern Syntax:** Supports Python/Rust style `{}` (auto-index) and `{0}` (explicit index).
*   **Zig-style Formatting:** Supports advanced formatting such as `{:0>5}` (zero-fill right align) and `{:*^10}` (star-fill center align).
*   **No-CRT I/O:** Uses a direct kernel output interface via PAL instead of `stdout`/`stderr`, removing `stdio.h` dependency.

### 4.2. Memory Mapping File (`mmap.h`)
*   Directly maps file system handles into the virtual memory address space.
*   **Zero-Copy Extraction:** Maximizes file data parsing performance by immediately interpreting `mmap` regions as `proven_u8str_view_t`.

### 4.3. Stackless Coroutines (`coro.h`)
*   **Zero-Allocation State Machines:** Implements asynchronous logic with lightweight state machines based on 4-byte integers using a modified Duff's Device technique.
*   **Yield/Await:** Execution can be suspended via `PROVEN_CORO_YIELD` and resumed later.

### 4.4. Multicore Job System (`job.h`)
*   **Lock-Free Scheduler:** Distributes tasks across multiple cores without mutexes using an `atomic`-based MPMC ring buffer.
*   **Hardware Parallelism:** Creates and manages worker thread pools matching the number of physical CPU cores via PAL.

---

## 5. Implementation Priorities and Roadmap
1.  **Phase 1-6:** Foundational Memory & Error Infrastructure. -> [Complete]
2.  **Phase 7-12:** Core Collections (Strings, Array, Map, Ring, Algorithm). -> [Complete]
3.  **Phase 13-15:** Filesystem PAL, Sorting, and Security Locking. -> [Complete]
4.  **Phase 16-18:** Modern Formatting, Mmap, and System I/O Environment. -> [Complete]
5.  **Phase 19-20:** Stackless Coroutines & Lock-Free Job Scheduler. -> [Complete]

---

## 6. Technical Specification: Concurrency Contract
*   **Zero Global State (Re-entrancy):** The `proven` library strictly avoids hidden static or global states. It is 100% re-entrant.
*   **Zero Internal Locking:** No mutexes are used inside individual data structures (ensuring maximum performance).
*   **External Synchronization:** If shared resources (Array, Map, etc.) are modified concurrently across multiple threads, the caller must manage external locks.
*   **Job System Isolation:** Only queue operations within the Job Pool are provided safely isolated with `atomic`.

---

## 7. Future Directions and Unimplemented Strategies

### 7.1. High-Efficiency Data Structure Concepts
*   **Cache-Aware Padding:** Rearranging struct fields to match CPU cache lines (64 bytes) to prevent False Sharing and improve access speed.
*   **Zero-Copy Serialization:** Implementing "In-Place" processing logic that parses and serializes data directly on `proven_mem_view_t` without additional memory allocation.
*   **SIMD Readiness:** Enforcing data layout on 16/32-byte boundaries to enable future SIMD acceleration.
