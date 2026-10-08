#include <stddef.h>

#include "worker_registry.h"

void initialize_worker_registry(
    struct WorkerRegistry *registry)
{
    if (registry == NULL)
    {
        return;
    }

    registry->count = 0;
}

int register_worker(
    struct WorkerRegistry *registry,
    struct Worker *worker)
{
    if (registry == NULL ||
        worker == NULL)
    {
        return 0;
    }

    /*
     * Prevent duplicate worker IDs.
     */
    if (find_registered_worker(
            registry,
            worker->id) != NULL)
    {
        return 0;
    }

    /*
     * Prevent the registry from
     * exceeding its capacity.
     */
    if (registry->count >=
        WORKER_REGISTRY_CAPACITY)
    {
        return 0;
    }

    /*
     * Store a copy of the worker.
     */
    registry->workers[registry->count] =
        *worker;

    registry->count++;

    return 1;
}

struct Worker *find_registered_worker(
    struct WorkerRegistry *registry,
    int worker_id)
{
    if (registry == NULL)
    {
        return NULL;
    }

    for (int i = 0;
         i < registry->count;
         i++)
    {
        if (registry->workers[i].id ==
            worker_id)
        {
            return &registry->workers[i];
        }
    }

    return NULL;
}

int get_registered_worker_count(
    struct WorkerRegistry *registry)
{
    if (registry == NULL)
    {
        return 0;
    }

    return registry->count;
}
