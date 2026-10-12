#!/bin/bash
#
# Wrapper that builds and runs the WAL compaction unit test.

set -u
cd "$(dirname "$0")/.."

BIN=/tmp/idr_recovery_compact_test

echo "===== Building recovery_compact_test ====="
gcc -Wall -Wextra -Isrc \
    tests/recovery_compact_test.c \
    src/recovery.c \
    src/wal.c \
    src/network_protocol.c \
    src/wire_fields.c \
    src/log.c \
    src/coordinator_tasks.c \
    src/task_manager.c \
    src/task_queue.c \
    -o "$BIN" || exit 1

"$BIN"
