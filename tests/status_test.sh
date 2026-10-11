#!/bin/bash
#
# Status query test.
#
#   1. Start coordinator.
#   2. STATUS on empty runtime -> workers=0, queue=0, submitted=0.
#   3. Submit a task with no worker running.
#   4. STATUS again -> submitted=1, queue=1 (task waiting, no worker).

set -u
export IDR_WAL_PATH=/tmp/idr_wal_status_test.log
rm -f "$IDR_WAL_PATH"

if ss -ltn 2>/dev/null | grep -q ":5001 "; then
    echo "FAIL: port 5001 already in use. Kill leftovers first:"
    echo "  pkill -9 -f 'idr_coord|idr_worker|runtime_server|worker_agent'"
    exit 1
fi

COORD_PID=""

cleanup() {
    if [ -n "$COORD_PID" ]; then
        kill -KILL "$COORD_PID" 2>/dev/null
        wait "$COORD_PID" 2>/dev/null
    fi
    true
}
trap cleanup EXIT

cd "$(dirname "$0")/.."

COORD_BIN=/tmp/idr_coord_status
CLIENT_BIN=/tmp/idr_client_status_submit
STATUS_BIN=/tmp/idr_client_status

COORD_LOG=/tmp/idr_coord_status.log

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

echo "===== Building submit client ====="
gcc -Wall -Wextra -Isrc \
    tests/client_submit.c src/network_protocol.c src/wire_fields.c \
    -o "$CLIENT_BIN" || exit 1

echo "===== Building status client ====="
gcc -Wall -Wextra -Isrc \
    tests/client_status.c src/network_protocol.c \
    -o "$STATUS_BIN" || exit 1

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

echo "===== STATUS on empty runtime ====="
OUT1=$("$STATUS_BIN")
echo "$OUT1"

if ! printf '%s' "$OUT1" | grep -q 'STATUS_REPLY|'; then
    echo "FAIL: no STATUS_REPLY"
    exit 1
fi
if ! printf '%s' "$OUT1" | grep -q 'workers=0'; then
    echo "FAIL: expected workers=0 on empty runtime"
    exit 1
fi
if ! printf '%s' "$OUT1" | grep -q 'queue=0'; then
    echo "FAIL: expected queue=0 on empty runtime"
    exit 1
fi
if ! printf '%s' "$OUT1" | grep -q 'submitted=0'; then
    echo "FAIL: expected submitted=0 on empty runtime"
    exit 1
fi

echo "===== Submitting one task (no worker) ====="
"$CLIENT_BIN" echo status-test

sleep 1

echo "===== STATUS after submit ====="
OUT2=$("$STATUS_BIN")
echo "$OUT2"

if ! printf '%s' "$OUT2" | grep -q 'submitted=1'; then
    echo "FAIL: expected submitted=1"
    exit 1
fi
if ! printf '%s' "$OUT2" | grep -q 'queue=1'; then
    echo "FAIL: expected queue=1 (no worker to dispatch to)"
    exit 1
fi

echo
echo "===== Coordinator log ====="
cat "$COORD_LOG"
echo

echo "===== Status test: PASS ====="
exit 0
