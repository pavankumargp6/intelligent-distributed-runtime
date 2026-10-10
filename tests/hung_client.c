/*
 * Hung client stub. Connects to the coordinator, sends the
 * 4-byte length header claiming a 32-byte payload, and then
 * sends nothing for N seconds. Under the old blocking
 * coordinator, this would prevent any other client from
 * being served.
 *
 * Usage:
 *   ./hung_client <host> <port> <hold_seconds>
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

int main(int argc, char *argv[])
{
    if (argc != 4)
    {
        fprintf(stderr,
                "Usage: %s <host> <port> <hold_seconds>\n",
                argv[0]);
        return 2;
    }

    const char *host = argv[1];
    int port = atoi(argv[2]);
    int hold = atoi(argv[3]);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0)
    {
        perror("inet_pton");
        close(fd);
        return 1;
    }
    addr.sin_port = htons(port);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("connect");
        close(fd);
        return 1;
    }

    /* Claim a 32-byte payload, then send nothing. */
    uint32_t net_len = htonl(32);
    if (send(fd, &net_len, 4, 0) != 4)
    {
        perror("send");
        close(fd);
        return 1;
    }

    printf("hung_client: connected, sent 4-byte header, holding %d s\n",
           hold);
    fflush(stdout);

    sleep(hold);

    close(fd);
    return 0;
}
