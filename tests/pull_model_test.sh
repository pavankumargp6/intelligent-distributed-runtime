#!/bin/bash
#
# Pull-model scheduling test.
#
# Demonstrates that the coordinator scopes task selection to the
# worker that actually asked, rather than the globally-best worker.
#
# Setup:
#   1. Start coordinator.
#   2. Start worker 1 (will register first, therefore be the
#      "global best" as computed by find_best_worker).
#   3. SIGSTOP worker 1 right after it registers, so it stays
#      WORKER_ACTIVE (fresh last_seen) but stops polling.
#   4. Start worker 2. It will poll for work while worker 1 is
#      still considered active.
#   5. Submit a task.
#
# Expected under old global-best selection:
#   The scheduler picks worker 1 (still active, still best), but
#   worker 2 is the one asking. Task is requeued repeatedly, and
#   the coordinator logs "Scheduler picked worker 1, but 2 asked".
#   Only after worker 1 ages out (~10s) does worker 2 get it.
#
# Expected under pull-model selection:
#   Worker 2 asks, worker 2 fits, task is dispatched to worker 2
#   on the first poll. No "Scheduler picked worker" line ever
#   appears.
#
# Assertions:
#   - the task completes
#   - the coordinator log contains no "Scheduler picked worker" line

set -u
export IDR_WORKER_SECRET="idr-test-worker-secret-do-not-use-in-production"
export IDR_CLIENT_SECRET="idr-test-client-secret-do-not-use-in-production"
export IDR_WAL_PATH=/tmp/idr_wal_pull_model_test.log
rm -f "$IDR_WAL_PATH"

if ss -ltn 2>/dev/null | grep -q ":5001 "; then
    echo "FAIL: port 5001 already in use. Kill leftovers first:"
    echo "  pkill -9 -f 'idr_coord|idr_worker|runtime_server|worker_agent'"
    exit 1
fi
COORD_PID=""
W1_PID=""
W2_PID=""

cleanup() {
    if [ -n "$W1_PID" ]; then
        kill -KILL "$W1_PID" 2>/dev/null
        wait "$W1_PID" 2>/dev/null
    fi
    if [ -n "$W2_PID" ]; then
        kill -KILL "$W2_PID" 2>/dev/null
        wait "$W2_PID" 2>/dev/null
    fi
    if [ -n "$COORD_PID" ]; then
        kill -KILL "$COORD_PID" 2>/dev/null
        wait "$COORD_PID" 2>/dev/null
    fi
    true
}
trap cleanup EXIT

cd "$(dirname "$0")/.."

COORD_BIN=/tmp/idr_coord_pull
WORKER_BIN=/tmp/idr_worker_pull
CLIENT_BIN=/tmp/idr_client_pull

COORD_LOG=/tmp/idr_coord_pull.log
W1_LOG=/tmp/idr_worker_pull_1.log
W2_LOG=/tmp/idr_worker_pull_2.log

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

: > "$COORD_LOG"
: > "$W1_LOG"
: > "$W2_LOG"

echo "===== Starting coordinator ====="
"$COORD_BIN" > "$COORD_LOG" 2>&1 &
COORD_PID=$!
sleep 1

if ! kill -0 "$COORD_PID" 2>/dev/null; then
    echo "FAIL: coordinator did not stay running."
    cat "$COORD_LOG"
    exit 1
fi

echo "===== Starting worker 1 ====="
"$WORKER_BIN" 1 > "$W1_LOG" 2>&1 &
W1_PID=$!

# Wait for worker 1 to register
for i in $(seq 1 8); do
    if grep -q "Worker registered with runtime" "$COORD_LOG" 2>/dev/null; then
        break
    fi
    sleep 1
done

if ! grep -q "Worker registered with runtime" "$COORD_LOG" 2>/dev/null; then
    echo "FAIL: worker 1 never registered"
    cat "$COORD_LOG"
    exit 1
fi

echo "===== Stopping worker 1 (SIGSTOP) ====="
kill -STOP "$W1_PID" 2>/dev/null
sleep 1

if ! kill -0 "$W1_PID" 2>/dev/null; then
    echo "FAIL: worker 1 died from SIGSTOP"
    exit 1
fi

echo "===== Starting worker 2 ====="
"$WORKER_BIN" 2 > "$W2_LOG" 2>&1 &
W2_PID=$!

# Wait for worker 2 to register (2 registrations total)
for i in $(seq 1 8); do
    count=$(grep -c "Worker registered with runtime" "$COORD_LOG" 2>/dev/null || echo 0)
    if [ "$count" -ge 2 ]; then
        break
    fi
    sleep 1
done

count=$(grep -c "Worker registered with runtime" "$COORD_LOG" 2>/dev/null || echo 0)
if [ "$count" -lt 2 ]; then
    echo "FAIL: worker 2 never registered (count=$count)"
    cat "$COORD_LOG"
    exit 1
fi

echo "===== Submitting task ====="
"$CLIENT_BIN" echo pull-model
CLIENT_RC=$?

if [ "$CLIENT_RC" -ne 0 ]; then
    echo "FAIL: client rejected the submission"
    exit 1
fi

echo "===== Waiting for task to complete (up to 15s) ====="
DONE=0
for i in $(seq 1 15); do
    if grep -q "Result: task=1" "$COORD_LOG" 2>/dev/null; then
        DONE=1
        break
    fi
    sleep 1
done

echo
echo "===== Coordinator log ====="
cat "$COORD_LOG"
echo

if [ "$DONE" -eq 0 ]; then
    echo "===== Pull model test: FAIL (task never completed) ====="
    exit 1
fi

if grep -q "Scheduler picked worker" "$COORD_LOG"; then
    echo "===== Pull model test: FAIL (global-best churn line present) ====="
    exit 1
fi

if ! grep -q "Dispatched task 1 to worker 2" "$COORD_LOG"; then
    echo "===== Pull model test: FAIL (task was not dispatched to worker 2) ====="
    exit 1
fi

echo "===== Pull model test: PASS ====="
exit 0
