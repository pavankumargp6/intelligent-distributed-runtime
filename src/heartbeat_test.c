#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "network_protocol.h"

#define RUNTIME_SERVER_PORT 5001

int main(void)
{
    /*
     * Create TCP socket.
     */
    int socket_fd =
        socket(AF_INET, SOCK_STREAM, 0);

    if (socket_fd < 0)
    {
        perror("socket");
        return 1;
    }

    /*
     * Configure runtime address.
     */
    struct sockaddr_in server_address = {0};

    server_address.sin_family = AF_INET;

    if (inet_pton(
            AF_INET,
            "127.0.0.1",
            &server_address.sin_addr) <= 0)
    {
        perror("inet_pton");
        close(socket_fd);
        return 1;
    }

    server_address.sin_port =
        htons(RUNTIME_SERVER_PORT);

    /*
     * Connect to runtime.
     */
    if (connect(
            socket_fd,
            (struct sockaddr *)&server_address,
            sizeof(server_address)) < 0)
    {
        perror("connect");
        close(socket_fd);
        return 1;
    }

    printf("Connected to runtime.\n");

    /*
     * Build heartbeat message.
     */
    char message[128];

    snprintf(
        message,
        sizeof(message),
        "HEARTBEAT|1"
    );

    printf(
        "Sending: %s\n",
        message
    );

    /*
     * Send heartbeat.
     */
    if (!send_message(
            socket_fd,
            message))
    {
        printf(
            "Failed to send heartbeat.\n"
        );

        close(socket_fd);
        return 1;
    }

    /*
     * Receive acknowledgement.
     */
    char response[128];

    if (!receive_message(
            socket_fd,
            response,
            sizeof(response)))
    {
        printf(
            "Failed to receive response.\n"
        );

        close(socket_fd);
        return 1;
    }

    printf(
        "Runtime response: %s\n",
        response
    );

    close(socket_fd);

    /*
     * Verify acknowledgement.
     */
    if (strcmp(
            response,
            "HEARTBEAT_ACK") == 0)
    {
        printf(
            "Heartbeat test passed. ✅\n"
        );

        return 0;
    }

    printf(
        "Heartbeat test failed. ❌\n"
    );

    return 1;
}
