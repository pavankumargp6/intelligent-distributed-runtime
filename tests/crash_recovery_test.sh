#!/bin/bash
#
# Crash-point recovery test.
#
# Every crash point is an _exit(99) injected at an exact moment
# of the durability path. For each point the test crashes the
# coordinator, restarts it on the same WAL without injection,
# and asserts that recovery produces a consistent state.
#
#   wal_after_write          record written, not yet fsynced
#   wal_after_fsync          record fsynced
#   segment_after_rotate     new segment created, manifest not updated
#   segment_before_unlink    snapshot in place, old segment not yet removed
#   snapshot_after_tmp_write snapshot temp file written, not renamed
#   snapshot_after_rename    snapshot visible, manifest still old
#   manifest_after_rename    manifest points at the new snapshot
#
# A process that dies mid-write leaves its writes in the page
# cache, so wal_after_write typically survives like a clean
# write; the assertion is that recovery accepts the state either
# way and never starts with a broken log.

set -u
export IDR_WORKER_SECRET="idr-test-worker-secret-do-not-use-in-production"
export IDR_CLIENT_SECRET="idr-test-client-secret-do-not-use-in-production"

if ss -ltn 2>/dev/null | grep -q ":5001 "; then
    echo "FAIL: port 5001 already in use."
    exit 1
fi

cd "$(dirname "$0")/.."

COORD_BIN=/tmp/idr_coord_crash
CLIENT_BIN=/tmp/idr_client_crash_submit
STATUS_BIN=/tmp/idr_client_crash_status
SEED_BIN=/tmp/idr_wal_seed_crash

echo "===== Building coordinator and clients ====="
gcc -Wall -Wextra \
    src/runtime_server_main.c src/runtime_server.c src/runtime.c \
    src/worker_registry.c src/task_manager.c src/task_queue.c \
    src/scheduler.c src/worker_pool.c src/worker.c src/worker_resources.c \
    src/process_info.c src/cpu_monitor.c src/task_executor.c src/task_metrics.c \
    src/runtime_stats.c src/network_protocol.c src/runtime_endpoint.c src/runtime_limits.c src/worker_registration.c \
    src/sha256.c src/hmac.c src/auth.c src/auth_config.c \
    src/coordinator_tasks.c src/coordinator_handlers.c src/wire_fields.c \
    src/log.c src/wal.c src/recovery.c src/metrics_json.c src/connection_pool.c \
    -o "$COORD_BIN" || exit 1

gcc -Wall -Wextra -Isrc \
    tests/client_submit.c src/network_protocol.c src/runtime_endpoint.c src/runtime_limits.c src/wire_fields.c \
    src/sha256.c src/hmac.c src/auth.c src/auth_config.c \
    -o "$CLIENT_BIN" || exit 1

gcc -Wall -Wextra -Isrc \
    tests/client_status.c src/network_protocol.c src/runtime_endpoint.c src/runtime_limits.c \
    src/sha256.c src/hmac.c src/auth.c src/auth_config.c \
    -o "$STATUS_BIN" || exit 1

gcc -Wall -Wextra -DIDR_TEST_WAL_NO_SYNC -Isrc \
    tests/wal_seed.c src/wal.c src/log.c \
    -o "$SEED_BIN" || exit 1

COORD_PID=""

cleanup() {
    [ -n "$COORD_PID" ] && kill -KILL "$COORD_PID" 2>/dev/null
    rm -f /tmp/idr_wal_crash2_* /tmp/idr_coord_crash_*.log
    true
}
trap cleanup EXIT

start_and_wait() {
    # $1 = wal base, $2 = log path, $3 = crash point ("" for none)
    local wal="$1" log="$2" crash="$3"

    if [ -n "$crash" ]; then
        IDR_WAL_PATH="$wal" IDR_TEST_CRASH_AT="$crash" \
            "$COORD_BIN" > "$log" 2>&1 &
    else
        IDR_WAL_PATH="$wal" "$COORD_BIN" > "$log" 2>&1 &
    fi

    COORD_PID=$!

    for i in $(seq 1 40); do
        ss -ltn 2>/dev/null | grep -q ':5001 ' && return 0
        kill -0 "$COORD_PID" 2>/dev/null || return 0
        sleep 0.25
    done

    return 0
}

wait_exit() {
    local rc=""
    for i in $(seq 1 60); do
        if ! kill -0 "$COORD_PID" 2>/dev/null; then
            wait "$COORD_PID" 2>/dev/null
            rc=$?
            COORD_PID=""
            echo "$rc"
            return 0
        fi
        sleep 0.25
    done
    kill -KILL "$COORD_PID" 2>/dev/null
    COORD_PID=""
    echo "timeout"
    return 1
}

