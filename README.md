# Intelligent Distributed Runtime (IDR)

**Intelligent Distributed Runtime (IDR)** is an experimental distributed
task execution system written in C. It uses a coordinator--worker
architecture to accept tasks from clients, schedule them to workers,
execute commands, and report results.

> **Project status:** This is an ongoing engineering project, not a
> production-ready distributed platform. Tests cover selected components
> and failure scenarios; they do not establish complete fault tolerance
> or production readiness.

## Architecture

``` mermaid
flowchart TD
    Client["Client applications"]
    Coordinator["Coordinator"]
    Scheduler["Task queue and scheduler"]
    WAL[("Write-ahead log")]
    Worker1["Worker agent"]
    Worker2["Worker agent"]

    Client -->|"Submit / query / status / metrics"| Coordinator
    Coordinator --> Scheduler
    Coordinator --> WAL
    Worker1 -->|"Register / heartbeat / request task"| Coordinator
    Worker2 -->|"Register / heartbeat / request task"| Coordinator
    Coordinator -->|"Assignment / response"| Worker1
    Coordinator -->|"Assignment / response"| Worker2
    Worker1 -->|"Accept / result"| Coordinator
    Worker2 -->|"Accept / result"| Coordinator
```

## Current capabilities

-   TCP communication between clients, coordinator, and worker agents.
-   Task submission, scheduling, command execution, result reporting,
    and status queries.
-   Worker registration and heartbeat-based liveness tracking.
-   Resource-aware task selection and worker-specific pull scheduling.
-   Length-prefixed protocol framing with a maximum payload size of 4096
    bytes.
-   HMAC-SHA256 message authentication using separate client and worker
    secrets, timestamp checks, and replay-related protections.
-   Write-ahead logging (WAL), log replay, and recovery/maintenance
    code.
-   Selected worker-failure and dispatch-timeout handling.
-   Unit and integration tests for protocol handling, authentication,
    scheduling, logging, recovery, and execution.

These are implemented areas, not a claim that every failure mode is
handled or that the system is production-hardened.

## Scheduling and execution

Workers request tasks from the coordinator. The scheduler scans queued
work for a task that fits the requesting worker's available resources.
It can skip a task that does not fit and select a later eligible task.

The worker executes the assigned command as a local process and reports
the result. Clients can query task status using the identifier returned
by submission.

Task states defined by the protocol include `CREATED`, `RUNNING`,
`COMPLETED`, and `FAILED`.

Selected dispatch-timeout handling distinguishes idempotent from
non-idempotent tasks. This does **not** guarantee exactly-once
execution: a command may have performed external side effects before a
failure is detected, so retries are not always safe.

## Network protocol

Messages use a length-prefixed TCP frame:

``` text
[4-byte network-order unsigned payload length][payload]
```

Payloads use an ASCII, pipe-delimited message format, with
length-prefixed fields for values that may contain delimiters or spaces.
Each request/response pair uses its own connection.

See [`docs/PROTOCOL.md`](docs/PROTOCOL.md) for message formats and
implementation details.

## Authentication and security

Secure mode is enabled by default. HMAC-SHA256 authenticates messages
using separate client and worker secrets.

Set both secrets before starting the coordinator, worker, and clients:

``` bash
export IDR_WORKER_SECRET='replace-with-a-strong-worker-secret'
export IDR_CLIENT_SECRET='replace-with-a-strong-client-secret'
```

Use independent, randomly generated secrets that satisfy the minimum
length enforced by the implementation. Do not use the placeholder values
above.

For local development and tests, authentication can be disabled with
`IDR_INSECURE=1`. In insecure mode, the coordinator requires a loopback
bind address. Do not use insecure mode for network-accessible
deployments.

**Security limitations:** - HMAC authenticates messages but does not
encrypt traffic. - TLS transport encryption is not implemented. -
Authentication does not make arbitrary submitted commands safe. - IDR is
not a hardened multi-tenant sandbox for untrusted workloads.

Do not expose the runtime to untrusted networks or use it to execute
untrusted commands.

## Requirements

-   Linux or a compatible POSIX environment
-   GCC or a compatible C compiler
-   GNU Make
-   Bash for shell-based tests

Ubuntu on WSL can be used as a development environment.

## Build

``` bash
git clone https://github.com/pavankumargp6/intelligent-distributed-runtime.git
cd intelligent-distributed-runtime
make
```

