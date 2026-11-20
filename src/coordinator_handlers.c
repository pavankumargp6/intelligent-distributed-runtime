#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "coordinator_tasks.h"
#include "coordinator_handlers.h"
#include "runtime.h"
#include "wire_fields.h"
#include "scheduler.h"
#include "task_manager.h"
#include "runtime_stats.h"
#include "worker_registry.h"
#include "connection_pool.h"
#include "log.h"
#include "wal.h"
static int parse_submit(const char *message,
                        struct Task *task)
{
    if (message == NULL || task == NULL)
    {
        return 0;
    }

    const char *prefix = "SUBMIT|";
    size_t prefix_length = strlen(prefix);

    if (strncmp(message, prefix, prefix_length) != 0)
    {
        return 0;
    }

    const char *cursor = message + prefix_length;

    memset(task, 0, sizeof(*task));
    task->assigned_worker_id = -1;
    task->state = TASK_CREATED;

    if (!wire_read_field(&cursor,
                         task->command,
                         sizeof(task->command)))
    {
        return 0;
    }

    if (!wire_read_delimiter(&cursor))
    {
        return 0;
    }

    long argument_count = 0;

    if (!wire_read_int(&cursor, &argument_count))
    {
        return 0;
    }

    if (argument_count < 0 ||
        argument_count > TASK_ARGUMENT_COUNT)
    {
        return 0;
    }

    int i;
    for (i = 0; i < (int)argument_count; i++)
    {
        if (!wire_read_delimiter(&cursor))
        {
            return 0;
        }

        if (!wire_read_field(&cursor,
                             task->arguments[i],
                             sizeof(task->arguments[i])))
        {
            return 0;
        }
    }

    task->argument_count = (int)argument_count;

    /*
     * Optional trailing field: "<idempotent:0|1>". If absent,
     * defaults to 0 (not safe to retry).
     */
    if (*cursor == '|')
    {
        cursor++;

        long idem = 0;

        if (wire_read_int(&cursor, &idem))
        {
            task->idempotent = idem ? 1 : 0;
        }
    }

    /*
     * Optional trailing field: "<timeout_seconds>". If absent,
     * defaults to 0 (no execution time limit). Only meaningful
     * when the idempotent field is present, per the positional
     * trailing-field rule.
     *
     * Negative or out-of-range values are rejected outright. We
     * do not silently coerce them to 0 because "0" means
     * "unlimited" and a client that asked for -1 must not be
     * silently granted an unbounded execution.
     */
    if (*cursor == '|')
    {
        cursor++;

        long timeout = 0;

        if (!wire_read_int(&cursor, &timeout))
        {
            return 0;
        }

        if (timeout < 0 || timeout > INT_MAX)
        {
            return 0;
        }

        task->timeout_seconds = (int)timeout;
    }

    return 1;
}

static int build_task_assign(const struct Task *task,
                             char *buffer,
                             size_t buffer_size)
{
    if (task == NULL || buffer == NULL || buffer_size == 0)
    {
        return 0;
    }

    const char *prefix = "TASK_ASSIGN";
    size_t prefix_length = strlen(prefix);

    if (prefix_length + 1 > buffer_size)
    {
        return 0;
    }

    memcpy(buffer, prefix, prefix_length);
    buffer[prefix_length] = '\0';

    size_t offset = prefix_length;

    offset = wire_append_delimiter(buffer, buffer_size, offset);
    if (offset == 0) return 0;

    offset = wire_append_int(buffer, buffer_size, offset, task->id);
    if (offset == 0) return 0;

    offset = wire_append_delimiter(buffer, buffer_size, offset);
    if (offset == 0) return 0;

    offset = wire_append_field(buffer, buffer_size, offset, task->command);
    if (offset == 0) return 0;

    offset = wire_append_delimiter(buffer, buffer_size, offset);
    if (offset == 0) return 0;

    offset = wire_append_int(buffer,
                             buffer_size,
                             offset,
                             task->argument_count);
    if (offset == 0) return 0;

    int i;
    for (i = 0; i < task->argument_count; i++)
    {
        offset = wire_append_delimiter(buffer, buffer_size, offset);
        if (offset == 0) return 0;

        offset = wire_append_field(buffer,
                                   buffer_size,
                                   offset,
                                   task->arguments[i]);
        if (offset == 0) return 0;
    }

    offset = wire_append_delimiter(buffer, buffer_size, offset);
    if (offset == 0) return 0;

    offset = wire_append_int(buffer, buffer_size, offset,
                             task->timeout_seconds);
    if (offset == 0) return 0;

    offset = wire_append_delimiter(buffer, buffer_size, offset);
    if (offset == 0) return 0;

    offset = wire_append_int(buffer, buffer_size, offset,
                             task->epoch);
    if (offset == 0) return 0;

    return 1;
}

