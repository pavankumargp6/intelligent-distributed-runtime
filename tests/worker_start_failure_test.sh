#!/bin/bash
#
# Worker start-failure reporting test (Phase 2).
#
# A worker accepts a dispatch before it starts the child. If the
# child cannot be started, the worker must report a FAILED result.
# Before v0.3.0 it exited silently, so the coordinator kept the
# entry in flight and QUERY returned QUERY_PENDING forever.
#
# IDR_TEST_FAIL_START=1 forces start_task() to fail inside the
# worker, exercising the reporting path end to end.
#
# Preconditions: build/runtime_server, build/worker_agent,
# build/client_submit, build/client_query (run `make` first).
# Port 5001 must be free.

set -u
export IDR_WORKER_SECRET="idr-test-worker-secret-do-not-use-in-production"
export IDR_CLIENT_SECRET="idr-test-client-secret-do-not-use-in-production"
export IDR_WAL_PATH=/tmp/idr_wal_start_failure_$$.log
export IDR_AGENT_INTERVAL_SECONDS=1
export IDR_TEST_FAIL_START=1

rm -f "${IDR_WAL_PATH}"*

if ss -ltn 2>/dev/null | grep -q ":5001 "; then
    echo "FAIL: port 5001 already in use. Kill leftovers first:"
    echo "  pkill -9 -f 'idr_coord|idr_worker|runtime_server|worker_agent'"
    exit 1
fi

cd "$(dirname "$0")/.."

COORD_LOG=/tmp/idr_coord_start_failure_$$.log
WORKER_LOG=/tmp/idr_worker_start_failure_$$.log
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
    echo "--- coordinator log ---"
    cat "$COORD_LOG" 2>/dev/null || true
    echo "--- worker log ---"
    cat "$WORKER_LOG" 2>/dev/null || true
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

echo "===== Starting worker with forced start failure ====="
./build/worker_agent 1 > "$WORKER_LOG" 2>&1 &
WORKER_PID=$!

sleep 1

echo "===== Submitting one task ====="
SUBMIT=$(./build/client_submit echo start-failure 2>/dev/null | grep -o 'SUBMIT_.*' | head -1)
echo "$SUBMIT"

case "$SUBMIT" in
    SUBMIT_ACK\|1) ;;
    *) fail "expected SUBMIT_ACK|1, got: $SUBMIT" ;;
esac

echo "===== Waiting for a terminal result ====="
Q=""

for i in $(seq 1 40); do
    Q=$(./build/client_query 1 2>/dev/null | grep -o 'QUERY_.*' | head -1)

    case "$Q" in
        QUERY_RESULT\|*) break ;;
    esac

    sleep 0.25
done

echo "QUERY: $Q"

case "$Q" in
    QUERY_RESULT\|1\|1\|-1\|3) ;;
    *)
        fail "start-failed task should be QUERY_RESULT|1|1|-1|3, got: $Q"
        ;;
esac

STATUS=$(./build/client_status 2>/dev/null | grep -o 'STATUS_REPLY.*' | head -1)
echo "STATUS: $STATUS"

if ! printf '%s' "$STATUS" | grep -q 'submitted=1' ||
   ! printf '%s' "$STATUS" | grep -q 'inflight=0' ||
   ! printf '%s' "$STATUS" | grep -q 'failed=1'; then
    fail "expected submitted=1, inflight=0, failed=1 in STATUS"
fi

echo
echo "===== Worker start-failure test: PASS ====="
exit 0
