#include <stdlib.h>
#include <limits.h>
#include <stddef.h>

#ifndef SIZE_MAX
#define SIZE_MAX ((size_t)-1)
#endif

#include "runtime_limits.h"

/*
 * Parse a size_t from the named environment variable.
 *
 * Returns the fallback when the variable is unset or empty.
 * Returns SIZE_MAX when the value is present but invalid (not a
 * non-negative integer within [min, max]). Callers that need to
 * distinguish "unset" from "invalid" should call port_from_env
 * directly, but for capacities we treat invalid as a fatal config
 * error because a typo should not silently change capacity.
 */
static size_t capacity_from_env(const char *name, size_t fallback,
                                size_t min, size_t max)
{
    const char *value = getenv(name);

    if (value == NULL || value[0] == '\0')
    {
        return fallback;
    }

    char *end = NULL;
    unsigned long long parsed = strtoull(value, &end, 10);

    if (end == value || end == NULL || *end != '\0' ||
        parsed < min || parsed > max)
    {
        return SIZE_MAX;
    }

    return (size_t)parsed;
}

size_t runtime_worker_registry_capacity(void)
{
    return capacity_from_env("IDR_WORKER_REGISTRY_CAPACITY", 32, 1, 100000);
}

size_t runtime_task_queue_capacity(void)
{
    return capacity_from_env("IDR_TASK_QUEUE_CAPACITY", 512, 1, 1000000);
}

size_t runtime_inflight_capacity(void)
{
    return capacity_from_env("IDR_INFLIGHT_CAPACITY", 64, 1, 100000);
}

size_t runtime_completed_capacity(void)
{
    return capacity_from_env("IDR_COMPLETED_CAPACITY", 64, 1, 100000);
}

size_t runtime_connection_pool_capacity(void)
{
    return capacity_from_env("IDR_CONNECTION_POOL_CAPACITY", 64, 1, 10000);
}

size_t runtime_worker_pool_capacity(void)
{
    return capacity_from_env("IDR_WORKER_POOL_CAPACITY", 16, 1, 100000);
}

size_t runtime_result_retention_capacity(void)
{
    return capacity_from_env("IDR_RESULT_RETENTION", 100000, 0, 10000000);
}