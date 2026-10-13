#!/bin/bash
#
# Wrapper that builds and runs the SHA-256 unit test.

set -u
cd "$(dirname "$0")/.."

BIN=/tmp/idr_sha256_test

echo "===== Building sha256_test ====="
gcc -Wall -Wextra -Isrc \
    tests/sha256_test.c \
    src/sha256.c \
    -o "$BIN" || exit 1

"$BIN"
