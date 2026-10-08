#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "worker.h"
#include "worker_registration.h"
#include "network_protocol.h"

#define RUNTIME_SERVER_PORT 5001

int register_worker_with_runtime(
    struct Worker *worker,
    const char *runtime_address)
{
    if (worker == NULL ||
        runtime_address == NULL)
    {
        return 0;
    }

    /*
     * Create TCP socket.
     */
    int socket_fd =
        socket(AF_INET, SOCK_STREAM, 0);

    if (socket_fd < 0)
    {
        perror("socket");
        return 0;
    }

    /*
     * Configure runtime server address.
     */
    struct sockaddr_in server_address = {0};

    server_address.sin_family = AF_INET;

    if (inet_pton(
            AF_INET,
            runtime_address,
            &server_address.sin_addr) <= 0)
    {
        perror("inet_pton");
        close(socket_fd);
        return 0;
    }

    server_address.sin_port =
        htons(RUNTIME_SERVER_PORT);

    /*
     * Connect to runtime coordinator.
     */
    if (connect(
            socket_fd,
            (struct sockaddr *)&server_address,
            sizeof(server_address)) < 0)
    {
        perror("connect");
        close(socket_fd);
        return 0;
    }

    printf("Connected to runtime.\n");

    /*
     * Build registration message.
     */
    char registration_message[
        WORKER_REGISTRATION_MESSAGE_SIZE
    ];

    if (!build_worker_registration(
            worker,
            registration_message,
            sizeof(registration_message)))
    {
        printf(
            "Failed to build registration message.\n"
        );

        close(socket_fd);
        return 0;
    }

    printf(
        "Registration: %s\n",
        registration_message
    );

    /*
     * Send framed registration message.
     */
    if (!send_message(
            socket_fd,
            registration_message))
    {
        printf(
            "Failed to send registration.\n"
        );

        close(socket_fd);
        return 0;
    }

    /*
     * Wait for coordinator response.
     */
    char response[128];

    if (!receive_message(
            socket_fd,
            response,
            sizeof(response)))
    {
        printf(
            "Failed to receive registration response.\n"
        );

        close(socket_fd);
        return 0;
    }

    printf(
        "Runtime response: %s\n",
        response
    );

    close(socket_fd);

    /*
     * Verify registration acknowledgement.
     */
    if (strcmp(
            response,
            "REGISTER_ACK") == 0)
    {
        printf(
            "Worker registration successful. ✅\n"
        );

        return 1;
    }

    printf(
        "Worker registration rejected. ❌\n"
    );

    return 0;
}
