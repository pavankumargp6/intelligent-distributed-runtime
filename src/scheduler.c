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
    scheduler->registry = NULL;
}

int schedule_next_task(struct Scheduler *scheduler,
                       struct Task *task,
                       struct Worker **selected_worker)
{
    if (scheduler == NULL ||
        scheduler->manager == NULL ||
        scheduler->worker_pool == NULL ||
        scheduler->registry == NULL ||
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
            scheduler->registry,
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
     * Record the selected worker directly
     * in the task.
     */
    task->assigned_worker_id =
        (*selected_worker)->id;

    /*
     * A suitable worker was found.
     */
    return 1;
}

int schedule_next_task_for_worker(struct Scheduler *scheduler,
                                  struct Task *task,
                                  int worker_id,
                                  struct Worker **selected_worker)
{
    if (scheduler == NULL ||
        scheduler->manager == NULL ||
        scheduler->worker_pool == NULL ||
        scheduler->registry == NULL ||
        task == NULL ||
        selected_worker == NULL ||
        worker_id <= 0)
    {
        return 0;
    }

    *selected_worker = NULL;

    struct Worker *asking = NULL;

    for (int i = 0; i < scheduler->worker_pool->count; i++)
    {
        if (scheduler->worker_pool->workers[i].id == worker_id)
        {
            asking = &scheduler->worker_pool->workers[i];
            break;
        }
    }

    if (asking == NULL)
    {
        return 0;
    }

    if (!is_worker_live(scheduler->registry, worker_id))
    {
        return 0;
    }

    if (!get_next_task(scheduler->manager, task))
    {
        return 0;
    }

    if (!task_fits_worker(task, &asking->resources))
    {
        submit_task(scheduler->manager, task);
        return 0;
    }

    task->assigned_worker_id = asking->id;
    *selected_worker = asking;

    return 1;
}
