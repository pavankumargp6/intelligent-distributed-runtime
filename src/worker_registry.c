#include <stddef.h>
#include <time.h>

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
     * Store the worker.
     */
    registry->workers[registry->count].worker =
        *worker;

    /*
     * The worker has just registered,
     * so record the current time.
     */
    registry->workers[registry->count].last_seen =
        time(NULL);

    /*
     * A newly registered worker is active.
     */
    registry->workers[registry->count].status =
        WORKER_ACTIVE;

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
        if (registry->workers[i].worker.id ==
            worker_id)
        {
            return &registry->workers[i].worker;
        }
    }

    return NULL;
}

int update_worker_heartbeat(
    struct WorkerRegistry *registry,
    int worker_id)
{
    if (registry == NULL)
    {
        return 0;
    }

    for (int i = 0;
         i < registry->count;
         i++)
    {
        struct WorkerRegistryEntry *entry =
            &registry->workers[i];

        if (entry->worker.id == worker_id)
        {
            entry->last_seen = time(NULL);

            entry->status = WORKER_ACTIVE;

            return 1;
        }
    }

    return 0;
}

int mark_stale_workers(
    struct WorkerRegistry *registry)
{
    if (registry == NULL)
    {
        return 0;
    }

    time_t current_time = time(NULL);

    int stale_count = 0;

    for (int i = 0;
         i < registry->count;
         i++)
    {
        struct WorkerRegistryEntry *entry =
            &registry->workers[i];

        /*
         * Ignore workers that are already
         * marked unavailable.
         */
        if (entry->status ==
            WORKER_UNAVAILABLE)
        {
            continue;
        }

        /*
         * Check how long it has been
         * since the last heartbeat.
         */
        double elapsed =
            difftime(
                current_time,
                entry->last_seen
            );

        if (elapsed >
            WORKER_HEARTBEAT_TIMEOUT)
        {
            entry->status =
                WORKER_UNAVAILABLE;

            stale_count++;
        }
    }

    return stale_count;
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
