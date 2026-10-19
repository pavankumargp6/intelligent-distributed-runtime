#!/bin/bash
set -u
cd "$(dirname "$0")/.."

COORD_LOG=/tmp/idr_metrics_http_$$.log
COORD_PID=""

cleanup() {
    kill $COORD_PID 2>/dev/null
    wait 2>/dev/null
    rm -f "$COORD_LOG"
}
trap cleanup EXIT

if ss -ltn 2>/dev/null | grep -q ':5001 ' || ss -ltn 2>/dev/null | grep -q ':8080 '; then
    echo "FAIL: port 5001 or 8080 already in use."; exit 1
fi

export IDR_INSECURE=1
./build/runtime_server > "$COORD_LOG" 2>&1 &
COORD_PID=$!

for i in $(seq 1 40); do ss -ltn 2>/dev/null | grep -q ':8080 ' && break; sleep 0.25; done

# 1. Test Valid HTTP Request
RESP=$(curl -s -w "\\n%{http_code}" http://127.0.0.1:8080/api/metrics)
BODY=$(echo "$RESP" | head -n -1)
CODE=$(echo "$RESP" | tail -n 1)

if [ "$CODE" != "200" ]; then
    echo "FAIL: expected HTTP 200, got $CODE"
    exit 1
fi

# 2. Validate JSON using Python
if echo "$BODY" | python3 -c "import sys, json; data=json.load(sys.stdin); sys.exit(0 if 'submitted' in data else 1)"; then
    echo "PASS: returned valid JSON with expected fields"
else
    echo "FAIL: invalid JSON or missing fields"
    echo "$BODY"
    exit 1
fi

# 3. Test Malformed Request (should still return 200 or at least not crash)
CODE_MAL=$(curl -s -o /dev/null -w "%{http_code}" -X POST http://127.0.0.1:8080/api/metrics)
if [ "$CODE_MAL" = "200" ]; then
    echo "PASS: handled malformed request gracefully"
else
    echo "FAIL: expected 200 on malformed request, got $CODE_MAL"
    exit 1
fi

# 4. Verify coordinator is still alive
if ! kill -0 $COORD_PID 2>/dev/null; then
    echo "FAIL: coordinator crashed"
    exit 1
fi

echo "===== metrics http test: PASS ====="
exit 0
