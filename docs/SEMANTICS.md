# IDR Task Semantics and Delivery Guarantees

Status: Current for v0.2 (Phase 1), with v1.0 targets called out
Last updated: 2026-11-18

This document defines what a client, worker, and operator may assume
about task submission, execution, results, and recovery.

If the implementation and this document disagree, that is a bug. The
document deliberately separates **current v0.2 behavior** from **v1.0
target behavior** so that gaps are visible rather than implied away.

## 1. Scope

This document covers:

- task submission and acknowledgement;
- task ID allocation;
- dispatch, acceptance, and execution;
- timeouts and retries;
- result reporting and query behavior;
- crash recovery;
- delivery guarantees;
- retention and observability of task state.

It does not define the wire encoding. See `docs/PROTOCOL.md` for that.
It does not define internal data structures. See
`docs/ARCHITECTURE.md` for that.

## 2. Definitions

- **Task**: a command, its arguments, an idempotency flag, and an
  optional execution timeout.
- **Attempt**: one execution of a task by one worker. A retried task can
  have more than one attempt.
- **Acknowledged submission**: a client received `SUBMIT_ACK|<task_id>`.
- **Durable**: written to the WAL and fsynced before the coordinator
  acknowledges it.
- **Terminal state**: `COMPLETED`, `FAILED`, `TIMED_OUT`, or (target)
  `UNKNOWN`.
- **At-most-once execution**: the task may run zero or one time, never
  twice.
- **At-least-once execution**: the task may run one or more times.
- **Exactly-once execution**: the task runs exactly one time. IDR does
  not currently provide this for arbitrary side effects.

## 3. Task lifecycle

### 3.1 Wire state codes

The current `enum TaskState` values are:

| Value | Name | Meaning |
|---|---|---|
| 0 | `TASK_CREATED` | accepted; queued or being assigned |
| 1 | `TASK_RUNNING` | reserved; not used by the coordinator |
| 2 | `TASK_COMPLETED` | worker reported exit status 0 |
| 3 | `TASK_FAILED` | worker reported nonzero, signal, worker loss, or dispatch timeout |
| 4 | `TASK_TIMED_OUT` | worker killed the task after its execution timeout |
| 5 | `TASK_UNKNOWN` | the task may have run, but its outcome cannot be determined safely |

`TASK_UNKNOWN` was added in v0.2 (Phase 1). It is a terminal state for
the current attempt. It is not silently retried for a non-idempotent
task.

### 3.2 Coordinator view

The coordinator tracks a task in one of four places:

1. **queued** - accepted and durable, waiting for a worker;
2. **in-flight** - assigned to a worker, optionally accepted;
3. **retained history** - recent terminal results, bounded by the
   retention policy (100,000 results or 7 days in v0.2);
4. **WAL only** - durable history that is not in memory.

The in-memory model is deliberately small. The WAL is the durable
record; the queues and history are caches.

Target lifecycle:

```
SUBMIT
  |
  v
QUEUED --dispatch--> DISPATCHED --accept--> ACCEPTED --start--> RUNNING
  |                       |                    |                  |
  |                       |                    |                  +--> COMPLETED
  |                       |                    |                  +--> FAILED
  |                       |                    |                  +--> TIMED_OUT
  |                       |                    |
  |                       +--------------------+---------------------> UNKNOWN
  |                                                               (crash or
  |                                                                lost worker)
  +---------------------------------------------------------------> FAILED
                                                                  (rejected or
                                                                   queue closed)
```

v0.1 collapsed `QUEUED`, `DISPATCHED`, and `ACCEPTED` into
`TASK_CREATED` plus side tables. v0.2 records `DISPATCH` and `ACCEPT`
durably and uses them during recovery, but the wire state still reports
all three non-terminal phases as `TASK_CREATED`.

## 4. Submission

### 4.1 Validation

A `SUBMIT` is rejected if:

- the message is malformed or exceeds the protocol limits;
- the command or an argument is too long for its field;
- the argument count is negative or greater than 16;
- the timeout is negative or greater than `INT_MAX`;
- the queue is full;
- the WAL cannot be appended or fsynced;
- the task cannot be enqueued.

Current behavior that may surprise a client:

- An empty command is accepted. It fails later at `execvp` with exit
  status 127, not at submission time.
