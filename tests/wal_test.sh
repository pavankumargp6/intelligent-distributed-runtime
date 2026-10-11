#!/bin/bash
#
# Wrapper that builds and runs the WAL unit test.

set -u
cd "$(dirname "$0")/.."

BIN=/tmp/idr_wal_test

echo "===== Building wal_test ====="
gcc -Wall -Wextra -Isrc tests/wal_test.c src/wal.c -o "$BIN" || exit 1

"$BIN"
