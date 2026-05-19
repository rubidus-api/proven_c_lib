# Chapter 4: Containers and Algorithms

This chapter covers `array.h`, `list.h`, `ring.h`, `map.h`, and `algorithm.h`.

## Table of contents

1. [Dynamic array](#1-dynamic-array)
2. [Intrusive list](#2-intrusive-list)
3. [Ring buffer](#3-ring-buffer)
4. [Hash map](#4-hash-map)
5. [Algorithms](#5-algorithms)
6. [Examples and misuse cases](#6-examples-and-misuse-cases)

## 1. Dynamic array

`proven_array_t` is a generic growable vector. It stores its allocator internally and owns contiguous element storage.

### Structures

```c
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
if (!proven_is_ok(r.err)) return r.err;
proven_array_t nums = r.value;

PROVEN_ARRAY_PUSH(&nums, int, 10);
PROVEN_ARRAY_PUSH(&nums, int, 20);

const int *first = PROVEN_ARRAY_GET(&nums, int, 0);
if (first) use_int(*first);

PROVEN_ARRAY_DESTROY(&nums);
```

## 2. Intrusive list

`proven_list_t` is an intrusive doubly-linked circular list. It allocates no nodes. Each user object embeds a `proven_list_node_t`.

### Structures

```c
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
    proven_list_node_t link;
} Item;

proven_list_t list;
proven_list_init(&list);

Item a = { .value = 1 };
Item b = { .value = 2 };
proven_list_push_back(&list, &a.link);
proven_list_push_back(&list, &b.link);

proven_list_node_t *it = NULL;
PROVEN_LIST_FOR_EACH(it, &list) {
    Item *item = PROVEN_LIST_ENTRY(it, Item, link);
    use_item(item);
}
```

## 3. Ring buffer

`proven_ring_t` is a fixed-capacity FIFO. Push fails when full. It stores its allocator internally.

### Structures

```c
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
if (!proven_is_ok(r.err)) return r.err;
proven_ring_t q = r.value;

proven_err_t e = PROVEN_RING_PUSH(&q, int, 7);
if (!proven_is_ok(e)) {
    PROVEN_RING_DESTROY(&q);
    return e;
}

int out = 0;
PROVEN_RING_POP(&q, int, &out);
PROVEN_RING_DESTROY(&q);
```

## 4. Hash map

`proven_map_t` is an open-addressing hash map with tombstones. It supports integer keys and borrowed or owned U8 string keys. It stores its allocator internally.

### Structures

```c
typedef enum {
    PROVEN_KEY_TYPE_INT,
    PROVEN_KEY_TYPE_U8_BORROWED
} proven_key_type_t;

typedef union {
    proven_size_t id;
    proven_u8str_view_t str;
} proven_map_key_t;

typedef struct {
    proven_allocator_t alloc;
    proven_mem_mut_t internal;
    proven_size_t len;
    proven_size_t used;
    proven_size_t cap;
    proven_size_t elem_size;
    proven_size_t align;
    proven_size_t bucket_stride;
    proven_size_t payload_offset;
    proven_key_type_t key_type;
} proven_map_t;

typedef struct {
    proven_err_t err;
    proven_map_t value;
} proven_result_map_t;
```

Field notes:

- `len`: live entries.
- `used`: live entries plus tombstones.
- `cap`: bucket capacity, maintained as a power of two.
- `key_type`: key mode selected at creation.
- `PROVEN_KEY_TYPE_U8_BORROWED` does not copy key bytes.
- `PROVEN_KEY_TYPE_U8_OWNED` duplicates key bytes into map-owned storage and frees them on remove or destroy.

### Functions

| API | Intent | Return |
|---|---|---|
| `proven_map_create(alloc, init_cap, key_type, elem_size, align)` | Create a map. | `proven_result_map_t`. |
| `proven_map_is_valid(map)` | Validate map invariants. | `bool`. |
| `proven_map_reserve(map, new_cap)` | Ensure capacity. | `proven_err_t`. |
| `proven_map_set_with_scratch(map, key, element, scratch)` | Insert/update using scratch for temporary alias-safe copies. | `proven_err_t`. |
| `proven_map_set(map, key, element)` | Insert/update. | `proven_err_t`. |
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
| `PROVEN_MAP_SET_INT(map_ptr, int_key, type, value)` | Set integer key. |
| `PROVEN_MAP_SET_WITH_SCRATCH_INT(map_ptr, int_key, type, value, scratch)` | Set integer key using scratch allocator. |
| `PROVEN_MAP_SET_U8_BORROWED(map_ptr, u8_view, type, value)` | Set borrowed U8 key. |
| `PROVEN_MAP_SET_WITH_SCRATCH_U8_BORROWED(map_ptr, u8_view, type, value, scratch)` | Set borrowed U8 key using scratch allocator. |
| `PROVEN_MAP_GET_INT(map_ptr, type, int_key)` | Get const value by integer key. |
| `PROVEN_MAP_GET_U8_BORROWED(map_ptr, type, u8_view)` | Get const value by borrowed U8 key. |
| `PROVEN_MAP_GET_MUT_INT(map_ptr, type, int_key)` | Get mutable value by integer key. |
| `PROVEN_MAP_GET_MUT_U8_BORROWED(map_ptr, type, u8_view)` | Get mutable value by borrowed U8 key. |
| `PROVEN_MAP_REMOVE_INT(map_ptr, int_key)` | Remove integer key. |
| `PROVEN_MAP_REMOVE_U8_BORROWED(map_ptr, u8_view)` | Remove U8 key. |
| `PROVEN_MAP_DESTROY(map_ptr)` | Destroy map. |

Example:

```c
typedef struct UserInfo {
    int level;
    double budget;
} UserInfo;

proven_result_map_t r = PROVEN_MAP_INIT_INT(alloc, UserInfo, 8);
if (!proven_is_ok(r.err)) return r.err;
proven_map_t users = r.value;

UserInfo u = { .level = 3, .budget = 99.0 };
PROVEN_MAP_SET_INT(&users, 404, UserInfo, u);

const UserInfo *found = PROVEN_MAP_GET_INT(&users, UserInfo, 404);
if (found) use_user(found);

PROVEN_MAP_DESTROY(&users);
```

## 5. Algorithms

Array algorithms operate on `proven_array_t` and a comparison callback.

### Comparison function

```c
typedef int (*proven_compare_fn_t)(const void *a, const void *b);
```

Return negative if `a < b`, zero if equal, positive if `a > b`.

### Functions

| API | Intent | Return |
|---|---|---|
| `proven_array_sort(arr, cmp)` | Sort array in place. | void. |
| `proven_array_binary_search(arr, key, cmp)` | Search sorted array. | pointer to element or null. |
| `proven_array_linear_search(arr, key, cmp)` | Search any array by scanning. | pointer to element or null. |

Example:

```c
static int cmp_int(const void *a, const void *b) {
    int x = *(const int *)a;
    int y = *(const int *)b;
    return (x > y) - (x < y);
}

proven_array_sort(&nums, cmp_int);
int key = 20;
int *hit = proven_array_binary_search(&nums, &key, cmp_int);
```

## 6. Examples and misuse cases

### Pointers into arrays can become stale

Wrong:

```c
int *p = PROVEN_ARRAY_GET_MUT(&nums, int, 0);
PROVEN_ARRAY_PUSH(&nums, int, 30);
*p = 99; /* wrong: push may have reallocated the array */
```

Correct:

```c
PROVEN_ARRAY_PUSH(&nums, int, 30);
int *p = PROVEN_ARRAY_GET_MUT(&nums, int, 0);
if (p) *p = 99;
```

### One intrusive node belongs to one list at a time

Wrong:

```c
proven_list_push_back(&list_a, &item.link);
proven_list_push_back(&list_b, &item.link); /* wrong */
```

Use one embedded node per membership.

### Remove while iterating with the safe macro

Correct:

```c
proven_list_node_t *it = NULL;
proven_list_node_t *next = NULL;
PROVEN_LIST_FOR_EACH_SAFE(it, next, &list) {
    Item *item = PROVEN_LIST_ENTRY(it, Item, link);
    if (should_remove(item)) {
        proven_list_remove(it);
    }
}
```

### Ring push does not grow

Wrong:

```c
PROVEN_RING_PUSH(&q, int, value); /* wrong if you ignore full-ring errors */
```

Correct:

```c
proven_err_t e = PROVEN_RING_PUSH(&q, int, value);
if (e == PROVEN_ERR_OUT_OF_BOUNDS) {
    handle_full_queue();
}
```

### Borrowed map keys must outlive entries

Wrong:

```c
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

### Binary search requires sorted input

Wrong:

```c
int *hit = proven_array_binary_search(&nums, &key, cmp_int);
/* wrong if nums was not sorted by cmp_int */
```
