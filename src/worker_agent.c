#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "worker.h"
#include "worker_registration_client.h"

#define RUNTIME_ADDRESS "127.0.0.1"
#define HEARTBEAT_INTERVAL_SECONDS 3

int main(int argc, char *argv[])
{
    /*
     * Worker ID is provided through
     * the command line.
     *
     * Example:
     *
     * ./worker_agent 1
     */
    if (argc != 2)
    {
        printf(
            "Usage: %s <worker_id>\n",
            argv[0]
        );

        return 1;
    }

    int worker_id = atoi(argv[1]);

    if (worker_id <= 0)
    {
        printf(
            "Worker ID must be greater than 0.\n"
        );

        return 1;
    }

    /*
     * Create and initialize the worker.
     */
    struct Worker worker;

    if (!initialize_worker(
            &worker,
            worker_id))
    {
        printf(
            "Failed to initialize worker.\n"
        );

        return 1;
    }

    printf("\n");
    printf("========================================\n");
    printf("             WORKER AGENT\n");
    printf("========================================\n");

    printf(
        "Worker ID        : %d\n",
        worker.id
    );

    printf(
        "Total CPU        : %.2f%%\n",
        worker.resources.total_cpu_percent
    );

    printf(
        "Available CPU    : %.2f%%\n",
        worker.resources.available_cpu_percent
    );

    printf(
        "Total Memory     : %ld MB\n",
        worker.resources.total_memory_mb
    );

    printf(
        "Available Memory : %ld MB\n",
        worker.resources.available_memory_mb
    );

    printf(
        "Status           : STARTING\n"
    );

    printf("========================================\n");

    /*
     * Register the worker with the
     * runtime coordinator.
     */
    printf(
        "Registering worker with runtime...\n"
    );

    if (!register_worker_with_runtime(
            &worker,
            RUNTIME_ADDRESS))
    {
        printf(
            "Worker registration failed.\n"
        );

        return 1;
    }

    printf(
        "Worker status: ACTIVE\n"
    );

    /*
     * ------------------------------------------------
     * Heartbeat loop
     * ------------------------------------------------
     *
     * The worker periodically tells the
     * runtime that it is still alive.
     */
    while (1)
    {
        sleep(
            HEARTBEAT_INTERVAL_SECONDS
        );

        printf(
            "\nSending heartbeat...\n"
        );

        if (!send_worker_heartbeat(
                &worker,
                RUNTIME_ADDRESS))
        {
            printf(
                "Heartbeat failed.\n"
            );

            /*
             * For now, keep the worker alive
             * and try again during the next
             * heartbeat interval.
             */
            continue;
        }
    }

    return 0;
}
