# `proven` Library Test Documentation

This document records the full suite of validations implemented during the Test-Driven Development (TDD) cycle of the `proven` library. No external testing frameworks are utilized; all tests are self-contained C files compiled and executed strictly via the `nob.c` build script to guarantee `no-CRT` conformity and architectural integrity.

## Execution
Run all tests simultaneously by invoking the build script at the root directory:
```bash
cc nob.c -o nob
./nob
```

---

## Test Phases & Verification Matrix

### Phase 1: Core Primitives (`tests/test_phase1.c`)
**Intent:** To fundamentally prevent "pointer decay" (where arrays or pointers lose their length when passed to functions) by establishing and verifying a safe Slice struct system.
*   **Test 1.1 (Memory Slice View):** Verifies that `.ptr` and `.size` properties are mapped accurately without omission when creating a `proven_mem_view_t` struct, ensuring the integrity of read-only data.
*   **Test 1.2 (Memory Slice Mut):** Checks the structural foundation of the writable `proven_mem_mut_t` type, ensuring it prevents out-of-bounds access beyond its defined boundary (`.size`) when assigned a buffer address.

### Phase 2: Alignment Utilities (`tests/test_phase2.c`)
**Intent:** To verify the bitwise arithmetic that aligns pointer addresses to Power-of-2 physical boundaries, suppressing memory alignment errors and maximizing CPU cache efficiency.
*   **Test 2.1 (Align Up Arithmetic):** Evaluates `proven_mem_align_up()` by passing dynamically allocated arbitrary memory addresses, verifying via bitmask matching that it accurately performs ceiling operations to requested alignment units (e.g., 8-byte, 16-byte, 32-byte).

### Phase 3: Error & Result Primitives (`tests/test_phase3.c`)
**Intent:** To eliminate implicit errors caused by ambiguous `-1` or `NULL` returns, and verify the integrity of explicit **Result patterns** coupled with compiler warnings (`[[nodiscard]]`).
*   **Test 3.1 (Macro Evaluation):** Cross-verifies via boolean logic that the `proven_is_ok` function accurately evaluates the success code (`PROVEN_OK` == 0).
*   **Test 3.2 (Result Structure Integration):** Verifies the `proven_result_mem_mut_t` type: on success, the allocated slice is safely contained in `.value`; on failure (e.g., `PROVEN_ERR_OUT_OF_BOUNDS`), the internal pointer is strictly blocked (`NULL` setting) and only a clear system error code (`err`) is emitted.

### Phase 4: Arena Linear Allocator (`tests/test_phase4.c`)
**Intent:** To verify that the Region-based Bump Allocator securely defends its limits while distributing memory at ultra-high speeds (O(1) allocation and deallocation).
*   **Test 4.1 (Arena Creation):** Checks that the starting `offset` is perfectly initialized to 0 when creating the object by passing a pre-allocated array pointer from user-space (e.g., block size 128).
*   **Test 4.2 (Standard & Aligned Alloc):** Tracks whether the internal offset of the Arena bumps accurately according to the calculated padding space during standard 10-byte allocations and custom 32-byte aligned 4-byte allocations.
*   **Test 4.3 (OOM Defense):** Verifies that when an intentionally large allocation (e.g., 200 bytes) exceeding the Arena's remaining limit (e.g., 128) is requested, it safely and gracefully identifies/returns `PROVEN_ERR_NOMEM` without causing a forced exit or Segfault error.
*   **Test 4.4 (Arena Reset):** Proves that after calling `proven_arena_reset()` (which performs single-pass batch deallocation to eliminate fragmentation), the offset is immediately rolled back to 0 internally, allowing reallocation at 100% efficiency.

### Phase 5: Buffer & U8 Strings (`tests/test_phase5.c`)
**Intent:** To verify that secure string handling and dynamic sequence processing can run independently without relying on string functions (`strlen`, `strcpy`, etc.) included in the OS or `libc`.
*   **Test 5.1 (Poly-Buffer Bound Check):** Checks that allocations are performed via the injected Allocator interface upon `proven_buf_t` creation, and that the `PROVEN_ERR_OUT_OF_BOUNDS` shield triggers immediately when attempting to `append` data exceeding the pre-allocated capacity (`cap`).
*   **Test 5.2 (Zero-Cost Macro Literal):** Verifies that the `PROVEN_LIT("Hello")` macro, which captures string size at compile time, immediately sets `.size = 5` without runtime loop costs (`strlen`).
*   **Test 5.3 (U8 Null-Sealing & Concat):** Proves that when assembling multiple string fragments with `proven_u8str_append`, apart from boundary defense, the tail is strictly sealed with a `\0` value to prevent string corruption when passing to existing C libraries.
*   **Test 5.4 (View Cast vs C-String Extraction):** Verifies the integrity of two extraction tracks: Zero-Cost extraction (`as_cstr`) which only casts the original pointer, and isolated copy generation (`view_to_cstr`) using the VTable Allocator.
*   **Test 5.5 (Short-Circuit Equality):** Examines the `proven_u8str_view_eq` comparison function to verify that it makes an early reject decision at O(1) speed by comparing `.size` before unnecessarily iterating through O(N) bytes.

