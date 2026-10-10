#include <string.h>

#include "coordinator_tasks.h"

void coordinator_tasks_init(struct InFlightTable *table)
{
    if (table == NULL)
    {
        return;
    }

    memset(table, 0, sizeof(*table));
    table->next_task_id = 1;
}

int coordinator_allocate_task_id(struct InFlightTable *table)
{
    if (table == NULL)
    {
        return -1;
    }

    int id = table->next_task_id;
    table->next_task_id++;

    return id;
}

int coordinator_record_inflight(struct InFlightTable *table,
                                const struct Task *task,
                                int worker_id)
{
    if (table == NULL || task == NULL)
    {
        return 0;
    }

    int i;
    for (i = 0; i < COORDINATOR_MAX_INFLIGHT; i++)
    {
        if (!table->entries[i].in_use)
        {
            table->entries[i].in_use = 1;
            table->entries[i].task_id = task->id;
            table->entries[i].worker_id = worker_id;
            table->entries[i].task = *task;
            return 1;
        }
    }

    return 0;
}

int coordinator_take_inflight(struct InFlightTable *table,
                              int task_id,
                              struct Task *task,
                              int *worker_id)
{
    if (table == NULL)
    {
        return 0;
    }

    int i;
    for (i = 0; i < COORDINATOR_MAX_INFLIGHT; i++)
    {
        if (table->entries[i].in_use &&
            table->entries[i].task_id == task_id)
        {
            if (task != NULL)
            {
                *task = table->entries[i].task;
            }

            if (worker_id != NULL)
            {
                *worker_id = table->entries[i].worker_id;
            }

            table->entries[i].in_use = 0;
            return 1;
        }
    }

    return 0;
}
