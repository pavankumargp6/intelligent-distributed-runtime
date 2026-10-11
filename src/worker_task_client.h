#ifndef WORKER_TASK_CLIENT_H
#define WORKER_TASK_CLIENT_H

#include "task.h"
#include "worker.h"

#define WORKER_TASK_MESSAGE_SIZE 4096

int request_task_from_runtime(
    const struct Worker *worker,
    const char *runtime_address,
    struct Task *task,
    int *has_task);

/*
 * Send TASK_ACCEPT|<task_id> on a fresh connection and wait
 * for TASK_ACCEPT_ACK. Called by the worker immediately after
 * it parses a TASK_ASSIGN, before it forks the executor.
 *
 * Returns 1 when the coordinator acknowledges.
 * Returns 0 on network, protocol, or reject response.
 */
int send_task_accept_to_runtime(
    const struct Worker *worker,
    const char *runtime_address,
    int task_id);

int send_task_result_to_runtime(
    const struct Worker *worker,
    const char *runtime_address,
    const struct Task *task);

#endif
