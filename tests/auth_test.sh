#!/bin/bash
#
# Wrapper that builds and runs the auth unit test.

set -u
cd "$(dirname "$0")/.."

BIN=/tmp/idr_auth_test

echo "===== Building auth_test ====="
gcc -Wall -Wextra -Isrc \
    tests/auth_test.c \
    src/auth.c \
    src/hmac.c \
    src/sha256.c \
    -o "$BIN" || exit 1

"$BIN"
