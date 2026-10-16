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
    int tasks_timed_out;

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
 * Record a task that hit its execution timeout.
 *
 * Kept separate from record_task_failed so operators can
 * distinguish "the command ran and returned non-zero" from
 * "the command was killed because it exceeded its deadline".
 */
void record_task_timed_out(struct RuntimeStats *stats);

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
