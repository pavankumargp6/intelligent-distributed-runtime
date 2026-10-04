#ifndef RUNTIME_H
#define RUNTIME_H

#include "task_manager.h"
#include "scheduler.h"
#include "worker_pool.h"
#include "task_executor.h"

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

/*
 * Submit a task to the runtime.
 */
int runtime_submit_task(struct Runtime *runtime,
                        struct Task *task);

/*
 * Schedule and execute the next
 * available task.
 */
int runtime_run_next_task(struct Runtime *runtime);

#endif
