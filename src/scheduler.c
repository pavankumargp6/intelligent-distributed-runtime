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
     * Reserve a task slot on the selected worker before assigning.
     * The slot is released when the task completes, is reaped, or
     * the worker disappears, so a worker can never be handed more
     * tasks than its capacity.
     */
    if (!worker_reserve_slot(scheduler->registry,
                            (*selected_worker)->id))
    {
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
     * Increment the worker's epoch and store it in the task.
     * This fencing token is included in TASK_ASSIGN, echoed back
     * in TASK_ACCEPT and TASK_RESULT to reject stale responses
     * from previous dispatch attempts.
     */
    task->epoch = worker_increment_epoch(scheduler->registry,
                                         (*selected_worker)->id);
    if (task->epoch < 0) {
        /* Should not happen: worker was just verified live. */
        worker_release_slot(scheduler->registry,
                            (*selected_worker)->id);
        submit_task(scheduler->manager, task);
        *selected_worker = NULL;
        return 0;
    }

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

    if (asking == NULL || !is_worker_live(scheduler->registry, worker_id))
    {
        return 0;
    }

    struct TaskQueue *queue = &scheduler->manager->queue;

    /* 
     * Scan the queue for the first task that fits this worker.
     * This prevents Head-of-Line (HOL) blocking.
     */
    for (int i = 0; i < queue->count; i++)
    {
        int actual_index = (queue->front + i) % (int)queue->capacity;
        struct Task *candidate = &queue->tasks[actual_index];

        if (task_fits_worker(candidate, &asking->resources) &&
            worker_reserve_slot(scheduler->registry, asking->id))
        {
            /*
             * O(1) trick: Swap the fitting task with the front of the queue,
             * then use the normal dequeue_task to pop it.
             */
            if (i != 0)
            {
                struct Task temp = queue->tasks[queue->front];
                queue->tasks[queue->front] = *candidate;
                *candidate = temp;
            }

            dequeue_task(queue, task);
            task->assigned_worker_id = asking->id;

            /*
             * Increment the worker's epoch and store it in the task.
             */
            task->epoch = worker_increment_epoch(scheduler->registry,
                                                 asking->id);
            if (task->epoch < 0) {
                worker_release_slot(scheduler->registry, asking->id);
                return 0;
            }

            *selected_worker = asking;
            return 1;
        }
    }

    /* No tasks in the queue fit this worker right now. */
    return 0;
}
