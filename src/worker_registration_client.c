#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "worker.h"
#include "worker_registration.h"
#include "network_protocol.h"
#include "auth.h"
#include "auth_config.h"
#include "log.h"

#define RUNTIME_SERVER_PORT 5001

static int connect_to_runtime(const char *runtime_address)
{
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

int register_worker_with_runtime(
    struct Worker *worker,
    const char *runtime_address)
{
    if (worker == NULL || runtime_address == NULL)
    {
        return 0;
    }

    int fd = connect_to_runtime(runtime_address);

    if (fd < 0)
    {
        return 0;
    }

    char plain[WORKER_REGISTRATION_MESSAGE_SIZE];

    if (!build_worker_registration(worker, plain, sizeof(plain)))
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

    if (strcmp(verified, "REGISTER_ACK") == 0)
    {
        return 1;
    }

    return 0;
}

int send_worker_heartbeat(
    const struct Worker *worker,
    const char *runtime_address)
{
    if (worker == NULL || runtime_address == NULL)
    {
        return 0;
    }

    int fd = connect_to_runtime(runtime_address);

    if (fd < 0)
    {
        return 0;
    }

    char plain[128];

    if (!build_worker_heartbeat(worker, plain, sizeof(plain)))
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

    if (strcmp(verified, "HEARTBEAT_ACK") == 0)
    {
        return 1;
    }

    return 0;
}
