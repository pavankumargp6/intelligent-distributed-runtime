#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include <poll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "runtime_server.h"
#include "network_protocol.h"
#include "worker_registration.h"
#include "coordinator_tasks.h"
#include "coordinator_handlers.h"
#include "connection_pool.h"
#include "recovery.h"
#include "wal.h"
#include "log.h"

#define MAX_POLLFDS (CONNECTION_POOL_CAPACITY + 1)
#define DEFAULT_WAL_PATH "./idr-wal.log"
#define DEFAULT_BIND_ADDR "127.0.0.1"

static int handle_worker_message(
    struct Runtime *runtime,
    struct InFlightTable *table,
    struct Connection *conn,
    const char *message)
{
    log_debug(
        "message_received",
        "Received: %s",
        message
    );

    if (strncmp(message, "REGISTER|", 9) == 0)
    {
        struct Worker worker = {0};

        if (!parse_worker_registration(message, &worker))
        {
            log_warn("registration_invalid",
                     "Invalid worker registration.");
            pool_send_message(conn, "REGISTER_REJECT");
            return 0;
        }

        log_info("worker_registration",
                 "Worker registration: id=%d cpu=%.2f%% mem_mb=%ld",
                 worker.id,
                 worker.resources.total_cpu_percent,
                 worker.resources.total_memory_mb);

        if (!runtime_register_worker(runtime, &worker))
        {
            log_warn("registration_failed",
                     "Failed to register worker.");
            pool_send_message(conn, "REGISTER_REJECT");
            return 0;
        }

        log_info("worker_registered",
                 "Worker registered with runtime. ✅");

        if (!pool_send_message(conn, "REGISTER_ACK"))
        {
            log_warn("register_ack_failed",
                     "Failed to queue registration acknowledgement.");
            return 0;
        }

        log_debug("register_ack_sent", "Sent: REGISTER_ACK");
        return 1;
    }

    if (strncmp(message, "HEARTBEAT|", 10) == 0)
    {
        int worker_id;

        if (!parse_worker_heartbeat(message, &worker_id))
        {
            log_warn("heartbeat_invalid",
                     "Invalid heartbeat message.");
            pool_send_message(conn, "HEARTBEAT_REJECT");
            return 0;
        }

        log_debug("heartbeat_received",
                  "Heartbeat received from worker %d.",
                  worker_id);

        if (!update_worker_heartbeat(&runtime->registry, worker_id))
        {
            log_warn("heartbeat_unknown_worker",
                     "Unknown worker: %d",
                     worker_id);
            pool_send_message(conn, "HEARTBEAT_REJECT");
            return 0;
        }

        if (!pool_send_message(conn, "HEARTBEAT_ACK"))
        {
            log_warn("heartbeat_ack_failed",
                     "Failed to queue heartbeat acknowledgement.");
            return 0;
        }

        log_debug("heartbeat_ack_sent", "Sent: HEARTBEAT_ACK");
        return 1;
    }

    if (strncmp(message, "SUBMIT|", 7) == 0)
    {
        return coordinator_handle_submit(
            runtime, table, conn, message);
    }

    if (strncmp(message, "TASK_REQUEST|", 13) == 0)
    {
        return coordinator_handle_task_request(
            runtime, table, conn, message);
    }

    if (strncmp(message, "TASK_ACCEPT|", 12) == 0)
    {
        return coordinator_handle_task_accept(
            runtime, table, conn, message);
    }

    if (strncmp(message, "TASK_RESULT|", 12) == 0)
    {
        return coordinator_handle_task_result(
            runtime, table, conn, message);
    }

    if (strncmp(message, "QUERY|", 6) == 0)
    {
        return coordinator_handle_query(
            runtime, table, conn, message);
    }

    if (strcmp(message, "STATUS") == 0)
    {
        return coordinator_handle_status(
            runtime, table, conn);
    }

    log_warn("message_unknown", "Unknown message: %s", message);
    pool_send_message(conn, "UNKNOWN_COMMAND");
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

    struct InFlightTable in_flight;
    coordinator_tasks_init(&in_flight);

    const char *wal_path = getenv("IDR_WAL_PATH");
    if (wal_path == NULL || wal_path[0] == '\0')
    {
        wal_path = DEFAULT_WAL_PATH;
    }

    runtime->wal.fd = -1;

    /*
     * Compact the WAL before opening it for append.
     *
     * Compaction rewrites the file with only live records:
     * RESULTs for completed tasks, SUBMITs for tasks still
     * pending. It writes to a temporary file and renames, so
     * the original WAL is untouched if compaction fails.
     *
     * Best-effort: log a warning on failure and continue. The
     * un-compacted WAL is still valid; replay will read it.
     */
    if (!recovery_compact_wal(wal_path))
    {
        log_warn("wal_compact_skipped",
                 "Could not compact WAL at %s; continuing with original",
                 wal_path);
    }

    if (!wal_open_for_append(&runtime->wal, wal_path))
    {
        log_error("wal_open_failed",
                  "Could not open WAL at %s", wal_path);
        return 0;
    }

    log_info("wal_opened", "Write-ahead log: %s", wal_path);

    if (recovery_replay_wal(runtime, &in_flight, wal_path) < 0)
    {
        log_error("wal_replay_failed",
                  "Could not replay WAL; refusing to start.");
        wal_close(&runtime->wal);
        return 0;
    }

    static struct ConnectionPool pool;
    pool_init(&pool);

    int server_socket = socket(AF_INET, SOCK_STREAM, 0);

    if (server_socket < 0)
    {
        perror("socket");
        wal_close(&runtime->wal);
        return 0;
    }

    int reuse = 1;

    if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR,
                   &reuse, sizeof(reuse)) < 0)
    {
        perror("setsockopt");
        close(server_socket);
        wal_close(&runtime->wal);
        return 0;
    }

    int flags = fcntl(server_socket, F_GETFL, 0);

    if (flags < 0 ||
        fcntl(server_socket, F_SETFL, flags | O_NONBLOCK) < 0)
    {
        perror("fcntl");
        close(server_socket);
        wal_close(&runtime->wal);
        return 0;
    }

    const char *bind_addr = getenv("IDR_BIND_ADDR");
    if (bind_addr == NULL || bind_addr[0] == '\0')
    {
        bind_addr = DEFAULT_BIND_ADDR;
    }

    struct sockaddr_in server_address = {0};

    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(port);

    if (inet_pton(AF_INET, bind_addr,
                  &server_address.sin_addr) != 1)
    {
        log_error("bind_addr_invalid",
                  "IDR_BIND_ADDR is not a valid IPv4 address: %s",
                  bind_addr);
        close(server_socket);
        wal_close(&runtime->wal);
        return 0;
    }

    if (bind(server_socket,
             (struct sockaddr *)&server_address,
             sizeof(server_address)) < 0)
    {
        perror("bind");
        close(server_socket);
        wal_close(&runtime->wal);
        return 0;
    }

    if (listen(server_socket, 16) < 0)
    {
        perror("listen");
        close(server_socket);
        wal_close(&runtime->wal);
        return 0;
    }

    printf("\n");
    printf("========================================\n");
    printf("        RUNTIME TCP COORDINATOR\n");
    printf("========================================\n");
    printf("Bind   : %s\n", bind_addr);
    printf("Port   : %d\n", port);
    printf("Status : LISTENING\n");
    printf("========================================\n");

    log_info("coordinator_listening",
             "Listening on %s:%d",
             bind_addr,
             port);

    struct pollfd pollfds[MAX_POLLFDS];
    const int poll_timeout_ms = 1000;

    while (1)
    {
        pollfds[0].fd = server_socket;
        pollfds[0].events = POLLIN;
        pollfds[0].revents = 0;

        int nfds = pool_build_pollfds(&pool, pollfds, MAX_POLLFDS);

        int poll_result = poll(pollfds, nfds, poll_timeout_ms);

        if (poll_result < 0)
        {
            if (errno == EINTR) continue;
            perror("poll");
            continue;
        }

        if (pollfds[0].revents & POLLIN)
        {
            while (1)
            {
                struct sockaddr_in client_address;
                socklen_t client_length = sizeof(client_address);

                int client_socket = accept(
                    server_socket,
                    (struct sockaddr *)&client_address,
                    &client_length);

                if (client_socket < 0)
                {
                    if (errno == EAGAIN ||
                        errno == EWOULDBLOCK) break;
                    if (errno == EINTR) continue;
                    perror("accept");
                    break;
                }

                log_debug("connection_accepted",
                          "Connection accepted.");

                int idx = pool_add(&pool, client_socket);

                if (idx < 0)
                {
                    log_warn("pool_full",
                             "Connection pool full; rejecting.");
                    close(client_socket);
                }
            }
        }

        for (int i = 1; i < nfds; i++)
        {
            short revents = pollfds[i].revents;

            if (!(revents & (POLLIN | POLLOUT | POLLHUP | POLLERR)))
            {
                continue;
            }

            int slot = -1;

            for (int j = 0; j < CONNECTION_POOL_CAPACITY; j++)
            {
                if (pool.conns[j].fd == pollfds[i].fd)
                {
                    slot = j;
                    break;
                }
            }

            if (slot < 0) continue;

            if (revents & (POLLIN | POLLHUP | POLLERR))
            {
                int step = pool_step(&pool.conns[slot]);

                if (step < 0)
                {
                    pool_remove(&pool, slot);
                    continue;
                }

                if (step == 1)
                {
                    const char *payload =
                        pool.conns[slot].buffer + 4;

                    handle_worker_message(runtime,
                                          &in_flight,
                                          &pool.conns[slot],
                                          payload);

                    pool.conns[slot].closing_after_flush = 1;
                }
            }

            if (pool.conns[slot].send_offset <
                pool.conns[slot].send_length)
            {
                if (!pool_flush(&pool.conns[slot]))
                {
                    pool_remove(&pool, slot);
                    continue;
                }
            }

            if (pool.conns[slot].closing_after_flush &&
                pool.conns[slot].send_offset ==
                pool.conns[slot].send_length)
            {
                pool_remove(&pool, slot);
            }
        }

        int expired = pool_expire_idle(&pool);

        if (expired > 0)
        {
            log_info("idle_connections_closed",
                     "Idle connections closed: %d", expired);
        }

        int stale_workers = mark_stale_workers(&runtime->registry);

        if (stale_workers > 0)
        {
            log_info("workers_marked_unavailable",
                     "Workers marked unavailable: %d",
                     stale_workers);
        }

        int reaped = coordinator_reap_dead_worker_tasks(
            runtime, &in_flight);

        if (reaped > 0)
        {
            log_info("in_flight_reaped",
                     "Reaped in-flight tasks from unavailable workers: %d",
                     reaped);
        }

        int unaccepted = coordinator_reap_unaccepted_tasks(
            runtime, &in_flight);

        if (unaccepted > 0)
        {
            log_info("unaccepted_reaped",
                     "Reaped in-flight tasks with no TASK_ACCEPT: %d",
                     unaccepted);
        }
    }

    close(server_socket);

    return 1;
}
