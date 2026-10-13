/*
 * Minimal IDR client.
 *
 * Usage:
 *   ./client_submit [--idempotent] <command> [args...]
 *
 * Connects to the coordinator, sends SUBMIT|..., verifies the
 * signed response, and prints it.
 *
 * In secure mode (default), the request is signed with
 * IDR_CLIENT_SECRET. Set IDR_INSECURE=1 to disable.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "network_protocol.h"
#include "wire_fields.h"
#include "auth.h"
#include "auth_config.h"

#define RUNTIME_ADDRESS "127.0.0.1"
#define RUNTIME_SERVER_PORT 5001

int main(int argc, char *argv[])
{
    int idempotent = 0;
    int first_arg = 1;

    if (argc >= 2 && strcmp(argv[1], "--idempotent") == 0)
    {
        idempotent = 1;
        first_arg = 2;
    }

    if (argc <= first_arg)
    {
        fprintf(stderr,
                "Usage: %s [--idempotent] <command> [args...]\n",
                argv[0]);
        return 2;
    }

    if (!auth_config_init())
    {
        fprintf(stderr,
                "Authentication config invalid. Set "
                "IDR_WORKER_SECRET and IDR_CLIENT_SECRET "
                "(>= %d bytes each), or IDR_INSECURE=1.\n",
                AUTH_CONFIG_MIN_SECRET_BYTES);
        return 1;
    }

    char plain[NETWORK_MAX_MESSAGE_SIZE];

    memcpy(plain, "SUBMIT", 6);
    size_t offset = 6;

    offset = wire_append_delimiter(plain, sizeof(plain), offset);
    if (offset == 0) { fprintf(stderr, "build failed at 1\n"); return 1; }

    offset = wire_append_field(plain, sizeof(plain), offset, argv[first_arg]);
    if (offset == 0) { fprintf(stderr, "build failed at 2\n"); return 1; }

    offset = wire_append_delimiter(plain, sizeof(plain), offset);
    if (offset == 0) { fprintf(stderr, "build failed at 3\n"); return 1; }

    offset = wire_append_int(plain, sizeof(plain), offset, argc - first_arg - 1);
    if (offset == 0) { fprintf(stderr, "build failed at 4\n"); return 1; }

    for (int i = first_arg + 1; i < argc; i++)
    {
        offset = wire_append_delimiter(plain, sizeof(plain), offset);
        if (offset == 0) { fprintf(stderr, "build failed at 5\n"); return 1; }

        offset = wire_append_field(plain, sizeof(plain), offset, argv[i]);
        if (offset == 0) { fprintf(stderr, "build failed at 6\n"); return 1; }
    }

    if (idempotent)
    {
        offset = wire_append_delimiter(plain, sizeof(plain), offset);
        if (offset == 0) { fprintf(stderr, "build failed at 7\n"); return 1; }

        offset = wire_append_int(plain, sizeof(plain), offset, 1);
        if (offset == 0) { fprintf(stderr, "build failed at 8\n"); return 1; }
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    if (inet_pton(AF_INET, RUNTIME_ADDRESS, &addr.sin_addr) <= 0)
    {
        perror("inet_pton");
        close(fd);
        return 1;
    }
    addr.sin_port = htons(RUNTIME_SERVER_PORT);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("connect");
        close(fd);
        return 1;
    }

    char to_send[AUTH_MAX_SIGNED_SIZE];
    const char *payload = plain;

    if (auth_config_is_secure())
    {
        AuthResult r = auth_sign(auth_config_client_secret(),
                                 AUTH_DIR_CLIENT_TO_COORD,
                                 AUTH_ROLE_CLIENT,
                                 plain,
                                 to_send,
                                 sizeof(to_send));
        if (r != AUTH_OK)
        {
            fprintf(stderr, "auth_sign failed: %s\n",
                    auth_result_name(r));
            close(fd);
            return 1;
        }
        payload = to_send;
    }

    printf("Submitting: %s\n", plain);
    fflush(stdout);

    if (!send_message(fd, payload))
    {
        fprintf(stderr, "send failed\n");
        close(fd);
        return 1;
    }

    char response[AUTH_MAX_SIGNED_SIZE];
    if (!receive_message(fd, response, sizeof(response)))
    {
        fprintf(stderr, "receive failed\n");
        close(fd);
        return 1;
    }

    close(fd);

    char verified[AUTH_MAX_SIGNED_SIZE];
    const char *final_response = response;

    if (auth_config_is_secure())
    {
        AuthResult r = auth_verify(auth_config_client_secret(),
                                   AUTH_DIR_COORD_TO_CLIENT,
                                   AUTH_ROLE_COORDINATOR,
                                   response,
                                   verified,
                                   sizeof(verified));
        if (r != AUTH_OK)
        {
            fprintf(stderr, "auth_verify failed: %s\n",
                    auth_result_name(r));
            return 1;
        }
        final_response = verified;
    }

    printf("Response: %s\n", final_response);

    if (strncmp(final_response, "SUBMIT_ACK|", 11) == 0)
    {
        printf("PASS: task accepted, id=%s\n", final_response + 11);
        return 0;
    }

    printf("FAIL: submission rejected\n");
    return 1;
}
