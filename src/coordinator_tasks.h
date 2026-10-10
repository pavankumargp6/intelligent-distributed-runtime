#ifndef COORDINATOR_TASKS_H
#define COORDINATOR_TASKS_H

#include "task.h"

#define COORDINATOR_MAX_INFLIGHT 64
#define COORDINATOR_MAX_COMPLETED 64

struct CompletedRecord
{
    int in_use;
    int task_id;
    int worker_id;
    int exit_status;
    int state;
};

struct InFlightTable
{
    struct
    {
        int in_use;
        int task_id;
        int worker_id;
        struct Task task;
    } entries[COORDINATOR_MAX_INFLIGHT];

    /*
     * Recently finished tasks, in a ring. Entries age out
     * after COORDINATOR_MAX_COMPLETED newer completions.
     * No persistence; a coordinator restart loses this.
     */
    struct CompletedRecord completed[COORDINATOR_MAX_COMPLETED];
    int completed_next;

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

int coordinator_is_inflight(struct InFlightTable *table,
                            int task_id);

void coordinator_record_completed(struct InFlightTable *table,
                                  int task_id,
                                  int worker_id,
                                  int exit_status,
                                  int state);

int coordinator_lookup_completed(struct InFlightTable *table,
                                 int task_id,
                                 int *worker_id,
                                 int *exit_status,
                                 int *state);

#endif
