/*
 * Wrong-role worker stub.
 *
 * Behaves like a worker for the purpose of registration, except
 * it signs the REGISTER message with IDR_CLIENT_SECRET instead
 * of IDR_WORKER_SECRET.
 *
 * The coordinator classifies REGISTER as a worker message and
 * verifies with the worker secret. The MAC was computed with
 * the wrong secret, so verification fails. The coordinator
 * closes the connection without responding.
 *
 * Exit codes:
 *   0  coordinator rejected the signed REGISTER (expected)
 *   1  coordinator accepted or otherwise responded (unexpected)
 *   2  usage / setup error
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "network_protocol.h"
#include "worker.h"
#include "worker_registration.h"
#include "auth.h"

#define RUNTIME_SERVER_PORT 5001

static int connect_to_runtime(const char *address)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0)
    {
        perror("socket");
        return -1;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;

    if (inet_pton(AF_INET, address, &addr.sin_addr) <= 0)
    {
        perror("inet_pton");
        close(fd);
        return -1;
    }

    addr.sin_port = htons(RUNTIME_SERVER_PORT);

    if (connect(fd,
                (struct sockaddr *)&addr,
                sizeof(addr)) < 0)
    {
        perror("connect");
        close(fd);
        return -1;
    }

    /*
     * Short receive timeout. The coordinator closes the
     * connection on auth failure; we want to detect either
     * a close (recv returns 0) or an unexpected response
     * within a couple of seconds.
     */
    struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    return fd;
}

int main(int argc, char *argv[])
{
    if (argc != 2)
    {
        fprintf(stderr, "Usage: %s <worker_id>\n", argv[0]);
        return 2;
    }

    int worker_id = atoi(argv[1]);

    if (worker_id <= 0)
    {
        fprintf(stderr, "worker_id must be positive\n");
        return 2;
    }

    const char *client_secret = getenv("IDR_CLIENT_SECRET");

    if (client_secret == NULL || client_secret[0] == '\0')
    {
        fprintf(stderr, "IDR_CLIENT_SECRET not set\n");
        return 2;
    }

    struct Worker worker;

    if (!initialize_worker(&worker, worker_id))
    {
        fprintf(stderr, "initialize_worker failed\n");
        return 2;
    }

    char plain[WORKER_REGISTRATION_MESSAGE_SIZE];

    if (!build_worker_registration(&worker, plain, sizeof(plain)))
    {
        fprintf(stderr, "build_worker_registration failed\n");
        return 2;
    }

    char signed_buf[AUTH_MAX_SIGNED_SIZE];

    AuthResult r = auth_sign(client_secret,
                             AUTH_DIR_WORKER_TO_COORD,
                             AUTH_ROLE_WORKER,
                             plain,
                             signed_buf,
                             sizeof(signed_buf));

    if (r != AUTH_OK)
    {
        fprintf(stderr, "auth_sign failed: %s\n",
                auth_result_name(r));
        return 2;
    }

    int fd = connect_to_runtime("127.0.0.1");

    if (fd < 0)
    {
        return 2;
    }

    if (!send_message(fd, signed_buf))
    {
        fprintf(stderr, "send failed\n");
        close(fd);
        return 2;
    }

    char response[AUTH_MAX_SIGNED_SIZE];

    if (!receive_message(fd, response, sizeof(response)))
    {
        /* No response — coordinator rejected as expected. */
        printf("wrong_role_worker: no response received (rejected)\n");
        close(fd);
        return 0;
    }

    printf("wrong_role_worker: unexpected response: %s\n", response);
    close(fd);
    return 1;
}
