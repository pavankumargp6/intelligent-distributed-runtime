#!/bin/bash
#
# Builds and runs the snapshot/recovery integration test.

set -u
cd "$(dirname "$0")/.."

BIN=/tmp/idr_recovery_snapshot_test

echo "===== Building recovery_snapshot_test ====="
gcc -Wall -Wextra -Isrc \
    tests/recovery_snapshot_test.c \
    src/recovery.c \
    src/wal.c \
    src/log.c \
    src/coordinator_tasks.c \
    src/runtime_limits.c \
    src/task_manager.c \
    src/task_queue.c \
    src/task_metrics.c \
    src/runtime_stats.c \
    src/wire_fields.c \
    -o "$BIN" || exit 1

"$BIN"
