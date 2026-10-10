# Intelligent Distributed Runtime

An experimental distributed computing runtime being built from the ground up to explore operating systems, distributed systems, scheduling, resource management, fault tolerance, and intelligent workload placement.

## Project Goal

The goal is to build a runtime that can:

- Discover and monitor system processes and resources
- Execute computational tasks
- Schedule tasks across multiple worker nodes
- Detect worker failures
- Reassign interrupted tasks
- Coordinate distributed nodes
- Experiment with intelligent workload scheduling
- Provide isolation and resource controls for workloads

The project is being developed incrementally, starting with a local runtime and gradually evolving toward a distributed and adaptive execution system.

---

# Current Progress

## Linux Process Layer

- [x] Basic C process manager
- [x] Process information reader using `/proc`
- [x] Automatic PID discovery
- [x] PID detection using `/proc`
- [x] Git/GitHub project setup

## Resource Monitoring

- [x] Process information collection
- [x] CPU usage measurement
- [x] CPU tick monitoring using `/proc/[PID]/stat`
- [x] Resource snapshot abstraction
- [x] Process memory and thread monitoring

## Task Execution

- [x] Task abstraction
- [x] Local task executor using `fork()` and `execvp()`
- [x] Child process creation
- [x] Task exit-status handling
- [x] Task failure detection
- [x] FIFO task queue
- [x] Task manager
- [x] FIFO scheduler
- [x] End-to-end task execution
- [x] Concurrent multi-task execution experiment

## Execution Experiment

The runtime was tested by submitting three independent tasks, each requiring approximately three seconds to execute.

All three tasks were started before waiting for their completion.

### Result

```text
Tasks      : 3
Elapsed    : 3.01 seconds
Expected   : approximately 3 seconds
# Intelligent Distributed Runtime (IDR)

A C/POSIX runtime that distributes tasks from a coordinator to
worker processes over TCP. A client submits a command, the
coordinator queues and schedules it, a worker executes it locally,
and the result flows back. Built with Linux system programming:
sockets, `fork`/`execvp`, `poll`, signals, and a length-prefixed
TCP protocol.

## Status

Working end to end:

- Client task submission over TCP
- Coordinator-driven scheduling and dispatch
- Remote execution on a worker process
- Result reporting back to the coordinator
- Client task query (read-after-write across processes)
- Worker registration and heartbeat-based liveness detection
- Fault handling: worker disappearing mid-task is detected and
  the task is recorded as `TASK_FAILED`
- Pull-model scheduling: task selection is scoped to the worker
  that asked, not to a global-best choice

Not yet implemented:

- Concurrent connections in the coordinator (see *Limitations*)
- Persistence across coordinator restarts
- Authentication or authorization
- Retry / recovery policies

## Architecture

    ┌────────────┐          ┌──────────────────────┐          ┌────────────┐
    │   client   │  SUBMIT  │      coordinator     │  ASSIGN  │   worker   │
    │            ├─────────►│                      ├─────────►│   agent    │
    │  submit +  │          │  queue + scheduler   │          │            │
    │   query    │  QUERY   │  registry + metrics  │  RESULT  │  fork/exec │
    │            │◄─────────┤                      │◄─────────┤            │
    └────────────┘          └──────────────────────┘          └────────────┘
                                    ▲                                ▲
                                    │                                │
                                    │ REGISTER / HEARTBEAT /         │
                                    │ TASK_REQUEST                   │
                                    └────────────────────────────────┘

Every arrow is a length-prefixed TCP message on port 5001.

## Protocol

Framing: `[4-byte network-order uint32 payload length][payload]`.
Payloads are newline-free ASCII. Variable-length string fields
(command, each argument) are length-prefixed as `<len>:<bytes>` so
characters like `|`, spaces, and newlines inside them cannot
corrupt the message.

| Direction        | Message                                              |
|------------------|------------------------------------------------------|
| worker → coord   | `REGISTER|<id>|<cpu>|<mem_mb>`                       |
| coord → worker   | `REGISTER_ACK` / `REGISTER_REJECT`                   |
| worker → coord   | `HEARTBEAT|<id>`                                     |
| coord → worker   | `HEARTBEAT_ACK` / `HEARTBEAT_REJECT`                 |
| client → coord   | `SUBMIT|<len>:<cmd>|<argc>|<len>:<a1>|...`           |
| coord → client   | `SUBMIT_ACK|<task_id>` / `SUBMIT_REJECT`             |
| worker → coord   | `TASK_REQUEST|<worker_id>`                           |
| coord → worker   | `TASK_ASSIGN|<id>|<len>:<cmd>|<argc>|<a1>|...` / `NO_TASK` |
| worker → coord   | `TASK_RESULT|<task_id>|<worker_id>|<exit>|<state>`   |
| coord → worker   | `TASK_RESULT_ACK`                                    |
| client → coord   | `QUERY|<task_id>`                                    |
| coord → client   | `QUERY_RESULT|<id>|<worker>|<exit>|<state>` / `QUERY_PENDING|<id>` / `QUERY_UNKNOWN|<id>` |

Task state values: `0 = CREATED`, `1 = RUNNING`, `2 = COMPLETED`,
`3 = FAILED`.

Example: submitting `echo hello world` produces the wire message

    SUBMIT|4:echo|2|5:hello|5:world

## Building

Everything builds with `gcc -Wall -Wextra`. No external
dependencies beyond libc and POSIX.

### Coordinator

```bash
gcc -Wall -Wextra \
    src/runtime_server_main.c src/runtime_server.c src/runtime.c \
    src/worker_registry.c src/task_manager.c src/task_queue.c \
    src/scheduler.c src/worker_pool.c src/worker.c src/worker_resources.c \
    src/process_info.c src/cpu_monitor.c src/task_executor.c src/task_metrics.c \
    src/runtime_stats.c src/network_protocol.c src/worker_registration.c \
    src/coordinator_tasks.c src/coordinator_handlers.c src/wire_fields.c \
    -o runtime_server
