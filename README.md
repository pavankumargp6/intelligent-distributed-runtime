# Intelligent Distributed Runtime (IDR)

[![CI](https://github.com/pavankumargp6/intelligent-distributed-runtime/actions/workflows/ci.yml/badge.svg)](https://github.com/pavankumargp6/intelligent-distributed-runtime/actions/workflows/ci.yml)

A C/POSIX runtime that distributes tasks from a coordinator to
worker processes over TCP. A client submits a command, the
coordinator queues and schedules it, a worker executes it locally,
and the result flows back. Built with Linux system programming:
sockets, `fork`/`execvp`, `poll`, signals, and a length-prefixed
TCP protocol.

## Status

Working end to end (all with passing integration tests):

- Client task submission over TCP
- Coordinator-driven scheduling and dispatch
- Remote execution on a worker process
- Result reporting back to the coordinator
- Client task query (read-after-write across processes)
- Worker registration and heartbeat-based liveness detection
- Fault handling: a worker disappearing mid-task is detected
  and the task is recorded as `TASK_FAILED`
- Pull-model scheduling: task selection is scoped to the worker
  that asked, not to a global-best choice
- Concurrent connections in the coordinator: a hung client does
  not block other clients or workers

Not yet implemented:

- Non-blocking writes with per-connection send buffers
- Persistence across coordinator restarts
- Task-runner crash detection when heartbeats continue
- Authentication or authorization
- Retry / recovery policies

## Architecture

    client  ──SUBMIT──►  coordinator  ──TASK_ASSIGN──►  worker agent
            ◄──QUERY───  (queue,      ◄──TASK_RESULT──  (fork/exec)
                         scheduler,
                         registry,
                         metrics)
            ▲                                           ▲
            │                                           │
            └─ REGISTER / HEARTBEAT / TASK_REQUEST ──────┘

Every arrow is a length-prefixed TCP message on port 5001.
## Performance & Benchmarks

The IDR coordinator is built to handle high-concurrency task bursts efficiently. Below are the results from a benchmark run using 4 local workers processing a queue of 1,000 no-op tasks (`true` command).

**Hardware:** WSL2, Ubuntu (Windows 11 host)
**Workload:** 1,000 tasks / 4 workers

| Metric | p50 (Median) | p95 (Tail) |
| :--- | :--- | :--- |
| **End-to-End Latency** | 15 ms | 29 ms |
| **Queue Wait Time** | 4 ms | 17 ms |

*   **Submission Throughput:** 3,618 tasks/sec
*   **Completion Throughput:** 1,000 tasks/sec (wall-clock)
*   **Total Connections:** 50,632 (demonstrating aggressive client-side retry and backpressure handling)

### Scheduler Fairness
The scheduler uses an **O(1) scan-and-swap** algorithm to prevent Head-of-Line (HOL) blocking. If the task at the front of the queue does not fit the available CPU/Memory of a requesting worker, the scheduler scans for the next task that *does* fit, swaps it to the front, and assigns it. Automated integration tests verify that multiple workers split workloads exactly 50/50 (5 tasks each for a 10-task test) without starvation.
## Protocol

Framing: `[4-byte network-order uint32 payload length][payload]`.

This section is a summary. The canonical reference, including
every message type, the authentication scheme, worked wire
examples, and the task state machine, is in
[docs/PROTOCOL.md](docs/PROTOCOL.md).

Changes to a message format should be made to PROTOCOL.md
first, then reflected here.
Payloads are newline-free ASCII. Variable-length string fields
(command, each argument) are length-prefixed as `<len>:<bytes>` so
characters like `|`, spaces, and newlines inside them cannot
corrupt the message.

| Direction        | Message                                                       |
|------------------|---------------------------------------------------------------|
| worker → coord   | `REGISTER|<id>|<cpu>|<mem_mb>`                                |
| coord → worker   | `REGISTER_ACK` / `REGISTER_REJECT`                            |
| worker → coord   | `HEARTBEAT|<id>`                                              |
| coord → worker   | `HEARTBEAT_ACK` / `HEARTBEAT_REJECT`                          |
| client → coord   | `SUBMIT\|<len>:<cmd>\|<argc>\|<len>:<a1>\|...\|[idempotent:0\|1]` |
| coord → client   | `SUBMIT_ACK|<task_id>` / `SUBMIT_REJECT`                      |
| worker → coord   | `TASK_ACCEPT\|<task_id>`                                    |
| coord → worker   | `TASK_ACCEPT_ACK\|<task_id>` / `TASK_ACCEPT_REJECT\|<task_id>\|<reason>` |
| worker → coord   | `TASK_REQUEST|<worker_id>`                                    |
| coord → worker   | `TASK_ASSIGN|<id>|<len>:<cmd>|<argc>|<a1>|...` / `NO_TASK`    |
| worker → coord   | `TASK_RESULT|<task_id>|<worker_id>|<exit>|<state>`            |
| coord → worker   | `TASK_RESULT_ACK`                                             |
| client → coord   | `QUERY|<task_id>`                                             |
| coord → client   | `QUERY_RESULT|<id>|<worker>|<exit>|<state>` / `QUERY_PENDING|<id>` / `QUERY_UNKNOWN|<id>` |

Task state values: `0 = CREATED`, `1 = RUNNING`, `2 = COMPLETED`,
`3 = FAILED`.

Example: submitting `echo hello world` produces the wire message

    SUBMIT|4:echo|2|5:hello|5:world

## Authentication

Every message on the wire is signed with HMAC-SHA256 by
default. The signed form is:

    <original_message>|<timestamp>|<hex_hmac>

where `<hex_hmac>` is HMAC-SHA256 of

    IDR-AUTH-V1|<direction>|<role>|<timestamp>|<original_message>

computed with a role-specific secret. `<direction>` and
`<role>` bind the MAC to the message type and to the signer,
so a MAC computed for one context cannot be replayed in
another.

Two modes are supported:

- **Secure mode (default).** Both `IDR_WORKER_SECRET` and
  `IDR_CLIENT_SECRET` must be set (32 bytes minimum each).
  Every incoming message is verified and every outgoing
  response is signed. The coordinator refuses to start
  without them.

- **Insecure development mode.** Enabled by
  `IDR_INSECURE=1`. Authentication is disabled entirely.
  The coordinator additionally requires a loopback bind
  address in this mode and refuses to start on `0.0.0.0`.

Signed messages carry a Unix timestamp. The receiver
rejects messages older than 60 seconds or more than 5
seconds in the future. This narrows the replay window; a
recent-MAC cache that fully closes it is on the roadmap.

HMAC authenticates but does not encrypt. The wire is still
plaintext. TLS is out of scope; do not expose the
coordinator to an untrusted network merely because
authentication was added.

## Building

Everything builds with `gcc -Wall -Wextra`. No external
dependencies beyond libc and POSIX.

    make          # coordinator, worker, and both clients -> build/
    make asan     # same, with AddressSanitizer + UBSan -> build/asan/
    make test     # run unit + integration tests
    make clean    # remove build/
    make help     # target list

The coordinator binary is at `build/runtime_server`, the worker at
`build/worker_agent`, and the clients at `build/client_submit` and
`build/client_query`.

To reproduce the exact gcc commands the Makefile runs, see the
`COORDINATOR_SRCS`, `WORKER_SRCS`, `CLIENT_SUBMIT_SRCS`, and
`CLIENT_QUERY_SRCS` variables at the top of `Makefile`.

## Running

Three terminals (or three hosts — change `RUNTIME_ADDRESS` in the
worker and client sources):

    # Terminal 1
    ./runtime_server

    # Terminal 2
    ./worker_agent 1

    # Terminal 3
    ./client_submit echo hello world     # Response: SUBMIT_ACK|1
    ./client_query 1                     # Response: QUERY_RESULT|1|1|0|2

By default the coordinator binds to `127.0.0.1` and is only
reachable from the local host. To accept connections from other
machines, set `IDR_BIND_ADDR=0.0.0.0` (or a specific local
address) before starting the coordinator:

    IDR_BIND_ADDR=0.0.0.0 ./build/runtime_server

There is no authentication, so opening the port exposes workers
to command execution from anyone who can reach the coordinator.
Do this only on a trusted network.

If you submit before any worker is running, the task is queued and
dispatched when a worker registers.

## Tests

All tests compile their own binaries into `/tmp`, start and tear
down processes, and exit non-zero on failure.

| Test                        | What it proves                                                     |
|-----------------------------|--------------------------------------------------------------------|
| `connection_pool_test.c`    | Incremental framing: partial reads, complete messages, oversize length, peer close, idle expiry. |
| `sha256_test.c`             | SHA-256 against NIST FIPS 180-4 vectors.                |
| `hmac_test.c`               | HMAC-SHA256 against RFC 4231 vectors.                   |
| `auth_test.c`               | Sign/verify round-trip and every failure mode.         |
| `worker_remote_exec_test.sh`| Worker-side protocol against a fake coordinator.                   |
| `remote_exec_e2e_test.sh`   | Full lifecycle: submit → dispatch → execute → report → query.       |
| `fault_injection_test.sh`   | A worker killed mid-task is detected and its task recorded as `TASK_FAILED`. |
| `pull_model_test.sh`        | Selection is scoped to the asking worker, not global-best.          |
| `hung_client_test.sh`       | A stuck client does not block other clients or workers.             |
| `dispatch_timeout_test.sh` | A silent worker never sends TASK_ACCEPT; the reaper fails non-idempotent tasks and re-queues idempotent ones. |

Run them:

    bash tests/remote_exec_e2e_test.sh
    bash tests/fault_injection_test.sh
    bash tests/pull_model_test.sh
    bash tests/hung_client_test.sh
    bash tests/dispatch_timeout_test.sh

    gcc -Wall -Wextra -Isrc \
        tests/connection_pool_test.c src/connection_pool.c src/network_protocol.c \
        -o /tmp/connection_pool_test && /tmp/connection_pool_test

If a test fails and leaves a coordinator holding port 5001:

    pkill -9 -f 'idr_coord|idr_worker|runtime_server|worker_agent'

## Repository layout

    docs/
      PROTOCOL.md               canonical wire protocol specification
## Repository layout

    src/
      runtime.c/h                 runtime core
      task.h                      task struct and state enum
      task_manager.c/h            FIFO wrapper
      task_queue.c/h              bounded queue (capacity 64)
      scheduler.c/h               worker selection, global and per-worker
      worker_pool.c/h             worker collection
      worker.c/h                  individual worker
      worker_resources.c/h        CPU/memory accounting
      process_info.c/h            /proc readers
      cpu_monitor.c/h             CPU sampling
      worker_registry.c/h         registration and liveness
      worker_registration.c/h     REGISTER / HEARTBEAT wire format
      worker_registration_client.c/h  worker-side registration
      worker_task_client.c/h      worker-side TASK_REQUEST / TASK_RESULT
      task_executor.c/h           local fork/execvp and waitpid
      task_metrics.c/h            per-task timing
      runtime_stats.c/h           runtime-wide counters
      network_protocol.c/h        framing
      wire_fields.c/h             length-prefixed field helpers
      coordinator_tasks.c/h       in-flight table + completed ring
      coordinator_handlers.c/h    SUBMIT / TASK_REQUEST / TASK_RESULT / QUERY
      connection_pool.c/h         concurrent connection handling
      runtime_server.c/h          poll loop, dispatch, housekeeping
      runtime_server_main.c       coordinator entry point
      worker_agent.c              worker entry point

    tests/
      client_submit.c             SUBMIT client
      client_query.c              QUERY client
      hung_client.c               partial-frame client stub
      fake_coordinator.c          coordinator stub for worker tests
      connection_pool_test.c      unit test for the connection pool
      runtime_scheduling_test.c   scheduler regression
      worker_remote_exec_test.sh
      remote_exec_e2e_test.sh
      fault_injection_test.sh
      pull_model_test.sh
      hung_client_test.sh

## Limitations

These are real, verified, and not hidden behind optimistic
language.

- **Single-threaded but I/O-multiplexed.** The coordinator handles
  many concurrent connections via `poll()`. Reads are
  non-blocking; writes are still blocking, which is fine for the
  ~30-byte responses on a low-latency link but would need
  per-connection send buffers for a slow or lossy network.
- **No persistence.** In-flight tasks and the completed ring live
  in memory. A coordinator restart loses both. Completed entries
  age out after 64 newer completions.
- **At-least-once for idempotent tasks; at-most-once for the
  rest.** A dispatched task is not trusted until the worker
  sends `TASK_ACCEPT`. If no accept arrives within
  `DISPATCH_TIMEOUT_SECONDS` (15), the coordinator checks the
  task's `idempotent` flag. Idempotent tasks (`SUBMIT|...|1`)
  are re-queued and re-dispatched. Non-idempotent tasks
  (`SUBMIT|...|0`, the default) are marked `TASK_FAILED` with
  `exit_status = -1` and are not retried. A task with external
  side effects (email, payment, file write) must be submitted
  as non-idempotent.
- **Worker-side task-runner crash without heartbeat loss is not
  detected.** If the worker's heartbeat continues but its
  forked task-runner child segfaults, the coordinator will not
  notice. The reaper in this version fires on missing
  `TASK_ACCEPT` and on missing heartbeats; it does not fire on
  a task whose worker is alive but whose executor is dead.
  Detecting that requires per-task execution timeouts or a
  worker-side signal.- **Task-runner crash without heartbeat loss is not detected.** If
  the worker's heartbeat continues but its forked task-runner child
  segfaults, the coordinator will not notice. Detecting this
  requires per-task timeouts or a worker-side signal, neither of
  which exists.
- **No transport encryption.** Authentication is enforced on
  every message (see the Authentication section), but the
  wire is still plaintext. TLS is out of scope. Do not
  expose the coordinator to an untrusted network merely
  because authentication is present.

## Roadmap

- **Phase 6 — Observability.** Structured logging, latency metrics
  (queue wait, dispatch latency, execution time, end-to-end), and
  a `Makefile` or build script so the README stops asking readers
  to paste multi-line gcc commands.
- **Later.** Per-connection worker state so workers don't
  re-register each poll cycle. Non-blocking writes with
  per-connection send buffers. Worker-side task-runner crash
  detection. A dispatch acknowledgement so at-least-once
  semantics become possible. Client-facing task cancellation.
  TLS.

## License


All rights reserved by the author.
