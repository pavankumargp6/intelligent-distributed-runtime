#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "task_manager.h"
#include "worker_resources.h"

struct Scheduler
{
    struct TaskManager *manager;

    struct WorkerResources resources;
};

void initialize_scheduler(struct Scheduler *scheduler,
                          struct TaskManager *manager);

int schedule_next_task(struct Scheduler *scheduler,
                       struct Task *task);

#endif
