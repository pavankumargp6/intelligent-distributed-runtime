#include <stdio.h>
#include "cpu_monitor.h"

int read_cpu_ticks(int pid, long *total_ticks)
{
    char path[256];

    snprintf(path, sizeof(path), "/proc/%d/stat", pid);

    FILE *file = fopen(path, "r");

    if (file == NULL)
    {
        return 0;
    }

    int process_pid;
    char name[256];
    char state;

    long value;
    long utime;
    long stime;

    if (fscanf(file, "%d %255s %c", &process_pid, name, &state) != 3)
    {
        fclose(file);
        return 0;
    }

    for (int i = 4; i <= 13; i++)
    {
        if (fscanf(file, "%ld", &value) != 1)
        {
            fclose(file);
            return 0;
        }
    }

    if (fscanf(file, "%ld %ld", &utime, &stime) != 2)
    {
        fclose(file);
        return 0;
    }

    fclose(file);

    *total_ticks = utime + stime;

    return 1;
}

double calculate_cpu_usage(long start_ticks,
                           long end_ticks,
                           double elapsed_seconds)
{
    if (elapsed_seconds <= 0.0)
    {
        return 0.0;
    }

    long tick_difference = end_ticks - start_ticks;

    double cpu_seconds = (double)tick_difference / 100.0;

    double cpu_percentage =
        (cpu_seconds / elapsed_seconds) * 100.0;

    return cpu_percentage;
}
