#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "worker_task_client.h"
#include "network_protocol.h"
#include "wire_fields.h"

#define RUNTIME_SERVER_PORT 5001

static int connect_to_runtime(const char *runtime_address)
{
    if (runtime_address == NULL)
    {
        return -1;
    }

    int socket_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (socket_fd < 0)
    {
        perror("socket");
        return -1;
    }

    struct sockaddr_in server_address = {0};

    server_address.sin_family = AF_INET;

    if (inet_pton(AF_INET,
                  runtime_address,
                  &server_address.sin_addr) <= 0)
    {
        perror("inet_pton");
        close(socket_fd);
        return -1;
    }

    server_address.sin_port = htons(RUNTIME_SERVER_PORT);

    if (connect(socket_fd,
                (struct sockaddr *)&server_address,
                sizeof(server_address)) < 0)
    {
        perror("connect");
        close(socket_fd);
        return -1;
    }

    return socket_fd;
}

static int parse_task_assign(const char *message,
                             struct Task *task)
{
    if (message == NULL || task == NULL)
    {
        return 0;
    }

    const char *prefix = "TASK_ASSIGN|";
    size_t prefix_length = strlen(prefix);

    if (strncmp(message, prefix, prefix_length) != 0)
    {
        return 0;
    }

    const char *cursor = message + prefix_length;

    long task_id = 0;

    if (!wire_read_int(&cursor, &task_id))
    {
        return 0;
    }

    if (!wire_read_delimiter(&cursor))
    {
        return 0;
    }

    memset(task, 0, sizeof(*task));
    task->id = (int)task_id;
    task->assigned_worker_id = -1;

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

int request_task_from_runtime(
    const struct Worker *worker,
    const char *runtime_address,
    struct Task *task,
    int *has_task)
{
    if (worker == NULL || runtime_address == NULL ||
        task == NULL || has_task == NULL)
    {
        return 0;
    }

    *has_task = 0;

    int socket_fd = connect_to_runtime(runtime_address);

    if (socket_fd < 0)
    {
        return 0;
    }

    char request[64];
    int request_length = snprintf(request,
                                  sizeof(request),
                                  "TASK_REQUEST|%d",
                                  worker->id);

    if (request_length <= 0 ||
        (size_t)request_length >= sizeof(request))
    {
        close(socket_fd);
        return 0;
    }

    if (!send_message(socket_fd, request))
    {
        close(socket_fd);
        return 0;
    }

    char response[WORKER_TASK_MESSAGE_SIZE];

    if (!receive_message(socket_fd,
                         response,
                         sizeof(response)))
    {
        close(socket_fd);
        return 0;
    }

    close(socket_fd);

    if (strcmp(response, "NO_TASK") == 0)
    {
        return 1;
    }

    if (!parse_task_assign(response, task))
    {
        return 0;
    }

    *has_task = 1;

    return 1;
}

int send_task_result_to_runtime(
    const struct Worker *worker,
    const char *runtime_address,
    const struct Task *task)
{
    if (worker == NULL || runtime_address == NULL ||
        task == NULL)
    {
        return 0;
    }

    int socket_fd = connect_to_runtime(runtime_address);

    if (socket_fd < 0)
    {
        return 0;
    }

    char message[256];
    int message_length = snprintf(
        message,
        sizeof(message),
        "TASK_RESULT|%d|%d|%d|%d",
        task->id,
        worker->id,
        task->exit_status,
        (int)task->state);

    if (message_length <= 0 ||
        (size_t)message_length >= sizeof(message))
    {
        close(socket_fd);
        return 0;
    }

    if (!send_message(socket_fd, message))
    {
        close(socket_fd);
        return 0;
    }

    char response[64];

    if (!receive_message(socket_fd,
                         response,
                         sizeof(response)))
    {
        close(socket_fd);
        return 0;
    }

    close(socket_fd);

    if (strcmp(response, "TASK_RESULT_ACK") != 0)
    {
        return 0;
    }

    return 1;
}
