#include <stddef.h>
#include <stdlib.h>
#include <time.h>

#include "worker_registry.h"

int initialize_worker_registry(
    struct WorkerRegistry *registry,
    size_t capacity)
{
    if (registry == NULL || capacity == 0) {
        return 0;
    }

    registry->workers = calloc(capacity, sizeof(struct WorkerRegistryEntry));
    if (registry->workers == NULL) {
        registry->capacity = 0;
        registry->count = 0;
        return 0;
    }

    registry->capacity = capacity;
    registry->count = 0;

    return 1;
}

void destroy_worker_registry(
    struct WorkerRegistry *registry)
{
    if (registry == NULL) {
        return;
    }

    free(registry->workers);
    registry->workers = NULL;
    registry->capacity = 0;
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
    for (size_t i = 0; i < (size_t)registry->count; i++) {
        if (registry->workers[i].worker.id == worker->id) {
            registry->workers[i].worker = *worker;
            registry->workers[i].last_seen = time(NULL);
            registry->workers[i].status = WORKER_ACTIVE;
            /*
             * A re-registration is proof that the previous
             * process is gone, so it cannot still be running
             * any of the tasks the coordinator reserved for
             * it. The caller reaps those tasks before calling
             * here; resetting the reservation count keeps a
             * leaked reservation from stranding the new
             * process.
             */
            registry->workers[i].running_tasks = 0;
            return 1;
        }
    }

    /*
     * Do not exceed registry capacity.
     */
    if ((size_t)registry->count >= registry->capacity) {
        return 0;
    }

    /*
     * Add a new worker.
     */
    size_t index = (size_t)registry->count;

    registry->workers[index].worker = *worker;
    registry->workers[index].last_seen = time(NULL);
    registry->workers[index].status = WORKER_ACTIVE;
    registry->workers[index].running_tasks = 0;
    registry->workers[index].max_running_tasks = DEFAULT_WORKER_MAX_TASKS;
    registry->workers[index].epoch = 0;

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

    for (size_t i = 0; i < (size_t)registry->count; i++) {
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

    for (size_t i = 0; i < (size_t)registry->count; i++) {
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

    for (size_t i = 0; i < (size_t)registry->count; i++) {
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

int is_worker_live(
    struct WorkerRegistry *registry,
    int worker_id)
{
    if (registry == NULL || worker_id <= 0) {
        return 0;
    }

    for (size_t i = 0; i < (size_t)registry->count; i++) {
        if (registry->workers[i].worker.id == worker_id) {
            return registry->workers[i].status == WORKER_ACTIVE ? 1 : 0;
        }
    }

    return 0;
}

int worker_running_tasks(
    struct WorkerRegistry *registry,
    int worker_id)
{
    if (registry == NULL || worker_id <= 0) {
        return 0;
    }

    for (size_t i = 0; i < (size_t)registry->count; i++) {
        if (registry->workers[i].worker.id == worker_id) {
            return registry->workers[i].running_tasks;
        }
    }

    return 0;
}

int worker_max_running_tasks(
    struct WorkerRegistry *registry,
    int worker_id)
{
    if (registry == NULL || worker_id <= 0) {
        return DEFAULT_WORKER_MAX_TASKS;
    }

    for (size_t i = 0; i < (size_t)registry->count; i++) {
        if (registry->workers[i].worker.id == worker_id) {
            int cap = registry->workers[i].max_running_tasks;
            return cap > 0 ? cap : DEFAULT_WORKER_MAX_TASKS;
        }
    }

    return DEFAULT_WORKER_MAX_TASKS;
}

int worker_reserve_slot(
    struct WorkerRegistry *registry,
    int worker_id)
{
    if (registry == NULL || worker_id <= 0) {
        return 0;
    }

    for (size_t i = 0; i < (size_t)registry->count; i++) {
        if (registry->workers[i].worker.id == worker_id) {
            if (registry->workers[i].status != WORKER_ACTIVE) {
                return 0;
            }

            int cap = registry->workers[i].max_running_tasks;
            if (cap <= 0) {
                cap = DEFAULT_WORKER_MAX_TASKS;
            }

            if (registry->workers[i].running_tasks >= cap) {
                return 0;
            }

            registry->workers[i].running_tasks++;
            return 1;
        }
    }

    return 0;
}

int worker_release_slot(
    struct WorkerRegistry *registry,
    int worker_id)
{
    if (registry == NULL || worker_id <= 0) {
        return 0;
    }

    for (size_t i = 0; i < (size_t)registry->count; i++) {
        if (registry->workers[i].worker.id == worker_id) {
            if (registry->workers[i].running_tasks > 0) {
                registry->workers[i].running_tasks--;
            }
            return 1;
        }
    }

    return 0;
}

int worker_update_load(
    struct WorkerRegistry *registry,
    int worker_id,
    int running_tasks,
    int max_running_tasks)
{
    if (registry == NULL || worker_id <= 0) {
        return 0;
    }

    for (size_t i = 0; i < (size_t)registry->count; i++) {
        if (registry->workers[i].worker.id == worker_id) {
            /*
             * running_tasks is owned by the coordinator: it
             * counts slots reserved at dispatch and released
             * on result, timeout, or reap. A worker-reported
             * count describes its live children and can
             * legitimately lag a dispatch the worker has not
             * polled yet. Adopting the report would
             * oversubscribe the worker in that window (and,
             * before the agent reaped children, a stale
             * report could strand it forever). Only the
             * capacity and liveness come from a heartbeat.
             */
            (void)running_tasks;

            registry->workers[i].max_running_tasks =
                max_running_tasks > 0 ? max_running_tasks : DEFAULT_WORKER_MAX_TASKS;
            registry->workers[i].last_seen = time(NULL);
            return 1;
        }
    }

    return 0;
}

int worker_get_epoch(
    struct WorkerRegistry *registry,
    int worker_id)
{
    if (registry == NULL || worker_id <= 0) {
        return -1;
    }

    for (size_t i = 0; i < (size_t)registry->count; i++) {
        if (registry->workers[i].worker.id == worker_id) {
            return registry->workers[i].epoch;
        }
    }

    return -1;
}

int worker_increment_epoch(
    struct WorkerRegistry *registry,
    int worker_id)
{
    if (registry == NULL || worker_id <= 0) {
        return -1;
    }

    for (size_t i = 0; i < (size_t)registry->count; i++) {
        if (registry->workers[i].worker.id == worker_id) {
            registry->workers[i].epoch++;
            return registry->workers[i].epoch;
        }
    }

    return -1;
}