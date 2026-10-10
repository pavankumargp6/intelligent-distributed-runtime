#!/bin/bash
#
# End-to-end test for worker-side remote execution.
#
# Starts a fake coordinator, runs the worker agent,
# and checks that the worker reports a valid
# TASK_RESULT for the assigned task.
#
# Usage:
#
#   bash tests/worker_remote_exec_test.sh
#
# Exit code 0 means the worker side is working.

set -u

cd "$(dirname "$0")/.."

WORKER_BIN=/tmp/idr_worker_agent_test
COORD_BIN=/tmp/idr_fake_coordinator_test

echo "===== Building fake coordinator ====="
gcc -Wall -Wextra -Isrc \
    tests/fake_coordinator.c \
    src/network_protocol.c \
    -o "$COORD_BIN" || exit 1

echo "===== Building worker agent ====="
gcc -Wall -Wextra -Isrc \
    src/worker_agent.c \
    src/worker.c \
    src/worker_resources.c \
    src/process_info.c \
    src/cpu_monitor.c \
    src/worker_registration.c \
    src/worker_registration_client.c \
    src/worker_task_client.c \
    src/wire_fields.c \
    src/task_executor.c \
    src/task_metrics.c \
    src/network_protocol.c \
    -o "$WORKER_BIN" || exit 1

echo "===== Starting fake coordinator ====="
"$COORD_BIN" &
COORD_PID=$!

# Give the coordinator a moment to bind the port.
sleep 0.5

echo "===== Starting worker agent ====="
timeout 30 "$WORKER_BIN" 1 || true

echo "===== Waiting for fake coordinator verdict ====="
wait "$COORD_PID"
EXIT_CODE=$?

if [ "$EXIT_CODE" -eq 0 ]; then
    echo "===== Worker remote execution test: PASS ====="
else
    echo "===== Worker remote execution test: FAIL (exit $EXIT_CODE) ====="
fi

exit "$EXIT_CODE"
