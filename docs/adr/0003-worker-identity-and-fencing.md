# ADR-003: Worker Identity and Fencing

- Status: Proposed
- Date: 2026-11-15
- Phase: 2
- Related: `docs/THREAT_MODEL.md`, ADR-002

## Context

Authentication is currently role-based. There are two shared secrets:

- `IDR_CLIENT_SECRET` for clients;
- `IDR_WORKER_SECRET` for workers.

Any holder of the worker secret can:

- register any positive worker ID;
- send `TASK_ACCEPT` for any in-flight task;
- send `TASK_RESULT` for any task, with any worker ID.

The coordinator does not validate either message against the assigned
worker. `TASK_ACCEPT|task_id` has no worker ID at all. `TASK_RESULT`
parses a worker ID, but `coordinator_handle_task_result()` reads the
original assigned worker into `original_worker` and never compares it.

This is acceptable for a single trusted worker process. It is not
acceptable for a distributed deployment, and it makes worker
reincarnation unsafe: an old worker process can report a result for a
task that has been reassigned to its replacement.

## Decision

Bind every worker to an identity and a fencing epoch.

### 1. Enrollment

An operator provisions a worker credential for a specific worker ID:

- a per-worker HMAC secret, or
- a client certificate whose subject or SAN contains the worker ID.

The coordinator stores a verifier for the credential, not the secret
itself where the credential type allows it. The worker presents the
credential on registration. The coordinator rejects:

- an unknown worker ID;
- a credential that does not match the enrolled worker;
- a registration for a disabled worker.

The existing shared `IDR_WORKER_SECRET` can remain as a migration
fallback, but it must be disabled in the v1.0 secure configuration.

### 2. Epoch

On every successful registration, the coordinator increments and returns
a worker epoch:

```
REGISTER_ACK|<worker_id>|<epoch>
```

The epoch is monotonic per worker ID. The worker includes it in every
subsequent message:

```
HEARTBEAT|<worker_id>|<epoch>
TASK_REQUEST|<worker_id>|<epoch>
TASK_ACCEPT|<task_id>|<worker_id>|<epoch>
TASK_RESULT|<task_id>|<worker_id>|<epoch>|<exit_status>|<state>
```

The coordinator rejects messages whose epoch is not the current epoch
for that worker. A stale worker cannot accept or complete a task after
its replacement has registered.

### 3. Assignment ownership

The coordinator records `worker_id`, `epoch`, and `attempt` for every
in-flight task. On `TASK_ACCEPT` and `TASK_RESULT` it checks:

- the task exists and is in flight;
- the worker ID matches the assignment;
- the epoch matches the current epoch;
- the attempt matches the current attempt.

A mismatch is rejected with a specific response:

```
TASK_ACCEPT_REJECT|<task_id>|wrong_worker|stale_epoch
TASK_RESULT_REJECT|<task_id>|wrong_worker|stale_epoch
```

The worker logs the rejection and stops retrying that task. The
coordinator does not change task state based on a rejected message.

### 4. Attempts

Each dispatch increments the task's attempt counter. A retry after a
timeout or worker loss is a new attempt. The result carries the attempt
number, so a late result from attempt 1 cannot complete attempt 2.

This is required for the at-least-once policy in ADR-002: retries are
safe only if the coordinator can distinguish one attempt from another.

### 5. Rotation and revocation

- Per-worker secrets can be rotated without restarting other workers.
- A revoked worker's epoch is invalidated; its in-flight tasks are
  reaped according to ADR-002.
- A disabled worker cannot re-register.
- Enrollment tokens are single-use and expire.

### 6. Backward compatibility

The protocol change is not backward compatible with the current worker
agent. Because the project is pre-1.0, the v0.3.0 release may require
coordinator and worker upgrades together. The release notes must say so.

If a mixed-version deployment is required, use a protocol version field
and support the old shared-secret registration during a documented
transition. Do not silently accept unauthenticated worker IDs.

## Consequences

Positive:

- A worker can only act on its own assignments.
- Reincarnated workers cannot complete old tasks.
- Results and accepts are attributable to a specific worker instance.
- Per-worker revocation does not require rotating a global secret.
- The groundwork for leader and worker fencing in HA is in place.

Negative:

- Enrollment and rotation tooling is required.
- The worker protocol and configuration change.
- More fields in every worker message.
- Tests must cover wrong-worker, stale-epoch, and stale-attempt cases.

## Alternatives considered

### Keep the shared worker secret

Rejected for v1.0. It cannot support attribution, revocation, or
fencing. It can remain only as a development fallback.

### Identify workers by source IP

Rejected. NAT, proxies, container networking, and address reuse make IP
addresses unreliable. An attacker on the same host or network can also
spoof or share an address.

### Use mutual TLS alone without epochs

Insufficient. mTLS proves which credential is speaking, but a restarted
worker with the same certificate is still indistinguishable from its
previous incarnation. Epochs are required for fencing.

### Use a global monotonic task lease instead of worker epochs

A lease per task can work, but it does not authenticate the worker and
does not solve the "which worker instance is this?" problem. Epochs and
leases are complementary.

## Validation

The change is accepted only when these tests exist and pass:

- a worker cannot register an ID it was not enrolled for;
- a worker cannot accept another worker's task;
- a worker cannot report another worker's result;
- a result from a stale epoch is rejected;
- a result from a stale attempt is rejected;
- re-registration invalidates the old epoch;
- revocation reaps the worker's in-flight tasks per ADR-002;
- secret rotation works without restarting other workers;
- the old shared-secret path is disabled or explicitly marked insecure.

## Migration

1. Add the epoch to registration and heartbeat messages.
2. Add worker ID, epoch, and attempt to accept and result messages.
3. Add coordinator-side ownership and epoch checks.
4. Add per-worker credential enrollment and storage.
5. Disable the shared worker secret in secure mode.
6. Update the worker agent, tests, and documentation together.

Because the project is pre-1.0, a coordinated upgrade is acceptable.
Document it in `CHANGELOG.md` and the release notes.