assert_listening() {
    for i in $(seq 1 40); do
        ss -ltn 2>/dev/null | grep -q ':5001 ' && return 0
        sleep 0.25
    done
    echo "FAIL: coordinator is not listening after recovery ($1)"
    cat "$2"
    exit 1
}

status_field() {
    "$STATUS_BIN" 2>/dev/null | grep -o 'STATUS_REPLY.*'
}

# ------------------------------------------------------------------
# Crash points that fire while appending a task record.
# ------------------------------------------------------------------

for POINT in wal_after_write wal_after_fsync; do
    echo
    echo "===== Crash point: $POINT ====="

    WAL=/tmp/idr_wal_crash2_record_$POINT.$$.log
    LOG=/tmp/idr_coord_crash_$POINT.$$.log
    rm -f "$WAL"*

    start_and_wait "$WAL" "$LOG" "$POINT"

    if ! ss -ltn 2>/dev/null | grep -q ':5001 '; then
        echo "FAIL: coordinator never started for $POINT"
        cat "$LOG"
        exit 1
    fi

    "$CLIENT_BIN" echo crash-test > /dev/null 2>&1 || true

    RC=$(wait_exit)
    if [ "$RC" != "99" ]; then
        echo "FAIL: $POINT did not crash with status 99 (got $RC)"
        cat "$LOG"
        exit 1
    fi
    echo "  crashed with status 99 as expected"

    start_and_wait "$WAL" "$LOG" ""
    assert_listening "$POINT-restart" "$LOG"

    RESP=$(status_field)
    echo "  STATUS: $RESP"

    if ! printf '%s' "$RESP" | grep -q 'submitted=1'; then
        echo "FAIL: expected submitted=1 after $POINT recovery"
        cat "$LOG"
        exit 1
    fi

    kill -KILL "$COORD_PID" 2>/dev/null
    COORD_PID=""
    sleep 0.5
done

# ------------------------------------------------------------------
# Crash points that fire during the startup snapshot.
# ------------------------------------------------------------------

for POINT in segment_after_rotate \
             snapshot_after_tmp_write \
             snapshot_after_rename \
             manifest_after_rename; do
    echo
    echo "===== Crash point: $POINT ====="

    WAL=/tmp/idr_wal_crash2_snap_$POINT.$$.log
    LOG=/tmp/idr_coord_crash_$POINT.$$.log
    rm -f "$WAL"*

    start_and_wait "$WAL" "$LOG" "$POINT"
    RC=$(wait_exit)

    if [ "$RC" != "99" ]; then
        echo "FAIL: $POINT did not crash with status 99 (got $RC)"
        cat "$LOG"
        exit 1
    fi
    echo "  crashed with status 99 as expected"

    start_and_wait "$WAL" "$LOG" ""
    assert_listening "$POINT-restart" "$LOG"

    "$CLIENT_BIN" echo after-crash > /dev/null 2>&1
    sleep 1

    RESP=$(status_field)
    echo "  STATUS: $RESP"

    if ! printf '%s' "$RESP" | grep -q 'submitted=1'; then
        echo "FAIL: expected submitted=1 after $POINT recovery"
        cat "$LOG"
        exit 1
    fi

    kill -KILL "$COORD_PID" 2>/dev/null
    COORD_PID=""
    sleep 0.5
done

# ------------------------------------------------------------------
# Crash point that fires while truncating segments behind a snapshot.
# ------------------------------------------------------------------

echo
echo "===== Crash point: segment_before_unlink ====="

WAL=/tmp/idr_wal_crash2_unlink.$$.log
LOG=/tmp/idr_coord_crash_unlink.$$.log
rm -f "$WAL"*

"$SEED_BIN" "$WAL" 0 20 4096 > /dev/null || exit 1

start_and_wait "$WAL" "$LOG" "segment_before_unlink"
RC=$(wait_exit)

if [ "$RC" != "99" ]; then
    echo "FAIL: segment_before_unlink did not crash with status 99 (got $RC)"
    cat "$LOG"
    exit 1
fi
echo "  crashed with status 99 as expected"

if ! ls "$WAL".seg.* 2>/dev/null | head -1 > /dev/null; then
    echo "FAIL: segments vanished before the crash point could matter"
    exit 1
fi

start_and_wait "$WAL" "$LOG" ""
assert_listening "segment_before_unlink-restart" "$LOG"

RESP=$(status_field)
echo "  STATUS: $RESP"

if ! printf '%s' "$RESP" | grep -q 'submitted=20'; then
    echo "FAIL: expected submitted=20 after segment_before_unlink recovery"
    cat "$LOG"
    exit 1
fi

if ! printf '%s' "$RESP" | grep -q 'queue=20'; then
    echo "FAIL: expected queue=20 after segment_before_unlink recovery"
    cat "$LOG"
    exit 1
fi

kill -KILL "$COORD_PID" 2>/dev/null
COORD_PID=""

echo
echo "===== Crash recovery test: PASS ====="
exit 0
