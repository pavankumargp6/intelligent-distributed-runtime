#include <stddef.h>

#include "scheduler.h"

void initialize_scheduler(struct Scheduler *scheduler,
                          struct TaskManager *manager,
                          struct WorkerPool *worker_pool)
{
    if (scheduler == NULL)
    {
        return;
    }

    scheduler->manager = manager;
    scheduler->worker_pool = worker_pool;
}

int schedule_next_task(struct Scheduler *scheduler,
                       struct Task *task,
                       struct Worker **selected_worker)
{
    if (scheduler == NULL ||
        scheduler->manager == NULL ||
        scheduler->worker_pool == NULL ||
        task == NULL ||
        selected_worker == NULL)
    {
        return 0;
    }

    /*
     * Get the next task from the FIFO queue.
     */
    if (!get_next_task(scheduler->manager, task))
    {
        return 0;
    }

    /*
     * Find the best worker that has enough
     * CPU and memory for this task.
     */
    if (!find_best_worker(
            scheduler->worker_pool,
            task,
            selected_worker))
    {
        /*
         * No worker can currently execute
         * the task.
         *
         * Return the task to the queue
         * so it is not lost.
         */
        submit_task(scheduler->manager, task);

        *selected_worker = NULL;

        return 0;
    }

    /*
     * A suitable worker was found.
     */
    return 1;
}
