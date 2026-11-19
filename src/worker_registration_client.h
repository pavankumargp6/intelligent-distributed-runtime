#ifndef WORKER_REGISTRATION_CLIENT_H
#define WORKER_REGISTRATION_CLIENT_H

#include "worker.h"

/*
 * Connect to the runtime coordinator
 * and register the worker.
 *
 * Returns 1 on success.
 * Returns 0 on failure.
 */
int register_worker_with_runtime(
    struct Worker *worker,
    const char *runtime_address
);

/*
 * Connect to the runtime coordinator
 * and register the worker, reporting its current running-task
 * count and capacity. The coordinator uses these fields for
 * per-worker admission control.
 *
 * Returns 1 on success.
 * Returns 0 on failure.
 */
int register_worker_with_runtime_with_load(
    struct Worker *worker,
    const char *runtime_address,
    int running_tasks,
    int max_running_tasks
);

/*
 * Connect to the runtime coordinator
 * and send one heartbeat.
 *
 * Returns 1 on success.
 * Returns 0 on failure.
 */
int send_worker_heartbeat(
    const struct Worker *worker,
    const char *runtime_address
);

/*
 * Connect to the runtime coordinator
 * and send one heartbeat with the worker's running-task count and
 * capacity. The coordinator updates its admission-control view of
 * the worker from these fields.
 *
 * Returns 1 on success.
 * Returns 0 on failure.
 */
int send_worker_heartbeat_with_load(
    const struct Worker *worker,
    const char *runtime_address,
    int running_tasks,
    int max_running_tasks
);

#endif
