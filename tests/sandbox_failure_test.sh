#!/bin/bash
set -u
cd "$(dirname "$0")/.."

WAL=/tmp/idr_wal_fail_$$.log
COORD_LOG=/tmp/idr_coord_fail_$$.log
COORD_PID=""
WORKER_PID=""

cleanup() {
    kill $COORD_PID $WORKER_PID 2>/dev/null
    wait 2>/dev/null
    rm -f "$WAL" "$COORD_LOG" /tmp/idr_w_fail_$$.log
}
trap cleanup EXIT

if ss -ltn 2>/dev/null | grep -q ':5001 '; then
    echo "FAIL: port 5001 in use."; exit 1
fi

export IDR_INSECURE=1
# Point to a nonexistent directory to force chroot() to fail
export IDR_SANDBOX_DIR="/tmp/this_directory_does_not_exist_12345"

IDR_WAL_PATH="$WAL" ./build/runtime_server > "$COORD_LOG" 2>&1 &
COORD_PID=$!
for i in $(seq 1 40); do ss -ltn 2>/dev/null | grep -q ':5001 ' && break; sleep 0.25; done

./build/worker_agent 1 > /tmp/idr_w_fail_$$.log 2>&1 &
WORKER_PID=$!
for i in $(seq 1 40); do grep -q 'worker_registered' /tmp/idr_w_fail_$$.log 2>/dev/null && break; sleep 0.25; done

echo "===== Submitting task with broken sandbox ====="
SUBMIT_OUT=$(./build/client_submit echo "this should fail")
TASK_ID=$(echo "$SUBMIT_OUT" | grep -o 'SUBMIT_ACK|[0-9]*' | cut -d'|' -f2)

sleep 4

RESP=$(./build/client_query "$TASK_ID" 2>/dev/null | grep -o 'QUERY_RESULT.*')
echo "Result: $RESP"

# The task should fail with 125 or 126 because the sandbox couldn't be established.
# It should NOT succeed (which would mean it ran on the host).
if [[ "$RESP" == *"|125|3"* || "$RESP" == *"|126|3"* ]]; then
    echo "PASS: task failed closed because the sandbox could not be established."
else
    echo "FAIL: task executed despite a broken sandbox! Result: $RESP"
    exit 1
fi

echo "===== sandbox failure test: PASS ====="
exit 0
