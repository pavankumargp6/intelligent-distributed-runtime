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

int send_task_result_to_runtime(
    const struct Worker *worker,
    const char *runtime_address,
    const struct Task *task);

#endif
