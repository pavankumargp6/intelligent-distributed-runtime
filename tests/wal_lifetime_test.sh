#!/bin/bash
#
# Builds and runs the lifetime scale test. Compiled with
# IDR_TEST_WAL_NO_SYNC: this test measures capacity and bounded
# memory/history behaviour over millions of records, not fsync
# durability (the crash and recovery tests cover that).

set -u
cd "$(dirname "$0")/.."

BIN=/tmp/idr_wal_lifetime_test

echo "===== Building wal_lifetime_test ====="
gcc -Wall -Wextra -DIDR_TEST_WAL_NO_SYNC -Isrc \
    tests/wal_lifetime_test.c \
    src/recovery.c \
    src/wal.c \
    src/log.c \
    src/coordinator_tasks.c \
    src/task_manager.c \
    src/task_queue.c \
    src/task_metrics.c \
    src/runtime_stats.c \
    src/wire_fields.c \
    -o "$BIN" || exit 1

"$BIN"
