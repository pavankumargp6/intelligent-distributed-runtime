#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "task_manager.h"

struct Scheduler
{
    struct TaskManager *manager;
};

void initialize_scheduler(struct Scheduler *scheduler,
                          struct TaskManager *manager);

int schedule_next_task(struct Scheduler *scheduler,
                       struct Task *task);

#endif

