#include <stddef.h>

#include "scheduler.h"

void initialize_scheduler(struct Scheduler *scheduler,
                          struct TaskManager *manager)
{
    if (scheduler == NULL)
    {
        return;
    }

    scheduler->manager = manager;
}

int schedule_next_task(struct Scheduler *scheduler,
                       struct Task *task)
{
    if (scheduler == NULL ||
        scheduler->manager == NULL ||
        task == NULL)
    {
        return 0;
    }

    return get_next_task(scheduler->manager, task);
}
