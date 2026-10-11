#ifndef RECOVERY_H
#define RECOVERY_H

#include "runtime.h"
#include "coordinator_tasks.h"

/*
 * Replay a WAL file into the runtime and in-flight table.
 *
 * Builds a picture of every submitted task and every recorded
 * result. Tasks that have a result go into the completed ring;
 * tasks without a result are re-enqueued so the scheduler can
 * dispatch them once a worker asks.
 *
 * Sets table->next_task_id to (max recovered task_id) + 1 so
 * new submissions do not collide with recovered ones.
 *
 * Returns the number of tasks recovered (queued + completed),
 * or -1 on read error.
 */
int recovery_replay_wal(struct Runtime *runtime,
                        struct InFlightTable *table,
                        const char *path);

#endif