- The idempotency field is parsed as an integer and any non-zero value
  is treated as `1`. `2`, `-1`, and `true` are all "idempotent".
- The timeout field is positional. To send a timeout without changing
  the default idempotency, send an explicit `0` for the idempotency
  field first: `...|0|30`.
- Extra bytes after the timeout field are ignored rather than rejected.

Current limits:

| Limit | Value |
|---|---|
| Message size | 4,096 bytes |
| Command length | 255 bytes plus NUL |
| Argument count | 16 |
| Argument length | 127 bytes plus NUL |
| Queue depth | 64 tasks |

### 4.2 Task IDs

Task IDs are positive integers and are allocated by the coordinator.
They are monotonically increasing for the lifetime of the WAL, but they
are **not guaranteed to be contiguous**.

Current behavior: an ID is allocated before the queue-full check. A
rejected submission therefore consumes an ID. Clients must use the ID
returned in `SUBMIT_ACK`, not assume the next ID.

v0.2 persists `next_task_id` in the snapshot, so IDs remain monotonic
across segment truncation and never collide with an older task. The
v1.0 target here is not ID allocation but client-supplied idempotency
keys for deduplication (section 4.3).

### 4.3 Acknowledgement contract

The coordinator performs these steps in order:

1. validate the message;
2. allocate a task ID;
3. check queue capacity;
4. build and append a `SUBMIT` record;
5. fsync the WAL;
6. enqueue the task;
7. send `SUBMIT_ACK|<task_id>`.

Therefore:

> **`SUBMIT_ACK` means the task is durable and queued. If the client
> does not receive `SUBMIT_ACK`, the submission is not guaranteed to
> exist.**

A missing ACK is ambiguous. The task may have been durably recorded
before the connection failed, or it may never have been recorded. A
client that retries can create a second task. v0.2 has no
submission deduplication.

v1.0 target: an optional client-supplied idempotency key, scoped to a
client identity, with a documented retention window. Retrying the same
key returns the original task ID instead of creating a new task.

### 4.4 Rejections

| Response | Meaning |
|---|---|
| `SUBMIT_REJECT` | malformed input, queue full, or WAL failure; no task is acknowledged |
| connection close | authentication, framing, or transport failure; no task is acknowledged |

A client must not treat a rejection as a completed task.

## 5. Dispatch and execution

### 5.1 Assignment

Workers pull work with `TASK_REQUEST`. The coordinator selects a queued
task that fits the requesting worker and sends `TASK_ASSIGN`. The
assignment includes the task ID, command, arguments, and execution
timeout.

The coordinator records the task in its in-flight table with:

- the assigned worker ID;
- the dispatch timestamp;
- an `accepted` flag, initially false.

### 5.2 Acceptance

The worker sends `TASK_ACCEPT|<task_id>` after parsing the assignment
and before forking the task process. v0.2 does **not** include the
worker ID or an epoch in this message; epoch fencing is Phase 2.

If no `TASK_ACCEPT` arrives within `DISPATCH_TIMEOUT_SECONDS` (15
seconds), the coordinator applies the dispatch-timeout policy:

- idempotent task: requeue it while the attempt count stays below
  `COORDINATOR_MAX_ATTEMPTS`; otherwise mark it `TASK_UNKNOWN`;
- non-idempotent task: mark it `TASK_UNKNOWN` with exit status `-1`
  and write a `RESULT` record before releasing the in-flight entry.

### 5.3 Execution

The worker forks a child, places it in a new process group, optionally
enters the experimental sandbox, and calls `execvp`.

The task's execution timeout starts when the worker starts the process.
Queue wait and dispatch time do not count against it. A timeout of 0
means no execution timeout.

On timeout, the worker sends `SIGKILL` to the task's process group and
reports `TASK_TIMED_OUT` with exit status `-1`.

The coordinator does not enforce an execution deadline. If a worker
stays alive and keeps heartbeating but its task child hangs, the task
runs until the task's own timeout expires. If the task has no timeout,
it can run indefinitely.

There is a related liveness gap after acceptance. If the worker cannot
start the task (`start_task` fails) or cannot wait for it
(`wait_for_task` fails), the task-runner child exits without sending a
`RESULT`. The dispatch timeout no longer applies because `TASK_ACCEPT`
was received, and the worker keeps heartbeating, so the task remains
in flight indefinitely. v1.0 requires the worker to report `FAILED` for
every start and wait failure.

