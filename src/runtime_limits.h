#ifndef RUNTIME_LIMITS_H
#define RUNTIME_LIMITS_H

/*
 * Centralized capacity limits for the coordinator.
 *
 * All values can be overridden at runtime via environment variables.
 * The defaults match the v0.2.x compile-time constants so existing
 * deployments see no behavior change unless they opt in.
 */

/*
 * Worker registry (active workers the coordinator knows about).
 *
 * Env: IDR_WORKER_REGISTRY_CAPACITY
 * Default: 32
 * Min: 1
 * Max: 100000
 */
size_t runtime_worker_registry_capacity(void);

/*
 * Task queue (tasks waiting for a worker).
 *
 * Env: IDR_TASK_QUEUE_CAPACITY
 * Default: 512
 * Min: 1
 * Max: 1000000
 */
size_t runtime_task_queue_capacity(void);

/*
 * In-flight table (tasks dispatched to workers, awaiting result).
 *
 * Env: IDR_INFLIGHT_CAPACITY
 * Default: 64
 * Min: 1
 * Max: 100000
 */
size_t runtime_inflight_capacity(void);

/*
 * Completed ring (recently finished tasks for fast STATUS/QUERY).
 *
 * Env: IDR_COMPLETED_CAPACITY
 * Default: 64
 * Min: 1
 * Max: 100000
 */
size_t runtime_completed_capacity(void);

/*
 * Connection pool (concurrent client/worker connections).
 *
 * Env: IDR_CONNECTION_POOL_CAPACITY
 * Default: 64
 * Min: 1
 * Max: 10000
 */
size_t runtime_connection_pool_capacity(void);

/*
 * Worker pool (worker metadata for scheduling decisions).
 *
 * Env: IDR_WORKER_POOL_CAPACITY
 * Default: 16
 * Min: 1
 * Max: 100000
 */
size_t runtime_worker_pool_capacity(void);

/*
 * Result retention (terminal results kept beyond the completed ring).
 *
 * Env: IDR_RESULT_RETENTION (already used)
 * Default: 100000
 * Min: 0 (disabled)
 * Max: 10000000
 */
size_t runtime_result_retention_capacity(void);

#endif