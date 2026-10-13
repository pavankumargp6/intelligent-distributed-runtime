#!/bin/bash
#
# Wrapper that builds and runs the HMAC-SHA256 unit test.

set -u
cd "$(dirname "$0")/.."

BIN=/tmp/idr_hmac_test

echo "===== Building hmac_test ====="
gcc -Wall -Wextra -Isrc \
    tests/hmac_test.c \
    src/hmac.c \
    src/sha256.c \
    -o "$BIN" || exit 1

"$BIN"