### 5.4 Result classification

The worker reports one of:

| Condition | State | Exit status |
|---|---|---|
| process exited with status 0 | `TASK_COMPLETED` | 0 |
| process exited nonzero | `TASK_FAILED` | exit code |
| process killed by a signal | `TASK_FAILED` | -1 |
| execution timeout expired | `TASK_TIMED_OUT` | -1 |
| worker could not start the task | `TASK_FAILED` | -1 |

The coordinator writes a `RESULT` record and fsyncs it before removing
the task from the in-flight table and before sending
`TASK_RESULT_ACK`. The first terminal result wins: later results are
acknowledged and ignored, and a result from a worker other than the
assigned worker is rejected with `TASK_RESULT_REJECT`.

If the `RESULT` append fails, no ACK is sent and the in-flight entry is
retained, so the result can be retried. The worker-side retry with
backoff is a Phase 2 item; v0.2 relies on the reaper and restart
reconciliation if the worker gives up.

### 5.5 Worker loss and reincarnation

A worker is marked unavailable after `WORKER_HEARTBEAT_TIMEOUT` (10
seconds) without a heartbeat. The coordinator reaps that worker's
in-flight tasks.

v0.2 behavior:

- idempotent in-flight tasks are requeued while the attempt count stays
  below `COORDINATOR_MAX_ATTEMPTS`; otherwise they become
  `TASK_UNKNOWN`;
- non-idempotent in-flight tasks become `TASK_UNKNOWN` with exit
  status -1, because the command may have run;
- if a worker re-registers with the same ID, in-flight tasks from the
  previous incarnation are reconciled with the same policy.

v1.0 target:

- epoch fencing so a reincarnated worker cannot act on an old
  assignment;
- attempt history exposed on the wire;
- worker epochs prevent a stale worker from accepting or completing a
  task assigned to its replacement.

## 6. Results, queries, and retention

### 6.1 Query behavior

`QUERY|<task_id>` returns:

| Response | Condition |
|---|---|
| `QUERY_PENDING|<task_id>` | task is currently in flight |
| `QUERY_RESULT|<id>|<worker>|<exit>|<state>` | terminal result is retained in memory |
| `QUERY_EXPIRED|<task_id>` | id is below `next_task_id` but no longer queued, in flight, or retained |
| `QUERY_UNKNOWN|<task_id>` | task is not in flight and not retained; a queued task also returns this in v0.2 |

v0.2 gap: a task that is queued but not yet dispatched is not
in-flight and is not in the retained history, so `QUERY` returns
`QUERY_UNKNOWN`. This behavior is pinned by tests. Phase 2 target:
queued tasks return `QUERY_PENDING`.

v0.2 retention: the retained history holds the last
`IDR_RESULT_RETENTION` (default 100,000) terminal results within
`IDR_RESULT_RETENTION_SECONDS` (default 7 days). A result older than
that returns `QUERY_EXPIRED`, which is distinct from `QUERY_UNKNOWN`.
`QUERY_EXPIRED` was added in v0.2.

### 6.2 Retention

Current v0.2 retains task results:

- in the WAL: records after the latest snapshot; segments fully
  covered by a snapshot are removed, so the log is bounded by the
  snapshot cadence and retention, not by lifetime task count;
- in memory: the last `IDR_RESULT_RETENTION` results, for at most
  `IDR_RESULT_RETENTION_SECONDS`;
- in metrics: process-lifetime counters and averages, restored from
  the snapshot on restart.

v1.0 target:

- move both knobs into the validated configuration file and CLI;
- expose retention/expiry counters in metrics;
- document backup and restore drills for snapshots and segments.

## 7. Crash recovery

### 7.1 Current v0.2 recovery (WAL 2.0)

At startup the coordinator:

1. loads `<base>.manifest`, which names the active snapshot and
   segment boundary;
2. loads the newest valid snapshot (falling back to the previous one
   if the newest is corrupt, or to the newest orphaned snapshot if the
   manifest is unreadable) and restores its LSN, `next_task_id`,
   counters, queued tasks, in-flight tasks, and retained results;
3. verifies that the segment chain is contiguous and still includes
   the segment created by the snapshot rotation (`boundary_seq + 1`);
   a missing required segment refuses startup;
