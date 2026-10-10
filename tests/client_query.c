#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "network_protocol.h"

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

    char message[64];
    int n = snprintf(message, sizeof(message), "QUERY|%ld", task_id);

    if (n <= 0 || (size_t)n >= sizeof(message))
    {
        close(fd);
        return 1;
    }

    printf("Sending: %s\n", message);
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

    if (strncmp(response, "QUERY_RESULT|", 13) == 0)
    {
        return 0;
    }

    return 1;
}
