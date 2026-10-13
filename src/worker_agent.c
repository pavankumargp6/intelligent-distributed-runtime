#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>

#include "worker.h"
#include "worker_registration_client.h"
#include "worker_task_client.h"
#include "task.h"
#include "task_executor.h"
#include "log.h"

#define RUNTIME_ADDRESS "127.0.0.1"
#define DEFAULT_AGENT_INTERVAL_SECONDS 3
#define RETRY_INTERVAL_SECONDS 3

/*
 * Configurable agent cycle interval. Defaults to 3 seconds,
 * matching the original hardcoded value. Setting the
 * IDR_AGENT_INTERVAL_SECONDS environment variable to 0 (or a
 * fractional value) reduces the delay between task poll
 * cycles.
 *
 * This is a benchmark affordance: it lets the performance
 * harness observe the runtime's actual per-task overhead
 * instead of waiting on the default 3-second idle timer. It
 * does not change the worker's architecture.
 *
 * Values outside [0, 3600] are rejected with a warning and the
 * default is used.
 */
static double parse_agent_interval(void)
{
    const char *env = getenv("IDR_AGENT_INTERVAL_SECONDS");

    if (env == NULL || env[0] == '\0')
    {
        return (double)DEFAULT_AGENT_INTERVAL_SECONDS;
    }

    char *end = NULL;
    double parsed = strtod(env, &end);

    if (end == env || *end != '\0' ||
        parsed < 0.0 || parsed > 3600.0)
    {
        fprintf(stderr,
                "warning: IDR_AGENT_INTERVAL_SECONDS=%s is invalid; "
                "using default %d\n",
                env,
                DEFAULT_AGENT_INTERVAL_SECONDS);
        return (double)DEFAULT_AGENT_INTERVAL_SECONDS;
    }

    return parsed;
}

static void sleep_seconds(double seconds)
{
    if (seconds <= 0.0)
    {
        return;
    }

    struct timespec ts;
    ts.tv_sec = (time_t)seconds;
    ts.tv_nsec = (long)((seconds - (double)ts.tv_sec) * 1e9);

    if (ts.tv_nsec < 0)
    {
        ts.tv_nsec = 0;
    }

    struct timespec rem = {0};

    while (nanosleep(&ts, &rem) != 0)
    {
        if (errno != EINTR)
        {
            break;
        }
        ts = rem;
    }
}

static void run_task_in_child(const struct Worker *worker,
                              const struct Task *assigned)
{
    signal(SIGCHLD, SIG_DFL);
    struct Task task = *assigned;

    if (!start_task(&task))
    {
        log_error("task_start_failed",
                  "[task %d] start_task failed.",
                  task.id);
        _exit(1);
    }

    if (!wait_for_task(&task))
    {
        log_error("task_wait_failed",
                  "[task %d] wait_for_task failed.",
                  task.id);
        _exit(1);
    }

    log_info("task_finished",
             "[task %d] finished: pid=%d exit_status=%d state=%d",
             task.id,
             task.pid,
             task.exit_status,
             task.state);

    if (!send_task_result_to_runtime(worker,
                                     RUNTIME_ADDRESS,
                                     &task))
    {
        log_error("task_result_report_failed",
                  "[task %d] could not report result to coordinator.",
                  task.id);
        _exit(1);
    }

    log_info("task_result_reported",
             "[task %d] result reported.",
             task.id);

    _exit(0);
}

int main(int argc, char *argv[])
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    log_init();

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

    signal(SIGCHLD, SIG_IGN);

    struct Worker worker;

    if (!initialize_worker(&worker, (int)parsed_id))
    {
        log_error("worker_init_failed", "Failed to initialize worker.");
        return 1;
    }

    double agent_interval = parse_agent_interval();

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
    printf("Cycle interval   : %.3f s\n", agent_interval);
    printf("========================================\n");

    int registered = 0;

    while (1)
    {
        if (!registered)
        {
            log_info("worker_registering",
                     "Registering worker %d with runtime...",
                     worker.id);

            if (register_worker_with_runtime(&worker,
                                             RUNTIME_ADDRESS))
            {
                registered = 1;
                log_info("worker_registered",
                         "Worker registered. Status: ACTIVE");
            }
            else
            {
                log_warn("worker_registration_failed",
                         "Registration failed. Retrying in %d seconds...",
                         RETRY_INTERVAL_SECONDS);
                sleep(RETRY_INTERVAL_SECONDS);
                continue;
            }
        }

        sleep_seconds(agent_interval);

        log_debug("heartbeat_sending",
                  "Sending heartbeat for worker %d...",
                  worker.id);

        if (!send_worker_heartbeat(&worker, RUNTIME_ADDRESS))
        {
            log_warn("heartbeat_failed",
                     "Heartbeat failed. Will attempt registration again.");
            registered = 0;
            continue;
        }

        log_debug("heartbeat_acked", "Heartbeat acknowledged.");

        struct Task task;
        int has_task = 0;

        log_debug("task_requesting", "Requesting task from runtime...");

        if (!request_task_from_runtime(&worker,
                                       RUNTIME_ADDRESS,
                                       &task,
                                       &has_task))
        {
            log_warn("task_request_failed", "Task request failed.");
            continue;
        }

        if (!has_task)
        {
            log_debug("no_task", "No task available.");
            continue;
        }

        log_info("task_received",
                 "Task received: id=%d command=%s argc=%d",
                 task.id,
                 task.command,
                 task.argument_count);

        if (!send_task_accept_to_runtime(&worker,
                                         RUNTIME_ADDRESS,
                                         task.id))
        {
            log_warn("task_accept_failed",
                     "Could not accept task %d; discarding.",
                     task.id);
            continue;
        }

        pid_t child = fork();

        if (child < 0)
        {
            log_error("fork_failed", "fork: %s", strerror(errno));
            continue;
        }

        if (child == 0)
        {
            run_task_in_child(&worker, &task);
        }
    }

    return 0;
}