4. replays only segments with records after the snapshot LSN;
5. for each replayed task:
   - `SUBMIT` only: queue it;
   - `DISPATCH` or `ACCEPT` without `RESULT`: apply the idempotency
     policy below;
   - `RESULT`: terminal; deduplicate late results, first result wins;
6. imports a v0.1 text WAL once (`<base>`) and renames it
   `<base>.legacy`;
7. writes a fresh snapshot so recovery starts from bounded state.

Policy for a task with a durable `DISPATCH` or `ACCEPT` but no
`RESULT`:

- `idempotent=true`: requeue with a new attempt, bounded by
  `COORDINATOR_MAX_ATTEMPTS` (default 3). Recovery does not increment
  the attempt counter; the coordinator increments it once at dispatch.
- `idempotent=false`: terminal `TASK_UNKNOWN` (state 5). The task may
  have run; it is not silently retried.

A `SUBMIT` with no `DISPATCH` provably never reached a worker and is
always queued.

v0.1 behavior, for historical reference: it compacted the WAL,
replayed every `SUBMIT` and `RESULT`, rebuilt up to 4,096 submissions
and results in static arrays, requeued every submission without a
result regardless of idempotency, capped the requeue at the 64-entry
queue, and refused to start after 4,096 lifetime tasks.

### 7.2 Crash-point behavior

"Ack" means the coordinator sent the relevant acknowledgement.

| Crash point | Client-visible state | v0.2 recovery behavior |
|---|---|---|
| Before `SUBMIT` fsync | no ACK | no task |
| After `SUBMIT` fsync, before ACK | no ACK (ambiguous) | task is recovered and may run |
| After ACK, before dispatch | task queued | task is requeued |
| After `DISPATCH` fsync, before accept | no result | non-idempotent: `TASK_UNKNOWN`; idempotent: requeued (bounded) |
| After `ACCEPT` fsync, before result | no result | non-idempotent: `TASK_UNKNOWN`; idempotent: requeued (bounded) |
| After `RESULT` fsync, before ACK | result durable | task is terminal; duplicate result is ACKed and ignored |
| After result ACK | terminal | task is terminal |

If a requeue finds the 512-entry queue full, the task stays tracked in
the in-flight table and is reconciled again; it is not dropped.

The dangerous rows are **After `DISPATCH` fsync, before accept** and
**After `ACCEPT` fsync, before result** for non-idempotent tasks:
recovery cannot prove whether the command ran, so v0.2 reports
`TASK_UNKNOWN` instead of claiming failure or silently retrying.
`tests/crash_recovery_test.sh` injects a crash at every point in this
table, plus the snapshot and segment-removal steps, and restarts on
the same WAL.

### 7.3 Target v1.0 refinements

WAL 2.0 implements the snapshot and segment recovery pipeline from
ADR-002. Remaining refinements for v1.0:

1. expose attempt numbers and `TASK_UNKNOWN` history on the wire;
2. return `QUERY_PENDING` for queued tasks (Phase 2);
3. add backup/restore and disk-full drills (Phase 5).

Recovery time and RSS are measured in the lifetime test: 2 ms and
3.7 MB peak RSS after 1,000,000 tasks on the reference host.

## 8. Delivery guarantees

### 8.1 Current v0.2

| Operation | Guarantee |
|---|---|
| Submission after `SUBMIT_ACK` | durable and queued |
| Submission without `SUBMIT_ACK` | unknown; retry can duplicate |
| Idempotent task on dispatch timeout | at-least-once, bounded attempts (default 3) |
| Non-idempotent task on dispatch timeout | at-most-once; `TASK_UNKNOWN` |
| Idempotent task across a coordinator crash | at-least-once, bounded attempts |
| Non-idempotent task across a coordinator crash | at-most-once; `TASK_UNKNOWN` if it was dispatched or accepted |
| Result after `TASK_RESULT_ACK` | durable terminal result |
| Result without ACK | unknown; the worker does not retry (Phase 2) |
| Result whose WAL append fails | no ACK; the task stays in flight and is reconciled after restart |
| Query for queued task | `QUERY_UNKNOWN` (v0.2 behavior; `QUERY_PENDING` is Phase 2) |
| Query for aged-out terminal task | `QUERY_EXPIRED` |