### Phase 6: Polymorphic Deallocation Strategies (`tests/test_dealloc.c`)
**Intent:** To ensure that the deallocation (`free_fn`) mechanism implemented via Object-Oriented VTable factory traits dynamically selects the correct logic depending on the context (`Arena` vs `Heap`).
*   **Test 6.1 (Arena No-Op Free Check):** Observes whether individual element deletion calls via `.free_fn` in an Arena context are safely ignored (`No-Op`) to prevent fragmentation.
*   **Test 6.2 (Heap PAL Delegation):** Indirectly verifies that calling `free_fn` in a Heap context is routed strictly through the structural Platform Abstraction Layer (PAL), executing the actual target system's memory release (`proven_sys_mem_free`) logic.

### Phase 7: U8Str Mutation & Searching (`tests/test_phase7_u8str_mut.c`)
**Intent:** To expand character string processing into fully-fledged data manipulation, providing rigorous mutation functionalities (replace, insert, remove) strictly bound to memory capacity, and view-level search boundaries (find, slice) retaining zero runtime overhead where possible.
*   **Test 7.1 (Advanced View Search):** Validates rolling iterations by manipulating `start_offset` within `proven_u8str_view_find`. Ensures it flawlessly skips previously scanned sections yielding efficiency.
*   **Test 7.2 (Prefix, Suffix, Slice):** Tests `starts_with` and `ends_with` implementations for parser tokenizing needs, alongside `proven_u8str_view_slice` guaranteeing it restricts OOB indexing implicitly.
*   **Test 7.3 (Unified Atomic Mutation):** Examines `proven_u8str_replace_at`, `insert`, `remove`, tracking string length updates (exact, shrink, expand directions), ensuring remaining strings are pulled/pushed efficiently safely avoiding memory overlaps.
*   **Test 7.4 (Replace First Integration):** Tests whether extracting offset paths gracefully links `find` algorithms straight into localized `replace_at` execution points seamlessly.
*   **Test 7.5 (Ironclad OOB Rejection):** Strictly ensures that expanding mutations instantly reject state manipulation returning `PROVEN_ERR_OUT_OF_BOUNDS` keeping string values 100% untampered if it overflows requested allocator `cap`.

### Phase 8: Dynamic Array (Generic Vectors) (`tests/test_phase8_array.c`)
**Intent:** To implement a fully generic, type-safe Dynamic Array (Vector) in C, utilizing robust zero-overhead offset tracking while gracefully executing Polymorphic auto-migrations (Growth).
*   **Test 8.1 (Creation & Macros):** Asserts robust generic alignments verifying the extraction macro `PROVEN_ARRAY_INIT` and checking standard boundaries on structurally typed inputs via `PROVEN_ARRAY_PUSH` making temporary rvalue clones safely.
*   **Test 8.2 (Polymorphic Growth Migration):** Validates pointer swap logic when pushing over physical capacity constraints. Strictly validates that inner block references forcefully shift their domains carrying existing data exactly.
*   **Test 8.3 (Popping and Bound Guards):** Evaluates exact bounds restrictions asserting invalid indexes strictly yield runtime `NULL` bypassing undefined overflows, while validating sequential structural destructuring (pop).
*   **Test 8.4 (Stack-Arena Integration & Zero-Copy Extension):** Injects array vectors into fully stack-based non-allocating bump arenas validating identical core algorithms seamlessly operate without incurring true system allocation penalties. Validates **Zero-Copy Trait Optimization** mathematically extending contiguous bounds exactly holding the original Memory Pointer permanently until external interrupts block tail logic efficiently.

### Phase 9: Intrusive Doubly-Linked List (`tests/test_phase9_list.c`)
**Intent:** Introduce a "Zero-Allocation" Kernel-style caching structure negating standard boxed models, utilizing standard C bounds math (`offsetof`) to embed and track nodes securely ignoring external heap interactions fundamentally guaranteeing cache locality logic.
*   **Test 9.1 (Zero-Allocation Hooking):** Validates generic insertion boundaries (`proven_list_push_back`) injecting payload trackers straight onto stack structures explicitly demonstrating OOM protection scaling organically without hitting memory triggers.
*   **Test 9.2 (Mathematical Offset Extraction):** Asserts core mechanism `PROVEN_CONTAINER_OF` resolving exact struct-level reverse offsets and reconstructing complex consumer values smoothly validating C's byte-shifting Provenance model perfectly tracks memory layout logic seamlessly.
*   **Test 9.3 (Safe Deletion Traversal):** Enforces safe memory cleanup loops using `PROVEN_LIST_FOR_EACH_SAFE` protecting current pointer variables during internal iterations while `proven_list_remove` actively detaches them eliminating segfault segmentation risk vectors.

