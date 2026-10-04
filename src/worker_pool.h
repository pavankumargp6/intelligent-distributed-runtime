#ifndef WORKER_POOL_H
#define WORKER_POOL_H

#include "worker.h"

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

#endif
