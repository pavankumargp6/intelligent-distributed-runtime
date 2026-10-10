#include <stdio.h>
#include <string.h>

#include "task_executor.h"

int main(void)
{
    struct Task task = {0};

    task.id = 1;

    strcpy(task.command, "sleep");

    task.argument_count = 1;
    strcpy(task.arguments[0], "3");

    printf("Starting task %d...\n", task.id);
    printf("Command      : %s %s\n",
           task.command,
           task.arguments[0]);

    if (!start_task(&task))
    {
        printf("Could not start task.\n");
        return 1;
    }

    printf("\n");
    printf("Task started successfully.\n");
    printf("PID          : %d\n", task.pid);
    printf("State        : %d\n", task.state);

    printf("\nWaiting for task to finish...\n");

    if (!wait_for_task(&task))
    {
        printf("Could not wait for task.\n");
        return 1;
    }

    printf("\n");
    printf("================================\n");
    printf("        TASK RESULT\n");
    printf("================================\n");
    printf("Task ID      : %d\n", task.id);
    printf("PID          : %d\n", task.pid);
    printf("State        : %d\n", task.state);
    printf("Exit status  : %d\n", task.exit_status);
    printf("================================\n");

    return 0;
}
