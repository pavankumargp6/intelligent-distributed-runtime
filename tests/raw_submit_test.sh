#!/bin/bash
set -u
cd "$(dirname "$0")/.."

COORD_LOG=/tmp/idr_raw_submit_coord_$$.log
COORD_PID=""

cleanup() {
    if [ -n "$COORD_PID" ]; then
        kill "$COORD_PID" 2>/dev/null
        wait "$COORD_PID" 2>/dev/null
    fi
    rm -f "$COORD_LOG"
}
trap cleanup EXIT

fail() {
    echo "FAIL: $1"
    echo "--- coordinator log ---"
    cat "$COORD_LOG" 2>/dev/null || true
    exit 1
}

if ss -ltn 2>/dev/null | grep -q ':5001 '; then
    echo "FAIL: port 5001 already in use. Kill leftovers first:"
    echo "  pkill -9 -f 'runtime_server|worker_agent'"
    exit 1
fi

export IDR_INSECURE=1
./build/runtime_server > "$COORD_LOG" 2>&1 &
COORD_PID=$!

for i in $(seq 1 40); do
    ss -ltn 2>/dev/null | grep -q ':5001 ' && break
    sleep 0.25
done
ss -ltn 2>/dev/null | grep -q ':5001 ' || fail "coordinator did not start listening"

send() {
    echo "send: $1"
    ./build/raw_submit "$1"
}

R=$(send 'SUBMIT|4:echo|1|3:one|0|-1')
echo "recv: $R"
case "$R" in
    SUBMIT_REJECT) ;;
    *) fail "negative timeout should be SUBMIT_REJECT, got: $R" ;;
esac

R=$(send 'SUBMIT|4:echo|1|3:one|0|99999999999')
echo "recv: $R"
case "$R" in
    SUBMIT_REJECT) ;;
    *) fail "out-of-range timeout should be SUBMIT_REJECT, got: $R" ;;
esac

R=$(send 'SUBMIT|4:echo|1|3:one|0|5')
echo "recv: $R"
case "$R" in
    SUBMIT_ACK\|*) ;;
    *) fail "valid timeout should be SUBMIT_ACK, got: $R" ;;
esac

echo
echo "===== raw SUBMIT rejection test: PASS ====="
exit 0