Build outputs are written under `build/`.

  Binary                   Purpose
  ------------------------ -------------------------
  `build/runtime_server`   Coordinator
  `build/worker_agent`     Worker agent
  `build/client_submit`    Submit a task
  `build/client_query`     Query task state
  `build/client_status`    Request runtime status
  `build/client_metrics`   Request runtime metrics

Useful Make targets:

``` bash
make          # Build binaries
make asan     # AddressSanitizer and UndefinedBehaviorSanitizer build
make ubsan    # UndefinedBehaviorSanitizer build
make analyzer # GCC static analysis
make test     # Configured unit and integration tests
make clean    # Remove build outputs
make help     # List targets
```

Analysis and sanitizer builds can help find certain defects, but do not
prove the code is defect-free.

## Run locally

These examples assume the coordinator, worker, and client run on the
same machine.

### 1. Start the coordinator

``` bash
export IDR_WORKER_SECRET='replace-with-a-strong-worker-secret'
export IDR_CLIENT_SECRET='replace-with-a-strong-client-secret'
./build/runtime_server
```

The coordinator listens on TCP port `5001` by default and binds to
`127.0.0.1` unless configured otherwise. The default WAL path is
`./idr-wal.log`; set `IDR_WAL_PATH` to change it.

### 2. Start a worker

In another terminal, from the repository directory:

``` bash
export IDR_WORKER_SECRET='replace-with-a-strong-worker-secret'
export IDR_CLIENT_SECRET='replace-with-a-strong-client-secret'
./build/worker_agent 1
```

The worker's coordinator address is currently configured in the source
as `127.0.0.1`. The polling interval can be configured with
`IDR_AGENT_INTERVAL_SECONDS`.

### 3. Submit and query a task

In a third terminal:

``` bash
export IDR_WORKER_SECRET='replace-with-a-strong-worker-secret'
export IDR_CLIENT_SECRET='replace-with-a-strong-client-secret'
./build/client_submit echo hello world
```

Use the task identifier returned by submission to query its state:

``` bash
./build/client_query <task_id>
```

Replace `<task_id>` with the returned identifier. Consult the client
source and [`docs/PROTOCOL.md`](docs/PROTOCOL.md) for exact command-line
and response formats.

## Testing and benchmarks

Run the configured test suite:

``` bash
make test
```

Tests cover selected behavior in network framing, SHA-256/HMAC and
authentication, replay-related checks, WAL replay/recovery/compaction,
worker registration and resource reporting, scheduler fairness, command
execution, task lifecycle, dispatch timeouts, status, and metrics.

A benchmark harness is available at `tests/bench_runtime.sh`:

``` bash
bash tests/bench_runtime.sh --help
```

It can report end-to-end latency, queue wait, submission rate,
completion rate, and connection counts for a configured local workload.
Results depend on hardware, configuration, and workload. This README
intentionally omits performance figures until a specific run and its
configuration can be reproduced.

Continuous integration is configured in
[`.github/workflows/ci.yml`](.github/workflows/ci.yml).

## Repository layout

``` text
.
├── .github/workflows/   CI configuration
├── docs/
│   └── PROTOCOL.md      Protocol reference
├── src/                 Coordinator, worker, protocol, scheduler,
│                        authentication, logging, and recovery code
├── tests/               Unit/integration tests and benchmark harness
├── Makefile             Build, analysis, and test targets
└── README.md
```

## Known limitations

-   **Coordinator availability:** There is no replicated, highly
    available coordinator.
-   **Execution guarantees:** A worker or dispatch failure cannot prove
    that a command had no external side effects. Retry safety depends on
    task behavior and failure timing.
-   **Process supervision:** Heartbeat-based liveness does not prove
    every task subprocess is healthy.
-   **Transport security:** HMAC provides authentication, not
    encryption. TLS is not implemented.
-   **I/O and backpressure:** The coordinator multiplexes connections,
    but writes are not fully non-blocking with per-connection send
    buffers.
-   **Recovery guarantees:** WAL replay and recovery code exist, but do
    not by themselves guarantee correct recovery for every crash timing,
    partial write, or external side effect.
-   **Operational hardening:** The project does not claim
    production-grade isolation, deployment automation, or operational
    resilience.

## Project goals

IDR is a systems-programming project for exploring distributed
coordination, TCP protocol design, process execution, resource-aware
scheduling, message authentication, logging and recovery, and automated
testing. Prefer behavior demonstrated by the source and tests over
assumptions about what a distributed runtime should provide.
# Intelligent Distributed Runtime (IDR)