static int build_wal_submit(const struct Task *task,
                            char *buffer,
                            size_t buffer_size)
{
    const char *prefix = "SUBMIT";
    size_t prefix_length = strlen(prefix);

    if (prefix_length + 1 > buffer_size)
    {
        return 0;
    }

    memcpy(buffer, prefix, prefix_length);
    buffer[prefix_length] = '\0';

    size_t offset = prefix_length;

    offset = wire_append_delimiter(buffer, buffer_size, offset);
    if (offset == 0) return 0;

    offset = wire_append_int(buffer, buffer_size, offset, task->id);
    if (offset == 0) return 0;

    offset = wire_append_delimiter(buffer, buffer_size, offset);
    if (offset == 0) return 0;

    offset = wire_append_field(buffer, buffer_size, offset, task->command);
    if (offset == 0) return 0;

    offset = wire_append_delimiter(buffer, buffer_size, offset);
    if (offset == 0) return 0;

    offset = wire_append_int(buffer,
                             buffer_size,
                             offset,
                             task->argument_count);
    if (offset == 0) return 0;

    for (int i = 0; i < task->argument_count; i++)
    {
        offset = wire_append_delimiter(buffer, buffer_size, offset);
        if (offset == 0) return 0;

        offset = wire_append_field(buffer,
                                   buffer_size,
                                   offset,
                                   task->arguments[i]);
        if (offset == 0) return 0;
    }

    offset = wire_append_delimiter(buffer, buffer_size, offset);
    if (offset == 0) return 0;

    offset = wire_append_int(buffer, buffer_size, offset, task->idempotent);
    if (offset == 0) return 0;

    offset = wire_append_delimiter(buffer, buffer_size, offset);
    if (offset == 0) return 0;

    offset = wire_append_int(buffer, buffer_size, offset,
                             task->timeout_seconds);
    if (offset == 0) return 0;

    return 1;
}

static int append_result_record(struct Runtime *runtime,
                                int task_id,
                                int worker_id,
                                int exit_status,
                                int state)
{
    char line[128];

    int n = snprintf(line,
                     sizeof(line),
                     "RESULT|%d|%d|%d|%d",
                     task_id,
                     worker_id,
                     exit_status,
                     state);

    if (n <= 0 || (size_t)n >= sizeof(line))
    {
        log_error("wal_line_too_long",
                  "Could not format RESULT record for task %d.",
                  task_id);
        return 0;
    }

    if (!wal_append(&runtime->wal, line))
    {
        log_error("wal_append_failed",
                  "Could not append RESULT record for task %d. "
                  "Keeping the task in-flight so the result can be "
                  "made durable later.",
                  task_id);
        return 0;
    }

    return 1;
}

static int append_dispatch_record(struct Runtime *runtime,
                                  int task_id,
                                  int worker_id,
                                  int attempt)
{
    char line[128];

    int n = snprintf(line,
                     sizeof(line),
                     "DISPATCH|%d|%d|0|%d|%lld",
                     task_id,
                     worker_id,
                     attempt,
                     (long long)time(NULL));

    if (n <= 0 || (size_t)n >= sizeof(line))
    {
        return 0;
    }

    if (!wal_append(&runtime->wal, line))
    {
        log_error("wal_dispatch_append_failed",
                  "Could not append DISPATCH record for task %d.",
                  task_id);
        return 0;
    }

    return 1;
}

static int append_accept_record(struct Runtime *runtime,
                                int task_id,
                                int worker_id,
                                int attempt)
{
    char line[128];

    int n = snprintf(line,
                     sizeof(line),
                     "ACCEPT|%d|%d|0|%d|%lld",
                     task_id,
                     worker_id,
                     attempt,
                     (long long)time(NULL));

    if (n <= 0 || (size_t)n >= sizeof(line))
    {
        return 0;
    }

    if (!wal_append(&runtime->wal, line))
    {
        log_error("wal_accept_append_failed",
                  "Could not append ACCEPT record for task %d.",
                  task_id);
        return 0;
    }

    return 1;
}

/*
 * Apply the retry policy to an in-flight task whose worker is
 * gone or never acknowledged the assignment.
 *
 * Idempotent tasks are requeued while their recorded attempt
 * count is below COORDINATOR_MAX_ATTEMPTS. Non-idempotent tasks
 * become TASK_UNKNOWN because the coordinator cannot prove
 * whether the command ran; calling them FAILED would invite an
 * unsafe blind retry.
 *
 * The terminal RESULT is appended before the in-memory entry is
 * removed, so a crash at any point either replays the old
 * dispatch (and retries or reports unknown) or sees the result.
 *
 * Returns 1 when the task was requeued or finalized, and 0 when
 * it could not be handled now (the reaper will try again).
 */
