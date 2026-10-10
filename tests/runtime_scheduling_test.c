#include <stdio.h>
#include <string.h>
#include "runtime.h"

int main(void)
{
    struct TaskManager manager;
    struct WorkerPool pool;
    struct Scheduler scheduler;
    struct Runtime runtime;
    struct Worker worker = {0};
    struct Task task = {0};
    struct Task scheduled = {0};
    struct Worker *selected = NULL;

    initialize_task_manager(&manager);
    initialize_worker_pool(&pool);
    initialize_scheduler(&scheduler, &manager, &pool);
    initialize_runtime(&runtime, &manager, &scheduler, &pool);

    worker.id = 1;
    worker.resources.available_cpu_percent = 100.0;
    worker.resources.available_memory_mb = 1024;

    if (!runtime_register_worker(&runtime, &worker)) {
        fprintf(stderr, "FAIL: worker registration\n");
        return 1;
    }

    task.id = 101;
    task.required_cpu_percent = 1.0;
    task.required_memory_mb = 1;
    task.assigned_worker_id = -1;
    strcpy(task.command, "true");

    if (!runtime_submit_task(&runtime, &task)) {
        fprintf(stderr, "FAIL: task submission\n");
        return 2;
    }

    /* Mark the only worker unavailable. */
    runtime.registry.workers[0].status = WORKER_UNAVAILABLE;

    if (schedule_next_task(&scheduler, &scheduled, &selected)) {
        fprintf(stderr, "FAIL: scheduled with no active workers\n");
        return 3;
    }

    if (manager.queue.count != 1) {
        fprintf(stderr, "FAIL: task was not preserved in queue\n");
        return 4;
    }

    printf("PASS: task remains queued when worker is unavailable.\n");

    /* Reactivate the worker and try scheduling again. */
    runtime.registry.workers[0].status = WORKER_ACTIVE;

    if (!schedule_next_task(&scheduler, &scheduled, &selected)) {
        fprintf(stderr, "FAIL: task not scheduled after recovery\n");
        return 5;
    }

    if (selected == NULL || selected->id != 1) {
        fprintf(stderr, "FAIL: wrong worker selected after recovery\n");
        return 6;
    }

    printf("PASS: queued task scheduled after worker recovery.\n");
    return 0;
}
