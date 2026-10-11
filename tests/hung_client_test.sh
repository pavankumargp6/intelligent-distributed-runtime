#!/bin/bash
#
# Hung-client test.
#
# Proves the coordinator no longer blocks on a slow / stuck
# client. Under the old blocking accept loop, this test would
# time out: the coordinator would be stuck in recv() on the
# hung connection and would never service the well-behaved
# client_submit and client_query that follow.
#
# Steps:
#   1. Start coordinator.
#   2. Start a hung client that sends a partial frame and holds.
#   3. Submit a task via client_submit and confirm it is ACKed
#      within 2 seconds wall-clock.
#   4. Query the task and confirm a valid response.
#   5. Wait for the coordinator to reap the idle connection.

set -u
export IDR_WAL_PATH=/tmp/idr_wal_hung_client_test.log
COORD_PID=""
HUNG_PID=""

cleanup() {
    [ -n "$HUNG_PID" ] && kill -KILL "$HUNG_PID" 2>/dev/null
    [ -n "$COORD_PID" ] && kill -KILL "$COORD_PID" 2>/dev/null
    true
}
trap cleanup EXIT

if ss -ltn 2>/dev/null | grep -q ':5001 '; then
    echo "FAIL: port 5001 already in use. Kill leftovers first:"
    echo "  pkill -9 -f 'idr_coord|idr_worker|runtime_server|worker_agent'"
    exit 1
fi

cd "$(dirname "$0")/.."

COORD_BIN=/tmp/idr_coord_hung
CLIENT_BIN=/tmp/idr_client_hung
QUERY_BIN=/tmp/idr_query_hung
HUNG_BIN=/tmp/idr_hung

COORD_LOG=/tmp/idr_coord_hung.log

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

echo "===== Building client_submit ====="
gcc -Wall -Wextra -Isrc \
    tests/client_submit.c src/network_protocol.c src/wire_fields.c \
    -o "$CLIENT_BIN" || exit 1

echo "===== Building client_query ====="
gcc -Wall -Wextra -Isrc \
    tests/client_query.c src/network_protocol.c \
    -o "$QUERY_BIN" || exit 1

echo "===== Building hung_client ====="
gcc -Wall -Wextra -Isrc \
    tests/hung_client.c \
    -o "$HUNG_BIN" || exit 1

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

echo "===== Starting hung client (holds 10 s) ====="
"$HUNG_BIN" 127.0.0.1 5001 10 &
HUNG_PID=$!
sleep 1

echo "===== Submitting a task while hung client is connected ====="
START=$(date +%s)
"$CLIENT_BIN" echo hung-test
CLIENT_RC=$?
END=$(date +%s)
ELAPSED=$((END - START))

echo "client_submit took ${ELAPSED}s, exit=${CLIENT_RC}"

if [ "$CLIENT_RC" -ne 0 ]; then
    echo "===== Hung client test: FAIL (submit rejected) ====="
    cat "$COORD_LOG"
    exit 1
fi

if [ "$ELAPSED" -gt 2 ]; then
    echo "===== Hung client test: FAIL (submit blocked ${ELAPSED}s) ====="
    cat "$COORD_LOG"
    exit 1
fi

echo "===== Querying task 1 ====="
"$QUERY_BIN" 1 | tee /tmp/hung_query.out

if grep -q "^Response: QUERY_" /tmp/hung_query.out; then
    :
else
    echo "===== Hung client test: FAIL (no valid query response) ====="
    cat "$COORD_LOG"
    exit 1
fi

echo "===== Waiting for coordinator to reap the idle connection ====="
REAPED=0
for i in $(seq 1 10); do
    if grep -q "Idle connections closed: 1" "$COORD_LOG" 2>/dev/null; then
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
    echo "===== Hung client test: FAIL (idle connection never reaped) ====="
    exit 1
fi

echo "===== Hung client test: PASS ====="
exit 0
