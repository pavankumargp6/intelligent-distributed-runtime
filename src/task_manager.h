#ifndef TASK_MANAGER_H
#define TASK_MANAGER_H

#include "task_queue.h"

struct TaskManager
{
    struct TaskQueue queue;
};

void initialize_task_manager(struct TaskManager *manager);

int submit_task(struct TaskManager *manager,
                struct Task *task);

int get_next_task(struct TaskManager *manager,
                  struct Task *task);

#endif
