# ADR-001: Snapshot and WAL Segmentation

- Status: Proposed
- Date: 2026-11-15
- Phase: 1
- Related: `docs/SEMANTICS.md`, `docs/ARCHITECTURE.md`

## Context

The current WAL is a single append-only text file. It records every
`SUBMIT` and `RESULT` line, and startup compaction rewrites the file but
keeps every record for every task ID it knows about.

This has three consequences:

1. The WAL never shrinks. A long-running deployment grows without bound.
2. Compaction and replay are capped at `RECOVERY_MAX_TASKS` (4,096
   unique tasks). After 4,096 lifetime tasks, compaction logs
   `wal_compact_index_full`, replay logs `wal_recovery_overflow`, and the
   coordinator refuses to start.
3. Recovery buffers submissions and results in static arrays. Raising
   the cap without changing the design would increase memory use and
   recovery time linearly.

Deleting the WAL is currently the only way to restart after the cap. That
discards the durability state the WAL exists to provide.

The code comment in `src/recovery.c` explicitly accepts a larger WAL in
exchange for not losing task payloads. That trade-off is reasonable in
isolation, but it is incompatible with a fixed recovery cap.

## Decision

Replace full-history compaction with:

1. **WAL segments.** Rotation produces `wal-<sequence>.log` files with a
   header containing a magic value, format version, segment sequence, and
   creation time. Each record carries a sequence number (LSN), a type, a
   payload length, a CRC32C checksum, and the payload.
2. **Snapshots.** A snapshot contains:
   - a magic value and format version;
   - the snapshot LSN;
   - `next_task_id`;
   - all non-terminal tasks (queued, dispatched, accepted) with their
     full payload and attempt metadata;
   - terminal task summaries needed to answer queries for the retention
     window;
   - a checksum over the snapshot contents.
3. **Segment truncation.** After a snapshot is durable, delete segments
   whose highest LSN is less than or equal to the snapshot LSN. Keep the
   active segment.
4. **Atomic snapshot writes.** Write to a temporary file, fsync it,
   rename it over the target, then fsync the containing directory. Keep
   the previous snapshot so a corrupt latest snapshot can fall back.
5. **Bounded recovery.** Load the latest valid snapshot, replay only the
   segments after its LSN, and keep active tasks in a dynamically sized
   hash map. Do not load every historical terminal task into memory.
6. **Retention.** Terminal results are retained for a configurable
   window (default proposal: 7 days or 1,000,000 tasks, whichever comes
   first). Expired results return a distinct response, not an ambiguous
   "unknown".
7. **Corruption handling.** A torn final record at EOF is truncated to
   the last valid record with a warning. Corruption before the final
   record refuses startup and requires an explicit `idrctl wal verify
   --repair` operation that reports the data-loss boundary.

## Design sketch

### Record format

```
magic[4] | version[2] | type[1] | flags[1] | lsn[8] | length[4] |
payload[length] | crc32c[4]
```

The exact layout is an implementation detail, but the following
properties are required:

- a reader can detect truncation and corruption;
- an unknown version fails closed;
- a record cannot be silently skipped in the middle of a segment;
- the checksum covers the header and payload.

### Snapshot lifecycle

1. Determine a snapshot LSN and quiesce writes at that boundary.
2. Serialize active tasks and retained terminal summaries.
3. Write `snapshot-<lsn>.tmp`, fsync, rename to `snapshot-<lsn>.dat`.
4. Rename or update a small manifest that points to the current and
   previous snapshot.
5. Fsync the directory.
6. Delete segments fully covered by the snapshot.
7. Resume writes.

The manifest must never point at a snapshot that is not durable. If the
process crashes between steps 3 and 5, the previous manifest remains
valid and the temporary file is ignored.

### Recovery

1. Read the manifest.
2. Load the current snapshot. If it is corrupt, load the previous one.
3. Replay segments after the snapshot LSN in order.
4. Apply records to the in-memory task table.
5. Restore `next_task_id` from the snapshot, then take the maximum of
   that value and any replayed task ID plus one.
6. Rebuild metrics counters from the snapshot and replay.

## Consequences

Positive:

- WAL size is bounded by the retention window and segment size.
- Recovery time is proportional to the data since the last snapshot, not
  lifetime task count.
- The 4,096-task cliff disappears.
- `next_task_id` survives truncation without reuse.
- Corruption is detected instead of silently skipped.

Negative:

- More files and a manifest to manage.
- Snapshot serialization and retention add complexity.
- Recovery is no longer a single-file read.
- Existing WALs need a migration tool or a documented one-time
  conversion.

## Alternatives considered

### Raise `RECOVERY_MAX_TASKS`

Rejected. It moves the cliff and increases recovery memory and time. The
WAL still grows without bound.

### Store task state in SQLite

Deferred. It would simplify queries and transactions, but adds a
dependency and changes the project's character. The snapshot design can
be implemented with the existing C code and no new runtime dependency.

### Keep the WAL and never compact

Rejected. It is the current failure mode.

### Snapshot only `next_task_id` and active tasks

Insufficient by itself. Querying terminal results after segment
truncation needs a retained terminal index or a documented expiration
response. The snapshot therefore includes terminal summaries for the
retention window.

## Validation

The change is accepted only when these tests exist and pass:

- 1M lifetime tasks with a restart every 10k tasks;
- recovery under 5 seconds and under 256 MB RSS on the reference host;
- crash injection before and after snapshot rename, manifest update,
  segment rotation, and segment unlink;
- torn-tail truncation succeeds;
- mid-file corruption refuses startup;
- corrupted latest snapshot falls back to the previous snapshot;
- `next_task_id` never decreases or reuses an ID;
- WAL size stays within the configured retention bound;
- a completed task remains queryable within retention and expires
  predictably afterward.

## Migration

Because the project has no stable release yet, v0.2.0 may introduce the
new format directly. Existing `idr-wal.log` files can be imported with a
one-time `idrctl wal migrate` command or left behind with a documented
clean-start procedure. The migration must preserve `next_task_id` and
must report any tasks that cannot be represented.
