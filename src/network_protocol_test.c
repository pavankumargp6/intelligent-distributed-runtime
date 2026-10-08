#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>

#include "network_protocol.h"

int main(void)
{
    /*
     * Create a pair of connected sockets.
     *
     * socketpair() gives us two endpoints
     * that can communicate with each other.
     *
     * This lets us test our protocol without
     * needing a real network server.
     */
    int sockets[2];

    if (socketpair(
            AF_UNIX,
            SOCK_STREAM,
            0,
            sockets) < 0)
    {
        perror("socketpair");
        return 1;
    }

    printf("Socket pair created. ✅\n");

    /*
     * Send a framed message through
     * the first socket.
     */
    const char *message = "PING";

    if (!send_message(
            sockets[0],
            message))
    {
        printf("Failed to send message. ❌\n");

        close(sockets[0]);
        close(sockets[1]);

        return 1;
    }

    printf("Sent message: %s\n", message);

    /*
     * Receive the framed message through
     * the second socket.
     */
    char buffer[128];

    if (!receive_message(
            sockets[1],
            buffer,
            sizeof(buffer)))
    {
        printf("Failed to receive message. ❌\n");

        close(sockets[0]);
        close(sockets[1]);

        return 1;
    }

    printf("Received message: %s\n", buffer);

    /*
     * Verify that the received message
     * matches what was sent.
     */
    if (strcmp(buffer, message) == 0)
    {
        printf("Message framing test passed. ✅\n");
    }
    else
    {
        printf("Message framing test failed. ❌\n");

        close(sockets[0]);
        close(sockets[1]);

        return 1;
    }

    /*
     * Close both socket endpoints.
     */
    close(sockets[0]);
    close(sockets[1]);

    return 0;
}
