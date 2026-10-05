#include <time.h>

#include "task_metrics.h"

/*
 * Return the current monotonic time
 * in seconds.
 *
 * CLOCK_MONOTONIC is used because it
 * does not jump if the system clock changes.
 */
double get_monotonic_time(void)
{
    struct timespec time_value;

    clock_gettime(CLOCK_MONOTONIC, &time_value);

    return (double)time_value.tv_sec
           + (double)time_value.tv_nsec / 1000000000.0;
}

/*
 * Initialize task metrics.
 */
void initialize_task_metrics(struct TaskMetrics *metrics)
{
    if (metrics == NULL)
    {
        return;
    }

    metrics->submission_time = 0.0;
    metrics->start_time = 0.0;
    metrics->completion_time = 0.0;

    metrics->queue_wait_seconds = 0.0;
    metrics->execution_seconds = 0.0;
}

/*
 * Record the moment the task
 * enters the runtime queue.
 */
void record_task_submission(struct TaskMetrics *metrics)
{
    if (metrics == NULL)
    {
        return;
    }

    metrics->submission_time =
        get_monotonic_time();
}

/*
 * Record the moment execution begins.
 */
void record_task_start(struct TaskMetrics *metrics)
{
    if (metrics == NULL)
    {
        return;
    }

    metrics->start_time =
        get_monotonic_time();

    metrics->queue_wait_seconds =
        metrics->start_time
        - metrics->submission_time;
}

/*
 * Record the moment execution finishes
 * and calculate execution duration.
 */
void record_task_completion(struct TaskMetrics *metrics)
{
    if (metrics == NULL)
    {
        return;
    }

    metrics->completion_time =
        get_monotonic_time();

    metrics->execution_seconds =
        metrics->completion_time
        - metrics->start_time;
}

