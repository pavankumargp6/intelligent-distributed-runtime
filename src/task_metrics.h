#ifndef TASK_METRICS_H
#define TASK_METRICS_H

/*
 * Timing information collected
 * during a task's lifecycle.
 */
struct TaskMetrics
{
    double submission_time;
    double start_time;
    double completion_time;

    double queue_wait_seconds;
    double execution_seconds;
};

/*
 * Return the current monotonic time
 * in seconds.
 */
double get_monotonic_time(void);

/*
 * Initialize task metrics.
 */
void initialize_task_metrics(struct TaskMetrics *metrics);

/*
 * Record task submission time.
 */
void record_task_submission(struct TaskMetrics *metrics);

/*
 * Record task start time.
 */
void record_task_start(struct TaskMetrics *metrics);

/*
 * Record task completion time and
 * calculate final timing values.
 */
void record_task_completion(struct TaskMetrics *metrics);

#endif
