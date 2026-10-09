#include <stddef.h>
#include <time.h>

#include "worker_registry.h"

void initialize_worker_registry(
    struct WorkerRegistry *registry)
{
    if (registry == NULL) {
        return;
    }

    registry->count = 0;
}

int register_worker(
    struct WorkerRegistry *registry,
    struct Worker *worker)
{
    if (registry == NULL || worker == NULL ||
        worker->id <= 0) {
        return 0;
    }

    /*
     * If the worker already exists, refresh its
     * information instead of rejecting registration.
     */
    for (int i = 0; i < registry->count; i++) {
        if (registry->workers[i].worker.id == worker->id) {
            registry->workers[i].worker = *worker;
            registry->workers[i].last_seen = time(NULL);
            registry->workers[i].status = WORKER_ACTIVE;

            return 1;
        }
    }

    /*
     * Do not exceed registry capacity.
     */
    if (registry->count >= WORKER_REGISTRY_CAPACITY) {
        return 0;
    }

    /*
     * Add a new worker.
     */
    int index = registry->count;

    registry->workers[index].worker = *worker;
    registry->workers[index].last_seen = time(NULL);
    registry->workers[index].status = WORKER_ACTIVE;

    registry->count++;

    return 1;
}

struct Worker *find_registered_worker(
    struct WorkerRegistry *registry,
    int worker_id)
{
    if (registry == NULL || worker_id <= 0) {
        return NULL;
    }

    for (int i = 0; i < registry->count; i++) {
        if (registry->workers[i].worker.id == worker_id) {
            return &registry->workers[i].worker;
        }
    }

    return NULL;
}

int update_worker_heartbeat(
    struct WorkerRegistry *registry,
    int worker_id)
{
    if (registry == NULL || worker_id <= 0) {
        return 0;
    }

    for (int i = 0; i < registry->count; i++) {
        if (registry->workers[i].worker.id == worker_id) {
            registry->workers[i].last_seen = time(NULL);
            registry->workers[i].status = WORKER_ACTIVE;

            return 1;
        }
    }

    return 0;
}

int mark_stale_workers(
    struct WorkerRegistry *registry)
{
    if (registry == NULL) {
        return 0;
    }

    time_t current_time = time(NULL);
    int marked_unavailable = 0;

    for (int i = 0; i < registry->count; i++) {
        if (registry->workers[i].status != WORKER_ACTIVE) {
            continue;
        }

        double elapsed = difftime(
            current_time,
            registry->workers[i].last_seen);

        if (elapsed > WORKER_HEARTBEAT_TIMEOUT) {
            registry->workers[i].status = WORKER_UNAVAILABLE;
            marked_unavailable++;
        }
    }

    return marked_unavailable;
}

int get_registered_worker_count(
    struct WorkerRegistry *registry)
{
    if (registry == NULL) {
        return 0;
    }

    return registry->count;
}
