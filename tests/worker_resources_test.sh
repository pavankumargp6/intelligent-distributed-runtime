#!/bin/bash
#
# Wrapper that builds and runs the worker_resources unit test.
# The underlying collect_worker_resources call sleeps 1 second
# to sample CPU, so this test takes ~1s.

set -u
cd "$(dirname "$0")/.."

BIN=/tmp/idr_worker_resources_test

echo "===== Building worker_resources_test ====="
gcc -Wall -Wextra -Isrc \
    tests/worker_resources_test.c \
    src/worker_resources.c \
    src/log.c \
    -o "$BIN" || exit 1

"$BIN"
