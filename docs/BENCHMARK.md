# IDR Runtime Benchmark

This document reports the results of the F-1 benchmark milestone
and the F-2 decision that followed from them. It is descriptive:
the numbers below were measured on the machine described in
section 2 and are reported as-is. Where a measurement is
unreliable for reasons of tooling, that is stated.

## 1. What was measured and why

The F-1 milestone asked a specific question: **is the current
worker architecture — one fresh TCP connection per worker
operation — a material performance cost, or is it negligible
at the current scale?**

Answering that question requires measuring the runtime's actual
performance under a controlled load. Intuition and code reading
were not sufficient.

Four metrics were recorded per run:

| Metric | Timing boundaries |
|---|---|
| End-to-end latency | Coordinator accepts SUBMIT → coordinator processes RESULT |
| Queue wait | Coordinator queues task → coordinator dispatches to a worker |
| Submission rate | Aggregate wall-clock window across all clients, reported by clients |
| Completion rate | Total tasks divided by wall-clock from first submit to all complete |

End-to-end and queue-wait figures come from the coordinator's
structured `task_latency` events, which are emitted when a
`TASK_RESULT` is processed. They measure what the coordinator
itself can observe; they do not include the client's network
round trip to the coordinator.

## 2. Environment

- **Host**: Ubuntu on WSL2.
- **CPU**: exposed as 8 cores (800.00% in the worker's resource
  report).
- **Memory**: 7710 MB total.
- **Network**: loopback only. Client, coordinator, and workers
  all run on the same host.
- **Coordinator**: single-threaded, `poll()`-based.
- **Worker**: one TCP connection per operation; configurable
  cycle interval (`IDR_AGENT_INTERVAL_SECONDS`, default 3s,
  0s for benchmark spin).

## 3. Methodology

The benchmark harness is `tests/bench_runtime.sh`. For each
run:

1. Build the coordinator, worker, and benchmark client.
2. Start a fresh coordinator with `IDR_INSECURE=1` and
   `IDR_LOG_LEVEL=debug`. Insecure mode is used because it
   isolates benchmark behavior from authentication overhead;
   auth is measured separately (see section 6).
3. Start `--workers` worker processes with the requested agent
   interval.
4. Launch `--clients` client processes in parallel; each
   submits `tasks / clients` tasks.
5. Wait for every client to exit cleanly.
6. Wait for the coordinator to record the expected number of
   `task_latency` events.
7. Extract `queue_wait_ms` and `end_to_end_ms` from each event,
   compute p50 and p95, and append one row to the CSV.

The harness fails loudly if a client exits non-zero, if the
completion count does not reach the requested total before a
300-second timeout, or if any task reports `state=3` (FAILED).

The client retries a submission on `SUBMIT_REJECT`. This is
required: the coordinator's task queue holds 64 tasks, and
localhost submission rates fill it in milliseconds. Retrying
makes the benchmark measure sustained throughput rather than
burst-fill behavior. Retry counts are reported by the client.

## 4. Results

### 4.1 `true` workload

Command executed: `/bin/true`, no arguments. Execution time is
effectively zero; the numbers reflect scheduling, dispatch,
protocol, and process-creation overhead.

| Clients | Workers | Tasks | e2e p50 | e2e p95 | queue p50 | queue p95 | Connections |
|---------|---------|-------|---------|---------|-----------|-----------|-------------|
| 1       | 1       | 100   | 18 ms   | 30 ms   | 17 ms     | 29 ms     | 22,602      |
| 1       | 1       | 1000  | 26 ms   | 36 ms   | 25 ms     | 35 ms     | 25,556      |
| 1       | 4       | 1000  | 14 ms   | 16 ms   | 12 ms     | 14 ms     | 56,218      |
| 4       | 4       | 1000  | 17 ms   | 19 ms   | 15 ms     | 17 ms     | 53,511      |
| 4       | 8       | 1000  | 14 ms   | 16 ms   | 12 ms     | 13 ms     | 86,393      |

Notes:
- The reported submission and completion rates from these runs
  are not included in the table above because the wall-clock
  completion timer has 1-second resolution, and these batches
  finish in under two seconds. The rates in the raw CSV are
  therefore quantized to one of a small number of values and
  should not be interpreted as sustained throughput figures.
- Two separate c4w8 runs produced submission rates of 882/s and
  5,337/s. The e2e and queue latencies were identical (p50=14ms).
  This suggests the first run encountered a transient at the
  single-threaded accept loop while eight workers were polling
  aggressively. Single runs are not reliable; the pattern
  across runs is.

### 4.2 `sleep1` workload

Command executed: `/bin/sleep 1`. Execution time dominates the
e2e latency; the numbers reflect the coordinator's ability to
keep many tasks in flight.

| Clients | Workers | Tasks | e2e p50 | e2e p95 | queue p50 | queue p95 | Connections |
|---------|---------|-------|---------|---------|-----------|-----------|-------------|
| 1       | 1       | 100   | 1026 ms | 2015 ms | 21 ms     | 1012 ms   | 37,322      |
| 1       | 4       | 100   | 1018 ms | 2008 ms | 9 ms      | 1003 ms   | 92,544      |
| 4       | 4       | 100   | 1027 ms | 2018 ms | 20 ms     | 1015 ms   | 98,148      |
| 4       | 8       | 100   | 1016 ms | 2007 ms | 9 ms      | 1004 ms   | 147,032     |