**Intelligent Distributed Runtime (IDR)** is an experimental distributed
task execution system written in C. It uses a coordinator--worker
architecture to accept tasks from clients, schedule them to workers,
execute commands, and report results.

> **Project status:** This is an ongoing engineering project, not a
> production-ready distributed platform. Tests cover selected components
> and failure scenarios; they do not establish complete fault tolerance
> or production readiness.

## Architecture

``` mermaid
flowchart TD
    Client["Client applications"]
    Coordinator["Coordinator"]
    Scheduler["Task queue and scheduler"]
    WAL[("Write-ahead log")]
    Worker1["Worker agent"]
    Worker2["Worker agent"]

    Client -->|"Submit / query / status / metrics"| Coordinator
    Coordinator --> Scheduler
    Coordinator --> WAL
    Worker1 -->|"Register / heartbeat / request task"| Coordinator
    Worker2 -->|"Register / heartbeat / request task"| Coordinator
    Coordinator -->|"Assignment / response"| Worker1
    Coordinator -->|"Assignment / response"| Worker2
    Worker1 -->|"Accept / result"| Coordinator
    Worker2 -->|"Accept / result"| Coordinator
```

## Current capabilities

-   TCP communication between clients, coordinator, and worker agents.
-   Task submission, scheduling, command execution, result reporting,
    and status queries.
-   Worker registration and heartbeat-based liveness tracking.
-   Resource-aware task selection and worker-specific pull scheduling.
-   Length-prefixed protocol framing with a maximum payload size of 4096
    bytes.
-   HMAC-SHA256 message authentication using separate client and worker
    secrets, timestamp checks, and replay-related protections.
-   Write-ahead logging (WAL), log replay, and recovery/maintenance
    code.
-   Selected worker-failure and dispatch-timeout handling.
-   Unit and integration tests for protocol handling, authentication,
    scheduling, logging, recovery, and execution.

These are implemented areas, not a claim that every failure mode is
handled or that the system is production-hardened.

## Scheduling and execution

Workers request tasks from the coordinator. The scheduler scans queued
work for a task that fits the requesting worker's available resources.
It can skip a task that does not fit and select a later eligible task.

The worker executes the assigned command as a local process and reports
the result. Clients can query task status using the identifier returned
by submission.

Task states defined by the protocol include `CREATED`, `RUNNING`,
`COMPLETED`, and `FAILED`.

Selected dispatch-timeout handling distinguishes idempotent from
non-idempotent tasks. This does **not** guarantee exactly-once
execution: a command may have performed external side effects before a
failure is detected, so retries are not always safe.

## Network protocol

Messages use a length-prefixed TCP frame:

``` text
[4-byte network-order unsigned payload length][payload]
```

Payloads use an ASCII, pipe-delimited message format, with
length-prefixed fields for values that may contain delimiters or spaces.
Each request/response pair uses its own connection.

See [`docs/PROTOCOL.md`](docs/PROTOCOL.md) for message formats and
implementation details.

## Authentication and security

Secure mode is enabled by default. HMAC-SHA256 authenticates messages
using separate client and worker secrets.

Set both secrets before starting the coordinator, worker, and clients:

``` bash
export IDR_WORKER_SECRET='replace-with-a-strong-worker-secret'
export IDR_CLIENT_SECRET='replace-with-a-strong-client-secret'
```

Use independent, randomly generated secrets that satisfy the minimum
length enforced by the implementation. Do not use the placeholder values
above.

For local development and tests, authentication can be disabled with
`IDR_INSECURE=1`. In insecure mode, the coordinator requires a loopback
bind address. Do not use insecure mode for network-accessible
deployments.

**Security limitations:** - HMAC authenticates messages but does not
encrypt traffic. - TLS transport encryption is not implemented. -
Authentication does not make arbitrary submitted commands safe. - IDR is
not a hardened multi-tenant sandbox for untrusted workloads.

Do not expose the runtime to untrusted networks or use it to execute
untrusted commands.

## Requirements

-   Linux or a compatible POSIX environment
-   GCC or a compatible C compiler
-   GNU Make
-   Bash for shell-based tests

Ubuntu on WSL can be used as a development environment.

## Build

``` bash
git clone https://github.com/pavankumargp6/intelligent-distributed-runtime.git
cd intelligent-distributed-runtime
make
```

