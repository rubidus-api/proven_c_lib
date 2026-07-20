# Chapter 2: Allocation — Heap, Arenas, Pools, and Buffers

**Part II — The vocabulary every program uses. Prerequisite: [Chapter 1](manual-01-foundation.md).**
**After this chapter** you can choose an allocation strategy deliberately instead of reaching for
`malloc` by reflex, and you will know which of the three costs you are paying.

This chapter covers `heap.h`, `arena.h`, `pool.h`, `allocator.h`, and `buffer.h`. The
thread-safety and pointer-provenance material that used to be section 7 here now lives in
[Chapter 6](manual-06-execution-and-platform.md), with the rest of the concurrency subject — it
was the hardest material in the book sitting in one of the first chapters.

## Table of contents

1. [Why allocation is a parameter, and the heap allocator](#1-why-allocation-is-a-parameter-and-the-heap-allocator)
2. [Arena: many things, one lifetime](#2-arena-many-things-one-lifetime)
3. [Pool: many things, one size](#3-pool-many-things-one-size)
4. [The allocator trait](#4-the-allocator-trait)
5. [Raw byte buffer](#5-raw-byte-buffer)
6. [Examples and misuse cases](#6-examples-and-misuse-cases)

## 1. Why allocation is a parameter, and the heap allocator

### The problem with `malloc`

`malloc` is a global. Any function may call it, nothing in a signature says whether a function
does, and every call goes to the same general-purpose allocator no matter what the memory is for.

That has four consequences you have probably met:

- **You cannot tell from a signature whether a function allocates.** `char *build_message(int n)`
  might allocate, might return a pointer into a static buffer, might return a literal. The type is
  identical in all three cases, so the caller cannot know whether to free, and the answer lives in
  a comment that may be wrong.
- **You cannot change the strategy for one part of a program.** If a parser makes ten thousand
  small allocations that all die at the end of the parse, `malloc` and `free` will do ten thousand
  general-purpose allocations and ten thousand frees. A bump allocator would do one. There is no
  way to say so without rewriting every call.
- **You cannot test the failure path.** Making `malloc` fail on demand means intercepting it
  globally — `LD_PRELOAD`, a linker trick, a `#define malloc my_malloc` that also catches the
  library's internal calls. Meanwhile the branch you most want to test is the one that runs when
  memory runs out.
- **You cannot use it at all where there is no heap.** Firmware, a kernel, a bootloader: no
  `malloc`, and every library that assumes one is unusable.

`malloc` is not badly designed. It is a fixed policy, hardcoded into every call site by the fact
that it takes no parameter saying otherwise.

### What this library does instead

**An allocator is a value, and it is a parameter.** A function that can allocate takes a
`proven_allocator_t`; a function that cannot, does not. That is the whole idea, and every
consequence follows from it:

```text
proven_err_t proven_u8str_append(proven_u8str_t *str, proven_u8str_view_t data);
proven_err_t proven_u8str_append_grow(proven_allocator_t alloc, proven_u8str_t *str, proven_u8str_view_t data);
```

Read the signatures. The first cannot grow the string, so it fails when the text does not fit. The
second can, and says so by taking the allocator. You never have to wonder which one you called.

The same code now works with three different strategies, chosen by the caller:

| Allocator | Get one from | Frees individually? | Use it when |
|---|---|---|---|
| **Heap** | `proven_heap_allocator()` | Yes | The general case. Objects with unrelated lifetimes. |
| **Arena** | `proven_arena_create(backing)`, then `proven_arena_as_allocator(&a)` | **No** — free is a no-op; you reset or destroy the whole thing | Many allocations that all die at the same moment: one request, one frame, one parse. |
| **Pool** | `proven_pool_init(&p, base, size, align, bin_cap)`, then `proven_pool_as_allocator(&p)` | Yes, into a free list | Many objects of **one fixed size**, allocated and freed repeatedly. |

Start with the heap. Reach for the other two when you have a reason, and the reason is usually a
measurement.

### The heap allocator

```text
proven_allocator_t proven_heap_allocator(void);
```

This is `malloc`, `realloc` and `free` wearing the library's interface. It is what you should use
unless you have a specific reason not to, and there is nothing clever about it — which is the
point. Every example in this manual that does not have a reason to do otherwise uses it:

```c
proven_allocator_t heap = proven_heap_allocator();

proven_result_u8str_t s = proven_u8str_create(heap, 64);
if (!proven_is_ok(s.err)) {
    return;   /* nothing was created, so there is nothing to destroy */
}
(void)proven_u8str_append(&s.value, PROVEN_LIT("ready"));
proven_u8str_destroy(heap, &s.value);   /* the SAME allocator */
```

The value is four words — a context pointer and three function pointers — and it is passed by
value. Copying it is free, storing it in your own struct is fine, and it does not need to be
destroyed.

**In freestanding builds `proven_heap_allocator` does not exist.** There is no `malloc` to wrap.
That is not a limitation to work around; it is the reason the whole library takes allocators as
parameters, and it is why an arena over a static array makes the same code run on a
microcontroller. See [freestanding mode](manual-freestanding.md).

Wrong — destroying with a different allocator than you created with:

```text
proven_result_u8str_t s = proven_u8str_create(arena_alloc, 64);
proven_u8str_destroy(heap_alloc, &s.value);   /* wrong: heap free on arena memory */
```

The object does not remember which allocator produced it — that is what keeps it small. Nothing
checks this today, and the failure is heap corruption that surfaces somewhere else, later. Pair
them by construction: keep the allocator next to the object, or pass both together.

## 2. Arena: many things, one lifetime

### The problem: many small allocations with the same death date

Consider parsing a configuration file. You allocate a string for each key, a string for each
value, a node for each section — a few thousand small allocations. Then the parse finishes, you
build your result, and every one of those allocations becomes garbage at the same instant.

With `malloc` you pay for that twice. Each allocation searches a free list, updates bookkeeping,
and returns memory with a header attached; each `free` returns it and possibly coalesces
neighbours. And you must not miss one, because a leak is one forgotten `free` away.

The observation an arena makes is that **all of those objects have the same lifetime**, so
individual frees are pointless work. If they die together, they can be freed together.

### How an arena works

An arena is a block of memory and an offset. Allocating means rounding the offset up to the
alignment you asked for, returning the address, and moving the offset along. That is the entire
algorithm:

```text
[####used####|                    free                    ]
              ^ offset
```

- **Allocation is a few instructions.** No search, no free list, no per-allocation header.
- **`free` does nothing at all.** It is a no-op, deliberately.
- **You reclaim by resetting or destroying the whole arena**, which sets the offset back to zero.
  One operation frees ten thousand objects.

The memory comes from you. `proven_arena_create` takes a `proven_mem_mut_t` — a block you got
from the heap, or a `static` array, or a region on the stack. The arena never allocates on its own,
which is what lets it work with no heap underneath.

### What you give up

An arena is not a general-purpose allocator, and the trade is real:

- **You cannot free one object.** If the lifetimes are not actually shared, an arena leaks by
  design — memory is only reclaimed at reset.
- **Running out is a hard limit.** The backing block is fixed. `PROVEN_ERR_NOMEM` here does not
  mean the machine is out of memory, it means this arena is.
- **Every pointer into it dies at reset**, all at once, with nothing to warn you. A view that
  outlives the reset is a dangling view; see contract 2 in
  [Chapter 0](manual-00-start-here.md#5-the-five-contracts-you-will-meet-on-every-page).

Use an arena when the shared lifetime is a fact about your program, not a hope.

### `proven_arena_t`

```text
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
if (!proven_is_ok(r.err)) {
    return;   /* the arena cannot grow: it reports NOMEM instead */
}

proven_arena_reset(&arena);   /* reclaims r and everything else at once */
proven_arena_destroy(&arena);
```

### Arena with growable containers

Growable containers can use an arena allocator, but growth may abandon earlier blocks because arena `free` is a no-op. Reserve capacity up front when possible.

Correct:

```c
alignas(max_align_t) proven_byte_t storage[4096];
proven_arena_t arena = proven_arena_create((proven_mem_mut_t){
    .ptr = storage,
    .size = sizeof storage,
});
proven_allocator_t a = proven_arena_as_allocator(&arena);

/* Ask for the capacity up front, so growth never has to abandon a block. */
proven_result_array_t ar = PROVEN_ARRAY_INIT(a, int, 128);
if (!proven_is_ok(ar.err)) {
    return;
}
(void)PROVEN_ARRAY_PUSH(&ar.value, int, 10);
PROVEN_ARRAY_DESTROY(&ar.value);   /* correct, but arena free reclaims nothing */
proven_arena_reset(&arena);        /* this is what gives the bytes back */
```

Wrong — growing into an arena from a tiny initial capacity:

```text
proven_result_array_t ar = PROVEN_ARRAY_INIT(a, int, 1);
for (int i = 0; i < 10000; ++i) {
    PROVEN_ARRAY_PUSH(&ar.value, int, i); /* wrong: every regrow abandons the old block */
}
```

Each regrow asks the arena for a bigger block and then "frees" the old one — which, in an arena,
does nothing. The array ends up correct and the arena ends up holding every intermediate size it
ever allocated: 1, 2, 4, 8 … 8192 elements' worth of abandoned space, on top of the one buffer you
wanted. Reserve the capacity up front, as the correct version above does.

Wrong — a view that outlives the reset:

```text
proven_u8str_view_t name = /* ... built in the arena ... */;
proven_arena_reset(&arena);
use(name);                 /* wrong: those bytes are now free space */
```

This is the arena's sharpest edge. Reset does not touch the memory it reclaims, so the bytes are
usually still there and the bug usually does not show up in testing — right up until the next
allocation writes over them.

## 3. Pool: many things, one size

### The problem an arena does not solve

An arena assumes shared lifetimes. Plenty of workloads have the opposite shape: objects of one
type, created and destroyed continuously, in no particular order. A linked list of events. Nodes
in a tree that grows and shrinks. Connection records that come and go.

An arena cannot do this — it never reclaims a single object, so a long-running program would grow
without bound. The heap can, and that is exactly what it costs: every allocation searches, every
free updates bookkeeping, and the general-purpose allocator does that general-purpose work for a
request it already made a thousand times.

The observation a pool makes is that **all these objects are the same size**. If they are the same
size, a freed one fits a future request exactly, so it can be handed straight back out with no
search at all.

### How a pool works

A pool keeps a small stack of freed blocks — the *bin* — and does the obvious thing:

- **Allocate**: if the bin has anything in it, pop one and return it. That is a pointer read and a
  decrement. Only when the bin is empty does it go to the underlying allocator.
- **Free**: if the bin has room, push the block onto it for reuse. If the bin is full, hand the
  memory back to the underlying allocator so the pool does not park memory forever.

`bin_cap` is therefore a dial: how much memory this pool is allowed to keep in reserve.

### What you give up

- **One pool serves exactly one size and alignment.** A request for anything else is refused with
  `PROVEN_ERR_INVALID_ARG` — not served from somewhere else. A stricter alignment than the pool
  was built with is refused too; a looser one is fine, because the block already satisfies it.
- **The pool does not track live objects.** `proven_pool_destroy` frees what is in the bin, not
  what you are still holding. Free everything you allocated first, or you have leaked it *and* it
  is now dangling.

Arena versus pool, in one line each: an arena is for *allocate many, free all at once*; a pool is
for *allocate and free the same size, over and over, cheaply*.

### `proven_pool_t`

```text
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
- `item_size`: exact object size accepted by the pool. Any other size is rejected with `PROVEN_ERR_INVALID_ARG`.
- `item_align`: the alignment every block is allocated with. A request for a *stricter* alignment is rejected with `PROVEN_ERR_INVALID_ARG`; a looser one is served, since the block already meets it.
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

```text
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
proven_err_t e = proven_pool_init(&pool, alloc, sizeof(Node), alignof(Node), 64);
if (!proven_is_ok(e)) {
    return;
}

proven_allocator_t node_alloc = proven_pool_as_allocator(&pool);
proven_result_mem_mut_t n = node_alloc.alloc_fn(node_alloc.ctx, sizeof(Node), alignof(Node));
if (proven_is_ok(n.err)) {
    /* Hand it back before destroy: the pool does not track live items. */
    node_alloc.free_fn(node_alloc.ctx, n.value.ptr);
}

proven_pool_destroy(&pool);
```

Wrong:

```text
node_alloc.alloc_fn(node_alloc.ctx, sizeof(LargerObject), alignof(LargerObject));
/* wrong: one pool is for one fixed object size and alignment */
```

## 4. The allocator trait

### Why this section is fourth and not first

You have now used three allocators without seeing the interface they share, and that was
deliberate. The interface is the least interesting part of the idea and the hardest thing to read
cold: three function-pointer typedefs and an alignment contract mean very little until you have
seen a heap, an arena and a pool behind them.

You need this section for two things: writing an allocator of your own, and understanding the
rules every allocator in this library promises to follow. If you are only ever going to *use* the
three above, `proven_heap_allocator()`, `proven_arena_as_allocator()` and
`proven_pool_as_allocator()` are the whole API and you can skip ahead.

A trait, here, is a struct of function pointers used as an interface — C's version of a virtual
table, written out by hand. `proven_allocator_t` is the library's most important one; `stream.h`
and `random.h` use the same shape.

### Function pointer types

```text
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
- `realloc_fn` changes allocation size and must be failure-atomic: on failure the
  old block is still valid and unmodified, so the caller has lost nothing.
- A block must be reallocated and freed with the **same `align`** it was allocated
  with. An allocator may pick a different underlying mechanism for over-aligned
  requests than for ordinary ones, and the heap allocator does: `align <=
  alignof(max_align_t)` — which is every string, buffer and byte array in this
  library — goes through `malloc`/`realloc` so that growth can happen in place,
  and anything more strictly aligned goes through an aligned allocator. Handing a
  block back under a different alignment class is undefined.
- `free_fn` releases memory. If an allocator needs size metadata, it must track that internally.

### `proven_allocator_t`

```text
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

### Reference

| Member / API | Shape | Contract |
|---|---|---|
| `ctx` | `void *` | The allocator's own state. Opaque to callers; passed back as the first argument. |
| `alloc_fn` | `proven_alloc_fn_t` | Allocate `size` bytes at `align`. Returns `proven_result_mem_mut_t`. |
| `realloc_fn` | `proven_realloc_fn_t` | Resize. **Failure-atomic**: on failure the old block is untouched and still valid. |
| `free_fn` | `proven_free_fn_t` | Release. Must be given the same `align` class the block was allocated with. |
| `proven_alloc_is_valid(alloc)` | `static inline bool` | True when every function pointer is non-null — i.e. the trait can be called. |

The three rules that every allocator in this library obeys, and that yours must:

1. **Same allocator, whole lifetime.** Allocate, reallocate and free a block with one allocator.
2. **Same alignment class.** A block allocated at one alignment must be reallocated and freed at
   that alignment; allocators may use a different mechanism for over-aligned requests.
3. **Failure changes nothing.** A failed `realloc_fn` leaves the old block valid and unmodified.

```text
static inline bool proven_alloc_is_valid(proven_allocator_t alloc);
```

Purpose: check that all function pointers are non-null.

Return: true if the trait can be called.

Correct:

```c
proven_allocator_t heap = proven_heap_allocator();
proven_err_t err = proven_alloc_is_valid(heap) ? PROVEN_OK : PROVEN_ERR_UNSUPPORTED;
(void)err;   /* a freestanding build hands back a zero allocator, not a valid one */
```

Wrong:

```text
proven_allocator_t alloc = {0};
proven_result_mem_mut_t r = alloc.alloc_fn(alloc.ctx, 64, 8); /* wrong: null call */
```

## 5. Raw byte buffer

### What it is for

`proven_buf_t` is the plainest thing in this chapter: a pointer, a length and a capacity. It owns
its bytes, it never grows, and it is what `proven_u8str_t` and `proven_u16str_t` are built out of.

Reach for it directly when you want *bytes* rather than *text* — a record you are assembling, a
frame you are about to write to a socket — and you know the maximum size in advance. When the
content is text, use the string types in [Chapter 3](manual-03-strings-text.md) instead: they give
you the same storage plus NUL-termination, views, searching and formatting.

Like everything else here it does **not** store its allocator, so `proven_buf_destroy` takes the
same one `proven_buf_create` was given.

### `proven_buf_t`

```text
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

```text
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
if (!proven_is_ok(r.err)) {
    return;
}
proven_buf_t buf = r.value;

proven_err_t e = proven_buf_append(&buf, (proven_mem_view_t){
    .ptr = (const proven_byte_t *)"abc",
    .size = 3,
});
(void)e;   /* PROVEN_ERR_OUT_OF_BOUNDS if it would not fit: the buffer never grows */

proven_buf_destroy(alloc, &buf);
```

## 6. Examples and misuse cases

### Match allocator on destroy

Correct:

```c
proven_allocator_t heap = proven_heap_allocator();

proven_result_buf_t r = proven_buf_create(heap, 128);
if (!proven_is_ok(r.err)) {
    return;
}
proven_buf_destroy(heap, &r.value);   /* the same allocator that created it */
```

Wrong:

```text
proven_result_buf_t r = proven_buf_create(heap, 128);
proven_buf_destroy(other_alloc, &r.value); /* wrong: allocator mismatch */
```

### Arena free is a no-op

```c
alignas(max_align_t) proven_byte_t storage[1024];
proven_arena_t arena = proven_arena_create((proven_mem_mut_t){
    .ptr = storage,
    .size = sizeof storage,
});
proven_allocator_t a = proven_arena_as_allocator(&arena);

proven_result_mem_mut_t r = proven_arena_alloc(&arena, 32);
if (proven_is_ok(r.err)) {
    /* Legal, and correct to write - but it intentionally reclaims nothing. */
    a.free_fn(a.ctx, r.value.ptr);
}
proven_arena_reset(&arena);   /* only this gives the 32 bytes back */
```

### Pool size contract

Correct:

```c
typedef struct Node { int value; } Node;

proven_pool_t pool = {0};
if (!proven_is_ok(proven_pool_init(&pool, alloc, sizeof(Node), alignof(Node), 8))) {
    return;
}
proven_allocator_t node_alloc = proven_pool_as_allocator(&pool);

/* The pool serves exactly the size and alignment it was configured with. */
proven_result_mem_mut_t n = node_alloc.alloc_fn(node_alloc.ctx, sizeof(Node), alignof(Node));
if (proven_is_ok(n.err)) {
    node_alloc.free_fn(node_alloc.ctx, n.value.ptr);
}

/* Anything else is refused with PROVEN_ERR_INVALID_ARG rather than served. */
proven_result_mem_mut_t wrong = node_alloc.alloc_fn(node_alloc.ctx, sizeof(Node) * 2, alignof(Node));
(void)wrong;

proven_pool_destroy(&pool);
```

Wrong:

```text
node_alloc.alloc_fn(node_alloc.ctx, sizeof(Other), alignof(Other));
/* wrong: PROVEN_ERR_INVALID_ARG - a pool is for one size and one alignment */
```

### Buffer append does not grow

Wrong:

```text
proven_buf_append(&buf, huge_data); /* returns out-of-bounds; no automatic growth */
```

Use `proven_u8str_append_grow()` or an array if you need growth.

### Buffer append tolerates overlap

Correct:

```c
proven_result_buf_t r = proven_buf_create(alloc, 64);
if (!proven_is_ok(r.err)) {
    return;
}
proven_buf_t buf = r.value;

proven_err_t e = proven_buf_append(&buf, (proven_mem_view_t){
    .ptr = (const proven_byte_t *)"abcdefgh",
    .size = 8,
});
if (proven_is_ok(e)) {
    /* The source view points back into the buffer's own bytes. That is allowed:
     * append moves rather than copies, so the overlap is well defined. */
    e = proven_buf_append(&buf, (proven_mem_view_t){
        .ptr = buf.ptr + 2,
        .size = 4,
    });
}
(void)e;

proven_buf_destroy(alloc, &buf);
```

Wrong:

```text
proven_sys_mem_copy(buf.ptr + buf.len, buf.ptr + 2, 4); /* copy is not the public contract here */
```

### Worked example: an arena over caller-supplied memory

Compiled and run by the test suite. It shows the bump-and-drop lifetime that makes an arena worth reaching for: allocations are nearly free, nothing is individually freed, and `proven_arena_reset` reclaims the whole region at once.

<!-- example: manual/examples/ex_02_arena.c -->
```c
/*
 * An arena does not own memory: it bumps a pointer through memory YOU own. That
 * is the whole trade. Allocation is an add, individual frees do not exist, and
 * you get everything back at once with a reset.
 *
 * The shape that makes it worth using is bump-then-drop: a phase allocates
 * freely, the phase ends, one reset reclaims the lot. No per-object bookkeeping
 * to get wrong, and nothing to leak - the backing storage below is a plain
 * array with automatic storage duration.
 */

int main(void) {
    /* The backing store is the caller's. Over-align it so the arena can satisfy
     * any alignment a caller asks for out of the first byte. */
    alignas(max_align_t) static proven_byte_t storage[4096];

    proven_arena_t arena = proven_arena_create((proven_mem_mut_t){
        .ptr = storage,
        .size = sizeof storage,
    });

    /* --- bump ------------------------------------------------------------- */
    proven_result_mem_mut_t a = proven_arena_alloc(&arena, 64);
    EXAMPLE_REQUIRE(proven_is_ok(a.err), "64 bytes must fit in a 4 KiB arena");
    EXAMPLE_REQUIRE(a.value.ptr == storage, "the first allocation starts at the backing store");

    /* Explicit alignment when the type demands more than PROVEN_DEFAULT_ALIGNMENT.
     * The arena pads to reach it, so the bytes it skips are simply gone until reset. */
    proven_result_mem_mut_t b = proven_arena_alloc_aligned(&arena, 32, 64);
    EXAMPLE_REQUIRE(proven_is_ok(b.err), "an over-aligned block must still fit");
    EXAMPLE_REQUIRE(((uintptr_t)b.value.ptr % 64) == 0, "the block must honour the requested alignment");

    /* --- the arena as an allocator for another API ------------------------- */
    /* Anything in proven that takes a proven_allocator_t can be driven by the
     * arena. The string below therefore lives inside `storage`. */
    proven_allocator_t arena_alloc = proven_arena_as_allocator(&arena);
    EXAMPLE_REQUIRE(proven_alloc_is_valid(arena_alloc), "the arena must expose a usable allocator");

    proven_result_u8str_t s = proven_u8str_create(arena_alloc, 32);
    EXAMPLE_REQUIRE(proven_is_ok(s.err), "the arena should be able to back a 32-byte string");

    proven_err_t err = proven_u8str_append_grow(arena_alloc, &s.value, PROVEN_LIT("scratch line"));
    EXAMPLE_REQUIRE(proven_is_ok(err), "appending into an arena-backed string must succeed");

    /* Destroying it is still correct and still required by the ownership rules -
     * but arena free is a no-op, so it reclaims nothing. That is not a leak: the
     * bytes belong to `storage`, and the reset below is what returns them. */
    proven_u8str_destroy(arena_alloc, &s.value);

    proven_size_t used = arena.offset;
    EXAMPLE_REQUIRE(used > 64, "every allocation above came out of the same backing store");

    /* --- drop -------------------------------------------------------------- */
    /* One statement frees the 64-byte block, the aligned block and the string.
     * Reset costs the same whether ten objects were allocated or ten thousand. */
    proven_arena_reset(&arena);
    EXAMPLE_REQUIRE(arena.offset == 0, "reset must reclaim every allocation at once");

    /* Proof that the storage really is reusable: the next allocation lands back
     * at the start. Every pointer handed out before the reset is dangling now -
     * that is the price of the reset being free. */
    proven_result_mem_mut_t c = proven_arena_alloc(&arena, 64);
    EXAMPLE_REQUIRE(proven_is_ok(c.err), "allocation after reset must succeed");
    EXAMPLE_REQUIRE(c.value.ptr == storage, "after reset the arena bumps from the beginning again");

    /* --- exhaustion is an error, not a crash -------------------------------- */
    proven_result_mem_mut_t too_big = proven_arena_alloc(&arena, sizeof storage);
    EXAMPLE_REQUIRE(too_big.err == PROVEN_ERR_NOMEM, "an arena cannot grow: it reports NOMEM instead");

    printf("arena: %zu bytes used before reset, %zu in use now\n",
           (size_t)used, (size_t)arena.offset);

    /* Formal cleanup. A no-op for a caller-backed arena, but writing it keeps
     * the lifetime obvious if the backing store later becomes heap memory. */
    proven_arena_destroy(&arena);
    return EXAMPLE_OK();
}
```

### Worked example: a pool that recycles fixed-size blocks

Compiled and run by the test suite. It shows a freed block coming straight back out of the recycle bin, and what the pool refuses.
<!-- example: manual/examples/ex_02_pool.c -->
```c
/*
 * A pool is a churn optimizer, not a region. It is for one type: allocate and
 * free the same fixed-size block over and over - list nodes, events, particles -
 * without paying malloc every time.
 *
 * It keeps a small stack of freed blocks (the "bin"). Freeing pushes a block
 * onto the bin instead of returning it to the base allocator; allocating pops
 * one back off. Both are O(1) and neither touches the heap. That recycling is
 * the entire point, and the check below proves it happens.
 *
 * Ownership: the pool caches freed blocks, but it does NOT track the blocks it
 * has handed out. Every block you take, you must give back before destroy - the
 * pool cannot free what it does not know about.
 */

typedef struct {
    int id;
    int score;
} node_t;

int main(void) {
    proven_allocator_t heap = proven_heap_allocator();
    EXAMPLE_REQUIRE(proven_alloc_is_valid(heap), "hosted builds have a heap allocator");

    /* The pool takes a base allocator for the blocks it cannot serve from the
     * bin, plus the exact size and alignment of the one type it manages. The
     * last argument caps how many freed blocks are parked for reuse. */
    proven_pool_t pool = {0};
    proven_err_t err = proven_pool_init(&pool, heap, sizeof(node_t), alignof(node_t), 4);
    EXAMPLE_REQUIRE(proven_is_ok(err), "initializing a pool of node_t must succeed");
    if (!proven_is_ok(err)) {
        return 1;
    }

    proven_allocator_t nodes = proven_pool_as_allocator(&pool);

    /* --- first block: nothing in the bin, so it comes from the heap --------- */
    proven_result_mem_mut_t first = nodes.alloc_fn(nodes.ctx, sizeof(node_t), alignof(node_t));
    EXAMPLE_REQUIRE(proven_is_ok(first.err), "the pool must be able to serve its own item type");
    if (!proven_is_ok(first.err)) {
        proven_pool_destroy(&pool);
        return 1;
    }

    node_t *n = (node_t *)first.value.ptr;
    *n = (node_t){ .id = 1, .score = 100 };
    void *first_addr = n;

    /* --- hand it back: it lands in the bin, not back on the heap ------------ */
    nodes.free_fn(nodes.ctx, n);
    EXAMPLE_REQUIRE(pool.bin_len == 1, "a freed block is cached for reuse, not returned to the heap");
    /* `n` is dangling from here on. The pool owns those bytes again. */

    /* --- second block: the freed one is handed straight back ---------------- */
    proven_result_mem_mut_t second = nodes.alloc_fn(nodes.ctx, sizeof(node_t), alignof(node_t));
    EXAMPLE_REQUIRE(proven_is_ok(second.err), "allocating from a non-empty bin must succeed");
    EXAMPLE_REQUIRE(second.value.ptr == first_addr, "the recycled block is the one that was freed");
    EXAMPLE_REQUIRE(pool.bin_len == 0, "taking it back out empties the bin");

    /* Recycled memory is NOT zeroed for you - it is whatever the pool left there.
     * Initialize every field, exactly as you would for a fresh malloc. */
    node_t *m = (node_t *)second.value.ptr;
    *m = (node_t){ .id = 2, .score = 50 };
    EXAMPLE_REQUIRE(m->id == 2, "the recycled block is ours to overwrite");

    /* --- one pool serves one size and one alignment ------------------------- */
    /* A request for anything else is refused: this is not a general allocator, and it will
     * not silently hand you a block of the wrong size. The code is PROVEN_ERR_UNSUPPORTED -
     * "not my job" - and not INVALID_ARG, which would read as "you passed me garbage" and
     * send you hunting for a bug in your own code. */
    proven_result_mem_mut_t wrong = nodes.alloc_fn(nodes.ctx, sizeof(node_t) * 2, alignof(node_t));
    EXAMPLE_REQUIRE(wrong.err == PROVEN_ERR_UNSUPPORTED, "the pool only serves its configured item size");

    /* --- return every live block before destroying -------------------------- */
    /* proven_pool_destroy frees what is in the bin and the bin itself. `m` is
     * still handed out, so if we skipped this free it would leak. */
    nodes.free_fn(nodes.ctx, m);

    printf("pool: %zu block(s) cached for reuse at teardown\n", (size_t)pool.bin_len);

    proven_pool_destroy(&pool);
    return EXAMPLE_OK();
}
```
