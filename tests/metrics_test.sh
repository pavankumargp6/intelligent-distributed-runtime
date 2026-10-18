#!/bin/bash
set -u
cd "$(dirname "$0")/.."

WAL=/tmp/idr_metrics_wal_$$.log
COORD_LOG=/tmp/idr_metrics_coord_$$.log
COORD_PID=""
WORKER_PID=""

cleanup() {
    kill $COORD_PID $WORKER_PID 2>/dev/null
    wait 2>/dev/null
    rm -f "$WAL" "$COORD_LOG" /tmp/idr_metrics_worker_$$.log
}
trap cleanup EXIT

if ss -ltn 2>/dev/null | grep -q ':5001 '; then
    echo "FAIL: port 5001 in use."; exit 1
fi

export IDR_INSECURE=1
IDR_WAL_PATH="$WAL" ./build/runtime_server > "$COORD_LOG" 2>&1 &
COORD_PID=$!

for i in $(seq 1 40); do ss -ltn 2>/dev/null | grep -q ':5001 ' && break; sleep 0.25; done

./build/worker_agent 1 > /tmp/idr_metrics_worker_$$.log 2>&1 &
WORKER_PID=$!
for i in $(seq 1 40); do grep -q 'worker_registered' /tmp/idr_metrics_worker_$$.log 2>/dev/null && break; sleep 0.25; done

# Submit one fast task
./build/client_submit echo test > /dev/null
sleep 4

RESP=$(./build/client_metrics 2>/dev/null | grep -o 'METRICS_REPLY.*')

if [[ "$RESP" == METRICS_REPLY\|*submitted=1*completed=1* ]]; then
    echo "===== metrics test: PASS ====="
    exit 0
else
    echo "FAIL: unexpected metrics response: $RESP"
    exit 1
fi
