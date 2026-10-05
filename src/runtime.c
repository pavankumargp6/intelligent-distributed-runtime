#include <stddef.h>
#include <stdio.h>

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

    /*
     * Initialize runtime-wide statistics.
     */
    initialize_runtime_stats(&runtime->stats);
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

    /*
     * Initialize metrics before the task
     * enters the runtime queue.
     */
    initialize_task_metrics(&task->metrics);

    /*
     * Record when the task enters
     * the runtime.
     */
    record_task_submission(&task->metrics);

    /*
     * Attempt to submit the task to
     * the task manager.
     */
    int result =
        submit_task(runtime->task_manager, task);

    /*
     * Only count the task as submitted
     * if the queue accepted it.
     */
    if (result)
    {
        record_task_submitted(&runtime->stats);
    }

    return result;
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
     * Record when execution begins.
     *
     * The submission timestamp is already
     * stored inside the task.
     */
    record_task_start(&task.metrics);

    /*
     * Execute only after the scheduler
     * has assigned the task.
     */
    if (!start_assigned_task(&task))
    {
        record_task_failed(&runtime->stats);
        return 0;
    }

    /*
     * Wait for the task to finish and
     * update its final state.
     */
    if (!wait_for_task(&task))
    {
        record_task_failed(&runtime->stats);
        return 0;
    }

    /*
     * Record when execution finishes.
     */
    record_task_completion(&task.metrics);

    /*
     * Record successful or failed task
     * metrics in runtime-wide statistics.
     */
    if (task.state == TASK_COMPLETED)
    {
        record_task_completed(
            &runtime->stats,
            task.metrics.queue_wait_seconds,
            task.metrics.execution_seconds
        );
    }
    else
    {
        record_task_failed(&runtime->stats);
    }

    /*
     * Display the collected metrics.
     */
    printf("\n");
    printf("========================================\n");
    printf("          TASK METRICS\n");
    printf("========================================\n");
    printf("Queue wait      : %.3f seconds\n",
           task.metrics.queue_wait_seconds);
    printf("Execution time  : %.3f seconds\n",
           task.metrics.execution_seconds);
    printf("========================================\n");

    return 1;
}
