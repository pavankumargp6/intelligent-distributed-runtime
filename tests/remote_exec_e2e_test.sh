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
#   7. Verify a task_latency event was emitted.
#
# Exit 0 on PASS, nonzero on FAIL.

set -u
export IDR_WORKER_SECRET="idr-test-worker-secret-do-not-use-in-production"
export IDR_CLIENT_SECRET="idr-test-client-secret-do-not-use-in-production"
export IDR_WAL_PATH=/tmp/idr_wal_remote_exec_e2e_test.log
rm -f "$IDR_WAL_PATH"

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

COORD_BIN=/tmp/idr_coord_e2e
WORKER_BIN=/tmp/idr_worker_e2e
CLIENT_BIN=/tmp/idr_client_e2e

QUERY_BIN=/tmp/idr_query_e2e
QUERY_LOG=/tmp/idr_query_e2e.log

COORD_LOG=/tmp/idr_coord_e2e.log
WORKER_LOG=/tmp/idr_worker_e2e.log

echo "===== Building coordinator ====="
gcc -Wall -Wextra \
    src/runtime_server_main.c src/runtime_server.c src/runtime.c \
    src/worker_registry.c src/task_manager.c src/task_queue.c \
    src/scheduler.c src/worker_pool.c src/worker.c src/worker_resources.c \
    src/process_info.c src/cpu_monitor.c src/task_executor.c src/task_metrics.c \
    src/runtime_stats.c src/network_protocol.c src/worker_registration.c \
    src/sha256.c src/hmac.c src/auth.c src/auth_config.c \
    src/coordinator_tasks.c src/coordinator_handlers.c src/wire_fields.c \
    src/log.c \
    src/wal.c \
    src/recovery.c src/metrics_json.c \
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
    src/sha256.c src/hmac.c src/auth.c src/auth_config.c \
    -o "$WORKER_BIN" || exit 1

echo "===== Building client ====="
gcc -Wall -Wextra -Isrc \
    tests/client_submit.c src/network_protocol.c src/wire_fields.c \
    src/sha256.c src/hmac.c src/auth.c src/auth_config.c \
    -o "$CLIENT_BIN" || exit 1

echo "===== Building query client ====="
gcc -Wall -Wextra -Isrc \
    tests/client_query.c src/network_protocol.c \
    src/sha256.c src/hmac.c src/auth.c src/auth_config.c \
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

echo "===== Querying task 1 ====="
"$QUERY_BIN" 1 | tee "$QUERY_LOG"
QUERY_RC=${PIPESTATUS[0]}

if ! grep -q "QUERY_RESULT|1|1|0|2" "$QUERY_LOG"; then
    echo "===== E2E test: FAIL (query did not return expected result) ====="
    exit 1
fi

if [ "$QUERY_RC" -ne 0 ]; then
    echo "===== E2E test: FAIL (query client exit $QUERY_RC) ====="
    exit 1
fi

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

# Phase 2b: verify a per-task latency event was emitted with
# the fields we expect.
if ! grep -q '"event":"task_latency"' "$COORD_LOG"; then
    echo "===== E2E test: FAIL (no task_latency event in coordinator log) ====="
    exit 1
fi

if ! grep -q 'queue_wait_ms=' "$COORD_LOG" \
   || ! grep -q 'end_to_end_ms=' "$COORD_LOG"; then
    echo "===== E2E test: FAIL (task_latency missing expected fields) ====="
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