static int reconcile_abandoned_task(struct Runtime *runtime,
                                    struct InFlightTable *table,
                                    int task_id,
                                    int worker_id,
                                    const struct Task *source,
                                    const char *reason)
{
    struct Task task = *source;

    if (task.idempotent &&
        task.attempt < COORDINATOR_MAX_ATTEMPTS)
    {
        task.assigned_worker_id = -1;
        task.state = TASK_CREATED;

        if (!coordinator_take_inflight(table, task_id, NULL, NULL))
        {
            return 0;
        }

        if (submit_task(runtime->task_manager, &task))
        {
            log_warn("task_requeued",
                     "Requeued idempotent task %d after %s "
                     "(attempt %d of %d)",
                     task_id,
                     reason,
                     task.attempt,
                     COORDINATOR_MAX_ATTEMPTS);
            return 1;
        }

        /*
         * The queue is full. Re-track the task so the next
         * reaper pass can try again instead of losing it.
         */
        if (coordinator_record_inflight(table, &task, worker_id))
        {
            log_warn("task_requeue_deferred",
                     "Could not requeue idempotent task %d (%s): "
                     "queue is full; will retry",
                     task_id,
                     reason);
            return 0;
        }

        log_error("task_requeue_lost",
                  "Could not requeue or re-track idempotent task %d",
                  task_id);
        return 0;
    }

    int terminal_state = task.idempotent
                             ? TASK_FAILED
                             : TASK_UNKNOWN;

    if (!append_result_record(runtime,
                              task_id,
                              worker_id,
                              -1,
                              terminal_state))
    {
        return 0;
    }

    if (!coordinator_take_inflight(table, task_id, NULL, NULL))
    {
        return 0;
    }

    /*
     * Release the worker's slot. The task was dispatched to this
     * worker, so the reservation must be returned even when the
     * worker is gone or the task is abandoned.
     */
    worker_release_slot(&runtime->registry, worker_id);

    coordinator_record_completed(table,
                                 task_id,
                                 worker_id,
                                 -1,
                                 terminal_state);

    if (terminal_state == TASK_UNKNOWN)
    {
        record_task_unknown(&runtime->stats);
        log_warn("task_unknown",
                 "Task %d is non-idempotent and was abandoned "
                 "(%s); recording TASK_UNKNOWN",
                 task_id,
                 reason);
    }
    else
    {
        record_task_failed(&runtime->stats);
        log_warn("task_retry_exhausted",
                 "Task %d exhausted %d attempts (%s); "
                 "recording TASK_FAILED",
                 task_id,
                 COORDINATOR_MAX_ATTEMPTS,
                 reason);
    }

    return 1;
}

int coordinator_handle_submit(struct Runtime *runtime,
                              struct InFlightTable *table,
                              struct Connection *conn,
                              const char *message)
{
    if (runtime == NULL || table == NULL || conn == NULL ||
        message == NULL)
    {
        return 0;
    }

    struct Task task;

    if (!parse_submit(message, &task))
    {
        log_warn("submit_invalid", "Invalid SUBMIT message.");
        pool_send_message(conn, "SUBMIT_REJECT");
        return 0;
    }

    int id = coordinator_allocate_task_id(table);

    if (id <= 0)
    {
        log_error("task_id_alloc_failed", "Could not allocate task ID.");
        pool_send_message(conn, "SUBMIT_REJECT");
        return 0;
    }

    task.id = id;

    if (is_task_queue_full(&runtime->task_manager->queue))
    {
        log_warn("queue_full", "Task queue full; rejecting task %d.", id);
        pool_send_message(conn, "SUBMIT_REJECT");
        return 0;
    }

    char wal_line[NETWORK_MAX_MESSAGE_SIZE];

    if (!build_wal_submit(&task, wal_line, sizeof(wal_line)))
    {
        log_error("wal_build_failed",
                  "Could not build WAL record for task %d.", id);
        pool_send_message(conn, "SUBMIT_REJECT");
        return 0;
    }

    if (!wal_append(&runtime->wal, wal_line))
    {
        log_error("wal_append_failed",
                  "Could not append WAL record for task %d.", id);
        pool_send_message(conn, "SUBMIT_REJECT");
        return 0;
    }

    if (!runtime_submit_task(runtime, &task))
    {
        log_warn("task_rejected",
                 "runtime_submit_task rejected task %d.",
                 id);
        pool_send_message(conn, "SUBMIT_REJECT");
        return 0;
    }

    log_info("task_accepted",
             "Accepted task %d: %s argc=%d",
             id,
             task.command,
             task.argument_count);

    char response[64];
    int n = snprintf(response,
                     sizeof(response),
                     "SUBMIT_ACK|%d",
                     id);

    if (n <= 0 || (size_t)n >= sizeof(response))
    {
        pool_send_message(conn, "SUBMIT_REJECT");
        return 0;
    }

    pool_send_message(conn, response);

    return 1;
}

