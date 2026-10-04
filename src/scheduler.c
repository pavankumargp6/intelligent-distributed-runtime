#include <stddef.h>

#include "scheduler.h"

void initialize_scheduler(struct Scheduler *scheduler,
                          struct TaskManager *manager)
{
    if (scheduler == NULL)
    {
        return;
    }

    scheduler->manager = manager;

    /*
     * Collect the worker's current resources
     * when the scheduler is initialized.
     */
    if (!collect_worker_resources(&scheduler->resources))
    {
        /*
         * If resource collection fails,
         * initialize the values to zero.
         */
        scheduler->resources.total_cpu_percent = 0.0;
        scheduler->resources.available_cpu_percent = 0.0;

        scheduler->resources.total_memory_mb = 0;
        scheduler->resources.available_memory_mb = 0;
    }
}

int schedule_next_task(struct Scheduler *scheduler,
                       struct Task *task)
{
    if (scheduler == NULL ||
        scheduler->manager == NULL ||
        task == NULL)
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
     * Refresh the worker's resource information
     * before making the scheduling decision.
     */
    if (!collect_worker_resources(&scheduler->resources))
    {
        /*
         * Resource information is unavailable.
         * Do not execute the task.
         *
         * The task has already been removed from
         * the queue, so put it back.
         */
        submit_task(scheduler->manager, task);

        return 0;
    }

    /*
     * Check whether the task's resource requirements
     * fit within the worker's current resources.
     */
    if (!task_fits_worker(task,
                          &scheduler->resources))
    {
        /*
         * The task cannot currently run.
         *
         * Put it back into the queue so it is
         * not lost.
         */
        submit_task(scheduler->manager, task);

        return 0;
    }

    /*
     * The task fits on this worker.
     */
    return 1;
}
