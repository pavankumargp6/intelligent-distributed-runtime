#!/bin/bash
#
# WAL overflow test.
#
# Writes a WAL with more than RECOVERY_MAX_TASKS (4096) records
# and verifies the coordinator refuses to start with an
# explicit error. The records are written directly, without
# fsync, so generation is fast.

set -u
export IDR_WORKER_SECRET="idr-test-worker-secret-do-not-use-in-production"
export IDR_CLIENT_SECRET="idr-test-client-secret-do-not-use-in-production"

if ss -ltn 2>/dev/null | grep -q ":5001 "; then
    echo "FAIL: port 5001 already in use."
    exit 1
fi

COORD_PID=""
WAL_PATH=/tmp/idr_wal_overflow_$$.log
COORD_LOG=/tmp/idr_coord_overflow_$$.log

cleanup() {
    [ -n "$COORD_PID" ] && kill -KILL "$COORD_PID" 2>/dev/null
    rm -f "$WAL_PATH" "$COORD_LOG"
    true
}
trap cleanup EXIT

cd "$(dirname "$0")/.."

COORD_BIN=/tmp/idr_coord_overflow

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

echo "===== Generating 4100-record WAL ====="
awk 'BEGIN {
    for (i = 1; i <= 4100; i++) {
        printf "SUBMIT|%d|4:echo|1|1:x|0\n", i
    }
}' > "$WAL_PATH"

LINES=$(wc -l < "$WAL_PATH")
echo "WAL has $LINES lines"

echo "===== Starting coordinator (should refuse) ====="
IDR_WAL_PATH="$WAL_PATH" "$COORD_BIN" > "$COORD_LOG" 2>&1 &
COORD_PID=$!

sleep 2

if kill -0 "$COORD_PID" 2>/dev/null; then
    echo "FAIL: coordinator is still running despite overflow"
    kill -KILL "$COORD_PID" 2>/dev/null
    cat "$COORD_LOG"
    exit 1
fi

echo "===== Coordinator log ====="
cat "$COORD_LOG"
echo

if ! grep -q '"event":"wal_recovery_overflow"' "$COORD_LOG"; then
    echo "FAIL: expected wal_recovery_overflow in coordinator log"
    exit 1
fi

if ! grep -q '"event":"wal_replay_failed"' "$COORD_LOG"; then
    echo "FAIL: expected wal_replay_failed in coordinator log"
    exit 1
fi

echo "===== WAL overflow test: PASS ====="
exit
