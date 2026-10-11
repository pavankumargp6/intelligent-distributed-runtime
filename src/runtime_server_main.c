#include <stdio.h>

#include "runtime.h"
#include "runtime_server.h"
#include "log.h"

#define RUNTIME_SERVER_PORT 5001

int main(void)
{
    /*
     * Force line-buffered stdout so redirected logs are
     * flushed promptly and survive SIGTERM.
     */
    setvbuf(stdout, NULL, _IOLBF, 0);
    log_init();

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

    log_info("coordinator_starting", "Starting runtime coordinator...");

    /*
     * Start the TCP coordinator.
     */
    if (!start_runtime_server(
            &runtime,
            RUNTIME_SERVER_PORT))
    {
        log_error("coordinator_start_failed", "Runtime server failed.");

        return 1;
    }

    return 0;
}
