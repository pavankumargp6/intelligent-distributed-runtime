/*
 * bench_client — submits N tasks as fast as possible.
 *
 * Usage:
 *   bench_client <host> <task_count> <workload:true|sleep1> <task_ids_file>
 *
 * On success, prints to stdout:
 *   submission_start_ns=<n>
 *   submission_end_ns=<n>
 *   tasks_submitted=<n>
 *   submission_throughput=<n>
 *   retries=<n>                number of SUBMIT_REJECT retries
 *
 * Exit 0 on success, 1 on any unrecoverable error.
 *
 * When the coordinator rejects a submission with SUBMIT_REJECT
 * (queue full), the client sleeps briefly and retries the same
 * submission. The retry count is bounded to avoid an infinite
 * loop if the coordinator is genuinely unhealthy.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "network_protocol.h"
#include "wire_fields.h"
#include "auth.h"
#include "auth_config.h"

#define RUNTIME_SERVER_PORT 5001
#define MAX_RETRIES_PER_SUBMIT 5000
#define RETRY_SLEEP_NS 500000L   /* 0.5 ms */

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL +
           (uint64_t)ts.tv_nsec;
}

static void sleep_ns(long ns)
{
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = ns;
    nanosleep(&ts, NULL);
}

static int connect_to_server(const char *host)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0)
    {
        perror("socket");
        return -1;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;

    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0)
    {
        perror("inet_pton");
        close(fd);
        return -1;
    }

    addr.sin_port = htons(RUNTIME_SERVER_PORT);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("connect");
        close(fd);
        return -1;
    }

    return fd;
}

/*
 * Return values:
 *   1   success; *task_id_out set
 *   0   unrecoverable error
 *  -1   coordinator replied SUBMIT_REJECT; caller should retry
 */
static int submit_one(const char *host,
                      const char *command,
                      const char *argument,
                      int *task_id_out)
{
    char plain[NETWORK_MAX_MESSAGE_SIZE];
    memcpy(plain, "SUBMIT", 6);
    size_t offset = 6;

    offset = wire_append_delimiter(plain, sizeof(plain), offset);
    if (offset == 0) return 0;

    offset = wire_append_field(plain, sizeof(plain), offset, command);
    if (offset == 0) return 0;

    offset = wire_append_delimiter(plain, sizeof(plain), offset);
    if (offset == 0) return 0;

    int argc = (argument == NULL) ? 0 : 1;

    offset = wire_append_int(plain, sizeof(plain), offset, argc);
    if (offset == 0) return 0;

    if (argument != NULL)
    {
        offset = wire_append_delimiter(plain, sizeof(plain), offset);
        if (offset == 0) return 0;

        offset = wire_append_field(plain, sizeof(plain), offset, argument);
        if (offset == 0) return 0;
    }

    int fd = connect_to_server(host);
    if (fd < 0) return 0;

    const char *payload = plain;
    char signed_buf[AUTH_MAX_SIGNED_SIZE];

    if (auth_config_is_secure())
    {
        AuthResult r = auth_sign(auth_config_client_secret(),
                                 AUTH_DIR_CLIENT_TO_COORD,
                                 AUTH_ROLE_CLIENT,
                                 plain,
                                 signed_buf,
                                 sizeof(signed_buf));
        if (r != AUTH_OK)
        {
            fprintf(stderr,
                    "auth_sign failed: %s\n",
                    auth_result_name(r));
            close(fd);
            return 0;
        }
        payload = signed_buf;
    }

    if (!send_message(fd, payload))
    {
        fprintf(stderr, "send_message failed\n");
        close(fd);
        return 0;
    }

    char response[AUTH_MAX_SIGNED_SIZE];

    if (!receive_message(fd, response, sizeof(response)))
    {
        fprintf(stderr, "receive_message failed\n");
        close(fd);
        return 0;
    }

    close(fd);

    char verified[AUTH_MAX_SIGNED_SIZE];
    const char *final = response;

    if (auth_config_is_secure())
    {
        AuthResult r = auth_verify(auth_config_client_secret(),
                                   AUTH_DIR_COORD_TO_CLIENT,
                                   AUTH_ROLE_COORDINATOR,
                                   response,
                                   verified,
                                   sizeof(verified));
        if (r != AUTH_OK)
        {
            fprintf(stderr,
                    "auth_verify failed: %s\n",
                    auth_result_name(r));
            return 0;
        }
        final = verified;
    }

    if (strcmp(final, "SUBMIT_REJECT") == 0)
    {
        return -1;
    }

    if (strncmp(final, "SUBMIT_ACK|", 11) != 0)
    {
        fprintf(stderr, "unexpected response: %s\n", final);
        return 0;
    }

    char *end = NULL;
    long id = strtol(final + 11, &end, 10);

    if (end == final + 11 || id <= 0)
    {
        fprintf(stderr, "bad task ID in ACK: %s\n", final);
        return 0;
    }

    *task_id_out = (int)id;
    return 1;
}

