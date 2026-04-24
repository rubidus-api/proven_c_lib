#include "proven.h"
#include "proven_test.h"
#include "proven/sysio.h"



// 1. Defining generic consumer Struct
typedef struct {
    int job_id;
    proven_list_node_t linkage;  // The intrusive payload hook
    float computed_value;
} job_task_t;

int main() {
    PROVEN_TEST_INFO("Running Phase 9 Intrusive Linked List Tests...");

    // ============================================
    // 1. List Initialization
    // ============================================
    proven_list_t job_queue;
    proven_list_init(&job_queue);
    
    PROVEN_TEST_ASSERT(proven_list_is_empty(&job_queue), "Testing condition: proven_list_is_empty(&job_queue)", "Review logic surrounding proven_list_is_empty(&job_queue)");

    // ============================================
    // 2. Safe Structural Node Appending
    // ============================================
    job_task_t t1 = { .job_id = 10, .computed_value = 1.0f };
    job_task_t t2 = { .job_id = 20, .computed_value = 2.0f };
    job_task_t t3 = { .job_id = 30, .computed_value = 3.0f };

    // Zero-alloc attachment bypassing malloc!
    proven_list_push_back(&job_queue, &t1.linkage);
    PROVEN_TEST_ASSERT(!proven_list_is_empty(&job_queue), "Testing condition: !proven_list_is_empty(&job_queue)", "Review logic surrounding !proven_list_is_empty(&job_queue)");
    
    proven_list_push_back(&job_queue, &t2.linkage);
    proven_list_push_back(&job_queue, &t3.linkage);

    // ============================================
    // 3. Mathematical Reverse Iteration & Extraction
    // ============================================
    int sum = 0;
    proven_list_node_t *curr;
    
    PROVEN_LIST_FOR_EACH(curr, &job_queue) {
        // Extraction
        job_task_t *task = PROVEN_CONTAINER_OF(curr, job_task_t, linkage);
        sum += task->job_id;
    }
    
    // Assert 10 + 20 + 30 = 60 implicitly validating memory bounds distance offset calculations.
    PROVEN_TEST_ASSERT(sum == 60, "Testing condition: sum == 60", "Review logic surrounding sum == 60");

    // ============================================
    // 4. Safe Detachment Loop
    // ============================================
    proven_list_node_t *safe_next;
    sum = 0;

    // Mutative deletion iterating over active chain links seamlessly avoiding UB segmentation faults
    PROVEN_LIST_FOR_EACH_SAFE(curr, safe_next, &job_queue) {
        job_task_t *task = PROVEN_CONTAINER_OF(curr, job_task_t, linkage);
        if (task->job_id == 20) {
            proven_list_remove(curr);
        } else {
            sum += task->job_id;
        }
    }
    
    // Remaining objects should only be t1(10) and t3(30) => 40
    PROVEN_TEST_ASSERT(sum == 40, "Testing condition: sum == 40", "Review logic surrounding sum == 40");

    // Structural validations resolving boundary linkages
    job_task_t *first = PROVEN_LIST_ENTRY(job_queue.head.next, job_task_t, linkage);
    job_task_t *last = PROVEN_LIST_ENTRY(job_queue.head.prev, job_task_t, linkage);
    PROVEN_TEST_ASSERT(first->job_id == 10, "Testing condition: first->job_id == 10", "Review logic surrounding first->job_id == 10");
    PROVEN_TEST_ASSERT(last->job_id == 30, "Testing condition: last->job_id == 30", "Review logic surrounding last->job_id == 30");

    PROVEN_TEST_INFO("All Phase 9 Intrusive List Tests Passed Successfully!");
    return 0;
}
