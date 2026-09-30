#include <stdio.h>
#include <time.h>

#include "resource_monitor.h"
#include "cpu_monitor.h"

int collect_resource_snapshot(int pid,
                              struct ResourceSnapshot *snapshot)
{
    if (snapshot == NULL)
    {
        return 0;
    }

    /*
     * First collect normal process information.
     */
    if (!read_process_info(pid, &snapshot->process))
    {
        return 0;
    }

    /*
     * Take the first CPU measurement.
     */
    long start_ticks;

    if (!read_cpu_ticks(pid, &start_ticks))
    {
        return 0;
    }

    struct timespec start_time;
    struct timespec end_time;

    clock_gettime(CLOCK_MONOTONIC, &start_time);

    /*
     * Wait approximately one second.
     * The actual elapsed time is measured using
     * CLOCK_MONOTONIC.
     */
    struct timespec delay;
    delay.tv_sec = 1;
    delay.tv_nsec = 0;

    nanosleep(&delay, NULL);

    /*
     * Take the second CPU measurement.
     */
    long end_ticks;

    if (!read_cpu_ticks(pid, &end_ticks))
    {
        return 0;
    }

    clock_gettime(CLOCK_MONOTONIC, &end_time);

    double elapsed_seconds =
        (double)(end_time.tv_sec - start_time.tv_sec)
        + (double)(end_time.tv_nsec - start_time.tv_nsec)
          / 1000000000.0;

    snapshot->cpu_usage =
        calculate_cpu_usage(
            start_ticks,
            end_ticks,
            elapsed_seconds
        );

    return 1;
}
