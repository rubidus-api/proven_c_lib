#include "example.h"

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
