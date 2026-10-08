#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <netinet/in.h>

#include "worker_server.h"
#include "network_protocol.h"

int start_worker_server(struct Worker *worker,
                        int port)
{
    if (worker == NULL)
    {
        return 0;
    }

    /*
     * Create TCP socket.
     */
    int server_socket =
        socket(AF_INET, SOCK_STREAM, 0);

    if (server_socket < 0)
    {
        perror("socket");
        return 0;
    }

    /*
     * Allow the port to be reused
     * when restarting the worker.
     */
    int reuse = 1;

    if (setsockopt(
            server_socket,
            SOL_SOCKET,
            SO_REUSEADDR,
            &reuse,
            sizeof(reuse)) < 0)
    {
        perror("setsockopt");
        close(server_socket);
        return 0;
    }

    /*
     * Configure server address.
     */
    struct sockaddr_in server_address = {0};

    server_address.sin_family = AF_INET;

    server_address.sin_addr.s_addr =
        htonl(INADDR_ANY);

    server_address.sin_port =
        htons(port);

    /*
     * Bind socket to port.
     */
    if (bind(
            server_socket,
            (struct sockaddr *)&server_address,
            sizeof(server_address)) < 0)
    {
        perror("bind");
        close(server_socket);
        return 0;
    }

    /*
     * Start listening.
     */
    if (listen(server_socket, 5) < 0)
    {
        perror("listen");
        close(server_socket);
        return 0;
    }

    printf("\n");
    printf("========================================\n");
    printf("          WORKER TCP SERVER\n");
    printf("========================================\n");

    printf("Worker ID : %d\n", worker->id);
    printf("Port      : %d\n", port);
    printf("Status    : LISTENING\n");

    printf("========================================\n");

    /*
     * Accept one client connection.
     */
    struct sockaddr_in client_address;

    socklen_t client_length =
        sizeof(client_address);

    int client_socket =
        accept(
            server_socket,
            (struct sockaddr *)&client_address,
            &client_length
        );

    if (client_socket < 0)
    {
        perror("accept");
        close(server_socket);
        return 0;
    }

    printf("Client connected.\n");

    /*
     * Receive one framed message.
     */
    char buffer[128];

    if (!receive_message(
            client_socket,
            buffer,
            sizeof(buffer)))
    {
        printf("Failed to receive message.\n");

        close(client_socket);
        close(server_socket);

        return 0;
    }

    printf("Received: %s\n", buffer);

    /*
     * Process the command.
     */
    if (strcmp(buffer, "PING") == 0)
    {
        if (!send_message(
                client_socket,
                "PONG"))
        {
            printf("Failed to send PONG.\n");

            close(client_socket);
            close(server_socket);

            return 0;
        }

        printf("Sent: PONG\n");
    }
    else
    {
        if (!send_message(
                client_socket,
                "UNKNOWN_COMMAND"))
        {
            printf("Failed to send response.\n");

            close(client_socket);
            close(server_socket);

            return 0;
        }

        printf("Sent: UNKNOWN_COMMAND\n");
    }

    close(client_socket);
    close(server_socket);

    printf("Worker server stopped.\n");

    return 1;
}
