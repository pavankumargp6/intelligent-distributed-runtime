#include <stddef.h>

#include "worker_pool.h"

/*
 * Initialize an empty worker pool.
 */
void initialize_worker_pool(struct WorkerPool *pool)
{
    if (pool == NULL) {
        return;
    }

    pool->count = 0;
}

/*
 * Add a worker or update an existing worker
 * with the same ID.
 */
int add_worker(struct WorkerPool *pool,
               struct Worker *worker)
{
    if (pool == NULL || worker == NULL ||
        worker->id <= 0) {
        return 0;
    }

    /*
     * Update an existing worker instead of
     * creating a duplicate entry.
     */
    for (int i = 0; i < pool->count; i++) {
        if (pool->workers[i].id == worker->id) {
            pool->workers[i] = *worker;
            return 1;
        }
    }

    /*
     * Enforce capacity for new workers.
     */
    if (pool->count >= WORKER_POOL_CAPACITY) {
        return 0;
    }

    /*
     * Add the new worker.
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
    if (pool == NULL) {
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
                     struct WorkerRegistry *registry,
                     struct Task *task,
                     struct Worker **selected_worker)
{
    if (pool == NULL || registry == NULL ||
        task == NULL || selected_worker == NULL) {
        return 0;
    }

    *selected_worker = NULL;

    for (int i = 0; i < pool->count; i++) {
        struct Worker *worker = &pool->workers[i];
        int active = 0;

        for (int j = 0; j < registry->count; j++) {
            if (registry->workers[j].worker.id == worker->id) {
                active = (registry->workers[j].status == WORKER_ACTIVE);
                break;
            }
        }

        if (!active) {
            continue;
        }

        if (!task_fits_worker(task, &worker->resources)) {
            continue;
        }

        if (*selected_worker == NULL ||
            worker->resources.available_cpu_percent >
            (*selected_worker)->resources.available_cpu_percent) {
            *selected_worker = worker;
        }
    }

    return *selected_worker != NULL;
}
