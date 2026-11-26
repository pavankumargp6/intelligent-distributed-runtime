#include <stddef.h>

#include "task_manager.h"
#include "runtime_limits.h"

void initialize_task_manager(
    struct TaskManager *manager,
    size_t queue_capacity)
{
    if (manager == NULL)
    {
        return;
    }

    initialize_task_queue(&manager->queue, queue_capacity);
}

int submit_task(struct TaskManager *manager,
                struct Task *task)
{
    if (manager == NULL || task == NULL)
    {
        return 0;
    }

    return enqueue_task(&manager->queue, task);
}

int get_next_task(struct TaskManager *manager,
                  struct Task *task)
{
    if (manager == NULL || task == NULL)
    {
        return 0;
    }

    return dequeue_task(&manager->queue, task);
}