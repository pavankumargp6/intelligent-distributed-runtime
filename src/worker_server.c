#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "worker_server.h"

int start_worker_server(struct Worker *worker,
                        int port)
{
    if (worker == NULL)
    {
        return 0;
    }

    /*
     * Create a TCP socket.
     */
    int server_socket =
        socket(AF_INET, SOCK_STREAM, 0);

    if (server_socket < 0)
    {
        perror("socket");
        return 0;
    }

    /*
     * Allow the operating system to
     * reuse the port after the server
     * is restarted.
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
     * Describe the server address.
     */
    struct sockaddr_in server_address = {0};

    server_address.sin_family = AF_INET;

    /*
     * Accept connections through any
     * local network interface.
     */
    server_address.sin_addr.s_addr =
        htonl(INADDR_ANY);

    /*
     * Convert the port to network
     * byte order.
     */
    server_address.sin_port =
        htons(port);

    /*
     * Bind the socket to the address
     * and port.
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
     * Put the socket into listening mode.
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
     * Wait for one incoming connection.
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
     * Receive a message from the client.
     */
    char buffer[128] = {0};

    ssize_t bytes_received =
        recv(
            client_socket,
            buffer,
            sizeof(buffer) - 1,
            0
        );

    if (bytes_received < 0)
    {
        perror("recv");
        close(client_socket);
        close(server_socket);
        return 0;
    }

    /*
     * TCP is a byte stream.
     *
     * Add a null terminator so that
     * the received bytes can safely
     * be treated as a C string.
     */
    buffer[bytes_received] = '\0';

    printf("Received: %s\n", buffer);

    /*
     * Respond to a PING request.
     */
    if (strcmp(buffer, "PING") == 0)
    {
        const char *response = "PONG";

        if (send(
                client_socket,
                response,
                strlen(response),
                0
            ) < 0)
        {
            perror("send");
            close(client_socket);
            close(server_socket);
            return 0;
        }

        printf("Sent: %s\n", response);
    }
    else
    {
        const char *response = "UNKNOWN_COMMAND";

        if (send(
                client_socket,
                response,
                strlen(response),
                0
            ) < 0)
        {
            perror("send");
            close(client_socket);
            close(server_socket);
            return 0;
        }

        printf("Sent: %s\n", response);
    }

    /*
     * Close the client connection.
     */
    close(client_socket);

    printf("Client disconnected.\n");

    /*
     * This first protocol implementation
     * handles one connection.
     */
    close(server_socket);

    printf("Worker server stopped.\n");

    return 1;
}
