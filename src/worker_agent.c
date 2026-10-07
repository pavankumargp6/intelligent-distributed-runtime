#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "worker.h"

int main(int argc, char *argv[])
{
    /*
     * The worker ID is provided
     * as a command-line argument.
     *
     * Example:
     *
     * ./worker_agent 1
     */
    if (argc != 2)
    {
        printf("Usage: %s <worker_id>\n", argv[0]);
        return 1;
    }

    int worker_id = atoi(argv[1]);

    if (worker_id <= 0)
    {
        printf("Worker ID must be greater than 0.\n");
        return 1;
    }

    /*
     * Create the worker.
     */
    struct Worker worker;

    /*
     * Initialize the worker.
     *
     * This collects the worker's
     * current CPU and memory resources.
     */
    if (!initialize_worker(&worker, worker_id))
    {
        printf("Failed to initialize worker.\n");
        return 1;
    }

    /*
     * Display worker information.
     */
    printf("\n");
    printf("========================================\n");
    printf("             WORKER AGENT\n");
    printf("========================================\n");

    printf("Worker ID           : %d\n",
           worker.id);

    printf("Total CPU           : %.2f%%\n",
           worker.resources.total_cpu_percent);

    printf("Available CPU       : %.2f%%\n",
           worker.resources.available_cpu_percent);

    printf("Total Memory        : %ld MB\n",
           worker.resources.total_memory_mb);

    printf("Available Memory    : %ld MB\n",
           worker.resources.available_memory_mb);

    printf("Status              : READY\n");

    printf("========================================\n");

    /*
     * Keep the worker agent alive.
     *
     * sleep() prevents the worker from
     * consuming CPU while it is idle.
     */
    while (1)
    {
        sleep(5);
    }

    return 0;
}
