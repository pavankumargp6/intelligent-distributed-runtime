#include <stddef.h>

#include "worker_pool.h"

/*
 * Initialize an empty worker pool.
 */
void initialize_worker_pool(struct WorkerPool *pool)
{
    if (pool == NULL)
    {
        return;
    }

    pool->count = 0;
}

/*
 * Add a worker to the pool.
 */
int add_worker(struct WorkerPool *pool,
               struct Worker *worker)
{
    if (pool == NULL ||
        worker == NULL)
    {
        return 0;
    }

    /*
     * Do not add workers beyond
     * the pool's capacity.
     */
    if (pool->count >= WORKER_POOL_CAPACITY)
    {
        return 0;
    }

    /*
     * Copy the worker into the pool.
     */
    pool->workers[pool->count] = *worker;

    pool->count++;

    return 1;
}

/*
 * Return the number of workers
 * currently stored in the pool.
 */
int get_worker_count(struct WorkerPool *pool)
{
    if (pool == NULL)
    {
        return 0;
    }

    return pool->count;
}
