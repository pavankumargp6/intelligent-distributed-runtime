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
    table->completed_next = 0;
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

int coordinator_is_inflight(struct InFlightTable *table,
                            int task_id)
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
            return 1;
        }
    }

    return 0;
}

void coordinator_record_completed(struct InFlightTable *table,
                                  int task_id,
                                  int worker_id,
                                  int exit_status,
                                  int state)
{
    if (table == NULL)
    {
        return;
    }

    struct CompletedRecord *slot =
        &table->completed[table->completed_next];

    slot->in_use = 1;
    slot->task_id = task_id;
    slot->worker_id = worker_id;
    slot->exit_status = exit_status;
    slot->state = state;

    table->completed_next =
        (table->completed_next + 1) % COORDINATOR_MAX_COMPLETED;
}

int coordinator_lookup_completed(struct InFlightTable *table,
                                 int task_id,
                                 int *worker_id,
                                 int *exit_status,
                                 int *state)
{
    if (table == NULL)
    {
        return 0;
    }

    int i;
    for (i = 0; i < COORDINATOR_MAX_COMPLETED; i++)
    {
        if (table->completed[i].in_use &&
            table->completed[i].task_id == task_id)
        {
            if (worker_id != NULL)
            {
                *worker_id = table->completed[i].worker_id;
            }
            if (exit_status != NULL)
            {
                *exit_status = table->completed[i].exit_status;
            }
            if (state != NULL)
            {
                *state = table->completed[i].state;
            }
            return 1;
        }
    }

    return 0;
}
