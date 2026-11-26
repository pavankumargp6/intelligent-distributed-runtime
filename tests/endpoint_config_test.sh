#!/bin/bash
#
# Endpoint configuration test.
#
# Verifies that:
#   1. An invalid IDR_PORT is refused at startup instead of
#      silently listening on the default port.
#   2. The coordinator listens on IDR_PORT.
#   3. Clients dial IDR_COORDINATOR_PORT (and
#      IDR_COORDINATOR_HOST).
#   4. A worker dials IDR_COORDINATOR_HOST:IDR_COORDINATOR_PORT
#      and completes an end-to-end task.
#
# Runs in insecure mode so the test does not need secrets; the
# coordinator enforces loopback-only binding in that mode.

set -u
export IDR_INSECURE=1
unset IDR_WORKER_SECRET
unset IDR_CLIENT_SECRET

PORT=15001
WAL=/tmp/idr_wal_endpoint_test.log
COORD_LOG=/tmp/idr_coord_endpoint_test.log
INVALID_LOG=/tmp/idr_coord_endpoint_invalid.log
WORKER_LOG=/tmp/idr_worker_endpoint_test.log
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
    rm -f "$WAL" "$WAL".* "$INVALID_LOG" "$COORD_LOG" "$WORKER_LOG"
    true
}
trap cleanup EXIT

if ss -ltn 2>/dev/null | grep -q ":$PORT "; then
    echo "FAIL: port $PORT already in use."
    exit 1
fi

cd "$(dirname "$0")/.."

# Remove any WAL artifacts a previous run left behind. Segments and
# snapshots carry suffixes, so a bare `rm "$WAL"` leaves durable state
# that the next coordinator recovers - and then task ids no longer
# start at 1.
rm -f "$WAL" "$WAL".*

COORD_BIN=/tmp/idr_coord_endpoint
WORKER_BIN=/tmp/idr_worker_endpoint
CLIENT_BIN=/tmp/idr_client_endpoint
QUERY_BIN=/tmp/idr_client_query_endpoint
STATUS_BIN=/tmp/idr_client_status_endpoint

echo "===== Building coordinator ====="
gcc -Wall -Wextra \
    src/runtime_server_main.c src/runtime_server.c src/runtime.c \
    src/worker_registry.c src/task_manager.c src/task_queue.c \
    src/scheduler.c src/worker_pool.c src/worker.c src/worker_resources.c \
    src/process_info.c src/cpu_monitor.c src/task_executor.c src/task_metrics.c \
    src/runtime_stats.c src/network_protocol.c src/runtime_endpoint.c src/runtime_limits.c \
    src/worker_registration.c \
    src/sha256.c src/hmac.c src/auth.c src/auth_config.c \
    src/coordinator_tasks.c src/coordinator_handlers.c src/wire_fields.c \
    src/log.c src/wal.c src/recovery.c src/metrics_json.c src/connection_pool.c \
    -o "$COORD_BIN" || exit 1

echo "===== Building worker ====="
gcc -Wall -Wextra -Isrc \
    src/worker_agent.c src/worker.c src/worker_resources.c \
    src/process_info.c src/cpu_monitor.c \
    src/worker_registration.c src/worker_registration_client.c \
    src/worker_task_client.c src/task_executor.c src/task_metrics.c \
    src/network_protocol.c src/runtime_endpoint.c src/runtime_limits.c src/wire_fields.c \
    src/log.c src/sha256.c src/hmac.c src/auth.c src/auth_config.c \
    -o "$WORKER_BIN" || exit 1

echo "===== Building clients ====="
gcc -Wall -Wextra -Isrc \
    tests/client_submit.c src/network_protocol.c src/runtime_endpoint.c src/runtime_limits.c \
    src/wire_fields.c src/sha256.c src/hmac.c src/auth.c src/auth_config.c \
    -o "$CLIENT_BIN" || exit 1

gcc -Wall -Wextra -Isrc \
    tests/client_query.c src/network_protocol.c src/runtime_endpoint.c src/runtime_limits.c \
    src/sha256.c src/hmac.c src/auth.c src/auth_config.c \
    -o "$QUERY_BIN" || exit 1

