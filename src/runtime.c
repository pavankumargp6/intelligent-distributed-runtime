#include <stddef.h>

#include "runtime.h"

void initialize_runtime(struct Runtime *runtime,
                        struct TaskManager *task_manager,
                        struct Scheduler *scheduler,
                        struct WorkerPool *worker_pool)
{
    if (runtime == NULL)
    {
        return;
    }

    runtime->task_manager = task_manager;
    runtime->scheduler = scheduler;
    runtime->worker_pool = worker_pool;
}
