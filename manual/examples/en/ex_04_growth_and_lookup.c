#include "example.h"

/*
 * The container chapters show each structure on its own. A real program uses
 * several at once, and the questions that come up then are not about any single
 * container:
 *
 *   - How do I stop a container reallocating while it fills?  reserve.
 *   - How do I check a container somebody handed me is usable?  is_valid.
 *   - How do I search when the data is NOT sorted?  linear search - and knowing
 *     why binary search would give a wrong answer here.
 *   - How do I update a value already in a map without looking it up twice?
 *     get_mut.
 *   - Do I need the attack-resistant hash, or the fast one?  it depends on who
 *     chooses the keys, and map_hash lets you see the difference.
 *   - How do I checksum data that arrives in pieces?  crc32_update.
 *
 * The program is a small event intake: events arrive, are counted per client,
 * the most recent few are kept for a diagnostic dump, and the batch is
 * checksummed as it streams past.
 */

typedef struct {
    proven_u32 client;
    proven_u32 code;      /* an event code; 0 means "connection closed" */
} event_t;

/* Search by client id: this is the comparison for finding an event, not for
 * sorting them. The array below is in ARRIVAL order and stays that way. */
static int by_client(const void *a, const void *b) {
    const event_t *x = (const event_t *)a;
    const event_t *y = (const event_t *)b;
    return (x->client > y->client) - (x->client < y->client);
}

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    /* --- 1. reserve: decide the capacity once ----------------------------- */

    proven_result_array_t ar = PROVEN_ARRAY_INIT(alloc, event_t, 4);
    EXAMPLE_REQUIRE(proven_is_ok(ar.err), "creating the event log must succeed");
    if (!proven_is_ok(ar.err)) {
        return 1;
    }
    proven_array_t events = ar.value;

    /* We know the batch size before we start, so ask for the room once. Without
     * this the array doubles as it fills, copying its contents each time; behind
     * an arena allocator each of those copies also leaves the old block behind
     * until the arena is reset. */
    proven_err_t err = proven_array_reserve(&events, 16);
    EXAMPLE_REQUIRE(proven_is_ok(err), "reserving room for the whole batch must succeed");
    EXAMPLE_REQUIRE(events.cap >= 16, "the capacity is now at least what was asked for");

    /* is_valid asks whether the handle itself is structurally sound - a pointer,
     * a length and a capacity that agree. Assert it where a container arrives
     * from other code, or after a zero-initialised handle might have escaped;
     * it is not something to repeat after every push. */
    EXAMPLE_REQUIRE(proven_array_is_valid(&events), "a created array must be structurally valid");

    proven_array_t never_created = {0};
    EXAMPLE_REQUIRE(!proven_array_is_valid(&never_created),
                    "a zero-initialised handle is not a usable array");

    static const event_t batch[] = {
        { .client = 7, .code = 200 }, { .client = 3, .code = 200 },
        { .client = 7, .code = 404 }, { .client = 9, .code = 200 },
        { .client = 3, .code = 500 }, { .client = 7, .code = 0   },
    };
    proven_size_t batch_len = sizeof batch / sizeof batch[0];

    proven_size_t cap_before = events.cap;
    for (proven_size_t i = 0; i < batch_len; ++i) {
        err = PROVEN_ARRAY_PUSH(&events, event_t, batch[i]);
        EXAMPLE_REQUIRE(proven_is_ok(err), "pushing an event must succeed");
    }
    EXAMPLE_REQUIRE(events.cap == cap_before, "the reserve was enough: nothing reallocated while filling");

    /* --- 2. searching data that is not sorted ----------------------------- */

    /* The log is in arrival order, which is the order we want to keep: it is the
     * thing being recorded. Binary search would be faster and WRONG here, since
     * it may only be used on a sorted range - on unsorted data it does not
     * return "not found", it returns nonsense. Linear search is the correct
     * tool, and O(n) over a batch this size is nothing. */
    event_t key = { .client = 9, .code = 0 };
    const event_t *found = (const event_t *)proven_array_linear_search(&events, &key, by_client);
    EXAMPLE_REQUIRE(found != NULL, "client 9 appears in the batch");
    EXAMPLE_REQUIRE(found->code == 200, "and linear search returns the FIRST match in order");

    event_t absent = { .client = 42, .code = 0 };
    EXAMPLE_REQUIRE(proven_array_linear_search(&events, &absent, by_client) == NULL,
                    "a client that never appeared is reported as not found");

    /* --- 3. a ring for the most recent events ----------------------------- */

    proven_result_ring_t rr = PROVEN_RING_INIT(alloc, event_t, 4);
    EXAMPLE_REQUIRE(proven_is_ok(rr.err), "creating the recent-events ring must succeed");
    proven_ring_t recent = rr.value;
    EXAMPLE_REQUIRE(proven_ring_is_valid(&recent), "a created ring must be structurally valid");

    proven_ring_t unset = {0};
    EXAMPLE_REQUIRE(!proven_ring_is_valid(&unset), "a zero-initialised ring handle is not usable");

    /* This ring refuses when full rather than overwriting, so "keep the most
     * recent four" means dropping the oldest ourselves before pushing. */
    for (proven_size_t i = 0; i < batch_len; ++i) {
        if (recent.len == recent.cap) {
            event_t dropped;
            err = PROVEN_RING_POP(&recent, event_t, &dropped);
            EXAMPLE_REQUIRE(proven_is_ok(err), "a full ring must yield its oldest element");
        }
        err = PROVEN_RING_PUSH(&recent, event_t, batch[i]);
        EXAMPLE_REQUIRE(proven_is_ok(err), "pushing after making room must succeed");
    }
    EXAMPLE_REQUIRE(recent.len == 4, "the ring holds the four most recent events");

    /* --- 4. counting per client, with one lookup per update --------------- */

    /* create_with_capacity is proven_map_create under a name that says why the
     * capacity argument is there: sizing it now avoids rehashing later, and a
     * rehash both copies every bucket and invalidates every pointer previously
     * returned by get_mut. */
    proven_result_map_t mr = proven_map_create_with_capacity(alloc, 8, PROVEN_KEY_TYPE_INT,
                                                            sizeof(proven_u32), alignof(proven_u32));
    EXAMPLE_REQUIRE(proven_is_ok(mr.err), "creating the counter map must succeed");
    proven_map_t counts = mr.value;
    EXAMPLE_REQUIRE(proven_map_is_valid(&counts), "a created map must be structurally valid");

    err = proven_map_reserve(&counts, 32);
    EXAMPLE_REQUIRE(proven_is_ok(err), "reserving map capacity must succeed");

    for (proven_size_t i = 0; i < batch_len; ++i) {
        proven_map_key_t k = { .id = batch[i].client };

        /* get_mut returns a pointer INTO the map's storage, so the counter is
         * incremented where it lives: one lookup, no copy back. The pointer is
         * good only until the next insert - which is another reason the capacity
         * was reserved above. */
        proven_u32 *seen = (proven_u32 *)proven_map_get_mut(&counts, k);
        if (seen) {
            *seen += 1;
        } else {
            proven_u32 one = 1;
            err = proven_map_set(&counts, k, &one);
            EXAMPLE_REQUIRE(proven_is_ok(err), "inserting a new client must succeed");
        }
    }

    const proven_u32 *seven = PROVEN_MAP_GET_INT(&counts, proven_u32, 7);
    EXAMPLE_REQUIRE(seven != NULL && *seven == 3, "client 7 sent three events");

    /* A closing event (code 0) means the client is gone: drop its counter. */
    for (proven_size_t i = 0; i < batch_len; ++i) {
        if (batch[i].code == 0) {
            err = proven_map_remove(&counts, (proven_map_key_t){ .id = batch[i].client });
            EXAMPLE_REQUIRE(proven_is_ok(err), "removing a present key must succeed");
        }
    }
    EXAMPLE_REQUIRE(PROVEN_MAP_GET_INT(&counts, proven_u32, 7) == NULL, "the closed client is gone");

    /* --- 5. which hash, and how to see the difference --------------------- */

    /* Two string-key maps over the same keys. The default one hashes with keyed
     * SipHash, so an attacker who chooses the keys cannot force them all into
     * one bucket. The trusted one uses fast FNV-1a and is the right choice ONLY
     * when your own code chooses every key. */
    proven_result_map_t untrusted = PROVEN_MAP_INIT_U8_BORROWED(alloc, proven_u32, 8);
    EXAMPLE_REQUIRE(proven_is_ok(untrusted.err), "creating the default string-key map must succeed");
    proven_map_t from_network = untrusted.value;

    proven_result_map_t tr = proven_map_create_trusted(alloc, 8, PROVEN_KEY_TYPE_U8_BORROWED,
                                                       sizeof(proven_u32), alignof(proven_u32));
    EXAMPLE_REQUIRE(proven_is_ok(tr.err), "creating the trusted-key map must succeed");
    proven_map_t internal = tr.value;

    EXAMPLE_REQUIRE(from_network.trusted_keys == false, "the default map defends against chosen keys");
    EXAMPLE_REQUIRE(internal.trusted_keys == true, "the trusted map opts out of that defence");

    /* map_hash exposes the value the map actually places a key by, so the
     * choice is observable rather than something you take on faith. */
    proven_map_key_t name = { .str = PROVEN_LIT("user-agent") };
    proven_u64 keyed = proven_map_hash(&from_network, name);
    proven_u64 fast  = proven_map_hash(&internal, name);
    EXAMPLE_REQUIRE(keyed != fast, "the same key hashes differently under the two functions");
    EXAMPLE_REQUIRE(proven_map_hash(&internal, name) == fast, "and each function is deterministic");

    /* Keys that live in memory the map does not own are BORROWED: the bytes must
     * outlive the map. These are string literals, so they do. When the key comes
     * from a buffer you are about to reuse, use an owned-key map instead
     * (PROVEN_MAP_INIT_U8_OWNED / proven_map_set_u8_owned), which copies. */
    proven_u32 hits = 1;
    err = proven_map_set(&from_network, name, &hits);
    EXAMPLE_REQUIRE(proven_is_ok(err), "inserting a borrowed string key must succeed");
    EXAMPLE_REQUIRE(PROVEN_MAP_GET_U8_BORROWED(&from_network, proven_u32, PROVEN_LIT("user-agent")) != NULL,
                    "and it can be looked up by an equal view, not the same pointer");

    /* --- 6. checksumming a stream in chunks ------------------------------- */

    /* The batch is checksummed as it goes past, which is what a program reading
     * a file or a socket has to do: it never holds the whole thing. Start the
     * running value at 0, feed each chunk in, and the final value is the same
     * one a single call over the concatenation would produce. */
    proven_u32 running = 0;
    for (proven_size_t i = 0; i < batch_len; ++i) {
        proven_mem_view_t chunk = { .ptr = (const proven_byte_t *)&batch[i], .size = sizeof batch[i] };
        running = proven_crc32_update(running, chunk);
    }
    proven_mem_view_t whole = { .ptr = (const proven_byte_t *)batch, .size = sizeof batch };
    EXAMPLE_REQUIRE(running == proven_crc32(whole),
                    "chunked and whole-buffer CRC-32 must agree, whatever the chunking");

    printf("events=%zu recent=%zu crc32=%08x\n",
           (size_t)events.len, (size_t)recent.len, (unsigned)running);

    proven_map_destroy(&internal);
    proven_map_destroy(&from_network);
    proven_map_destroy(&counts);
    PROVEN_RING_DESTROY(&recent);
    PROVEN_ARRAY_DESTROY(&events);
    return EXAMPLE_OK();
}
