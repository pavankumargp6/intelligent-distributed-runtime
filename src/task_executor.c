#define _GNU_SOURCE
#include <sched.h>
#include <sys/mount.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
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

/*
 * Terminate the task process and its entire process group, then
 * block until the direct child is reaped.
 *
 * Safe to call whether the child is still running, already exited,
 * or its process group has vanished. Idempotent: task->pid is set
 * to -1 so a second call returns immediately.
 */
static void terminate_and_reap(struct Task *task)
{
    if (task == NULL || task->pid <= 0)
    {
        return;
    }

    /*
     * The child called setpgid(0, 0) before exec, so its pgid
     * equals its pid. Killing the negative pid signals the whole
     * group: the direct child plus any descendants it spawned.
     *
     * If the child failed to setpgid (e.g. exec raced ahead), no
     * process group exists with pgid == child pid, so kill(-pid)
     * returns ESRCH and we fall back to killing the direct child
     * only. That is the pre-existing behavior and is safe.
     */
    if (kill(-task->pid, SIGKILL) < 0 && errno != ESRCH)
    {
        /* Best-effort fallback: kill the direct child. */
        (void)kill(task->pid, SIGKILL);
    }

    int status = 0;
    pid_t result;

    do
    {
        result = waitpid(task->pid, &status, 0);
    } while (result < 0 && errno == EINTR);

    task->pid = -1;
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

    /*
     * A negative timeout must not reach fork(). parse_submit()
     * rejects it at the coordinator, but we defend here too so
     * that no code path can spawn a child that wait_for_task()
     * then refuses to wait on.
     */
    if (task->timeout_seconds < 0)
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
        /*
         * Put the child in its own process group so the parent
         * can kill the entire subtree on timeout.
         */
        (void)setpgid(0, 0);

        /*
         * Mini-Docker Sandbox (Phase 7)
         * Only active if IDR_SANDBOX_DIR is set.
         */
        const char *sandbox = getenv("IDR_SANDBOX_DIR");

        if (sandbox != NULL && sandbox[0] != '\0')
        {
            if (unshare(CLONE_NEWUSER | CLONE_NEWUTS | CLONE_NEWNS) != 0)
            {
                perror("unshare failed");
                _exit(125);
            }

            /* Disable setgroups before writing the GID map. */
            FILE *f = fopen("/proc/self/setgroups", "w");
            if (f != NULL)
            {
                int setgroups_write_result = fprintf(f, "deny");
                int setgroups_close_result = fclose(f);
                if (setgroups_write_result < 0 || setgroups_close_result != 0)
                {
                    perror("setgroups configuration failed");
                    _exit(125);
                }
            }
            else if (errno != ENOENT)
            {
                perror("opening /proc/self/setgroups failed");
                _exit(125);
            }

            char map_path[64];

            if (snprintf(map_path, sizeof(map_path),
                         "/proc/self/uid_map") >= (int)sizeof(map_path))
            {
                _exit(125);
            }

            f = fopen(map_path, "w");
            if (f == NULL)
            {
                perror("opening uid_map failed");
                _exit(125);
            }

            int uid_map_write_result = fprintf(f, "0 %d 1", getuid());
            int uid_map_close_result = fclose(f);
            if (uid_map_write_result < 0 || uid_map_close_result != 0)
            {
                perror("writing uid_map failed");
                _exit(125);
            }

            if (snprintf(map_path, sizeof(map_path),
                         "/proc/self/gid_map") >= (int)sizeof(map_path))
            {
                _exit(125);
            }

            f = fopen(map_path, "w");
            if (f == NULL)
            {
                perror("opening gid_map failed");
                _exit(125);
            }

            int gid_map_write_result = fprintf(f, "0 %d 1", getgid());
            int gid_map_close_result = fclose(f);
            if (gid_map_write_result < 0 || gid_map_close_result != 0)
            {
                perror("writing gid_map failed");
                _exit(125);
            }

            if (sethostname("idr-sandbox", 11) != 0)
            {
                perror("sethostname failed");
                _exit(125);
            }

            /* Make mounts private so they don't affect the host. */
            if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0)
            {
                perror("making mounts private failed");
                _exit(125);
            }

            if (chroot(sandbox) != 0)
            {
                perror("chroot failed");
                _exit(126);
            }

            if (chdir("/") != 0)
            {
                perror("chdir after chroot failed");
                _exit(126);
            }
        }
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
    if (task == NULL)
    {
        return 0;
    }

    if (task->pid <= 0)
    {
        task->state = TASK_FAILED;
        task->exit_status = -1;
        return 0;
    }

    if (task->timeout_seconds < 0)
    {
        terminate_and_reap(task);
        task->state = TASK_FAILED;
        task->exit_status = -1;
        return 0;
    }

    struct timespec start;
    struct timespec now;

    if (!monotonic_now(&start))
    {
        terminate_and_reap(task);
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

            task->pid = -1;
            return 1;
        }

        if (result < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }

            terminate_and_reap(task);
            task->state = TASK_FAILED;
            task->exit_status = -1;
            return 0;
        }

        if (task->timeout_seconds > 0)
        {
            if (!monotonic_now(&now))
            {
                terminate_and_reap(task);
                task->state = TASK_FAILED;
                task->exit_status = -1;
                return 0;
            }

            if (elapsed_seconds(&start, &now) >=
                (double)task->timeout_seconds)
            {
                terminate_and_reap(task);
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
                terminate_and_reap(task);
                task->state = TASK_FAILED;
                task->exit_status = -1;
                return 0;
            }
        }
    }
}
