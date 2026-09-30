#ifndef TASK_H
#define TASK_H

#define TASK_COMMAND_SIZE 256
#define TASK_ARGUMENT_COUNT 16
#define TASK_ARGUMENT_SIZE 128

enum TaskState
{
    TASK_CREATED = 0,
    TASK_RUNNING = 1,
    TASK_COMPLETED = 2,
    TASK_FAILED = 3
};

struct Task
{
    int id;

    char command[TASK_COMMAND_SIZE];

    int argument_count;
    char arguments[TASK_ARGUMENT_COUNT][TASK_ARGUMENT_SIZE];

    enum TaskState state;

    int pid;
    int exit_status;
};

#endif
