#!/bin/bash
#
# Unit test for src/log.c. Verifies level filtering, JSON
# validity, and escaping.

set -u
cd "$(dirname "$0")/.."

BIN=/tmp/idr_log_test

echo "===== Building log_test ====="
gcc -Wall -Wextra -Isrc tests/log_test.c src/log.c -o "$BIN" || exit 1

FAIL=0

# ---- Test 1: default level is INFO; DEBUG suppressed ----
echo "===== Test 1: default level ====="
OUT=$(unset IDR_LOG_LEVEL; "$BIN" default)
LINE_COUNT=$(printf '%s\n' "$OUT" | wc -l)

if [ "$LINE_COUNT" -ne 3 ]; then
    echo "FAIL: expected 3 lines at INFO level, got $LINE_COUNT"
    printf '%s\n' "$OUT"
    FAIL=1
fi

if printf '%s\n' "$OUT" | grep -q 'dbg_should_not_appear'; then
    echo "FAIL: debug event leaked at INFO level"
    FAIL=1
fi

# ---- Test 2: every emitted line is valid JSON ----
echo "===== Test 2: JSON validity ====="
JSON_OK=1
while IFS= read -r line; do
    if ! printf '%s' "$line" | python3 -c 'import sys,json; json.loads(sys.stdin.read())' 2>/dev/null; then
        echo "FAIL: not valid JSON: $line"
        JSON_OK=0
    fi
done <<< "$OUT"

if [ "$JSON_OK" -ne 1 ]; then
    FAIL=1
fi

# ---- Test 3: IDR_LOG_LEVEL=debug enables debug output ----
echo "===== Test 3: debug level ====="
OUT=$(IDR_LOG_LEVEL=debug "$BIN" debug)
if ! printf '%s\n' "$OUT" | grep -q '"event":"debug_event"'; then
    echo "FAIL: debug event missing at DEBUG level"
    printf '%s\n' "$OUT"
    FAIL=1
fi

# ---- Test 4: escaping produces valid JSON with expected contents ----
echo "===== Test 4: escaping ====="
OUT=$("$BIN" escaping)
if ! printf '%s' "$OUT" | python3 -c '
import sys, json
obj = json.loads(sys.stdin.read())
msg = obj["msg"]
assert "quote=\"" in msg, "quote not escaped"
assert "backslash=\\" in msg, "backslash not escaped"
assert "tab=\t" in msg, "tab not preserved"
print("escape OK")
' > /dev/null 2>&1; then
    echo "FAIL: escaping test failed"
    printf '%s\n' "$OUT"
    FAIL=1
fi

if [ "$FAIL" -eq 0 ]; then
    echo "===== log test: PASS ====="
    exit 0
fi

echo "===== log test: FAIL ====="
exit 1