int coordinator_handle_task_request(struct Runtime *runtime,
                                    struct InFlightTable *table,
                                    struct Connection *conn,
                                    const char *message)
{
    if (runtime == NULL || table == NULL || conn == NULL ||
        message == NULL)
    {
        return 0;
    }

    const char *prefix = "TASK_REQUEST|";
    size_t prefix_length = strlen(prefix);

    if (strncmp(message, prefix, prefix_length) != 0)
    {
        pool_send_message(conn, "NO_TASK");
        return 0;
    }

    const char *cursor = message + prefix_length;

    long requesting_worker_id = 0;

    if (!wire_read_int(&cursor, &requesting_worker_id) ||
        requesting_worker_id <= 0)
    {
        pool_send_message(conn, "NO_TASK");
        return 0;
    }

    struct Task task;
    struct Worker *selected = NULL;

    if (!schedule_next_task_for_worker(runtime->scheduler,
                                       &task,
                                       (int)requesting_worker_id,
                                       &selected) ||
        selected == NULL)
    {
        pool_send_message(conn, "NO_TASK");
        return 1;
    }

    char assign[NETWORK_MAX_MESSAGE_SIZE];

    if (!build_task_assign(&task, assign, sizeof(assign)))
    {
        log_error("assign_build_failed",
                  "Could not build TASK_ASSIGN for task %d.",
                  task.id);
        submit_task(runtime->task_manager, &task);
        pool_send_message(conn, "NO_TASK");
        return 0;
    }

    /*
     * Count the attempt and durably record the dispatch before
     * the worker can see it. If the coordinator dies after this
     * point, recovery knows the worker may have run the task.
     */
    task.attempt++;

    /*
     * Record the dispatch moment BEFORE the task is copied into
     * the in-flight table: the result handler reads the table's
     * copy, so a start time recorded only on the local struct
     * would be lost and execution time would be measured from
     * zero (i.e. the monotonic clock's boot-relative value).
     */
    record_task_start(&task.metrics);

    if (!coordinator_record_inflight(table, &task, selected->id))
    {
        log_warn("inflight_full",
                 "In-flight table full; requeueing task %d.",
                 task.id);
        task.assigned_worker_id = -1;
        task.state = TASK_CREATED;
        submit_task(runtime->task_manager, &task);
        pool_send_message(conn, "NO_TASK");
        return 0;
    }

    if (!append_dispatch_record(runtime,
                                task.id,
                                selected->id,
                                task.attempt))
    {
        coordinator_take_inflight(table, task.id, NULL, NULL);
        task.assigned_worker_id = -1;
        task.state = TASK_CREATED;
        submit_task(runtime->task_manager, &task);
        pool_send_message(conn, "NO_TASK");
        return 0;
    }

    if (!pool_send_message(conn, assign))
    {
        log_warn("assign_send_failed",
                 "Failed to queue TASK_ASSIGN for task %d; "
                 "the dispatch timeout will reap it.",
                 task.id);
        return 0;
    }

    log_info("task_dispatched",
             "Dispatched task %d to worker %d.",
             task.id,
             selected->id);

    return 1;
}

