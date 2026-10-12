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

/*
 * Rewrite the WAL at path with only the live records.
 *
 * "Live" means:
 *   - for a task_id with a RESULT record: keep only the RESULT
 *   - for a task_id with a SUBMIT record but no RESULT: keep
 *     only the SUBMIT
 *
 * A task that is complete no longer needs its command or
 * arguments on disk; the completed ring stores only
 * (id, worker, exit, state). A pending task still needs its
 * SUBMIT record so a future restart can re-queue it.
 *
 * Writes to <path>.compact, fsyncs, and renames atomically
 * over the original. A missing file is not an error (returns
 * 1 with no work).
 *
 * Returns 1 on success, 0 on failure.
 */
int recovery_compact_wal(const char *path);

#endif
