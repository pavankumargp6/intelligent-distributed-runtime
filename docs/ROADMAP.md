# IDR Roadmap to 1.0

Status: Phase 0 in progress
Last updated: 2026-11-15

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

| Dimension | Current v0.x | v1.0 target |
|---|---|---|
| Durability | WAL keeps every record; recovery stops after 4,096 lifetime tasks | Snapshots + segment truncation; 1M+ lifetime tasks; crash-tested |
| Semantics | Implicit at-least-once; `idempotent` ignored on replay | Explicit per-task policy; uncertain non-idempotent tasks are not silently retried |
| Scheduling | Static capacity fit; `available == total` | Live slots/load; reservations; fairness; backpressure |
| Scale | Fixed 64 queue/in-flight/completed; 16-worker pool | Dynamic limits; 100+ workers; 1k+ concurrent; 10k+ queued |
| Security | Shared role secrets; plaintext; experimental sandbox | Per-worker identity; TLS/mTLS; quotas; sandbox; fuzzed parsers |
| Operability | JSON metrics; environment-variable config | Prometheus; config/CLI; graceful drain; packaging; runbooks |
| Availability | Single coordinator | Raft HA; failover under 5 seconds; chaos-tested |
| Evidence | 143 checks; sanitizer builds only | Sanitizers/coverage/fuzz/chaos/bench in CI; reproducible releases |

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
| 1 | WAL 2.0: snapshots, segments, recovery policy | 3 weeks | 1M tasks, bounded WAL, crash-tested |
| 2 | Live scheduling, worker lifecycle, identity | 3 weeks | 100 workers, slots enforced, stale results rejected |
| 3 | Persistent connections, scale, benchmarks | 2-3 weeks | 3-10x throughput, reproducible p99 data |
| 4 | TLS, per-worker credentials, sandbox, fuzzing | 3-4 weeks | Secure by default, fuzz-clean |
| 5 | Metrics, config, CLI, packaging, runbooks | 2 weeks | Operator can run it without reading source |
| 6 | Raft HA and chaos testing | 8-12 weeks | Failover under 5 seconds, no split-brain |
| 7 | Evidence, releases, community | Continuous | External contributor + reproducible release |

## Phase 0 - Contract and baseline

Status: in progress.

The goal is to make the current behavior and the intended behavior
unambiguous before changing durability or scheduling.

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
- [ ] Create GitHub issues for Phase 1 work
- [ ] Tag the Phase 0 documentation baseline

Exit criteria: a new contributor can answer "what happens if the
coordinator dies mid-task?" and "what does SUBMIT_ACK guarantee?" from
the documentation alone.

## Phase 1 - WAL 2.0

Status: planned.

Context: `src/recovery.c` keeps every `SUBMIT` and `RESULT` line during
compaction, while both compaction and replay are capped at 4,096 unique
tasks (`RECOVERY_MAX_TASKS`). After 4,096 lifetime tasks, startup
compaction fails with `wal_compact_index_full`, replay fails with
`wal_recovery_overflow`, and the coordinator refuses to start. Deleting
the WAL is currently the only recovery path.

- [ ] Replace full-history compaction with snapshots plus WAL segments
- [ ] Persist `next_task_id` in the snapshot or a small meta file
- [ ] Add record checksums, segment headers, and a versioned snapshot format
- [ ] Write snapshots atomically (`tmp` -> fsync -> rename -> fsync directory)
- [ ] Keep the last two snapshots and detect corrupt snapshots
- [ ] Add `DISPATCH` and `ACCEPT` records to the WAL
- [ ] Define recovery behavior for queued, dispatched, accepted, and terminal tasks
- [ ] Truncate segments fully covered by a snapshot
- [ ] Define and enforce a retention policy for completed results
- [ ] Add a crash-point test harness (`IDR_TEST_CRASH_AT=...`)
- [ ] Add torn-tail and mid-file corruption tests
- [ ] Add a 1M-lifetime-task restart test that runs in CI or nightly CI
- [ ] Measure recovery time and RSS as the WAL grows

Exit criteria: 1M lifetime tasks with restarts every 10k tasks; recovery
under 5 seconds; bounded WAL size; no acknowledged task lost; no silent
duplicate execution of a non-idempotent task.

## Phase 2 - Scheduler and worker lifecycle

Status: planned.

- [ ] Use worker slots as the primary admission control
- [ ] Report running tasks, free slots, load, and epoch in heartbeats
- [ ] Reserve capacity on dispatch and release it on result/timeout/reap
- [ ] Replace fixed task and worker structures with dynamic structures
- [ ] Make queue and in-flight limits configurable
- [ ] Add fair-share and priority scheduling
- [ ] Add queue high-water marks and explicit backpressure
- [ ] Add a worker-side child supervisor (`waitpid`, SIGCHLD, child cap)
- [ ] Enforce per-task execution timeouts and process-group cleanup
- [ ] Retry result reporting with backoff and duplicate suppression
- [ ] Add per-worker enrollment credentials
- [ ] Add worker epochs/fencing tokens to assignments and results
- [ ] Validate that `TASK_ACCEPT` and `TASK_RESULT` come from the assigned worker
- [ ] Make the coordinator endpoint configurable for clients and workers
- [ ] Make QUERY return `QUERY_PENDING` for queued tasks

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
| Current v0.x | 7.5 |
| Phase 1 complete | 8.5 |
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
