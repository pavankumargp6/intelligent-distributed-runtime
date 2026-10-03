#ifndef WORKER_RESOURCES_H
#define WORKER_RESOURCES_H

#include "task.h"

/*
 * Resources available on a worker node.
 *
 * CPU capacity is represented as a percentage
 * of one CPU core.
 *
 * Memory values are represented in megabytes.
 */
struct WorkerResources
{
    double total_cpu_percent;
    double available_cpu_percent;

    long total_memory_mb;
    long available_memory_mb;
};

/*
 * Collect the current resources available
 * on this worker node.
 */
int collect_worker_resources(struct WorkerResources *resources);

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
                     const struct WorkerResources *resources);

#endif
