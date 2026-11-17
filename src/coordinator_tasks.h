#ifndef COORDINATOR_TASKS_H
#define COORDINATOR_TASKS_H

#include <stddef.h>
#include <time.h>

#include "task.h"

#define COORDINATOR_MAX_INFLIGHT 64
#define COORDINATOR_MAX_COMPLETED 64

/*
 * Idempotent tasks are retried at most this many times before
 * the coordinator gives up and records TASK_FAILED.
 */
#define COORDINATOR_MAX_ATTEMPTS 3

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

/*
 * A retained terminal result.
 *
 * Terminal results are kept beyond the 64-entry completion ring
 * so QUERY can answer for completed tasks after recovery and so
 * the retention policy has something to expire. Records are
 * stored in completion order (oldest first in the logical
 * sequence) and indexed by task ID.
 */
struct HistoryRecord
{
    int task_id;
    int worker_id;
    int exit_status;
    int state;
    time_t completed_at;
};

struct TaskHistory
{
    struct HistoryRecord *entries;
    size_t capacity;
    size_t count;
    size_t head;

    /*
     * Open-addressed index: 0 means empty, -1 means a deleted
     * slot (tombstone), and any other value is a ring position
     * plus one.
     */
    int *slots;
    size_t slot_count;
    size_t tombstones;

    time_t retention_seconds;
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

    /*
     * Retained terminal results. May have capacity 0 if the
     * process could not allocate the configured retention
     * table; the completion ring still works in that case.
     */
    struct TaskHistory history;

    int next_task_id;
};

void coordinator_tasks_init(struct InFlightTable *table);

void coordinator_tasks_destroy(struct InFlightTable *table);

int coordinator_allocate_task_id(struct InFlightTable *table);

int coordinator_record_inflight(struct InFlightTable *table,
                                const struct Task *task,
                                int worker_id);

int coordinator_take_inflight(struct InFlightTable *table,
                              int task_id,
                              struct Task *task,
                              int *worker_id);

/*
 * Copy the in-flight entry without removing it.
 *
 * Used by handlers that must append a durable record before
 * changing in-memory state.
 */
int coordinator_peek_inflight(struct InFlightTable *table,
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

/*
 * Mark accepted and return the assigned worker and attempt
 * number. Returns 1 if the entry existed.
 */
int coordinator_mark_accepted_worker(struct InFlightTable *table,
                                     int task_id,
                                     int *worker_id,
                                     int *attempt);

void coordinator_record_completed(struct InFlightTable *table,
                                  int task_id,
                                  int worker_id,
                                  int exit_status,
                                  int state);

/*
 * Record a terminal result with its original completion time.
 * Used when restoring a snapshot so the retention window does
 * not restart on every recovery.
 */
void coordinator_record_completed_at(struct InFlightTable *table,
                                     int task_id,
                                     int worker_id,
                                     int exit_status,
                                     int state,
                                     time_t completed_at);

int coordinator_lookup_completed(struct InFlightTable *table,
                                 int task_id,
                                 int *worker_id,
                                 int *exit_status,
                                 int *state);

/*
 * Retained-history helpers.
 */

int coordinator_history_init(struct TaskHistory *history,
                             size_t capacity,
                             time_t retention_seconds);

void coordinator_history_destroy(struct TaskHistory *history);

/*
 * Insert a terminal result. The first result for a task wins:
 * returns 0 without changing anything when the task is already
 * present. Returns 1 when a new record was stored.
 */
int coordinator_history_record(struct TaskHistory *history,
                               int task_id,
                               int worker_id,
                               int exit_status,
                               int state,
                               time_t completed_at);

int coordinator_history_lookup(const struct TaskHistory *history,
                               int task_id,
                               struct HistoryRecord *out);

size_t coordinator_history_count(const struct TaskHistory *history);

/*
 * Access records in completion order. index 0 is the oldest
 * retained record.
 */
int coordinator_history_get(const struct TaskHistory *history,
                            size_t index,
                            struct HistoryRecord *out);

#endif
