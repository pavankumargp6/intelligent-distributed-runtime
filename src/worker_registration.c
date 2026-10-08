#include <stdio.h>
#include <string.h>

#include "worker_registration.h"

int build_worker_registration(
    const struct Worker *worker,
    char *buffer,
    int buffer_size)
{
    if (worker == NULL ||
        buffer == NULL ||
        buffer_size <= 0)
    {
        return 0;
    }

    int written = snprintf(
        buffer,
        buffer_size,
        "REGISTER|%d|%.2f|%ld",
        worker->id,
        worker->resources.total_cpu_percent,
        worker->resources.total_memory_mb
    );

    if (written < 0 ||
        written >= buffer_size)
    {
        return 0;
    }

    return 1;
}

int parse_worker_registration(
    const char *message,
    struct Worker *worker)
{
    if (message == NULL ||
        worker == NULL)
    {
        return 0;
    }

    int worker_id;
    double total_cpu;
    long total_memory;

    int fields = sscanf(
        message,
        "REGISTER|%d|%lf|%ld",
        &worker_id,
        &total_cpu,
        &total_memory
    );

    if (fields != 3)
    {
        return 0;
    }

    if (worker_id <= 0 ||
        total_cpu <= 0.0 ||
        total_memory <= 0)
    {
        return 0;
    }

    worker->id = worker_id;

    worker->resources.total_cpu_percent =
        total_cpu;

    worker->resources.available_cpu_percent =
        total_cpu;

    worker->resources.total_memory_mb =
        total_memory;

    worker->resources.available_memory_mb =
        total_memory;

    return 1;
}

int build_worker_heartbeat(
    const struct Worker *worker,
    char *buffer,
    int buffer_size)
{
    if (worker == NULL ||
        buffer == NULL ||
        buffer_size <= 0)
    {
        return 0;
    }

    int written = snprintf(
        buffer,
        buffer_size,
        "HEARTBEAT|%d",
        worker->id
    );

    if (written < 0 ||
        written >= buffer_size)
    {
        return 0;
    }

    return 1;
}

int parse_worker_heartbeat(
    const char *message,
    int *worker_id)
{
    if (message == NULL ||
        worker_id == NULL)
    {
        return 0;
    }

    int parsed_id;

    int fields = sscanf(
        message,
        "HEARTBEAT|%d",
        &parsed_id
    );

    if (fields != 1)
    {
        return 0;
    }

    if (parsed_id <= 0)
    {
        return 0;
    }

    *worker_id = parsed_id;

    return 1;
}
