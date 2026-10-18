#!/bin/bash
set -u
cd "$(dirname "$0")/.."

WAL=/tmp/idr_fair_wal_$$.log
COORD_LOG=/tmp/idr_fair_coord_$$.log
W1_LOG=/tmp/idr_fair_w1_$$.log
W2_LOG=/tmp/idr_fair_w2_$$.log

COORD_PID=""
W1_PID=""
W2_PID=""

cleanup() {
    kill $COORD_PID $W1_PID $W2_PID 2>/dev/null
    wait 2>/dev/null
    rm -f "$WAL" "$COORD_LOG" "$W1_LOG" "$W2_LOG"
}
trap cleanup EXIT

if ss -ltn 2>/dev/null | grep -q ':5001 '; then
    echo "FAIL: port 5001 in use."; exit 1
fi

export IDR_INSECURE=1
IDR_WAL_PATH="$WAL" ./build/runtime_server > "$COORD_LOG" 2>&1 &
COORD_PID=$!

for i in $(seq 1 40); do ss -ltn 2>/dev/null | grep -q ':5001 ' && break; sleep 0.25; done

./build/worker_agent 1 > "$W1_LOG" 2>&1 &
W1_PID=$!

./build/worker_agent 2 > "$W2_LOG" 2>&1 &
W2_PID=$!

# Wait for both to register
for i in $(seq 1 40); do
    REG=$(grep -c 'worker_registered' "$COORD_LOG" 2>/dev/null || :)
    [ "$REG" -ge 2 ] && break
    sleep 0.25
done

# Submit 10 sleep tasks
for i in $(seq 1 10); do
    ./build/client_submit --timeout 5 sleep 1 > /dev/null
done

# Wait for them to finish
for i in $(seq 1 40); do
    COMP=$(grep -c '"event":"task_result"' "$COORD_LOG" 2>/dev/null || :)
    [ "$COMP" -ge 10 ] && break
    sleep 0.5
done

W1_TASKS=$(grep '"event":"task_dispatched"' "$COORD_LOG" | grep -c 'worker 1')
W2_TASKS=$(grep '"event":"task_dispatched"' "$COORD_LOG" | grep -c 'worker 2')

echo "Worker 1 executed: $W1_TASKS tasks"
echo "Worker 2 executed: $W2_TASKS tasks"

if [ "$W1_TASKS" -eq 5 ] && [ "$W2_TASKS" -eq 5 ]; then
    echo "===== scheduler fairness test: PASS ====="
    exit 0
else
    echo "FAIL: expected exactly 5 tasks each, got W1=$W1_TASKS W2=$W2_TASKS"
    exit 1
fi
