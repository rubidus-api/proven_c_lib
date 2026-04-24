#include "proven.h"
#include "proven_test.h"
#include "proven/sysio.h"

#include <string.h>


// 1. Comparison for integers
int compare_int(const void *a, const void *b) {
    int va = *(const int*)a;
    int vb = *(const int*)b;
    return (va > vb) - (va < vb);
}

// 2. Comparison for structs
typedef struct {
    int id;
    int score;
} score_t;

int compare_score(const void *a, const void *b) {
    const score_t *sa = (const score_t*)a;
    const score_t *sb = (const score_t*)b;
    // Sort by score descending, then by id ascending
    if (sa->score != sb->score) {
        return (sb->score > sa->score) - (sb->score < sa->score);
    }
    return (sa->id > sb->id) - (sa->id < sb->id);
}

int main() {
    PROVEN_TEST_INFO("Running Phase 12 Algorithm Tests...");

    proven_allocator_t heap = proven_heap_allocator();

    // ============================================
    // 1. Integer Sorting & Binary Search
    // ============================================
    proven_result_array_t res = PROVEN_ARRAY_INIT(heap, int, 5);
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(res.err), "Testing condition: PROVEN_IS_OK(res.err)", "Review logic surrounding PROVEN_IS_OK(res.err)");
    proven_array_t arr = res.value;

    PROVEN_TEST_ASSERT(PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&arr, int, 50)), "Testing condition: PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&arr, int, 50))", "Review logic surrounding PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&arr, int, 50))");
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&arr, int, 10)), "Testing condition: PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&arr, int, 10))", "Review logic surrounding PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&arr, int, 10))");
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&arr, int, 40)), "Testing condition: PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&arr, int, 40))", "Review logic surrounding PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&arr, int, 40))");
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&arr, int, 20)), "Testing condition: PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&arr, int, 20))", "Review logic surrounding PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&arr, int, 20))");
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&arr, int, 30)), "Testing condition: PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&arr, int, 30))", "Review logic surrounding PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&arr, int, 30))");

    // Sort the integers
    proven_array_sort(&arr, compare_int);

    // Verify order
    PROVEN_TEST_ASSERT(*(int*)proven_array_get(&arr, 0) == 10, "Testing condition: *(int*)proven_array_get(&arr, 0) == 10", "Review logic surrounding *(int*)proven_array_get(&arr, 0) == 10");
    PROVEN_TEST_ASSERT(*(int*)proven_array_get(&arr, 1) == 20, "Testing condition: *(int*)proven_array_get(&arr, 1) == 20", "Review logic surrounding *(int*)proven_array_get(&arr, 1) == 20");
    PROVEN_TEST_ASSERT(*(int*)proven_array_get(&arr, 2) == 30, "Testing condition: *(int*)proven_array_get(&arr, 2) == 30", "Review logic surrounding *(int*)proven_array_get(&arr, 2) == 30");
    PROVEN_TEST_ASSERT(*(int*)proven_array_get(&arr, 3) == 40, "Testing condition: *(int*)proven_array_get(&arr, 3) == 40", "Review logic surrounding *(int*)proven_array_get(&arr, 3) == 40");
    PROVEN_TEST_ASSERT(*(int*)proven_array_get(&arr, 4) == 50, "Testing condition: *(int*)proven_array_get(&arr, 4) == 50", "Review logic surrounding *(int*)proven_array_get(&arr, 4) == 50");

    // Binary Search
    int key = 30;
    int *found = (int*)proven_array_binary_search(&arr, &key, compare_int);
    PROVEN_TEST_ASSERT(found != NULL && *found == 30, "Testing condition: found != NULL && *found == 30", "Review logic surrounding found != NULL && *found == 30");

    key = 25; // Not in array
    found = (int*)proven_array_binary_search(&arr, &key, compare_int);
    PROVEN_TEST_ASSERT(found == NULL, "Testing condition: found == NULL", "Review logic surrounding found == NULL");

    PROVEN_ARRAY_DESTROY(&arr);

    // ============================================
    // 2. Struct Sorting (Complex Comparator)
    // ============================================
    proven_result_array_t s_res = PROVEN_ARRAY_INIT(heap, score_t, 3);
    proven_array_t s_arr = s_res.value;

    score_t p1 = { .id = 1, .score = 100 };
    score_t p2 = { .id = 2, .score = 500 };
    score_t p3 = { .id = 3, .score = 100 };

    PROVEN_TEST_ASSERT(PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&s_arr, score_t, p1)), "Testing condition: PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&s_arr, score_t, p1))", "Review logic surrounding PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&s_arr, score_t, p1))");
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&s_arr, score_t, p2)), "Testing condition: PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&s_arr, score_t, p2))", "Review logic surrounding PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&s_arr, score_t, p2))");
    PROVEN_TEST_ASSERT(PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&s_arr, score_t, p3)), "Testing condition: PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&s_arr, score_t, p3))", "Review logic surrounding PROVEN_IS_OK(PROVEN_ARRAY_PUSH(&s_arr, score_t, p3))");

    // Sort (Score DESC, then ID ASC)
    proven_array_sort(&s_arr, compare_score);

    // Expected: [p2 (500), p1 (100, id 1), p3 (100, id 3)]
    score_t *r0 = (score_t*)proven_array_get(&s_arr, 0);
    score_t *r1 = (score_t*)proven_array_get(&s_arr, 1);
    score_t *r2 = (score_t*)proven_array_get(&s_arr, 2);

    PROVEN_TEST_ASSERT(r0->score == 500, "Testing condition: r0->score == 500", "Review logic surrounding r0->score == 500");
    PROVEN_TEST_ASSERT(r1->score == 100 && r1->id == 1, "Testing condition: r1->score == 100 && r1->id == 1", "Review logic surrounding r1->score == 100 && r1->id == 1");
    PROVEN_TEST_ASSERT(r2->score == 100 && r2->id == 3, "Testing condition: r2->score == 100 && r2->id == 3", "Review logic surrounding r2->score == 100 && r2->id == 3");

    PROVEN_ARRAY_DESTROY(&s_arr);

    PROVEN_TEST_INFO("All Phase 12 Algorithm Tests Passed Successfully!");
    return 0;
}
