#include <stdio.h>
#include <stdlib.h>

#include "worker.h"
#include "worker_registration_client.h"

int main(int argc, char *argv[])
{
    /*
     * Usage:
     *
     * ./worker_registration_test <worker_id>
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
     * Initialize the worker and collect
     * its current resource information.
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
    printf("       WORKER REGISTRATION TEST\n");
    printf("========================================\n");

    printf("Worker ID      : %d\n",
           worker.id);

    printf("Total CPU      : %.2f%%\n",
           worker.resources.total_cpu_percent);

    printf("Total Memory   : %ld MB\n",
           worker.resources.total_memory_mb);

    printf("========================================\n");

    /*
     * Connect to the runtime coordinator.
     *
     * 127.0.0.1 means the coordinator
     * is running on this same machine.
     */
    if (!register_worker_with_runtime(
            &worker,
            "127.0.0.1"))
    {
        printf(
            "\nWorker registration failed. ❌\n"
        );

        return 1;
    }

    printf(
        "\nWorker registration test passed. ✅\n"
    );

    return 0;
}
