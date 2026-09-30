#ifndef TASK_EXECUTOR_H
#define TASK_EXECUTOR_H

#include "task.h"

int start_task(struct Task *task);

int wait_for_task(struct Task *task);

#endif
