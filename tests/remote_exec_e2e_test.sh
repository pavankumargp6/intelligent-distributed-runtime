#!/bin/bash
#
# End-to-end IDR test:
#
#   1. Build coordinator, worker, client.
#   2. Start the real coordinator.
#   3. Submit a task BEFORE any worker is running
#      (verifies the task stays queued).
#   4. Start the real worker agent.
#   5. Wait for the coordinator to log the result.
#   6. Verify task id=1, worker id=1, exit=0, state=2.
#
# Exit 0 on PASS, nonzero on FAIL.

set -u
cd "$(dirname "$0")/.."

COORD_BIN=/tmp/idr_coord_e2e
WORKER_BIN=/tmp/idr_worker_e2e
CLIENT_BIN=/tmp/idr_client_e2e

COORD_LOG=/tmp/idr_coord_e2e.log
WORKER_LOG=/tmp/idr_worker_e2e.log

echo "===== Building coordinator ====="
gcc -Wall -Wextra \
    src/runtime_server_main.c src/runtime_server.c src/runtime.c \
    src/worker_registry.c src/task_manager.c src/task_queue.c \
    src/scheduler.c src/worker_pool.c src/worker.c src/worker_resources.c \
    src/process_info.c src/cpu_monitor.c src/task_executor.c src/task_metrics.c \
    src/runtime_stats.c src/network_protocol.c src/worker_registration.c \
    src/coordinator_tasks.c src/coordinator_handlers.c src/wire_fields.c \
    -o "$COORD_BIN" || exit 1

echo "===== Building worker ====="
gcc -Wall -Wextra -Isrc \
    src/worker_agent.c src/worker.c src/worker_resources.c \
    src/process_info.c src/cpu_monitor.c \
    src/worker_registration.c src/worker_registration_client.c \
    src/worker_task_client.c src/wire_fields.c \
    src/task_executor.c src/task_metrics.c src/network_protocol.c \
    -o "$WORKER_BIN" || exit 1

echo "===== Building client ====="
gcc -Wall -Wextra -Isrc \
    tests/client_submit.c src/network_protocol.c src/wire_fields.c \
    -o "$CLIENT_BIN" || exit 1

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

echo "===== Submitting task BEFORE any worker is running ====="
"$CLIENT_BIN" echo hello world
CLIENT_RC=$?

echo "===== Starting worker ====="
"$WORKER_BIN" 1 > "$WORKER_LOG" 2>&1 &
WORKER_PID=$!

echo "===== Waiting for end-to-end result (up to 15s) ====="
FOUND=0
for i in $(seq 1 15); do
    if grep -q "Result: task=" "$COORD_LOG" 2>/dev/null; then
        FOUND=1
        break
    fi
    sleep 1
done

kill "$WORKER_PID" 2>/dev/null
kill "$COORD_PID" 2>/dev/null
wait 2>/dev/null
true

echo
echo "===== Coordinator log ====="
cat "$COORD_LOG"
echo
echo "===== Worker log (tail) ====="
tail -20 "$WORKER_LOG"
echo

if [ "$CLIENT_RC" -ne 0 ]; then
    echo "===== E2E test: FAIL (client rejected) ====="
    exit 1
fi

if [ "$FOUND" -eq 0 ]; then
    echo "===== E2E test: FAIL (no result before timeout) ====="
    exit 1
fi

if grep -q "Accepted task 1" "$COORD_LOG" \
   && grep -q "Dispatched task 1 to worker 1" "$COORD_LOG" \
   && grep -q "Result: task=1 worker=1 exit=0 state=2" "$COORD_LOG"; then
    echo "===== E2E test: PASS ====="
    exit 0
fi

echo "===== E2E test: FAIL (unexpected result) ====="
exit 1
