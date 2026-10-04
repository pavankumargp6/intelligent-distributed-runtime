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

/*
 * Find the best worker for a task.
 *
 * A worker is eligible when it has enough
 * CPU and memory for the task.
 *
 * Among eligible workers, select the one
 * with the most available CPU.
 */
int find_best_worker(struct WorkerPool *pool,
                     struct Task *task,
                     struct Worker **selected_worker)
{
    if (pool == NULL ||
        task == NULL ||
        selected_worker == NULL)
    {
        return 0;
    }

    /*
     * No worker has been selected yet.
     */
    *selected_worker = NULL;

    for (int i = 0; i < pool->count; i++)
    {
        struct Worker *worker = &pool->workers[i];

        /*
         * Check whether this worker has
         * enough resources for the task.
         */
        if (!task_fits_worker(
                task,
                &worker->resources))
        {
            continue;
        }

        /*
         * If this is the first suitable worker,
         * select it.
         */
        if (*selected_worker == NULL)
        {
            *selected_worker = worker;
            continue;
        }

        /*
         * Prefer the worker with more
         * available CPU.
         */
        if (worker->resources.available_cpu_percent >
            (*selected_worker)
                ->resources.available_cpu_percent)
        {
            *selected_worker = worker;
        }
    }

    /*
     * No suitable worker was found.
     */
    if (*selected_worker == NULL)
    {
        return 0;
    }

    return 1;
}
