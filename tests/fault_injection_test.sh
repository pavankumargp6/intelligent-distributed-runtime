#!/bin/bash
#
# Fault injection test:
#
#   1. Start coordinator.
#   2. Submit a long-running task (sleep 30).
#   3. Start worker under setsid so we can kill its whole process group.
#   4. Wait for coordinator to dispatch the task.
#   5. Kill the worker group while the task is in flight.
#   6. Wait for the coordinator to reap the in-flight task.
#   7. Query and assert TASK_FAILED with exit_status -1.
#
# Proves: the coordinator does not lose tasks when a worker
# disappears mid-execution, and does not silently retry them.

set -u
export IDR_WAL_PATH=/tmp/idr_wal_fault_injection_test.log

if ss -ltn 2>/dev/null | grep -q ":5001 "; then
    echo "FAIL: port 5001 already in use. Kill leftovers first:"
    echo "  pkill -9 -f 'idr_coord|idr_worker|runtime_server|worker_agent'"
    exit 1
fi
COORD_PID=""
WORKER_PID=""

cleanup() {
    if [ -n "$WORKER_PID" ]; then
        kill -KILL -"$WORKER_PID" 2>/dev/null
        kill -KILL "$WORKER_PID" 2>/dev/null
        wait "$WORKER_PID" 2>/dev/null
    fi
    if [ -n "$COORD_PID" ]; then
        kill -KILL "$COORD_PID" 2>/dev/null
        wait "$COORD_PID" 2>/dev/null
    fi
    true
}
trap cleanup EXIT

cd "$(dirname "$0")/.."

COORD_BIN=/tmp/idr_coord_fault
WORKER_BIN=/tmp/idr_worker_fault
CLIENT_BIN=/tmp/idr_client_fault
QUERY_BIN=/tmp/idr_query_fault

COORD_LOG=/tmp/idr_coord_fault.log
WORKER_LOG=/tmp/idr_worker_fault.log
QUERY_LOG=/tmp/idr_query_fault.log

echo "===== Building coordinator ====="
gcc -Wall -Wextra \
    src/runtime_server_main.c src/runtime_server.c src/runtime.c \
    src/worker_registry.c src/task_manager.c src/task_queue.c \
    src/scheduler.c src/worker_pool.c src/worker.c src/worker_resources.c \
    src/process_info.c src/cpu_monitor.c src/task_executor.c src/task_metrics.c \
    src/runtime_stats.c src/network_protocol.c src/worker_registration.c \
    src/coordinator_tasks.c src/coordinator_handlers.c src/wire_fields.c \
    src/log.c \
    src/wal.c \
    src/recovery.c \
    src/connection_pool.c \
    -o "$COORD_BIN" || exit 1

echo "===== Building worker ====="
gcc -Wall -Wextra -Isrc \
    src/worker_agent.c src/worker.c src/worker_resources.c \
    src/log.c \
    src/process_info.c src/cpu_monitor.c \
    src/worker_registration.c src/worker_registration_client.c \
    src/worker_task_client.c src/wire_fields.c \
    src/task_executor.c src/task_metrics.c src/network_protocol.c \
    -o "$WORKER_BIN" || exit 1

echo "===== Building client ====="
gcc -Wall -Wextra -Isrc \
    tests/client_submit.c src/network_protocol.c src/wire_fields.c \
    -o "$CLIENT_BIN" || exit 1

echo "===== Building query client ====="
gcc -Wall -Wextra -Isrc \
    tests/client_query.c src/network_protocol.c \
    -o "$QUERY_BIN" || exit 1

: > "$COORD_LOG"
: > "$WORKER_LOG"

echo "===== Starting coordinator ====="
"$COORD_BIN" > "$COORD_LOG" 2>&1 &
COORD_PID=$!
sleep 1

if ! kill -0 "$COORD_PID" 2>/dev/null; then
    echo "FAIL: coordinator did not stay running."
    cat "$COORD_LOG"
    exit 1
fi

echo "===== Submitting long task ====="
"$CLIENT_BIN" sleep 30
CLIENT_RC=$?

if [ "$CLIENT_RC" -ne 0 ]; then
    echo "FAIL: client rejected the submission"
    exit 1
fi

echo "===== Starting worker (setsid) ====="
setsid "$WORKER_BIN" 1 > "$WORKER_LOG" 2>&1 < /dev/null &
WORKER_PID=$!

echo "===== Waiting for dispatch ====="
DISPATCHED=0
for i in $(seq 1 15); do
    if grep -q "Dispatched task 1 to worker 1" "$COORD_LOG" 2>/dev/null; then
        DISPATCHED=1
        break
    fi
    sleep 1
done

if [ "$DISPATCHED" -eq 0 ]; then
    echo "FAIL: task was never dispatched"
    echo "=== Coordinator log ==="
    cat "$COORD_LOG"
    echo "=== Worker log ==="
    tail -30 "$WORKER_LOG"
    exit 1
fi

echo "===== Killing worker process group ====="
kill -TERM -"$WORKER_PID" 2>/dev/null
sleep 1

echo "===== Waiting for reaper (up to 25s) ====="
REAPED=0
for i in $(seq 1 25); do
    if grep -q "Reaped in-flight task 1: worker 1 not live" "$COORD_LOG" 2>/dev/null; then
        REAPED=1
        break
    fi
    sleep 1
done

echo
echo "===== Coordinator log ====="
cat "$COORD_LOG"
echo

if [ "$REAPED" -eq 0 ]; then
    echo "===== Fault test: FAIL (task was not reaped) ====="
    exit 1
fi

echo "===== Querying task 1 ====="
"$QUERY_BIN" 1 | tee "$QUERY_LOG"
QUERY_RC=${PIPESTATUS[0]}

if ! grep -q "QUERY_RESULT|1|1|-1|3" "$QUERY_LOG"; then
    echo "===== Fault test: FAIL (query did not return TASK_FAILED) ====="
    exit 1
fi

if [ "$QUERY_RC" -ne 0 ]; then
    echo "===== Fault test: FAIL (query client exit $QUERY_RC) ====="
    exit 1
fi

echo "===== Fault test: PASS ====="
exit 0
