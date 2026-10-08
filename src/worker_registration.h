#ifndef WORKER_REGISTRATION_H
#define WORKER_REGISTRATION_H

#include "worker.h"

/*
 * Maximum size of a worker registration message.
 */
#define WORKER_REGISTRATION_MESSAGE_SIZE 256

/*
 * Build a registration message for a worker.
 *
 * Format:
 *
 * REGISTER|worker_id|cpu|memory
 */
int build_worker_registration(
    const struct Worker *worker,
    char *buffer,
    int buffer_size
);

/*
 * Parse a worker registration message.
 *
 * Returns 1 on success.
 * Returns 0 on invalid input.
 */
int parse_worker_registration(
    const char *message,
    struct Worker *worker
);

/*
 * Build a heartbeat message.
 *
 * Format:
 *
 * HEARTBEAT|worker_id
 */
int build_worker_heartbeat(
    const struct Worker *worker,
    char *buffer,
    int buffer_size
);

/*
 * Parse a heartbeat message.
 *
 * Returns the worker ID through worker_id.
 *
 * Returns 1 on success.
 * Returns 0 on invalid input.
 */
int parse_worker_heartbeat(
    const char *message,
    int *worker_id
);

#endif
