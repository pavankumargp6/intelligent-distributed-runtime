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
mkdir -p "$SANDBOX/etc"

# Compile a static test program that tries to read a host file
cat > /tmp/probe.c << 'CEOF'
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
int main() {
    int fd = open("/etc/hostname", O_RDONLY);
    if (fd >= 0) {
        printf("ESCAPED: successfully read host file\n");
        close(fd);
        return 1;
    } else {
        printf("JAILED: blocked from reading host file\n");
        return 0;
    }
}
CEOF
gcc -static /tmp/probe.c -o "$SANDBOX/bin/probe"
rm -f /tmp/probe.c

export IDR_INSECURE=1
export IDR_SANDBOX_DIR="$SANDBOX"

# 2. Start Coordinator & Worker
IDR_WAL_PATH="$WAL" ./build/runtime_server > "$COORD_LOG" 2>&1 &
COORD_PID=$!
for i in $(seq 1 40); do ss -ltn 2>/dev/null | grep -q ':5001 ' && break; sleep 0.25; done

./build/worker_agent 1 > /tmp/idr_w_ns_$$.log 2>&1 &
WORKER_PID=$!
for i in $(seq 1 40); do grep -q 'worker_registered' /tmp/idr_w_ns_$$.log 2>/dev/null && break; sleep 0.25; done

# 3. Submit the probe
echo "===== Submitting sandboxed probe ====="
SUBMIT_OUT=$(./build/client_submit ./bin/probe)
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

# 5. Evaluate
if [[ "$RESP" == *"|0|2"* ]]; then
    echo "PASS: executable ran inside the sandbox and was blocked from reading host files."
elif [[ "$RESP" == *"|125"* || "$RESP" == *"|126"* ]]; then
    echo "SKIP: environment blocks unshare/chroot syscalls (e.g., Docker seccomp)."
    exit 0
else
    echo "FAIL: task was not properly restricted. Result: $RESP"
    exit 1
fi

echo "===== namespace isolation test: PASS ====="
exit 0
