#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "worker.h"
#include "worker_registration_client.h"

#define RUNTIME_ADDRESS "127.0.0.1"
#define HEARTBEAT_INTERVAL_SECONDS 3
#define RETRY_INTERVAL_SECONDS 3

int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <worker_id>\n", argv[0]);
        return 1;
    }

    char *end = NULL;
    long parsed_id = strtol(argv[1], &end, 10);

    if (end == argv[1] || *end != '\0' ||
        parsed_id <= 0 || parsed_id > 2147483647L) {
        fprintf(stderr, "Worker ID must be a positive integer.\n");
        return 1;
    }

    struct Worker worker;

    if (!initialize_worker(&worker, (int)parsed_id)) {
        fprintf(stderr, "Failed to initialize worker.\n");
        return 1;
    }

    printf("\n========================================\n");
    printf("             WORKER AGENT\n");
    printf("========================================\n");
    printf("Worker ID        : %d\n", worker.id);
    printf("Total CPU        : %.2f%%\n",
           worker.resources.total_cpu_percent);
    printf("Available CPU    : %.2f%%\n",
           worker.resources.available_cpu_percent);
    printf("Total Memory     : %ld MB\n",
           worker.resources.total_memory_mb);
    printf("Available Memory : %ld MB\n",
           worker.resources.available_memory_mb);
    printf("========================================\n");

    int registered = 0;

    while (1) {
        if (!registered) {
            printf("Registering worker %d with runtime...\n",
                   worker.id);
            fflush(stdout);

            if (register_worker_with_runtime(
                    &worker, RUNTIME_ADDRESS)) {
                registered = 1;
                printf("Worker registered. Status: ACTIVE\n");
            } else {
                printf("Registration failed. Retrying in %d seconds...\n",
                       RETRY_INTERVAL_SECONDS);
                fflush(stdout);
                sleep(RETRY_INTERVAL_SECONDS);
                continue;
            }
        }

        sleep(HEARTBEAT_INTERVAL_SECONDS);

        printf("Sending heartbeat for worker %d...\n", worker.id);
        fflush(stdout);

        if (send_worker_heartbeat(&worker, RUNTIME_ADDRESS)) {
            printf("Heartbeat acknowledged.\n");
        } else {
            printf("Heartbeat failed. Will attempt registration again.\n");
            registered = 0;
        }

        fflush(stdout);
    }

    return 0;
}