int coordinator_handle_task_accept(struct Runtime *runtime,
                                   struct InFlightTable *table,
                                   struct Connection *conn,
                                   const char *message)
{
    if (runtime == NULL || table == NULL || conn == NULL ||
        message == NULL)
    {
        return 0;
    }

    const char *prefix = "TASK_ACCEPT|";
    size_t prefix_length = strlen(prefix);

    if (strncmp(message, prefix, prefix_length) != 0)
    {
        return 0;
    }

    const char *cursor = message + prefix_length;

    long task_id = 0;
    long epoch = 0;

    if (!wire_read_int(&cursor, &task_id) || task_id <= 0)
    {
        pool_send_message(conn, "TASK_ACCEPT_REJECT|-1|bad_id");
        return 0;
    }

    /*
     * Read epoch (optional for backward compatibility).
     * If not present, treat as epoch 0.
     */
    wire_read_delimiter(&cursor);
    wire_read_int(&cursor, &epoch);

    struct Task task;
    int worker_id = -1;

    if (!coordinator_peek_inflight(table,
                                   (int)task_id,
                                   &task,
                                   &worker_id))
    {
        log_warn("task_accept_unknown",
                 "TASK_ACCEPT for unknown task %ld", task_id);

        char reject[64];
        int n = snprintf(reject, sizeof(reject),
                         "TASK_ACCEPT_REJECT|%ld|unknown_task",
                         task_id);
        if (n > 0 && (size_t)n < sizeof(reject))
        {
            pool_send_message(conn, reject);
        }
        return 0;
    }

    /*
     * Validate epoch: the accept must match the task's epoch.
     * This rejects stale accepts from previous dispatch attempts.
     */
    if (task.epoch != (int)epoch)
    {
        log_warn("task_accept_epoch_mismatch",
                 "TASK_ACCEPT for task %ld: expected epoch %d, got %ld",
                 task_id,
                 task.epoch,
                 epoch);

        char reject[64];
        int n = snprintf(reject, sizeof(reject),
                         "TASK_ACCEPT_REJECT|%ld|epoch_mismatch",
                         task_id);
        if (n > 0 && (size_t)n < sizeof(reject))
        {
            pool_send_message(conn, reject);
        }
        return 0;
    }

    /*
     * Persist the acknowledgement before marking the in-memory
     * entry accepted. If this append fails the worker is told to
     * abort, and the dispatch timeout applies the retry policy.
     */
    if (!append_accept_record(runtime,
                              (int)task_id,
                              worker_id,
                              task.attempt))
    {
        char reject[64];
        int n = snprintf(reject, sizeof(reject),
                         "TASK_ACCEPT_REJECT|%ld|wal_failed",
                         task_id);
        if (n > 0 && (size_t)n < sizeof(reject))
        {
            pool_send_message(conn, reject);
        }
        return 0;
    }

    if (!coordinator_mark_accepted(table, (int)task_id))
    {
        log_warn("task_accept_unknown",
                 "TASK_ACCEPT for task %ld disappeared", task_id);
        return 0;
    }

    log_debug("task_accepted_by_worker",
              "Task %ld accepted by worker %d (attempt %d)",
              task_id,
              worker_id,
              task.attempt);

    char ack[64];
    int n = snprintf(ack, sizeof(ack),
                     "TASK_ACCEPT_ACK|%ld", task_id);

    if (n > 0 && (size_t)n < sizeof(ack))
    {
        pool_send_message(conn, ack);
    }

    return 1;
}

