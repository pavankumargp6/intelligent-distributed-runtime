#ifndef RECOVERY_H
#define RECOVERY_H

#include "runtime.h"
#include "coordinator_tasks.h"

/*
 * Start the coordinator's durability layer.
 *
 * This loads the newest valid snapshot (falling back to the
 * previous one), replays WAL segments after its LSN, imports a
 * legacy single-file WAL when no v2 data exists, applies the
 * recovery policy for queued/dispatched/accepted tasks,
 * opens the WAL for append, and writes an initial snapshot.
 *
 * Returns 1 on success, 0 when the coordinator must refuse to
 * start (corruption, queue overflow, unwritable storage).
 */
int recovery_start(struct Runtime *runtime,
                   struct InFlightTable *table,
                   const char *wal_base_path);

/*
 * Rotate to a new segment, write a snapshot covering every
 * record up to the rotation boundary, update the manifest,
 * and truncate segments that are fully covered by the previous
 * snapshot. Safe to call repeatedly.
 */
int recovery_take_snapshot(struct Runtime *runtime,
                           struct InFlightTable *table,
                           struct Wal *wal);

/*
 * Return 1 when the WAL policy says a snapshot is due.
 */
int recovery_snapshot_due(const struct Wal *wal);

#endif
