#include <stddef.h>

#include "worker.h"

/*
 * Initialize a worker.
 *
 * This assigns the worker ID and
 * collects its initial resources.
 */
int initialize_worker(struct Worker *worker,
                      int id)
{
    if (worker == NULL)
    {
        return 0;
    }

    worker->id = id;

    /*
     * Collect the worker's current
     * CPU and memory information.
     */
    if (!collect_worker_resources(&worker->resources))
    {
        return 0;
    }

    return 1;
}

/*
 * Refresh the worker's resource information.
 */
int refresh_worker_resources(struct Worker *worker)
{
    if (worker == NULL)
    {
        return 0;
    }

    return collect_worker_resources(&worker->resources);
}