### Phase 10: Intrusive Ring Buffer (Circular Queue) (`tests/test_phase10_ring.c`)
**Intent:** Extends core structures forming extremely fast, strictly bounded structural logic wrapping indices directly mapping arrays forming continuous cyclic data streams immune from fragmentation entirely.
*   **Test 10.1 (Zero-Allocation Circular Wrapping):** Inserts limits restricting allocations aggressively demonstrating physical pointer index wraps simulating infinite data feeding behavior cleanly reusing available domain slices securely efficiently.
*   **Test 10.2 (Overfill & Starvation Rejection Checks):** Exhausts the ring triggering full state capacity boundaries resulting in OOB warnings shielding underlying memory. Demonstrates exact inverse protection on starvation (pop looping on zero entities) gracefully stopping logic operations explicitly.

### Phase 11: Open-Addressing Hash Map Engine (`tests/test_phase11_map.c`)
**Intent:** Introduce a core routing and lookup architecture bypassing O(N) penalties without incurring Pointer Chaining fragmentation penalties. Unifies mathematically heavy hashing paths (FNV-1a / SplitMix64) generating unified memory contiguous `[ State | Key | Value ]` structures scaling transparently.
*   **Test 11.1 (Integer Key Fast Lookups & Tombstones):** Asserts correct logical placements handling exact index overrides matching structural states correctly. Injects structural Tombstone evaluations proving Open-Addressing iteration probes skip deactivated chunks mathematically perfectly resolving memory leaks inherently.
*   **Test 11.2 (U8String Native Views & Dynamic Migrations):** Proves native string view extraction generating deterministic lookup indexes exactly accurately. Simulates massive injection counts crossing the 75% load factor explicitly forcing entire array physical block duplications carrying embedded structures correctly realigned cleanly.

### Phase 12: Generic Sorting & Searching (`tests/test_phase12_algorithm.c`)
**Intent:** Provide high-performance algorithm primitives for data processing.
*   **Test 12.1 (Quicksort - Integers):** Validates the generic quicksort implementation sorts a primitive array of integers in O(n log n) time in-place.
*   **Test 12.2 (Quicksort - Structs):** Demonstrates sorting complex structures using a custom comparator (e.g., Score DESC, ID ASC).
*   **Test 12.3 (Search Algorithms):** Verifies O(log n) lookup on a correctly sorted array via `proven_array_binary_search`, and validates fallback `proven_array_linear_search` logic for unsorted arrays.

### Phase 13: System File I/O (`tests/test_phase13_fs.c`)
**Intent:** Provide a secure, platform-abstracted file system interface.
*   **Test 13.1 (Atomic Write):** Validates opening a file with `CREATE | WRITE` modes and accurately flushing byte views onto disk accurately.
*   **Test 13.2 (Read All Utility):** Tests the high-level `proven_fs_read_all` helper which combines file size detection, allocator interaction, and sequential reads into a single result.
*   **Test 13.3 (File Size Detection):** Verifies that `proven_fs_size` accurately reports the physical byte count on disk matching input constants.

### Phase 14: Advanced Filesystem & Directory Engine (`tests/test_phase14_fs_advanced.c`)
**Intent:** Provide high-level management of directories and file lifecycle states.
*   **Test 14.1 (Dir Lifecycle):** Validates atomic `mkdir` and `rmdir` ensuring directory presence before and after operations.
*   **Test 14.2 (Move & Rename):** Proves that file identifiers can be migrated across paths using `proven_fs_rename` without data loss.
*   **Test 14.3 (Sorted Directory Listing):** Lists directory contents into a `proven_array_t`, verifying that entries are automatically sorted (Directories first, then alphabetical) using the internal sorting engine.
*   **Test 14.4 (File Copy):** Verifies the buffered copy mechanism for duplicating files efficiently without loading entire files into memory.

### Phase 19: Stackless Coroutines (`tests/test_phase19_coro.c`)
**Intent:** Introduce hyper-lightweight (4 bytes) Zero-overhead asynchronous state machines directly on plain single-core C using Duff's Device macro loops.
*   **Test 19.1 (Duff's Switch Yielding):** Simulates a multi-part long running network task iteratively yielding processing ticks without engaging `malloc` or complex hardware threaded stack management explicitly returning program execution flawlessly.

### Phase 20: Lock-Free Multi-core Job Pool (`tests/test_phase20_job.c`)
**Intent:** Deliver absolute maximum throughput parallel computing across real CPU cores avoiding pessimistic kernel locks utilizing true C11 Atomic (`<stdatomic.h>`) Ring Buffer scheduling (MPMC).
*   **Test 20.1 (High Contention Submits):** Submits 1,000 parallel computing commands across 4 physically dedicated OS Worker Threads demonstrating lock-free bounds evaluation and exact state persistence without a singular segfault or dead-lock vector.
*   **Test 20.2 (Atomic Uniqueness Audit):** Proves Data Race immunity inherently mapping exact tracking arrays guaranteeing absolute isolation showing exact `1:1` workload processing ratios perfectly.