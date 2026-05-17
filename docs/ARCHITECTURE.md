# Architecture

## Overview
*   **Core Library**: Uses custom types (`proven_u8`, `proven_size_t`) mapping directly to underlying data streams. Uses a Polymorphic VTable Allocator (`proven_allocator_t`).
*   **Platform Abstraction**: All standard library OS interfaces are strictly isolated to the `/platform/` layer (e.g., `proven_sys_mem.c`).
*   **Error Handling**: Returns `Result` structures (e.g., `proven_result_mem_mut_t`) containing both Error code and value. Do NOT use macros hiding early returns.

## Concurrency
*   **Minimal Internal Locking**: Standard structures avoid hidden locks to maximize single-thread performance.
*   **Stackless Coroutines**: Utilizes macros (`PROVEN_CORO_BEGIN`, `PROVEN_CORO_YIELD`) based on Duff's device.
*   **Bounded MPMC Scheduler**: Ring buffer approach using atomic headers without mutexes.

## Memory Model
*   Strict aliasing and provenance rules apply. Data handles are distinct from memory pointers.
