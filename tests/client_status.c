/*
 * Minimal IDR status client.
 *
 * Sends STATUS and prints the coordinator's reply.
 * Exits 0 on STATUS_REPLY, nonzero otherwise.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "network_protocol.h"

#define RUNTIME_ADDRESS "127.0.0.1"
#define RUNTIME_SERVER_PORT 5001

int main(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0)
    {
        perror("socket");
        return 1;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;

    if (inet_pton(AF_INET,
                  RUNTIME_ADDRESS,
                  &addr.sin_addr) <= 0)
    {
        perror("inet_pton");
        close(fd);
        return 1;
    }

    addr.sin_port = htons(RUNTIME_SERVER_PORT);

    if (connect(fd,
                (struct sockaddr *)&addr,
                sizeof(addr)) < 0)
    {
        perror("connect");
        close(fd);
        return 1;
    }

    if (!send_message(fd, "STATUS"))
    {
        fprintf(stderr, "send failed\n");
        close(fd);
        return 1;
    }

    char response[256];

    if (!receive_message(fd, response, sizeof(response)))
    {
        fprintf(stderr, "receive failed\n");
        close(fd);
        return 1;
    }

    close(fd);

    printf("%s\n", response);

    if (strncmp(response, "STATUS_REPLY|", 13) == 0)
    {
        return 0;
    }

    return 1;
}
