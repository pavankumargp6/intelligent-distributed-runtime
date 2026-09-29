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

## Current Progress

### Linux Process Layer

- [x] Basic C process manager
- [x] Process information reader using `/proc`
- [x] Automatic PID discovery
- [x] PID detection using `/proc`
- [x] Git/GitHub project setup

### Upcoming

- [ ] Automatic process information collection
- [ ] CPU and memory monitoring
- [ ] Task execution system
- [ ] Job/task queue
- [ ] Basic scheduler
- [ ] Multi-node communication
- [ ] Failure detection and recovery
- [ ] Distributed coordination
- [ ] Intelligent scheduling
- [ ] Sandboxing and resource limits
- [ ] Benchmarking

## Technology Stack

- C
- Linux / WSL2
- Bash
- Git / GitHub
- Python
- Go
- Docker
- eBPF

## Philosophy

The project is being developed incrementally with an emphasis on understanding the underlying systems rather than relying on high-level abstractions.

Each major component will be implemented, tested, measured, and documented before being integrated into the larger runtime.
