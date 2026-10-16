#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "coordinator_handlers.h"
#include "network_protocol.h"
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

    record_task_start(&task.metrics);

    if (!coordinator_record_inflight(table, &task, selected->id))
    {
        log_warn("inflight_full",
                 "In-flight table full; requeueing task %d.",
                 task.id);
        submit_task(runtime->task_manager, &task);
        pool_send_message(conn, "NO_TASK");
        return 0;
    }

    char assign[NETWORK_MAX_MESSAGE_SIZE];

    if (!build_task_assign(&task, assign, sizeof(assign)))
    {
        log_error("assign_build_failed",
                  "Could not build TASK_ASSIGN for task %d.",
                  task.id);
        coordinator_take_inflight(table, task.id, NULL, NULL);
        submit_task(runtime->task_manager, &task);
        pool_send_message(conn, "NO_TASK");
        return 0;
    }

    if (!pool_send_message(conn, assign))
    {
        log_warn("assign_send_failed",
                 "Failed to queue TASK_ASSIGN for task %d.",
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
    (void)runtime;

    if (table == NULL || conn == NULL || message == NULL)
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

    if (!wire_read_int(&cursor, &task_id) || task_id <= 0)
    {
        pool_send_message(conn, "TASK_ACCEPT_REJECT|-1|bad_id");
        return 0;
    }

    if (!coordinator_mark_accepted(table, (int)task_id))
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

    log_debug("task_accepted_by_worker",
              "Task %ld accepted by worker", task_id);

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

    int fields = sscanf(message,
                        "TASK_RESULT|%d|%d|%d|%d",
                        &task_id,
                        &worker_id,
                        &exit_status,
                        &state);

    if (fields != 4)
    {
        log_warn("result_malformed",
                 "Malformed TASK_RESULT: %s",
                 message);
        pool_send_message(conn, "UNKNOWN_COMMAND");
        return 0;
    }

    struct Task task;
    int original_worker = -1;

    if (!coordinator_take_inflight(table,
                                   task_id,
                                   &task,
                                   &original_worker))
    {
        log_warn("result_unknown_task",
                 "Result for unknown task %d (worker %d).",
                 task_id,
                 worker_id);

        pool_send_message(conn, "TASK_RESULT_ACK");
        return 0;
    }

    task.exit_status = exit_status;
    task.state = (enum TaskState)state;

    record_task_completion(&task.metrics);

    char wal_line[128];

    int n_wal = snprintf(wal_line,
                         sizeof(wal_line),
                         "RESULT|%d|%d|%d|%d",
                         task_id,
                         worker_id,
                         exit_status,
                         state);

    if (n_wal > 0 && (size_t)n_wal < sizeof(wal_line))
    {
        if (!wal_append(&runtime->wal, wal_line))
        {
            log_warn("wal_append_failed",
                     "Could not append RESULT record for task %d.",
                     task_id);
        }
    }

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
    (void)runtime;

    if (table == NULL || conn == NULL || message == NULL)
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

    char response[256];

    int n = snprintf(response,
                     sizeof(response),
                     "STATUS_REPLY|workers=%d|active=%d|queue=%d|"
                     "inflight=%d|submitted=%d|completed=%d|failed=%d",
                     workers,
                     active,
                     queue,
                     inflight,
                     submitted,
                     completed,
                     failed);

    if (n <= 0 || (size_t)n >= sizeof(response))
    {
        return 0;
    }

    pool_send_message(conn, response);

    log_info("status_reported",
             "STATUS workers=%d active=%d queue=%d inflight=%d "
             "submitted=%d completed=%d failed=%d",
             workers,
             active,
             queue,
             inflight,
             submitted,
             completed,
             failed);

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

        if (!coordinator_take_inflight(table,
                                       task_id,
                                       &task,
                                       &reported_worker))
        {
            continue;
        }

        coordinator_record_completed(table,
                                     task_id,
                                     reported_worker,
                                     -1,
                                     (int)TASK_FAILED);

        record_task_failed(&runtime->stats);

        log_info("in_flight_task_reaped",
                 "Reaped in-flight task %d: worker %d not live.",
                 task_id,
                 reported_worker);

        reaped++;
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
        int worker_id = table->entries[i].worker_id;

        struct Task task;
        int reported_worker = -1;

        if (!coordinator_take_inflight(table,
                                       task_id,
                                       &task,
                                       &reported_worker))
        {
            continue;
        }

        if (task.idempotent)
        {
            task.assigned_worker_id = -1;
            task.state = TASK_CREATED;

            if (submit_task(runtime->task_manager, &task))
            {
                log_warn("task_requeued_timeout",
                         "Requeued idempotent task %d after dispatch "
                         "timeout (worker %d never accepted)",
                         task_id,
                         worker_id);
            }
            else
            {
                log_error("task_requeue_failed",
                          "Could not requeue idempotent task %d "
                          "after dispatch timeout", task_id);

                coordinator_record_completed(table,
                                             task_id,
                                             worker_id,
                                             -1,
                                             (int)TASK_FAILED);
                record_task_failed(&runtime->stats);
            }
        }
        else
        {
            coordinator_record_completed(table,
                                         task_id,
                                         worker_id,
                                         -1,
                                         (int)TASK_FAILED);
            record_task_failed(&runtime->stats);

            char wal_line[128];
            int n = snprintf(wal_line, sizeof(wal_line),
                             "RESULT|%d|%d|%d|%d",
                             task_id,
                             worker_id,
                             -1,
                             (int)TASK_FAILED);
            if (n > 0 && (size_t)n < sizeof(wal_line))
            {
                (void)wal_append(&runtime->wal, wal_line);
            }

            log_warn("task_failed_timeout",
                     "Task %d failed: dispatched to worker %d but "
                     "no TASK_ACCEPT within %d seconds "
                     "(not idempotent, not retried)",
                     task_id,
                     worker_id,
                     DISPATCH_TIMEOUT_SECONDS);
        }

        reaped++;
    }

    return reaped;
}