int main(int argc, char *argv[])
{
    if (argc != 5)
    {
        fprintf(stderr,
                "Usage: %s <host> <task_count> <workload:true|sleep1> "
                "<task_ids_file>\n",
                argv[0]);
        return 2;
    }

    const char *host = argv[1];
    int task_count = atoi(argv[2]);
    const char *workload = argv[3];
    const char *ids_file = argv[4];

    if (task_count <= 0)
    {
        fprintf(stderr, "task_count must be positive\n");
        return 2;
    }

    const char *command = NULL;
    const char *argument = NULL;

    if (strcmp(workload, "true") == 0)
    {
        command = "true";
        argument = NULL;
    }
    else if (strcmp(workload, "sleep1") == 0)
    {
        command = "sleep";
        argument = "1";
    }
    else
    {
        fprintf(stderr,
                "unknown workload: %s (expected true or sleep1)\n",
                workload);
        return 2;
    }

    if (!auth_config_init())
    {
        fprintf(stderr,
                "auth_config_init failed; set IDR_WORKER_SECRET and "
                "IDR_CLIENT_SECRET, or IDR_INSECURE=1\n");
        return 1;
    }

    FILE *ids = fopen(ids_file, "w");

    if (ids == NULL)
    {
        perror("fopen");
        return 1;
    }

    uint64_t t_start = now_ns();
    long total_retries = 0;

    for (int i = 0; i < task_count; i++)
    {
        int id = 0;
        int attempt = 0;
        int ok = 0;

        while (attempt < MAX_RETRIES_PER_SUBMIT)
        {
            int rc = submit_one(host, command, argument, &id);

            if (rc == 1)
            {
                ok = 1;
                break;
            }

            if (rc == 0)
            {
                fprintf(stderr, "submit %d failed\n", i);
                fclose(ids);
                return 1;
            }

            /* rc == -1: SUBMIT_REJECT, back off and retry. */
            total_retries++;
            attempt++;
            sleep_ns(RETRY_SLEEP_NS);
        }

        if (!ok)
        {
            fprintf(stderr,
                    "submit %d exhausted %d retries\n",
                    i,
                    MAX_RETRIES_PER_SUBMIT);
            fclose(ids);
            return 1;
        }

        fprintf(ids, "%d\n", id);
    }

    uint64_t t_end = now_ns();

    fclose(ids);

    double elapsed_s = (double)(t_end - t_start) / 1e9;

    double throughput = elapsed_s > 0.0
        ? (double)task_count / elapsed_s
        : 0.0;

    printf("submission_start_ns=%llu\n",
           (unsigned long long)t_start);
    printf("submission_end_ns=%llu\n",
           (unsigned long long)t_end);
    printf("tasks_submitted=%d\n", task_count);
    printf("submission_throughput=%.2f\n", throughput);
    printf("retries=%ld\n", total_retries);

    return 0;
}
