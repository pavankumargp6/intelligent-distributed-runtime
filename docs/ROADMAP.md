# IDR Roadmap to 1.0

Status: Phase 0 and Phase 1 complete (v0.2.2); Phase 2 next
Last updated: 2026-11-18

This document is the canonical project roadmap. It records what "10/10"
means for IDR, the order in which the remaining work will be done, and
the exit criteria for each phase.

## What 10/10 means

A 10/10 project is not a project with no bugs. It is a project that:

1. preserves acknowledged work across crashes;
2. has explicit, tested delivery semantics;
3. scales past demo-sized limits;
4. is secure by default;
5. can be operated and upgraded by someone who did not write it; and
6. proves the above with repeatable tests, benchmarks, and releases.

Against that rubric, the current gaps are concentrated in durability,
semantics, scale, and evidence.

| Dimension | Current (v0.2) | v1.0 target |
|---|---|---|
| Durability | WAL 2.0: framed segments, snapshots, bounded footprint; 1M lifetime tasks with 100 restarts, 42 MB peak RSS, 7 crash points, and corruption/missing-segment tests | Backup/restore and disk-full drills; chaos runs |
| Semantics | Explicit policy: uncertain non-idempotent tasks become `TASK_UNKNOWN`; queued tasks return `QUERY_PENDING` (v0.3.0) | `QUERY_PENDING_QUEUED` with queue position; attempts exposed on the wire |
| Scheduling | Static capacity fit; `available == total` | Live slots/load; reservations; fairness; backpressure |
| Scale | Queue 512; in-flight/completed 64; 16-worker pool | Dynamic limits; 100+ workers; 1k+ concurrent; 10k+ queued |
| Security | Shared role secrets; plaintext; experimental sandbox | Per-worker identity; TLS/mTLS; quotas; sandbox; fuzzed parsers |
| Operability | JSON metrics; environment-variable config | Prometheus; config/CLI; graceful drain; packaging; runbooks |
| Availability | Single coordinator | Raft HA; failover under 5 seconds; chaos-tested |
| Evidence | Crash matrix + 1M lifetime test; sanitizer builds only | Sanitizers/coverage/fuzz/chaos/bench in CI; reproducible releases |

## Two finish lines

- **Portfolio 10/10:** Phases 0-5, with security scoped to per-worker
  identity, TLS, and parser fuzzing. HA can remain a documented design.
  Estimated at 3-4 months of focused part-time work.
- **Production 10/10:** all phases, including HA and an external security
  review. Estimated at 9-12 months of focused part-time work.

Do not start Phase 6 before Phases 1 and 2 are complete. Consensus does
not fix a broken local write-ahead log.

## Phase overview

| Phase | Focus | Estimate | Exit criterion |
|---|---|---|---|
| 0 | Contract, architecture, threat model, release policy | 3-5 days | Docs answer the hard failure questions |
| 1 | WAL 2.0: snapshots, segments, recovery policy (done, v0.2.0) | 3 weeks | 1M tasks, bounded WAL, crash-tested |
| 2 | Live scheduling, worker lifecycle, identity | 3 weeks | 100 workers, slots enforced, stale results rejected |
| 3 | Persistent connections, scale, benchmarks | 2-3 weeks | 3-10x throughput, reproducible p99 data |
| 4 | TLS, per-worker credentials, sandbox, fuzzing | 3-4 weeks | Secure by default, fuzz-clean |
| 5 | Metrics, config, CLI, packaging, runbooks | 2 weeks | Operator can run it without reading source |
| 6 | Raft HA and chaos testing | 8-12 weeks | Failover under 5 seconds, no split-brain |
| 7 | Evidence, releases, community | Continuous | External contributor + reproducible release |

## Phase 0 - Contract and baseline

Status: complete. Tagged `phase-0-baseline`.

The goal was to make the current behavior and the intended behavior
unambiguous before changing durability or scheduling. That baseline is
now fixed, and Phase 1 starts from it.

