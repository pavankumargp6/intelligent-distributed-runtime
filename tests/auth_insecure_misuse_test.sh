#!/bin/bash
#
# Insecure-mode misuse test.
#
# Starts the coordinator with IDR_INSECURE=1 and a non-loopback
# bind address. The coordinator must refuse to start with
# auth_insecure_bind_refused in its log.
#
# The bind address is 0.0.0.0. This does not require the
# machine to have that address assigned; inet_pton parses it,
# bind() would succeed, but our startup check rejects before
# bind is even called.

set -u
unset IDR_INSECURE

export IDR_INSECURE=1
export IDR_BIND_ADDR=0.0.0.0

unset IDR_WORKER_SECRET
unset IDR_CLIENT_SECRET

if ss -ltn 2>/dev/null | grep -q ":5001 "; then
    echo "FAIL: port 5001 already in use."
    exit 1
fi

cd "$(dirname "$0")/.."

COORD_BIN=/tmp/idr_coord_insecure_misuse
COORD_LOG=/tmp/idr_coord_insecure_misuse.log

echo "===== Building coordinator ====="
gcc -Wall -Wextra \
    src/runtime_server_main.c src/runtime_server.c src/runtime.c \
    src/worker_registry.c src/task_manager.c src/task_queue.c \
    src/scheduler.c src/worker_pool.c src/worker.c src/worker_resources.c \
    src/process_info.c src/cpu_monitor.c src/task_executor.c src/task_metrics.c \
    src/runtime_stats.c src/network_protocol.c src/worker_registration.c \
    src/coordinator_tasks.c src/coordinator_handlers.c src/wire_fields.c \
    src/log.c src/wal.c src/recovery.c src/connection_pool.c \
    src/sha256.c src/hmac.c src/auth.c src/auth_config.c \
    -o "$COORD_BIN" || exit 1

rm -f /tmp/idr_wal_insecure_misuse.log
: > "$COORD_LOG"

echo "===== Starting coordinator in insecure mode on 0.0.0.0 (should refuse) ====="
IDR_WAL_PATH=/tmp/idr_wal_insecure_misuse.log "$COORD_BIN" > "$COORD_LOG" 2>&1
COORD_RC=$?

echo "Coordinator exit code: $COORD_RC"
echo

echo "===== Coordinator log ====="
cat "$COORD_LOG"
echo

rm -f /tmp/idr_wal_insecure_misuse.log

if [ "$COORD_RC" -eq 0 ]; then
    echo "FAIL: coordinator exited 0 in insecure mode on 0.0.0.0"
    exit 1
fi

if ! grep -q '"event":"auth_insecure_bind_refused"' "$COORD_LOG"; then
    echo "FAIL: expected auth_insecure_bind_refused in coordinator log"
    exit 1
fi

echo "===== Insecure-mode misuse test: PASS ====="
exit 0
