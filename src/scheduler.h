#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "task_manager.h"
#include "worker_pool.h"
#include "worker_registry.h"

struct Scheduler
{
    struct TaskManager *manager;

    struct WorkerPool *worker_pool;
    struct WorkerRegistry *registry;
};

void initialize_scheduler(struct Scheduler *scheduler,
                          struct TaskManager *manager,
                          struct WorkerPool *worker_pool);

int schedule_next_task(struct Scheduler *scheduler,
                       struct Task *task,
                       struct Worker **selected_worker);

#endif