- [x] Define the 10/10 rubric and finish lines (`docs/ROADMAP.md`)
- [x] Document task lifecycle and delivery guarantees (`docs/SEMANTICS.md`)
- [x] Document components, invariants, and limits (`docs/ARCHITECTURE.md`)
- [x] Document assets, adversaries, and current gaps (`docs/THREAT_MODEL.md`)
- [x] Define service-level objectives (`docs/SLO.md`)
- [x] Define versioning and release policy (`docs/RELEASES.md`)
- [x] Record the three foundational decisions:
  - [x] `docs/adr/0001-snapshot-and-wal-segmentation.md`
  - [x] `docs/adr/0002-delivery-semantics.md`
  - [x] `docs/adr/0003-worker-identity-and-fencing.md`
- [x] Link the documents from the README
- [x] Create GitHub issues for Phase 1 work (#1-#8)
- [x] Tag the Phase 0 documentation baseline (`phase-0-baseline`)

Exit criteria: a new contributor can answer "what happens if the
coordinator dies mid-task?" and "what does SUBMIT_ACK guarantee?" from
the documentation alone.

## Phase 1 - WAL 2.0

Status: complete. Shipped as v0.2.0 in PR #10 (implementation and
tests), documented in PR #11, hardened in v0.2.1, and closed out with
the periodic-restart benchmark in v0.2.2. Tracking issues: #1-#8.

Context: v0.1 kept every `SUBMIT` and `RESULT` line and capped both
compaction and replay at 4,096 unique tasks, so after 4,096 lifetime
tasks the coordinator refused to start. WAL 2.0 replaces full-history
compaction with snapshots, so the on-disk state is bounded by the
retention policy rather than by lifetime task count.

- [x] Replace full-history compaction with snapshots plus WAL segments
- [x] Persist `next_task_id` in the snapshot
- [x] Add record checksums, segment headers, and a versioned snapshot format
- [x] Write snapshots atomically (`tmp` -> fsync -> rename -> fsync directory)
- [x] Keep the last two snapshots and detect corrupt snapshots
- [x] Add `DISPATCH` and `ACCEPT` records to the WAL
- [x] Define recovery behavior for queued, dispatched, accepted, and terminal tasks
- [x] Truncate segments fully covered by a snapshot
- [x] Define and enforce a retention policy for completed results
- [x] Add a crash-point test harness (`IDR_TEST_CRASH_AT=...`)
- [x] Add torn-tail, mid-file, snapshot, manifest, and unknown-version
      corruption tests
- [x] Refuse recovery when a segment required by the snapshot is missing
- [x] Add a 1M-lifetime-task restart test (`tests/wal_lifetime_test.sh`)
- [x] Restart every 10,000 tasks and verify the queue, counters,
      retained history, and `next_task_id` after each restart
- [x] Measure recovery time, RSS, snapshot bytes, and WAL bytes as the
      WAL grows (1 ms and 42 MB peak RSS across 100 in-process restarts
      after 1M tasks; the test asserts the 256 MB bound)

What shipped:

- **On-disk format.** Segments are `<base>.seg.<20-digit seq>` with a
  24-byte header and a CRC32C per record. Snapshots are
  `<base>.snap.<20-digit lsn>.dat` with their own magic, version,
  checksum, and length fields. `<base>.manifest` is written atomically
  and names the active snapshot and segment boundary. v0.1 text WALs
  are imported once and renamed `<base>.legacy`.
- **Bounded footprint.** Snapshots are written every 10,000 records or
  300 seconds and after startup recovery. Segments fully covered by
  the latest snapshot are removed; the two most recent snapshots are
  kept so a corrupt newest snapshot can fall back to the previous one.
- **Recovery.** `recovery_start()` loads the latest valid snapshot,
  verifies that the segment chain is contiguous and still contains the
  segment created by the snapshot rotation, replays only segments after
  the snapshot LSN, and materializes queued, in-flight, and retained
  terminal tasks. A missing required segment refuses startup instead of
  silently recovering incomplete state.
- **Policy.** Non-idempotent tasks whose outcome cannot be determined
  recover to `TASK_UNKNOWN` (state 5). Idempotent tasks are requeued,
  bounded by `COORDINATOR_MAX_ATTEMPTS` (default 3). `DISPATCH` is
  appended before the worker sees the assignment, `ACCEPT` before the
  in-memory accept, and `RESULT` before the in-flight entry is
  released. First result wins; results from the wrong worker are
  rejected with `TASK_RESULT_REJECT`.
