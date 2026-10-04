#ifndef WORKER_H
#define WORKER_H

#include "worker_resources.h"

/*
 * Represents one worker node
 * in the distributed runtime.
 */
struct Worker
{
    int id;

    struct WorkerResources resources;
};

/*
 * Initialize a worker and collect
 * its current resource information.
 *
 * Returns:
 *
 * 1 -> success
 * 0 -> failure
 */
int initialize_worker(struct Worker *worker,
                      int id);

/*
 * Refresh the worker's current
 * resource information.
 *
 * Returns:
 *
 * 1 -> success
 * 0 -> failure
 */
int refresh_worker_resources(struct Worker *worker);

#endif
