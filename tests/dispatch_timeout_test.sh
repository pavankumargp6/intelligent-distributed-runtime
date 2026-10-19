#!/bin/bash
#
# Dispatch timeout test.
#
# Uses a deliberately silent worker that receives TASK_ASSIGN
# but never sends TASK_ACCEPT. Verifies the coordinator's
# dispatch-timeout reaper (Phase 4d):
#
#   Part A (non-idempotent task):
#     - coordinator logs task_failed_timeout
#     - query returns QUERY_RESULT|1|1|-1|3
#
#   Part B (idempotent task):
#     - coordinator logs task_requeued_timeout
#
# DISPATCH_TIMEOUT_SECONDS is 15. Test waits 20s per part.

set -u
export IDR_WORKER_SECRET="idr-test-worker-secret-do-not-use-in-production"
export IDR_CLIENT_SECRET="idr-test-client-secret-do-not-use-in-production"

if ss -ltn 2>/dev/null | grep -q ":5001 "; then
    echo "FAIL: port 5001 already in use. Kill leftovers first:"
    echo "  pkill -9 -f 'idr_coord|idr_worker|runtime_server|worker_agent'"
    exit 1
fi

export IDR_WAL_PATH=/tmp/idr_wal_dispatch_timeout_test.log
rm -f "$IDR_WAL_PATH"

COORD_PID=""
WORKER_PID=""

cleanup() {
    if [ -n "$WORKER_PID" ]; then
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

COORD_BIN=/tmp/idr_coord_timeout
WORKER_BIN=/tmp/idr_silent_worker
CLIENT_BIN=/tmp/idr_client_submit_timeout
QUERY_BIN=/tmp/idr_client_query_timeout

COORD_LOG=/tmp/idr_coord_timeout.log

echo "===== Building coordinator ====="
gcc -Wall -Wextra \
    src/runtime_server_main.c src/runtime_server.c src/runtime.c \
    src/worker_registry.c src/task_manager.c src/task_queue.c \
    src/scheduler.c src/worker_pool.c src/worker.c src/worker_resources.c \
    src/process_info.c src/cpu_monitor.c src/task_executor.c src/task_metrics.c \
    src/runtime_stats.c src/network_protocol.c src/worker_registration.c \
    src/sha256.c src/hmac.c src/auth.c src/auth_config.c \
    src/coordinator_tasks.c src/coordinator_handlers.c src/wire_fields.c \
    src/log.c src/wal.c src/recovery.c src/metrics_json.c src/connection_pool.c \
    -o "$COORD_BIN" || exit 1

echo "===== Building silent worker ====="
gcc -Wall -Wextra -Isrc \
    tests/silent_worker.c src/worker.c src/worker_resources.c \
    src/process_info.c src/cpu_monitor.c \
    src/worker_registration.c src/worker_registration_client.c \
    src/worker_task_client.c src/wire_fields.c \
    src/log.c src/network_protocol.c \
    src/sha256.c src/hmac.c src/auth.c src/auth_config.c \
    -o "$WORKER_BIN" || exit 1

echo "===== Building submit client ====="
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

echo "===== Starting coordinator ====="
"$COORD_BIN" > "$COORD_LOG" 2>&1 &
COORD_PID=$!
sleep 1

if ! kill -0 "$COORD_PID" 2>/dev/null; then
    echo "FAIL: coordinator did not stay running."
    cat "$COORD_LOG"
    exit 1
fi

echo "===== Starting silent worker ====="
"$WORKER_BIN" 1 >> "$COORD_LOG" 2>&1 &
WORKER_PID=$!

for i in $(seq 1 8); do
    if grep -q "Worker registered with runtime" "$COORD_LOG" 2>/dev/null; then
        break
    fi
    sleep 1
done

if ! grep -q "Worker registered with runtime" "$COORD_LOG"; then
    echo "FAIL: silent worker did not register"
    cat "$COORD_LOG"
    exit 1
fi

# ========= Part A: non-idempotent =========
echo
echo "===== Part A: non-idempotent task ====="
"$CLIENT_BIN" echo A

for i in $(seq 1 10); do
    if grep -q "Dispatched task 1 to worker 1" "$COORD_LOG"; then
        break
    fi
    sleep 1
done

if ! grep -q "Dispatched task 1 to worker 1" "$COORD_LOG"; then
    echo "FAIL: task 1 never dispatched"
    cat "$COORD_LOG"
    exit 1
fi

echo "  waiting for dispatch timeout (20s)..."
sleep 20

if ! grep -q "task_failed_timeout" "$COORD_LOG"; then
    echo "FAIL: task_failed_timeout not logged"
    cat "$COORD_LOG"
    exit 1
fi
echo "  task_failed_timeout logged."

echo "  querying task 1..."
QUERY_OUT=$("$QUERY_BIN" 1)
echo "  $QUERY_OUT"

if ! printf '%s' "$QUERY_OUT" | grep -q 'QUERY_RESULT|1|1|-1|3'; then
    echo "FAIL: expected QUERY_RESULT|1|1|-1|3"
    cat "$COORD_LOG"
    exit 1
fi

# ========= Part B: idempotent =========
echo
echo "===== Part B: idempotent task ====="
"$CLIENT_BIN" --idempotent echo B

for i in $(seq 1 10); do
    if grep -q "Dispatched task 2 to worker 1" "$COORD_LOG"; then
        break
    fi
    sleep 1
done

if ! grep -q "Dispatched task 2 to worker 1" "$COORD_LOG"; then
    echo "FAIL: task 2 never dispatched"
    cat "$COORD_LOG"
    exit 1
fi

echo "  waiting for dispatch timeout (20s)..."
sleep 20

if ! grep -q "task_requeued_timeout" "$COORD_LOG"; then
    echo "FAIL: task_requeued_timeout not logged"
    cat "$COORD_LOG"
    exit 1
fi
echo "  task_requeued_timeout logged."

echo
echo "===== Coordinator log ====="
cat "$COORD_LOG"
echo

echo "===== Dispatch timeout test: PASS ====="
exit 0