- **Retention.** The in-memory completed index keeps the last
  `IDR_RESULT_RETENTION` (100,000) results for
  `IDR_RESULT_RETENTION_SECONDS` (7 days). IDs below the allocator
  watermark that are no longer tracked return `QUERY_EXPIRED`.
- **Evidence.** `tests/crash_recovery_test.sh` injects a crash at
  seven durability points and restarts on the same WAL after each.
  `tests/wal_test.c` and `tests/recovery_snapshot_test.c` cover torn
  tails, mid-file CRC corruption, unknown format versions, corrupt
  manifests, snapshot fallback, and missing segments both inside and
  beyond the snapshot boundary. `tests/wal_lifetime_test.c` pushes
  1,000,000 tasks (2M records) through the log, restarts the coordinator
  every 10,000 tasks (100 restarts), and asserts bounded history and
  segments. The timed final restart recovers in **1 ms**; peak RSS is
  **42 MB** across the 100 in-process restarts (3.7 MB for a single
  restart). Raw data and host details are in `docs/BENCHMARK.md`.

Delivered in Phase 2 (v0.3.0 development):

- queued tasks return `QUERY_PENDING`; v0.2 returned `QUERY_UNKNOWN`,
  and both behaviors are pinned by tests and documented in
  `docs/SEMANTICS.md`;
- a worker that accepts a task but cannot start or wait for it reports
  `TASK_FAILED` instead of exiting silently.

Exit criteria: met. 1M lifetime tasks with a restart every 10,000
tasks; bounded WAL (3 segments and 1.9 MB peak between snapshots after
2M records in the test); recovery in about 1 ms with 42 MB peak RSS
across 100 in-process restarts; no acknowledged task lost across the
crash matrix or the missing-segment tests; no silent duplicate
execution of non-idempotent tasks (they become `TASK_UNKNOWN`).

## Phase 2 - Scheduler and worker lifecycle

Status: in progress (v0.3.0 development).

- [x] Use worker slots as the primary admission control
- [x] Report running tasks and capacity (free slots) in heartbeats
- [ ] Report CPU/memory load in heartbeats (registration-time only in v0.3.0)
- [x] Reserve capacity on dispatch and release it on result/timeout/reap
- [ ] Replace fixed task and worker structures with dynamic structures
- [ ] Make queue and in-flight limits configurable
- [ ] Add fair-share and priority scheduling
- [ ] Add queue high-water marks and explicit backpressure
- [x] Report `TASK_FAILED` when a worker cannot start or wait for an accepted task
- [x] Add a worker-side child supervisor (`waitpid`, SIGCHLD, child cap)
- [ ] Enforce per-task execution timeouts and process-group cleanup
- [ ] Retry result reporting with backoff and duplicate suppression
- [ ] Add per-worker enrollment credentials
- [x] Add worker epochs/fencing tokens to assignments and results
- [x] Validate that `TASK_ACCEPT` and `TASK_RESULT` come from the assigned worker
- [x] Make the coordinator endpoint configurable for clients and workers
- [x] Make QUERY return `QUERY_PENDING` for queued tasks

Exit criteria: 100 workers; 1k concurrent tasks; no worker exceeds its
slot count; stale or duplicate results are rejected; queued tasks are
observable; clients and workers can run on different hosts.

## Phase 3 - Scale and performance

Status: planned.

- [ ] Add persistent connections and connection reuse
- [ ] Add request IDs before adding multiplexing
- [ ] Move the coordinator event loop to `epoll` with a dynamic pool
- [ ] Add group commit for WAL fsyncs
- [ ] Reduce the approximately 50 TCP connections per task noted in the README
- [ ] Rebuild the benchmark harness with nanosecond timers
- [ ] Report p50/p95/p99/p99.9 and confidence intervals across runs
- [ ] Publish raw benchmark data and reference hardware
- [ ] Add a `make bench` target and a nightly regression job
- [ ] Profile coordinator CPU, RSS, fsync latency, and queue wait
- [ ] Add a 24-hour soak test with fd and RSS leak checks

Exit criteria: 3-10x current throughput; documented latency curves;
bounded memory and file descriptors; reproducible results on a named
machine.

## Phase 4 - Security baseline

Status: planned.

