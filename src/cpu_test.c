#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "cpu_monitor.h"

double elapsed_seconds(struct timespec start, struct timespec end)
{
    return (double)(end.tv_sec - start.tv_sec)
         + (double)(end.tv_nsec - start.tv_nsec) / 1000000000.0;
}

int main(int argc, char *argv[])
{
    if (argc != 2)
    {
        printf("Usage: %s <PID>\n", argv[0]);
        return 1;
    }

    int pid = atoi(argv[1]);

    long start_ticks;
    long end_ticks;

    struct timespec start_time;
    struct timespec end_time;

    if (!read_cpu_ticks(pid, &start_ticks))
    {
        printf("Could not read PID %d.\n", pid);
        return 1;
    }

    clock_gettime(CLOCK_MONOTONIC, &start_time);

    struct timespec delay;
    delay.tv_sec = 1;
    delay.tv_nsec = 0;

    nanosleep(&delay, NULL);

    clock_gettime(CLOCK_MONOTONIC, &end_time);

    if (!read_cpu_ticks(pid, &end_ticks))
    {
        printf("Process %d no longer exists.\n", pid);
        return 1;
    }

    long tick_difference = end_ticks - start_ticks;

    double cpu_seconds = (double)tick_difference / 100.0;

    double wall_seconds =
        elapsed_seconds(start_time, end_time);

    double cpu_percentage =
        calculate_cpu_usage(
            start_ticks,
            end_ticks,
            wall_seconds
        );

    printf("Initial CPU ticks : %ld\n", start_ticks);
    printf("Final CPU ticks   : %ld\n", end_ticks);
    printf("CPU ticks used    : %ld\n", tick_difference);
    printf("CPU time used     : %.2f seconds\n", cpu_seconds);
    printf("Elapsed time      : %.6f seconds\n", wall_seconds);
    printf("CPU usage         : %.2f%%\n", cpu_percentage);

    return 0;
}
