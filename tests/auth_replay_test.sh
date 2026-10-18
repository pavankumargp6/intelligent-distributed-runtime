#!/bin/bash
#
# Wrapper that builds and runs the auth replay cache test.

set -u
unset IDR_INSECURE
cd "$(dirname "$0")/.."

BIN=/tmp/idr_auth_replay_test

echo "===== Building auth_replay_test ====="
gcc -Wall -Wextra -Isrc \
    tests/auth_replay_test.c \
    src/auth.c \
    src/hmac.c \
    src/sha256.c \
    -o "$BIN" || exit 1

"$BIN"
