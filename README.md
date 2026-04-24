# Proven C Library

![C23](https://img.shields.io/badge/Standard-C23-blue.svg)
![No-CRT](https://img.shields.io/badge/Dependency-No--CRT-red.svg)
![License](https://img.shields.io/badge/License-MIT-green.svg)

**Proven** is a hyper-optimized, dependency-free (No-CRT) C23 foundation library built for system programmers, game engine developers, and extreme high-performance applications. It abandons the legacy C standard library in favor of Data-Oriented Design (DOD), polymorphic arena allocators, strict pointer safety bounds, and absolute performance determinism.

---

## Core Design Philosophy

### 1. The No-CRT Doctrine & Platform Abstraction
The core logic **NEVER** directly calls `<stdlib.h>`, `<stdio.h>`, or `<string.h>`. All external system calls are strictly routed through isolated Platform Abstraction Layers (PAL like `proven_sys_mem.c` and `proven_sys_thread.c`). This ensures your code is highly deterministic, portable across OS kernels, and perfectly suited for WASM or bare-metal embedded targets.

### 2. VTable Polymorphism & Arena Allocation
Forget runtime fragmentation caused by `malloc` and `free`. `proven` enforces region-based memory management. 
Data structures (`proven_array_t`, `proven_u8str_t`, etc.) are entirely decoupled from specific memory managers. They bind dynamically to a trait-based `proven_allocator_t` interface, allowing zero-cost memory shifts between Arenas, Heaps, or custom allocators.

### 3. Safety Without Rust (Result Patterns)
We eradicated raw un-bounded pointers, `NULL` checking, and ambiguous out-pointer parameters.
* **Return by Value:** Everything that can fail returns a `proven_result_..._t` wrapping both the error cause (`proven_err_t`) and the value securely.
* **Memory Views:** Slices (`proven_mem_view_t`) handle length and boundaries securely out-of-the-box, mitigating out-of-bounds access.

---

## Concurrency & Multi-threading

### Thread Safety Contract
For maximum single-thread efficiency, the library forces **Zero Internal Locking**. Core algorithms do not silently employ mutexes or spinlocks. If you share resources (like `proven_array_t`) across OS threads, you are externally responsible for managing thread barriers.

### Stackless Coroutines (Duff's Device)
Features a Zero-Overhead asynchronous engine relying on 4-byte strict C-macro state machines. It allows yielding and awaiting execution without expensive OS thread context-switching or heap allocation.

```c
struct Fetcher {
    proven_coro_t coro;
    int progress;
};

int fetch_data(struct Fetcher *f) {
    // Zero context-switching overhead
    PROVEN_CORO_BEGIN(&f->coro);

    proven_println("Loading...");
    PROVEN_CORO_YIELD(&f->coro); // Pauses and resolves to caller

    while(f->progress < 100) {
        PROVEN_CORO_YIELD(&f->coro);
    }
    
    PROVEN_CORO_END(&f->coro);
}
```

### Lock-Free MPMC Job System
A Multi-Producer Multi-Consumer (MPMC) atomic ring-buffer scheduling pool using C11 `<stdatomic.h>`. It processes thousands of workloads across physical cores rapidly without any Mutex dead-locking.

```c
// 1. Initialize Thread Pool (Lock-Free)
proven_job_system_init(heap_allocator, 4, 1024);

// 2. Submit thousands of jobs gracefully
for (int i = 0; i < 1000; i++) {
    while (!proven_job_submit(my_task_routine, arg)) {
        proven_sys_thread_yield(); // Ring buffer full, back off
    }
}

// 3. Graceful automated barrier tracking
proven_job_system_shutdown();
```

---

## Data Structures & Primitives

### Explicit Error Handling & Pointers
```c
// Proven Library Pattern (Safe, fast, verbose)
proven_result_mem_mut_t res = alloc.alloc_fn(ctx, 128, 8);
if (!proven_is_ok(res.err)) {
    return res.err; // Explicit early exit
}
proven_byte_t *safe_ptr = res.value.ptr;
```

### Compile-Time Strings
Compress strings into unboxed views at `O(1)` runtime cost, deleting `strlen` overhead permanently.
```c
proven_u8str_view_t text = PROVEN_LIT("Hello");
```

### High-Performance Containers
* **`proven_array_t` (Generic Vector):** Macro-wrapped, type-inferred auto-growing arrays utilizing internal trait Allocators.
* **`proven_map_t` (Open-Addressing HashMap):** Eliminates linked-list chaining cache misses. Features Tombstone detection and SplitMix64 algorithms for `O(1)` access.
* **`proven_list_node_t` (Intrusive Doubly-Linked List):** Zero-allocation nodes embedded inside parent structures. Deducted mathematically at runtime via `PROVEN_CONTAINER_OF` for kernel-grade optimization.
* **`proven_ring_t`:** Capacity-bounded circular data pipes.

---

## System I/O & Filesystem

Bypasses traditional `stdio.h` bottlenecks via direct abstractions over POSIX and Windows API. Features direct memory mapped files (`mmap`), advisory locking, and unified zero-copy extraction.

```c
proven_fs_handle_t file = proven_sys_fs_open("data.bin", PROVEN_FS_MODE_READ);

// Zero-copy I/O: Map physical file bytes directly into virtual address space
proven_result_mmap_t mapping = proven_mmap_create(file, 0, size, PROVEN_MMAP_PROT_READ, false);

if (proven_is_ok(mapping.err)) {
    proven_u8str_view_t view = proven_mmap_view(mapping.value);
    // Parse binary safely...
    proven_mmap_close(mapping.value);
}
```

---

## The 20-Phase TDD Verification Pipeline

`proven` utilizes `nob.h` (No Build-system plugin) running a strict 20-phase Test-Driven Development pipeline ensuring supreme memory stability.

1. **Foundation & Memory Defense (Phase 1-6):** Proves Out-of-bounds (OOB) defense, automatic alignment math, OOM crash handling, and Polymorphic dynamic dispatch binding guarantees.
2. **Data Structure Engines (Phase 7-12):** Validates memory layout migrations in generic arrays, Hashmap reallocation boundaries (75% Load threshold trigger), and container deduction safety mechanisms without segfaulting. Includes binary search and sorting.
3. **Filesystem & Security (Phase 13-15):** Tests atomic file slurping, directory traversal, sorted listing, permissions (chmod), and advisory locking.
4. **Advanced Modules & Concurrency (Phase 16-20):** Demonstrates Zig-style formatting, Mmap efficiency, System I/O & Env extraction, Stackless state-machine jumps, and 1,000 parallel Lock-Free Atomic Job scheduling.

---

## Quick Start

This library uses a standalone C build script (`nob.c`). No Make, No CMake.

```bash
# 1. Compile the build engine
cc nob.c -o nob

# 2. Run the build pipeline and all 20 TDD phases
./nob
```

---

## Author & License

* **Developed by:** rubidus-api (<rubidus@gmail.com>)
* **License:** [MIT License](LICENSE)