gcc -Wall -Wextra -Isrc \
    tests/client_status.c src/network_protocol.c src/runtime_endpoint.c src/runtime_limits.c \
    src/sha256.c src/hmac.c src/auth.c src/auth_config.c \
    -o "$STATUS_BIN" || exit 1

# ========= Part A: invalid port is refused =========
echo
echo "===== Part A: invalid IDR_PORT is refused ====="

for BAD_PORT in not-a-port 0 70000; do
    : > "$INVALID_LOG"
    IDR_PORT="$BAD_PORT" IDR_WAL_PATH="${WAL}.invalid" \
        "$COORD_BIN" > "$INVALID_LOG" 2>&1
    RC=$?

    if [ "$RC" -eq 0 ]; then
        echo "FAIL: coordinator accepted IDR_PORT=$BAD_PORT"
        cat "$INVALID_LOG"
        exit 1
    fi

    if ! grep -q '"event":"coordinator_port_invalid"' "$INVALID_LOG"; then
        echo "FAIL: IDR_PORT=$BAD_PORT rejected without a clear log"
        cat "$INVALID_LOG"
        exit 1
    fi

    echo "  IDR_PORT=$BAD_PORT rejected."
done

rm -f "${WAL}.invalid"*

# ========= Part B: custom port end-to-end =========
echo
echo "===== Part B: IDR_PORT=$PORT end-to-end ====="

IDR_PORT="$PORT" IDR_WAL_PATH="$WAL" "$COORD_BIN" > "$COORD_LOG" 2>&1 &
COORD_PID=$!

for i in $(seq 1 40); do
    ss -ltn 2>/dev/null | grep -q ":$PORT " && break
    sleep 0.25
done

if ! ss -ltn 2>/dev/null | grep -q ":$PORT "; then
    echo "FAIL: coordinator did not listen on $PORT"
    cat "$COORD_LOG"
    exit 1
fi

if ! grep -q "Listening on 127.0.0.1:$PORT" "$COORD_LOG"; then
    echo "FAIL: coordinator did not log the configured port"
    cat "$COORD_LOG"
    exit 1
fi

SUBMIT_OUT=$(IDR_COORDINATOR_PORT="$PORT" "$CLIENT_BIN" echo endpoint-ok)
TASK_ID=$(printf '%s\n' "$SUBMIT_OUT" | sed -n 's/^Response: SUBMIT_ACK|\([0-9][0-9]*\)$/\1/p' | head -1)
if [ -z "$TASK_ID" ]; then
    echo "FAIL: submit against port $PORT failed: $SUBMIT_OUT"
    cat "$COORD_LOG"
    exit 1
fi
echo "  submit on port $PORT: OK (task $TASK_ID)"

IDR_COORDINATOR_HOST=127.0.0.1 IDR_COORDINATOR_PORT="$PORT" \
    "$WORKER_BIN" 1 > "$WORKER_LOG" 2>&1 &
WORKER_PID=$!

RESULT=""
for i in $(seq 1 60); do
    RESULT=$(IDR_COORDINATOR_PORT="$PORT" "$QUERY_BIN" "$TASK_ID" 2>/dev/null | grep -o "QUERY_RESULT|$TASK_ID.*" || true)
    if [ -n "$RESULT" ]; then
        break
    fi
    sleep 0.25
done

if [ "$RESULT" != "QUERY_RESULT|$TASK_ID|1|0|2" ]; then
    echo "FAIL: expected QUERY_RESULT|$TASK_ID|1|0|2, got: ${RESULT:-<none>}"
    cat "$COORD_LOG"
    echo "--- worker log ---"
    cat "$WORKER_LOG" 2>/dev/null
    exit 1
fi
echo "  end-to-end on port $PORT: PASS"

STATUS_OUT=$(IDR_COORDINATOR_PORT="$PORT" "$STATUS_BIN" 2>/dev/null)
if ! printf '%s' "$STATUS_OUT" | grep -q 'workers=1'; then
    echo "FAIL: status did not report the worker: $STATUS_OUT"
    exit 1
fi
echo "  status on port $PORT: PASS"

echo
echo "===== Endpoint configuration test: PASS ====="
exit 0
