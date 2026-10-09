#ifndef WORKER_POOL_H
#define WORKER_POOL_H

#include "worker.h"
#include "worker_registry.h"

#define WORKER_POOL_CAPACITY 16

/*
 * Collection of workers managed by
 * the distributed runtime.
 */
struct WorkerPool
{
    struct Worker workers[WORKER_POOL_CAPACITY];

    int count;
};

/*
 * Initialize an empty worker pool.
 */
void initialize_worker_pool(struct WorkerPool *pool);

/*
 * Add a worker to the pool.
 *
 * Returns:
 *
 * 1 -> success
 * 0 -> failure
 */
int add_worker(struct WorkerPool *pool,
               struct Worker *worker);

/*
 * Get the number of workers
 * currently in the pool.
 */
int get_worker_count(struct WorkerPool *pool);

/*
 * Find the best worker for a task.
 *
 * The selected worker must have enough
 * CPU and memory for the task.
 *
 * Among all workers that can run the task,
 * the worker with the most available CPU
 * is selected.
 *
 * Returns:
 *
 * 1 -> suitable worker found
 * 0 -> no suitable worker found
 */
int find_best_worker(struct WorkerPool *pool,
                     struct WorkerRegistry *registry,
                     struct Task *task,
                     struct Worker **selected_worker);

#endif
