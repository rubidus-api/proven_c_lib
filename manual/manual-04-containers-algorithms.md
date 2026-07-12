# Chapter 4: Containers and Algorithms

This chapter covers `array.h`, `list.h`, `ring.h`, `map.h`, and `algorithm.h`.

## Table of contents

1. [Dynamic array](#1-dynamic-array)
2. [Intrusive list](#2-intrusive-list)
3. [Ring buffer](#3-ring-buffer)
4. [Hash map](#4-hash-map)
5. [Algorithms](#5-algorithms)
6. [Hashing, by use case](#6-hashing-by-use-case)
7. [Examples and misuse cases](#7-examples-and-misuse-cases)

## 1. Dynamic array

`proven_array_t` is a generic growable vector. It stores its allocator internally and owns contiguous element storage.

### Structures

```text
typedef struct {
    proven_allocator_t alloc;
    proven_byte_t *data;
    proven_size_t len;
    proven_size_t cap;
    proven_size_t elem_size;
    proven_size_t align;
} proven_array_t;

typedef struct {
    proven_err_t err;
    proven_array_t value;
} proven_result_array_t;
```

Fields:

- `alloc`: allocator used for growth and destruction.
- `data`: byte storage for elements.
- `len`: current element count.
- `cap`: capacity in elements.
- `elem_size`: size of each element.
- `align`: alignment of each element.

### Functions

| API | Intent | Return |
|---|---|---|
| `proven_array_create(alloc, init_cap, elem_size, align)` | Create a generic array. | `proven_result_array_t`. |
| `proven_array_is_valid(arr)` | Validate public array invariants. | `bool`. |
| `proven_array_reserve(arr, new_cap)` | Ensure at least `new_cap` elements. | `proven_err_t`. |
| `proven_array_push(arr, element)` | Append one element by copying from pointer. | `proven_err_t`. |
| `proven_array_pop(arr, out_element)` | Pop last element; `out_element` may be null to discard. | `proven_err_t`. |
| `proven_array_get_mut(arr, index)` | Get mutable element pointer. | pointer or null. |
| `proven_array_get(arr, index)` | Get const element pointer. | pointer or null. |
| `proven_array_destroy(arr)` | Free storage and clear state. | void. |

### Macros

| Macro | Intent |
|---|---|
| `PROVEN_ARRAY_INIT(alloc, type, init_cap)` | Type-safe create using `sizeof(type)` and `alignof(type)`. |
| `PROVEN_ARRAY_PUSH(arr_ptr, type, value)` | Push rvalue or lvalue by temporary compound literal. |
| `PROVEN_ARRAY_POP(arr_ptr, type, out_ptr)` | Pop into output pointer or discard with null. |
| `PROVEN_ARRAY_GET(arr_ptr, type, index)` | Typed const element pointer. |
| `PROVEN_ARRAY_GET_MUT(arr_ptr, type, index)` | Typed mutable element pointer. |
| `PROVEN_ARRAY_DESTROY(arr_ptr)` | Destroy array. |

Example:

```c
proven_result_array_t r = PROVEN_ARRAY_INIT(alloc, int, 4);
if (!proven_is_ok(r.err)) {
    return;
}
proven_array_t nums = r.value;

(void)PROVEN_ARRAY_PUSH(&nums, int, 10);
(void)PROVEN_ARRAY_PUSH(&nums, int, 20);

/* GET points into the array's own storage, and returns NULL out of range. */
const int *first = PROVEN_ARRAY_GET(&nums, int, 0);
if (first) {
    proven_println("first={}", PROVEN_ARG(*first));
}

/* The array stores its allocator, so destroy needs nothing but the array. */
PROVEN_ARRAY_DESTROY(&nums);
```

## 2. Intrusive list

`proven_list_t` is an intrusive doubly-linked circular list. It allocates no nodes. Each user object embeds a `proven_list_node_t`.

### Structures

```text
typedef struct proven_list_node_t {
    struct proven_list_node_t *next;
    struct proven_list_node_t *prev;
} proven_list_node_t;

typedef struct {
    proven_list_node_t head;
} proven_list_t;
```

### Functions and macros

| API | Intent | Return |
|---|---|---|
| `proven_list_init(list)` | Initialize circular sentinel. | void. |
| `proven_list_insert_after(target, node)` | Insert `node` after `target`. | void. |
| `proven_list_push_back(list, node)` | Insert at tail. | void. |
| `proven_list_remove(node)` | Detach node and poison links to null. | void. |
| `proven_list_is_empty(list)` | Test empty or null list. | int truth value. |
| `PROVEN_CONTAINER_OF(ptr, type, member)` | Convert member pointer to parent object pointer. | pointer. |
| `PROVEN_LIST_FOR_EACH(iter, list)` | Iterate nodes. | loop macro. |
| `PROVEN_LIST_FOR_EACH_SAFE(iter, safe_next, list)` | Iterate safely while removing current node. | loop macro. |
| `PROVEN_LIST_ENTRY(ptr, type, member)` | Convert list node to containing object. | pointer. |

Example:

```c
typedef struct Item {
    int value;
    proven_list_node_t link;   /* the list lives inside the object; it allocates nothing */
} Item;

proven_list_t list;
proven_list_init(&list);

Item a = { .value = 1 };
Item b = { .value = 2 };
proven_list_push_back(&list, &a.link);
proven_list_push_back(&list, &b.link);

int total = 0;
proven_list_node_t *it = NULL;
PROVEN_LIST_FOR_EACH(it, &list) {
    /* The iterator walks nodes; ENTRY converts a node back to its owner. */
    Item *item = PROVEN_LIST_ENTRY(it, Item, link);
    total += item->value;
}
proven_println("total={}", PROVEN_ARG(total));   /* 3 */
```

## 3. Ring buffer

`proven_ring_t` is a fixed-capacity FIFO. Push fails when full. It stores its allocator internally.

### Structures

```text
typedef struct {
    proven_allocator_t alloc;
    proven_mem_mut_t internal;
    proven_size_t head;
    proven_size_t tail;
    proven_size_t len;
    proven_size_t cap;
    proven_size_t elem_size;
    proven_size_t align;
} proven_ring_t;

typedef struct {
    proven_err_t err;
    proven_ring_t value;
} proven_result_ring_t;
```

### Functions and macros

| API | Intent | Return |
|---|---|---|
| `proven_ring_create(alloc, cap, elem_size, align)` | Create fixed-capacity ring. | `proven_result_ring_t`. |
| `proven_ring_is_valid(ring)` | Validate ring invariants. | `bool`. |
| `proven_ring_push(ring, element)` | Push one element; fails when full. | `proven_err_t`. |
| `proven_ring_pop(ring, out_element)` | Pop one element; null output discards. | `proven_err_t`. |
| `proven_ring_destroy(ring)` | Free storage. | void. |
| `PROVEN_RING_INIT(alloc, type, cap)` | Type-safe create. | `proven_result_ring_t`. |
| `PROVEN_RING_PUSH(ring_ptr, type, value)` | Type-safe push. | `proven_err_t`. |
| `PROVEN_RING_POP(ring_ptr, type, out_ptr)` | Type-safe pop. | `proven_err_t`. |
| `PROVEN_RING_DESTROY(ring_ptr)` | Destroy ring. | void. |

Example:

```c
proven_result_ring_t r = PROVEN_RING_INIT(alloc, int, 8);
if (!proven_is_ok(r.err)) {
    return;
}
proven_ring_t q = r.value;

proven_err_t e = PROVEN_RING_PUSH(&q, int, 7);
if (!proven_is_ok(e)) {
    /* The ring is fixed-capacity: a full ring is PROVEN_ERR_OUT_OF_BOUNDS,
     * never a silent grow. */
    PROVEN_RING_DESTROY(&q);
    return;
}

int out = 0;
e = PROVEN_RING_POP(&q, int, &out);   /* FIFO: out == 7. Empty ring is OUT_OF_BOUNDS. */
(void)e;

PROVEN_RING_DESTROY(&q);
```

## 4. Hash map

`proven_map_t` is an open-addressing hash map with tombstones. It supports integer keys and borrowed or owned U8 string keys. It stores its allocator internally.

### Structures

```text
typedef enum {
    PROVEN_KEY_TYPE_INT,          /* keys are proven_size_t integers */
    PROVEN_KEY_TYPE_U8_BORROWED,  /* keys are u8 views; caller keeps the bytes alive */
    PROVEN_KEY_TYPE_U8_OWNED      /* keys are u8 views; the map copies and frees the bytes */
} proven_key_type_t;

typedef union {
    proven_size_t id;             /* used when key_type == PROVEN_KEY_TYPE_INT */
    proven_u8str_view_t str;      /* used for the two U8 key modes */
} proven_map_key_t;

typedef struct {
    proven_allocator_t alloc;     /* allocator for the bucket array and owned keys */
    proven_mem_mut_t internal;    /* the single contiguous bucket array (ptr + size) */
    proven_size_t len;            /* live entries */
    proven_size_t used;           /* live entries + tombstones (drives the load factor) */
    proven_size_t cap;            /* number of buckets, always a power of two */
    proven_size_t elem_size;      /* size of one stored value */
    proven_size_t align;          /* alignment requested for the value */
    proven_size_t bucket_stride;  /* bytes per bucket: align_up(header + elem_size) */
    proven_size_t payload_offset; /* bytes from a bucket start to its value payload */
    proven_key_type_t key_type;   /* key mode chosen at create time */
} proven_map_t;

typedef struct {
    proven_err_t err;
    proven_map_t value;
} proven_result_map_t;
```

### How it works internally

The map is a single flat array of `cap` buckets — there are no per-entry
allocations for values (and none for keys in INT/BORROWED mode), so lookups stay
cache-friendly. Each bucket is laid out as:

```
[ header: state + key ][ padding to the value's alignment ][ value payload ]
^ bucket start                                              ^ payload_offset
|<------------------------- bucket_stride ------------------------------------>|
```

The header's `state` is one of **EMPTY** (never used), **OCCUPIED** (holds a live
key+value), or **TOMBSTONE** (a removed entry). `payload_offset` and
`bucket_stride` are computed once at create time from `elem_size`/`align`, so
addressing bucket `i`'s value is just `internal.ptr + i*bucket_stride + payload_offset`.

- **Hashing.** Integer keys go through a SplitMix/Murmur-style bit-mix finalizer
  (so sequential ids spread across buckets); U8 keys use FNV-1a over the bytes.
- **Probing.** Linear open addressing: the start bucket is `hash & (cap - 1)`
  (cheap because `cap` is a power of two), then the search walks forward one
  bucket at a time, wrapping around, until it finds the key (OCCUPIED with an
  equal key) or an EMPTY bucket (which proves the key is absent). Linear probing
  keeps the walked buckets contiguous in memory.
- **Removal and tombstones.** `proven_map_remove` cannot just blank a bucket —
  that would cut a probe chain and hide later keys. It marks the bucket TOMBSTONE
  instead. Tombstones are skipped by lookups but still consume a slot, which is
  why `used` (live + tombstones) — not `len` — drives growth.
- **Load factor and resize.** When `used >= cap * 3/4`, the map allocates a new
  bucket array of the next power of two and **rehashes** every OCCUPIED entry into
  it, dropping all tombstones in the process. Capacity only grows; it never shrinks.
  Reserve ahead with `proven_map_reserve` (especially with arena allocators, to
  avoid leaving dead arrays behind).

### Key modes — choosing one

- `PROVEN_KEY_TYPE_INT`: keys are `proven_size_t`. No key storage.
- `PROVEN_KEY_TYPE_U8_BORROWED`: the bucket stores the *view* (pointer + length).
  **The map never copies the bytes**, so the caller must keep the exact bytes
  alive and unmoved for the whole time the entry exists. Cheapest for keys that
  already live somewhere stable (string literals, interned strings).
- `PROVEN_KEY_TYPE_U8_OWNED`: on insert (`proven_map_set_u8_owned` /
  `PROVEN_MAP_SET_U8_OWNED`) the map **duplicates** the key bytes into its own
  storage and frees them on remove/destroy, so the source buffer may be released
  or reused immediately after the call.

### The `set_with_scratch` / alias case

If the `element` you pass to `proven_map_set` points *into the map's own bucket
array* (for example, copying a value from one key to another via a pointer
returned by `proven_map_get`), a rehash triggered by that same insert would move
or free the bytes you are reading. `proven_map_set_with_scratch` (and the
`*_WITH_SCRATCH_*` macros) capture the source bytes into a temporary buffer from
the scratch allocator first, so the insert is alias-safe. Persistent storage
still uses `map->alloc`; the scratch allocator is only for that transient copy.

### Functions

| API | Intent | Return |
|---|---|---|
| `proven_map_create(alloc, init_cap, key_type, elem_size, align)` | Create a map. | `proven_result_map_t`. |
| `proven_map_is_valid(map)` | Validate map invariants. | `bool`. |
| `proven_map_reserve(map, new_cap)` | Ensure capacity. | `proven_err_t`. |
| `proven_map_set_with_scratch(map, key, element, scratch)` | Insert/update using scratch for temporary alias-safe copies. | `proven_err_t`. |
| `proven_map_set(map, key, element)` | Insert/update. | `proven_err_t`. |
| `proven_map_set_u8_owned(map, key, element)` | Insert/update with map-owned U8 key storage. | `proven_err_t`. |
| `proven_map_get_mut(map, key)` | Lookup mutable value. | pointer or null. |
| `proven_map_get(map, key)` | Lookup const value. | pointer or null. |
| `proven_map_remove(map, key)` | Remove key if present. | `proven_err_t`. |
| `proven_map_destroy(map)` | Free map storage. | void. |

### Macros

| Macro | Intent |
|---|---|
| `proven_map_create_with_capacity(...)` | Alias emphasizing capacity allocation. |
| `PROVEN_MAP_INIT_INT(alloc, type, init_cap)` | Create integer-key typed map. |
| `PROVEN_MAP_INIT_U8_BORROWED(alloc, type, init_cap)` | Create borrowed-string-key typed map. |
| `PROVEN_MAP_INIT_U8_OWNED(alloc, type, init_cap)` | Create owned-string-key typed map. |
| `PROVEN_MAP_SET_INT(map_ptr, int_key, type, value)` | Set integer key. |
| `PROVEN_MAP_SET_WITH_SCRATCH_INT(map_ptr, int_key, type, value, scratch)` | Set integer key using scratch allocator. |
| `PROVEN_MAP_SET_U8_BORROWED(map_ptr, u8_view, type, value)` | Set borrowed U8 key. |
| `PROVEN_MAP_SET_U8_OWNED(map_ptr, u8_view, type, value)` | Set owned U8 key. |
| `PROVEN_MAP_SET_WITH_SCRATCH_U8_BORROWED(map_ptr, u8_view, type, value, scratch)` | Set borrowed U8 key using scratch allocator. |
| `PROVEN_MAP_GET_INT(map_ptr, type, int_key)` | Get const value by integer key. |
| `PROVEN_MAP_GET_U8_BORROWED(map_ptr, type, u8_view)` | Get const value by borrowed U8 key. |
| `PROVEN_MAP_GET_U8_OWNED(map_ptr, type, u8_view)` | Get const value by owned U8 key. |
| `PROVEN_MAP_GET_MUT_INT(map_ptr, type, int_key)` | Get mutable value by integer key. |
| `PROVEN_MAP_GET_MUT_U8_BORROWED(map_ptr, type, u8_view)` | Get mutable value by borrowed U8 key. |
| `PROVEN_MAP_GET_MUT_U8_OWNED(map_ptr, type, u8_view)` | Get mutable value by owned U8 key. |
| `PROVEN_MAP_REMOVE_INT(map_ptr, int_key)` | Remove integer key. |
| `PROVEN_MAP_REMOVE_U8_BORROWED(map_ptr, u8_view)` | Remove U8 key. |
| `PROVEN_MAP_REMOVE_U8_OWNED(map_ptr, u8_view)` | Remove owned U8 key. |
| `PROVEN_MAP_DESTROY(map_ptr)` | Destroy map. |

Owned key path:

`PROVEN_KEY_TYPE_U8_OWNED` stores a duplicate of the key bytes inside the map. Use it when the source buffer may move or be freed after insertion. The owned bytes are released on remove and destroy, and they move with the entry during rehash.

`PROVEN_HARDENED` and debug validation can reject some borrowed-key pointers that fall inside the map's own internal storage. That check is defensive only; it does not make borrowed keys self-owning.

Example:

```c
typedef struct UserInfo {
    int level;
    double budget;
} UserInfo;

proven_result_map_t r = PROVEN_MAP_INIT_INT(alloc, UserInfo, 8);
if (!proven_is_ok(r.err)) {
    return;
}
proven_map_t users = r.value;

UserInfo u = { .level = 3, .budget = 99.0 };
(void)PROVEN_MAP_SET_INT(&users, 404, UserInfo, u);

/* GET returns a pointer into the bucket array, or NULL when absent. Any insert
 * that rehashes invalidates it: look it up, use it, drop it. */
const UserInfo *found = PROVEN_MAP_GET_INT(&users, UserInfo, 404);
if (found) {
    proven_println("level={}", PROVEN_ARG(found->level));
}

PROVEN_MAP_DESTROY(&users);
```

## 5. Algorithms

Array algorithms operate on `proven_array_t` and a comparison callback.

### Comparison function

```text
typedef int (*proven_compare_fn_t)(const void *a, const void *b);
```

Return negative if `a < b`, zero if equal, positive if `a > b`.

### Functions

| API | Intent | Return |
|---|---|---|
| `proven_array_sort(arr, cmp)` | Sort array in place. | void. |
| `proven_array_binary_search(arr, key, cmp)` | Search sorted array. | pointer to element or null. |
| `proven_array_linear_search(arr, key, cmp)` | Search any array by scanning. | pointer to element or null. |

`proven_array_sort` is an introsort: a Bentley-McIlroy three-way partition, an
insertion-sort cutoff for small ranges, and a heapsort fallback once the
recursion exceeds `2*log2(n)` levels.

Two properties are worth stating, because they are the ones that bite:

- **O(n log n) is a guarantee, not a typical case.** The heapsort fallback is
  what makes it one. A sort whose worst case can be reached by an adversarial
  ordering is a denial of service in any program that sorts data it did not
  author.
- **Duplicate keys are the fast case, not the slow one.** Elements equal to the
  pivot are collected into a run that is final and never recursed into, so
  all-equal input costs a single pass. This matters because low-cardinality keys
  - a status column, an enum, a bucket id - are what callers actually sort by,
  and they are exactly what a naive two-way partition degrades on.

The sort is not stable: equal elements may be reordered.

The comparator is an ordinary file-scope function, so the shape is:

```text
static int cmp_int(const void *a, const void *b) {
    int x = *(const int *)a;
    int y = *(const int *)b;
    return (x > y) - (x < y);   /* not (x - y): that overflows */
}

proven_array_sort(&nums, cmp_int);
int key = 20;
int *hit = proven_array_binary_search(&nums, &key, cmp_int);
```

The worked example at the end of this chapter (`manual/examples/ex_04_array.c`)
is the compiled-and-run version of exactly this.

## 6. Hashing, by use case

There is no single "hash". Which function is correct depends entirely on what you are
doing with the result, and reaching for the wrong one gives you either a program that is
needlessly slow or one that is quietly insecure. `proven/hash.h` offers exactly one
primitive per job so the choice is made once you name the job:

| You are... | Use | And crucially |
|---|---|---|
| hashing keys into **your own** table, **trusted** input | `proven_hash_bytes` (FNV-1a) | fast; a crypto hash here is ~50× slower for no gain |
| hashing keys from **untrusted** input into a table | `proven_hash_keyed` (SipHash-2-4) | FNV lets an attacker collide every key into one bucket and turn your O(1) table into O(n²) |
| checking data was not **corrupted** in transit or on disk | `proven_crc32` | a checksum; interoperates with gzip/zlib/PNG |
| **fingerprinting** content: dedup, content-addressing, "same file?" | `proven_sha256` | the only one safe against a *deliberately* forged match |

The one line to remember: **CRC-32 and FNV detect accident, not attack.** Do not use them
to decide whether two things are "the same" when someone might benefit from fooling you —
that is what `proven_sha256` is for. And a keyed hash is only safe if the key is a real
secret chosen once from real randomness; a fixed key is no key at all.

Every function is byte-exact and endianness-independent: the same input gives the same
output on any target, because a fingerprint that changed with the machine would be a
fingerprint of the machine, not the content. All four are royalty-free algorithms (FNV,
CRC-32: public domain; SipHash: CC0; SHA-256: FIPS 180-4, unpatented), implemented from
their specifications and checked against each one's official known-answer vectors.

Compiled and run by the test suite:

<!-- example: manual/examples/ex_04_hash.c -->
```c
/*
 * Hashing, by use case. The module gives you exactly one function per job, so the only
 * decision is which job you have - and getting THAT wrong is the whole danger.
 */

int main(void) {
    proven_mem_view_t data = proven_mem_view_from_u8(PROVEN_LIT("the quick brown fox"));

    /* Job 1: hash a key into your own table, trusted input. Fast, non-cryptographic. */
    proven_u64 table_hash = proven_hash_bytes(data);
    EXAMPLE_REQUIRE(table_hash != 0, "FNV-1a produces a spread-out 64-bit value");

    /* Job 2: hash a key from UNTRUSTED input. Same purpose, but an attacker who picks the
     * input still cannot make everything collide, because they do not have the key. Pick
     * the key once at startup from real randomness; a fixed key defeats the point. */
    proven_byte_t key[16] = { 0 };   /* in real code: fill from a random source, once */
    proven_u64 safe_hash = proven_hash_keyed(data, key);
    EXAMPLE_REQUIRE(safe_hash != table_hash, "a keyed hash is a different function");

    /* Job 3: did these bytes get corrupted? A checksum, not a hash. Interoperates with
     * gzip/zlib/PNG, which all carry this exact CRC-32. */
    proven_u32 checksum = proven_crc32(data);
    /* The canonical CRC-32 sanity value, so you can see it is the real one: */
    EXAMPLE_REQUIRE(proven_crc32(proven_mem_view_from_u8(PROVEN_LIT("123456789"))) == 0xcbf43926u,
                    "CRC-32 of \"123456789\" is the shared check value");
    (void)checksum;

    /* Job 4: fingerprint content - dedup, content-addressing, "are these the same file",
     * answered safely even against someone trying to forge a match. This is the one you
     * reach for when the answer must not be foolable. */
    proven_byte_t digest[PROVEN_SHA256_SIZE];
    proven_sha256(data, digest);

    char hex[65];
    proven_sha256_to_hex(digest, hex);
    /* The same spelling sha256sum and git print, so it interoperates: */
    EXAMPLE_REQUIRE(hex[64] == '\0' && proven_cstr_len(hex) == 64,
                    "a SHA-256 fingerprint is 64 lowercase hex characters");

    /* SHA-256 streams, for content you cannot hold in memory at once - the digest depends
     * only on the bytes, never on how they were chunked. */
    proven_sha256_t ctx;
    proven_sha256_init(&ctx);
    proven_sha256_update(&ctx, proven_mem_view_from_u8(PROVEN_LIT("the quick ")));
    proven_sha256_update(&ctx, proven_mem_view_from_u8(PROVEN_LIT("brown fox")));
    proven_byte_t streamed[PROVEN_SHA256_SIZE];
    proven_sha256_final(&ctx, streamed);

    bool same = true;
    for (proven_size_t i = 0; i < PROVEN_SHA256_SIZE; ++i) {
        if (streamed[i] != digest[i]) same = false;
    }
    EXAMPLE_REQUIRE(same, "two updates of the halves equal one hash of the whole");

    return EXAMPLE_OK();
}
```

## 7. Examples and misuse cases

### Pointers into arrays can become stale

Wrong:

```text
int *p = PROVEN_ARRAY_GET_MUT(&nums, int, 0);
PROVEN_ARRAY_PUSH(&nums, int, 30);
*p = 99; /* wrong: push may have reallocated the array */
```

Correct:

```c
proven_result_array_t r = PROVEN_ARRAY_INIT(alloc, int, 2);
if (!proven_is_ok(r.err)) {
    return;
}
proven_array_t nums = r.value;
(void)PROVEN_ARRAY_PUSH(&nums, int, 10);

/* Push first, then take the pointer. A pointer fetched before a push may point
 * at a block the array has already reallocated away. */
(void)PROVEN_ARRAY_PUSH(&nums, int, 30);
int *p = PROVEN_ARRAY_GET_MUT(&nums, int, 0);
if (p) {
    *p = 99;
}

PROVEN_ARRAY_DESTROY(&nums);
```

### One intrusive node belongs to one list at a time

Wrong:

```text
proven_list_push_back(&list_a, &item.link);
proven_list_push_back(&list_b, &item.link); /* wrong */
```

Use one embedded node per membership.

### Remove while iterating with the safe macro

Correct:

```c
typedef struct Item {
    int value;
    proven_list_node_t link;
} Item;

proven_list_t list;
proven_list_init(&list);
Item a = { .value = 1 };
Item b = { .value = 2 };
proven_list_push_back(&list, &a.link);
proven_list_push_back(&list, &b.link);

/* The SAFE form reads `it->next` into `next` before the body runs, so removing
 * `it` (which nulls its links) cannot strand the loop. */
proven_list_node_t *it = NULL;
proven_list_node_t *next = NULL;
PROVEN_LIST_FOR_EACH_SAFE(it, next, &list) {
    Item *item = PROVEN_LIST_ENTRY(it, Item, link);
    if (item->value % 2 == 0) {
        proven_list_remove(it);
    }
}
```

### Ring push does not grow

Wrong:

```text
PROVEN_RING_PUSH(&q, int, value); /* wrong if you ignore full-ring errors */
```

Correct:

```c
proven_result_ring_t r = PROVEN_RING_INIT(alloc, int, 2);
if (!proven_is_ok(r.err)) {
    return;
}
proven_ring_t q = r.value;

int value = 7;
for (int i = 0; i < 3; ++i) {
    proven_err_t e = PROVEN_RING_PUSH(&q, int, value);
    if (e == PROVEN_ERR_OUT_OF_BOUNDS) {
        /* The ring is full. Drop the item, or drain one first - but decide. */
        int drop = 0;
        (void)PROVEN_RING_POP(&q, int, &drop);
        e = PROVEN_RING_PUSH(&q, int, value);
    }
    (void)e;
}

PROVEN_RING_DESTROY(&q);
```

### Borrowed map keys must outlive entries

Wrong:

```text
proven_u8str_t key = make_key(alloc);
PROVEN_MAP_SET_U8_BORROWED(&m, proven_u8str_as_view(&key), int, 1);
proven_u8str_destroy(alloc, &key);
/* wrong: map still points at freed key bytes */
```

Correct options:

- Use string literals or other stable storage for borrowed keys.
- Store owned key objects elsewhere and destroy them after removing map entries.
- Use `PROVEN_KEY_TYPE_U8_OWNED` and `proven_map_set_u8_owned()` when the map should own key storage.

### Use scratch when inserting a value borrowed from the same map

If `element` points into map storage and insertion can rehash, use `proven_map_set_with_scratch()` or the scratch macros.

```c
proven_result_map_t r = PROVEN_MAP_INIT_INT(alloc, int, 4);
if (!proven_is_ok(r.err)) {
    return;
}
proven_map_t m = r.value;
(void)PROVEN_MAP_SET_INT(&m, 1, int, 42);

/* `src` points into the map's own bucket array. Passing it straight to
 * proven_map_set would be an alias: a rehash triggered by that same insert can
 * free the bytes it is still reading from. The scratch form captures the source
 * bytes into a temporary from `scratch` first, so the insert is alias-safe. */
const int *src = PROVEN_MAP_GET_INT(&m, int, 1);
if (src) {
    (void)proven_map_set_with_scratch(&m, (proven_map_key_t){ .id = 2 }, src, scratch);
}

PROVEN_MAP_DESTROY(&m);
```

### Binary search requires sorted input

Wrong:

```text
int *hit = proven_array_binary_search(&nums, &key, cmp_int);
/* wrong if nums was not sorted by cmp_int */
```

### Worked example: array, sort, binary search

Compiled and run by the test suite. Note the sort's behaviour on duplicate keys: they are the *fast* case, not the quadratic one.

<!-- example: manual/examples/ex_04_array.c -->
```c
/*
 * proven_array_t is a growable vector of one element type. It stores the
 * allocator you created it with, so push can grow and destroy can free without
 * you passing the allocator back in - which also means the array must be
 * destroyed with nothing but itself.
 *
 * The PROVEN_ARRAY_* macros are the type-safe face of the void* core: they
 * derive sizeof/alignof from the type and hand the element to the array by
 * pointer, so pushing an int into an array of records will not compile.
 */

/* A task queue keyed by priority. Real data is low-cardinality: three
 * priorities, many tasks. That matters for the sort - see below. */
typedef struct {
    int priority;
    int id;
} task_t;

/* The comparator is the array's ordering. The same one MUST be used for the
 * sort and for the binary search - a search under a different order is a bug
 * the compiler cannot see.
 *
 * The (x > y) - (x < y) form is deliberate: subtracting the two ints would
 * overflow for large values and hand back a nonsense sign. */
static int cmp_priority(const void *a, const void *b) {
    int x = ((const task_t *)a)->priority;
    int y = ((const task_t *)b)->priority;
    return (x > y) - (x < y);
}

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    /* --- push / get / pop --------------------------------------------------- */
    /* init_cap is a hint, not a limit: push grows past it. Sizing it right just
     * avoids the reallocations. */
    proven_result_array_t r = PROVEN_ARRAY_INIT(alloc, task_t, 4);
    EXAMPLE_REQUIRE(proven_is_ok(r.err), "creating an array of task_t must succeed");
    if (!proven_is_ok(r.err)) {
        return 1;
    }
    proven_array_t tasks = r.value;

    /* Deliberately duplicate-heavy, because that is what real keys look like:
     * a status column, an enum, a bucket id. */
    static const task_t seed[] = {
        { .priority = 2, .id = 10 }, { .priority = 0, .id = 11 },
        { .priority = 2, .id = 12 }, { .priority = 1, .id = 13 },
        { .priority = 2, .id = 14 }, { .priority = 0, .id = 15 },
        { .priority = 1, .id = 16 }, { .priority = 2, .id = 17 },
    };

    for (proven_size_t i = 0; i < sizeof seed / sizeof seed[0]; ++i) {
        proven_err_t err = PROVEN_ARRAY_PUSH(&tasks, task_t, seed[i]);
        EXAMPLE_REQUIRE(proven_is_ok(err), "pushing into a growable array must succeed");
        if (!proven_is_ok(err)) {
            PROVEN_ARRAY_DESTROY(&tasks);
            return 1;
        }
    }
    EXAMPLE_REQUIRE(tasks.len == 8, "eight pushes give eight elements");

    /* get returns a pointer INTO the array's storage - it is not a copy, and the
     * next push may reallocate and leave it dangling. Fetch it after the pushes,
     * use it, do not store it. */
    const task_t *front = PROVEN_ARRAY_GET(&tasks, task_t, 0);
    EXAMPLE_REQUIRE(front && front->id == 10, "element 0 is the first one pushed");

    /* Out of range is a null pointer, not a crash and not UB. */
    EXAMPLE_REQUIRE(PROVEN_ARRAY_GET(&tasks, task_t, 99) == NULL, "an out-of-range index yields NULL");

    /* pop copies the last element out (pass NULL to just discard it). */
    task_t last = {0};
    proven_err_t err = PROVEN_ARRAY_POP(&tasks, task_t, &last);
    EXAMPLE_REQUIRE(proven_is_ok(err), "popping a non-empty array must succeed");
    EXAMPLE_REQUIRE(last.id == 17 && tasks.len == 7, "pop returns the last element and shrinks the array");

    /* Put it back: the rest of the example wants all eight. */
    err = PROVEN_ARRAY_PUSH(&tasks, task_t, last);
    EXAMPLE_REQUIRE(proven_is_ok(err), "pushing the popped element back must succeed");

    /* --- sort --------------------------------------------------------------- */
    /* An introsort: O(n log n) is a guarantee, not an average - the heapsort
     * fallback rules out the quadratic case an adversary could otherwise steer
     * you into. And equal keys are the FAST path here: everything equal to the
     * pivot is partitioned into a run that is never recursed into, so the
     * duplicate priorities above cost less than distinct ones would, not more.
     *
     * It is not stable: task 10 and task 12 may come out in either order. */
    proven_array_sort(&tasks, cmp_priority);

    for (proven_size_t i = 1; i < tasks.len; ++i) {
        const task_t *prev = PROVEN_ARRAY_GET(&tasks, task_t, i - 1);
        const task_t *cur  = PROVEN_ARRAY_GET(&tasks, task_t, i);
        EXAMPLE_REQUIRE(prev && cur && prev->priority <= cur->priority,
                        "after sorting, priorities must be non-decreasing");
    }

    /* --- binary search ------------------------------------------------------ */
    /* Only legal because the array is sorted by this exact comparator. The key
     * is a whole element, but only the fields the comparator reads matter. */
    task_t key = { .priority = 1, .id = 0 };
    const task_t *hit = (const task_t *)proven_array_binary_search(&tasks, &key, cmp_priority);
    EXAMPLE_REQUIRE(hit != NULL, "priority 1 is present, so the search must find it");
    EXAMPLE_REQUIRE(hit && hit->priority == 1, "the hit must be an element with the searched key");
    /* With duplicate keys it finds SOME element with that key, not the first one.
     * If you need the first, scan backwards from the hit. */

    task_t absent = { .priority = 9, .id = 0 };
    EXAMPLE_REQUIRE(proven_array_binary_search(&tasks, &absent, cmp_priority) == NULL,
                    "a key that is not there must return NULL");

    printf("tasks: %zu sorted, found priority %d (id %d)\n",
           (size_t)tasks.len, hit ? hit->priority : -1, hit ? hit->id : -1);

    /* The array owns its element storage; destroy returns it through the
     * allocator the array was created with. Elements are plain data here - if
     * they owned anything, you would have to destroy each one first. */
    PROVEN_ARRAY_DESTROY(&tasks);
    return EXAMPLE_OK();
}
```

### Worked example: a map with integer keys and with owned string keys

Compiled and run by the test suite. The owned-key half proves the point that is easy to get wrong: the map copies the key, so the buffer you built it in is yours to reuse immediately.

<!-- example: manual/examples/ex_04_map.c -->
```c
/*
 * proven_map_t is a flat open-addressing hash map. The value type is fixed at
 * create time and stored inline in the bucket array - there is no per-entry
 * allocation for values, and get hands back a pointer straight into that array.
 *
 * The interesting decision is the KEY:
 *
 *   PROVEN_KEY_TYPE_INT          - the key is a proven_size_t. Nothing to own.
 *   PROVEN_KEY_TYPE_U8_BORROWED  - the bucket stores your pointer and length.
 *                                  The map never copies the bytes, so YOU must
 *                                  keep them alive and unmoved for as long as
 *                                  the entry exists. Right for string literals.
 *   PROVEN_KEY_TYPE_U8_OWNED     - the map copies the key bytes into its own
 *                                  storage and frees them again. Right for keys
 *                                  built at runtime, which is most of them.
 *
 * The second half of this example is the reason OWNED exists.
 */

typedef struct {
    int  level;
    long score;
} player_t;

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    /* --- integer keys ------------------------------------------------------- */
    proven_result_map_t r = PROVEN_MAP_INIT_INT(alloc, player_t, 8);
    EXAMPLE_REQUIRE(proven_is_ok(r.err), "creating an int-keyed map must succeed");
    if (!proven_is_ok(r.err)) {
        return 1;
    }
    proven_map_t by_id = r.value;

    proven_err_t err = PROVEN_MAP_SET_INT(&by_id, 404, player_t, ((player_t){ .level = 3, .score = 990 }));
    EXAMPLE_REQUIRE(proven_is_ok(err), "inserting into the map must succeed");
    err = PROVEN_MAP_SET_INT(&by_id, 7, player_t, ((player_t){ .level = 1, .score = 10 }));
    EXAMPLE_REQUIRE(proven_is_ok(err), "inserting a second key must succeed");

    /* set on an existing key replaces the value; it does not add an entry. */
    err = PROVEN_MAP_SET_INT(&by_id, 7, player_t, ((player_t){ .level = 2, .score = 40 }));
    EXAMPLE_REQUIRE(proven_is_ok(err), "re-setting an existing key must succeed");
    EXAMPLE_REQUIRE(by_id.len == 2, "re-setting a key replaces its value rather than adding an entry");

    /* get returns a pointer into the bucket array, or NULL when absent. It is
     * invalidated by any insert that rehashes - look it up, use it, drop it. */
    const player_t *p = PROVEN_MAP_GET_INT(&by_id, player_t, 7);
    EXAMPLE_REQUIRE(p && p->level == 2 && p->score == 40, "get must see the replaced value");
    EXAMPLE_REQUIRE(PROVEN_MAP_GET_INT(&by_id, player_t, 999) == NULL, "a missing key yields NULL");

    err = PROVEN_MAP_REMOVE_INT(&by_id, 7);
    EXAMPLE_REQUIRE(proven_is_ok(err), "removing a present key must succeed");
    EXAMPLE_REQUIRE(PROVEN_MAP_GET_INT(&by_id, player_t, 7) == NULL, "a removed key is gone");
    EXAMPLE_REQUIRE(by_id.len == 1, "removal decrements the live entry count");

    PROVEN_MAP_DESTROY(&by_id);

    /* --- owned string keys --------------------------------------------------- */
    /* Same map, keyed by a name that we build at runtime - the case where a
     * borrowed key would be a dangling pointer waiting to happen. */
    proven_result_map_t rm = PROVEN_MAP_INIT_U8_OWNED(alloc, player_t, 8);
    EXAMPLE_REQUIRE(proven_is_ok(rm.err), "creating an owned-string-keyed map must succeed");
    if (!proven_is_ok(rm.err)) {
        return 1;
    }
    proven_map_t by_name = rm.value;

    /* A scratch buffer we intend to reuse for every key. With a BORROWED map
     * that plan is fatal: every entry would point at these same bytes. */
    proven_byte_t scratch[32];
    proven_u8str_t name = proven_u8str_borrow(scratch, sizeof scratch);

    err = proven_u8str_append(&name, PROVEN_LIT("ada"));
    EXAMPLE_REQUIRE(proven_is_ok(err), "building the first key must succeed");

    /* set_u8_owned COPIES the key bytes into map storage. After it returns, the
     * map's key no longer has anything to do with `scratch`. */
    err = PROVEN_MAP_SET_U8_OWNED(&by_name, proven_u8str_as_view(&name), player_t,
                                  ((player_t){ .level = 9, .score = 5000 }));
    EXAMPLE_REQUIRE(proven_is_ok(err), "inserting with an owned key must succeed");

    /* So the buffer is immediately free to be reused for the next key... */
    err = proven_u8str_reset(&name);
    EXAMPLE_REQUIRE(proven_is_ok(err), "the key buffer is ours again the moment set returns");
    err = proven_u8str_append(&name, PROVEN_LIT("grace"));
    EXAMPLE_REQUIRE(proven_is_ok(err), "overwriting the buffer with the next key must succeed");

    err = PROVEN_MAP_SET_U8_OWNED(&by_name, proven_u8str_as_view(&name), player_t,
                                  ((player_t){ .level = 4, .score = 700 }));
    EXAMPLE_REQUIRE(proven_is_ok(err), "inserting the second owned key must succeed");

    /* ...and the first entry is untouched by that overwrite. This is the whole
     * point: the map holds its own copy of "ada", not a view of a buffer that
     * now says "grace". A BORROWED map would report two entries both keyed
     * "grace" - or worse, one keyed by freed memory. */
    const player_t *ada = PROVEN_MAP_GET_U8_OWNED(&by_name, player_t, PROVEN_LIT("ada"));
    EXAMPLE_REQUIRE(ada && ada->score == 5000, "the copied key survives the caller reusing its buffer");

    const player_t *grace = PROVEN_MAP_GET_U8_OWNED(&by_name, player_t, PROVEN_LIT("grace"));
    EXAMPLE_REQUIRE(grace && grace->score == 700, "the second key is a separate entry");
    EXAMPLE_REQUIRE(by_name.len == 2, "two distinct keys means two entries");

    /* Remove frees the key copy the map made - you never free it yourself. */
    err = PROVEN_MAP_REMOVE_U8_OWNED(&by_name, PROVEN_LIT("ada"));
    EXAMPLE_REQUIRE(proven_is_ok(err), "removing an owned key must succeed");
    EXAMPLE_REQUIRE(PROVEN_MAP_GET_U8_OWNED(&by_name, player_t, PROVEN_LIT("ada")) == NULL,
                    "the removed entry is gone");

    printf("map: %zu name(s) left, grace at level %d\n",
           (size_t)by_name.len, grace ? grace->level : -1);

    /* destroy frees the bucket array AND every key copy still in it ("grace"
     * here). `scratch` is ours and outlives the map; the borrowed `name` handle
     * has nothing to free. */
    PROVEN_MAP_DESTROY(&by_name);
    proven_u8str_destroy(alloc, &name);
    return EXAMPLE_OK();
}
```
