#!/bin/bash
set -u
cd "$(dirname "$0")/.."

COORD_BIN=/tmp/idr_coord_corrupt
STATUS_BIN=/tmp/idr_status_corrupt
WAL=/tmp/idr_wal_corrupt_$$.log
COORD_LOG=/tmp/idr_coord_corrupt_$$.log
COORD_PID=""

cleanup() {
    kill $COORD_PID 2>/dev/null
    wait $COORD_PID 2>/dev/null
    rm -f "$WAL" "$COORD_LOG" "$COORD_BIN" "$STATUS_BIN"
}
trap cleanup EXIT

if ss -ltn 2>/dev/null | grep -q ':5001 '; then
    echo "FAIL: port 5001 in use."; exit 1
fi

echo "===== Building coordinator & client ====="
gcc -Wall -Wextra -Isrc \
    src/runtime_server_main.c src/runtime_server.c src/runtime.c \
    src/worker_registry.c src/task_manager.c src/task_queue.c \
    src/scheduler.c src/worker_pool.c src/worker.c src/worker_resources.c \
    src/process_info.c src/cpu_monitor.c src/task_executor.c src/task_metrics.c \
    src/runtime_stats.c src/network_protocol.c src/worker_registration.c \
    src/sha256.c src/hmac.c src/auth.c src/auth_config.c \
    src/coordinator_tasks.c src/coordinator_handlers.c src/wire_fields.c \
    src/log.c src/wal.c src/recovery.c src/metrics_json.c \
    src/connection_pool.c \
    -o "$COORD_BIN" || exit 1

gcc -Wall -Wextra -Isrc \
    tests/client_status.c src/network_protocol.c \
    src/sha256.c src/hmac.c src/auth.c src/auth_config.c \
    -o "$STATUS_BIN" || exit 1

# Create the corrupted WAL
echo "===== Creating corrupted WAL ====="
echo "SUBMIT|1|4:echo|1|3:one|0|0" > "$WAL"
# Inject a 9000-byte corrupted line (exceeds WAL_LINE_MAX)
python3 -c "print('X' * 9000)" >> "$WAL"
# Add a second valid line
echo "SUBMIT|2|4:echo|1|3:two|0|0" >> "$WAL"

export IDR_INSECURE=1
IDR_WAL_PATH="$WAL" "$COORD_BIN" > "$COORD_LOG" 2>&1 &
COORD_PID=$!

for i in $(seq 1 40); do
    ss -ltn 2>/dev/null | grep -q ':5001 ' && break
    sleep 0.25
done

if ! ss -ltn 2>/dev/null | grep -q ':5001 '; then
    echo "FAIL: coordinator crashed on corrupted WAL"
    cat "$COORD_LOG"
    exit 1
fi

# Fetch status to verify it recovered exactly 2 tasks (skipping the corrupt one)
RESP=$("$STATUS_BIN" 2>/dev/null | grep -o 'STATUS_REPLY.*')

echo "Recovered Status: $RESP"

if [[ "$RESP" == *"submitted=2"* && "$RESP" == *"queue=2"* ]]; then
    echo "PASS: coordinator recovered 2 valid tasks and skipped the corruption."
else
    echo "FAIL: expected submitted=2 queue=2, got: $RESP"
    exit 1
fi

echo "===== WAL corruption recovery test: PASS ====="
exit 0
