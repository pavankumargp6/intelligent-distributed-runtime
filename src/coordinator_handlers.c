#include <stdio.h>
#include <string.h>

#include "coordinator_handlers.h"
#include "network_protocol.h"
#include "wire_fields.h"
#include "scheduler.h"
#include "task_manager.h"
#include "runtime_stats.h"
#include "worker_registry.h"

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

    return 1;
}

int coordinator_handle_submit(struct Runtime *runtime,
                              struct InFlightTable *table,
                              int client_socket,
                              const char *message)
{
    if (runtime == NULL || table == NULL || message == NULL)
    {
        return 0;
    }

    struct Task task;

    if (!parse_submit(message, &task))
    {
        printf("Invalid SUBMIT message.\n");
        send_message(client_socket, "SUBMIT_REJECT");
        return 0;
    }

    int id = coordinator_allocate_task_id(table);

    if (id <= 0)
    {
        printf("Could not allocate task ID.\n");
        send_message(client_socket, "SUBMIT_REJECT");
        return 0;
    }

    task.id = id;

    if (!runtime_submit_task(runtime, &task))
    {
        printf("runtime_submit_task rejected task %d.\n", id);
        send_message(client_socket, "SUBMIT_REJECT");
        return 0;
    }

    printf("Accepted task %d: %s argc=%d\n",
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
        send_message(client_socket, "SUBMIT_REJECT");
        return 0;
    }

    send_message(client_socket, response);

    return 1;
}

int coordinator_handle_task_request(struct Runtime *runtime,
                                    struct InFlightTable *table,
                                    int worker_socket,
                                    const char *message)
{
    if (runtime == NULL || table == NULL || message == NULL)
    {
        return 0;
    }

    const char *prefix = "TASK_REQUEST|";
    size_t prefix_length = strlen(prefix);

    if (strncmp(message, prefix, prefix_length) != 0)
    {
        send_message(worker_socket, "NO_TASK");
        return 0;
    }

    const char *cursor = message + prefix_length;

    long requesting_worker_id = 0;

    if (!wire_read_int(&cursor, &requesting_worker_id) ||
        requesting_worker_id <= 0)
    {
        send_message(worker_socket, "NO_TASK");
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
        send_message(worker_socket, "NO_TASK");
        return 1;
    }

    if (!coordinator_record_inflight(table, &task, selected->id))
    {
        printf("In-flight table full; requeueing task %d.\n", task.id);
        submit_task(runtime->task_manager, &task);
        send_message(worker_socket, "NO_TASK");
        return 0;
    }

    char assign[NETWORK_MAX_MESSAGE_SIZE];

    if (!build_task_assign(&task, assign, sizeof(assign)))
    {
        printf("Could not build TASK_ASSIGN for task %d.\n", task.id);
        coordinator_take_inflight(table, task.id, NULL, NULL);
        submit_task(runtime->task_manager, &task);
        send_message(worker_socket, "NO_TASK");
        return 0;
    }

    if (!send_message(worker_socket, assign))
    {
        printf("Failed to send TASK_ASSIGN for task %d.\n", task.id);
        return 0;
    }

    printf("Dispatched task %d to worker %d.\n",
           task.id,
           selected->id);

    return 1;
}

int coordinator_handle_task_result(struct Runtime *runtime,
                                   struct InFlightTable *table,
                                   int worker_socket,
                                   const char *message)
{
    if (runtime == NULL || table == NULL || message == NULL)
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
        printf("Malformed TASK_RESULT: %s\n", message);
        send_message(worker_socket, "UNKNOWN_COMMAND");
        return 0;
    }

    struct Task task;
    int original_worker = -1;

    if (!coordinator_take_inflight(table,
                                   task_id,
                                   &task,
                                   &original_worker))
    {
        printf("Result for unknown task %d (worker %d).\n",
               task_id,
               worker_id);

        send_message(worker_socket, "TASK_RESULT_ACK");
        return 0;
    }

    task.exit_status = exit_status;
    task.state = (enum TaskState)state;

    coordinator_record_completed(table,
                                 task_id,
                                 worker_id,
                                 exit_status,
                                 state);

    printf("Result: task=%d worker=%d exit=%d state=%d\n",
           task_id,
           worker_id,
           exit_status,
           state);

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

    send_message(worker_socket, "TASK_RESULT_ACK");

    return 1;
}

int coordinator_handle_query(struct Runtime *runtime,
                             struct InFlightTable *table,
                             int client_socket,
                             const char *message)
{
    (void)runtime;

    if (table == NULL || message == NULL)
    {
        return 0;
    }

    const char *prefix = "QUERY|";
    size_t prefix_length = strlen(prefix);

    if (strncmp(message, prefix, prefix_length) != 0)
    {
        send_message(client_socket, "QUERY_UNKNOWN|-1");
        return 0;
    }

    const char *cursor = message + prefix_length;

    long task_id = 0;

    if (!wire_read_int(&cursor, &task_id) || task_id <= 0)
    {
        send_message(client_socket, "QUERY_UNKNOWN|-1");
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
        send_message(client_socket, response);
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
        send_message(client_socket, response);
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
    send_message(client_socket, response);
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

        printf("Reaped in-flight task %d: worker %d not live.\n",
               task_id,
               reported_worker);

        reaped++;
    }

    return reaped;
}
