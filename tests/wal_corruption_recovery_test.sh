#!/bin/bash
#
# WAL 2.0 corruption + torn-tail recovery test.
#
#   Case 1 (torn tail): the final record of the newest segment is
#   cut in half. Recovery must truncate it, start, and keep every
#   complete record before it.
#
#   Case 2 (mid-file corruption): a byte in the payload of a
#   record in a non-final segment is flipped. Recovery must
#   refuse to start rather than silently replay a broken log.
#
# The WAL is seeded as v2 segments so both cases exercise the
# framed, CRC-protected format.

set -u
export IDR_WORKER_SECRET="idr-test-worker-secret-do-not-use-in-production"
export IDR_CLIENT_SECRET="idr-test-client-secret-do-not-use-in-production"
export IDR_INSECURE=1

COORD_PID=""

cleanup() {
    [ -n "$COORD_PID" ] && kill -KILL "$COORD_PID" 2>/dev/null
    rm -f /tmp/idr_wal_corrupt2_* "$COORD_BIN" "$STATUS_BIN" "$SEED_BIN"
    true
}
trap cleanup EXIT

cd "$(dirname "$0")/.."

COORD_BIN=/tmp/idr_coord_corrupt2
STATUS_BIN=/tmp/idr_status_corrupt2
SEED_BIN=/tmp/idr_wal_seed_corrupt2

echo "===== Building coordinator, client, seeder ====="
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

gcc -Wall -Wextra -Isrc \
    tests/client_status.c src/network_protocol.c src/runtime_endpoint.c \
    src/sha256.c src/hmac.c src/auth.c src/auth_config.c \
    -o "$STATUS_BIN" || exit 1

gcc -Wall -Wextra -DIDR_TEST_WAL_NO_SYNC -Isrc \
    tests/wal_seed.c src/wal.c src/log.c \
    -o "$SEED_BIN" || exit 1

if ss -ltn 2>/dev/null | grep -q ':5001 '; then
    echo "FAIL: port 5001 in use."; exit 1
fi

start_coord() {
    local wal="$1" log="$2"
    IDR_WAL_PATH="$wal" "$COORD_BIN" > "$log" 2>&1 &
    COORD_PID=$!

    for i in $(seq 1 40); do
        ss -ltn 2>/dev/null | grep -q ':5001 ' && return 0
        sleep 0.25
    done

    return 1
}

stop_coord() {
    [ -n "$COORD_PID" ] && kill -KILL "$COORD_PID" 2>/dev/null
    COORD_PID=""
    sleep 0.5
}

wait_port_free() {
    for i in $(seq 1 40); do
        ss -ltn 2>/dev/null | grep -q ':5001 ' || return 0
        sleep 0.25
    done
    return 1
}

# ---------- Case 1: torn tail ----------

echo
echo "===== Case 1: torn final record ====="

WAL1=/tmp/idr_wal_corrupt2_torn_$$.log
LOG1=/tmp/idr_coord_corrupt2_torn_$$.log

"$SEED_BIN" "$WAL1" 0 20 4096 > /dev/null || exit 1

NEWEST=$(ls "$WAL1".seg.* | sort | tail -1)
echo "newest segment: $NEWEST"
printf 'GARBAGE' >> "$NEWEST"

if ! start_coord "$WAL1" "$LOG1"; then
    echo "FAIL: coordinator did not start after torn-tail truncation"
    cat "$LOG1"
    exit 1
fi

RESP=$("$STATUS_BIN" 2>/dev/null | grep -o 'STATUS_REPLY.*')
echo "STATUS: $RESP"

if ! printf '%s' "$RESP" | grep -q 'submitted=20'; then
    echo "FAIL: expected submitted=20 after torn-tail recovery, got: $RESP"
    exit 1
fi

if ! printf '%s' "$RESP" | grep -q 'queue=20'; then
    echo "FAIL: expected queue=20, got: $RESP"
    exit 1
fi

if ! grep -q 'wal_torn_tail_truncated' "$LOG1"; then
    echo "FAIL: expected wal_torn_tail_truncated in the log"
    cat "$LOG1"
    exit 1
fi

stop_coord
wait_port_free || true

# ---------- Case 2: mid-file corruption ----------

echo
echo "===== Case 2: corrupted record mid-file ====="

WAL2=/tmp/idr_wal_corrupt2_mid_$$.log
LOG2=/tmp/idr_coord_corrupt2_mid_$$.log

"$SEED_BIN" "$WAL2" 0 200 4096 > /dev/null || exit 1

FIRST=$(ls "$WAL2".seg.* | sort | head -1)
echo "corrupting $FIRST"

python3 - "$FIRST" <<'PY'
import sys
path = sys.argv[1]
with open(path, "r+b") as f:
    # Skip the 24-byte segment header, then jump past the record
    # header into the payload of the second record on disk.
    f.seek(24 + 20 + 60)
    byte = f.read(1)
    f.seek(24 + 20 + 60)
    f.write(bytes([byte[0] ^ 0xFF]))
PY

IDR_WAL_PATH="$WAL2" "$COORD_BIN" > "$LOG2" 2>&1 &
COORD_PID=$!
sleep 2

if ss -ltn 2>/dev/null | grep -q ':5001 '; then
    echo "FAIL: coordinator started despite mid-file corruption"
    cat "$LOG2"
    exit 1
fi

COORD_PID=""

if ! grep -q 'wal_record_crc_invalid' "$LOG2"; then
    echo "FAIL: expected wal_record_crc_invalid in the log"
    cat "$LOG2"
    exit 1
fi

if ! grep -q 'wal_recovery_failed' "$LOG2"; then
    echo "FAIL: expected wal_recovery_failed (refuse to start)"
    cat "$LOG2"
    exit 1
fi

echo "PASS: coordinator refused to start on a corrupt WAL"

echo
echo "===== WAL corruption recovery test: PASS ====="
exit 0