int coordinator_handle_task_result(struct Runtime *runtime,
                                   struct InFlightTable *table,
                                   struct Connection *conn,
                                   const char *message)
{
    if (runtime == NULL || table == NULL || conn == NULL ||
        message == NULL)
    {
        return 0;
    }

    int task_id = -1;
    int worker_id = -1;
    int exit_status = -1;
    int state = -1;
    int epoch = 0;

    int fields = sscanf(message,
                        "TASK_RESULT|%d|%d|%d|%d|%d",
                        &task_id,
                        &worker_id,
                        &exit_status,
                        &state,
                        &epoch);

    if (fields < 4)
    {
        log_warn("result_malformed",
                 "Malformed TASK_RESULT: %s",
                 message);
        pool_send_message(conn, "UNKNOWN_COMMAND");
        return 0;
    }

    if (state < TASK_COMPLETED || state > TASK_UNKNOWN)
    {
        log_warn("result_state_invalid",
                 "TASK_RESULT for task %d has invalid state %d",
                 task_id,
                 state);
        pool_send_message(conn, "UNKNOWN_COMMAND");
        return 0;
    }

    struct Task task;
    int original_worker = -1;

    if (!coordinator_peek_inflight(table,
                                   task_id,
                                   &task,
                                   &original_worker))
    {
        /*
         * A duplicate RESULT for an already-terminal task is
         * acknowledged so a worker that retried the send does
         * not wedge. A RESULT for an ID the coordinator never
         * dispatched is also acknowledged: there is no in-flight
         * state to corrupt, and the first RESULT still wins.
         */
        int known_worker = -1;
        int known_exit = -1;
        int known_state = -1;

        if (coordinator_lookup_completed(table,
                                         task_id,
                                         &known_worker,
                                         &known_exit,
                                         &known_state))
        {
            log_debug("result_duplicate",
                      "Duplicate result for task %d; already %d.",
                      task_id,
                      known_state);
        }
        else
        {
            log_warn("result_unknown_task",
                     "Result for unknown task %d (worker %d).",
                     task_id,
                     worker_id);
        }

        pool_send_message(conn, "TASK_RESULT_ACK");
        return 1;
    }

    /*
     * The worker that reports a result must be the worker the
     * task is currently assigned to. A result from an earlier
     * attempt is stale: accepting it could overwrite the
     * outcome of a later execution.
     */
    if (worker_id != original_worker)
    {
        log_warn("result_worker_mismatch",
                 "Task %d is assigned to worker %d but worker %d "
                 "reported a result; rejecting the stale result.",
                 task_id,
                 original_worker,
                 worker_id);

        pool_send_message(conn, "TASK_RESULT_REJECT");
        return 0;
    }

    /*
     * Validate epoch: the result must match the task's epoch.
     * This rejects stale results from previous dispatch attempts
     * (e.g., a worker that was slow to respond, or a network
     * partition that caused a duplicate dispatch).
     */
    if (task.epoch != epoch)
    {
        log_warn("result_epoch_mismatch",
                 "TASK_RESULT for task %d: expected epoch %d, got %d",
                 task_id,
                 task.epoch,
                 epoch);

        pool_send_message(conn, "TASK_RESULT_REJECT");
        return 0;
    }

    /*
     * Persist the first result before removing the in-flight
     * entry. If the append fails, the task stays in-flight and
     * the worker's result can be retried (or the task is reaped
     * by policy after a restart).
     */
    if (!append_result_record(runtime,
                              task_id,
                              worker_id,
                              exit_status,
                              state))
    {
        return 0;
    }

    if (!coordinator_take_inflight(table,
                                   task_id,
                                   &task,
                                   &original_worker))
    {
        log_warn("result_race_lost",
                 "Task %d left in-flight between peek and take",
                 task_id);
    }

    task.exit_status = exit_status;
    task.state = (enum TaskState)state;

    /*
     * Release the worker's task slot now that the task is
     * terminal. The slot was reserved at dispatch time, so
     * every result path must release it exactly once.
     */
    worker_release_slot(&runtime->registry, worker_id);

    record_task_completion(&task.metrics);

    coordinator_record_completed(table,
                                 task_id,
                                 worker_id,
                                 exit_status,
                                 state);

    log_info("task_result",
             "Result: task=%d worker=%d exit=%d state=%d",
             task_id,
             worker_id,
             exit_status,
             state);

    log_info("task_latency",
             "task_id=%d worker_id=%d exit=%d state=%d "
             "queue_wait_ms=%.0f dispatch_to_result_ms=%.0f "
             "end_to_end_ms=%.0f",
             task_id,
             worker_id,
             exit_status,
             state,
             task.metrics.queue_wait_seconds * 1000.0,
             task.metrics.execution_seconds * 1000.0,
             (task.metrics.queue_wait_seconds
              + task.metrics.execution_seconds) * 1000.0);

    if (task.state == TASK_COMPLETED)
    {
        record_task_completed(&runtime->stats,
                              task.metrics.queue_wait_seconds,
                              task.metrics.execution_seconds);
    }
    else if (task.state == TASK_TIMED_OUT)
    {
        record_task_timed_out(&runtime->stats);
    }
    else if (task.state == TASK_UNKNOWN)
    {
        record_task_unknown(&runtime->stats);
    }
    else
    {
        record_task_failed(&runtime->stats);
    }
    pool_send_message(conn, "TASK_RESULT_ACK");

    return 1;
}

