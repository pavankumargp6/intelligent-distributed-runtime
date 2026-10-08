#ifndef WORKER_REGISTRATION_CLIENT_H
#define WORKER_REGISTRATION_CLIENT_H

#include "worker.h"

/*
 * Connect to the runtime coordinator
 * and register the worker.
 *
 * Returns 1 on success.
 * Returns 0 on failure.
 */
int register_worker_with_runtime(
    struct Worker *worker,
    const char *runtime_address);

#endif
