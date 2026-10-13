#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "worker_task_client.h"
#include "network_protocol.h"
#include "wire_fields.h"
#include "auth.h"
#include "auth_config.h"
#include "log.h"

#define RUNTIME_SERVER_PORT 5001

static int connect_to_runtime(const char *runtime_address)
{
    if (runtime_address == NULL)
    {
        return -1;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0)
    {
        perror("socket");
        return -1;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;

    if (inet_pton(AF_INET, runtime_address, &addr.sin_addr) <= 0)
    {
        perror("inet_pton");
        close(fd);
        return -1;
    }

    addr.sin_port = htons(RUNTIME_SERVER_PORT);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("connect");
        close(fd);
        return -1;
    }

    return fd;
}

static int sign_if_secure(const char *plain,
                          char *signed_buf,
                          size_t signed_buf_size,
                          const char **out_message)
{
    if (!auth_config_is_secure())
    {
        *out_message = plain;
        return 1;
    }

    AuthResult r = auth_sign(auth_config_worker_secret(),
                             AUTH_DIR_WORKER_TO_COORD,
                             AUTH_ROLE_WORKER,
                             plain,
                             signed_buf,
                             signed_buf_size);

    if (r != AUTH_OK)
    {
        log_warn("sign_failed", "auth_sign: %s", auth_result_name(r));
        return 0;
    }

    *out_message = signed_buf;
    return 1;
}

static int verify_if_secure(const char *signed_message,
                            char *plain_buf,
                            size_t plain_buf_size,
                            const char **out_message)
{
    if (!auth_config_is_secure())
    {
        *out_message = signed_message;
        return 1;
    }

    AuthResult r = auth_verify(auth_config_worker_secret(),
                               AUTH_DIR_COORD_TO_WORKER,
                               AUTH_ROLE_COORDINATOR,
                               signed_message,
                               plain_buf,
                               plain_buf_size);

    if (r != AUTH_OK)
    {
        log_warn("verify_failed", "auth_verify: %s",
                 auth_result_name(r));
        return 0;
    }

    *out_message = plain_buf;
    return 1;
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

    for (int i = 0; i < (int)argument_count; i++)
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

    int fd = connect_to_runtime(runtime_address);

    if (fd < 0)
    {
        return 0;
    }

    char plain[64];
    int n = snprintf(plain, sizeof(plain),
                     "TASK_REQUEST|%d", worker->id);

    if (n <= 0 || (size_t)n >= sizeof(plain))
    {
        close(fd);
        return 0;
    }

    char signed_buf[AUTH_MAX_SIGNED_SIZE];
    const char *to_send = NULL;

    if (!sign_if_secure(plain, signed_buf, sizeof(signed_buf), &to_send))
    {
        close(fd);
        return 0;
    }

    if (!send_message(fd, to_send))
    {
        close(fd);
        return 0;
    }

    char response[AUTH_MAX_SIGNED_SIZE];

    if (!receive_message(fd, response, sizeof(response)))
    {
        close(fd);
        return 0;
    }

    close(fd);

    char plain_response[AUTH_MAX_SIGNED_SIZE];
    const char *verified = NULL;

    if (!verify_if_secure(response,
                          plain_response,
                          sizeof(plain_response),
                          &verified))
    {
        return 0;
    }

    if (strcmp(verified, "NO_TASK") == 0)
    {
        return 1;
    }

    if (!parse_task_assign(verified, task))
    {
        return 0;
    }

    *has_task = 1;

    return 1;
}

int send_task_accept_to_runtime(
    const struct Worker *worker,
    const char *runtime_address,
    int task_id)
{
    if (worker == NULL || runtime_address == NULL || task_id <= 0)
    {
        return 0;
    }

    int fd = connect_to_runtime(runtime_address);

    if (fd < 0)
    {
        return 0;
    }

    char plain[64];
    int n = snprintf(plain, sizeof(plain),
                     "TASK_ACCEPT|%d", task_id);

    if (n <= 0 || (size_t)n >= sizeof(plain))
    {
        close(fd);
        return 0;
    }

    char signed_buf[AUTH_MAX_SIGNED_SIZE];
    const char *to_send = NULL;

    if (!sign_if_secure(plain, signed_buf, sizeof(signed_buf), &to_send))
    {
        close(fd);
        return 0;
    }

    if (!send_message(fd, to_send))
    {
        close(fd);
        return 0;
    }

    char response[AUTH_MAX_SIGNED_SIZE];

    if (!receive_message(fd, response, sizeof(response)))
    {
        close(fd);
        return 0;
    }

    close(fd);

    char plain_response[AUTH_MAX_SIGNED_SIZE];
    const char *verified = NULL;

    if (!verify_if_secure(response,
                          plain_response,
                          sizeof(plain_response),
                          &verified))
    {
        return 0;
    }

    if (strncmp(verified, "TASK_ACCEPT_ACK|", 16) != 0)
    {
        log_warn("task_accept_rejected",
                 "Coordinator replied %s for task %d",
                 verified, task_id);
        return 0;
    }

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

    int fd = connect_to_runtime(runtime_address);

    if (fd < 0)
    {
        return 0;
    }

    char plain[256];
    int n = snprintf(plain, sizeof(plain),
                     "TASK_RESULT|%d|%d|%d|%d",
                     task->id,
                     worker->id,
                     task->exit_status,
                     (int)task->state);

    if (n <= 0 || (size_t)n >= sizeof(plain))
    {
        close(fd);
        return 0;
    }

    char signed_buf[AUTH_MAX_SIGNED_SIZE];
    const char *to_send = NULL;

    if (!sign_if_secure(plain, signed_buf, sizeof(signed_buf), &to_send))
    {
        close(fd);
        return 0;
    }

    if (!send_message(fd, to_send))
    {
        close(fd);
        return 0;
    }

    char response[AUTH_MAX_SIGNED_SIZE];

    if (!receive_message(fd, response, sizeof(response)))
    {
        close(fd);
        return 0;
    }

    close(fd);

    char plain_response[AUTH_MAX_SIGNED_SIZE];
    const char *verified = NULL;

    if (!verify_if_secure(response,
                          plain_response,
                          sizeof(plain_response),
                          &verified))
    {
        return 0;
    }

    if (strcmp(verified, "TASK_RESULT_ACK") != 0)
    {
        return 0;
    }

    return 1;
}