v0.2 therefore does **not** provide exactly-once execution for
non-idempotent tasks. It does provide a precise distinction between
"failed" and "unknown": an uncertain non-idempotent outcome is never
reported as `FAILED` and never silently retried.

### 8.2 Target v1.0

| Task kind | Execution guarantee | Uncertain outcome |
|---|---|---|
| `idempotent=true` | at-least-once, bounded retries | retry with a new attempt |
| `idempotent=false` | at-most-once | `TASK_UNKNOWN`, no automatic retry |

v0.2 already implements this table for crash recovery and reaping.
v1.0 adds attempt history on the wire and an operator-visible retry
workflow.

An operator may explicitly retry an `UNKNOWN` task. That is a new
attempt and must be visible in the task history.

Duplicate `TASK_RESULT` messages are idempotent: the coordinator keeps
the first terminal result for a task and acknowledges duplicates without
changing state.

## 9. Timeouts

| Timeout | Value | Scope |
|---|---|---|
| Auth clock skew | 60 seconds past, 5 seconds future | signed messages |
| Dispatch timeout | 15 seconds | assignment sent, no `TASK_ACCEPT` |
| Worker heartbeat timeout | 10 seconds | worker liveness |
| Execution timeout | per task, 0 = unlimited | from worker process start |
| Connection idle timeout | 5 seconds | coordinator connection pool |
| Agent poll interval | 0 seconds default | worker heartbeat/task poll |

The execution timeout is enforced by the worker, not the coordinator.
The coordinator's dispatch timeout stops applying once `TASK_ACCEPT`
arrives.

## 10. Capacity limits that affect semantics

| Limit | Current value |
|---|---|
| Queue depth | 512 |
| In-flight tasks | 64 |
| Retained results in memory | 100,000 (7 days) |
| Worker pool | 16 |
| Worker registry | 32 |
| Coordinator connections | 64 |
| WAL recovery tasks | snapshot-driven; no fixed lifetime cap |
| WAL segment size | 4 MiB (default) |
| Snapshot cadence | 10,000 records or 300 seconds |
| WAL record size | 8,192 bytes |
| Protocol message size | 4,096 bytes |
| Replay cache | 8,192 MACs |

These are still demo-scale limits except for durability, which is now
bounded by snapshots and retention rather than lifetime task count.
v1.0 makes the queue, in-flight table, worker pool, and retention
configurable and removes the fixed-size assumptions from the hot path.

## 11. Versioning and compatibility

- Authentication uses the `IDR-AUTH-V1` marker inside the MAC input.
- WAL 2.0 segments carry the `IDRS` magic, a format version, a
  64-bit sequence, and a CRC32C per record.
- Snapshots carry the `IDRSNP2` magic, a version, a payload checksum,
  and length fields; the manifest is written atomically.
- v0.1 text WALs are imported once and renamed `<base>.legacy`.
- Protocol compatibility policy is defined in `docs/RELEASES.md`.

## 12. Non-goals

IDR does not currently provide:

- exactly-once side effects for arbitrary commands;
- distributed transactions across tasks;
- cross-coordinator task migration;
- coordinator high availability;
- transport encryption;
- a hostile multi-tenant sandbox;
- per-client identity or authorization;
- automatic client submission deduplication.

These are roadmap items or explicit non-goals, not implied guarantees.

## 13. Open decisions

The following decisions are tracked in ADRs. Resolved in v0.2 (Phase 1):

- default retry limit for idempotent tasks: `COORDINATOR_MAX_ATTEMPTS`,
  default 3;
- wire encoding and semantics of `TASK_UNKNOWN`: wire state 5, terminal
  for the current attempt, never silently retried;
- default retention window for terminal results: 100,000 results or
  7 days, configurable with `IDR_RESULT_RETENTION` and
  `IDR_RESULT_RETENTION_SECONDS`.

Still open before v1.0:

- retention window and scope for client idempotency keys;
- whether task attempts are exposed in the wire protocol;
- whether `TASK_RUNNING` becomes a coordinator-visible state.

## References

- `docs/PROTOCOL.md` - wire encoding, authentication, and message catalog
- `docs/ARCHITECTURE.md` - components, invariants, and limits
- `docs/adr/0002-delivery-semantics.md` - delivery policy decision
- `docs/adr/0001-snapshot-and-wal-segmentation.md` - recovery design
- `docs/adr/0003-worker-identity-and-fencing.md` - worker identity