Build outputs are written under `build/`.

  Binary                   Purpose
  ------------------------ -------------------------
  `build/runtime_server`   Coordinator
  `build/worker_agent`     Worker agent
  `build/client_submit`    Submit a task
  `build/client_query`     Query task state
  `build/client_status`    Request runtime status
  `build/client_metrics`   Request runtime metrics

Useful Make targets:

``` bash
make          # Build binaries
make asan     # AddressSanitizer and UndefinedBehaviorSanitizer build
make ubsan    # UndefinedBehaviorSanitizer build
make analyzer # GCC static analysis
make test     # Configured unit and integration tests
make clean    # Remove build outputs
make help     # List targets
```

Analysis and sanitizer builds can help find certain defects, but do not
prove the code is defect-free.

## Run locally

These examples assume the coordinator, worker, and client run on the
same machine.

### 1. Start the coordinator

``` bash
export IDR_WORKER_SECRET='replace-with-a-strong-worker-secret'
export IDR_CLIENT_SECRET='replace-with-a-strong-client-secret'
./build/runtime_server
```

The coordinator listens on TCP port `5001` by default and binds to
`127.0.0.1` unless configured otherwise. The default WAL path is
`./idr-wal.log`; set `IDR_WAL_PATH` to change it.

### 2. Start a worker

In another terminal, from the repository directory:

``` bash
export IDR_WORKER_SECRET='replace-with-a-strong-worker-secret'
export IDR_CLIENT_SECRET='replace-with-a-strong-client-secret'
./build/worker_agent 1
```

The worker's coordinator address is currently configured in the source
as `127.0.0.1`. The polling interval can be configured with
`IDR_AGENT_INTERVAL_SECONDS`.

### 3. Submit and query a task

In a third terminal:

``` bash
export IDR_WORKER_SECRET='replace-with-a-strong-worker-secret'
export IDR_CLIENT_SECRET='replace-with-a-strong-client-secret'
./build/client_submit echo hello world
```

Use the task identifier returned by submission to query its state:

``` bash
./build/client_query <task_id>
```

Replace `<task_id>` with the returned identifier. Consult the client
source and [`docs/PROTOCOL.md`](docs/PROTOCOL.md) for exact command-line
and response formats.

## Testing and benchmarks

Run the configured test suite:

``` bash
make test
```

Tests cover selected behavior in network framing, SHA-256/HMAC and
authentication, replay-related checks, WAL replay/recovery/compaction,
worker registration and resource reporting, scheduler fairness, command
execution, task lifecycle, dispatch timeouts, status, and metrics.

A benchmark harness is available at `tests/bench_runtime.sh`:

``` bash
bash tests/bench_runtime.sh --help
```

It can report end-to-end latency, queue wait, submission rate,
completion rate, and connection counts for a configured local workload.
Results depend on hardware, configuration, and workload. This README
intentionally omits performance figures until a specific run and its
configuration can be reproduced.

Continuous integration is configured in
[`.github/workflows/ci.yml`](.github/workflows/ci.yml).

## Repository layout

``` text
.
├── .github/workflows/   CI configuration
├── docs/
│   └── PROTOCOL.md      Protocol reference
├── src/                 Coordinator, worker, protocol, scheduler,
│                        authentication, logging, and recovery code
├── tests/               Unit/integration tests and benchmark harness
├── Makefile             Build, analysis, and test targets
└── README.md
```

## Known limitations

-   **Coordinator availability:** There is no replicated, highly
    available coordinator.
-   **Execution guarantees:** A worker or dispatch failure cannot prove
    that a command had no external side effects. Retry safety depends on
    task behavior and failure timing.
-   **Process supervision:** Heartbeat-based liveness does not prove
    every task subprocess is healthy.
-   **Transport security:** HMAC provides authentication, not
    encryption. TLS is not implemented.
-   **I/O and backpressure:** The coordinator multiplexes connections,
    but writes are not fully non-blocking with per-connection send
    buffers.
-   **Recovery guarantees:** WAL replay and recovery code exist, but do
    not by themselves guarantee correct recovery for every crash timing,
    partial write, or external side effect.
-   **Operational hardening:** The project does not claim
    production-grade isolation, deployment automation, or operational
    resilience.

## Project goals

IDR is a systems-programming project for exploring distributed
coordination, TCP protocol design, process execution, resource-aware
scheduling, message authentication, logging and recovery, and automated
testing. Prefer behavior demonstrated by the source and tests over
assumptions about what a distributed runtime should provide.
