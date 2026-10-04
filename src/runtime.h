#ifndef RUNTIME_H
#define RUNTIME_H

#include "task_manager.h"
#include "scheduler.h"
#include "worker_pool.h"

/*
 * Top-level controller for the
 * Intelligent Distributed Runtime.
 */
struct Runtime
{
    struct TaskManager *task_manager;

    struct Scheduler *scheduler;

    struct WorkerPool *worker_pool;
};

/*
 * Initialize the runtime with
 * its core subsystems.
 */
void initialize_runtime(struct Runtime *runtime,
                        struct TaskManager *task_manager,
                        struct Scheduler *scheduler,
                        struct WorkerPool *worker_pool);

#endif
