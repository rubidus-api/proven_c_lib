#ifndef PROVEN_JOB_H
#define PROVEN_JOB_H

#include "proven/types.h"
#include "proven/error.h"
#include "proven/allocator.h"

/**
 * @file job.h
 * @brief Zero-overhead Lock-Free MPMC (Multi-Producer Multi-Consumer) Job Scheduler.
 * 
 * Submits work to a pre-allocated pool of worker threads.
 * Synchronization is handled exclusively through C11 Atomics (stdatomic.h),
 * allowing threads to pull work without ever triggering expensive block/sleep states (until inherently starved).
 */

/**
 * @brief Defines a unit of independent work.
 */
typedef struct {
    void (*routine)(void* arg);
    void* arg;
} proven_job_t;

/**
 * @brief Initialize the global Job System.
 * 
 * @param alloc The allocator for the Lock-Free Ring Buffer.
 * @param num_workers Total OS threads to reserve.
 * @param max_queue_capacity Size of the command buffer. MUST be a power of 2!
 * 
 * @return PROVEN_OK if successful.
 */
[[nodiscard]]
proven_err_t proven_job_system_init(proven_allocator_t alloc, proven_size_t num_workers, proven_size_t max_queue_capacity);

/**
 * @brief Shuts down the Job System. 
 * Blocks calling thread until the queue is completely exhausted and all workers are gracefully joined.
 */
void proven_job_system_shutdown(void);

/**
 * @brief Enqueue work to the Ring-Buffer.
 * This is 100% Lock-Free and can be safely called simultaneously from hundreds of threads.
 * 
 * @return true if enqueued successfully. false if the ring buffer is completely full.
 */
[[nodiscard]]
bool proven_job_submit(void (*routine)(void*), void* arg);

/**
 * @brief Forces the calling thread to attempt executing one task from the queue.
 * Useful for the main thread to contribute to the job pool while waiting for completion.
 * 
 * @return true if a job was found and executed. false if the queue was empty.
 */
[[nodiscard]]
bool proven_job_execute_one(void);

#endif /* PROVEN_JOB_H */
