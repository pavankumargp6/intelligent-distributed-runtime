#!/bin/bash
#
# WAL recovery test:
#
#   1. Start coordinator with a fresh WAL path.
#   2. Submit 3 tasks with no worker running.
#   3. STATUS -> submitted=3, queue=3.
#   4. SIGKILL the coordinator.
#   5. Restart with the same WAL path.
#   6. STATUS -> submitted=3, queue=3 (recovered from WAL).
#   7. WAL is compacted: contains exactly 3 lines.
#   8. Submit a 4th task, verify it gets id=4.

set -u

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
    rm -f "$WAL_PATH"
    true
}
trap cleanup EXIT

cd "$(dirname "$0")/.."

COORD_BIN=/tmp/idr_coord_wal
CLIENT_BIN=/tmp/idr_client_wal_submit
STATUS_BIN=/tmp/idr_client_wal_status

COORD_LOG=/tmp/idr_coord_wal.log
WAL_PATH=/tmp/idr_wal_test_recovery_$$.log

echo "===== Building coordinator ====="
gcc -Wall -Wextra \
    src/runtime_server_main.c src/runtime_server.c src/runtime.c \
    src/worker_registry.c src/task_manager.c src/task_queue.c \
    src/scheduler.c src/worker_pool.c src/worker.c src/worker_resources.c \
    src/process_info.c src/cpu_monitor.c src/task_executor.c src/task_metrics.c \
    src/runtime_stats.c src/network_protocol.c src/worker_registration.c \
    src/coordinator_tasks.c src/coordinator_handlers.c src/wire_fields.c \
    src/log.c src/wal.c src/recovery.c \
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

rm -f "$WAL_PATH"
: > "$COORD_LOG"

start_coord() {
    IDR_WAL_PATH="$WAL_PATH" "$COORD_BIN" >> "$COORD_LOG" 2>&1 &
    COORD_PID=$!
    sleep 1

    if ! kill -0 "$COORD_PID" 2>/dev/null; then
        echo "FAIL: coordinator did not stay running."
        cat "$COORD_LOG"
        exit 1
    fi
}

stop_coord_hard() {
    if [ -n "$COORD_PID" ]; then
        kill -KILL "$COORD_PID" 2>/dev/null
        wait "$COORD_PID" 2>/dev/null
        COORD_PID=""
        sleep 1
    fi
}

echo "===== Starting coordinator (first time) ====="
start_coord

echo "===== Submitting 3 tasks ====="
"$CLIENT_BIN" echo one
"$CLIENT_BIN" echo two
"$CLIENT_BIN" echo three

sleep 1

echo "===== STATUS before crash ====="
OUT1=$("$STATUS_BIN")
echo "$OUT1"

if ! printf '%s' "$OUT1" | grep -q 'submitted=3'; then
    echo "FAIL: expected submitted=3 before crash"
    exit 1
fi
if ! printf '%s' "$OUT1" | grep -q 'queue=3'; then
    echo "FAIL: expected queue=3 before crash"
    exit 1
fi

echo "===== Killing coordinator (SIGKILL) ====="
stop_coord_hard

echo "===== WAL file ====="
ls -l "$WAL_PATH"
echo "----- contents -----"
cat "$WAL_PATH"
echo "----- end -----"

echo "===== Restarting coordinator ====="
start_coord

sleep 1

echo "===== STATUS after recovery ====="
OUT2=$("$STATUS_BIN")
echo "$OUT2"

if ! printf '%s' "$OUT2" | grep -q 'submitted=3'; then
    echo "FAIL: expected submitted=3 after recovery"
    exit 1
fi
if ! printf '%s' "$OUT2" | grep -q 'queue=3'; then
    echo "FAIL: expected queue=3 after recovery"
    exit 1
fi

echo "===== WAL size after restart ====="
AFTER_LINES=$(wc -l < "$WAL_PATH")
echo "WAL line count after restart: $AFTER_LINES"
if [ "$AFTER_LINES" -ne 3 ]; then
    echo "FAIL: expected 3 lines after compaction, got $AFTER_LINES"
    exit 1
fi

echo "===== Submitting 4th task (should get id=4) ====="
OUT_SUBMIT=$("$CLIENT_BIN" echo four)
echo "$OUT_SUBMIT"

if ! printf '%s' "$OUT_SUBMIT" | grep -q 'id=4'; then
    echo "FAIL: expected 4th task to get id=4"
    exit 1
fi

echo
echo "===== Coordinator log ====="
cat "$COORD_LOG"
echo

echo "===== WAL recovery test: PASS ====="
exit 0
