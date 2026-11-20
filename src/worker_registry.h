#ifndef WORKER_REGISTRY_H
#define WORKER_REGISTRY_H

#include <time.h>

#include "worker.h"

#define WORKER_REGISTRY_CAPACITY 32

/*
 * Default per-worker task cap. A worker that does not report a
 * capacity in its registration or heartbeat is treated as able to
 * run this many tasks at once. It is a floor, not a promise:
 * the coordinator still applies CPU and memory fit on top of it.
 */
#define DEFAULT_WORKER_MAX_TASKS 4

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
 * liveness information and its current
 * running-task count.
 *
 * The running count is the admission-control
 * state: the coordinator reserves a slot when
 * it dispatches a task and releases it when
 * the task completes, is reaped, or the
 * worker disappears. Dispatch never sends a
 * task to a worker that is already at its
 * cap.
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

    /*
     * Tasks this worker is currently executing,
     * as last reported by the worker.
     */
    int running_tasks;

    /*
     * Maximum number of tasks this worker may
     * run at once. Defaults to
     * DEFAULT_WORKER_MAX_TASKS and is set from
     * the worker's registration or heartbeat.
     */
    int max_running_tasks;

    /*
     * Epoch/fencing token for the worker.
     *
     * Incremented by the coordinator each time a task is
     * dispatched to this worker. Used to reject stale
     * TASK_ACCEPT and TASK_RESULT messages from previous
     * dispatch attempts (e.g., if a worker restarts or
     * a network partition occurs).
     */
    int epoch;
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


/*
 * Check whether a worker is currently live.
 *
 * Returns 1 if the worker exists and is
 * WORKER_ACTIVE. Returns 0 otherwise.
 */
int is_worker_live(
    struct WorkerRegistry *registry,
    int worker_id
);

/*
 * Number of tasks the coordinator currently believes the
 * worker is running.
 */
int worker_running_tasks(
    struct WorkerRegistry *registry,
    int worker_id
);

/*
 * Maximum number of tasks the worker may run at once.
 *
 * Returns the configured cap, or DEFAULT_WORKER_MAX_TASKS
 * when the worker has not reported one.
 */
int worker_max_running_tasks(
    struct WorkerRegistry *registry,
    int worker_id
);

/*
 * Reserve one task slot on a worker.
 *
 * Returns 1 and increments the running count when the worker
 * is live and below its cap. Returns 0 when the worker is
 * unknown, unavailable, or at capacity.
 */
int worker_reserve_slot(
    struct WorkerRegistry *registry,
    int worker_id
);

/*
 * Release a task slot on a worker.
 *
 * Returns 1 if the worker existed and a slot was released,
 * 0 otherwise. Decrements the running count without going
 * below zero.
 */
int worker_release_slot(
    struct WorkerRegistry *registry,
    int worker_id
);

/*
 * Set the worker's reported running count and capacity.
 *
 * Used by the heartbeat handler so the coordinator's view of
 * the worker's load tracks the worker's own view. Returns 1
 * if the worker existed, 0 otherwise.
 */
int worker_update_load(
    struct WorkerRegistry *registry,
    int worker_id,
    int running_tasks,
    int max_running_tasks
);

/*
 * Get the worker's current epoch.
 *
 * Returns the epoch value, or -1 if the worker does not exist.
 */
int worker_get_epoch(
    struct WorkerRegistry *registry,
    int worker_id
);

/*
 * Increment and return the worker's epoch.
 *
 * Called by the scheduler when dispatching a task to this worker.
 * Returns the new epoch value, or -1 if the worker does not exist.
 */
int worker_increment_epoch(
    struct WorkerRegistry *registry,
    int worker_id
);
#endif
