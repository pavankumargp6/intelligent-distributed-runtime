/*
 * Minimal IDR query client.
 *
 * Usage:
 *   ./client_query <task_id>
 *
 * Sends QUERY|<id>, verifies the signed response, and prints
 * it. Exits 0 only for QUERY_RESULT|..., 1 otherwise.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "network_protocol.h"
#include "auth.h"
#include "auth_config.h"

#define RUNTIME_ADDRESS "127.0.0.1"
#define RUNTIME_SERVER_PORT 5001

int main(int argc, char *argv[])
{
    if (argc != 2)
    {
        fprintf(stderr, "Usage: %s <task_id>\n", argv[0]);
        return 2;
    }

    char *end = NULL;
    long task_id = strtol(argv[1], &end, 10);

    if (end == argv[1] || *end != '\0' || task_id <= 0)
    {
        fprintf(stderr, "task_id must be a positive integer\n");
        return 2;
    }

    if (!auth_config_init())
    {
        fprintf(stderr,
                "Authentication config invalid. Set "
                "IDR_WORKER_SECRET and IDR_CLIENT_SECRET, or "
                "IDR_INSECURE=1.\n");
        return 1;
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

    char plain[64];
    int n = snprintf(plain, sizeof(plain), "QUERY|%ld", task_id);

    if (n <= 0 || (size_t)n >= sizeof(plain))
    {
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

    printf("Sending: %s\n", plain);
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

    if (strncmp(final_response, "QUERY_RESULT|", 13) == 0)
    {
        return 0;
    }

    return 1;
}
