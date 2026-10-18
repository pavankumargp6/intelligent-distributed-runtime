#!/bin/bash
set -u
cd "$(dirname "$0")/.."

WAL=/tmp/idr_reinc_wal_$$.log
COORD_LOG=/tmp/idr_reinc_coord_$$.log
COORD_PID=""
WORKER_PID=""

cleanup() {
    kill $COORD_PID $WORKER_PID 2>/dev/null
    wait 2>/dev/null
    rm -f "$WAL" "$COORD_LOG" /tmp/idr_reinc_w1_$$.log /tmp/idr_reinc_w2_$$.log
}
trap cleanup EXIT

if ss -ltn 2>/dev/null | grep -q ':5001 '; then
    echo "FAIL: port 5001 in use."; exit 1
fi

export IDR_INSECURE=1
IDR_WAL_PATH="$WAL" ./build/runtime_server > "$COORD_LOG" 2>&1 &
COORD_PID=$!

for i in $(seq 1 40); do ss -ltn 2>/dev/null | grep -q ':5001 ' && break; sleep 0.25; done

./build/worker_agent 1 > /tmp/idr_reinc_w1_$$.log 2>&1 &
WORKER_PID=$!
for i in $(seq 1 40); do grep -q 'worker_registered' /tmp/idr_reinc_w1_$$.log 2>/dev/null && break; sleep 0.25; done

./build/client_submit --timeout 30 sleep 20 > /dev/null
sleep 6

kill -9 $WORKER_PID
wait $WORKER_PID 2>/dev/null

./build/worker_agent 1 > /tmp/idr_reinc_w2_$$.log 2>&1 &
WORKER_PID=$!
sleep 2

RESP=$(./build/client_query 1 2>/dev/null | grep -o 'QUERY_.*')

if [ "$RESP" = "QUERY_RESULT|1|1|-1|3" ]; then
    echo "===== worker reincarnation test: PASS ====="
    exit 0
else
    echo "FAIL: expected QUERY_RESULT|1|1|-1|3, got: $RESP"
    exit 1
fi