- [ ] Add TLS 1.3 and optional mutual TLS
- [ ] Add per-worker and per-client credentials with rotation
- [ ] Shard the replay cache per identity and bound its memory
- [ ] Add command allowlists, size limits, and per-client quotas
- [ ] Add per-worker labels and scheduling constraints
- [ ] Add seccomp-bpf, cgroups v2, namespaces, and rlimits to the sandbox
- [ ] Keep the sandbox experimental until escape tests pass
- [ ] Add fuzz targets for the protocol, WAL, and auth parsers
- [ ] Run short fuzz jobs in CI and long jobs nightly
- [ ] Add `SECURITY.md` and a disclosure policy
- [ ] Commission an external review before calling the project production-ready

Exit criteria: no plaintext production mode; fuzzers clean for the
agreed duration; authentication bypass and sandbox escape tests pass.

## Phase 5 - Operability and release

Status: planned.

- [ ] Make the metrics endpoint non-blocking
- [ ] Add Prometheus metrics, health, and readiness endpoints
- [ ] Add correlation IDs to logs
- [ ] Add a configuration file with validation and CLI overrides
- [ ] Add graceful drain and shutdown
- [ ] Add `idrctl` for status, workers, tasks, drain, snapshot, verify, and restore
- [ ] Add systemd and Docker packaging
- [ ] Add runbooks for disk full, corruption, stuck workers, and upgrades
- [ ] Add backup and restore drills
- [ ] Adopt SemVer, a changelog, and signed release artifacts

Exit criteria: a new operator can deploy, upgrade, snapshot, restore, and
diagnose the system from documentation alone.

## Phase 6 - High availability

Status: planned; production track only.

- [ ] Choose a proven Raft implementation or budget for a Jepsen-style suite
- [ ] Replicate the task state machine and WAL through the Raft log
- [ ] Add leader election, log snapshots, and membership changes
- [ ] Add leader and worker fencing tokens
- [ ] Reconnect workers after a leader change
- [ ] Add chaos tests for leader kill, partition, clock skew, and disk stall
- [ ] Verify linearizability and no duplicate execution

Exit criteria: failover under 5 seconds; no split-brain; no duplicate
execution; chaos suite green.

## Phase 7 - Evidence and community

Status: continuous.

- [ ] Run the test suite under ASAN and UBSAN, not just build them
- [ ] Add coverage reporting and a diff-coverage gate
- [ ] Add `clang-tidy`, `cppcheck`, and `scan-build` jobs
- [ ] Add a multi-distribution, multi-compiler CI matrix
- [ ] Add nightly fuzz, soak, and benchmark jobs
- [ ] Publish architecture decision records and benchmark write-ups
- [ ] Add contributor and issue templates
- [ ] Ship a Docker Compose demo with coordinator, workers, and dashboard
- [ ] Land a pull request from someone outside the project

Exit criteria: an external contributor can build, test, and land a
change; releases are reproducible; benchmarks are reproducible.

## Score progression

These are subjective rubric scores, not release gates.

| Milestone | Expected score |
|---|---|
| v0.1 baseline | 7.5 |
| Phase 1 complete (v0.2.0) | 8.5 |
| Phase 2 complete | 9.0 |
| Phase 3 complete | 9.3 |
| Phase 4 complete | 9.6 |
| Phase 5 complete | 9.8 |
| Phase 6 plus external audit | 10 (production) |

For a portfolio 10/10, Phases 1, 2, 3, and 5 plus a security baseline and
a polished demo are sufficient. The HA design can be the "what I would
do next" story rather than ten weeks of Raft.

## Guardrails

- Do not just raise `RECOVERY_MAX_TASKS`. That moves the cliff and makes
  replay slower and more memory-hungry.
- Do not build Raft before Phase 1. Consensus does not fix a broken
  single-node WAL.
- Do not claim exactly-once without a crash test that kills the
  coordinator mid-task.
- Do not optimize before the benchmark harness has nanosecond resolution
  and multiple runs.
- Keep `main` releasable. Each phase ends with tests, documentation, and
  a tag.

## References

- `docs/SEMANTICS.md` - task lifecycle and delivery guarantees
- `docs/ARCHITECTURE.md` - components, invariants, and limits
- `docs/THREAT_MODEL.md` - assets, adversaries, and mitigations
- `docs/SLO.md` - service-level objectives and measurement
- `docs/RELEASES.md` - versioning and release policy
- `docs/adr/` - architecture decision records
