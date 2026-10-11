/*
 * Minimal IDR client.
 *
 * Usage:
 *   ./client_submit [--idempotent] <command> [args...]
 *
 * Connects to the coordinator, sends SUBMIT|..., and prints
 * the response.
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

    char message[NETWORK_MAX_MESSAGE_SIZE];

    memcpy(message, "SUBMIT", 6);
    size_t offset = 6;

    offset = wire_append_delimiter(message, sizeof(message), offset);
    if (offset == 0) { fprintf(stderr, "build failed at 1\n"); return 1; }

    offset = wire_append_field(message, sizeof(message), offset, argv[first_arg]);
    if (offset == 0) { fprintf(stderr, "build failed at 2\n"); return 1; }

    offset = wire_append_delimiter(message, sizeof(message), offset);
    if (offset == 0) { fprintf(stderr, "build failed at 3\n"); return 1; }

    offset = wire_append_int(message, sizeof(message), offset, argc - first_arg - 1);
    if (offset == 0) { fprintf(stderr, "build failed at 4\n"); return 1; }

    int i;
    for (i = first_arg + 1; i < argc; i++)
    {
        offset = wire_append_delimiter(message, sizeof(message), offset);
        if (offset == 0) { fprintf(stderr, "build failed at 5\n"); return 1; }

        offset = wire_append_field(message, sizeof(message), offset, argv[i]);
        if (offset == 0) { fprintf(stderr, "build failed at 6\n"); return 1; }
    }

    if (idempotent)
    {
        offset = wire_append_delimiter(message, sizeof(message), offset);
        if (offset == 0) { fprintf(stderr, "build failed at 7\n"); return 1; }

        offset = wire_append_int(message, sizeof(message), offset, 1);
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

    printf("Submitting: %s\n", message);
    fflush(stdout);

    if (!send_message(fd, message))
    {
        fprintf(stderr, "send failed\n");
        close(fd);
        return 1;
    }

    char response[128];
    if (!receive_message(fd, response, sizeof(response)))
    {
        fprintf(stderr, "receive failed\n");
        close(fd);
        return 1;
    }

    close(fd);

    printf("Response: %s\n", response);

    if (strncmp(response, "SUBMIT_ACK|", 11) == 0)
    {
        printf("PASS: task accepted, id=%s\n", response + 11);
        return 0;
    }

    printf("FAIL: submission rejected\n");
    return 1;
}
