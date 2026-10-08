#ifndef WORKER_REGISTRY_H
#define WORKER_REGISTRY_H

#include <time.h>

#include "worker.h"

#define WORKER_REGISTRY_CAPACITY 32

/*
 * A worker is considered unavailable if
 * the runtime has not heard from it for
 * this many seconds.
 */
#define WORKER_HEARTBEAT_TIMEOUT 10

/*
 * Current state of a registered worker.
 */
enum WorkerStatus
{
    WORKER_ACTIVE = 0,
    WORKER_UNAVAILABLE = 1
};

/*
 * Stores a worker together with
 * liveness information.
 */
struct WorkerRegistryEntry
{
    struct Worker worker;

    /*
     * Time when the runtime last heard
     * from this worker.
     */
    time_t last_seen;

    /*
     * Current availability state.
     */
    enum WorkerStatus status;
};

struct WorkerRegistry
{
    struct WorkerRegistryEntry workers[
        WORKER_REGISTRY_CAPACITY
    ];

    int count;
};

/*
 * Initialize the registry.
 */
void initialize_worker_registry(
    struct WorkerRegistry *registry
);

/*
 * Register a worker.
 */
int register_worker(
    struct WorkerRegistry *registry,
    struct Worker *worker
);

/*
 * Find a worker by ID.
 */
struct Worker *find_registered_worker(
    struct WorkerRegistry *registry,
    int worker_id
);

/*
 * Update the last-seen time of a worker.
 *
 * Returns 1 if the worker exists.
 * Returns 0 otherwise.
 */
int update_worker_heartbeat(
    struct WorkerRegistry *registry,
    int worker_id
);

/*
 * Mark workers as unavailable when
 * their heartbeat has timed out.
 *
 * Returns the number of workers
 * that became unavailable.
 */
int mark_stale_workers(
    struct WorkerRegistry *registry
);

/*
 * Return the number of registered workers.
 */
int get_registered_worker_count(
    struct WorkerRegistry *registry
);

#endif
