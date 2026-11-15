# IDR Task Semantics and Delivery Guarantees

Status: Draft for v1.0
Last updated: 2026-11-15

This document defines what a client, worker, and operator may assume
about task submission, execution, results, and recovery.

If the implementation and this document disagree, that is a bug. The
document deliberately separates **current v0.x behavior** from **v1.0
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

v1.0 adds:

| Value | Name | Meaning |
|---|---|---|
| 5 | `TASK_UNKNOWN` | the task may have run, but its outcome cannot be determined safely |

`TASK_UNKNOWN` is a terminal state for the current attempt. It is not
silently retried for a non-idempotent task.

### 3.2 Coordinator view

The coordinator tracks a task in one of four places:

1. **queued** - accepted and durable, waiting for a worker;
2. **in-flight** - assigned to a worker, optionally accepted;
3. **completed ring** - recent terminal results, currently 64 entries;
4. **WAL only** - durable history that is not in memory.

The in-memory model is deliberately small. The WAL is the durable
record; the queues and rings are caches.

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

Current v0.x collapses `QUEUED`, `DISPATCHED`, and `ACCEPTED` into
`TASK_CREATED` plus side tables. The wire state does not expose those
intermediate states.

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

v1.0 target: persist `next_task_id` in the snapshot so IDs remain
monotonic across WAL truncation and never collide with an older task.

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
client that retries can create a second task. Current v0.x has no
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
and before forking the task process. Current v0.x does **not** include
the worker ID or an epoch in this message.

If no `TASK_ACCEPT` arrives within `DISPATCH_TIMEOUT_SECONDS` (15
seconds), the coordinator applies the dispatch-timeout policy:

- idempotent task: requeue it;
- non-idempotent task: mark it `FAILED` with exit status `-1` and write
  a `RESULT` record.

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

The coordinator writes a `RESULT` record and fsyncs it before sending
`TASK_RESULT_ACK`. A result for an unknown task is acknowledged but
ignored.

Current ordering defect: the coordinator removes the task from the
in-flight table *before* appending the `RESULT` record. If the append
fails, it sends no ACK, but the task is already gone from in-flight and
was never added to the completed ring. The worker does not retry a
failed result report, so the result is dropped. After a restart the
`SUBMIT` has no `RESULT` and the task is treated as unfinished. v1.0
must keep the task in flight until its result is durable and must retry
result reporting.

### 5.5 Worker loss and reincarnation

A worker is marked unavailable after `WORKER_HEARTBEAT_TIMEOUT` (10
seconds) without a heartbeat. The coordinator reaps that worker's
in-flight tasks.

Current v0.x behavior:

- all in-flight tasks for the lost worker are marked `FAILED` with exit
  status -1, including idempotent tasks;
- if a worker re-registers with the same ID, in-flight tasks from the
  previous incarnation are failed immediately.

v1.0 target:

- idempotent tasks are requeued;
- non-idempotent tasks become `UNKNOWN` unless the coordinator can prove
  the task never started;
- worker epochs prevent a stale worker from accepting or completing a
  task assigned to its replacement.

## 6. Results, queries, and retention

### 6.1 Query behavior

`QUERY|<task_id>` returns:

| Response | Condition |
|---|---|
| `QUERY_PENDING|<task_id>` | task is currently in flight |
| `QUERY_RESULT|<id>|<worker>|<exit>|<state>` | terminal result is in the completed ring |
| `QUERY_UNKNOWN|<task_id>` | task is not in flight and not in the completed ring |

Current gap: a task that is queued but not yet dispatched is not
in-flight and is not in the completed ring, so `QUERY` returns
`QUERY_UNKNOWN`. v1.0 target: queued tasks return `QUERY_PENDING`.

Current gap: the completed ring holds only the last 64 results. A
terminal result older than that is `QUERY_UNKNOWN` even though the WAL
still contains it. v1.0 target: query terminal results from a snapshot
or an on-disk index, with an explicit retention policy.

### 6.2 Retention

Current v0.x retains task results:

- in the WAL: every record, until the WAL is manually removed or the
  4,096-task recovery limit is hit;
- in memory: the last 64 completed results;
- in metrics: process-lifetime counters and averages, reset on restart.

v1.0 target:

- terminal results are retained for a documented time or count;
- expired results return a distinct `QUERY_EXPIRED` (or documented
  equivalent), not an ambiguous `QUERY_UNKNOWN`;
- the snapshot or an on-disk index supports efficient lookup;
- retention is configurable and does not make the WAL unbounded.

## 7. Crash recovery

### 7.1 Current v0.x recovery

At startup the coordinator:

1. compacts the WAL;
2. opens it for append;
3. replays every `SUBMIT` and `RESULT` record;
4. rebuilds up to 4,096 submissions and 4,096 results in static arrays;
5. tries to re-queue every `SUBMIT` that has no matching `RESULT`;
6. sets `next_task_id` to the largest task ID seen plus one.

Consequences:

- every `SUBMIT` without a `RESULT` is treated as unfinished and is
  retried, regardless of the `idempotent` flag;
- a non-idempotent task can run twice if it executed before the crash
  but its result was not durably recorded;
