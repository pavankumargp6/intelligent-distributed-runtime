#ifndef COORDINATOR_TASKS_H
#define COORDINATOR_TASKS_H

#include <time.h>

#include "task.h"

#define COORDINATOR_MAX_INFLIGHT 64
#define COORDINATOR_MAX_COMPLETED 64

/*
 * How long a dispatched task may sit in-flight without a
 * TASK_ACCEPT before the coordinator gives up on it. Only
 * matters for tasks whose worker connection dropped between
 * assign and accept.
 */
#define DISPATCH_TIMEOUT_SECONDS 15

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

        /*
         * Set when the coordinator sends TASK_ASSIGN.
         * Used with DISPATCH_TIMEOUT_SECONDS to detect a
         * task that was dispatched but never accepted.
         */
        time_t dispatched_at;

        /*
         * Set when the worker sends TASK_ACCEPT. Before this
         * point the coordinator cannot be sure the worker
         * even parsed the assignment.
         */
        int accepted;
    } entries[COORDINATOR_MAX_INFLIGHT];

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

/*
 * Mark the in-flight entry for task_id as accepted. Returns
 * 1 if the entry existed, 0 otherwise. Idempotent: calling
 * twice has no effect beyond the first.
 */
int coordinator_mark_accepted(struct InFlightTable *table,
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
