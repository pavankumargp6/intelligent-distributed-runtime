#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include <poll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "runtime_server.h"
#include "network_protocol.h"
#include "worker_registration.h"
#include "coordinator_tasks.h"
#include "coordinator_handlers.h"
#include "connection_pool.h"
#include "log.h"

#define MAX_POLLFDS (CONNECTION_POOL_CAPACITY + 1)

/*
 * Dispatch one complete message that the connection pool has
 * already read into "message". The worker_socket is used only
 * for sending the response; the connection is closed by the
 * pool after this function returns.
 */
static int handle_worker_message(
    struct Runtime *runtime,
    struct InFlightTable *table,
    int worker_socket,
    const char *message)
{
    log_debug(
        "message_received",
        "Received: %s",
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
            log_warn(
                "registration_invalid",
                "Invalid worker registration."
            );

            send_message(
                worker_socket,
                "REGISTER_REJECT"
            );

            return 0;
        }

        log_info(
            "worker_registration",
            "Worker registration: id=%d cpu=%.2f%% mem_mb=%ld",
            worker.id,
            worker.resources.total_cpu_percent,
            worker.resources.total_memory_mb
        );

        if (!runtime_register_worker(
                runtime,
                &worker))
        {
            log_warn(
                "registration_failed",
                "Failed to register worker."
            );

            send_message(
                worker_socket,
                "REGISTER_REJECT"
            );

            return 0;
        }

        log_info(
            "worker_registered",
            "Worker registered with runtime. ✅"
        );

        if (!send_message(
                worker_socket,
                "REGISTER_ACK"))
        {
            log_warn(
                "register_ack_failed",
                "Failed to send registration acknowledgement."
            );

            return 0;
        }

        log_debug(
            "register_ack_sent",
            "Sent: REGISTER_ACK"
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
            log_warn(
                "heartbeat_invalid",
                "Invalid heartbeat message."
            );

            send_message(
                worker_socket,
                "HEARTBEAT_REJECT"
            );

            return 0;
        }

        log_debug(
            "heartbeat_received",
            "Heartbeat received from worker %d.",
            worker_id
        );

        if (!update_worker_heartbeat(
                &runtime->registry,
                worker_id))
        {
            log_warn(
                "heartbeat_unknown_worker",
                "Unknown worker: %d",
                worker_id
            );

            send_message(
                worker_socket,
                "HEARTBEAT_REJECT"
            );

            return 0;
        }

        if (!send_message(
                worker_socket,
                "HEARTBEAT_ACK"))
        {
            log_warn(
                "heartbeat_ack_failed",
                "Failed to send heartbeat acknowledgement."
            );

            return 0;
        }

        log_debug(
            "heartbeat_ack_sent",
            "Sent: HEARTBEAT_ACK"
        );

        return 1;
    }

    /*
     * ------------------------------------------------
     * Client task submission
     * ------------------------------------------------
     */
    if (strncmp(
            message,
            "SUBMIT|",
            7) == 0)
    {
        return coordinator_handle_submit(
            runtime,
            table,
            worker_socket,
            message
        );
    }

    /*
     * ------------------------------------------------
     * Worker task request
     * ------------------------------------------------
     */
    if (strncmp(
            message,
            "TASK_REQUEST|",
            13) == 0)
    {
        return coordinator_handle_task_request(
            runtime,
            table,
            worker_socket,
            message
        );
    }

    /*
     * ------------------------------------------------
     * Worker task result
     * ------------------------------------------------
     */
    if (strncmp(
            message,
            "TASK_RESULT|",
            12) == 0)
    {
        return coordinator_handle_task_result(
            runtime,
            table,
            worker_socket,
            message
        );
    }

    /*
     * ------------------------------------------------
     * Client task query
     * ------------------------------------------------
     */
    if (strncmp(
            message,
            "QUERY|",
            6) == 0)
    {
        return coordinator_handle_query(
            runtime,
            table,
            worker_socket,
            message
        );
    }

    /*
     * ------------------------------------------------
     * Client status query
     * ------------------------------------------------
     */
    if (strcmp(message, "STATUS") == 0)
    {
        return coordinator_handle_status(
            runtime,
            table,
            worker_socket
        );
    }

    /*
     * Unknown message.
     */
    log_warn(
        "message_unknown",
        "Unknown message: %s",
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
     * In-flight task table. Single-threaded server,
     * so no locking is required yet.
     */
    struct InFlightTable in_flight;
    coordinator_tasks_init(&in_flight);

    /*
     * Connection pool. ~256 KB, so static (BSS) rather
     * than stack.
     */
    static struct ConnectionPool pool;
    pool_init(&pool);

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
     * Non-blocking listener so accept() cannot stall the
     * poll loop.
     */
    int flags = fcntl(server_socket, F_GETFL, 0);

    if (flags < 0 ||
        fcntl(server_socket, F_SETFL, flags | O_NONBLOCK) < 0)
    {
        perror("fcntl");
        close(server_socket);
        return 0;
    }

    struct sockaddr_in server_address = {0};

    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = htonl(INADDR_ANY);
    server_address.sin_port = htons(port);

    if (bind(
            server_socket,
            (struct sockaddr *)&server_address,
            sizeof(server_address)) < 0)
    {
        perror("bind");
        close(server_socket);
        return 0;
    }

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
    printf("Port   : %d\n", port);
    printf("Status : LISTENING\n");
    printf("========================================\n");

    struct pollfd pollfds[MAX_POLLFDS];
    const int poll_timeout_ms = 1000;

    while (1)
    {
        pollfds[0].fd = server_socket;
        pollfds[0].events = POLLIN;
        pollfds[0].revents = 0;

        int nfds = pool_build_pollfds(
            &pool,
            pollfds,
            MAX_POLLFDS
        );

        int poll_result =
            poll(pollfds, nfds, poll_timeout_ms);

        if (poll_result < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            perror("poll");
            continue;
        }

        /*
         * Accept all pending connections.
         */
        if (pollfds[0].revents & POLLIN)
        {
            while (1)
            {
                struct sockaddr_in client_address;
                socklen_t client_length =
                    sizeof(client_address);

                int client_socket = accept(
                    server_socket,
                    (struct sockaddr *)&client_address,
                    &client_length
                );

                if (client_socket < 0)
                {
                    if (errno == EAGAIN ||
                        errno == EWOULDBLOCK)
                    {
                        break;
                    }
                    if (errno == EINTR)
                    {
                        continue;
                    }
                    perror("accept");
                    break;
                }

                log_debug(
                    "connection_accepted",
                    "Connection accepted."
                );

                int idx = pool_add(&pool, client_socket);

                if (idx < 0)
                {
                    log_warn(
                        "pool_full",
                        "Connection pool full; rejecting."
                    );
                    close(client_socket);
                }
            }
        }

        /*
         * Read from every ready connection.
         */
        for (int i = 1; i < nfds; i++)
        {
            short revents = pollfds[i].revents;

            if (!(revents & (POLLIN | POLLHUP | POLLERR)))
            {
                continue;
            }

            int slot = -1;

            for (int j = 0;
                 j < CONNECTION_POOL_CAPACITY;
                 j++)
            {
                if (pool.conns[j].fd == pollfds[i].fd)
                {
                    slot = j;
                    break;
                }
            }

            if (slot < 0)
            {
                continue;
            }

            int step = pool_step(&pool.conns[slot]);

            if (step < 0)
            {
                pool_remove(&pool, slot);
                continue;
            }

            if (step == 0)
            {
                continue;
            }

            /*
             * Complete message. Dispatch on the payload
             * (skip the 4-byte header), send response,
             * then close the connection.
             */
            int fd = pool.conns[slot].fd;
            const char *payload = pool.conns[slot].buffer + 4;

            handle_worker_message(
                runtime,
                &in_flight,
                fd,
                payload
            );

            pool_remove(&pool, slot);
        }

        /*
         * Housekeeping every tick.
         */
        int expired = pool_expire_idle(&pool);

        if (expired > 0)
        {
            log_info(
                "idle_connections_closed",
                "Idle connections closed: %d",
                expired
            );
        }

        int stale_workers =
            mark_stale_workers(
                &runtime->registry
            );

        if (stale_workers > 0)
        {
            log_info(
                "workers_marked_unavailable",
                "Workers marked unavailable: %d",
                stale_workers
            );
        }

        int reaped = coordinator_reap_dead_worker_tasks(
            runtime,
            &in_flight
        );

        if (reaped > 0)
        {
            log_info(
                "in_flight_reaped",
                "Reaped in-flight tasks from unavailable workers: %d",
                reaped
            );
        }
    }

    close(server_socket);

    return 1;
}
