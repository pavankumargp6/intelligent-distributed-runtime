# ADR-002: Delivery Semantics for Tasks and Results

- Status: Proposed
- Date: 2026-11-15
- Phase: 1
- Related: `docs/SEMANTICS.md`, ADR-001, ADR-003

## Context

The current implementation has an `idempotent` flag, but it is applied
inconsistently:

- dispatch-timeout reaping retries idempotent tasks and fails
  non-idempotent tasks;
- worker re-registration fails all in-flight tasks, including idempotent
  ones;
- crash recovery requeues every `SUBMIT` without a `RESULT`, regardless
  of the flag.

The last case is the dangerous one. A non-idempotent task may have
executed before the coordinator crashed, but if its `RESULT` was not
durably recorded, recovery will run it again. The README says "strict
durability guarantees" without defining whether that means at-least-once
or at-most-once execution.

There is also no `UNKNOWN` terminal state. A task whose outcome cannot be
determined is currently forced into `FAILED`, which hides the possibility
that it ran.

## Decision

Define the delivery policy per task, based on the `idempotent` flag:

| Task kind | Delivery policy | On uncertain outcome |
|---|---|---|
| `idempotent=true` | At-least-once, bounded retries | Retry with a new attempt |
| `idempotent=false` | At-most-once | `TASK_UNKNOWN`, no automatic retry |

An operator may explicitly retry an `UNKNOWN` task. That creates a new
attempt and must be visible in task history.

### Required WAL records

Add two records before the result:

```
DISPATCH|<task_id>|<worker_id>|<epoch>|<attempt>|<timestamp>
ACCEPT|<task_id>|<worker_id>|<epoch>|<attempt>|<timestamp>
```

`DISPATCH` is appended and fsynced before the coordinator sends
`TASK_ASSIGN`. `ACCEPT` is appended after `TASK_ACCEPT` is received. The
exact fsync policy for `ACCEPT` can be decided during implementation, but
the record must never claim a task was accepted if it was not.

### Recovery policy

At startup, after snapshot load and segment replay:

| WAL state | Idempotent task | Non-idempotent task |
|---|---|---|
| `SUBMIT` only | Queue a new attempt | Queue a new attempt |
| `DISPATCH`, no `ACCEPT` or `RESULT` | Requeue with a new attempt | `TASK_UNKNOWN` |
| `ACCEPT`, no `RESULT` | Requeue with a new attempt | `TASK_UNKNOWN` |
| `RESULT` | Terminal, first result wins | Terminal, first result wins |

The `SUBMIT`-only case is safe because the coordinator never sent the
task to a worker. The other uncertain cases are not safe for
non-idempotent tasks.

### Dispatch timeout

Current behavior:

- idempotent: requeue;
- non-idempotent: fail with `TASK_FAILED` and exit status -1.

Target behavior:

- idempotent: requeue with a new attempt, up to the retry limit;
- non-idempotent: `TASK_UNKNOWN`, because the worker may have received
  and started the assignment before `TASK_ACCEPT` was sent.

`TASK_FAILED` should mean "the command ran and failed" or "the task was
rejected before it could run." It should not mean "we do not know."

### Worker loss

When a worker is marked unavailable:

- idempotent tasks: requeue with a new attempt;
- non-idempotent tasks: `TASK_UNKNOWN`;
- if the task was still queued and never dispatched, requeue safely.

### Worker re-registration

A worker that registers with the same ID gets a new epoch. Tasks from the
old epoch are handled by the worker-loss policy. Stale results from the
old epoch are rejected (ADR-003).

### Duplicate results

The first terminal `RESULT` for a task wins. Later results for the same
task are acknowledged but ignored, provided the worker and epoch are
valid. A result from a stale epoch is rejected, not acknowledged as a
state transition.

### Retry limit and backoff

Idempotent tasks default to at most 3 attempts. Backoff is exponential
with jitter, configurable by the operator. After the limit, the task
becomes `TASK_FAILED` with a terminal reason that identifies the retry
exhaustion.

### Client submissions

Client submission deduplication is separate from task execution
semantics. Add an optional client-supplied idempotency key:

- scope: authenticated client identity plus key;
- retention: configurable, default 24 hours;
- behavior: a retry with the same key returns the original task ID and
  status instead of creating a new task;
- conflict: the same key with a different payload is rejected.

Until client identity exists (ADR-003), idempotency keys are scoped to
the client role and are best-effort. Do not claim exactly-once submission
without per-client identity.

### Wire state

Add `TASK_UNKNOWN = 5` to the state enum. The coordinator may expose an
`unknown_reason` in a future protocol revision:

- `crash_after_dispatch`;
- `crash_after_accept`;
- `worker_lost`;
- `dispatch_timeout`;
- `stale_epoch`.

The first implementation may log the reason without putting it on the
wire, but the reason must be available for operations.

## Consequences

Positive:

- Non-idempotent tasks are never silently executed twice.
- The `idempotent` flag has one consistent meaning.
- Operators can distinguish "failed" from "unknown."
- Crash recovery behavior is testable and documented.

Negative:

- More tasks may end in `UNKNOWN` after a crash.
- Operators need a retry tool for `UNKNOWN` tasks.
- Two additional WAL records per task increase write volume.
- The retry policy adds state and tests.

## Alternatives considered

### Always retry every unfinished task

Rejected. It preserves availability but can duplicate side effects. It is
the current behavior and the reason for this ADR.

### Never retry unfinished tasks

Rejected. It is safe for side effects but discards work for idempotent
tasks and makes crash recovery much less useful.

### Exactly-once execution

Rejected for v1.0. It requires distributed transactions or idempotent
side effects supplied by the task itself. IDR can provide at-most-once or
at-least-once, not exactly-once for arbitrary commands.

### Use only `DISPATCH` and no `ACCEPT`

Insufficient for observability. `ACCEPT` is not required to decide the
safety policy, but it distinguishes "assignment may have arrived" from
"worker explicitly claimed the task" and improves dispatch-timeout
diagnostics.

## Validation

The change is accepted only when these tests exist and pass:

- kill the coordinator before `SUBMIT` fsync, after fsync before ACK,
  after dispatch, after accept, and after result fsync;
- for each crash point, assert the policy in the table above;
- non-idempotent task executed exactly zero or one time across the
  matrix;
- idempotent task retried up to the configured limit and then failed;
- duplicate result messages do not change terminal state;
- stale-epoch results are rejected;
- client idempotency key returns the original task ID;
- `UNKNOWN` is queryable and has an operator-visible reason.
