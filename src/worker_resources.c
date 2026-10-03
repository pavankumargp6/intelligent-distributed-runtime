#include <stdio.h>
#include <unistd.h>

#include "worker_resources.h"
#include "task.h"

/*
 * Read system-wide CPU time counters from /proc/stat.
 *
 * total_ticks = total CPU time across all CPU states.
 * idle_ticks  = idle + iowait time.
 */
static int read_system_cpu_ticks(long *total_ticks,
                                 long *idle_ticks)
{
    if (total_ticks == NULL ||
        idle_ticks == NULL)
    {
        return 0;
    }

    FILE *file = fopen("/proc/stat", "r");

    if (file == NULL)
    {
        return 0;
    }

    char line[256];

    if (fgets(line, sizeof(line), file) == NULL)
    {
        fclose(file);
        return 0;
    }

    fclose(file);

    long user = 0;
    long nice = 0;
    long system = 0;
    long idle = 0;
    long iowait = 0;
    long irq = 0;
    long softirq = 0;
    long steal = 0;

    int fields = sscanf(
        line,
        "cpu %ld %ld %ld %ld %ld %ld %ld %ld",
        &user,
        &nice,
        &system,
        &idle,
        &iowait,
        &irq,
        &softirq,
        &steal
    );

    if (fields < 5)
    {
        return 0;
    }

    *total_ticks =
        user +
        nice +
        system +
        idle +
        iowait +
        irq +
        softirq +
        steal;

    *idle_ticks =
        idle + iowait;

    return 1;
}

/*
 * Collect the current resources available
 * on this worker node.
 */
int collect_worker_resources(struct WorkerResources *resources)
{
    if (resources == NULL)
    {
        return 0;
    }

    /*
     * Get the number of CPUs currently available.
     *
     * Each CPU contributes 100% capacity.
     */
    long cpu_count = sysconf(_SC_NPROCESSORS_ONLN);

    if (cpu_count <= 0)
    {
        return 0;
    }

    resources->total_cpu_percent =
        (double)cpu_count * 100.0;

    /*
     * Read memory information from /proc/meminfo.
     */
    FILE *file = fopen("/proc/meminfo", "r");

    if (file == NULL)
    {
        return 0;
    }

    char line[256];

    long total_memory_kb = 0;
    long available_memory_kb = 0;

    while (fgets(line, sizeof(line), file))
    {
        if (sscanf(line,
                   "MemTotal: %ld kB",
                   &total_memory_kb) == 1)
        {
            continue;
        }

        if (sscanf(line,
                   "MemAvailable: %ld kB",
                   &available_memory_kb) == 1)
        {
            continue;
        }
    }

    fclose(file);

    if (total_memory_kb <= 0 ||
        available_memory_kb < 0)
    {
        return 0;
    }

    /*
     * Convert kilobytes to megabytes.
     */
    resources->total_memory_mb =
        total_memory_kb / 1024;

    resources->available_memory_mb =
        available_memory_kb / 1024;

    /*
     * Take the first CPU measurement.
     */
    long start_total_ticks;
    long start_idle_ticks;

    if (!read_system_cpu_ticks(
            &start_total_ticks,
            &start_idle_ticks))
    {
        return 0;
    }

    /*
     * Wait one second so that CPU activity
     * can be measured.
     */
    sleep(1);

    /*
     * Take the second CPU measurement.
     */
    long end_total_ticks;
    long end_idle_ticks;

    if (!read_system_cpu_ticks(
            &end_total_ticks,
            &end_idle_ticks))
    {
        return 0;
    }

    long total_difference =
        end_total_ticks - start_total_ticks;

    long idle_difference =
        end_idle_ticks - start_idle_ticks;

    if (total_difference <= 0 ||
        idle_difference < 0)
    {
        return 0;
    }

    /*
     * Calculate CPU utilization across
     * the entire worker.
     */
    double cpu_utilization_percent =
        ((double)(total_difference - idle_difference)
         / (double)total_difference)
        * 100.0;

    /*
     * Convert utilization into our
     * multi-core percentage scale.
     *
     * Example:
     *
     * 8 CPUs = 800% total capacity
     * 25% overall utilization = 200% used
     */
    double used_cpu_percent =
        (cpu_utilization_percent / 100.0)
        * resources->total_cpu_percent;

    resources->available_cpu_percent =
        resources->total_cpu_percent
        - used_cpu_percent;

    /*
     * Protect against measurement errors.
     */
    if (resources->available_cpu_percent < 0.0)
    {
        resources->available_cpu_percent = 0.0;
    }

    if (resources->available_cpu_percent >
        resources->total_cpu_percent)
    {
        resources->available_cpu_percent =
            resources->total_cpu_percent;
    }

    return 1;
}

/*
 * Determine whether a task can currently
 * fit on this worker.
 *
 * Returns:
 *
 * 1 -> task fits
 * 0 -> task does not fit
 */
int task_fits_worker(const struct Task *task,
                     const struct WorkerResources *resources)
{
    if (task == NULL ||
        resources == NULL)
    {
        return 0;
    }

    /*
     * CPU requirement must fit within
     * currently available CPU capacity.
     */
    if (task->required_cpu_percent >
        resources->available_cpu_percent)
    {
        return 0;
    }

    /*
     * Memory requirement must fit within
     * currently available memory.
     */
    if (task->required_memory_mb >
        resources->available_memory_mb)
    {
        return 0;
    }

    return 1;
}
