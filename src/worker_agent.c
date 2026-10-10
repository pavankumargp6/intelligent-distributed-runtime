#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>

#include "worker.h"
#include "worker_registration_client.h"
#include "worker_task_client.h"
#include "task.h"
#include "task_executor.h"

#define RUNTIME_ADDRESS "127.0.0.1"
#define HEARTBEAT_INTERVAL_SECONDS 3
#define RETRY_INTERVAL_SECONDS 3

static void run_task_in_child(const struct Worker *worker,
                              const struct Task *assigned)
{
    /*
     * Child process: execute the task locally and
     * report the result to the coordinator.
     *
     * We work on a local copy so we do not touch the
     * parent's memory (which is a separate address
     * space anyway) and so we can mutate the Task
     * freely while calling the executor.
     */
    signal(SIGCHLD, SIG_DFL);
    struct Task task = *assigned;

    if (!start_task(&task))
    {
        printf("[task %d] start_task failed.\n", task.id);
        fflush(stdout);
        _exit(1);
    }

    if (!wait_for_task(&task))
    {
        printf("[task %d] wait_for_task failed.\n", task.id);
        fflush(stdout);
        _exit(1);
    }

    printf("[task %d] finished: pid=%d exit_status=%d state=%d\n",
           task.id,
           task.pid,
           task.exit_status,
           task.state);
    fflush(stdout);

    if (!send_task_result_to_runtime(worker,
                                     RUNTIME_ADDRESS,
                                     &task))
    {
        printf("[task %d] could not report result to coordinator.\n",
               task.id);
        fflush(stdout);
        _exit(1);
    }

    printf("[task %d] result reported.\n", task.id);
    fflush(stdout);

    _exit(0);
}

int main(int argc, char *argv[])
{
    if (argc != 2)
    {
        fprintf(stderr, "Usage: %s <worker_id>\n", argv[0]);
        return 1;
    }

    char *end = NULL;
    long parsed_id = strtol(argv[1], &end, 10);

    if (end == argv[1] || *end != '\0' ||
        parsed_id <= 0 || parsed_id > 2147483647L)
    {
        fprintf(stderr, "Worker ID must be a positive integer.\n");
        return 1;
    }

    /*
     * Children are auto-reaped by the kernel.
     * The parent never blocks in waitpid, so
     * heartbeats continue while tasks run.
     */
    signal(SIGCHLD, SIG_IGN);

    struct Worker worker;

    if (!initialize_worker(&worker, (int)parsed_id))
    {
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

    while (1)
    {
        if (!registered)
        {
            printf("Registering worker %d with runtime...\n",
                   worker.id);
            fflush(stdout);

            if (register_worker_with_runtime(&worker,
                                             RUNTIME_ADDRESS))
            {
                registered = 1;
                printf("Worker registered. Status: ACTIVE\n");
            }
            else
            {
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

        if (!send_worker_heartbeat(&worker, RUNTIME_ADDRESS))
        {
            printf("Heartbeat failed. Will attempt registration again.\n");
            fflush(stdout);
            registered = 0;
            continue;
        }

        printf("Heartbeat acknowledged.\n");
        fflush(stdout);

        /*
         * Poll the coordinator for work.
         */
        struct Task task;
        int has_task = 0;

        printf("Requesting task from runtime...\n");
        fflush(stdout);

        if (!request_task_from_runtime(&worker,
                                       RUNTIME_ADDRESS,
                                       &task,
                                       &has_task))
        {
            printf("Task request failed.\n");
            fflush(stdout);
            continue;
        }

        if (!has_task)
        {
            printf("No task available.\n");
            fflush(stdout);
            continue;
        }

        printf("Task received: id=%d command=%s argc=%d\n",
               task.id,
               task.command,
               task.argument_count);
        fflush(stdout);

        pid_t child = fork();

        if (child < 0)
        {
            perror("fork");
            continue;
        }

        if (child == 0)
        {
            run_task_in_child(&worker, &task);
            /* run_task_in_child never returns */
        }

        /*
         * Parent returns to the loop; the child
         * will report the result when it finishes.
         */
    }

    return 0;
}
