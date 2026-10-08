#ifndef WORKER_REGISTRY_H
#define WORKER_REGISTRY_H

#include "worker.h"

#define WORKER_REGISTRY_CAPACITY 32

/*
 * Stores the currently known workers.
 */
struct WorkerRegistry
{
    struct Worker workers[WORKER_REGISTRY_CAPACITY];

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
 *
 * Returns 1 on success.
 * Returns 0 on failure.
 */
int register_worker(
    struct WorkerRegistry *registry,
    struct Worker *worker
);

/*
 * Find a worker by ID.
 *
 * Returns a pointer to the worker
 * if found, otherwise NULL.
 */
struct Worker *find_registered_worker(
    struct WorkerRegistry *registry,
    int worker_id
);

/*
 * Return the number of registered workers.
 */
int get_registered_worker_count(
    struct WorkerRegistry *registry
);

#endif
