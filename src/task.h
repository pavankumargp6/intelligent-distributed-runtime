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
    TASK_FAILED = 3,
    TASK_TIMED_OUT = 4,

    /*
     * The coordinator cannot prove whether the command ran.
     *
     * Used for non-idempotent tasks whose worker was lost or
     * whose dispatch was never acknowledged. Treating these as
     * FAILED would hide the possibility of duplicate side
     * effects if an operator blindly retried them.
     */
    TASK_UNKNOWN = 5
};

struct Task
{
    int id;

    char command[TASK_COMMAND_SIZE];

    int argument_count;
    char arguments[TASK_ARGUMENT_COUNT][TASK_ARGUMENT_SIZE];

    /*
     * Resource requirements for this task.
     *
     * CPU requirement is expressed as a percentage
     * of one CPU core.
     *
     * Memory requirement is expressed in megabytes.
     */
    double required_cpu_percent;
    long required_memory_mb;

    /*
     * ID of the worker selected to execute
     * this task.
     *
     * A value of -1 means that no worker
     * has been assigned yet.
     */
    int assigned_worker_id;

    /*
     * Timing and execution metrics
     * collected during the task lifecycle.
     */
    struct TaskMetrics metrics;

    enum TaskState state;

    /*
     * Whether the task can be safely retried on dispatch
     * timeout. 0 (default) means no: the coordinator will
     * fail the task rather than risk duplicate side effects.
     */
    int idempotent;

    /*
     * Maximum execution time in seconds, or 0 for no limit.
     *
     * Applies from the moment the worker starts the task
     * process. Queue wait and dispatch time do not count
     * against it.
     */
    int timeout_seconds;

    /*
     * Number of dispatch attempts made for this task.
     *
     * Starts at 0 when the task is submitted and is incremented
     * immediately before TASK_ASSIGN is written to the WAL.
     * Persisted in DISPATCH records and snapshots so the retry
     * limit survives coordinator restarts.
     */
    int attempt;

    int pid;
    int exit_status;
};

#endif
