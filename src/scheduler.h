#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "task_manager.h"
#include "worker_pool.h"
#include "worker_registry.h"

struct Scheduler
{
    struct TaskManager *manager;

    struct WorkerPool *worker_pool;
    struct WorkerRegistry *registry;
};

void initialize_scheduler(struct Scheduler *scheduler,
                          struct TaskManager *manager,
                          struct WorkerPool *worker_pool);

int schedule_next_task(struct Scheduler *scheduler,
                       struct Task *task,
                       struct Worker **selected_worker);


/*
 * Pick the next queued task that a specific worker can run.
 *
 * Used by the coordinator when a worker asks for work:
 * the asking worker is the only one we can reply to,
 * so selection is scoped to it rather than global-best.
 *
 * Returns 1 and sets *selected_worker when a task was
 * assigned. Returns 0 otherwise — no queued task, worker
 * not present, worker not live, or the head of the queue
 * does not fit this worker (in which case the task is
 * requeued).
 */
int schedule_next_task_for_worker(struct Scheduler *scheduler,
                                  struct Task *task,
                                  int worker_id,
                                  struct Worker **selected_worker);
#endif
