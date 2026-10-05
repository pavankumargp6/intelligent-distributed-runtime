#include <stdio.h>
#include <string.h>

#include "runtime.h"

int main(void)
{
    printf("\n");
    printf("========================================\n");
    printf("       RUNTIME FAILURE TEST\n");
    printf("========================================\n");

    struct TaskManager task_manager;
    struct WorkerPool worker_pool;
    struct Scheduler scheduler;
    struct Runtime runtime;
    struct Worker worker;

    if (!initialize_worker(&worker, 1))
    {
        printf("Failed to initialize worker.\n");
        return 1;
    }

    initialize_worker_pool(&worker_pool);

    if (!add_worker(&worker_pool, &worker))
    {
        printf("Failed to add worker to pool.\n");
        return 1;
    }

    initialize_task_manager(&task_manager);

    initialize_scheduler(
        &scheduler,
        &task_manager,
        &worker_pool
    );

    initialize_runtime(
        &runtime,
        &task_manager,
        &scheduler,
        &worker_pool
    );

    /*
     * Create a task that intentionally
     * exits with status 1.
     */
    struct Task task = {0};

    task.id = 1;

    strcpy(task.command, "sh");

    task.argument_count = 2;

    strcpy(task.arguments[0], "-c");
    strcpy(task.arguments[1], "exit 1");

    task.required_cpu_percent = 1.0;
    task.required_memory_mb = 1;

    task.assigned_worker_id = -1;

    task.state = TASK_CREATED;

    printf("\nSubmitting failing task...\n");

    if (!runtime_submit_task(&runtime, &task))
    {
        printf("Task submission failed.\n");
        return 1;
    }

    printf("Task submitted successfully. ✅\n");

    printf("\nRunning failing task...\n");

    if (!runtime_run_next_task(&runtime))
    {
        printf("Runtime failed to execute the task.\n");
    }

    printf("\n");
    printf("========================================\n");
    printf("        RUNTIME STATISTICS\n");
    printf("========================================\n");

    printf("Tasks submitted : %d\n",
           runtime.stats.tasks_submitted);

    printf("Tasks completed : %d\n",
           runtime.stats.tasks_completed);

    printf("Tasks failed    : %d\n",
           runtime.stats.tasks_failed);

    printf("Average queue wait : %.3f seconds\n",
           get_average_queue_wait(&runtime.stats));

    printf("Average execution  : %.3f seconds\n",
           get_average_execution_time(&runtime.stats));

    printf("========================================\n");

    /*
     * Verify RuntimeStats.
     */
    if (runtime.stats.tasks_submitted == 1 &&
        runtime.stats.tasks_completed == 0 &&
        runtime.stats.tasks_failed == 1)
    {
        printf("\nFailure statistics test passed. ✅\n");
    }
    else
    {
        printf("\nFailure statistics test FAILED. ❌\n");
        return 1;
    }

    return 0;
}