- because the queue holds only 64 tasks, a crash with more than 64
  unfinished tasks re-queues the first 64 it saw and leaves the rest in
  the WAL only. They are not lost from disk, but they do not run until a
  later restart re-reads the same records;
- the WAL does not shrink for completed tasks;
- after 4,096 lifetime tasks, recovery refuses to start.

The `idempotent` flag is honored for dispatch timeouts but not for
crash recovery. This is a known inconsistency and the subject of
ADR-002.

### 7.2 Crash-point behavior

The following table describes current behavior. "Ack" means the
coordinator sent the relevant acknowledgement.

| Crash point | Client-visible state | Recovery behavior |
|---|---|---|
| Before `SUBMIT` fsync | no ACK | no task |
| After `SUBMIT` fsync, before ACK | no ACK (ambiguous) | task is recovered and may run |
| After ACK, before dispatch | task queued | task is requeued |
| After dispatch, before accept | no result | task is requeued, even if non-idempotent |
| After accept, before result | no result | task is requeued, even if non-idempotent |
| After `RESULT` fsync, before ACK | result durable | task is terminal; duplicate result is ignored |
| After result ACK | terminal | task is terminal |

"Requeued" in this table means the task is durable and is offered to the
queue again. If the 64-entry queue is already full, it stays in the WAL
and waits for a later restart; see section 7.1.

The dangerous rows are **After dispatch, before accept** and **After
accept, before result** for non-idempotent tasks: the task may already
have run, but recovery cannot tell. The row **After SUBMIT fsync, before
ACK** is ambiguous for the client, but the task provably never reached a
worker. v1.0 narrows all three with `DISPATCH` and `ACCEPT` records and
the policy in ADR-002.

### 7.3 Target recovery

v1.0 recovery will:

1. load the latest valid snapshot;
2. replay WAL segments after the snapshot LSN;
3. for each task:
   - `SUBMIT` only: queue it;
   - `DISPATCH` or `ACCEPT` without `RESULT`: apply the idempotency
     policy;
   - `RESULT`: terminal, deduplicate late results;
4. restore `next_task_id` from the snapshot;
5. delete segments fully covered by the snapshot;
6. keep the last two valid snapshots for rollback.

## 8. Delivery guarantees

### 8.1 Current v0.x

| Operation | Guarantee |
|---|---|
| Submission after `SUBMIT_ACK` | durable and queued |
| Submission without `SUBMIT_ACK` | unknown; retry can duplicate |
| Idempotent task on dispatch timeout | at-least-once |
| Non-idempotent task on dispatch timeout | at-most-once |
| Any task across a coordinator crash | at-least-once, including non-idempotent tasks |
| Result after `TASK_RESULT_ACK` | durable terminal result |
| Result without ACK | unknown; worker does not retry |
| Result whose WAL append fails | dropped from memory; recovered as unfinished after restart |
| Query for queued task | may return `QUERY_UNKNOWN` |
| Query for old terminal task | may return `QUERY_UNKNOWN` after 64 results |

Current v0.x therefore does **not** provide exactly-once execution for
non-idempotent tasks. It also does not provide a reliable
"never duplicate" guarantee across coordinator crashes.

### 8.2 Target v1.0

| Task kind | Execution guarantee | Uncertain outcome |
|---|---|---|
| `idempotent=true` | at-least-once, bounded retries | retry with a new attempt |
| `idempotent=false` | at-most-once | `TASK_UNKNOWN`, no automatic retry |

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
| Queue depth | 64 |
| In-flight tasks | 64 |
| Completed results in memory | 64 |
| Worker pool | 16 |
| Worker registry | 32 |
| Coordinator connections | 64 |
| WAL recovery tasks | 4,096 |
| WAL line size | 8,192 bytes |
| Protocol message size | 4,096 bytes |
| Replay cache | 8,192 MACs |

These are v0.x demo-scale limits. v1.0 makes the queue, in-flight table,
completed index, worker pool, and recovery limits configurable and
removes the fixed-size assumptions from the hot path.

## 11. Versioning and compatibility

- Authentication uses the `IDR-AUTH-V1` marker inside the MAC input.
- The current WAL has no magic number, version, or per-record checksum.
- v1.0 snapshots and WAL segments will carry a magic value, a format
  version, and per-record checksums.
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

The following decisions are tracked in ADRs and must be resolved before
v1.0:

- default retry limit and backoff for idempotent tasks;
- wire encoding and exact semantics of `TASK_UNKNOWN`;
- default retention window for terminal results;
- retention window and scope for client idempotency keys;
- whether task attempts are exposed in the wire protocol;
- whether `TASK_RUNNING` becomes a coordinator-visible state.

## References

- `docs/PROTOCOL.md` - wire encoding, authentication, and message catalog
- `docs/ARCHITECTURE.md` - components, invariants, and limits
- `docs/adr/0002-delivery-semantics.md` - delivery policy decision
- `docs/adr/0001-snapshot-and-wal-segmentation.md` - recovery design
- `docs/adr/0003-worker-identity-and-fencing.md` - worker identity
