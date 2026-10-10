#ifndef COORDINATOR_TASKS_H
#define COORDINATOR_TASKS_H

#include "task.h"

#define COORDINATOR_MAX_INFLIGHT 64

struct InFlightTable
{
    struct
    {
        int in_use;
        int task_id;
        int worker_id;
        struct Task task;
    } entries[COORDINATOR_MAX_INFLIGHT];

    int next_task_id;
};

void coordinator_tasks_init(struct InFlightTable *table);

int coordinator_allocate_task_id(struct InFlightTable *table);

int coordinator_record_inflight(struct InFlightTable *table,
                                const struct Task *task,
                                int worker_id);

int coordinator_take_inflight(struct InFlightTable *table,
                              int task_id,
                              struct Task *task,
                              int *worker_id);

#endif