int coordinator_handle_query(struct Runtime *runtime,
                             struct InFlightTable *table,
                             struct Connection *conn,
                             const char *message)
{
    if (runtime == NULL || table == NULL || conn == NULL ||
        message == NULL)
    {
        return 0;
    }

    const char *prefix = "QUERY|";
    size_t prefix_length = strlen(prefix);

    if (strncmp(message, prefix, prefix_length) != 0)
    {
        pool_send_message(conn, "QUERY_UNKNOWN|-1");
        return 0;
    }

    const char *cursor = message + prefix_length;

    long task_id = 0;

    if (!wire_read_int(&cursor, &task_id) || task_id <= 0)
    {
        pool_send_message(conn, "QUERY_UNKNOWN|-1");
        return 0;
    }

    char response[128];
    int n;

    if (coordinator_is_inflight(table, (int)task_id))
    {
        n = snprintf(response,
                     sizeof(response),
                     "QUERY_PENDING|%ld",
                     task_id);
        if (n <= 0 || (size_t)n >= sizeof(response))
        {
            return 0;
        }
        pool_send_message(conn, response);
        return 1;
    }

    int worker_id = -1;
    int exit_status = -1;
    int state = -1;

    if (coordinator_lookup_completed(table,
                                     (int)task_id,
                                     &worker_id,
                                     &exit_status,
                                     &state))
    {
        n = snprintf(response,
                     sizeof(response),
                     "QUERY_RESULT|%ld|%d|%d|%d",
                     task_id,
                     worker_id,
                     exit_status,
                     state);
        if (n <= 0 || (size_t)n >= sizeof(response))
        {
            return 0;
        }
        pool_send_message(conn, response);
        return 1;
    }

    /*
     * A task that is still waiting in the queue has been accepted
     * and will be dispatched, so it is pending rather than
     * unknown. v0.3.0 returns the same QUERY_PENDING reply used
     * for in-flight tasks; a separate QUERY_PENDING_QUEUED reply
     * that exposes the queue position remains a v1.0 target.
     */
    if (runtime->task_manager != NULL &&
        task_queue_contains(&runtime->task_manager->queue,
                            (int)task_id))
    {
        n = snprintf(response,
                     sizeof(response),
                     "QUERY_PENDING|%ld",
                     task_id);
        if (n <= 0 || (size_t)n >= sizeof(response))
        {
            return 0;
        }
        pool_send_message(conn, response);
        return 1;
    }

    /*
     * IDs below next_task_id were allocated. If they are not
     * queued, in-flight, or retained, their result has aged out
     * of the retention window (or the submission was rejected
     * after allocation). Say so explicitly instead of claiming
     * the ID never existed.
     */
    if ((int)task_id < table->next_task_id)
    {
        n = snprintf(response,
                     sizeof(response),
                     "QUERY_EXPIRED|%ld",
                     task_id);
        if (n <= 0 || (size_t)n >= sizeof(response))
        {
            return 0;
        }
        pool_send_message(conn, response);
        return 1;
    }

    n = snprintf(response,
                 sizeof(response),
                 "QUERY_UNKNOWN|%ld",
                 task_id);
    if (n <= 0 || (size_t)n >= sizeof(response))
    {
        return 0;
    }
    pool_send_message(conn, response);
    return 1;
}

int coordinator_handle_status(struct Runtime *runtime,
                              struct InFlightTable *table,
                              struct Connection *conn)
{
    if (runtime == NULL || table == NULL || conn == NULL)
    {
        return 0;
    }

    int workers = runtime->registry.count;
    int active = 0;

    for (int i = 0; i < runtime->registry.count; i++)
    {
        if (runtime->registry.workers[i].status == WORKER_ACTIVE)
        {
            active++;
        }
    }

    int queue = 0;

    if (runtime->task_manager != NULL)
    {
        queue = runtime->task_manager->queue.count;
    }

    int inflight = 0;

    for (int i = 0; i < COORDINATOR_MAX_INFLIGHT; i++)
    {
        if (table->entries[i].in_use)
        {
            inflight++;
        }
    }

    int submitted = runtime->stats.tasks_submitted;
    int completed = runtime->stats.tasks_completed;
    int failed = runtime->stats.tasks_failed;
    int timed_out = runtime->stats.tasks_timed_out;
    int unknown = runtime->stats.tasks_unknown;

    char response[320];

    int n = snprintf(response,
                     sizeof(response),
                     "STATUS_REPLY|workers=%d|active=%d|queue=%d|"
                     "inflight=%d|submitted=%d|completed=%d|"
                     "failed=%d|timed_out=%d|unknown=%d",
                     workers,
                     active,
                     queue,
                     inflight,
                     submitted,
                     completed,
                     failed,
                     timed_out,
                     unknown);

    if (n <= 0 || (size_t)n >= sizeof(response))
    {
        return 0;
    }

    pool_send_message(conn, response);

    log_info("status_reported",
             "STATUS workers=%d active=%d queue=%d inflight=%d "
             "submitted=%d completed=%d failed=%d timed_out=%d "
             "unknown=%d",
             workers,
             active,
             queue,
             inflight,
             submitted,
             completed,
             failed,
             timed_out,
             unknown);
    return 1;
}

int coordinator_reap_dead_worker_tasks(struct Runtime *runtime,
                                       struct InFlightTable *table)
{
    if (runtime == NULL || table == NULL)
    {
        return 0;
    }

    int reaped = 0;

    int i;
    for (i = 0; i < COORDINATOR_MAX_INFLIGHT; i++)
    {
        if (!table->entries[i].in_use)
        {
            continue;
        }

        int worker_id = table->entries[i].worker_id;
        int task_id = table->entries[i].task_id;

        if (is_worker_live(&runtime->registry, worker_id))
        {
            continue;
        }

        struct Task task;
        int reported_worker = -1;

        if (!coordinator_peek_inflight(table,
                                       task_id,
                                       &task,
                                       &reported_worker))
        {
            continue;
        }

        if (reconcile_abandoned_task(runtime,
                                     table,
                                     task_id,
                                     reported_worker,
                                     &task,
                                     "worker_gone"))
        {
            log_info("in_flight_task_reaped",
                     "Reaped in-flight task %d: worker %d not live.",
                     task_id,
                     reported_worker);
            reaped++;
        }
    }

