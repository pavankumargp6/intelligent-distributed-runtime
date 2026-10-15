
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "task_executor.h"

#define TASK_POLL_INTERVAL_NS 10000000L

static int monotonic_now(struct timespec *now)
{
    return clock_gettime(CLOCK_MONOTONIC, now) == 0;
}

static double elapsed_seconds(const struct timespec *start,
                              const struct timespec *now)
{
    return (double)(now->tv_sec - start->tv_sec) +
           (double)(now->tv_nsec - start->tv_nsec) / 1000000000.0;
}

int start_task(struct Task *task)
{
    if (task == NULL)
    {
        return 0;
    }

    if (task->argument_count < 0 ||
        task->argument_count > TASK_ARGUMENT_COUNT)
    {
        task->state = TASK_FAILED;
        task->exit_status = -1;
        return 0;
    }

    char *argv[TASK_ARGUMENT_COUNT + 2];

    argv[0] = task->command;

    for (int i = 0; i < task->argument_count; i++)
    {
        argv[i + 1] = task->arguments[i];
    }

    argv[task->argument_count + 1] = NULL;

    pid_t pid = fork();

    if (pid < 0)
    {
        task->state = TASK_FAILED;
        task->exit_status = -1;
        return 0;
    }

    if (pid == 0)
    {
        execvp(task->command, argv);
        perror("execvp");
        _exit(127);
    }

    task->pid = pid;
    task->state = TASK_RUNNING;

    return 1;
}

int start_assigned_task(struct Task *task)
{
    if (task == NULL)
    {
        return 0;
    }

    if (task->state != TASK_CREATED ||
        task->assigned_worker_id < 0)
    {
        return 0;
    }

    return start_task(task);
}

int wait_for_task(struct Task *task)
{
    if (task == NULL || task->pid <= 0)
    {
        return 0;
    }

    if (task->timeout_seconds < 0)
    {
        task->state = TASK_FAILED;
        task->exit_status = -1;
        return 0;
    }

    struct timespec start;
    struct timespec now;

    if (!monotonic_now(&start))
    {
        task->state = TASK_FAILED;
        task->exit_status = -1;
        return 0;
    }

    for (;;)
    {
        int status = 0;
        pid_t result = waitpid(task->pid, &status, WNOHANG);

        if (result == task->pid)
        {
            if (WIFEXITED(status))
            {
                task->exit_status = WEXITSTATUS(status);
                task->state = (task->exit_status == 0)
                                  ? TASK_COMPLETED
                                  : TASK_FAILED;
            }
            else
            {
                task->state = TASK_FAILED;
                task->exit_status = -1;
            }

            return 1;
        }

        if (result < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }

            task->state = TASK_FAILED;
            task->exit_status = -1;
            return 0;
        }

        if (task->timeout_seconds > 0)
        {
            if (!monotonic_now(&now))
            {
                task->state = TASK_FAILED;
                task->exit_status = -1;
                return 0;
            }

            if (elapsed_seconds(&start, &now) >=
                (double)task->timeout_seconds)
            {
                /*
                 * Timeout reached. Terminate the task process
                 * and reap it so it does not remain a zombie.
                 */
                if (kill(task->pid, SIGKILL) < 0 && errno != ESRCH)
                {
                    task->state = TASK_FAILED;
                    task->exit_status = -1;
                    return 0;
                }

                do
                {
                    result = waitpid(task->pid, &status, 0);
                } while (result < 0 && errno == EINTR);

                if (result != task->pid)
                {
                    task->state = TASK_FAILED;
                    task->exit_status = -1;
                    return 0;
                }

                task->state = TASK_TIMED_OUT;
                task->exit_status = -1;
                return 1;
            }
        }

        struct timespec pause = {
            .tv_sec = 0,
            .tv_nsec = TASK_POLL_INTERVAL_NS
        };

        while (nanosleep(&pause, &pause) < 0)
        {
            if (errno != EINTR)
            {
                task->state = TASK_FAILED;
                task->exit_status = -1;
                return 0;
            }
        }
    }
}
