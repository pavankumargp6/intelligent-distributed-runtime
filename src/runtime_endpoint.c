#include <stdlib.h>

#include "runtime_endpoint.h"

/*
 * Parse a TCP port from the named environment variable.
 *
 * Returns the fallback when the variable is unset or empty, the
 * parsed port on success, and -1 when the value is present but
 * invalid. Rejecting garbage loudly is deliberate: a typo in a port
 * number should not silently start a coordinator somewhere nobody is
 * dialing.
 */
static int port_from_env(const char *name, int fallback)
{
    const char *value = getenv(name);

    if (value == NULL || value[0] == '\0')
    {
        return fallback;
    }

    char *end = NULL;
    long parsed = strtol(value, &end, 10);

    if (end == value || end == NULL || *end != '\0' ||
        parsed < 1 || parsed > 65535)
    {
        return -1;
    }

    return (int)parsed;
}

int runtime_listen_port(void)
{
    return port_from_env("IDR_PORT", DEFAULT_RUNTIME_PORT);
}

const char *runtime_connect_host(void)
{
    const char *value = getenv("IDR_COORDINATOR_HOST");

    if (value == NULL || value[0] == '\0')
    {
        return DEFAULT_RUNTIME_HOST;
    }

    return value;
}

int runtime_connect_port(void)
{
    return port_from_env("IDR_COORDINATOR_PORT", DEFAULT_RUNTIME_PORT);
}
