#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

int main(void)
{
    /*
     * Create a TCP socket.
     */
    int client_socket =
        socket(AF_INET, SOCK_STREAM, 0);

    if (client_socket < 0)
    {
        perror("socket");
        return 1;
    }

    /*
     * Describe the worker server.
     */
    struct sockaddr_in server_address = {0};

    server_address.sin_family = AF_INET;

    /*
     * The worker is running on the
     * same machine.
     */
    if (inet_pton(
            AF_INET,
            "127.0.0.1",
            &server_address.sin_addr) <= 0)
    {
        perror("inet_pton");
        close(client_socket);
        return 1;
    }

    /*
     * Worker TCP port.
     */
    server_address.sin_port = htons(5000);

    /*
     * Connect to the worker.
     */
    if (connect(
            client_socket,
            (struct sockaddr *)&server_address,
            sizeof(server_address)) < 0)
    {
        perror("connect");
        close(client_socket);
        return 1;
    }

    printf("Connected to worker.\n");

    /*
     * Send a PING message.
     */
    const char *message = "PING";

    if (send(
            client_socket,
            message,
            strlen(message),
            0) < 0)
    {
        perror("send");
        close(client_socket);
        return 1;
    }

    printf("Sent: %s\n", message);

    /*
     * Receive the worker's response.
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
        return 1;
    }

    buffer[bytes_received] = '\0';

    printf("Received: %s\n", buffer);

    /*
     * Close the connection.
     */
    close(client_socket);

    /*
     * Verify the expected response.
     */
    if (strcmp(buffer, "PONG") == 0)
    {
        printf("PING/PONG test passed. ✅\n");
        return 0;
    }

    printf("PING/PONG test failed. ❌\n");

    return 1;
}
