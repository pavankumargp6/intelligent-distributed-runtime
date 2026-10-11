#ifndef TASK_H
#define TASK_H

#include "task_metrics.h"

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

    double required_cpu_percent;
    long required_memory_mb;

    int assigned_worker_id;

    struct TaskMetrics metrics;

    enum TaskState state;

    /*
     * Whether the task can be safely retried on dispatch
     * timeout. 0 (default) means no: the coordinator will
     * fail the task rather than risk duplicate side effects.
     */
    int idempotent;

    int pid;
    int exit_status;
};
#endif
