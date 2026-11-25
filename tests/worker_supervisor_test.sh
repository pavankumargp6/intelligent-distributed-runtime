#!/bin/bash
#
# Worker child-supervisor test (Phase 2).
#
# A worker must track how many task children are actually
# alive. Before the supervisor existed, the agent incremented a
# counter at fork() and never decremented it, then reported that
# monotonically growing number in heartbeats. Once the count
# reached IDR_WORKER_MAX_TASKS the coordinator considered the
# worker permanently full and stopped dispatching to it.
#
# This test runs a worker with IDR_WORKER_MAX_TASKS=2 and submits
# several waves of short tasks. Every task must reach a terminal
# result, which is only possible if children are reaped and slots
# are released after each completion.
#
# Preconditions: build/runtime_server, build/worker_agent,
# build/client_submit, build/client_query, build/client_status
# (run `make` first). Port 5001 must be free.

set -u
export IDR_INSECURE=1
unset IDR_WORKER_SECRET
unset IDR_CLIENT_SECRET

export IDR_WAL_PATH=/tmp/idr_wal_supervisor_$$.log
export IDR_AGENT_INTERVAL_SECONDS=0.05
export IDR_WORKER_MAX_TASKS=2

rm -f "${IDR_WAL_PATH}"*

if ss -ltn 2>/dev/null | grep -q ':5001 '; then
    echo "FAIL: port 5001 already in use. Kill leftovers first:"
    echo "  pkill -9 -f 'idr_coord|idr_worker|runtime_server|worker_agent'"
    exit 1
fi

cd "$(dirname "$0")/.."

COORD_LOG=/tmp/idr_coord_supervisor_$$.log
WORKER_LOG=/tmp/idr_worker_supervisor_$$.log
COORD_PID=""
WORKER_PID=""

cleanup() {
    if [ -n "$WORKER_PID" ]; then
        kill -KILL "$WORKER_PID" 2>/dev/null
        wait "$WORKER_PID" 2>/dev/null
    fi
    if [ -n "$COORD_PID" ]; then
        kill -KILL "$COORD_PID" 2>/dev/null
        wait "$COORD_PID" 2>/dev/null
    fi
    rm -f "$COORD_LOG" "$WORKER_LOG" "${IDR_WAL_PATH}"*
    true
}
trap cleanup EXIT

fail() {
    echo "FAIL: $1"
    echo "--- coordinator log (tail) ---"
    tail -40 "$COORD_LOG" 2>/dev/null || true
    echo "--- worker log (tail) ---"
    tail -40 "$WORKER_LOG" 2>/dev/null || true
    exit 1
}

: > "$COORD_LOG"
: > "$WORKER_LOG"

echo "===== Starting coordinator ====="
./build/runtime_server > "$COORD_LOG" 2>&1 &
COORD_PID=$!

for i in $(seq 1 40); do
    ss -ltn 2>/dev/null | grep -q ':5001 ' && break
    sleep 0.25
done
ss -ltn 2>/dev/null | grep -q ':5001 ' || fail "coordinator did not start listening"

echo "===== Starting worker (max 2 tasks) ====="
./build/worker_agent 1 > "$WORKER_LOG" 2>&1 &
WORKER_PID=$!

sleep 1

submit_task() {
    local out
    out=$(./build/client_submit sleep 0.1 2>/dev/null | grep -o 'SUBMIT_ACK|[0-9]*' | head -1)
    printf '%s' "$out" | grep -o '[0-9]*$'
}

wait_for_result() {
    local id="$1"
    local q=""

    for _ in $(seq 1 80); do
        q=$(./build/client_query "$id" 2>/dev/null | grep -o 'QUERY_[A-Z]*|.*' | head -1)

        case "$q" in
            QUERY_RESULT\|*) printf '%s' "$q"; return 0 ;;
        esac

        sleep 0.25
    done

    printf '%s' "${q:-<none>}"
    return 1
}

echo "===== Wave 1: six tasks through a two-slot worker ====="
for i in $(seq 1 6); do
    id=$(submit_task)
    [ -n "$id" ] || fail "task $i was not accepted"
done

for id in $(seq 1 6); do
    q=$(wait_for_result "$id") || fail "task $id never finished: $q"
    echo "  task $id: $q"
done

echo "===== Wave 2: two more tasks after the slots were released ====="
for i in $(seq 7 8); do
    id=$(submit_task)
    [ -n "$id" ] || fail "wave-2 task $i was not accepted"
done

for id in $(seq 7 8); do
    q=$(wait_for_result "$id") || fail "wave-2 task $id never finished: $q"
    echo "  task $id: $q"
done

STATUS=$(./build/client_status 2>/dev/null | grep -o 'STATUS_REPLY.*' | head -1)
echo "STATUS: $STATUS"

for expected in submitted=8 completed=8 failed=0 inflight=0 queue=0; do
    if ! printf '%s' "$STATUS" | grep -q "$expected"; then
        fail "expected $expected in STATUS"
    fi
done

echo
echo "===== Worker child-supervisor test: PASS ====="
exit 0
