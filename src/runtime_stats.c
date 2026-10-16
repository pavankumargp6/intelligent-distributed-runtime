#include "runtime_stats.h"

void initialize_runtime_stats(struct RuntimeStats *stats)
{
    if (stats == 0)
    {
        return;
    }

    stats->tasks_submitted = 0;
    stats->tasks_completed = 0;
    stats->tasks_failed = 0;
    stats->tasks_timed_out = 0;

    stats->total_queue_wait_seconds = 0.0;
    stats->total_execution_seconds = 0.0;
}

void record_task_submitted(struct RuntimeStats *stats)
{
    if (stats == 0)
    {
        return;
    }

    stats->tasks_submitted++;
}

void record_task_completed(
    struct RuntimeStats *stats,
    double queue_wait_seconds,
    double execution_seconds)
{
    if (stats == 0)
    {
        return;
    }

    stats->tasks_completed++;

    stats->total_queue_wait_seconds +=
        queue_wait_seconds;

    stats->total_execution_seconds +=
        execution_seconds;
}

void record_task_failed(struct RuntimeStats *stats)
{
    if (stats == 0)
    {
        return;
    }

    stats->tasks_failed++;
}

void record_task_timed_out(struct RuntimeStats *stats)
{
    if (stats == 0)
    {
        return;
    }

    stats->tasks_timed_out++;
}

double get_average_queue_wait(
    const struct RuntimeStats *stats)
{
    if (stats == 0 ||
        stats->tasks_completed == 0)
    {
        return 0.0;
    }

    return stats->total_queue_wait_seconds /
           stats->tasks_completed;
}

double get_average_execution_time(
    const struct RuntimeStats *stats)
{
    if (stats == 0 ||
        stats->tasks_completed == 0)
    {
        return 0.0;
    }

    return stats->total_execution_seconds /
           stats->tasks_completed;
}
