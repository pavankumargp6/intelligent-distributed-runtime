#include <stdio.h>

#include "runtime.h"
#include "runtime_server.h"

#define RUNTIME_SERVER_PORT 5001

int main(void)
{
    struct TaskManager task_manager;
    struct WorkerPool worker_pool;
    struct Scheduler scheduler;
    struct Runtime runtime;

    /*
     * Initialize runtime subsystems.
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

    printf("Starting runtime coordinator...\n");

    /*
     * Start the TCP coordinator.
     */
    if (!start_runtime_server(
            &runtime,
            RUNTIME_SERVER_PORT))
    {
        printf(
            "Runtime server failed.\n"
        );

        return 1;
    }

    return 0;
}
