#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>

#include "task_executor.h"

int start_task(struct Task *task)
{
    if (task == NULL)
    {
        return 0;
    }

    /*
     * Build the argument list for execvp().
     *
     * Example:
     *
     * command = "sleep"
     * arguments[0] = "5"
     *
     * becomes:
     *
     * argv[0] = "sleep"
     * argv[1] = "5"
     * argv[2] = NULL
     */
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
        return 0;
    }

    if (pid == 0)
    {
        /*
         * Child process:
         * Replace this process with the requested command.
         */
        execvp(task->command, argv);

        /*
         * If execvp() returns, execution failed.
         */
        perror("execvp");
        _exit(127);
    }

    /*
     * Parent process:
     * Store the child's PID.
     */
    task->pid = pid;
    task->state = TASK_RUNNING;

    return 1;
}

int wait_for_task(struct Task *task)
{
    if (task == NULL)
    {
        return 0;
    }

    if (task->pid <= 0)
    {
        return 0;
    }

    /*
     * Wait for this specific child process.
     */
    int status;

    if (waitpid(task->pid, &status, 0) < 0)
    {
        task->state = TASK_FAILED;
        task->exit_status = -1;
        return 0;
    }

    /*
     * Convert the wait status into the actual exit code.
     */
    if (WIFEXITED(status))
    {
        task->exit_status = WEXITSTATUS(status);

        if (task->exit_status == 0)
        {
            task->state = TASK_COMPLETED;
        }
        else
        {
            task->state = TASK_FAILED;
        }
    }
    else
    {
        task->state = TASK_FAILED;
        task->exit_status = -1;
    }

    return 1;
}
