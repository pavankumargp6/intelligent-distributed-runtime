#ifndef RUNTIME_H
#define RUNTIME_H

#include "task_manager.h"
#include "scheduler.h"
#include "worker_pool.h"
#include "task_executor.h"
#include "task_metrics.h"
#include "runtime_stats.h"
#include "worker_registry.h"
#include "wal.h"

/*
 * Top-level controller for the
 * Intelligent Distributed Runtime.
 */
struct Runtime
{
    struct TaskManager *task_manager;

    struct Scheduler *scheduler;

    struct WorkerPool *worker_pool;

    /*
     * Registry of workers known
     * to the runtime.
     */
    struct WorkerRegistry registry;

    /*
     * Runtime-wide performance statistics.
     */
    struct RuntimeStats stats;

    /*
     * Write-ahead log for task submissions and results.
     * Opened by start_runtime_server; -1 fd means no
     * persistence (submissions are queued but not durable).
     */
    struct Wal wal;
};

int runtime_submit_task(struct Runtime *runtime,
                        struct Task *task);

int runtime_run_next_task(struct Runtime *runtime);

void initialize_runtime(struct Runtime *runtime,
                        struct TaskManager *task_manager,
                        struct Scheduler *scheduler,
                        struct WorkerPool *worker_pool);

int runtime_register_worker(
    struct Runtime *runtime,
    struct Worker *worker);

#endif
