#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "network_protocol.h"

int main(void)
{
    /*
     * Create TCP socket.
     */
    int client_socket =
        socket(AF_INET, SOCK_STREAM, 0);

    if (client_socket < 0)
    {
        perror("socket");
        return 1;
    }

    /*
     * Configure worker address.
     */
    struct sockaddr_in server_address = {0};

    server_address.sin_family = AF_INET;

    if (inet_pton(
            AF_INET,
            "127.0.0.1",
            &server_address.sin_addr) <= 0)
    {
        perror("inet_pton");
        close(client_socket);
        return 1;
    }

    server_address.sin_port =
        htons(5000);

    /*
     * Connect to worker.
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
     * Send framed PING message.
     */
    if (!send_message(
            client_socket,
            "PING"))
    {
        printf("Failed to send PING.\n");
        close(client_socket);
        return 1;
    }

    printf("Sent: PING\n");

    /*
     * Receive framed response.
     */
    char buffer[128];

    if (!receive_message(
            client_socket,
            buffer,
            sizeof(buffer)))
    {
        printf("Failed to receive response.\n");
        close(client_socket);
        return 1;
    }

    printf("Received: %s\n", buffer);

    close(client_socket);

    /*
     * Verify response.
     */
    if (strcmp(buffer, "PONG") == 0)
    {
        printf("Framed PING/PONG test passed. ✅\n");
        return 0;
    }

    printf("Framed PING/PONG test failed. ❌\n");

    return 1;
}
