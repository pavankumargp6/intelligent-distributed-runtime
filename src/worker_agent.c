#include <stdio.h>
#include <stdlib.h>

#include "worker.h"
#include "worker_server.h"

#define WORKER_SERVER_PORT 5000

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
     * Create and initialize the worker.
     */
    struct Worker worker;

    if (!initialize_worker(
            &worker,
            worker_id))
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

    printf("Worker ID        : %d\n",
           worker.id);

    printf("Total CPU        : %.2f%%\n",
           worker.resources.total_cpu_percent);

    printf("Available CPU    : %.2f%%\n",
           worker.resources.available_cpu_percent);

    printf("Total Memory     : %ld MB\n",
           worker.resources.total_memory_mb);

    printf("Available Memory : %ld MB\n",
           worker.resources.available_memory_mb);

    printf("Status           : READY\n");

    printf("========================================\n");

    /*
     * Start the worker TCP server.
     *
     * For now this server still handles
     * incoming connections. Registration
     * will be handled by the coordinator
     * side next.
     */
    if (!start_worker_server(
            &worker,
            WORKER_SERVER_PORT))
    {
        printf("Worker TCP server failed.\n");
        return 1;
    }

    return 0;
}
