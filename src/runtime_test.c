#include <stdio.h>
#include <string.h>

#include "runtime.h"

int main(void)
{
    printf("\n");
    printf("========================================\n");
    printf("       RUNTIME WORKER REGISTRATION TEST\n");
    printf("========================================\n");

    struct TaskManager task_manager;
    struct WorkerPool worker_pool;
    struct Scheduler scheduler;
    struct Runtime runtime;
    struct Worker worker;

    /*
     * Initialize a worker.
     */
    if (!initialize_worker(&worker, 1))
    {
        printf("Failed to initialize worker.\n");
        return 1;
    }

    printf("\nWorker initialized.\n");
    printf("Worker ID : %d\n", worker.id);

    /*
     * Initialize the runtime subsystems.
     */
    initialize_worker_pool(&worker_pool);

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
     * Register the worker with the runtime.
     */
    printf("\nRegistering worker with runtime...\n");

    if (!runtime_register_worker(
            &runtime,
            &worker))
    {
        printf("Worker registration failed. ❌\n");
        return 1;
    }

    printf("Worker registered successfully. ✅\n");

    /*
     * Display registry information.
     */
    printf("\n");
    printf("========================================\n");
    printf("          WORKER REGISTRY\n");
    printf("========================================\n");

    printf("Registered workers : %d\n",
           get_registered_worker_count(
               &runtime.registry));

    struct Worker *registered_worker =
        find_registered_worker(
            &runtime.registry,
            1
        );

    if (registered_worker != NULL)
    {
        printf("Worker ID          : %d\n",
               registered_worker->id);

        printf("Available CPU      : %.2f%%\n",
               registered_worker
                   ->resources
                   .available_cpu_percent);

        printf("Available Memory   : %ld MB\n",
               registered_worker
                   ->resources
                   .available_memory_mb);
    }

    printf("========================================\n");

    /*
     * Verify registration.
     */
    if (get_registered_worker_count(
            &runtime.registry) != 1)
    {
        printf("\nRegistration test FAILED. ❌\n");
        return 1;
    }

    if (registered_worker == NULL ||
        registered_worker->id != 1)
    {
        printf("\nWorker lookup test FAILED. ❌\n");
        return 1;
    }

    printf("\nWorker registration test passed. ✅\n");

    /*
     * Now test the existing failure
     * execution path as a regression test.
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

    if (!runtime_submit_task(
            &runtime,
            &task))
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

    printf("========================================\n");

    if (runtime.stats.tasks_submitted == 1 &&
        runtime.stats.tasks_completed == 0 &&
        runtime.stats.tasks_failed == 1)
    {
        printf("\nRegression test passed. ✅\n");
    }
    else
    {
        printf("\nRegression test FAILED. ❌\n");
        return 1;
    }

    return 0;
}
