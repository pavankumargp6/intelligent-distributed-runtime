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
