#ifndef RUNTIME_SERVER_H
#define RUNTIME_SERVER_H

#include "runtime.h"

/*
 * Start the runtime TCP coordinator.
 *
 * The coordinator accepts worker registration
 * connections on the specified port.
 */
int start_runtime_server(
    struct Runtime *runtime,
    int port);

#endif
