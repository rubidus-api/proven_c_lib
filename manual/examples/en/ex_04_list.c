#include "example.h"

/*
 * An INTRUSIVE list puts the links inside your struct instead of allocating a
 * node to hold your data.
 *
 * The list you were taught looks like this:
 *
 *     struct node { struct node *next; void *data; };
 *
 * Every insertion allocates a node, so a list of a thousand items costs a
 * thousand allocations that exist only to hold pointers, each one a chance to
 * fail and a thing to free. Worse, `data` is a void* - the list has no idea what
 * it holds, so every read is a cast the compiler cannot check.
 *
 * Intrusive lists invert it: YOU own the memory, and the link lives in it.
 * Inserting allocates nothing and cannot fail. Removing allocates nothing and
 * cannot fail. And because the link is a member of a known type, getting back
 * from a link to the object is arithmetic the compiler does for you, not a cast.
 *
 * The trade is that an object can only be in as many lists as it has link
 * members, and that is a decision you make when you declare the struct.
 */

/* The link is a member. This task can be in exactly one list at a time. */
typedef struct {
    int                 id;
    proven_list_node_t  link;
} task_t;

int main(void) {
    /* No allocator anywhere in this program: the tasks are on the stack, and the
     * list only ever rearranges pointers that live inside them. */
    task_t a = { .id = 1 };
    task_t b = { .id = 2 };
    task_t c = { .id = 3 };

    proven_list_t queue;
    proven_list_init(&queue);
    EXAMPLE_REQUIRE(proven_list_is_empty(&queue), "a freshly initialised list is empty");

    /* --- pushing cannot fail, because nothing is allocated ------------- */
    proven_list_push_back(&queue, &a.link);
    proven_list_push_back(&queue, &b.link);
    proven_list_push_back(&queue, &c.link);
    EXAMPLE_REQUIRE(!proven_list_is_empty(&queue), "three tasks are queued");

    /* --- walking: PROVEN_LIST_ENTRY gets the object back from the link -- */
    proven_list_node_t *it = NULL;
    int seen[3] = {0}, n = 0;
    PROVEN_LIST_FOR_EACH(it, &queue) {
        task_t *t = PROVEN_LIST_ENTRY(it, task_t, link);
        if (n < 3) seen[n++] = t->id;
    }
    EXAMPLE_REQUIRE(n == 3 && seen[0] == 1 && seen[1] == 2 && seen[2] == 3,
                    "the walk visits every task, in insertion order");

    /* --- removing while walking needs the SAFE form -------------------- */
    /* proven_list_remove writes through the node's own next/prev pointers, so a
     * plain FOR_EACH would read `it->next` from a node that has just been
     * unlinked. The _SAFE form reads the next pointer BEFORE the body runs. */
    proven_list_node_t *safe = NULL;
    PROVEN_LIST_FOR_EACH_SAFE(it, safe, &queue) {
        task_t *t = PROVEN_LIST_ENTRY(it, task_t, link);
        if (t->id == 2) proven_list_remove(it);
    }

    n = 0;
    PROVEN_LIST_FOR_EACH(it, &queue) {
        task_t *t = PROVEN_LIST_ENTRY(it, task_t, link);
        if (n < 3) seen[n++] = t->id;
    }
    EXAMPLE_REQUIRE(n == 2 && seen[0] == 1 && seen[1] == 3, "task 2 was unlinked");

    /* --- inserting in the middle is a pointer swap --------------------- */
    task_t d = { .id = 4 };
    proven_list_insert_after(&a.link, &d.link);

    n = 0;
    PROVEN_LIST_FOR_EACH(it, &queue) {
        task_t *t = PROVEN_LIST_ENTRY(it, task_t, link);
        if (n < 3) seen[n++] = t->id;
    }
    EXAMPLE_REQUIRE(n == 3 && seen[0] == 1 && seen[1] == 4 && seen[2] == 3,
                    "task 4 sits directly after task 1");

    /* There is nothing to destroy. The list never owned anything: `a`, `c` and
     * `d` are still perfectly good local variables, and their lifetime is the
     * function's, exactly as it would be without the list. */
    return EXAMPLE_OK();
}
