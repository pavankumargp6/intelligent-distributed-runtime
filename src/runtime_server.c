#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <netinet/in.h>

#include "runtime_server.h"
#include "network_protocol.h"
#include "worker_registration.h"

int start_runtime_server(
    struct Runtime *runtime,
    int port)
{
    if (runtime == NULL)
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
     * Allow the port to be reused when
     * the coordinator is restarted.
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
     * Bind the socket.
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
     * Start listening for workers.
     */
    if (listen(server_socket, 5) < 0)
    {
        perror("listen");
        close(server_socket);
        return 0;
    }

    printf("\n");
    printf("========================================\n");
    printf("        RUNTIME TCP COORDINATOR\n");
    printf("========================================\n");

    printf("Port   : %d\n", port);
    printf("Status : LISTENING\n");

    printf("========================================\n");

    /*
     * Accept one worker connection.
     */
    struct sockaddr_in worker_address;

    socklen_t worker_length =
        sizeof(worker_address);

    int worker_socket =
        accept(
            server_socket,
            (struct sockaddr *)&worker_address,
            &worker_length
        );

    if (worker_socket < 0)
    {
        perror("accept");
        close(server_socket);
        return 0;
    }

    printf("Worker connected.\n");

    /*
     * Receive registration message.
     */
    char message[
        WORKER_REGISTRATION_MESSAGE_SIZE
    ];

    if (!receive_message(
            worker_socket,
            message,
            sizeof(message)))
    {
        printf(
            "Failed to receive worker registration.\n"
        );

        close(worker_socket);
        close(server_socket);

        return 0;
    }

    printf("Received: %s\n", message);

    /*
     * Convert registration message
     * into a Worker structure.
     */
    struct Worker worker = {0};

    if (!parse_worker_registration(
            message,
            &worker))
    {
        printf(
            "Invalid worker registration.\n"
        );

        send_message(
            worker_socket,
            "REGISTER_REJECT"
        );

        close(worker_socket);
        close(server_socket);

        return 0;
    }

    printf("\n");
    printf("Worker registration:\n");
    printf("Worker ID      : %d\n",
           worker.id);

    printf("Total CPU      : %.2f%%\n",
           worker.resources.total_cpu_percent);

    printf("Total Memory   : %ld MB\n",
           worker.resources.total_memory_mb);

    /*
     * Register the worker with the runtime.
     */
    if (!runtime_register_worker(
            runtime,
            &worker))
    {
        printf(
            "Failed to register worker.\n"
        );

        send_message(
            worker_socket,
            "REGISTER_REJECT"
        );

        close(worker_socket);
        close(server_socket);

        return 0;
    }

    printf(
        "Worker registered with runtime. ✅\n"
    );

    /*
     * Send acknowledgement.
     */
    if (!send_message(
            worker_socket,
            "REGISTER_ACK"))
    {
        printf(
            "Failed to send registration acknowledgement.\n"
        );

        close(worker_socket);
        close(server_socket);

        return 0;
    }

    printf(
        "Sent: REGISTER_ACK\n"
    );

    /*
     * Close this registration connection.
     */
    close(worker_socket);
    close(server_socket);

    printf(
        "Runtime coordinator stopped.\n"
    );

    return 1;
}
