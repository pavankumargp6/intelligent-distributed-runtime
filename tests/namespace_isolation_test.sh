#!/bin/bash
set -u
cd "$(dirname "$0")/.."

WAL=/tmp/idr_wal_ns_$$.log
SANDBOX=/tmp/idr_sandbox_$$
COORD_LOG=/tmp/idr_coord_ns_$$.log
COORD_PID=""
WORKER_PID=""

cleanup() {
    kill $COORD_PID $WORKER_PID 2>/dev/null
    wait 2>/dev/null
    rm -rf "$WAL" "$SANDBOX" "$COORD_LOG" /tmp/idr_w_ns_$$.log
}
trap cleanup EXIT

if ss -ltn 2>/dev/null | grep -q ':5001 '; then
    echo "FAIL: port 5001 in use."; exit 1
fi

# 1. Setup the Sandbox
echo "===== Setting up sandbox at $SANDBOX ====="
mkdir -p "$SANDBOX/bin"
# Copy a static binary (true) into the sandbox so we can execute something
cp /bin/true "$SANDBOX/bin/true"
# Create a dummy /etc inside the sandbox
mkdir -p "$SANDBOX/etc"

export IDR_INSECURE=1
export IDR_SANDBOX_DIR="$SANDBOX"

# 2. Start Coordinator & Worker
IDR_WAL_PATH="$WAL" ./build/runtime_server > "$COORD_LOG" 2>&1 &
COORD_PID=$!
for i in $(seq 1 40); do ss -ltn 2>/dev/null | grep -q ':5001 ' && break; sleep 0.25; done

./build/worker_agent 1 > /tmp/idr_w_ns_$$.log 2>&1 &
WORKER_PID=$!
for i in $(seq 1 40); do grep -q 'worker_registered' /tmp/idr_w_ns_$$.log 2>/dev/null && break; sleep 0.25; done

# 3. Submit a task that tries to read the host's /etc/passwd
echo "===== Submitting sandboxed task ====="
# We use /bin/sh to try and cat the host file. If chroot works, it will fail.
SUBMIT_OUT=$(./build/client_submit /bin/sh -c "cat /etc/passwd")
echo "$SUBMIT_OUT"
TASK_ID=$(echo "$SUBMIT_OUT" | grep -o 'SUBMIT_ACK|[0-9]*' | cut -d'|' -f2)

if [ -z "$TASK_ID" ]; then
    echo "FAIL: could not parse task ID"
    exit 1
fi

sleep 4

# 4. Query Result
RESP=$(./build/client_query "$TASK_ID" 2>/dev/null | grep -o 'QUERY_RESULT.*')
echo "Result: $RESP"

# In a chroot, /bin/sh won't exist unless we copied it. 
# If execvp fails, it exits with 127. 
# If chroot failed, it exits with 126.
# If unshare failed, it exits with 125.
if [[ "$RESP" == *"|127"* ]]; then
    echo "PASS: unshare succeeded, chroot succeeded, and execvp correctly failed because /bin/sh is not in the sandbox."
else
    echo "FAIL: task was not properly restricted. Result: $RESP"
    exit 1
fi

echo "===== namespace isolation test: PASS ====="
exit 0
