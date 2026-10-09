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
     * Initialize the worker registry.
     */
    initialize_worker_registry(
        &runtime->registry
    );

    if (runtime->scheduler != NULL)
    {
        runtime->scheduler->registry = &runtime->registry;
    }

    /*
     * Initialize runtime statistics.
     */
    initialize_runtime_stats(
        &runtime->stats
    );
}

int runtime_register_worker(
    struct Runtime *runtime,
    struct Worker *worker)
{
    if (runtime == NULL ||
        worker == NULL ||
        runtime->worker_pool == NULL)
    {
        return 0;
    }

    /*
     * Register the worker in the
     * runtime registry.
     */
    if (!register_worker(
            &runtime->registry,
            worker))
    {
        return 0;
    }

    /*
     * Add the worker to the worker pool.
     *
     * The scheduler uses the worker pool
     * when selecting a worker for a task.
     */
    if (!add_worker(
            runtime->worker_pool,
            worker))
    {
        return 0;
    }

    return 1;
}

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
     * Initialize task metrics.
     */
    initialize_task_metrics(
        &task->metrics
    );

    /*
     * Record when the task entered
     * the runtime.
     */
    record_task_submission(
        &task->metrics
    );

    /*
     * Put the task into the FIFO queue.
     */
    int result =
        submit_task(
            runtime->task_manager,
            task
        );

    if (result)
    {
        record_task_submitted(
            &runtime->stats
        );
    }

    return result;
}

int runtime_run_next_task(
    struct Runtime *runtime)
{
    if (runtime == NULL ||
        runtime->scheduler == NULL)
    {
        return 0;
    }

    /*
     * The scheduler will retrieve the
     * next task and select a worker.
     */
    struct Task task = {0};

    struct Worker *selected_worker = NULL;

    if (!schedule_next_task(
            runtime->scheduler,
            &task,
            &selected_worker))
    {
        return 0;
    }

    /*
     * Make sure a worker was selected.
     */
    if (selected_worker == NULL)
    {
        return 0;
    }

    /*
     * Record when execution begins.
     */
    record_task_start(
        &task.metrics
    );

    /*
     * Start the task on the selected worker.
     */
    if (!start_assigned_task(&task))
    {
        record_task_failed(
            &runtime->stats
        );

        return 0;
    }

    /*
     * Wait for the task process to finish.
     */
    if (!wait_for_task(&task))
    {
        record_task_failed(
            &runtime->stats
        );

        return 0;
    }

    /*
     * Record completion timing.
     */
    record_task_completion(
        &task.metrics
    );

    /*
     * Update runtime statistics.
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
        record_task_failed(
            &runtime->stats
        );
    }

    /*
     * Display task metrics.
     */
    printf("\n");
    printf("========================================\n");
    printf("          TASK METRICS\n");
    printf("========================================\n");

    printf(
        "Queue wait      : %.3f seconds\n",
        task.metrics.queue_wait_seconds
    );

    printf(
        "Execution time  : %.3f seconds\n",
        task.metrics.execution_seconds
    );

    printf("========================================\n");

    return 1;
}
