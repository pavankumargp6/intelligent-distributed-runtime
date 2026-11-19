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
#include "worker_registry.h"

#define RUNTIME_ADDRESS "127.0.0.1"
#define DEFAULT_AGENT_INTERVAL_SECONDS 0
#define RETRY_INTERVAL_SECONDS 3

/*
 * Configurable agent cycle interval. The default is 0, so the
 * worker spin-polls the coordinator for tasks as fast as the
 * event loop allows. Set IDR_AGENT_INTERVAL_SECONDS to a positive
 * value to throttle the poll rate (the scheduler fairness test uses
 * 1 s so two workers split a batch deterministically).
 */

static double parse_agent_interval(void);
static void sleep_seconds(double seconds);

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

    int finished = start_task(&task);

    if (!finished)
    {
        log_error("task_start_failed",
                  "[task %d] start_task failed.",
                  task.id);
    }
    else if (!wait_for_task(&task))
    {
        log_error("task_wait_failed",
                  "[task %d] wait_for_task failed.",
                  task.id);
        finished = 0;
    }

    /*
     * A task that could not be started or waited on is still a
     * task the coordinator has marked in-flight. Report a failed
     * result instead of exiting silently: otherwise the entry
     * stays in flight as long as this worker heartbeats, and the
     * client sees QUERY_PENDING forever. Both start_task() and
     * wait_for_task() set state and exit_status on failure; the
     * guards below only cover a partial update.
     */
    if (!finished && task.state != TASK_FAILED &&
        task.state != TASK_TIMED_OUT &&
        task.state != TASK_COMPLETED)
    {
        task.state = TASK_FAILED;
        task.exit_status = -1;
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

    /*
     * The worker agent forks one child per task and never waits
     * on them (SIGCHLD is SIG_IGN, so the kernel reaps them). The
     * running count is therefore the number of children that have
     * been forked but whose results have not yet been reported.
     * It is reported to the coordinator on every heartbeat so the
     * coordinator can enforce per-worker capacity.
     */
    int running_tasks = 0;

    int max_running_tasks = DEFAULT_WORKER_MAX_TASKS;

    const char *env_max = getenv("IDR_WORKER_MAX_TASKS");
    if (env_max != NULL && env_max[0] != '\0')
    {
        char *end = NULL;
        long parsed = strtol(env_max, &end, 10);
        if (end != env_max && end != NULL && *end == '\0' &&
            parsed > 0)
        {
            max_running_tasks = (int)parsed;
        }
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
    printf("Max tasks        : %d\n", max_running_tasks);
    printf("========================================\n");

    int registered = 0;

    while (1)
    {
        if (!registered)
        {
            log_info("worker_registering",
                     "Registering worker %d with runtime...",
                     worker.id);

            if (register_worker_with_runtime_with_load(&worker,
                                             RUNTIME_ADDRESS,
                                             running_tasks,
                                             max_running_tasks))
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

        if (!send_worker_heartbeat_with_load(&worker,
                                              RUNTIME_ADDRESS,
                                              running_tasks,
                                              max_running_tasks))
        {
            log_warn("heartbeat_failed",
                     "Heartbeat failed. Will retry on next cycle.");
            /* Do not re-register immediately: the failure may be
             * transient (auth replay on the ACK). The next loop
             * iteration will retry the heartbeat with a fresh
             * timestamp. */
            sleep_seconds(1.0);
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

        running_tasks++;

        pid_t child = fork();

        if (child < 0)
        {
            log_error("fork_failed", "fork: %s", strerror(errno));
            running_tasks--;
            continue;
        }

        if (child == 0)
        {
            run_task_in_child(&worker, &task);
        }
    }

    return 0;
}
