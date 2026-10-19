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
#include "metrics_json.h"
#include "auth.h"
#include "auth_config.h"
#include "log.h"

#define MAX_POLLFDS (CONNECTION_POOL_CAPACITY + 2)
#define DEFAULT_WAL_PATH "./idr-wal.log"
#define DEFAULT_BIND_ADDR "127.0.0.1"

static int is_loopback_address(const char *addr)
{
    if (addr == NULL)
    {
        return 0;
    }

    return strcmp(addr, "127.0.0.1") == 0 ||
           strcmp(addr, "::1") == 0 ||
           strcmp(addr, "localhost") == 0;
}

/*
 * Classify an unsigned message by type. Sets expected_dir,
 * expected_role and is_worker_message on success. Returns 1 on
 * a recognized message, 0 on unknown.
 *
 * A message that does not begin with a known keyword is
 * rejected. This is where the coordinator decides which role
 * the sender claims to be, and thus which secret is required
 * to authenticate them.
 */
static int classify_message(const char *message,
                            AuthDirection *expected_dir,
                            AuthRole *expected_role,
                            int *is_worker_message)
{
    if (message == NULL)
    {
        return 0;
    }

    if (strncmp(message, "REGISTER|", 9) == 0 ||
        strncmp(message, "HEARTBEAT|", 10) == 0 ||
        strncmp(message, "TASK_REQUEST|", 13) == 0 ||
        strncmp(message, "TASK_ACCEPT|", 12) == 0 ||
        strncmp(message, "TASK_RESULT|", 12) == 0)
    {
        *expected_dir = AUTH_DIR_WORKER_TO_COORD;
        *expected_role = AUTH_ROLE_WORKER;
        *is_worker_message = 1;
        return 1;
    }
    if (strncmp(message, "SUBMIT|", 7) == 0 ||
        strncmp(message, "QUERY|", 6) == 0 ||
        strcmp(message, "METRICS") == 0 ||           /* <-- Add this */
        (strncmp(message, "STATUS", 6) == 0 &&
         (message[6] == '\0' || message[6] == '|')))

    {
        *expected_dir = AUTH_DIR_CLIENT_TO_COORD;
        *expected_role = AUTH_ROLE_CLIENT;
        *is_worker_message = 0;
        return 1;
    }

    return 0;
}

