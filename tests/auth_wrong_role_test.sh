#!/bin/bash
#
# Wrong-role integration test.
#
# Starts the coordinator in secure mode. Runs a stub that
# registers as a worker but signs REGISTER with
# IDR_CLIENT_SECRET instead of IDR_WORKER_SECRET. The
# coordinator must reject the message with auth_failed and
# close the connection without responding.

set -u
unset IDR_INSECURE

export IDR_WORKER_SECRET="idr-test-worker-secret-do-not-use-in-production"
export IDR_CLIENT_SECRET="idr-test-client-secret-do-not-use-in-production"

if ss -ltn 2>/dev/null | grep -q ":5001 "; then
    echo "FAIL: port 5001 already in use. Kill leftovers first."
    exit 1
fi

COORD_PID=""
COORD_LOG=/tmp/idr_coord_wrong_role.log

cleanup() {
    [ -n "$COORD_PID" ] && kill -KILL "$COORD_PID" 2>/dev/null
    rm -f /tmp/idr_wal_wrong_role.log
    true
}
trap cleanup EXIT

cd "$(dirname "$0")/.."

COORD_BIN=/tmp/idr_coord_wrong_role
STUB_BIN=/tmp/idr_wrong_role_worker

echo "===== Building coordinator ====="
gcc -Wall -Wextra \
    src/runtime_server_main.c src/runtime_server.c src/runtime.c \
    src/worker_registry.c src/task_manager.c src/task_queue.c \
    src/scheduler.c src/worker_pool.c src/worker.c src/worker_resources.c \
    src/process_info.c src/cpu_monitor.c src/task_executor.c src/task_metrics.c \
    src/runtime_stats.c src/network_protocol.c src/worker_registration.c \
    src/coordinator_tasks.c src/coordinator_handlers.c src/wire_fields.c \
    src/log.c src/wal.c src/recovery.c src/connection_pool.c \
    src/sha256.c src/hmac.c src/auth.c src/auth_config.c \
    -o "$COORD_BIN" || exit 1

echo "===== Building wrong-role worker stub ====="
gcc -Wall -Wextra -Isrc \
    tests/wrong_role_worker.c \
    src/worker.c src/worker_resources.c \
    src/process_info.c src/cpu_monitor.c \
    src/worker_registration.c \
    src/network_protocol.c src/wire_fields.c \
    src/log.c src/sha256.c src/hmac.c src/auth.c src/auth_config.c \
    -o "$STUB_BIN" || exit 1

: > "$COORD_LOG"

echo "===== Starting coordinator ====="
IDR_WAL_PATH=/tmp/idr_wal_wrong_role.log "$COORD_BIN" > "$COORD_LOG" 2>&1 &
COORD_PID=$!
sleep 1

if ! kill -0 "$COORD_PID" 2>/dev/null; then
    echo "FAIL: coordinator did not stay running."
    cat "$COORD_LOG"
    exit 1
fi

echo "===== Running wrong-role stub ====="
"$STUB_BIN" 1
STUB_RC=$?

sleep 1

echo "===== Coordinator log ====="
cat "$COORD_LOG"
echo

if [ "$STUB_RC" -ne 0 ]; then
    echo "FAIL: stub exit code $STUB_RC (expected 0 = rejected)"
    exit 1
fi

if ! grep -q '"event":"auth_failed"' "$COORD_LOG"; then
    echo "FAIL: coordinator did not log auth_failed"
    exit 1
fi

if grep -q '"event":"worker_registered"' "$COORD_LOG"; then
    echo "FAIL: coordinator registered a wrong-role worker"
    exit 1
fi

echo "===== Wrong-role test: PASS ====="
exit 0
