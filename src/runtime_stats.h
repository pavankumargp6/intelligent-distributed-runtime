#ifndef RUNTIME_STATS_H
#define RUNTIME_STATS_H

/*
 * Runtime-wide statistics.
 *
 * These values summarize the performance
 * of tasks executed by the runtime.
 */
struct RuntimeStats
{
    int tasks_submitted;
    int tasks_completed;
    int tasks_failed;

    double total_queue_wait_seconds;
    double total_execution_seconds;
};

/*
 * Initialize runtime statistics.
 */
void initialize_runtime_stats(struct RuntimeStats *stats);

/*
 * Record that a task was submitted.
 */
void record_task_submitted(struct RuntimeStats *stats);

/*
 * Record a completed task and its metrics.
 */
void record_task_completed(
    struct RuntimeStats *stats,
    double queue_wait_seconds,
    double execution_seconds
);

/*
 * Record a failed task.
 */
void record_task_failed(struct RuntimeStats *stats);

/*
 * Calculate average queue wait time.
 */
double get_average_queue_wait(
    const struct RuntimeStats *stats
);

/*
 * Calculate average execution time.
 */
double get_average_execution_time(
    const struct RuntimeStats *stats
);

#endif
