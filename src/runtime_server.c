#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "runtime_server.h"
#include "network_protocol.h"
#include "worker_registration.h"

static int handle_worker_message(
    struct Runtime *runtime,
    int worker_socket)
{
    char message[
        WORKER_REGISTRATION_MESSAGE_SIZE
    ];

    /*
     * Receive one framed message.
     */
    if (!receive_message(
            worker_socket,
            message,
            sizeof(message)))
    {
        printf(
            "Failed to receive worker message.\n"
        );

        return 0;
    }

    printf(
        "Received: %s\n",
        message
    );

    /*
     * ------------------------------------------------
     * Worker registration
     * ------------------------------------------------
     */
    if (strncmp(
            message,
            "REGISTER|",
            9) == 0)
    {
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

            return 0;
        }

        printf("\n");
        printf("Worker registration:\n");

        printf(
            "Worker ID      : %d\n",
            worker.id
        );

        printf(
            "Total CPU      : %.2f%%\n",
            worker.resources.total_cpu_percent
        );

        printf(
            "Total Memory   : %ld MB\n",
            worker.resources.total_memory_mb
        );

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

            return 0;
        }

        printf(
            "Worker registered with runtime. ✅\n"
        );

        /*
         * Send registration acknowledgement.
         */
        if (!send_message(
                worker_socket,
                "REGISTER_ACK"))
        {
            printf(
                "Failed to send registration acknowledgement.\n"
            );

            return 0;
        }

        printf(
            "Sent: REGISTER_ACK\n"
        );

        return 1;
    }

    /*
     * ------------------------------------------------
     * Worker heartbeat
     * ------------------------------------------------
     */
    if (strncmp(
            message,
            "HEARTBEAT|",
            10) == 0)
    {
        int worker_id;

        if (!parse_worker_heartbeat(
                message,
                &worker_id))
        {
            printf(
                "Invalid heartbeat message.\n"
            );

            send_message(
                worker_socket,
                "HEARTBEAT_REJECT"
            );

            return 0;
        }

        printf(
            "Heartbeat received from worker %d.\n",
            worker_id
        );

        /*
         * Update the worker's last-seen time.
         */
        if (!update_worker_heartbeat(
                &runtime->registry,
                worker_id))
        {
            printf(
                "Unknown worker: %d\n",
                worker_id
            );

            send_message(
                worker_socket,
                "HEARTBEAT_REJECT"
            );

            return 0;
        }

        /*
         * Send heartbeat acknowledgement.
         */
        if (!send_message(
                worker_socket,
                "HEARTBEAT_ACK"))
        {
            printf(
                "Failed to send heartbeat acknowledgement.\n"
            );

            return 0;
        }

        printf(
            "Sent: HEARTBEAT_ACK\n"
        );

        return 1;
    }

    /*
     * Unknown message.
     */
    printf(
        "Unknown message: %s\n",
        message
    );

    send_message(
        worker_socket,
        "UNKNOWN_COMMAND"
    );

    return 0;
}

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
     * Bind socket to the requested port.
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
    if (listen(server_socket, 16) < 0)
    {
        perror("listen");
        close(server_socket);
        return 0;
    }

    printf("\n");
    printf("========================================\n");
    printf("        RUNTIME TCP COORDINATOR\n");
    printf("========================================\n");

    printf(
        "Port   : %d\n",
        port
    );

    printf(
        "Status : LISTENING\n"
    );

    printf("========================================\n");

    /*
     * Configure poll().
     *
     * We monitor the server socket for
     * incoming worker connections.
     */
    struct pollfd server_poll;

    server_poll.fd = server_socket;
    server_poll.events = POLLIN;
    server_poll.revents = 0;

    /*
     * Check worker liveness every second.
     */
    const int poll_timeout_ms = 1000;

    /*
     * ------------------------------------------------
     * Event loop
     * ------------------------------------------------
     */
    while (1)
    {
        /*
         * Wait for either:
         *
         * 1. A new worker connection
         * 2. The one-second timeout
         */
        int poll_result =
            poll(
                &server_poll,
                1,
                poll_timeout_ms
            );

        if (poll_result < 0)
        {
            perror("poll");
            continue;
        }

        /*
         * A connection arrived.
         */
        if (poll_result > 0 &&
            (server_poll.revents & POLLIN))
        {
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
            }
            else
            {
                printf(
                    "\nWorker connection accepted.\n"
                );

                /*
                 * Process one request.
                 */
                handle_worker_message(
                    runtime,
                    worker_socket
                );

                /*
                 * Each connection currently
                 * represents one request.
                 */
                close(worker_socket);
            }
        }

        /*
         * The poll timeout lets us perform
         * failure detection even when there
         * are no incoming connections.
         */
        int stale_workers =
            mark_stale_workers(
                &runtime->registry
            );

        if (stale_workers > 0)
        {
            printf(
                "Workers marked unavailable: %d\n",
                stale_workers
            );
        }
    }

    close(server_socket);

    return 1;
}
