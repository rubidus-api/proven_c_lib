# Chapter 2: Allocation, Arenas, Pools, and Buffers

This chapter covers `allocator.h`, `heap.h`, `arena.h`, `pool.h`, and `buffer.h`.

## Table of contents

1. [Allocator trait](#1-allocator-trait)
2. [Heap allocator](#2-heap-allocator)
3. [Arena allocator](#3-arena-allocator)
4. [Pool allocator](#4-pool-allocator)
5. [Raw byte buffer](#5-raw-byte-buffer)
6. [Examples and misuse cases](#6-examples-and-misuse-cases)
7. [Allocator thread-safety & provenance](#7-allocator-thread-safety--provenance)

## 1. Allocator trait

### Function pointer types

```c
typedef proven_result_mem_mut_t (*proven_alloc_fn_t)(
    void *ctx,
    proven_size_t size,
    proven_size_t align
);

typedef proven_result_mem_mut_t (*proven_realloc_fn_t)(
    void *ctx,
    void *old_ptr,
    proven_size_t old_size,
    proven_size_t new_size,
    proven_size_t align
);

typedef void (*proven_free_fn_t)(void *ctx, void *ptr);
```

Intent:

- `alloc_fn` allocates a new byte slice.
- `realloc_fn` changes allocation size and must be failure-atomic.
- `free_fn` releases memory. If an allocator needs size metadata, it must track that internally.

### `proven_allocator_t`

```c
typedef struct {
    void *ctx;
    proven_alloc_fn_t alloc_fn;
    proven_realloc_fn_t realloc_fn;
    proven_free_fn_t free_fn;
} proven_allocator_t;
```

Fields:

- `ctx`: allocator-specific state.
- `alloc_fn`: allocation function.
- `realloc_fn`: reallocation function.
- `free_fn`: deallocation function.

### `proven_alloc_is_valid`

```c
static inline bool proven_alloc_is_valid(proven_allocator_t alloc);
```

Purpose: check that all function pointers are non-null.

Return: true if the trait can be called.

Correct:

```c
proven_allocator_t alloc = proven_heap_allocator();
if (!proven_alloc_is_valid(alloc)) {
    return PROVEN_ERR_UNSUPPORTED;
}
```

Wrong:

```c
proven_allocator_t alloc = {0};
proven_result_mem_mut_t r = alloc.alloc_fn(alloc.ctx, 64, 8); /* wrong: null call */
```

## 2. Heap allocator

```c
proven_allocator_t proven_heap_allocator(void);
```

Purpose: return a PAL-backed heap allocator.

Hosted behavior: returns a valid malloc-style allocator implemented through the platform memory layer.

Freestanding behavior: returns a zero allocator stub. `proven_alloc_is_valid()` returns false.

Example:

```c
proven_allocator_t heap = proven_heap_allocator();
if (!proven_alloc_is_valid(heap)) {
    return PROVEN_ERR_UNSUPPORTED;
}

proven_result_mem_mut_t r = heap.alloc_fn(heap.ctx, 256, PROVEN_DEFAULT_ALIGNMENT);
if (!proven_is_ok(r.err)) return r.err;

heap.free_fn(heap.ctx, r.value.ptr);
```

## 3. Arena allocator

An arena allocates linearly from caller-provided backing storage. Individual frees are no-ops. Reset discards all allocations at once.

### `proven_arena_t`

```c
typedef struct {
    proven_mem_mut_t backing;
    proven_size_t offset;
} proven_arena_t;
```

Fields:

- `backing`: mutable memory range owned by the caller.
- `offset`: next allocation position within `backing`.

### Arena functions and helpers

| API | Intent | Parameters | Return |
|---|---|---|---|
| `proven_arena_create(backing)` | Initialize an arena over a backing slice. | `backing`: caller-owned memory. | `proven_arena_t`. |
| `proven_arena_reset(arena)` | Discard all arena allocations. | `arena`. | void. |
| `proven_arena_destroy(arena)` | Formal cleanup. | `arena`. | void; no-op for caller-backed arenas. |
| `proven_arena_alloc_aligned(arena, size, align)` | Allocate with explicit alignment. | `arena`, byte `size`, power-of-two `align`. | `proven_result_mem_mut_t`. |
| `proven_arena_realloc_aligned(arena, old_ptr, old_size, new_size, align)` | Reallocate; can extend/shrink tail allocation in place. | old allocation details and new size. | `proven_result_mem_mut_t`. |
| `proven_arena_alloc(arena, size)` | Allocate with `PROVEN_DEFAULT_ALIGNMENT`. | `arena`, byte `size`. | `proven_result_mem_mut_t`. |
| `proven_arena_alloc_aligned_or_panic(arena, size, align)` | Allocate or call panic hook. | `arena`, `size`, `align`. | `proven_mem_mut_t`. |
| `proven_arena_alloc_or_panic(arena, size)` | Default-aligned panic allocation. | `arena`, `size`. | `proven_mem_mut_t`. |
| `proven_arena_as_allocator(arena)` | Expose arena as `proven_allocator_t`. | `arena`. | allocator trait, or zero allocator for null arena. |

Trait adapter helpers:

- `proven_arena_alloc_trait`
- `proven_arena_realloc_trait`
- `proven_arena_free_trait`

These are exposed because the allocator trait needs function pointers. Application code normally calls `proven_arena_as_allocator()` instead.

Example:

```c
alignas(max_align_t) proven_byte_t storage[4096];
proven_arena_t arena = proven_arena_create((proven_mem_mut_t){
    .ptr = storage,
    .size = sizeof storage,
});

proven_result_mem_mut_t r = proven_arena_alloc(&arena, 64);
if (!proven_is_ok(r.err)) return r.err;

proven_arena_reset(&arena);
```

### Arena with growable containers

Growable containers can use an arena allocator, but growth may abandon earlier blocks because arena `free` is a no-op. Reserve capacity up front when possible.

Correct:

```c
proven_allocator_t a = proven_arena_as_allocator(&arena);
proven_result_array_t ar = PROVEN_ARRAY_INIT(a, int, 128);
if (!proven_is_ok(ar.err)) return ar.err;
```

Risky:

```c
proven_result_array_t ar = PROVEN_ARRAY_INIT(a, int, 1);
for (int i = 0; i < 10000; ++i) {
    PROVEN_ARRAY_PUSH(&ar.value, int, i); /* may consume arena storage repeatedly */
}
```

## 4. Pool allocator

A pool recycles fixed-size blocks. It is useful when many objects have exactly the same size and alignment.

### `proven_pool_t`

```c
typedef struct {
    proven_allocator_t base_alloc;
    proven_size_t item_size;
    proven_size_t item_align;
    void **bin;
    proven_size_t bin_cap;
    proven_size_t bin_len;
} proven_pool_t;
```

Fields:

- `base_alloc`: allocator used for new blocks and the recycle bin.
- `item_size`: exact object size accepted by the pool.
- `item_align`: exact object alignment.
- `bin`: cached free-list array.
- `bin_cap`: maximum cached block count.
- `bin_len`: current cached block count.

### How recycling works

The pool does not own one big slab; it allocates each item individually from
`base_alloc` and keeps a small **stack of freed blocks** (the `bin`, an array of up
to `bin_cap` pointers):

- **Allocate.** If `bin_len > 0`, pop the top pointer (O(1), no `base_alloc` call) —
  this is the whole point of the pool. Otherwise fall through to `base_alloc`.
- **Free.** If `bin_len < bin_cap`, push the pointer onto the bin for reuse;
  otherwise (bin full) free it straight back to `base_alloc`. So the bin caps how
  much memory the pool keeps parked for reuse.
- **Destroy.** `proven_pool_destroy` frees every pointer still in the bin and the
  bin array itself — but it does **not** track live (handed-out) items, so you must
  free everything you allocated before destroying the pool.

This makes the pool a churn optimizer for short-lived same-type objects (nodes,
events), not a region allocator. Use an `arena` when you want "allocate many,
free all at once"; use a `pool` when you want "allocate/free the same size over
and over cheaply".

#### Counter-examples

```c
/* WRONG: the pool only handles its configured size/alignment. */
proven_allocator_t a = proven_pool_as_allocator(&pool);   /* item_size == sizeof(Node) */
a.alloc_fn(a.ctx, sizeof(BigThing), alignof(BigThing));   /* not the pool's item -> rejected */

/* WRONG: destroying while items are still live leaks (and dangles) them. */
void *p = a.alloc_fn(a.ctx, sizeof(Node), alignof(Node)).value.ptr;
proven_pool_destroy(&pool);   /* `p` is NOT freed and is now dangling */
/* RIGHT: free every handed-out item first, then destroy. */
```

### Pool functions

| API | Intent | Return |
|---|---|---|
| `proven_pool_init(pool, base_alloc, item_size, item_align, bin_cap)` | Initialize a fixed-size pool. | `PROVEN_OK` or error. |
| `proven_pool_as_allocator(pool)` | Return allocator trait backed by the pool. | `proven_allocator_t`. |
| `proven_pool_destroy(pool)` | Free cached blocks and bin storage. | void. |

Example:

```c
typedef struct Node { int value; } Node;

proven_pool_t pool = {0};
proven_err_t e = proven_pool_init(
    &pool,
    proven_heap_allocator(),
    sizeof(Node),
    alignof(Node),
    64
);
if (!proven_is_ok(e)) return e;

proven_allocator_t node_alloc = proven_pool_as_allocator(&pool);
proven_result_mem_mut_t n = node_alloc.alloc_fn(node_alloc.ctx, sizeof(Node), alignof(Node));
if (proven_is_ok(n.err)) {
    node_alloc.free_fn(node_alloc.ctx, n.value.ptr);
}

proven_pool_destroy(&pool);
```

Wrong:

```c
node_alloc.alloc_fn(node_alloc.ctx, sizeof(LargerObject), alignof(LargerObject));
/* wrong: one pool is for one fixed object size and alignment */
```

## 5. Raw byte buffer

`proven_buf_t` is a fixed-capacity byte buffer. It owns a byte allocation but does not store its allocator.

### `proven_buf_t`

```c
typedef struct {
    proven_byte_t *ptr;
    proven_size_t len;
    proven_size_t cap;
} proven_buf_t;
```

Fields:

- `ptr`: storage pointer.
- `len`: bytes currently used.
- `cap`: bytes allocated.

### `proven_result_buf_t`

```c
typedef struct {
    proven_err_t err;
    proven_buf_t value;
} proven_result_buf_t;
```

### Buffer functions

| API | Intent | Return |
|---|---|---|
| `proven_buf_create(alloc, cap)` | Allocate a non-zero-capacity buffer. | `proven_result_buf_t`; invalid allocator or zero cap returns `PROVEN_ERR_INVALID_ARG`. |
| `proven_buf_append(buf, data)` | Append raw bytes if they fit. | `PROVEN_OK` or error. |
| `proven_buf_destroy(alloc, buf)` | Free buffer storage with the matching allocator. | void. |

Example:

```c
proven_result_buf_t r = proven_buf_create(alloc, 64);
if (!proven_is_ok(r.err)) return r.err;
proven_buf_t buf = r.value;

proven_err_t e = proven_buf_append(&buf, (proven_mem_view_t){
    .ptr = (const proven_byte_t *)"abc",
    .size = 3,
});

proven_buf_destroy(alloc, &buf);
return e;
```

## 6. Examples and misuse cases

### Match allocator on destroy

Correct:

```c
proven_result_buf_t r = proven_buf_create(heap, 128);
if (!proven_is_ok(r.err)) return r.err;
proven_buf_destroy(heap, &r.value);
```

Wrong:

```c
proven_result_buf_t r = proven_buf_create(heap, 128);
proven_buf_destroy(other_alloc, &r.value); /* wrong: allocator mismatch */
```

### Arena free is a no-op

```c
proven_allocator_t a = proven_arena_as_allocator(&arena);
a.free_fn(a.ctx, ptr); /* legal call, but intentionally does not reclaim one object */
```

### Pool size contract

Correct:

```c
node_alloc.alloc_fn(node_alloc.ctx, sizeof(Node), alignof(Node));
```

Wrong:

```c
node_alloc.alloc_fn(node_alloc.ctx, sizeof(Other), alignof(Other));
```

### Buffer append does not grow

Wrong:

```c
proven_buf_append(&buf, huge_data); /* returns out-of-bounds; no automatic growth */
```

Use `proven_u8str_append_grow()` or an array if you need growth.

### Buffer append tolerates overlap

Correct:

```c
proven_buf_t buf = ...;
/* buf already contains initialized bytes */
proven_err_t e = proven_buf_append(&buf, (proven_mem_view_t){
    .ptr = buf.ptr + 2,
    .size = 4,
});
```

Wrong:

```c
proven_sys_mem_copy(buf.ptr + buf.len, buf.ptr + 2, 4); /* copy is not the public contract here */
```

## 7. Allocator thread-safety & provenance

The `proven_allocator_t` trait is just a `ctx` plus three function pointers; it
contains **no synchronization of its own**. Whether concurrent use is safe depends
entirely on the concrete allocator behind the trait, and on how you share the
*objects* the allocator produces. This section states the guarantees, then
explains the deeper pointer-provenance hazards and the lock-free concepts you
would need if you build concurrent structures on top.

### 7.1 What is and isn't thread-safe

| Allocator | Concurrent use of one instance | Why |
|---|---|---|
| `proven_heap_allocator()` | **Safe** for concurrent `alloc`/`realloc`/`free` | The trait is stateless (`ctx == NULL`); it forwards to the platform `aligned_alloc`/`posix_memalign`/`_aligned_malloc`+`free`, which are thread-safe since C11. |
| `proven_arena_t` | **Not safe** | `proven_arena_alloc_*` does a non-atomic read-modify-write of `arena->offset`. |
| `proven_pool_t` | **Not safe** | `proven_pool_*` pop/push the free-list (`bin`, `bin_len`) non-atomically. |

Two rules follow:

1. **"Allocator safe" is not "object safe."** Even with the heap allocator, the
   containers built on it (`proven_u8str_t`, `proven_array_t`, `proven_map_t`, …)
   add no internal locks. A `proven_u8str_t` that one thread appends to while
   another reads it is a data race regardless of how thread-safe the allocator is.
   Shared mutable objects always need *your* synchronization.
2. **Arena and pool must not be shared.** Concurrent `proven_arena_alloc` can tear
   `offset` and hand the *same bytes* to two threads or drop an allocation
   entirely; concurrent pool pop/push can hand out the same slot twice or underflow
   `bin_len`. A data race is undefined behavior on its own — see §7.3 for why the
   consequences are worse than "a wrong number."

### 7.2 Pointer provenance in one paragraph

In C's abstract machine every pointer carries **provenance**: the identity of the
storage instance it was derived from. Two rules matter here: (a) using a pointer
outside the lifetime or bounds of its provenance object is undefined; and (b) the
optimizer is allowed to assume that **pointers with different provenance do not
alias**, and to reorder or elide memory accesses on that basis. Allocation,
reallocation, and freeing all create and destroy provenance — which is exactly why
they interact badly with unsynchronized sharing.

### 7.3 Where allocation + provenance bite under threads

- **`realloc` always relocates here.** The platform `realloc` is *allocate-new +
  copy + free-old* (an aligned block cannot be resized in place portably), so a
  successful grow **always** returns a new object with new provenance and ends the
  old one. Any retained copy of the old pointer — a cached element pointer, a
  borrowed `proven_mem_view_t`/`proven_u8str_view_t` — is now dangling. In a single
  thread you avoid this by not holding a view across a grow; under threads another
  thread can grow a shared container at *any* instant, leaving your view pointing
  into freed storage (rule (a): UB, plus a data race).
- **Address reuse / ABA.** `free` then `alloc` (especially the pool's free-list
  recycling) can return the *same address* for a *different* object with *fresh*
  provenance. A thread still holding the old pointer assumes the old provenance; by
  rule (b) the compiler may treat the two as non-aliasing even though the bytes
  coincide, and without a happens-before edge the new object's writes need not be
  visible. Same address, different provenance — still UB.
- **`uintptr_t` round-trips + torn reads.** The arena converts `backing.ptr` to an
  integer for alignment math. It is careful to derive the *result* pointer by
  offsetting the original `backing.ptr` (preserving its provenance) rather than
  fabricating a pointer from the integer — the provenance-correct technique. But if
  `offset` is read torn under a race, the computed pointer can land *outside* the
  backing object, i.e. an access with no valid provenance for that location. The
  race produces not just a wrong offset but a pointer with no right to point there.
- **Data race × provenance reasoning = miscompilation, not just wrong values.**
  Because the compiler applies single-threaded, provenance-based non-aliasing
  reasoning within each thread, a racy allocator can let the optimizer "prove"
  non-aliasing that does not hold at runtime — yielding torn pointers, double
  allocations the compiler believes cannot overlap, or dropped stores. The failure
  mode is structural, not a flaky value.

### 7.4 The lock-free toolbox (concepts, if you build concurrency on top)

`proven` does **not** implement any lock-free allocator or safe-memory-reclamation
scheme. If you build concurrent data structures over these allocators, you supply
the following yourself (`<stdatomic.h>` is available — the job system in Chapter 6
uses it for its queue indices). These are the standard pieces and how they relate
to the provenance hazards above.

- **CAS (compare-and-swap).** The atomic primitive behind almost all lock-free
  code: `atomic_compare_exchange_strong(&p, &expected, desired)` atomically sets
  `p = desired` only if `p == expected`, otherwise reports the current value. It
  lets one thread publish a change only if no other thread changed the word first.

  ```c
  /* Treiber-stack style push (sketch) */
  _Atomic(node_t *) head;
  node_t *old = atomic_load(&head);
  do { n->next = old; } while (!atomic_compare_exchange_weak(&head, &old, n));
  ```

- **The ABA problem.** CAS compares *values*, not *history*. A lock-free pop reads
  `head == A`, plans to install `A->next`, then CASes. If, in between, other
  threads pop `A`, pop its successor, free them, and push `A` back (its address
  reused), the CAS still sees `head == A` and *succeeds* — installing a pointer to
  freed memory. The value matched (A→B→A) but the world changed. The pool's
  free-list is a textbook ABA candidate if naïvely made lock-free.
- **Tagged pointers / version counters.** Pack a monotonically increasing tag next
  to the pointer and CAS them together (a double-width CAS, or by stealing the
  low alignment bits / high bits). Every successful update bumps the tag, so an
  A→B→A sequence comes back with a *different* tag and the CAS fails — ABA detected.
  Costs/limits: bit-stealing needs guaranteed alignment and shrinks the usable
  address range; a full-width tag needs hardware double-word CAS (e.g. `cmpxchg16b`);
  the tag can in principle wrap. **Crucially, a tag fixes ABA *detection* on the
  shared word — it does not restore provenance.** The reused address is still a new
  object; the tag only stops you from *acting* on the stale view.
- **Hazard pointers (safe memory reclamation).** Each thread owns a few
  single-writer/multi-reader "hazard" slots. Before dereferencing a shared pointer
  it publishes that pointer into a slot and re-validates; a thread that wants to
  free an object first scans every hazard slot and **defers** the free (a retire
  list) if anyone is protecting it. This bounds memory and prevents use-after-free
  and reclamation-ABA, at the cost of a store + fence per protected access and a
  fixed number of simultaneously protected pointers per thread.
- **Epoch-based reclamation (EBR).** A global epoch counter; a thread "pins" the
  current epoch while touching shared structures, and retired memory is tagged with
  the epoch it was retired in. Memory retired in epoch *e* is freed only once every
  thread has been observed past *e* (a grace period of a couple of epochs). Cheaper
  per operation than hazard pointers (just a pinned flag), but a thread that stalls
  while pinned blocks all reclamation — unbounded memory growth. Variants:
  quiescent-state (QSBR) and interval-based reclamation.
- **How these tie back to provenance.** Reclamation schemes (hazard pointers, EBR)
  exist precisely to keep a freed object's *storage alive* — its provenance valid —
  until no thread can still reference it. CAS + a tag keep the shared *word*
  consistent but say nothing about the lifetime of what it points to. That is why a
  correct lock-free stack typically needs **both**: a tag (for ABA on the head word)
  *and* an SMR scheme (for safe reclamation of popped nodes). A `proven` pool or
  arena gives you neither, so concurrency must be layered above them, not assumed.

### 7.5 Safe patterns with `proven`

- **Per-thread arena/pool.** Give each thread its own `proven_arena_t` /
  `proven_pool_t`. No sharing means no race and no cross-thread provenance — the
  simplest correct design, and usually the fastest.
- **Heap for cross-thread alloc/free, but synchronize the objects.** It is fine for
  thread A to allocate and thread B to free via `proven_heap_allocator()`; it is
  *not* fine for both to mutate the same `proven_array_t`/`proven_u8str_t` without a
  lock.
- **Hand off ownership with a happens-before edge.** Transfer a pointer to another
  thread through a mutex, atomic with release/acquire ordering, or a queue (like the
  job system) so that only one thread observes it at a time. This keeps each
  allocation's provenance confined to a single thread and makes the producer's
  writes visible to the consumer.
- **Do not pass borrowed views across threads** unless the owner is guaranteed not
  to grow/move/free for the whole duration of the borrow — and remember a grow here
  always relocates.
- **If you must share an arena/pool, wrap it** in your own mutex (or build a real
  lock-free allocator with the tools in §7.4); the built-in ones assume a single
  owner at a time.
