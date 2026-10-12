#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "cpu_monitor.h"

/*
 * Read the process CPU tick counters from /proc/PID/stat.
 *
 * /proc/PID/stat layout (proc(5)):
 *
 *   pid (comm) state ppid pgrp session tty_nr tpgid flags
 *   minflt cminflt majflt cmajflt utime stime ...
 *
 * "comm" is wrapped in parens and may itself contain spaces
 * and ')' characters, so the field boundary cannot be found
 * with a naive fscanf. We locate the first '(' and the last
 * ')' instead, then sscanf the remaining space-separated
 * fields from a pointer just past the ')'.
 *
 * Returns 1 on success, 0 on any failure to read or parse.
 */
int read_cpu_ticks(int pid, long *total_ticks)
{
    if (total_ticks == NULL || pid <= 0)
    {
        return 0;
    }

    char path[256];

    snprintf(path, sizeof(path), "/proc/%d/stat", pid);

    FILE *file = fopen(path, "r");

    if (file == NULL)
    {
        return 0;
    }

    char line[1024];

    if (fgets(line, sizeof(line), file) == NULL)
    {
        fclose(file);
        return 0;
    }

    fclose(file);

    const char *open_paren = strchr(line, '(');
    const char *close_paren = strrchr(line, ')');

    if (open_paren == NULL ||
        close_paren == NULL ||
        close_paren < open_paren)
    {
        return 0;
    }

    /*
     * Everything after the last ')' is space-separated:
     *
     *   state ppid pgrp session tty_nr tpgid flags
     *   minflt cminflt majflt cmajflt utime stime ...
     *
     * We only care about utime and stime, but sscanf requires
     * us to consume the intervening fields in order.
     */
    const char *rest = close_paren + 1;

    char state;

    long ppid, pgrp, session, tty_nr, tpgid, flags;
    long minflt, cminflt, majflt, cmajflt;
    long utime, stime;

    int fields = sscanf(
        rest,
        " %c %ld %ld %ld %ld %ld %ld "
        "%ld %ld %ld %ld %ld %ld",
        &state,
        &ppid, &pgrp, &session, &tty_nr, &tpgid, &flags,
        &minflt, &cminflt, &majflt, &cmajflt,
        &utime, &stime
    );

    if (fields != 13)
    {
        return 0;
    }

    *total_ticks = utime + stime;

    return 1;
}

/*
 * Convert a delta of CPU ticks into a percentage over the
 * given elapsed wall-clock seconds.
 *
 * The tick rate is read from sysconf(_SC_CLK_TCK) rather than
 * assumed to be 100. USER_HZ is 100 on most Linux systems but
 * has been 1024 on some architectures.
 */
double calculate_cpu_usage(long start_ticks,
                           long end_ticks,
                           double elapsed_seconds)
{
    if (elapsed_seconds <= 0.0)
    {
        return 0.0;
    }

    long tick_difference = end_ticks - start_ticks;

    long hz = sysconf(_SC_CLK_TCK);

    if (hz <= 0)
    {
        hz = 100;
    }

    double cpu_seconds = (double)tick_difference / (double)hz;

    double cpu_percentage =
        (cpu_seconds / elapsed_seconds) * 100.0;

    return cpu_percentage;
}
