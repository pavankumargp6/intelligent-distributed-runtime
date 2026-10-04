#include <stddef.h>

#include "runtime.h"

void initialize_runtime(struct Runtime *runtime,
                        struct TaskManager *task_manager,
                        struct Scheduler *scheduler,
                        struct WorkerPool *worker_pool)
{
    if (runtime == NULL)
    {
        return;
    }

    runtime->task_manager = task_manager;
    runtime->scheduler = scheduler;
    runtime->worker_pool = worker_pool;
}

/*
 * Submit a task to the runtime.
 */
int runtime_submit_task(struct Runtime *runtime,
                        struct Task *task)
{
    if (runtime == NULL ||
        runtime->task_manager == NULL ||
        task == NULL)
    {
        return 0;
    }

    return submit_task(runtime->task_manager, task);
}

/*
 * Schedule and execute the next
 * available task.
 */
int runtime_run_next_task(struct Runtime *runtime)
{
    if (runtime == NULL ||
        runtime->scheduler == NULL)
    {
        return 0;
    }

    struct Task task = {0};
    struct Worker *selected_worker = NULL;

    /*
     * Ask the scheduler for the next
     * executable task and worker.
     */
    if (!schedule_next_task(
            runtime->scheduler,
            &task,
            &selected_worker))
    {
        return 0;
    }

    /*
     * The scheduler must provide a worker
     * before execution can begin.
     */
    if (selected_worker == NULL)
    {
        return 0;
    }

    /*
     * Execute only after the scheduler
     * has assigned the task.
     */
    if (!start_assigned_task(&task))
    {
        return 0;
    }

    /*
     * Wait for the task to finish and
     * update its final state.
     */
    if (!wait_for_task(&task))
    {
        return 0;
    }

    return 1;
}