    return reaped;
}

int coordinator_reap_unaccepted_tasks(struct Runtime *runtime,
                                      struct InFlightTable *table)
{
    if (runtime == NULL || table == NULL)
    {
        return 0;
    }

    time_t now = time(NULL);
    int reaped = 0;

    for (int i = 0; i < COORDINATOR_MAX_INFLIGHT; i++)
    {
        if (!table->entries[i].in_use)
        {
            continue;
        }

        if (table->entries[i].accepted)
        {
            continue;
        }

        double age = difftime(now, table->entries[i].dispatched_at);

        if (age <= (double)DISPATCH_TIMEOUT_SECONDS)
        {
            continue;
        }

        int task_id = table->entries[i].task_id;

        struct Task task;
        int reported_worker = -1;

        if (!coordinator_peek_inflight(table,
                                       task_id,
                                       &task,
                                       &reported_worker))
        {
            continue;
        }

        if (reconcile_abandoned_task(runtime,
                                     table,
                                     task_id,
                                     reported_worker,
                                     &task,
                                     "dispatch_timeout"))
        {
            reaped++;
        }
    }

    return reaped;
}
int coordinator_reap_worker_tasks(struct Runtime *runtime,
                                  struct InFlightTable *table,
                                  int worker_id)
{
    if (runtime == NULL || table == NULL || worker_id <= 0)
    {
        return 0;
    }

    int reaped = 0;

    for (int i = 0; i < COORDINATOR_MAX_INFLIGHT; i++)
    {
        if (!table->entries[i].in_use)
        {
            continue;
        }

        if (table->entries[i].worker_id != worker_id)
        {
            continue;
        }

        int task_id = table->entries[i].task_id;
        struct Task task;
        int reported_worker = -1;

        if (!coordinator_peek_inflight(table,
                                       task_id,
                                       &task,
                                       &reported_worker))
        {
            continue;
        }

        if (reconcile_abandoned_task(runtime,
                                     table,
                                     task_id,
                                     reported_worker,
                                     &task,
                                     "worker_reincarnated"))
        {
            log_warn("in_flight_task_reaped_incarnation",
                     "Reaped in-flight task %d: worker %d re-registered.",
                     task_id,
                     reported_worker);
            reaped++;
        }
    }

    return reaped;
}
int coordinator_handle_metrics(struct Runtime *runtime,
                               struct InFlightTable *table,
                               struct Connection *conn)
{
    if (runtime == NULL || table == NULL || conn == NULL)
    {
        return 0;
    }

    int queue = 0;
    if (runtime->task_manager != NULL)
    {
        queue = runtime->task_manager->queue.count;
    }

    int inflight = 0;
    for (int i = 0; i < COORDINATOR_MAX_INFLIGHT; i++)
    {
        if (table->entries[i].in_use)
        {
            inflight++;
        }
    }

    double avg_qw_ms = get_average_queue_wait(&runtime->stats) * 1000.0;
    double avg_exec_ms = get_average_execution_time(&runtime->stats) * 1000.0;

    char response[512];
    int n = snprintf(response, sizeof(response),
                     "METRICS_REPLY|queue=%d|inflight=%d|"
                     "submitted=%d|completed=%d|failed=%d|timed_out=%d|"
                     "unknown=%d|"
                     "avg_queue_wait_ms=%.2f|avg_exec_ms=%.2f",
                     queue,
                     inflight,
                     runtime->stats.tasks_submitted,
                     runtime->stats.tasks_completed,
                     runtime->stats.tasks_failed,
                     runtime->stats.tasks_timed_out,
                     runtime->stats.tasks_unknown,
                     avg_qw_ms,
                     avg_exec_ms);

    if (n <= 0 || (size_t)n >= sizeof(response))
    {
        return 0;
    }

    pool_send_message(conn, response);

    log_info("metrics_reported",
             "METRICS queue=%d inflight=%d submitted=%d completed=%d "
             "failed=%d timed_out=%d unknown=%d "
             "avg_qw_ms=%.2f avg_exec_ms=%.2f",
             queue,
             inflight,
             runtime->stats.tasks_submitted,
             runtime->stats.tasks_completed,
             runtime->stats.tasks_failed,
             runtime->stats.tasks_timed_out,
             runtime->stats.tasks_unknown,
             avg_qw_ms,
             avg_exec_ms);

    return 1;
}
