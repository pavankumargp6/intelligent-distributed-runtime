#include <stdio.h>

#include "runtime_stats.h"

int main(void)
{
    struct RuntimeStats stats;

    initialize_runtime_stats(&stats);

    printf("Initial statistics:\n");
    printf("Submitted : %d\n", stats.tasks_submitted);
    printf("Completed : %d\n", stats.tasks_completed);
    printf("Failed    : %d\n", stats.tasks_failed);

    record_task_submitted(&stats);
    record_task_submitted(&stats);
    record_task_submitted(&stats);

    record_task_completed(
        &stats,
        1.0,
        2.0
    );

    record_task_completed(
        &stats,
        3.0,
        4.0
    );

    record_task_failed(&stats);

    printf("\nFinal statistics:\n");
    printf("Submitted : %d\n", stats.tasks_submitted);
    printf("Completed : %d\n", stats.tasks_completed);
    printf("Failed    : %d\n", stats.tasks_failed);

    printf(
        "Average queue wait : %.2f seconds\n",
        get_average_queue_wait(&stats)
    );

    printf(
        "Average execution  : %.2f seconds\n",
        get_average_execution_time(&stats)
    );

    return 0;
}