static int handle_worker_message(
    struct Runtime *runtime,
    struct InFlightTable *table,
    struct Connection *conn,
    const char *message)
{
    AuthDirection expected_dir = AUTH_DIR_WORKER_TO_COORD;
    AuthRole expected_role = AUTH_ROLE_WORKER;
    int is_worker_message = 0;

    if (!classify_message(message,
                          &expected_dir,
                          &expected_role,
                          &is_worker_message))
    {
        log_warn("message_unknown",
                 "Unknown or unrecognized message: %.64s",
                 message);
        return 0;
    }

    /*
     * Verify the MAC in secure mode. In insecure mode we trust
     * the message as-is and use it directly for dispatch.
     */
    char unsigned_buf[AUTH_MAX_SIGNED_SIZE];
    const char *dispatch_msg = message;

    if (auth_config_is_secure())
    {
        const char *secret = is_worker_message
            ? auth_config_worker_secret()
            : auth_config_client_secret();

        AuthResult r = auth_verify(secret,
                                   expected_dir,
                                   expected_role,
                                   message,
                                   unsigned_buf,
                                   sizeof(unsigned_buf));

        if (r != AUTH_OK)
        {
            log_warn("auth_failed",
                     "Verification failed (%s) for message type %.32s",
                     auth_result_name(r),
                     message);
            return 0;
        }

        dispatch_msg = unsigned_buf;
    }

    /*
     * Record the authenticated role on the connection, and the
     * secret and direction to use when signing the response.
     */
    if (is_worker_message)
    {
        conn->peer_role = CONN_ROLE_WORKER;
        conn->response_direction = AUTH_DIR_COORD_TO_WORKER;
        conn->response_secret = auth_config_is_secure()
            ? auth_config_worker_secret()
            : NULL;
    }
    else
    {
        conn->peer_role = CONN_ROLE_CLIENT;
        conn->response_direction = AUTH_DIR_COORD_TO_CLIENT;
        conn->response_secret = auth_config_is_secure()
            ? auth_config_client_secret()
            : NULL;
    }

    log_debug("message_received",
              "Received: %s",
              dispatch_msg);

    if (strncmp(dispatch_msg, "REGISTER|", 9) == 0)
    {
        struct Worker worker = {0};

        if (!parse_worker_registration(dispatch_msg, &worker))
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
        /* Milestone H.2: If this worker was already registered, it
         * means it crashed and restarted. Any tasks it was running
         * are lost and must be failed immediately. */
        if (find_registered_worker(&runtime->registry, worker.id) != NULL)
        {
            int reaped = coordinator_reap_worker_tasks(runtime, table, worker.id);
            if (reaped > 0)
            {
                log_warn("worker_reincarnated",
                         "Worker %d re-registered; failed %d orphaned tasks.",
                         worker.id, reaped);
            }
        }



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

    if (strncmp(dispatch_msg, "HEARTBEAT|", 10) == 0)
    {
        int worker_id;

        if (!parse_worker_heartbeat(dispatch_msg, &worker_id))
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

    if (strncmp(dispatch_msg, "SUBMIT|", 7) == 0)
    {
        return coordinator_handle_submit(
            runtime, table, conn, dispatch_msg);
    }

    if (strncmp(dispatch_msg, "TASK_REQUEST|", 13) == 0)
    {
        return coordinator_handle_task_request(
            runtime, table, conn, dispatch_msg);
    }

    if (strncmp(dispatch_msg, "TASK_ACCEPT|", 12) == 0)
    {
        return coordinator_handle_task_accept(
            runtime, table, conn, dispatch_msg);
    }

    if (strncmp(dispatch_msg, "TASK_RESULT|", 12) == 0)
    {
        return coordinator_handle_task_result(
            runtime, table, conn, dispatch_msg);
    }

    if (strncmp(dispatch_msg, "QUERY|", 6) == 0)
    {
        return coordinator_handle_query(
            runtime, table, conn, dispatch_msg);
    }

    if (strcmp(dispatch_msg, "STATUS") == 0)
    {
        return coordinator_handle_status(
            runtime, table, conn);
    }
    if (strcmp(dispatch_msg, "METRICS") == 0)
    {
        return coordinator_handle_metrics(
            runtime, table, conn);
    }

    log_warn("message_unknown",
             "Unknown message after auth: %.64s",
             dispatch_msg);
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
     * Read the authentication configuration before doing
     * anything else. Refuse to start if the configuration is
     * invalid.
     */
    if (!auth_config_init())
    {
        log_error("auth_config_invalid",
                  "Authentication configuration is invalid. "
                  "Set IDR_WORKER_SECRET and IDR_CLIENT_SECRET to "
                  "at least %d bytes each, or set IDR_INSECURE=1 "
                  "for loopback-only development.",
                  AUTH_CONFIG_MIN_SECRET_BYTES);
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

    const char *bind_addr = getenv("IDR_BIND_ADDR");
    if (bind_addr == NULL || bind_addr[0] == '\0')
    {
        bind_addr = DEFAULT_BIND_ADDR;
    }

    /*
     * In insecure mode, require the bind address to be
     * loopback. This is a hard guarantee: insecure mode can
     * only ever accept local traffic.
     */
    if (!auth_config_is_secure())
    {
        if (!is_loopback_address(bind_addr))
        {
            log_error("auth_insecure_bind_refused",
                      "IDR_INSECURE=1 requires a loopback bind "
                      "address; got %s. Refusing to start.",
                      bind_addr);
            wal_close(&runtime->wal);
            return 0;
        }

        log_warn("auth_mode_insecure",
                 "Running without authentication. "
                 "Loopback-only. Do not use in production.");
    }
    else
    {
        log_info("auth_mode_secure",
                 "Authentication enabled. "
                 "Every message will be verified.");
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
    printf("Auth   : %s\n",
           auth_config_is_secure() ? "secure" : "insecure (loopback only)");
    printf("Status : LISTENING\n");
    printf("========================================\n");

    log_info("coordinator_listening",
             "Listening on %s:%d",
             bind_addr,
             port);


    /*
     * Setup a secondary socket for the HTTP metrics dashboard.
     * We bind to 127.0.0.1:8080.
     */
    int metrics_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (metrics_socket >= 0)
    {
        int m_reuse = 1;
        setsockopt(metrics_socket, SOL_SOCKET, SO_REUSEADDR, &m_reuse, sizeof(m_reuse));
        struct sockaddr_in m_addr = {0};
        m_addr.sin_family = AF_INET;
        m_addr.sin_port = htons(8080);
        m_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 127.0.0.1 only
        if (bind(metrics_socket, (struct sockaddr *)&m_addr, sizeof(m_addr)) < 0 || 
            listen(metrics_socket, 10) < 0)
        {
            perror("metrics_socket");
            close(metrics_socket);
            metrics_socket = -1;
        }
        else
        {
            log_info("metrics_listening", "Metrics dashboard available at http://127.0.0.1:8080/api/metrics");
        }
    }

    struct pollfd pollfds[MAX_POLLFDS];
    const int poll_timeout_ms = 1000;


    while (1)
    {
        pollfds[0].fd = server_socket;
        pollfds[0].events = POLLIN;
        pollfds[0].revents = 0;

        int nfds = pool_build_pollfds(&pool, pollfds, MAX_POLLFDS - 1);
        
        int total_fds = nfds;
        if (metrics_socket >= 0)
        {
            pollfds[nfds].fd = metrics_socket;
            pollfds[nfds].events = POLLIN;
            pollfds[nfds].revents = 0;
            total_fds = nfds + 1;
        }

        int poll_result = poll(pollfds, total_fds, poll_timeout_ms);

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

        if (metrics_socket >= 0 && (pollfds[nfds].revents & POLLIN))
        {
            int metric_fd = accept(metrics_socket, NULL, NULL);
            if (metric_fd >= 0)
            {
                char req[1024];
                /* Simple blocking read is fine for a local dashboard */
                recv(metric_fd, req, sizeof(req), 0);
                
                char resp[2048];
                if (build_metrics_http_response(runtime, &in_flight, resp, sizeof(resp)))
                {
                    send(metric_fd, resp, strlen(resp), MSG_NOSIGNAL);
                }
                close(metric_fd);
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
