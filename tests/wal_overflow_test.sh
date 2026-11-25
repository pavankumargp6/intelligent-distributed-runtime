#!/bin/bash
#
# WAL overflow test (WAL 2.0).
#
# Seeds 4,100 SUBMIT+RESULT pairs — far beyond the old
# RECOVERY_MAX_TASKS limit of 4,096 — and verifies the
# coordinator starts, replays every record, retains every
# result, and continues allocating ids from 4101.
#
# The records are generated with the unsynced test builder;
# the coordinator itself replays them with full validation.

set -u
export IDR_WORKER_SECRET="idr-test-worker-secret-do-not-use-in-production"
export IDR_CLIENT_SECRET="idr-test-client-secret-do-not-use-in-production"

PAIRS=4100

if ss -ltn 2>/dev/null | grep -q ":5001 "; then
    echo "FAIL: port 5001 already in use."
    exit 1
fi

COORD_PID=""
WAL_BASE=/tmp/idr_wal_overflow_$$.log
COORD_LOG=/tmp/idr_coord_overflow_$$.log
QUERY_LOG=/tmp/idr_query_overflow_$$.log

cleanup() {
    [ -n "$COORD_PID" ] && kill -KILL "$COORD_PID" 2>/dev/null
    rm -f "$WAL_BASE"* "$COORD_LOG" "$QUERY_LOG"
    true
}
trap cleanup EXIT

cd "$(dirname "$0")/.."

COORD_BIN=/tmp/idr_coord_overflow
SEED_BIN=/tmp/idr_wal_seed
CLIENT_BIN=/tmp/idr_client_overflow_submit
STATUS_BIN=/tmp/idr_client_overflow_status
QUERY_BIN=/tmp/idr_client_overflow_query

echo "===== Building coordinator ====="
gcc -Wall -Wextra \
    src/runtime_server_main.c src/runtime_server.c src/runtime.c \
    src/worker_registry.c src/task_manager.c src/task_queue.c \
    src/scheduler.c src/worker_pool.c src/worker.c src/worker_resources.c \
    src/process_info.c src/cpu_monitor.c src/task_executor.c src/task_metrics.c \
    src/runtime_stats.c src/network_protocol.c src/runtime_endpoint.c src/worker_registration.c \
    src/sha256.c src/hmac.c src/auth.c src/auth_config.c \
    src/coordinator_tasks.c src/coordinator_handlers.c src/wire_fields.c \
    src/log.c src/wal.c src/recovery.c src/metrics_json.c src/connection_pool.c \
    -o "$COORD_BIN" || exit 1

echo "===== Building seed helper and clients ====="
gcc -Wall -Wextra -DIDR_TEST_WAL_NO_SYNC -Isrc \
    tests/wal_seed.c src/wal.c src/log.c \
    -o "$SEED_BIN" || exit 1

gcc -Wall -Wextra -Isrc \
    tests/client_submit.c src/network_protocol.c src/runtime_endpoint.c src/wire_fields.c \
    src/sha256.c src/hmac.c src/auth.c src/auth_config.c \
    -o "$CLIENT_BIN" || exit 1

gcc -Wall -Wextra -Isrc \
    tests/client_status.c src/network_protocol.c src/runtime_endpoint.c \
    src/sha256.c src/hmac.c src/auth.c src/auth_config.c \
    -o "$STATUS_BIN" || exit 1

gcc -Wall -Wextra -Isrc \
    tests/client_query.c src/network_protocol.c src/runtime_endpoint.c \
    src/sha256.c src/hmac.c src/auth.c src/auth_config.c \
    -o "$QUERY_BIN" || exit 1

echo "===== Seeding $PAIRS SUBMIT+RESULT pairs ====="
"$SEED_BIN" "$WAL_BASE" "$PAIRS" || exit 1

ls -l "$WAL_BASE".seg.* | head -3
echo "segments: $(ls "$WAL_BASE".seg.* | wc -l)"

echo "===== Starting coordinator (must accept the overflow WAL) ====="
IDR_WAL_PATH="$WAL_BASE" "$COORD_BIN" > "$COORD_LOG" 2>&1 &
COORD_PID=$!

for i in $(seq 1 60); do
    ss -ltn 2>/dev/null | grep -q ':5001 ' && break
    sleep 0.25
done

if ! ss -ltn 2>/dev/null | grep -q ':5001 '; then
    echo "FAIL: coordinator refused to start on a $PAIRS-task WAL"
    cat "$COORD_LOG"
    exit 1
fi

RESP=$("$STATUS_BIN" 2>/dev/null | grep -o 'STATUS_REPLY.*')
echo "STATUS: $RESP"

if ! printf '%s' "$RESP" | grep -q 'submitted=4100'; then
    echo "FAIL: expected submitted=4100, got: $RESP"
    exit 1
fi

if ! printf '%s' "$RESP" | grep -q 'completed=4100'; then
    echo "FAIL: expected completed=4100, got: $RESP"
    exit 1
fi

if ! printf '%s' "$RESP" | grep -q 'queue=0'; then
    echo "FAIL: expected queue=0 (all tasks had results), got: $RESP"
    exit 1
fi

echo "===== Submitting task 4101 ====="
OUT_SUBMIT=$("$CLIENT_BIN" echo after-overflow)
echo "$OUT_SUBMIT"

if ! printf '%s' "$OUT_SUBMIT" | grep -q 'id=4101'; then
    echo "FAIL: expected new task id 4101"
    exit 1
fi

echo "===== Querying last retained result ====="
"$QUERY_BIN" 4100 > "$QUERY_LOG" 2>&1
if ! grep -q 'QUERY_RESULT|4100|1|0|2' "$QUERY_LOG"; then
    echo "FAIL: task 4100 result was not retained"
    cat "$QUERY_LOG"
    exit 1
fi

echo
echo "===== Coordinator log (tail) ====="
tail -12 "$COORD_LOG"
echo

echo "===== WAL overflow test: PASS ====="
exit 0
