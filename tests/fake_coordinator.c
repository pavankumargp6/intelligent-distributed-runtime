/*
 * A minimal stand-in for the real coordinator.
 *
 * It accepts worker registration, heartbeats, and
 * a single TASK_REQUEST. It then waits for the
 * matching TASK_RESULT and reports PASS/FAIL.
 *
 * The purpose is to test the worker side of remote
 * execution without depending on the real coordinator.
 *
 * Usage:
 *
 *   ./fake_coordinator
 *
 * Exits 0 on PASS, 1 on FAIL, 2 on setup error.
 * Times out after 25 seconds.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "network_protocol.h"

#define FAKE_COORDINATOR_PORT 5001
#define TEST_TIMEOUT_SECONDS 25
#define EXPECTED_WORKER_ID 1
#define EXPECTED_TASK_ID 42

static volatile sig_atomic_t timed_out = 0;

static void handle_alarm(int signum)
{
    (void)signum;
    timed_out = 1;
}

int main(void)
{
    printf("Fake coordinator starting on port %d...\n",
           FAKE_COORDINATOR_PORT);
    fflush(stdout);

    signal(SIGALRM, handle_alarm);
    alarm(TEST_TIMEOUT_SECONDS);

    int server_socket = socket(AF_INET, SOCK_STREAM, 0);

    if (server_socket < 0)
    {
        perror("socket");
        return 2;
    }

    int reuse = 1;
    setsockopt(server_socket,
               SOL_SOCKET,
               SO_REUSEADDR,
               &reuse,
               sizeof(reuse));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(FAKE_COORDINATOR_PORT);

    if (bind(server_socket,
             (struct sockaddr *)&addr,
             sizeof(addr)) < 0)
    {
        perror("bind");
        close(server_socket);
        return 2;
    }

    if (listen(server_socket, 8) < 0)
    {
        perror("listen");
        close(server_socket);
        return 2;
    }

    int task_assigned = 0;
    int result_received = 0;

    while (!result_received)
    {
        if (timed_out)
        {
            printf("FAIL: timed out waiting for TASK_RESULT.\n");
            close(server_socket);
            return 1;
        }

        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client = accept(server_socket,
                            (struct sockaddr *)&client_addr,
                            &client_len);

        if (client < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            perror("accept");
            break;
        }

        char message[4096];

        if (!receive_message(client, message, sizeof(message)))
        {
            printf("Warning: could not receive message.\n");
            close(client);
            continue;
        }

        printf("Received: %s\n", message);
        fflush(stdout);

        if (strncmp(message, "REGISTER|", 9) == 0)
        {
            send_message(client, "REGISTER_ACK");
        }
        else if (strncmp(message, "HEARTBEAT|", 10) == 0)
        {
            send_message(client, "HEARTBEAT_ACK");
        }
        else if (strncmp(message, "TASK_REQUEST|", 13) == 0)
        {
            if (!task_assigned)
            {
                char assignment[256];
                int n = snprintf(assignment,
                                 sizeof(assignment),
                                 "TASK_ASSIGN|%d|4:echo|1|5:hello",
                                 EXPECTED_TASK_ID);

                if (n <= 0 ||
                    (size_t)n >= sizeof(assignment))
                {
                    printf("FAIL: could not build assignment.\n");
                    close(client);
                    close(server_socket);
                    return 1;
                }

                if (!send_message(client, assignment))
                {
                    printf("FAIL: could not send assignment.\n");
                    close(client);
                    close(server_socket);
                    return 1;
                }

                task_assigned = 1;

                printf("Assigned task %d.\n", EXPECTED_TASK_ID);
                fflush(stdout);
            }
            else
            {
                send_message(client, "NO_TASK");
            }
        }
        else if (strncmp(message, "TASK_RESULT|", 12) == 0)
        {
            int task_id = -1;
            int worker_id = -1;
            int exit_status = -1;
            int state = -1;

            int fields = sscanf(message,
                                "TASK_RESULT|%d|%d|%d|%d",
                                &task_id,
                                &worker_id,
                                &exit_status,
                                &state);

            if (fields != 4)
            {
                printf("FAIL: malformed TASK_RESULT.\n");
                close(client);
                close(server_socket);
                return 1;
            }

            printf("Result: task=%d worker=%d exit=%d state=%d\n",
                   task_id,
                   worker_id,
                   exit_status,
                   state);
            fflush(stdout);

            if (task_id != EXPECTED_TASK_ID ||
                worker_id != EXPECTED_WORKER_ID)
            {
                printf("FAIL: unexpected task or worker id.\n");
                close(client);
                close(server_socket);
                return 1;
            }

            if (exit_status != 0 || state != 2)
            {
                printf("FAIL: unexpected exit status or state.\n");
                close(client);
                close(server_socket);
                return 1;
            }

            send_message(client, "TASK_RESULT_ACK");

            result_received = 1;
        }
        else
        {
            printf("Unknown message: %s\n", message);
            send_message(client, "UNKNOWN_COMMAND");
        }

        close(client);
    }

    printf("PASS: worker executed task remotely and returned the result.\n");

    close(server_socket);
    return 0;
}
