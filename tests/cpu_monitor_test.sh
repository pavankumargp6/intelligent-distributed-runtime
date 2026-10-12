#!/bin/bash
#
# Wrapper that builds and runs the cpu_monitor unit test.

set -u
cd "$(dirname "$0")/.."

BIN=/tmp/idr_cpu_monitor_test

echo "===== Building cpu_monitor_test ====="
gcc -Wall -Wextra -Isrc \
    tests/cpu_monitor_test.c \
    src/cpu_monitor.c \
    -o "$BIN" || exit 1

"$BIN"
