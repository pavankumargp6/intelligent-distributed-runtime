#!/bin/bash
#
# Builds and runs the lifetime scale test. Compiled with
# IDR_TEST_WAL_NO_SYNC: this test measures capacity and bounded
# memory/history behaviour over millions of records, not fsync
# durability (the crash and recovery tests cover that).

set -u
cd "$(dirname "$0")/.."

BIN=/tmp/idr_wal_lifetime_test
TIME_LOG=$(mktemp)
trap 'rm -f "$TIME_LOG"' EXIT

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

if command -v /usr/bin/time >/dev/null 2>&1; then
    /usr/bin/time -v -o "$TIME_LOG" "$BIN" || exit 1

    MAX_RSS_KB=$(awk '/Maximum resident set size/ { print $NF }' "$TIME_LOG")

    if [ -n "$MAX_RSS_KB" ]; then
        echo "Max RSS: ${MAX_RSS_KB} KiB (limit 262144 KiB)"

        if [ "$MAX_RSS_KB" -gt 262144 ]; then
            echo "FAIL: max RSS exceeds the 256 MiB bound" >&2
            exit 1
        fi
    fi
else
    "$BIN" || exit 1
fi