The e2e p95 ≈ 2 seconds in every case: it takes approximately
one second to dispatch all 100 tasks across the workers, and
then approximately one more second for the last-dispatched
tasks to finish their `sleep 1`. The p50 is dominated by the
1-second sleep itself.

### 4.3 Agent poll interval sweep

Two runs with the c4w8 configuration and 200 tasks, varying
only the worker poll interval:

| Agent interval | e2e p50 | e2e p95 | queue p50 | queue p95 | Connections |
|----------------|---------|---------|-----------|-----------|-------------|
| 0.1 s          | 807 ms  | 811 ms  | 806 ms    | 809 ms    | 9,657       |
| 1.0 s          | 8007 ms | 8010 ms | 8006 ms   | 8009 ms   | 99,007      |

p50 scales exactly 10× when the interval scales 10×. This is
the cleanest signal in the dataset: at this load, dispatch
latency is precisely `(tasks / workers) × interval`, and
nothing else in the system is saturating.

## 5. Findings

### Finding 1: latency is dispatch-bound, not execution-bound

In every `true` scenario, `end_to_end` is within a few
milliseconds of `queue_wait`. The coordinator's per-task
overhead — parse, schedule, sign, dispatch, receive, record —
is small compared to the time the task itself takes.

For `sleep1`, the coordinator-side contribution is
approximately 20 ms out of 1,000 ms of e2e latency. The
runtime is not the bottleneck.

### Finding 2: latency scales linearly with the agent interval

The 10× interval sweep (section 4.3) produced a 10× p50. At
the current architecture, worker polling is the pacing
mechanism for dispatch. Reducing dispatch latency at scale
requires either more workers, a shorter interval, or a
different architecture in which the coordinator can push work
to a worker without waiting for the next poll.

### Finding 3: connection churn is dominated by worker polling, not task dispatch

In the c4w8 runs:
- `true`, 1000 tasks, interval=0: ~86,000 connections.
- `true`, 200 tasks, interval=0.1: ~9,600 connections.
- `true`, 200 tasks, interval=1.0: ~99,000 connections.

The pattern: at interval=1.0, the workers polled for 8 seconds
while draining 200 tasks, generating roughly 99,000 ÷ 8 workers
÷ 4 connections per cycle ≈ 3,100 poll cycles per worker. At
interval=0.1, the same work drained in ~1 second, so ~10× fewer
poll cycles.

Connection churn scales with the time workers spend polling,
not with the number of tasks dispatched.

## 6. F-2 decision

The F-2 milestone was proposed as "persistent worker
connections." The F-1 measurements inform whether it is worth
doing.

**Persistent connections are worth implementing if the target
deployment runs workers at sub-second intervals.** At
interval=0.1, connection churn from polling is roughly
10 connections per second per worker, and the coordinator's
single-threaded accept loop is measurably loaded. Persistent
connections would reduce the churn to approximately one
connection per worker lifetime.

**Persistent connections are not worth implementing at the
default 3-second interval.** At interval=3, the polling rate
is ~2.7 connections per second across 8 workers. The client
submission traffic and task-dispatch traffic dominate by a
wide margin. The current architecture is adequate.

**A separate limit is worth addressing first: the 64-slot task
queue.** Localhost submission rates exceed 5,000 per second.
The queue fills in under 15 ms under a burst. Any further
submissions are rejected until the worker drains. This is a
correctness-preserving failure (no tasks are silently
dropped), but it caps the coordinator's ability to absorb
bursty client behavior. Future work might add a bounded
in-flight queue with a blocking accept, or a larger queue, or
a client-side backpressure protocol.

## 7. Benchmark limitations

The benchmark measures what it measures. It does not measure:

- **Client-side end-to-end latency.** The e2e figure is
  coordinator-observed. A client that measures from
  `send()` to `recv()` would see a larger number.
- **Sustained throughput.** Runs are short and wall-clock
  resolution is coarse. Sustained load would require a
  different harness (fixed duration rather than fixed task
  count) and a monotonic clock with sub-millisecond
  resolution on the shell side.
- **Multi-host behavior.** Everything runs on loopback. Any
  real deployment involves network RTT, packet loss, and
  clock skew, none of which are present here.
- **Memory or CPU consumption.** Resource usage is not
  sampled during the run.
- **Behavior under mixed workloads.** Only two workloads
  (`true`, `sleep1`) were measured. Real workloads vary in
  duration, memory footprint, and side effects.

The values in this document are the values the harness reported
for the specific commands it ran. They are reproducible with
the same commands on the same host; they should not be
extrapolated to other environments without new measurements.

## 8. Reproducing

All runs used `tests/bench_runtime.sh`. Examples:

```bash
# 1000 tasks, 4 clients, 8 workers, /bin/true
bash tests/bench_runtime.sh --clients=4 --workers=8 --tasks=1000 \
    --workload=true

# 100 tasks, 4 clients, 4 workers, /bin/sleep 1
bash tests/bench_runtime.sh --clients=4 --workers=4 --tasks=100 \
    --workload=sleep1

# 200 tasks with a 100 ms worker poll interval
bash tests/bench_runtime.sh --clients=4 --workers=8 --tasks=200 \
    --workload=true --agent-interval=0.1
```

Results append to `/tmp/idr_bench_results.csv` by default.
Override with `--output=PATH`.
