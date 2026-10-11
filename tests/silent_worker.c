/*
 * Silent worker stub.
 *
 * Behaves like a real worker except it never sends
 * TASK_ACCEPT after receiving a TASK_ASSIGN. Used by
 * dispatch_timeout_test.sh to trigger the coordinator's
 * dispatch-timeout reaper.
 *
 * Usage: ./silent_worker <worker_id>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#include "worker.h"
#include "worker_registration_client.h"
#include "worker_task_client.h"
#include "task.h"
#include "log.h"

#define RUNTIME_ADDRESS "127.0.0.1"
#define HEARTBEAT_INTERVAL_SECONDS 3
#define RETRY_INTERVAL_SECONDS 3

int main(int argc, char *argv[])
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    log_init();

    if (argc != 2)
    {
        fprintf(stderr, "Usage: %s <worker_id>\n", argv[0]);
        return 2;
    }

    int worker_id = atoi(argv[1]);

    if (worker_id <= 0)
    {
        fprintf(stderr, "worker_id must be positive\n");
        return 2;
    }

    signal(SIGCHLD, SIG_IGN);

    struct Worker worker;

    if (!initialize_worker(&worker, worker_id))
    {
        log_error("silent_init_failed", "Failed to init worker");
        return 1;
    }

    log_info("silent_worker_starting",
             "Silent worker %d starting (never sends TASK_ACCEPT)",
             worker_id);

    int registered = 0;

    while (1)
    {
        if (!registered)
        {
            if (register_worker_with_runtime(&worker, RUNTIME_ADDRESS))
            {
                registered = 1;
                log_info("silent_worker_registered", "Registered.");
            }
            else
            {
                sleep(RETRY_INTERVAL_SECONDS);
                continue;
            }
        }

        sleep(HEARTBEAT_INTERVAL_SECONDS);

        if (!send_worker_heartbeat(&worker, RUNTIME_ADDRESS))
        {
            log_warn("silent_heartbeat_failed",
                     "Heartbeat failed; will re-register.");
            registered = 0;
            continue;
        }

        struct Task task;
        int has_task = 0;

        if (!request_task_from_runtime(&worker,
                                       RUNTIME_ADDRESS,
                                       &task,
                                       &has_task))
        {
            continue;
        }

        if (!has_task)
        {
            continue;
        }

        /*
         * Deliberately do not send TASK_ACCEPT.
         * The coordinator's dispatch-timeout reaper will
         * decide what happens to this task.
         */
        log_info("silent_worker_got_task",
                 "Silent worker got task %d and will not accept it",
                 task.id);
    }

    return 0;
}
