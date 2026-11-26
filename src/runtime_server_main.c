#include <stdio.h>

#include "runtime.h"
#include "runtime_server.h"
#include "runtime_endpoint.h"
#include "runtime_limits.h"
#include "log.h"

int main(void)
{
    /*
     * Force line-buffered stdout so redirected logs are
     * flushed promptly and survive SIGTERM.
     */
    setvbuf(stdout, NULL, _IOLBF, 0);
    log_init();

    /*
     * Resolve the listening port before touching any durable
     * state. An invalid IDR_PORT is a configuration error we
     * want to reject before opening the WAL.
     */
    int port = runtime_listen_port();

    if (port < 0)
    {
        log_error("coordinator_port_invalid",
                  "IDR_PORT must be an integer between 1 and 65535.");
        return 1;
    }

    struct TaskManager task_manager;
    struct WorkerPool worker_pool;
    struct Scheduler scheduler;
    struct Runtime runtime;

    /*
     * Initialize runtime subsystems.
     */
    if (!initialize_worker_pool(&worker_pool, runtime_worker_pool_capacity()))
    {
        log_error("worker_pool_init_failed", "Failed to allocate worker pool.");
        return 1;
    }

    initialize_task_manager(&task_manager, runtime_task_queue_capacity());

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
            port))
    {
        log_error("coordinator_start_failed", "Runtime server failed.");

        return 1;
    }

    return 0;
}
