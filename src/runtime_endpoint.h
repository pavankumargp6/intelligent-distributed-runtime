#ifndef RUNTIME_ENDPOINT_H
#define RUNTIME_ENDPOINT_H

/*
 * Default coordinator endpoint.
 *
 * The coordinator listens on RUNTIME_PORT on RUNTIME_HOST. Clients
 * and workers dial the same endpoint unless overridden.
 */
#define DEFAULT_RUNTIME_HOST "127.0.0.1"
#define DEFAULT_RUNTIME_PORT 5001

/*
 * Port the coordinator should listen on.
 *
 * Reads IDR_PORT. Returns DEFAULT_RUNTIME_PORT when the variable is
 * unset or empty.
 *
 * Returns -1 when IDR_PORT is set but is not a valid TCP port
 * (1-65535). Callers should treat that as a fatal configuration
 * error rather than silently listening somewhere else.
 */
int runtime_listen_port(void);

/*
 * Host clients and workers should dial.
 *
 * Reads IDR_COORDINATOR_HOST. Returns DEFAULT_RUNTIME_HOST when the
 * variable is unset or empty.
 *
 * The returned pointer is owned by the environment; callers must not
 * free it.
 */
const char *runtime_connect_host(void);

/*
 * Port clients and workers should dial.
 *
 * Reads IDR_COORDINATOR_PORT. Returns DEFAULT_RUNTIME_PORT when the
 * variable is unset or empty.
 *
 * Returns -1 when IDR_COORDINATOR_PORT is set but is not a valid TCP
 * port (1-65535). Callers should treat that as a fatal configuration
 * error.
 */
int runtime_connect_port(void);

#endif
